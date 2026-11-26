#pragma once

#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mcla::cvar {

class CVarSystem {
public:
    static CVarSystem& Instance() {
        static CVarSystem instance;
        return instance;
    }

    // Where a CVar's current value came from (§1.1 of the Wave 3 spec).
    enum class Source { Default, Config, Runtime };
    using ChangeCallback = std::function<void(std::string_view name, std::string_view newValue)>;

    void RegisterString(const char* name, const char* defaultValue, const char* category, const char* description) {
        RegisterTyped(name, defaultValue, category, description, "string");
    }

    void RegisterInt(const char* name, int defaultValue, const char* category, const char* description) {
        RegisterTyped(name, std::to_string(defaultValue).c_str(), category, description, "int");
    }

    void RegisterBool(const char* name, bool defaultValue, const char* category, const char* description) {
        RegisterTyped(name, defaultValue ? "true" : "false", category, description, "bool");
    }

    void RegisterFloat(const char* name, float defaultValue, const char* category, const char* description) {
        RegisterTyped(name, std::to_string(defaultValue).c_str(), category, description, "float");
    }

    // Launcher contract (MCLA_DUMP_CVARS / --dump-cvars): every registered CVar as a
    // JSON array, sorted by name. Written before any device or guest work starts.
    bool WriteJsonDump(const std::filesystem::path& path) const {
        std::map<std::string, CVar> sorted;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sorted.insert(m_cvars.begin(), m_cvars.end());
        }
        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open()) return false;
        auto q = [](const std::string& s) {
            std::string o = "\"";
            for (char c : s) {
                if (c == '"' || c == '\\') o += '\\';
                if (static_cast<unsigned char>(c) >= 0x20) o += c;
            }
            return o + "\"";
        };
        f << "[\n";
        bool first = true;
        for (const auto& [name, c] : sorted) {
            f << (first ? "" : ",\n") << "  {\"name\":" << q(name) << ",\"type\":" << q(c.type)
              << ",\"default\":" << q(c.defaultValue) << ",\"current\":" << q(c.value)
              << ",\"category\":" << q(c.category) << ",\"description\":" << q(c.description) << "}";
            first = false;
        }
        f << "\n]\n";
        return f.good();
    }

    std::string_view GetString(const char* name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cvars.find(name);
        if (it != m_cvars.end()) {
            return it->second.value;
        }
        return "";
    }

    // True when a CVar has been registered. Row builders use this to avoid
    // emitting menu rows for CVars that do not exist in this build (no dead rows).
    bool Has(const char* name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cvars.find(name) != m_cvars.end();
    }

    Source GetSource(const char* name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cvars.find(name);
        return it != m_cvars.end() ? it->second.source : Source::Default;
    }

    // Updates the registered default; updates the live value only while the CVar
    // is still at its default (a Config/Runtime value is never clobbered).
    bool SetDefaultValue(const char* name, std::string_view value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cvars.find(name);
        if (it == m_cvars.end()) return false;
        it->second.defaultValue = std::string(value);
        if (it->second.source == Source::Default) it->second.value = it->second.defaultValue;
        return true;
    }

    void AddChangeCallback(const char* name, ChangeCallback cb) {
        if (!cb) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callbacks[name].push_back(std::move(cb));
    }

    int GetInt(const char* name) const {
        auto str = GetString(name);
        if (str.empty()) return 0;
        try { return std::stoi(std::string(str)); } catch (...) { return 0; }
    }

    bool GetBool(const char* name) const {
        auto str = GetString(name);
        return str == "true" || str == "1" || str == "yes" || str == "on";
    }

    float GetFloat(const char* name) const {
        auto str = GetString(name);
        if (str.empty()) return 0.0f;
        try { return std::stof(std::string(str)); } catch (...) { return 0.0f; }
    }

    bool SetString(const char* name, std::string_view value) {
        std::vector<ChangeCallback> callbacks;
        std::string newValue;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cvars.find(name);
            if (it == m_cvars.end()) return false;
            it->second.value = value;
            it->second.source = Source::Runtime;
            newValue = it->second.value;
            auto cbIt = m_callbacks.find(name);
            if (cbIt != m_callbacks.end()) callbacks = cbIt->second;
        }
        // Invoke after releasing m_mutex: a callback may call back into the
        // CVarSystem (e.g. re-read a value), which would deadlock otherwise.
        for (const auto& cb : callbacks) cb(name, std::string_view(newValue));
        return true;
    }

    bool SetInt(const char* name, int value) {
        return SetString(name, std::to_string(value));
    }

    bool SetBool(const char* name, bool value) {
        return SetString(name, value ? "true" : "false");
    }

    bool SetFloat(const char* name, float value) {
        return SetString(name, std::to_string(value));
    }

    void LoadConfig(const std::filesystem::path& path) {
        std::ifstream file(path);
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            // Skip comments and empty lines
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#') continue;

            // Parse key=value
            size_t eq = line.find('=', start);
            if (eq == std::string::npos) continue;

            std::string key = line.substr(start, eq - start);
            // Trim trailing whitespace from key
            while (!key.empty() && std::isspace(key.back())) key.pop_back();

            std::string value = line.substr(eq + 1);
            // Trim leading/trailing whitespace from value
            size_t vstart = value.find_first_not_of(" \t");
            if (vstart != std::string::npos) {
                value = value.substr(vstart);
                while (!value.empty() && std::isspace(value.back())) value.pop_back();
            }

            SetValueFromConfig(key.c_str(), value);
        }
    }

    void SaveConfig(const std::filesystem::path& path) const {
        std::ofstream file(path);
        if (!file.is_open()) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [name, cvar] : m_cvars) {
            file << name << " = " << cvar.value << "\n";
        }
    }

    // Selective, line-preserving save (§1.1). Only `names` are touched; every
    // other line (comments, [tables], launcher/CLI entries) is kept verbatim.
    // Writes path+".tmp", flushes, then renames over path.
    bool SaveConfigValues(const std::filesystem::path& path, const std::vector<std::string>& names) const {
        std::vector<std::pair<std::string, std::string>> values;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& name : names) {
                auto it = m_cvars.find(name);
                if (it != m_cvars.end()) values.emplace_back(name, it->second.value);
            }
        }
        if (values.empty()) return false;

        std::vector<std::string> lines;
        {
            std::ifstream in(path);
            std::string line;
            while (std::getline(in, line)) lines.push_back(line);
        }

        std::unordered_set<std::string> wanted;
        wanted.reserve(values.size());
        for (const auto& kv : values) wanted.insert(kv.first);

        // Insert appended names before the first [table] header, or at EOF.
        size_t firstTable = lines.size();
        for (size_t i = 0; i < lines.size(); ++i) {
            const size_t start = lines[i].find_first_not_of(" \t");
            if (start != std::string::npos && lines[i][start] == '[') { firstTable = i; break; }
        }

        std::unordered_set<std::string> seen;
        for (auto& line : lines) {
            const size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#') continue;
            const size_t eq = line.find('=', start);
            if (eq == std::string::npos) continue;
            std::string key = line.substr(start, eq - start);
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
            if (wanted.find(key) == wanted.end()) continue;
            for (const auto& kv : values) {
                if (kv.first == key) { line = key + " = " + kv.second; break; }
            }
            seen.insert(key);
        }

        std::vector<std::string> appended;
        for (const auto& kv : values) {
            if (seen.find(kv.first) == seen.end()) appended.push_back(kv.first + " = " + kv.second);
        }

        std::vector<std::string> out;
        out.reserve(lines.size() + appended.size());
        for (size_t i = 0; i <= lines.size(); ++i) {
            if (i == firstTable)
                for (const auto& a : appended) out.push_back(a);
            if (i < lines.size()) out.push_back(lines[i]);
        }

        const std::string tmp = path.string() + ".tmp";
        {
            std::ofstream file(tmp, std::ios::trunc);
            if (!file.is_open()) return false;
            for (const auto& line : out) file << line << "\n";
            file.flush();
            if (!file.good()) return false;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            // Windows implementations that refuse to replace an existing file:
            // best-effort fallback (not atomic, but never loses the source).
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::rename(tmp, path, ec);
        }
        if (ec) {
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            return false;
        }
        return true;
    }

private:
    struct CVar {
        std::string value;
        std::string category;
        std::string description;
        std::string defaultValue;
        std::string type;
        Source source = Source::Default;
    };

    void RegisterTyped(const char* name, const char* defaultValue, const char* category,
                       const char* description, const char* type) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cvars.find(name) == m_cvars.end()) {
            CVar c;
            c.value = defaultValue;
            c.category = category;
            c.description = description;
            c.defaultValue = defaultValue;
            c.type = type;
            c.source = Source::Default;
            m_cvars[name] = c;
        }
    }

    // Config load is silent (no listeners yet at startup) and stamps Config.
    void SetValueFromConfig(const char* name, std::string_view value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cvars.find(name);
        if (it == m_cvars.end()) return;
        it->second.value = value;
        it->second.source = Source::Config;
    }

    std::unordered_map<std::string, CVar> m_cvars;
    std::unordered_map<std::string, std::vector<ChangeCallback>> m_callbacks;
    mutable std::mutex m_mutex;

    CVarSystem() = default;
    ~CVarSystem() = default;
    CVarSystem(const CVarSystem&) = delete;
    CVarSystem& operator=(const CVarSystem&) = delete;
};

// Macro helpers for registration
#define MCLA_CVAR_STRING(name, default_val, category, desc) \
    namespace { \
        struct CVarRegistrar_##name { \
            CVarRegistrar_##name() { \
                mcla::cvar::CVarSystem::Instance().RegisterString(#name, default_val, category, desc); \
            } \
        } g_cvar_registrar_##name; \
    }

#define MCLA_CVAR_INT(name, default_val, category, desc) \
    namespace { \
        struct CVarRegistrar_##name { \
            CVarRegistrar_##name() { \
                mcla::cvar::CVarSystem::Instance().RegisterInt(#name, default_val, category, desc); \
            } \
        } g_cvar_registrar_##name; \
    }

#define MCLA_CVAR_BOOL(name, default_val, category, desc) \
    namespace { \
        struct CVarRegistrar_##name { \
            CVarRegistrar_##name() { \
                mcla::cvar::CVarSystem::Instance().RegisterBool(#name, default_val, category, desc); \
            } \
        } g_cvar_registrar_##name; \
    }

#define MCLA_CVAR_FLOAT(name, default_val, category, desc) \
    namespace { \
        struct CVarRegistrar_##name { \
            CVarRegistrar_##name() { \
                mcla::cvar::CVarSystem::Instance().RegisterFloat(#name, default_val, category, desc); \
            } \
        } g_cvar_registrar_##name; \
    }

// Getters
#define MCLA_CVAR_GET_STRING(name) mcla::cvar::CVarSystem::Instance().GetString(#name)
#define MCLA_CVAR_GET_INT(name) mcla::cvar::CVarSystem::Instance().GetInt(#name)
#define MCLA_CVAR_GET_BOOL(name) mcla::cvar::CVarSystem::Instance().GetBool(#name)
#define MCLA_CVAR_GET_FLOAT(name) mcla::cvar::CVarSystem::Instance().GetFloat(#name)

} // namespace mcla::cvar