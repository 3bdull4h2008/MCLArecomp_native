#include "diag_threads.h"
#include <cpu/ppc_context.h>
#include "app.h"
#include "logging.h"
#include "patches.h"
#include "boot_host.h"
#include "native_renderer.h"
#include "renderer_mode.h"
#include "render_thread.h"
#include "generated/ppc_xenon/ppc_recomp_shared.h"
#include "fs/vfs_rpf.h"
#include "kernel/memory.h"
#include "kernel/xam.h"
#include "apu/audio.h"
#include "hooks/hooks.h"   // Wave 3: MclaApplyAspectRatio (env-gated no-op by default)

// Forward declaration for GPU context poller (Phase 4)
void StartGpuContextPoller();
// Exported from kernel/xam.cpp and kernel/imports.cpp for clean save-drain on shutdown.
bool McHasUnclosedSaveBackups();
bool McHasOpenContentWriteHandles();
// Exported from hooks/frame_timing_ports.cpp to unblock ring/pump waiters on shutdown.
extern "C" void MclaPumpWakeShutdownBegins();

#include <fmt/format.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace mcla {

// Wave 3 (§2.4): cursor auto-hide toggle. Default true preserves the spec's
// intended behaviour; false restores an always-visible cursor.
MCLA_CVAR_BOOL(mcla_hide_cursor, true, "Controls", "Auto-hide the mouse cursor after idle / on gamepad input");

// Wave 3 (§1.4): the gate worker 1's wave3_menu.cpp:69 requires (Has && true).
// Default true is the spec's menu default; MCLA_NO_WAVE3_MENU=1 remains the
// owner's opt-out. Registering it here is what makes the Wave 3 menu live.
MCLA_CVAR_BOOL(mcla_settings_menu, true, "UI", "In-game PC settings menu");
// These were read (wave3_guards.cpp DevSwitchEnabled) and listed in the menu but never registered, so GetBool()
// returned false and the game's dev switches noraceshadows / noimpostors / nofsblur were ON by default.
MCLA_CVAR_BOOL(mcla_race_shadows, true, "Display", "Dynamic race shadows (game dev switch noraceshadows when off)");
MCLA_CVAR_BOOL(mcla_fast_car_shadows, false, "Display", "Fast (cheaper) car shadows");
MCLA_CVAR_BOOL(mcla_foliage_impostors, true, "Display", "Foliage impostors for distant trees");
MCLA_CVAR_BOOL(mcla_fullscreen_blur, true, "Display", "Fullscreen blur post effect");
MCLA_CVAR_BOOL(mcla_ambient_occlusion, true, "Display", "Ambient occlusion (game dev switch noao when off)");
MCLA_CVAR_BOOL(mcla_shadows, true, "Display", "Guest shadows (game dev switch noshadows when off)");

// Launcher/menu settings whose readers already exist but whose registrations did not.
// Defaults reproduce the current no-env behaviour; the launcher's env values are
// bridged into these CVars before the window/render thread start.
MCLA_CVAR_STRING(mcla_profile_name, "SWA", "Profile", "Synthetic local profile name (startup only)");
MCLA_CVAR_BOOL(mcla_depth_of_field, true, "Display", "Depth of field (live toggle)");
MCLA_CVAR_STRING(display_mode, "windowed", "Display", "Window mode: windowed | fullscreen | borderless | borderless_fullscreen");
MCLA_CVAR_BOOL(fullscreen, false, "Display", "Fullscreen window (live toggle)");
MCLA_CVAR_BOOL(vsync, false, "Display", "D3D12 present vsync (live toggle)");
MCLA_CVAR_INT(resolution_scale, 1, "Display", "Internal render scale 1..3 (next start)");
MCLA_CVAR_FLOAT(mcla_lod_scale, 1.0f, "Display", "Guest LOD distance scale (next start)");
MCLA_CVAR_STRING(mcla_button_prompts, "auto", "Controls", "Button prompts: auto (Xbox fallback) | xbox | playstation");
MCLA_CVAR_STRING(mcla_intro, "normal", "Controls", "Intro: normal | skip (next start)");
MCLA_CVAR_BOOL(mcla_render_2stage, true, "Performance", "Two-stage render pipeline (next start)");
MCLA_CVAR_BOOL(mcla_lean_census, true, "Performance", "Lean shared census counters (next start)");
MCLA_CVAR_BOOL(mcla_lean_copy, true, "Performance", "Lean per-draw copy path (next start)");

static bool MenuEnvBridgeDisabled() {
    static const bool disabled = [] {
        const char* e = std::getenv("MCLA_NO_MENU_ENV_BRIDGE");
        return e != nullptr && e[0] == '1';
    }();
    return disabled;
}

static bool MenuDisplayApplyDisabled() {
    static const bool disabled = [] {
        const char* e = std::getenv("MCLA_NO_MENU_DISPLAY_APPLY");
        return e != nullptr && e[0] == '1';
    }();
    return disabled;
}

static std::string SanitizeProfileName(std::string_view value) {
    std::string out;
    for (unsigned char c : value) {
        if (c >= 0x20u && c < 0x7Fu && out.size() < 15u) out.push_back(static_cast<char>(c));
    }
    return out;
}

static void SetProcessEnv(const char* name, const char* value) {
    (void)SetEnvironmentVariableA(name, value);
}

static bool UnquoteTomlValue(std::string_view value, std::string& out) {
    const size_t first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return false;
    value = value.substr(first);
    const size_t last = value.find_last_not_of(" \t\r");
    if (last == std::string_view::npos) return false;
    value = value.substr(0, last + 1u);
    if (value.size() < 2u || value.front() != '"' || value.back() != '"') return false;
    out.clear();
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '\\' && i + 2 < value.size() &&
            (value[i + 1] == '"' || value[i + 1] == '\\')) {
            out.push_back(value[++i]);
        } else {
            out.push_back(value[i]);
        }
    }
    return true;
}

// The standalone launcher writes TOML strings with quotes. CVarSystem's loader
// deliberately does not parse TOML, so normalize only quoted values for CVars
// that are actually registered. Numeric/boolean values remain untouched.
static void NormalizeQuotedMenuConfig(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return;
    auto& cv = mcla::cvar::CVarSystem::Instance();
    size_t changed = 0;
    std::string line;
    while (std::getline(in, line)) {
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        const size_t eq = line.find('=', start);
        if (eq == std::string::npos) continue;
        std::string key = line.substr(start, eq - start);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        std::string value;
        if (!cv.Has(key.c_str()) || !UnquoteTomlValue(std::string_view(line).substr(eq + 1u), value)) continue;
        (void)cv.SetString(key.c_str(), value);
        ++changed;
    }
    if (changed != 0u)
        MCLA_LOG_INFO("SETTINGS-WIRE: normalized {} quoted TOML CVar value(s)", changed);
}

static void BridgeLauncherEnvToCvars() {
    if (MenuEnvBridgeDisabled()) return;
    auto& cv = mcla::cvar::CVarSystem::Instance();

    if (const char* e = std::getenv("MCLA_PROFILE_NAME"); e && e[0] != '\0') {
        const std::string name = SanitizeProfileName(e);
        (void)cv.SetString("mcla_profile_name", name);
        MCLA_LOG_INFO("SETTINGS-WIRE: MCLA_PROFILE_NAME -> mcla_profile_name=\"{}\"", name);
    } else {
        const std::string name = SanitizeProfileName(cv.GetString("mcla_profile_name"));
        SetProcessEnv("MCLA_PROFILE_NAME", name.empty() ? nullptr : name.c_str());
    }

    if (const char* e = std::getenv("MCLA_DISPLAY_MODE"); e && e[0] != '\0') {
        (void)cv.SetString("display_mode", e);
        (void)cv.SetBool("fullscreen", std::strcmp(e, "fullscreen") == 0 ||
                                      std::strcmp(e, "borderless_fullscreen") == 0 ||
                                      std::strcmp(e, "borderless") == 0);
        MCLA_LOG_INFO("SETTINGS-WIRE: MCLA_DISPLAY_MODE={} -> display_mode/fullscreen", e);
    } else if (cv.GetSource("display_mode") == mcla::cvar::CVarSystem::Source::Default) {
        (void)cv.SetString("display_mode", cv.GetBool("fullscreen") ? "borderless_fullscreen" : "windowed");
    } else {
        const std::string_view mode = cv.GetString("display_mode");
        (void)cv.SetBool("fullscreen", mode == "fullscreen" || mode == "borderless_fullscreen" || mode == "borderless");
    }
    SetProcessEnv("MCLA_DISPLAY_MODE", std::string(cv.GetString("display_mode")).c_str());

    if (const char* e = std::getenv("MCLA_VSYNC"); e && e[0] != '\0')
        (void)cv.SetBool("vsync", e[0] == '1');
    SetProcessEnv("MCLA_VSYNC", cv.GetBool("vsync") ? "1" : "0");

    if (const char* e = std::getenv("MCLA_RENDER_SCALE"); e && e[0] != '\0') {
        char* end = nullptr;
        const unsigned long v = std::strtoul(e, &end, 10);
        if (end != e && *end == '\0' && v >= 1ul && v <= 3ul)
            (void)cv.SetInt("resolution_scale", static_cast<int>(v));
    }
    SetProcessEnv("MCLA_RENDER_SCALE", std::to_string(cv.GetInt("resolution_scale")).c_str());

    if (const char* e = std::getenv("MCLA_AMBIENT_OCCLUSION"); e && e[0] != '\0') {
        const bool on = e[0] != '0';
        (void)cv.SetBool("mcla_ambient_occlusion", on);
        MCLA_LOG_INFO("SETTINGS-WIRE: MCLA_AMBIENT_OCCLUSION={} -> mcla_ambient_occlusion={}",
                      e, on ? "true" : "false");
    }
    if (const char* e = std::getenv("MCLA_SHADOWS"); e && e[0] != '\0') {
        const bool on = e[0] != '0';
        (void)cv.SetBool("mcla_shadows", on);
        MCLA_LOG_INFO("SETTINGS-WIRE: MCLA_SHADOWS={} -> mcla_shadows={}",
                      e, on ? "true" : "false");
    }
    // wave3_guards.cpp reads these keys during guest dev-switch registration.
    // Seed them here because callbacks are installed after this startup bridge.
    SetProcessEnv("MCLA_DEV_NOAO", cv.GetBool("mcla_ambient_occlusion") ? nullptr : "1");
    SetProcessEnv("MCLA_DEV_NOSHADOWS", cv.GetBool("mcla_shadows") ? nullptr : "1");

    if (const char* e = std::getenv("MCLA_MOTION_BLUR"); e && e[0] != '\0') {
        const bool on = e[0] != '0';
        (void)cv.SetBool("mcla_motion_blur", on);
        // wave3_menu.cpp now owns the live decision. Removing the startup env
        // lets the in-game row turn blur back on after a launcher set it off.
        SetProcessEnv("MCLA_MOTION_BLUR", nullptr);
        MCLA_LOG_INFO("SETTINGS-WIRE: MCLA_MOTION_BLUR={} -> mcla_motion_blur={}", e, on ? "true" : "false");
    }

    if (const char* e = std::getenv("MCLA_FPS_CAP"); e && e[0] != '\0') {
        const char* noUnlock = std::getenv("MCLA_NO_FPS_UNLOCK");
        if (!(noUnlock && noUnlock[0] == '1')) {
            int32_t target = -1;
            if (std::strcmp(e, "uncapped") == 0) {
                target = 0;
            } else {
                char* end = nullptr;
                const long v = std::strtol(e, &end, 10);
                if (end != e && *end == '\0') {
                    if (v < 0) target = 0;
                    else if (v > 0) target = static_cast<int32_t>(std::min(v, 240l));
                }
            }
            if (target >= 0) {
                (void)cv.SetInt("mcla_fps", target);
                MCLA_LOG_INFO("SETTINGS-WIRE: MCLA_FPS_CAP={} -> mcla_fps={}", e, target);
            }
            // EffectiveFpsTarget caches MCLA_FPS_CAP on first use. Consume it
            // into the live CVar so the menu can change the cap during play.
            SetProcessEnv("MCLA_FPS_CAP", nullptr);
        }
    }

    if (const char* e = std::getenv("MCLA_LOD_SCALE"); e && e[0] != '\0') {
        char* end = nullptr;
        const float v = std::strtof(e, &end);
        const float safe = (end != e && *end == '\0' && std::isfinite(v) && v > 0.0f && v <= 16.0f) ? v : 1.0f;
        (void)cv.SetFloat("mcla_lod_scale", safe);
        MCLA_LOG_INFO("SETTINGS-WIRE: MCLA_LOD_SCALE={} -> mcla_lod_scale={}", e, safe);
    }
    SetProcessEnv("MCLA_LOD_SCALE", std::to_string(cv.GetFloat("mcla_lod_scale")).c_str());

    if (const char* e = std::getenv("MCLA_BTN_PROMPTS_PS"); e && e[0] != '\0')
        (void)cv.SetString("mcla_button_prompts", e[0] == '1' ? "ps" : "xbox");
    if (std::string_view(cv.GetString("mcla_button_prompts")) == "playstation")
        (void)cv.SetString("mcla_button_prompts", "ps");
    const std::string_view prompts = cv.GetString("mcla_button_prompts");
    const bool psPrompts = prompts == "playstation" || prompts == "ps";
    if (const char* fix = std::getenv("MCLA_BTN_PROMPTS_FIX"); !(fix && fix[0] != '\0'))
        SetProcessEnv("MCLA_BTN_PROMPTS_FIX", psPrompts ? "1" : "0");
    SetProcessEnv("MCLA_BTN_PROMPTS_PS", psPrompts ? "1" : "0");

    if (const char* e = std::getenv("MCLA_INTRO"); e && e[0] != '\0') {
        if (std::strcmp(e, "skip") == 0) (void)cv.SetString("mcla_intro", "skip");
        else if (std::strcmp(e, "normal") == 0 || std::strcmp(e, "fast") == 0)
            (void)cv.SetString("mcla_intro", "normal");
    }
    if (std::string_view(cv.GetString("mcla_intro")) == "fast")
        (void)cv.SetString("mcla_intro", "normal");
    const char* skipIntroEnv = std::getenv("MCLA_SKIP_INTRO");
    const bool skipIntro = std::string_view(cv.GetString("mcla_intro")) == "skip" ||
                           (skipIntroEnv && skipIntroEnv[0] == '1');
    SetProcessEnv("MCLA_INTRO", skipIntro ? "skip" : "normal");
    SetProcessEnv("MCLA_SKIP_INTRO_MASK", skipIntro ? "1" : "0");

    const char* noTwoStage = std::getenv("MCLA_NO_RENDER_2STAGE");
    const char* twoStage = std::getenv("MCLA_RENDER_2STAGE");
    if (noTwoStage && noTwoStage[0] == '1') (void)cv.SetBool("mcla_render_2stage", false);
    else if (twoStage && twoStage[0] != '\0') (void)cv.SetBool("mcla_render_2stage", twoStage[0] == '1');
    SetProcessEnv("MCLA_RENDER_2STAGE", cv.GetBool("mcla_render_2stage") ? "1" : "0");
    SetProcessEnv("MCLA_NO_RENDER_2STAGE", nullptr);

    const char* noLeanCensus = std::getenv("MCLA_NO_LEAN_CENSUS");
    const char* noLeanCopy = std::getenv("MCLA_NO_LEAN_COPY");
    if (noLeanCensus && noLeanCensus[0] != '\0') (void)cv.SetBool("mcla_lean_census", noLeanCensus[0] != '1');
    if (noLeanCopy && noLeanCopy[0] != '\0') (void)cv.SetBool("mcla_lean_copy", noLeanCopy[0] != '1');
    SetProcessEnv("MCLA_NO_LEAN_CENSUS", cv.GetBool("mcla_lean_census") ? "0" : "1");
    SetProcessEnv("MCLA_NO_LEAN_COPY", cv.GetBool("mcla_lean_copy") ? "0" : "1");
}

static std::atomic<int> s_pendingFullscreen{-1};

static void InstallMenuCvarCallbacks() {
    auto& cv = mcla::cvar::CVarSystem::Instance();
    cv.AddChangeCallback("mcla_profile_name", [](std::string_view, std::string_view value) {
        const std::string name = SanitizeProfileName(value);
        SetProcessEnv("MCLA_PROFILE_NAME", name.empty() ? nullptr : name.c_str());
        MCLA_LOG_INFO("SETTINGS-MENU: profile name saved; takes effect next start");
    });
    cv.AddChangeCallback("fullscreen", [](std::string_view, std::string_view value) {
        const bool on = value == "true" || value == "1" || value == "yes" || value == "on";
        auto& cvSys = mcla::cvar::CVarSystem::Instance();
        const std::string_view curMode = cvSys.GetString("display_mode");
        const std::string_view targetMode = on ? "borderless_fullscreen" : "windowed";
        if (curMode != targetMode && (!on || (curMode != "fullscreen" && curMode != "borderless"))) {
            (void)cvSys.SetString("display_mode", targetMode);
            SetProcessEnv("MCLA_DISPLAY_MODE", std::string(targetMode).c_str());
        }
        if (!MenuDisplayApplyDisabled()) s_pendingFullscreen.store(on ? 1 : 0);
    });
    cv.AddChangeCallback("display_mode", [](std::string_view, std::string_view value) {
        const bool on = value == "fullscreen" || value == "borderless_fullscreen" || value == "borderless";
        auto& cvSys = mcla::cvar::CVarSystem::Instance();
        if (cvSys.GetBool("fullscreen") != on) {
            (void)cvSys.SetBool("fullscreen", on);
        }
        SetProcessEnv("MCLA_DISPLAY_MODE", std::string(value).c_str());
        if (!MenuDisplayApplyDisabled()) s_pendingFullscreen.store(on ? 1 : 0);
    });
    cv.AddChangeCallback("vsync", [](std::string_view, std::string_view value) {
        const bool on = value == "true" || value == "1" || value == "yes" || value == "on";
        SetProcessEnv("MCLA_VSYNC", on ? "1" : "0");
        if (MenuDisplayApplyDisabled()) return;
        auto* app = GetApp();
        auto* backend = app ? app->GetD3D12Backend() : nullptr;
        if (backend && backend->IsInitialized()) {
            auto settings = backend->GetGraphicsSettings();
            if (settings.vsync != on) {
                settings.vsync = on;
                backend->SetGraphicsSettings(settings);
            }
        }
    });
    cv.AddChangeCallback("resolution_scale", [](std::string_view, std::string_view value) {
        SetProcessEnv("MCLA_RENDER_SCALE", std::string(value).c_str());
        MCLA_LOG_INFO("SETTINGS-MENU: resolution scale saved; takes effect next start");
    });
    cv.AddChangeCallback("mcla_lod_scale", [](std::string_view, std::string_view value) {
        SetProcessEnv("MCLA_LOD_SCALE", std::string(value).c_str());
        MCLA_LOG_INFO("SETTINGS-MENU: LOD scale saved; takes effect next start");
    });
    cv.AddChangeCallback("mcla_ambient_occlusion", [](std::string_view, std::string_view value) {
        const bool on = value == "true" || value == "1" || value == "yes" || value == "on";
        SetProcessEnv("MCLA_DEV_NOAO", on ? nullptr : "1");
        MCLA_LOG_INFO("SETTINGS-MENU: ambient occlusion saved; takes effect next start");
    });
    cv.AddChangeCallback("mcla_shadows", [](std::string_view, std::string_view value) {
        const bool on = value == "true" || value == "1" || value == "yes" || value == "on";
        SetProcessEnv("MCLA_DEV_NOSHADOWS", on ? nullptr : "1");
        MCLA_LOG_INFO("SETTINGS-MENU: shadows saved; takes effect next start");
    });
    cv.AddChangeCallback("mcla_button_prompts", [](std::string_view, std::string_view value) {
        const bool ps = value == "playstation" || value == "ps";
        SetProcessEnv("MCLA_BTN_PROMPTS_FIX", ps ? "1" : "0");
        SetProcessEnv("MCLA_BTN_PROMPTS_PS", ps ? "1" : "0");
    });
    cv.AddChangeCallback("mcla_intro", [](std::string_view, std::string_view value) {
        const bool skip = value == "skip";
        SetProcessEnv("MCLA_INTRO", skip ? "skip" : "normal");
        SetProcessEnv("MCLA_SKIP_INTRO_MASK", skip ? "1" : "0");
        SetProcessEnv("MCLA_SKIP_INTRO", nullptr);
        MCLA_LOG_INFO("SETTINGS-MENU: intro saved; takes effect next start");
    });
    cv.AddChangeCallback("mcla_render_2stage", [](std::string_view, std::string_view value) {
        const bool on = value == "true" || value == "1" || value == "yes" || value == "on";
        SetProcessEnv("MCLA_RENDER_2STAGE", on ? "1" : "0");
        SetProcessEnv("MCLA_NO_RENDER_2STAGE", nullptr);
        MCLA_LOG_INFO("SETTINGS-MENU: two-stage render saved; takes effect next start");
    });
    cv.AddChangeCallback("mcla_lean_census", [](std::string_view, std::string_view value) {
        const bool on = value == "true" || value == "1" || value == "yes" || value == "on";
        SetProcessEnv("MCLA_NO_LEAN_CENSUS", on ? "0" : "1");
        MCLA_LOG_INFO("SETTINGS-MENU: lean census saved; takes effect next start");
    });
    cv.AddChangeCallback("mcla_lean_copy", [](std::string_view, std::string_view value) {
        const bool on = value == "true" || value == "1" || value == "yes" || value == "on";
        SetProcessEnv("MCLA_NO_LEAN_COPY", on ? "0" : "1");
        MCLA_LOG_INFO("SETTINGS-MENU: lean copy saved; takes effect next start");
    });
}

// Wave 3 (§2.4 / §1.4): relaunch hand-off. A restarted process is launched with
// `--mcla_wait_pid=<hex>` (by MclaRelaunchGame in the settings-menu worker); it
// waits for that PID to exit before touching SDL/D3D12, so the old instance has
// released the window, device and audio endpoint. No argument = no wait, i.e.
// byte-for-byte the previous startup path.
struct RelaunchWaitResult {
    bool          hadArg = false;
    unsigned long pid = 0;
    bool          completed = false;
    bool          timedOut = false;
};
static RelaunchWaitResult s_relaunchWaitResult;

static void MclaWaitForLauncherPid()
{
    const char* cmd = GetCommandLineA();
    constexpr const char* kArg = "--mcla_wait_pid=";
    const char* p = std::strstr(cmd, kArg);
    if (!p) return;
    p += std::strlen(kArg);
    const unsigned long pidValue = std::strtoul(p, nullptr, 16);
    if (pidValue == 0 || pidValue > 0xFFFFFFFFul) return;
    s_relaunchWaitResult.hadArg = true;
    s_relaunchWaitResult.pid = pidValue;

    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pidValue));
    if (!h) {
        MCLA_LOG_WARN("RELAUNCH-WAIT pid={} OpenProcess failed (err={}) - starting anyway",
                      pidValue, GetLastError());
        return;
    }
    MCLA_LOG_INFO("RELAUNCH-WAIT pid={} waiting for previous instance to exit", pidValue);
    // Bounded: a wedged parent must not hold the new instance hostage forever.
    constexpr DWORD kWaitStepMs = 100;
    constexpr DWORD kWaitLimitMs = 30000;
    for (DWORD waited = 0; waited < kWaitLimitMs; waited += kWaitStepMs) {
        if (WaitForSingleObject(h, kWaitStepMs) == WAIT_OBJECT_0) {
            s_relaunchWaitResult.completed = true;
            MCLA_LOG_INFO("RELAUNCH-WAIT pid={} exited after {} ms", pidValue, waited + kWaitStepMs);
            CloseHandle(h);
            return;
        }
    }
    s_relaunchWaitResult.timedOut = true;
    MCLA_LOG_WARN("RELAUNCH-WAIT pid={} still alive after {} ms - starting anyway",
                  pidValue, kWaitLimitMs);
    CloseHandle(h);
}

static App* g_appInstance = nullptr;

App::App(const char* name) : m_name(name) {
    g_appInstance = this;
}

App::~App() {
    Shutdown();
    g_appInstance = nullptr;
}

App* GetApp() {
    return g_appInstance;
}

bool App::Initialize() {
    // Wave 3 (§2.4): a relaunch waits for its predecessor before any SDL/D3D12
    // state is created. No flag -> immediate no-op.
    MclaWaitForLauncherPid();

    // Initialize logging first so all subsequent log calls have a sink.
    // InitPaths() must run first to compute m_cacheRoot for the log file path.
    // Lines emitted before this point (InitSDL logs; InitPaths does not) are not
    // dropped: spdlog's default logger is a stdout sink -> they miss mcla.log.
    if (!InitSDL()) return false;
    if (!InitPaths()) return false;

    mcla::log::Initialize(m_name.c_str(), mcla::log::Level::Info,
                      (m_cacheRoot / "mcla.log").string().c_str());
    // Quiet by default: a boot logged 1.7M lines / 400 MB and held Legals on screen for minutes.
    // MCLA_LOG_LEVEL=2 restores the full census for diagnosis (0..6 = trace..off).
    // When unset (normal playable launch), default to Level::Off so zero log I/O or formatting occurs.
    const char* logLevel = std::getenv("MCLA_LOG_LEVEL");
    mcla::log::SetLevel(logLevel && logLevel[0] >= '0' && logLevel[0] <= '6'
                            ? static_cast<mcla::log::Level>(logLevel[0] - '0')
                            : mcla::log::Level::Off);

    MCLA_LOG_INFO("Initializing {}...", m_name);
    // Re-emit the wait outcome now that mcla.log has a sink (the pre-init rows
    // at the top of Initialize only reach the console).
    if (s_relaunchWaitResult.hadArg)
        MCLA_LOG_INFO("RELAUNCH-WAIT-SUM pid={} completed={} timedOut={}",
                      s_relaunchWaitResult.pid, s_relaunchWaitResult.completed ? 1 : 0,
                      s_relaunchWaitResult.timedOut ? 1 : 0);

    // The PPC ABI load/store macros route through the checked guest-memory view.
    SetActiveGuestMemoryView(&m_guestMemoryView);

    // Load CVars before the window (display_mode) and before InitD3D12 (renderer_mode).
    mcla::cvar::CVarSystem::Instance().LoadConfig(m_cacheRoot / "mcla.toml");
    NormalizeQuotedMenuConfig(m_cacheRoot / "mcla.toml");
    BridgeLauncherEnvToCvars();
    InstallMenuCvarCallbacks();
    if (!CreateSDLWindow()) return false;

    if (!InitD3D12()) return false;

    MCLA_LOG_INFO("Game data root: {}", m_gameDataRoot.string());
    MCLA_LOG_INFO("Cache root: {}", m_cacheRoot.string());

    // M5: open the SDL3 audio stream (48 kHz stereo s16). Failure is non-fatal
    // — the game must keep booting even with no output device.
    apu::Init();

    // F-609: open whatever controllers are already attached and take the
    // first sample BEFORE the guest boot worker is started below. SDL_Init
    // queued SDL_EVENT_GAMEPAD_ADDED for anything already plugged in, but
    // those events are not processed until MainLoop runs - and the guest
    // thread would otherwise poll XamInputGetState against a snapshot that
    // had never been filled, getting DEVICE_NOT_CONNECTED for port 0 during
    // exactly the window in which a title decides whether it has a pad.
    XamInputInit();

    // Phase 5 (BOOT_REBUILD_PLAN.md - retired, cf. src/boot_host.cpp:5): load
    // the game image into the 4 GiB guest window and prepare the boot context.
    // Must run after logging + paths (boot diagnostics need a sink) and before
    // mcla_ApplyPatches (guest-memory hooks need a live window).
    const std::filesystem::path xexPath = m_gameDataRoot / "default.xex";
    if (std::filesystem::exists(xexPath) && boot::LoadAndPrepare(xexPath.string(), m_bootEntry)) {
        MCLA_LOG_INFO("Guest image loaded; entry @0x{:08X}", m_bootEntry);
    } else {
        MCLA_LOG_WARN("Boot host not started: {} missing or load failed", xexPath.string());
    }

    // Wave 3 (§2.3): write the guest's 16:9 constant at 0x8201E7EC. Worker 2's
    // MclaApplyAspectRatio() is itself gated on MCLA_ASPECT_FIX=1 and uses
    // call_once, so with no env set this is a no-op. It must run AFTER
    // boot::LoadAndPrepare adopted GuestMemoryHeap, otherwise the checked read
    // of that guest VA cannot see the image (wave3_polish.cpp:160-164).
    MclaApplyAspectRatio();

    // Apply patches (installs PPC hooks). The dispatcher was never
    // instantiated before 2026-08-23 - mcla_ApplyPatches silently no-op'd on
    // every boot, leaving CP MMIO routing (doorbell consumer) and the whole
    // patch layer dead. Found via doorbell-never-drains trace.
    m_dispatcher = std::make_unique<FunctionDispatcher>();
    if (m_dispatcher) {
        mcla_ApplyPatches(m_dispatcher.get());
        // renderer_mode comes solely from the CVar system: the previous hard-coded
        // SetString("renderer_mode","native") is gone - the only writer left is the
        // LoadConfig() above. Compiled default is "native" (src/renderer_mode.cpp:8);
        // config/mcla_manifest.toml:16's "legacy" is inert - no src/ reader (F-053).
        mcla::native::InstallNativeRenderer(m_dispatcher.get());
    }

    // Initialize the RPF VFS: back-end for guest file IO (Nt*File hooks, RSC tab)
    mcla::vfs::RpfVirtualFileSystem& vfs = mcla::vfs::RpfVirtualFileSystem::Instance();
    std::filesystem::path extractedRoot = m_gameDataRoot / "mcla extracted cache";
    // T43.18 (2026-10-13): init UNCONDITIONALLY. The extracted dir is our
    // invention, not a game requirement: Initialize() no longer fails when
    // it is absent (warns, indexes game_data only) and nothing here creates
    // it. A missing dir must change nothing about the boot.
    if (vfs.Initialize(extractedRoot.string())) {
        vfs.Mount();
        MCLA_LOG_INFO("VFS initialized and mounted");
    } else {
        MCLA_LOG_ERROR("VFS: Initialize FAILED for '{}' -- guest file IO "
                       "will refuse every archive open (audlo fatal follows)",
                       extractedRoot.string());
    }

m_running = true;

    // Start GPU context poller (Phase 4) - waits for game to allocate GPU context
    StartGpuContextPoller();

    // P4.5': Start the dedicated render thread before boot::Start() (which blocks
    // in its 5-min watchdog loop; W0 below runs it on a detached thread). It
    // inits D3D12 on entry, then pops - kept awake by its own 33 ms heartbeat.
    mcla::native::g_renderThread.start();

    // Start the guest boot worker once the host services it will touch (VFS,
    // kernel stubs, logging) are up. The worker parks in the game main loop;
    // its outcome is reported in mcla.log by the boot watchdog.
    // NOTE: boot::Start blocks (watchdog loop) — render thread must already be up.
    // W0: do not block Initialize on the boot watchdog — the SDL main loop
    // must keep pumping events so the window stays visible even if the guest
    // hits a fatal and parks.
    if (m_bootEntry != 0) {
        std::thread([entry = m_bootEntry]() { mcla::diag::NameThisThread("boot-launcher");
            boot::Start(entry);
        }).detach();
    }

    return true;
}

void App::Run() {
    MainLoop();
}

static void WaitForSavesBeforeShutdown() {
    using Clock = std::chrono::steady_clock;
    auto saveInProgress = []() {
        return ::McHasUnclosedSaveBackups() || ::McHasOpenContentWriteHandles();
    };
    if (!saveInProgress()) {
        return;
    }
    const auto start = Clock::now();
    MCLA_LOG_INFO("SAVE-SHUTDOWN: save in progress; waiting up to 10 s for completion");
    while (saveInProgress() && (Clock::now() - start) < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto waitedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    if (saveInProgress()) {
        MCLA_LOG_WARN("SAVE-SHUTDOWN: save still in progress after {} ms", waitedMs);
    } else {
        MCLA_LOG_INFO("SAVE-SHUTDOWN: save finished after {} ms", waitedMs);
    }
}

void App::Shutdown() {
    if (!m_running && m_window == nullptr) return;

    m_running = false;
    MclaPumpWakeShutdownBegins();
    WaitForSavesBeforeShutdown();

    // Close audio before tearing down SDL so the stream/device goes away
    // cleanly and does not race SDL_Quit.
    apu::Shutdown();

    // P4.5': Stop the render thread before tearing down D3D12/window.
    // The render thread owns D3D12 and shuts it down in stop().
    mcla::native::g_renderThread.stop();

    // Wave 3 (§1.1): selective save. Only the menu-owned CVar names are
    // rewritten; every other line in mcla.toml (comments, [tables],
    // game_data_root / cache_root / renderer_mode from the launcher) is kept
    // verbatim by SaveConfigValues (cvar.h:186). Names that were never
    // registered are simply skipped, so this is safe across build variants.
    static const std::vector<std::string> kMenuSaveNames = {
        "mcla_settings_menu", "mcla_profile_name", "display_mode", "renderer_mode",
        "mcla_motion_blur", "mcla_depth_of_field", "mcla_display_gamma", "mcla_aspect_ratio",
        "mcla_lod_scale", "mcla_fps", "mcla_dt_max",
        "mcla_vibration", "mcla_left_stick_deadzone", "mcla_right_stick_deadzone",
        "mcla_script_pad_latch_ms", "mcla_hide_cursor",
        "mcla_fullscreen_blur", "mcla_race_shadows", "mcla_fast_car_shadows",
        "mcla_foliage_impostors", "mcla_ambient_occlusion", "mcla_shadows",
        "mcla_float_clock", "mcla_swap_unlock", "mcla_pace_at_swap",
        "mcla_physics_continuous", "mcla_checkpoint_sweep", "mcla_poll_wait",
        "mcla_poll_spin_us", "mcla_poll_tail_us", "mcla_pump_wake_on_demand",
        "mcla_pump_sleep_max_ms", "mcla_pump_wake_pause_s", "mcla_patch_groups",
        "mcla_render_2stage", "mcla_lean_census", "mcla_lean_copy",
        // Kept for build variants that register these rows; absent CVars are skipped.
        "mcla_ui_interpolate", "mcla_button_prompts", "mcla_intro",
        "vsync", "fullscreen", "resolution_scale"
    };
    if (!mcla::cvar::CVarSystem::Instance().SaveConfigValues(GetConfigPath(), kMenuSaveNames)) {
        MCLA_LOG_WARN("CONFIG-SAVE: no registered menu CVars to save at {}",
                      GetConfigPath().string());
    }

    // Unmount VFS
    mcla::vfs::RpfVirtualFileSystem::Instance().Unmount();

    // F-609: stop the motors and release every SDL_Gamepad* before SDL_Quit -
    // SDL_CloseGamepad must run while the joystick subsystem is still alive.
    XamInputShutdown();

    // Destroy window
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        m_hwnd = nullptr;
    }

    SDL_Quit();
    mcla::log::Shutdown();

    MCLA_LOG_INFO("Shutdown complete");
}

bool App::InitSDL() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        MCLA_LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    MCLA_LOG_INFO("SDL initialized");
    return true;
}

bool App::InitPaths() {
    // Get executable directory
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

    // Game data root: cvar undeclared in src/ (reads "") -> exe_dir/game_data
    std::string gameDataCvar(MCLA_CVAR_GET_STRING(game_data_root));
    if (!gameDataCvar.empty()) {
        m_gameDataRoot = gameDataCvar;
    } else {
        m_gameDataRoot = exeDir / "game_data";
        if (!std::filesystem::exists(m_gameDataRoot)) {
            m_gameDataRoot = "game_data";
        }
    }

    // Cache root: cvar undeclared in src/ (reads "") -> exe_dir/cache
    std::string cacheCvar(MCLA_CVAR_GET_STRING(cache_root));
    if (!cacheCvar.empty()) {
        m_cacheRoot = cacheCvar;
    } else {
        m_cacheRoot = exeDir / "cache";
    }

    std::error_code ec;
    std::filesystem::create_directories(m_cacheRoot, ec);

    return true;
}

bool App::CreateSDLWindow() {
    // Not resizable (no drag-resize / maximize). Display mode: mcla.toml `display_mode = windowed | fullscreen |
    // borderless | borderless_fullscreen` (env MCLA_DISPLAY_MODE overrides; legacy `fullscreen = true` = borderless_fullscreen).
    Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    std::string mode(MCLA_CVAR_GET_STRING(display_mode));
    if (const char* e = std::getenv("MCLA_DISPLAY_MODE")) mode = e;
    if (mode.empty() && MCLA_CVAR_GET_BOOL(fullscreen)) mode = "borderless_fullscreen";
    const bool exclusive = mode == "fullscreen", borderlessFs = mode == "borderless_fullscreen";
    if (exclusive || borderlessFs) flags |= SDL_WINDOW_FULLSCREEN;
    if (mode == "borderless") flags |= SDL_WINDOW_BORDERLESS;

    // MCLA_WINDOW_W/H: initial window size (guest output is still 1280x720, scaled at present). Range-checked like run_mcla.bat.
    if (const char* ew = std::getenv("MCLA_WINDOW_W")) if (const char* eh = std::getenv("MCLA_WINDOW_H")) {
        const long w = std::strtol(ew, nullptr, 10), h = std::strtol(eh, nullptr, 10);
        if (w >= 640 && w <= 7680 && h >= 480 && h <= 4320) { m_width = uint32_t(w); m_height = uint32_t(h); }
    }
    m_window = SDL_CreateWindow(m_name.c_str(), m_width, m_height, flags);
    if (!m_window) {
        MCLA_LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    if (exclusive) {  // exclusive fullscreen at the closest display mode to the requested size; borderless_fullscreen = desktop mode (null)
        SDL_DisplayMode dm{};
        if (!SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(m_window), int(m_width), int(m_height), 0.0f, true, &dm)) {
            MCLA_LOG_ERROR("FS-DISPLAY-FIX closest-mode FAILED req={}x{} sdlerr={}", m_width, m_height, SDL_GetError());
        } else if (!SDL_SetWindowFullscreenMode(m_window, &dm)) {
            // Previously unchecked: when this fails the window silently stays in desktop
            // (borderless) mode and no ERROR reached the log. P6 round 2.
            MCLA_LOG_ERROR("FS-DISPLAY-FIX SetWindowFullscreenMode FAILED mode={}x{} sdlerr={}", dm.w, dm.h, SDL_GetError());
        } else {
            SDL_SyncWindow(m_window);  // mode switch is async on some backends; make it immediate
            const SDL_DisplayMode* stored = SDL_GetWindowFullscreenMode(m_window);
            MCLA_LOG_ERROR("FS-DISPLAY-FIX exclusive OK req={}x{} mode={}x{} stored={}x{}",
                           m_width, m_height, dm.w, dm.h, stored ? stored->w : 0, stored ? stored->h : 0);
        }
    }
    MCLA_LOG_ERROR("DISPLAY-MODE {} ({}x{})", mode.empty() ? "windowed" : mode, m_width, m_height);

    // Get HWND for D3D12
    SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
    m_hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!m_hwnd) {
        MCLA_LOG_ERROR("Failed to get HWND from SDL window");
        return false;
    }

    // Get actual drawable size
    int w, h;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
    m_width = static_cast<uint32_t>(w);
    m_height = static_cast<uint32_t>(h);

    SDL_ShowWindow(m_window);
    // Keyboard input: the guest's menus read SDL keyboard state, so the game
    // window must own focus. Ask the OS for foreground (Windows may refuse if
    // another process holds it — the user can click the game window).
    SDL_RaiseWindow(m_window);
#if defined(_WIN32)
    {
        // Windows refuses SetForegroundWindow from a background process. Attach
        // to the foreground thread briefly so the game can claim focus when it
        // is launched from a terminal/launcher. Opt out with MCLA_NO_FORCE_FOCUS=1.
        static const bool s_forceFocus = [] {
            const char* e = std::getenv("MCLA_NO_FORCE_FOCUS");
            return !(e && e[0] == '1');
        }();
        HWND hw = reinterpret_cast<HWND>(m_hwnd);
        HWND fg = GetForegroundWindow();
        if (s_forceFocus && fg && fg != hw) {
            DWORD fgThread = GetWindowThreadProcessId(fg, nullptr);
            DWORD me = GetCurrentThreadId();
            if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, TRUE);
            SetForegroundWindow(hw);
            BringWindowToTop(hw);
            if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, FALSE);
        } else if (!s_forceFocus) {
            SetForegroundWindow(hw);
        }
    }
#endif
    MCLA_LOG_ERROR("WINDOW-FOCUS requested focused={}", SDL_GetKeyboardFocus() == m_window ? 1 : 0);
    // Definitive post-show state: SDL's own fullscreen flag + actual drawable pixels.
    // This is the marker a boot checks to prove the window fills the display with
    // no title bar/taskbar (fullscreen=1 and size = requested display mode).
    MCLA_LOG_ERROR("FS-DISPLAY-FIX window visible fullscreen={} size={}x{}",
                   (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) ? 1 : 0, m_width, m_height);
    // Hand the SDL flags to the D3D12 render thread so its FULLSCREEN-STATE line
    // can correlate the DXGI transition with the window state.
    mcla::native::SetSdlWindowFlags(static_cast<uint64_t>(SDL_GetWindowFlags(m_window)));
    MCLA_LOG_INFO("Window created: {}x{} HWND=0x{:X}", m_width, m_height, reinterpret_cast<uintptr_t>(m_hwnd));
    return true;
}

bool App::InitD3D12() {
    // D3D12 initialization is deferred to the render thread (P4′ Step 3).
    // The render thread owns all D3D12 calls. We just store the window params
    // and the render thread will initialize D3D12 when it starts.
    if (mcla::renderer::GetRendererMode() == mcla::renderer::RendererMode::Native) {
        mcla::native::g_renderThread.SetWindowParams(m_hwnd, m_width, m_height);
        MCLA_LOG_INFO("D3D12 deferred to render thread (HWND=0x{:X} {}x{})",
                      reinterpret_cast<uintptr_t>(m_hwnd), m_width, m_height);
    } else {
        MCLA_LOG_INFO("Renderer mode is legacy/capture; D3D12 not needed");
    }
    return true;
}

void App::MainLoop() {
    MCLA_LOG_INFO("Entering main loop");

    while (m_running) {
        // CVar callbacks may run on a guest thread. Apply the SDL window change
        // here, on the SDL/main thread, and resize the swap chain through the
        // existing render-command path.
        const int fullscreen = s_pendingFullscreen.exchange(-1);
        if (fullscreen >= 0 && m_window && !MenuDisplayApplyDisabled()) {
            const bool current = (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
            if (current != (fullscreen != 0)) {
                if (fullscreen != 0) (void)SDL_SetWindowFullscreenMode(m_window, nullptr);
                if (!SDL_SetWindowFullscreen(m_window, fullscreen != 0)) {
                    MCLA_LOG_ERROR("SETTINGS-MENU: fullscreen change failed: {}", SDL_GetError());
                } else {
                    SDL_SyncWindow(m_window);
                    int w = 0, h = 0;
                    SDL_GetWindowSizeInPixels(m_window, &w, &h);
                    if (w > 0 && h > 0) OnResize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                    mcla::native::SetSdlWindowFlags(static_cast<uint64_t>(SDL_GetWindowFlags(m_window)));
                    MCLA_LOG_INFO("SETTINGS-MENU: fullscreen={} size={}x{}", fullscreen != 0, w, h);
                }
            }
        }
        HandleEvents();

        // MCLA_LEGALS_STATE=1 (swarm p2-analyze): log the boot legal-SWF object
        // global and its countdown/visibility fields at title. Read-only.
        {
            static const bool s_legalsState = [] {
                const char* e = std::getenv("MCLA_LEGALS_STATE");
                return e && e[0] == '1';
            }();
            if (s_legalsState) {
                static uint64_t s_lastMs = 0;
                const uint64_t now = SDL_GetTicks();
                if (now - s_lastMs >= 2000u) {
                    s_lastMs = now;
                    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
                    uint32_t obj = 0, count = 0, vis = 0;
                    const bool ok = mem.ReadU32BE(0x82830B14u, &obj);
                    if (ok && obj) {
                        (void)mem.ReadU32BE(obj + 8u, &count);
                        (void)mem.ReadU32BE(obj + 12u, &vis);
                    }
                    MCLA_LOG_ERROR("LEGALS-STATE obj={:08X} count={} vis={} ok={}", obj, count, vis, ok ? 1 : 0);
                    // Swarm p2-analyze root cause: the legal-SWF object's +8 countdown
                    // starts at 10000 but the guest pump only decrements it while
                    // <=1000, so the object is never unloaded and its glyph draws
                    // bleed into the title. After the legal screen's normal window,
                    // write 0 via the checked helper so the GUEST's own per-frame
                    // unloader (sub_821FAA98) frees it. MCLA_NO_LEGALS_FIX=1 opts out.
                    // OPT-IN only: zeroing the whole object also removes the title
                    // logo (same SWF), so this is a diagnostic, not a default fix.
                    static const bool s_legalsFix = [] {
                        const char* e = std::getenv("MCLA_LEGALS_FIX");
                        return e && e[0] == '1';
                    }();
                    if (s_legalsFix && ok && obj && count != 0u && now >= 12000u) {
                        (void)mem.WriteU32BE(obj + 8u, 0u);
                        MCLA_LOG_ERROR("LEGALS-FIX zeroed +8 obj={:08X} after {} ms", obj, now);
                    }
                }
            }
        }

        if (!m_running) break;

        // Wave 3 (§2.4): cursor auto-hide. SDL cursor APIs are main-thread-only,
        // so this stays in the event loop. With mcla_hide_cursor off the cursor
        // is simply shown and no timer/gamepad query is consulted.
        if (!MCLA_CVAR_GET_BOOL(mcla_hide_cursor)) {
            if (m_cursorHidden) {
                SDL_ShowCursor();
                m_cursorHidden = false;
            }
        } else if (m_window && !m_isMinimized && !m_cursorHidden &&
                   (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MOUSE_FOCUS)) {
            const uint64_t now = SDL_GetTicks();
            if ((now - m_lastMouseMs) >= 3300u ||
                XamInputHadRecentGamepadActivity(m_lastMouseMs)) {
                SDL_HideCursor();
                m_cursorHidden = true;
            }
        }

        // Optional frame callback: never set today (no SetFrameCallback caller)
        if (m_frameCallback) {
            m_frameCallback();
        }

        // Small sleep to prevent 100% CPU when idle
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    MCLA_LOG_INFO("Main loop exited");
}

static bool F9ProbeDisabled() {
    static const bool v = [] { const char* e = std::getenv("MCLA_NO_F9_PROBE"); return e && e[0] == '1'; }();
    return v;
}

void App::HandleEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                MCLA_LOG_ERROR("APP-QUIT reason=SDL_EVENT_QUIT");
                m_running = false;
                break;

            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                OnResize(static_cast<uint32_t>(event.window.data1),
                         static_cast<uint32_t>(event.window.data2));
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                MCLA_LOG_ERROR("APP-QUIT reason=WINDOW_CLOSE_REQUESTED");
                m_running = false;
                break;

            // F-609: hot-plug. SDL_Init(SDL_INIT_GAMEPAD) is set at
            // src/app.cpp:179 but nothing ever opened a controller, so pads
            // that were attached at launch and pads attached later were both
            // invisible to the guest. The main thread owns every SDL_Gamepad*
            // and does all of the sampling (see the threading model in
            // kernel/xam.cpp).
            case SDL_EVENT_GAMEPAD_ADDED:
                XamInputGamepadAdded(event.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_REMOVED:
                XamInputGamepadRemoved(event.gdevice.which);
                break;

            // Wave 3 (§2.4): any mouse activity or focus gain restarts the idle
            // timer and brings a hidden cursor straight back.
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                m_lastMouseMs = SDL_GetTicks();
                if (m_cursorHidden) {
                    SDL_ShowCursor();
                    m_cursorHidden = false;
                }
                break;

            // Never leave the cursor hidden once focus has left the window.
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                if (m_cursorHidden) {
                    SDL_ShowCursor();
                    m_cursorHidden = false;
                }
                break;

            // F9 owner pixel probe: record every draw covering the pixel under the cursor
            // (owner_reports/f9_*). MCLA_NO_F9_PROBE=1 disables the key.
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_F9 && !event.key.repeat && !F9ProbeDisabled()) {
                    float mx = 0.0f, my = 0.0f;
                    int ww = 0, wh = 0;
                    SDL_GetMouseState(&mx, &my);
                    SDL_GetWindowSize(m_window, &ww, &wh);
                    if (ww > 0 && wh > 0)
                        mcla::native::D3D12Backend::F9RequestCapture(mx / static_cast<float>(ww), my / static_cast<float>(wh));
                }
                break;

            default:
                break;
        }
    }
    // TEST-ONLY: MCLA_F9_AUTO_MS=<ms since SDL init> fires one F9 at window PIXEL (MCLA_F9_AUTO_X, MCLA_F9_AUTO_Y).
    {
        static const uint64_t s_autoMs = [] { const char* e = std::getenv("MCLA_F9_AUTO_MS"); return e && *e ? std::strtoull(e, nullptr, 10) : 0ull; }();
        // MCLA_F9_AUTO_EVERY_MS=<ms> repeats it (max 40); X/Y may be comma lists, cycled per fire.
        static const uint64_t s_everyMs = [] { const char* e = std::getenv("MCLA_F9_AUTO_EVERY_MS"); return e && *e ? std::strtoull(e, nullptr, 10) : 0ull; }();
        static uint64_t s_nextMs = s_autoMs;
        static uint32_t s_fires = 0;
        if (s_nextMs && SDL_GetTicks() >= s_nextMs) {
            s_nextMs = (s_everyMs && s_fires < 39) ? s_nextMs + s_everyMs : 0;
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
            auto pick = [](const char* e, uint32_t k, double def) {
                if (!e) return def;
                int n = 1;
                for (const char* p = e; *p; ++p) n += (*p == ',');
                const char* p = e;
                for (uint32_t i = 0; i < k % n; ++i) p = std::strchr(p, ',') + 1;
                return std::atof(p);
            };
            if (pw > 0 && ph > 0)
                mcla::native::D3D12Backend::F9RequestCapture(
                    static_cast<float>(pick(std::getenv("MCLA_F9_AUTO_X"), s_fires, pw * 0.5)) / static_cast<float>(pw),
                    static_cast<float>(pick(std::getenv("MCLA_F9_AUTO_Y"), s_fires, ph * 0.5)) / static_cast<float>(ph));
            ++s_fires;
        }
    }

    // Sample the input snapshot only AFTER the drain: SDL_PollEvent is what
    // runs SDL_UpdateGamepads, so anything read before it is a frame behind.
    // The guest thread copies this snapshot under a lock and never calls SDL.
    XamInputPoll();
}

void App::OnResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    m_width = width;
    m_height = height;

    if (mcla::renderer::GetRendererMode() == mcla::renderer::RendererMode::Native) {
        mcla::native::RenderCommand cmd;
        cmd.type = mcla::native::RenderCommand::RESIZE;
        auto& resize = cmd.data.emplace<mcla::native::ResizeCommand>();
        resize.width = width;
        resize.height = height;
        mcla::native::g_commandQueue.push(cmd);
    }

    MCLA_LOG_INFO("Window resized to {}x{}", width, height);
}

// FunctionDispatcher implementation
//
// DESIGN NOTE: the recompiled guest dispatch table lives in
// Memory::InsertFunction / PPC_LOOKUP_FUNC (src/kernel/memory.h:170-176).
// The dispatcher map (m_functions) is written by SetFunction and AllocateThunk
// but read only by GetFunction – never at dispatch time, so SetFunction was
// dead code on its own.  This fix also writes the real dispatch table so that
// hooks installed via the dispatcher actually intercept guest calls.
// GetFunction falls back to the dispatch table so the original can be
// retrieved before the hook overwrites it.

App::FunctionDispatcher::PPCFunc App::FunctionDispatcher::GetFunction(uint32_t addr) const {
    // 1. Check the override map first (SetFunction registrations).
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_functions.find(addr);
        if (it != m_functions.end())
            return it->second;
    }
    // 2. Fall back to the real dispatch table so the original generated
    //    function can be retrieved before we overwrite it with a hook.
    return kernel::g_memory.FindFunction(addr);
}

void App::FunctionDispatcher::SetFunction(uint32_t addr, PPCFunc func) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_functions[addr] = func;
    // Also install in the real dispatch table so guest calls actually
    // reach the hook (or replacement) at runtime.
    //
    // F-952: only a VA inside the recompiled code range has a table row
    // (PPC_LOOKUP_FUNC has no bound check, src/ppc_context.h:124). Writing a
    // row for anything else lands at an unchecked offset inside the 4 GiB
    // guest window and corrupts unrelated guest RAM, so refuse loudly instead.
    if (addr < static_cast<uint32_t>(PPC_CODE_BASE) ||
        addr >= static_cast<uint32_t>(PPC_CODE_BASE + PPC_CODE_SIZE)) {
        static std::atomic<uint32_t> s_refused{0};
        const uint32_t n = s_refused.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n % 100) == 0)
            MCLA_LOG_ERROR("DISPATCH-SETFUNC-OOR #{} addr={:08X} is outside "
                           "[{:08X},{:08X}) - hook kept in the map only, table "
                           "NOT written",
                           n, addr, static_cast<uint32_t>(PPC_CODE_BASE),
                           static_cast<uint32_t>(PPC_CODE_BASE + PPC_CODE_SIZE));
        return;
    }
    kernel::g_memory.InsertFunction(addr, func);
}

uint32_t App::FunctionDispatcher::AllocateThunk(PPCFunc func, uint32_t originalAddr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // F-952: the thunk address must be dispatchable by the guest, so it comes
    // from the in-code-range allocator (kernel/memory.h) instead of the old
    // m_nextThunkAddr = 0x83000000 counter, which sat above the 4 GiB guest
    // image and had no function-table row: a guest `bctrl` to it indexed the
    // table out of bounds.
    const uint32_t thunkAddr =
        kernel::AllocateSyntheticGuestThunk(func, "dispatcher-thunk");
    if (thunkAddr == 0)
        return 0;
    m_thunks[thunkAddr] = {func, originalAddr};
    m_functions[thunkAddr] = func;
    return thunkAddr;
}

} // namespace mcla

