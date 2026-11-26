#include <stdafx.h>
#include "xam.h"
#include "xdm.h"
#include <cpu/guest_thread.h>
#include <cpu/ppc_context.h>
#include <ranges>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <tuple>
#include <vector>
#include <deque>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <xxhash.h>
#include <SDL3/SDL.h>
#include "logging.h"
#include "memory.h"
#include "user/config.h"
#include "common/cvar.h"

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS 0x00000000L
#endif

struct xxHash
{
    using is_avalanching = void;
    uint64_t operator()(XXH64_hash_t const& x) const noexcept { return x; }
};

template<typename T>
using xxHashMap = std::unordered_map<XXH64_hash_t, T, xxHash>;

struct XamListener;

std::unordered_set<XamListener*> gListeners;

struct XamListener : KernelObject
{
    uint32_t id{};
    uint64_t areas{};
    std::vector<std::tuple<uint32_t, uint32_t>> notifications;

    XamListener()
    {
        gListeners.insert(this);
    }

    ~XamListener()
    {
        gListeners.erase(this);
    }

    XamListener(const XamListener&) = delete;
    XamListener& operator=(const XamListener&) = delete;
};

std::array<xxHashMap<XHOSTCONTENT_DATA>, 3> gContentRegistry{};
xxHashMap<std::string> gRootMap;

std::string_view XamGetRootPath(const std::string_view& root)
{
    const auto result = gRootMap.find(StringHash(root));

    if (result == gRootMap.end())
        return "";

    return result->second;
}

void XamRootCreate(const std::string_view& root, const std::string_view& path)
{
    gRootMap.emplace(StringHash(root), path);
}

XCONTENT_DATA XamMakeContent(uint32_t type, const std::string_view& name)
{
    XCONTENT_DATA data{ 1, type };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    return data;
}

void XamRegisterContent(const XCONTENT_DATA& data, const std::string_view& root)
{
    const auto idx = data.dwContentType - 1;

    gContentRegistry[idx].emplace(StringHash(data.szFileName), XHOSTCONTENT_DATA{ data }).first->second.szRoot = root;
}

void XamRegisterContent(uint32_t type, const std::string_view name, const std::string_view& root)
{
    XCONTENT_DATA data{ 1, type, {}, "" };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    XamRegisterContent(data, root);
}

uint32_t XamNotifyCreateListener(uint64_t qwAreas)
{
    auto* listener = CreateKernelObject<XamListener>();

    listener->areas = qwAreas;

    return GetKernelHandle(listener);
}

void XamNotifyEnqueueEvent(uint32_t dwId, uint32_t dwParam)
{
    for (const auto& listener : gListeners)
    {
        if (((1 << MSG_AREA(dwId)) & listener->areas) == 0)
            continue;

        listener->notifications.emplace_back(dwId, dwParam);
    }
}

bool XNotifyGetNext(uint32_t hNotification, uint32_t dwMsgFilter, be<uint32_t>* pdwId, be<uint32_t>* pParam)
{
    auto& listener = *GetKernelObject<XamListener>(hNotification);

    if (dwMsgFilter)
    {
        for (size_t i = 0; i < listener.notifications.size(); i++)
        {
            if (std::get<0>(listener.notifications[i]) == dwMsgFilter)
            {
                if (pdwId)
                    *pdwId = std::get<0>(listener.notifications[i]);

                if (pParam)
                    *pParam = std::get<1>(listener.notifications[i]);

                listener.notifications.erase(listener.notifications.begin() + i);
                return true;
            }
        }
        return false;
    }

    if (listener.notifications.empty())
        return false;

    if (pdwId)
        *pdwId = std::get<0>(listener.notifications[0]);

    if (pParam)
        *pParam = std::get<1>(listener.notifications[0]);

    listener.notifications.erase(listener.notifications.begin());
    return true;
}

void McCompleteXamOverlapped(XXOVERLAPPED* ovl, uint32_t result, uint32_t length);

uint32_t XamShowMessageBoxUI(
    uint32_t dwUserIndex,
    be<uint16_t>* wszTitle,
    be<uint16_t>* wszText,
    uint32_t cButtons,
    xpointer<be<uint16_t>>* pwszButtons,
    uint32_t dwFocusButton,
    uint32_t dwFlags,
    be<uint32_t>* pResult,
    XXOVERLAPPED* pOverlapped)
{
    static std::atomic<uint32_t> s_census{0};
    const uint32_t n = s_census.fetch_add(1) + 1;
    if (n <= 20 || (n % 100) == 0) {
      MCLA_LOG_INFO("XAM-CENSUS[MsgBoxUI] #{:04} user={} buttons={} flags=0x{:X} ovl={} lr={:08X}", n,
                    dwUserIndex, cButtons, dwFlags, pOverlapped ? 1 : 0,
                    static_cast<uint32_t>(g_ppcContext ? g_ppcContext->lr : 0));
    }
    MCLA_LOG_INFO("XamShowMessageBoxUI: user={} buttons={} flags=0x{:X}", dwUserIndex, cButtons, dwFlags);
    (void)wszTitle;
    (void)wszText;
    (void)pwszButtons;
    (void)dwFocusButton;

    XamNotifyEnqueueEvent(9, 1);
    if (pResult)
        *pResult = 0;
    XamNotifyEnqueueEvent(9, 0);

    if (pOverlapped) {
        McCompleteXamOverlapped(pOverlapped, 0, 0);
        return 0x000003E5u; // X_ERROR_IO_PENDING
    }

    return 0;
}

// ===========================================================================
// F-949 (2026-11-10) SAVE CONTENT, modelled on Xenia's content manager.
//
// Host layout: <exe dir>/content/<content type %08X>/<XCONTENT_DATA file name>/
// -- Xenia's content_root/title_id/content_type/file_name
// (.research/xenia/src/xenia/kernel/xam/content_manager.cc:60-79) without the
// title-id level (this host runs one title). XamContentCreateEx mounts the
// guest's root name (`save:`) on that folder; NtCreateFile/NtReadFile/
// NtWriteFile in imports.cpp then serve `save:\...` from real host files.
// ===========================================================================
std::filesystem::path McContentStoreDir();
bool McContentMount(const std::string& root, const std::filesystem::path& dir);
bool McContentUnmount(const std::string& root);

static_assert(sizeof(XCONTENT_DATA) == 0x134); // content_manager.h:93

namespace
{
// .research/xenia/src/xenia/xbox.h:87-105, 118-119.
constexpr uint32_t kXErrorFileNotFound = 0x00000002;
constexpr uint32_t kXErrorPathNotFound = 0x00000003;
constexpr uint32_t kXErrorAccessDenied = 0x00000005;
constexpr uint32_t kXErrorNoMoreFiles = 0x00000012;
constexpr uint32_t kXErrorInvalidParameter = 0x00000057;
constexpr uint32_t kXErrorInsufficientBuffer = 0x0000007A;
constexpr uint32_t kXErrorAlreadyExists = 0x000000B7;
constexpr uint32_t kXErrorIoPending = 0x000003E5;
constexpr uint32_t kXErrorContentDeviceNotConnected = 0x0000048F;
constexpr uint32_t kXEInvalidArg = 0x80070057;
constexpr uint32_t kXEOutOfMemory = 0x8007000E;

// One safe host path component from guest bytes (at most `max`, NUL-stopped),
// or "" when it is empty, a dot name, or carries a separator/wildcard/control.
std::string SafeGuestName(const char* s, size_t max)
{
    std::string name;
    for (size_t i = 0; s && i < max && s[i]; ++i)
        name += s[i];
    if (name.empty() || name == "." || name == ".." ||
        name.find_first_of("\\/:*?\"<>|") != std::string::npos)
        return {};
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
            return {};
    return name;
}

// Saves go to <Documents>\MCLA Saves\<type> (persistent across runs). If that
// folder cannot be created (e.g. a file squats on the path), fall back to the
// exe's content folder so option "create save" never fails on a path error.
// shortcut: one profile folder (no per-user split), upgrade when profiles exist.
std::filesystem::path ContentSaveRoot()
{
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
        const std::filesystem::path root = std::filesystem::path(docs) / "MCLA Saves";
        CoTaskMemFree(docs);
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (!ec && std::filesystem::is_directory(root, ec))
            return root;
        MCLA_LOG_WARN("SAVE-ROOT fallback: '{}' unusable ({}), using build content folder",
                      root.string(), ec.message());
    }
    return McContentStoreDir() / "saves";
}

std::filesystem::path ContentTypeDir(uint32_t contentType)
{
    const std::string sub = fmt::format("{:08X}", contentType);
    const std::filesystem::path dir = ContentSaveRoot() / sub;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// ===========================================================================
// Atomic saved-game backup & restore (ported from RenanQueiroz rexglue-saved-game-backup.patch):
//   Package path: <save_root>/<type_hex>/<name>
//   Backup path:  <save_root>/Backups/<type_hex>/<name>
//   Marker file:  <backup_path>.inprogress
// ===========================================================================
constexpr const char* kBackupDirName = "Backups";
constexpr const char* kBackupMarkerSuffix = ".inprogress";

std::string LowerAsciiKey(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool IsHexDirName(const std::filesystem::path& name, size_t digits)
{
    const auto text = name.string();
    if (text.size() != digits)
        return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    });
}

std::filesystem::path BackupPathFor(const std::filesystem::path& packagePath)
{
    const auto typeDir = packagePath.parent_path();
    if (packagePath.filename().empty() || typeDir.filename().empty())
        return {};
    return typeDir.parent_path() / kBackupDirName / typeDir.filename() / packagePath.filename();
}

std::filesystem::path BackupMarkerFor(const std::filesystem::path& backupPath)
{
    std::filesystem::path marker = backupPath;
    marker += kBackupMarkerSuffix;
    return marker;
}

bool CreateEmptyMarkerFile(const std::filesystem::path& markerPath)
{
    HANDLE h = CreateFileW(markerPath.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}

void RemoveBackupForPackage(const std::filesystem::path& packagePath)
{
    const auto backupPath = BackupPathFor(packagePath);
    if (backupPath.empty())
        return;
    std::error_code markerEc;
    std::filesystem::remove(BackupMarkerFor(backupPath), markerEc);
    std::error_code ec;
    std::filesystem::remove_all(backupPath, ec);
    if (markerEc || ec) {
        MCLA_LOG_WARN("SAVE-BACKUP: could not remove backup for '{}': {}",
                      packagePath.filename().string(),
                      (markerEc ? markerEc : ec).message());
    }
}

std::mutex g_keptBackupsMtx;
std::unordered_map<std::string, std::filesystem::path> g_keptBackups;
std::once_flag g_restoreInterruptedOnce;

void RestoreInterruptedSavesFromRoot(const std::filesystem::path& saveRoot)
{
    const std::filesystem::path backupsRoot = saveRoot / kBackupDirName;
    std::error_code ec;
    if (!std::filesystem::is_directory(backupsRoot, ec))
        return;
    for (std::filesystem::directory_iterator typeIt(backupsRoot, ec), end;
         !ec && typeIt != end; typeIt.increment(ec)) {
        std::error_code entryEc;
        if (!typeIt->is_directory(entryEc) || !IsHexDirName(typeIt->path().filename(), 8))
            continue;
        const std::filesystem::path typeDir = typeIt->path();
        for (std::filesystem::directory_iterator pkgIt(typeDir, entryEc), end2;
             !entryEc && pkgIt != end2; pkgIt.increment(entryEc)) {
            std::error_code pkgEc;
            if (!pkgIt->is_directory(pkgEc))
                continue;
            const std::filesystem::path backupPath = pkgIt->path();
            const std::filesystem::path marker = BackupMarkerFor(backupPath);
            const std::filesystem::path packagePath =
                saveRoot / typeDir.filename() / backupPath.filename();
            const std::string name = backupPath.filename().string();
            std::error_code opEc;
            if (!std::filesystem::exists(marker, opEc)) {
                std::filesystem::remove_all(backupPath, opEc);
                continue;
            }
            std::filesystem::remove_all(packagePath, opEc);
            if (!opEc)
                std::filesystem::create_directories(packagePath.parent_path(), opEc);
            if (!opEc)
                std::filesystem::rename(backupPath, packagePath, opEc);
            if (opEc) {
                MCLA_LOG_ERROR("SAVE-BACKUP: could not restore interrupted save '{}' from '{}': {}",
                               name, backupPath.string(), opEc.message());
                continue;
            }
            std::filesystem::remove(marker, opEc);
            MCLA_LOG_WARN("SAVE-BACKUP: saved game '{}' was interrupted during rewrite; restored previous contents",
                          name);
        }
    }
}

void EnsureInterruptedSavesRestored()
{
    std::call_once(g_restoreInterruptedOnce, []() {
        RestoreInterruptedSavesFromRoot(ContentSaveRoot());
        const std::filesystem::path fallbackRoot = McContentStoreDir() / "saves";
        if (fallbackRoot != ContentSaveRoot())
            RestoreInterruptedSavesFromRoot(fallbackRoot);
    });
}

void KeepContentBackup(const std::string& rootName, uint32_t contentType,
                       const std::filesystem::path& packagePath)
{
    static const bool s_noBackup = []() {
        const char* e = std::getenv("MCLA_NO_SAVE_BACKUP");
        return e && e[0] == '1';
    }();
    if (s_noBackup || contentType != XCONTENTTYPE_SAVEDATA)
        return;

    std::error_code ec;
    const auto backupPath = BackupPathFor(packagePath);
    if (backupPath.empty() || !std::filesystem::exists(packagePath, ec))
        return;

    const auto marker = BackupMarkerFor(backupPath);
    const std::string name = packagePath.filename().string();

    RemoveBackupForPackage(packagePath);
    std::filesystem::create_directories(backupPath.parent_path(), ec);
    if (!ec && !CreateEmptyMarkerFile(marker))
        ec = std::make_error_code(std::errc::io_error);
    if (ec) {
        MCLA_LOG_WARN("SAVE-BACKUP: no backup kept for '{}' before rewrite: {}", name, ec.message());
        return;
    }

    std::filesystem::rename(packagePath, backupPath, ec);
    if (ec) {
        // Rename failed (e.g., AV/sync lock); fall back to recursive copy with marker written last.
        std::error_code cleanEc;
        std::filesystem::remove(marker, cleanEc);
        std::filesystem::remove_all(backupPath, cleanEc);
        ec.clear();
        std::filesystem::copy(packagePath, backupPath,
                              std::filesystem::copy_options::recursive, ec);
        if (!ec && !CreateEmptyMarkerFile(marker))
            ec = std::make_error_code(std::errc::io_error);
        if (ec) {
            MCLA_LOG_WARN("SAVE-BACKUP: copy fallback failed for '{}': {}", name, ec.message());
            RemoveBackupForPackage(packagePath);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_keptBackupsMtx);
        g_keptBackups[LowerAsciiKey(rootName)] = packagePath;
    }
    MCLA_LOG_INFO("SAVE-BACKUP: saved game '{}' rewriting; previous contents kept until '{}' closes",
                  name, rootName);
}

void DropKeptBackup(const std::string& rootName)
{
    std::filesystem::path packagePath;
    {
        std::lock_guard<std::mutex> lk(g_keptBackupsMtx);
        auto it = g_keptBackups.find(LowerAsciiKey(rootName));
        if (it == g_keptBackups.end())
            return;
        packagePath = std::move(it->second);
        g_keptBackups.erase(it);
    }
    RemoveBackupForPackage(packagePath);
    MCLA_LOG_INFO("SAVE-BACKUP: '{}' closed; previous contents of '{}' released",
                  rootName, packagePath.filename().string());
}

bool SaveMarkerDue(std::atomic<uint32_t>& c, uint32_t* n)
{
    *n = c.fetch_add(1, std::memory_order_relaxed) + 1;
    return *n <= 32 || (*n % 500) == 0;
}

// Xenia XStaticEnumerator<XCONTENT_DATA> (xenumerator.cc:60-79).
struct XContentEnumerator final : KernelObject
{
    std::vector<XCONTENT_DATA> items;
    size_t cursor = 0;
    uint32_t perCall = 0;

    XContentEnumerator(std::vector<XCONTENT_DATA>&& list, uint32_t itemsPerCall)
        : items(std::move(list)), perCall(itemsPerCall)
    {
    }

    uint32_t Wait(uint32_t timeout) override
    {
        (void)timeout;
        return STATUS_SUCCESS;
    }
};
} // namespace

// Xenia xeXamContentCreate (.research/xenia/src/xenia/kernel/xam/xam_content.cc:
// 120-221) with XamContentCreateEx's argument order (:223-231): r6 = flags,
// r7 = disposition out, r8 = license mask out, r9 = cache size, r10 = content
// size, stack = overlapped. (The parameter names here used to be dwTitleId /
// pdwContentId / ...; the registers were always these.)
uint32_t XamContentCreateEx(
    uint32_t dwUserIndex,
    const char* szRootName,
    const XCONTENT_DATA* pContentData,
    uint32_t dwFlags,
    be<uint32_t>* pdwDisposition,
    be<uint32_t>* pdwLicenseMask,
    uint32_t dwCacheSize,
    uint64_t qwContentSize,
    XXOVERLAPPED* pOverlapped)
{
    if (g_ppcContext) {
        const auto& c = *g_ppcContext;
        MCLA_LOG_ERROR("TEMP-XCONTENT {} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} r9={:08X} r10={:08X} lr={:08X}",
                       "CreateEx", c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r7.u32, c.r8.u32, c.r9.u32, c.r10.u32,
                       static_cast<uint32_t>(c.lr));
    }
    (void)dwUserIndex;
    (void)dwCacheSize;
    (void)qwContentSize;

    const std::string root = SafeGuestName(szRootName, 32);
    const std::string name =
        pContentData ? SafeGuestName(pContentData->szFileName, XCONTENT_MAX_FILENAME) : std::string();
    const uint32_t type = pContentData ? pContentData->dwContentType.get() : 0;
    std::filesystem::path dir;
    uint32_t result = kXErrorInvalidParameter;
    uint32_t disposition = 0; // kDispositionState (:118): 1 = Create, 2 = Open

    if (!root.empty() && !name.empty()) {
        EnsureInterruptedSavesRestored();
        dir = ContentTypeDir(type) / name;
        // Claim the root first, so an already-open root (ALREADY_EXISTS,
        // content_manager.cc:133-136 / :160-163) never deletes anything below.
        if (!McContentMount(root, dir)) {
            result = kXErrorAlreadyExists;
        } else {
            std::error_code ec;
            const bool exists = std::filesystem::exists(dir, ec);
            bool create = false;
            switch (dwFlags & 0xF) {
            case 1: // CREATE_NEW (:150-157)
                if (exists) result = kXErrorAlreadyExists;
                else create = true;
                break;
            case 2: // CREATE_ALWAYS (:158-164)
                if (exists) {
                    KeepContentBackup(root, type, dir);
                    std::filesystem::remove_all(dir, ec);
                }
                create = true;
                break;
            case 3: // OPEN_EXISTING (:165-172)
                if (!exists) result = kXErrorPathNotFound;
                else disposition = 2;
                break;
            case 4: // OPEN_ALWAYS (:173-180)
                if (exists) disposition = 2;
                else create = true;
                break;
            case 5: // TRUNCATE_EXISTING (:181-189)
                if (!exists) result = kXErrorPathNotFound;
                else {
                    KeepContentBackup(root, type, dir);
                    std::filesystem::remove_all(dir, ec);
                    create = true;
                }
                break;
            default: // :190-192
                break;
            }
            if (create) {
                // CreateContent (content_manager.cc:138-146)
                std::filesystem::create_directories(dir, ec);
                if (std::filesystem::is_directory(dir, ec)) disposition = 1;
                else result = kXErrorAccessDenied;
            }
            if (disposition != 0) result = 0;
            if (result != 0) McContentUnmount(root);
        }
    }

    if (pdwDisposition) *pdwDisposition = disposition; // :201-203
    if (pdwLicenseMask && result == 0) *pdwLicenseMask = 0; // :205-207

    static std::atomic<uint32_t> s_mount{0};
    uint32_t n = 0;
    if (SaveMarkerDue(s_mount, &n))
        MCLA_LOG_WARN("SAVE-MOUNT #{} root='{}' type={:08X} name='{}' flags=0x{:X} -> "
                      "result=0x{:X} disposition={} host='{}' ovl={} lr={:08X}",
                      n, root, type, name, dwFlags, result, disposition, dir.string(),
                      pOverlapped ? 1 : 0,
                      static_cast<uint32_t>(g_ppcContext ? g_ppcContext->lr : 0));

    if (pOverlapped) {
        McCompleteXamOverlapped(pOverlapped, result, disposition); // :209-219
        return kXErrorIoPending;
    }
    return result;
}

// Xenia XamContentClose_entry (xam_content.cc:279-291) -> CloseContent
// (content_manager.cc:180-194). Files still open under the root keep their
// host handles until the guest closes them (Xenia closes them, :185).
uint32_t XamContentClose(const char* szRootName, XXOVERLAPPED* pOverlapped)
{
    if (g_ppcContext) {
        const auto& c = *g_ppcContext;
        MCLA_LOG_ERROR("TEMP-XCONTENT {} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} r9={:08X} r10={:08X} lr={:08X}",
                       "Close", c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r7.u32, c.r8.u32, c.r9.u32, c.r10.u32,
                       static_cast<uint32_t>(c.lr));
    }
    const std::string root = SafeGuestName(szRootName, 32);
    const uint32_t result =
        (!root.empty() && McContentUnmount(root)) ? 0 : kXErrorFileNotFound;
    if (result == 0) {
        DropKeptBackup(root);
    }

    static std::atomic<uint32_t> s_unmount{0};
    uint32_t n = 0;
    if (SaveMarkerDue(s_unmount, &n))
        MCLA_LOG_WARN("SAVE-UNMOUNT #{} root='{}' -> result=0x{:X} ovl={} lr={:08X}",
                      n, root, result, pOverlapped ? 1 : 0,
                      static_cast<uint32_t>(g_ppcContext ? g_ppcContext->lr : 0));

    if (pOverlapped) {
        McCompleteXamOverlapped(pOverlapped, result, 0);
        return kXErrorIoPending;
    }
    return result;
}

// Xenia XamContentGetDeviceData_entry and its dummy device table
// (.research/xenia/src/xenia/kernel/xam/xam_content_device.cc:20-31, 108-124):
// HDD id 1 / type 1, 20 GB total, 3 GB free; ODD id 2 / type 4, 7 GB, 0 free.
// Consistent with XamShowDeviceSelectorUI (deviceId=1) and
// XamContentGetDeviceState (ids 1 and 2 connected), both in imports.cpp.
uint32_t XamContentGetDeviceData(uint32_t DeviceID, XDEVICE_DATA* pDeviceData)
{
    constexpr uint64_t kGB = 1024ull * 1024ull * 1024ull;
    const bool hdd = DeviceID == 1, odd = DeviceID == 2;
    uint32_t result = 0;
    if (!hdd && !odd) {
        result = kXErrorContentDeviceNotConnected;
    } else if (!pDeviceData) {
        result = kXErrorInvalidParameter;
    } else {
        std::memset(pDeviceData, 0, sizeof(XDEVICE_DATA)); // device_data.Zero()
        pDeviceData->DeviceID = DeviceID;
        pDeviceData->DeviceType = hdd ? 1u : 4u;
        pDeviceData->ulDeviceBytes = hdd ? 20 * kGB : 7 * kGB;
        pDeviceData->ulDeviceFreeBytes = hdd ? 3 * kGB : 0;
        const char* label = hdd ? "Dummy HDD" : "Dummy ODD";
        for (size_t i = 0; label[i] && i + 1 < XCONTENTDEVICE_MAX_NAME; ++i)
            pDeviceData->wszName[i] = static_cast<uint16_t>(label[i]);
    }

    static std::atomic<uint32_t> s_cdd{0};
    uint32_t n = 0;
    if (SaveMarkerDue(s_cdd, &n))
        MCLA_LOG_WARN("SAVE-DEVICE #{} DeviceID={:08X} -> result=0x{:X} lr={:08X}",
                      n, DeviceID, result,
                      static_cast<uint32_t>(g_ppcContext ? g_ppcContext->lr : 0));
    return result;
}

// Xenia XamContentCreateEnumerator_entry (xam_content.cc:69-116), listing the
// HDD content folders as ContentManager::ListContent does
// (content_manager.cc:81-108: one item per sub-directory).
uint32_t XamContentCreateEnumerator(
    uint32_t dwUserIndex,
    uint32_t DeviceID,
    uint32_t dwContentType,
    uint32_t dwContentFlags,
    uint32_t cItem,
    be<uint32_t>* pcbBuffer,
    be<uint32_t>* phEnum)
{
    uint32_t result = 0;
    uint32_t handle = 0;
    size_t found = 0;
    if ((DeviceID != 0 && DeviceID != 1 && DeviceID != 2) || !phEnum) {
        if (pcbBuffer) *pcbBuffer = 0; // :75-82
        result = kXEInvalidArg;
    } else {
        if (pcbBuffer) *pcbBuffer = static_cast<uint32_t>(sizeof(XCONTENT_DATA) * cItem);
        std::vector<XCONTENT_DATA> items;
        if (DeviceID != 2) { // HDD or any device (:97-104); the ODD lists nothing (:106-108)
            EnsureInterruptedSavesRestored();
            std::error_code ec;
            for (std::filesystem::directory_iterator it(ContentTypeDir(dwContentType), ec), end;
                 !ec && it != end; it.increment(ec)) {
                const std::string fname = it->path().filename().string();
                if (!it->is_directory(ec) ||
                    SafeGuestName(fname.c_str(), XCONTENT_MAX_FILENAME) != fname)
                    continue;
                XCONTENT_DATA d{};
                d.DeviceID = 1u;
                d.dwContentType = dwContentType;
                for (size_t i = 0; i < fname.size() && i + 1 < XCONTENT_MAX_DISPLAYNAME; ++i)
                    d.szDisplayName[i] = static_cast<uint16_t>(fname[i]);
                std::memcpy(d.szFileName, fname.data(), fname.size());
                items.push_back(d);
            }
        }
        found = items.size();
        handle = CreateRegisteredKernelObject<XContentEnumerator>(0, std::move(items), cItem);
        if (handle == GUEST_INVALID_HANDLE_VALUE) {
            handle = 0;
            result = kXEOutOfMemory;
        } else {
            *phEnum = handle;
        }
    }

    static std::atomic<uint32_t> s_cce{0};
    uint32_t n = 0;
    if (SaveMarkerDue(s_cce, &n))
        MCLA_LOG_WARN("SAVE-ENUM #{} create user={} dev={:08X} type={:08X} flags={:08X} "
                      "cItem={} -> result=0x{:X} items={} hEnum={:08X}",
                      n, dwUserIndex, DeviceID, dwContentType, dwContentFlags, cItem,
                      result, found, handle);
    return result;
}

bool McHasUnclosedSaveBackups()
{
    std::lock_guard<std::mutex> lk(g_keptBackupsMtx);
    return !g_keptBackups.empty();
}

// Xenia xeXamEnumerate / XamEnumerate_entry (xam_enum.cc:28-83) over
// XStaticUntypedEnumerator::WriteItems (xenumerator.cc:60-79). Unknown handles
// keep the X_ERROR_INVALID_HANDLE answer below.
uint32_t XamEnumerate(
    uint32_t hEnum,
    uint32_t dwFlags,
    void* pvBuffer,
    uint32_t cbBuffer,
    be<uint32_t>* pcItemsReturned,
    XXOVERLAPPED* pOverlapped)
{
    static std::atomic<uint32_t> s_cen{0};
    const uint32_t n = s_cen.fetch_add(1) + 1;

    XContentEnumerator* e = nullptr;
    if (hEnum != 0 && hEnum != GUEST_INVALID_HANDLE_VALUE)
        e = dynamic_cast<XContentEnumerator*>(LookupIdentityKernelObject(hEnum));
    if (e) {
        auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
        const uint32_t addr = pvBuffer ? mem.MapVirtual(pvBuffer) : 0;
        uint32_t result = kXErrorInvalidParameter;
        uint32_t count = 0;
        if (addr != 0) {
            if (e->cursor >= e->items.size() || e->perCall == 0) {
                result = kXErrorNoMoreFiles; // xenumerator.cc:63-66
            } else {
                // Also clamped to the caller's buffer (Xenia trusts it).
                count = static_cast<uint32_t>(std::min({e->items.size() - e->cursor,
                                                        size_t(e->perCall),
                                                        size_t(cbBuffer / sizeof(XCONTENT_DATA))}));
                if (count == 0) {
                    result = kXErrorInsufficientBuffer;
                } else if (!mem.WriteBytes(addr, &e->items[e->cursor],
                                           static_cast<uint32_t>(count * sizeof(XCONTENT_DATA)))) {
                    count = 0; // buffer outside the guest window: INVALID_PARAMETER
                } else {
                    e->cursor += count;
                    result = 0;
                }
            }
        }
        if (n <= 32 || (n % 500) == 0)
            MCLA_LOG_WARN("SAVE-ENUM #{} next hEnum={:08X} cb={} -> result=0x{:X} items={} "
                          "ovl={} lr={:08X}",
                          n, hEnum, cbBuffer, result, count, pOverlapped ? 1 : 0,
                          static_cast<uint32_t>(g_ppcContext ? g_ppcContext->lr : 0));
        if (pOverlapped) {
            McCompleteXamOverlapped(pOverlapped, result, count);
            return kXErrorIoPending;
        }
        if (pcItemsReturned) *pcItemsReturned = result == 0 ? count : 0;
        return result;
    }

    // Not a content enumerator. Xenia (xam_enum.cc:35-38) answers an unknown
    // handle with X_ERROR_INVALID_HANDLE synchronously; the old "return 0"
    // claimed success and never completed pOverlapped, so an overlapped caller
    // waited forever.
    if (n <= 16 || (n % 500) == 0)
        MCLA_LOG_ERROR("XamEnumerate[{}] hEnum={:08X} flags={:08X} cb={} ovl={} lr={:08X} "
                       "-> X_ERROR_INVALID_HANDLE (not a content enumerator)",
                       n, hEnum, dwFlags, cbBuffer, pOverlapped ? 1 : 0,
                       static_cast<uint32_t>(g_ppcContext ? g_ppcContext->lr : 0));
    return 6; // X_ERROR_INVALID_HANDLE
}

// ===========================================================================
// Xam input - real SDL3 gamepads, one per guest controller port, hot-plugged.
//
// F-609. The pre-fix body returned 0 for every port and filled the overlay
// from SDL_GetKeyboardState on the guest thread, so the title believed it
// held a wired 360 pad on all four ports at all times, forever, and never saw
// X / Y / BACK / either thumb button (those bits exist at src/kernel/xbox.h:
// 369-377 and were never written).
//
// THREADING MODEL - the entire design hangs off this. Do not "simplify" it:
//   * MAIN thread (App::HandleEvents) owns every SDL_Gamepad*. It drains the
//     SDL event queue, opens/closes pads on SDL_EVENT_GAMEPAD_ADDED/REMOVED,
//     and samples each pad into a POD snapshot. SDL_PollEvent is what drives
//     SDL_UpdateGamepads, so "sample immediately after the drain" is the only
//     point at which SDL's gamepad cache is guaranteed fresh.
//   * GUEST thread (XamInputGetState / GetCapabilities / SetState, and any
//     guest worker thread that polls input) makes NO SDL call at all: it
//     copies the snapshot under g_portLock and posts rumble requests through
//     atomics. The pre-fix read path called SDL_GetKeyboardState with no pump
//     of any kind (pre-fix src/kernel/xam.cpp:278), so it read whatever the
//     main loop last happened to leave in SDL's keyboard cache; there is no
//     SDL_PumpEvents equivalent that is legal off the main thread, which is
//     why the snapshot exists instead of a pump on the read path.
//     Rumble is therefore applied by the main thread up to one main-loop tick
//     (~1 ms, src/app.cpp:279) after the guest asks - far below the tens of
//     ms a real 360 pad takes to spin its motors up.
// ===========================================================================

namespace {

// Xbox 360 exposes four controller ports (XINPUT_USER_INDEX_MAX == 4).
constexpr uint32_t kNumPorts = 4;

// The keyboard fallback is bound to port 0 and only to port 0: the guest
// queries player 0 at boot, so any other slot would simply never be read.
constexpr uint32_t kKeyboardFallbackPort = 0;

// XInput's documented radial thumbstick dead zone, 7849 of 32767 - the figure
// the Xbox 360 control panel reports. A retail pad gates this in firmware, so
// skipping it would hand the guest the sub-1% drift a real pad never produces.
// SDL states the same thing from the other side: sticks sit "within ~8000 of
// zero" (third_party/SDL3/include/SDL3/SDL_gamepad.h:213).
constexpr int kStickDeadzone = 7849;

// Wave 3 (§2.2): per-stick dead zones and rumble are runtime-settable. The
// defaults keep the previous behaviour: 24.0% is the documented XInput figure
// (spec §2.2) and vibration stays on. No keyboard path reads these CVars.
MCLA_CVAR_FLOAT(mcla_left_stick_deadzone, 24.0f, "Controls", "Left stick dead zone (percent of full travel, 0..30)");
MCLA_CVAR_FLOAT(mcla_right_stick_deadzone, 24.0f, "Controls", "Right stick dead zone (percent of full travel, 0..30)");
MCLA_CVAR_BOOL(mcla_vibration, true, "Controls", "Controller vibration (rumble)");

// X_ERROR_DEVICE_NOT_CONNECTED == 0x48F
// (.research/xenia/src/xenia/xbox.h:100).
constexpr uint32_t kXErrorDeviceNotConnected = 0x0000048F;

// Xenia's kInput flag decoding (.research/xenia/src/xenia/kernel/xam/
// xam_input.cc:28-29 for the constants, :93-102 for the use).
constexpr uint32_t kXInputFlagGamepad = 0x01;
constexpr uint32_t kXInputFlagAnyUser = 1u << 30;

// Xbox 360 controller rings: player 1 green, 2 red, 3 blue, 4 yellow.
constexpr uint8_t kPortLed[kNumPorts][3] = {
    { 0, 255, 0 }, { 255, 0, 0 }, { 0, 0, 255 }, { 255, 255, 0 }
};

// Guest is big-endian PPC and the overlay handed to these functions is a
// *host* view of guest RAM (Memory::Translate, src/kernel/function.h:335),
// so every multi-byte field is stored swapped. That discipline is unchanged.
constexpr uint16_t Bswap16(uint16_t v)
{
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}
constexpr int16_t Bswap16s(int16_t v)
{
    return static_cast<int16_t>(Bswap16(static_cast<uint16_t>(v)));
}
constexpr uint32_t Bswap32(uint32_t v)
{
    return __builtin_bswap32(v);
}

enum class PadSource : uint8_t
{
    None     = 0,   // nothing on this port -> X_ERROR_DEVICE_NOT_CONNECTED
    Gamepad  = 1,   // a real SDL_Gamepad* is open on this port
    Keyboard = 2,   // LABELLED fallback, see SampleKeyboard()
};

struct Sample
{
    PadSource   source{PadSource::None};
    uint32_t    instance{0};       // SDL_JoystickID; 0 for the keyboard fallback
    uint16_t    buttons{0};        // host-order XAMINPUT_GAMEPAD_* bits
    uint8_t     ltrigger{0};
    uint8_t     rtrigger{0};
    int16_t     lx{0}, ly{0}, rx{0}, ry{0};
    bool        rumble{false};
    bool        led{false};
    const char* name{nullptr};
    const char* guid{nullptr};
};

// Main-thread-owned SDL handle. Never dereferenced off the main thread - the
// guest side only ever sees the PadSource/rumble/led facts copied into the
// snapshot below.
struct Pad
{
    SDL_Gamepad*   pad{nullptr};
    SDL_JoystickID id{0};
    bool           rumbleOk{false};
    bool           ledOk{false};
    char           name[64]{};
    char           guid[33]{};
};

// What the guest thread is allowed to see: plain host-order POD.
struct PortSnapshot
{
    PadSource source{PadSource::None};
    uint32_t  instance{0};
    uint32_t  generation{0};   // bumped whenever source/instance change
    uint32_t  packet{0};       // host order; strictly monotonic per port
    bool      seeded{false};   // packet has been published at least once
    bool      rumble{false};
    bool      led{false};
    uint16_t  buttons{0};
    uint8_t   ltrigger{0};
    uint8_t   rtrigger{0};
    int16_t   lx{0}, ly{0}, rx{0}, ry{0};
    char      name[64]{};
    char      guid[33]{};
};

std::mutex          g_portLock;
PortSnapshot        g_ports[kNumPorts];   // guarded by g_portLock
Pad                 g_pads[kNumPorts];    // MAIN THREAD ONLY
std::atomic<uint32_t> g_rumbleReq[kNumPorts];      // lo | hi<<16, guest -> main
std::atomic<bool>     g_rumblePending[kNumPorts];

// Wave 3 (§2.4): wall-clock stamp of the last REAL gamepad activity, written on
// the main thread by SampleGamepad and read by App::MainLoop for cursor
// auto-hide. SampleKeyboard never touches it, so a keyboard-only session cannot
// hide the cursor. SDL_GetTicks() is a SDL3 millisecond clock.
std::atomic<uint64_t> g_lastGamepadActivityMs{0};

// Host keystroke queue (b-3). Main thread enqueues menu-key edges; the guest
// thread dequeues one per XamInputGetKeystrokeEx. Driving keys are deliberately
// absent: the guest's own keyboard table treats W/D as steering, while the
// fallback pad already maps WASD to throttle/brake/steer. Sending both paths
// made W accelerate and turn right at the same time.
std::mutex                          g_ksLock;
std::deque<XAMINPUT_KEYSTROKE_HOST> g_ksQueue;
constexpr size_t                    kKsQueueCap = 256;

struct KeyMapping {
    SDL_Scancode scancode;
    uint16_t     vk;
};
static const KeyMapping kKeyMappings[] = {
    { SDL_SCANCODE_SPACE,  0x0020 },
    { SDL_SCANCODE_RETURN, 0x000D },
    { SDL_SCANCODE_ESCAPE, 0x001B },
    { SDL_SCANCODE_UP,     0x0026 },
    { SDL_SCANCODE_DOWN,   0x0028 },
    { SDL_SCANCODE_LEFT,   0x0025 },
    { SDL_SCANCODE_RIGHT,  0x0027 },
};
static bool s_prevKeyState[sizeof(kKeyMappings)/sizeof(kKeyMappings[0])] = {};

// True when `p` and the `size` bytes after it are both inside the 4 GiB guest
// window. Every out-parameter arriving through GUEST_FUNCTION_HOOK is a
// translated host pointer, so it has to be bounds checked before it is
// written; the pre-fix GetState wrote 12 bytes wherever it was handed.
bool GuestRangeOk(const void* p, size_t size)
{
    const mcla::kernel::Memory& m = mcla::kernel::g_memory;
    if (!m.IsInMemoryRange(p))
        return false;

    const uintptr_t begin = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = begin + size;
    return end >= begin && end <= reinterpret_cast<uintptr_t>(m.base) + m.size;
}

// Xbox 360 bits (src/kernel/xbox.h:364-377) keyed by SDL's positional button
// enum. SDL is positional rather than lettered, so WEST->X and NORTH->Y is
// the Xbox layout, which is also the physical layout of the pad this guest was
// authored against. BACK is View/Back, not a menu escape. SDL_GAMEPAD_BUTTON_
// GUIDE is dropped on purpose: a wired 360 pad has no Guide button and the
// kInput button word has no bit for one.
struct ButtonMap { SDL_GamepadButton sdl; uint16_t guest; };
constexpr ButtonMap kButtonMap[] = {
    { SDL_GAMEPAD_BUTTON_DPAD_UP,        XAMINPUT_GAMEPAD_DPAD_UP        },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN,      XAMINPUT_GAMEPAD_DPAD_DOWN      },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT,      XAMINPUT_GAMEPAD_DPAD_LEFT      },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     XAMINPUT_GAMEPAD_DPAD_RIGHT     },
    { SDL_GAMEPAD_BUTTON_START,          XAMINPUT_GAMEPAD_START          },
    { SDL_GAMEPAD_BUTTON_BACK,           XAMINPUT_GAMEPAD_BACK           },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK,     XAMINPUT_GAMEPAD_LEFT_THUMB     },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK,    XAMINPUT_GAMEPAD_RIGHT_THUMB    },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  XAMINPUT_GAMEPAD_LEFT_SHOULDER  },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
    { SDL_GAMEPAD_BUTTON_SOUTH,          XAMINPUT_GAMEPAD_A              },
    { SDL_GAMEPAD_BUTTON_EAST,           XAMINPUT_GAMEPAD_B              },
    { SDL_GAMEPAD_BUTTON_WEST,           XAMINPUT_GAMEPAD_X              },
    { SDL_GAMEPAD_BUTTON_NORTH,          XAMINPUT_GAMEPAD_Y              },
};

// Wave 3 (§2.2): the radial threshold is a parameter. Callers pass the
// per-stick value derived from their dead-zone CVar (clamped to 0..32000, so
// the (32767 - threshold) divisor below is always positive). The old
// hardcoded call site passed kStickDeadzone; its arithmetic is unchanged.
void ApplyDeadzone(int& x, int& y, int threshold)
{
    const int64_t mag2 = static_cast<int64_t>(x) * x + static_cast<int64_t>(y) * y;
    const int mag = static_cast<int>(std::sqrt(static_cast<double>(mag2)));
    if (mag <= threshold)
    {
        x = 0;
        y = 0;
        return;
    }

    // Rescale the MAGNITUDE (not the components by it): x*scale multiplied by mag twice, so a half stick
    // gave ~5611 instead of ~11223 (docs/AUDIT_AUDIO_INPUT.md P-IN1).
    const double m = std::min(static_cast<double>(mag), 32767.0);
    const double k = (m - threshold) * 32767.0 / (static_cast<double>(mag) * (32767 - threshold));
    x = std::clamp(static_cast<int>(x * k), -32768, 32767);
    y = std::clamp(static_cast<int>(y * k), -32768, 32767);
}

// SDL reports gamepad triggers as 0 (released) .. SDL_JOYSTICK_AXIS_MAX
// (32767, fully pressed) from SDL_GetGamepadAxis - and explicitly not the
// same range as the lower-level SDL_GetJoystickAxis
// (third_party/SDL3/include/SDL3/SDL_gamepad.h:216-218). XAMINPUT_GAMEPAD's
// triggers are 0..255. Pre-F-609 the only source was a binary U / O key, so
// a real pad's analog travel never reached the guest.
uint8_t TriggerToXInput(Sint16 v)
{
    if (v <= 0)
        return 0;
    return static_cast<uint8_t>((static_cast<uint32_t>(v) * 255u + 32767u / 2u) / 32767u);
}

// One stick, already in XInput orientation (+Y up) and dead-zoned.
// `threshold` is the caller's radial dead zone in 1/32767 units (§2.2).
void ReadStick(SDL_Gamepad* pad, SDL_GamepadAxis ax, SDL_GamepadAxis ay,
               int16_t& ox, int16_t& oy, int threshold)
{
    // SDL stick Y is +down and XInput sThumbLY is +up, hence the negation -
    // the same inversion UnleashedRecomp applies at
    // .research/UnleashedRecomp/UnleashedRecomp/hid/driver/sdl_hid.cpp:95,99.
    int x = SDL_GetGamepadAxis(pad, ax);
    int y = -static_cast<int>(SDL_GetGamepadAxis(pad, ay));   // clamp after negate
    y = std::clamp(y, -32768, 32767);

    ApplyDeadzone(x, y, threshold);
    ox = static_cast<int16_t>(x);
    oy = static_cast<int16_t>(y);
}

// §2.2: percent -> radial units. The clamp bounds the divisor in ApplyDeadzone
// and rejects nonsense CVars (NaN/inf fall back to 0 after std::lround of 0).
static int DeadzoneThreshold(float percent)
{
    if (!std::isfinite(percent)) percent = 0.0f;
    return std::clamp(static_cast<int>(std::lround(percent * 32767.0 / 100.0)), 0, 32000);
}

void Publish(uint32_t port, const Sample& in)
{
    const std::lock_guard<std::mutex> lock(g_portLock);
    PortSnapshot& s = g_ports[port];

    // dwPacketNumber is the guest's edge detector, so it has to go strictly up
    // whenever the state it describes changes - and it must never be derived
    // from whatever the caller left in the overlay. The pre-fix line read the
    // guest-visible (uninitialised) host word and incremented that (pre-fix
    // src/kernel/xam.cpp:314), so the first poll returned a packet number
    // built out of uninitialised host memory.
    const bool identityChanged = s.source != in.source || s.instance != in.instance;
    const bool stateChanged = s.seeded &&
        (s.buttons != in.buttons || s.ltrigger != in.ltrigger || s.rtrigger != in.rtrigger ||
         s.lx != in.lx || s.ly != in.ly || s.rx != in.rx || s.ry != in.ry);

    if (!s.seeded)
    {
        s.seeded = true;
        s.packet = 1;             // never hand the guest a zero packet number
    }
    else if (identityChanged || stateChanged)
    {
        s.packet++;               // strictly monotonic per port
    }

    if (identityChanged)
    {
        s.source     = in.source;
        s.instance   = in.instance;
        s.generation++;
        // A reconnect restarts the guest's edge sequence but the number the
        // guest can see still only ever goes up, so a title that cached the
        // last packet number cannot be fooled into replaying a stale edge.
    }

    s.buttons   = in.buttons;
    s.ltrigger  = in.ltrigger;
    s.rtrigger  = in.rtrigger;
    s.lx = in.lx; s.ly = in.ly; s.rx = in.rx; s.ry = in.ry;
    s.rumble = in.rumble;
    s.led    = in.led;

    // Unconditional, so an empty Sample{} clears the identity too: a port
    // whose controller was just unplugged must not keep advertising its name.
    const char* name = in.name ? in.name : "";
    strncpy(s.name, name, sizeof(s.name) - 1);
    s.name[sizeof(s.name) - 1] = 0;

    const char* guid = in.guid ? in.guid : "";
    strncpy(s.guid, guid, sizeof(s.guid) - 1);
    s.guid[sizeof(s.guid) - 1] = 0;
}

PortSnapshot ReadPort(uint32_t port)
{
    const std::lock_guard<std::mutex> lock(g_portLock);
    return g_ports[port];
}

Sample SampleGamepad(const Pad& p, uint32_t port)
{
    Sample s;
    s.source   = PadSource::Gamepad;
    s.instance = p.id;
    s.rumble   = p.rumbleOk;
    s.led      = p.ledOk;
    s.name     = p.name;
    s.guid     = p.guid;

    for (const ButtonMap& b : kButtonMap)
    {
        if (SDL_GetGamepadButton(p.pad, b.sdl))
            s.buttons |= b.guest;
    }

    s.ltrigger = TriggerToXInput(SDL_GetGamepadAxis(p.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    s.rtrigger = TriggerToXInput(SDL_GetGamepadAxis(p.pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));

    // §2.2: a CVar per stick, resolved from the live value each sample so the
    // pause menu can change dead zones without a restart.
    const int leftThreshold =
        DeadzoneThreshold(MCLA_CVAR_GET_FLOAT(mcla_left_stick_deadzone));
    const int rightThreshold =
        DeadzoneThreshold(MCLA_CVAR_GET_FLOAT(mcla_right_stick_deadzone));
    ReadStick(p.pad, SDL_GAMEPAD_AXIS_LEFTX,  SDL_GAMEPAD_AXIS_LEFTY,  s.lx, s.ly, leftThreshold);
    ReadStick(p.pad, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, s.rx, s.ry, rightThreshold);

    // §2.4: only a real pad counts. Triggers > 16 and axes beyond 8000 match
    // the spec's thresholds; a keyboard fallback never reaches this function.
    if (s.buttons != 0 || s.ltrigger > 16 || s.rtrigger > 16 ||
        std::abs(s.lx) > 8000 || std::abs(s.ly) > 8000 ||
        std::abs(s.rx) > 8000 || std::abs(s.ry) > 8000)
    {
        g_lastGamepadActivityMs.store(static_cast<uint64_t>(SDL_GetTicks()),
                                      std::memory_order_relaxed);
    }
    return s;
}

// ---------------------------------------------------------------------------
// LABELLED INPUT FALLBACK (F-609, explicitly opted-in diagnostic affordance).
// A host keyboard is not a controller, but the guest was authored against a
// wired 360 pad and will not present a menu at all without one, so port 0 is
// served from SDL_GetKeyboardState when - and only when - no SDL_Gamepad is
// open on that port. This is the one place in the file that reports input
// which did not come from hardware: it is tagged PadSource::Keyboard, named
// "host keyboard (labelled fallback)" in the port map, and every XamInput*
// log line produced from it carries the KBD_FALLBACK marker so a soak census
// (tools/soak_census.py) can never mistake it for a real pad. It never claims
// rumble - there are no motors.
//
// The verified key map below (SampleKeyboard, :633-654) is authoritative;
// docs/INPUT_DEEP_DIVE.md and docs/AUDIO_INPUT_3D_ANALYSIS.md describe it
// correctly except for two stale auto-press claims (see below). X, Y, BACK
// and both thumb buttons were absent pre-F-609 because the guest-visible
// bits were never written; they are C, V, B, 1, 2. A/S/D/W own the racing
// controls (steer/brake/throttle) and Q/E/I/J/K/L/U/O the rest. Arrow keys are
// menu-only D-pad. Driving keys are not sent through the guest keystroke queue:
// its own table interprets W/D as steering, which used to fight the fallback
// pad's throttle/steer mapping.
//
// PRESS-ENTER decision for the title PRESS START screen: RETURN (Enter)
// deliberately sets BOTH A (0x1000) and START (0x0010) (:639/:646); TAB sets
// START only and SPACE sets A only. The dual RETURN is intentional so Enter
// advances whether the guest tests A or START. There is NO auto-presser in
// this function: no timer, no env-gated pulse, no MCLA_AUTO_ADVANCE. A soak
// must physically press a key; do not re-add an auto-press shim (F-609).
// ---------------------------------------------------------------------------
// TEMP-AUTOENTER: MCLA_TEMP_AUTOENTER="35,50,62" holds Enter for 200 ms at those seconds after the
// first input poll -- in-process, so unattended test runs never send keystrokes to other windows.
// A token with a trailing 'd' ("50d") presses Down instead of Enter.
bool AutoKeyDown(bool wantDown)
{
    static const std::vector<std::pair<double, bool>> s_at = [] {
        std::vector<std::pair<double, bool>> v;
        if (const char* e = std::getenv("MCLA_TEMP_AUTOENTER"))
            for (const char* p = e; *p;) {
                const double t = std::strtod(p, const_cast<char**>(&p));
                const bool isDown = (*p == 'd');
                if (isDown) ++p;
                v.emplace_back(t, isDown);
                while (*p == ',' || *p == ' ') ++p;
            }
        return v;
    }();
    if (s_at.empty()) return false;
    static const auto t0 = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (const auto& [a, d] : s_at) if (d == wantDown && t >= a && t < a + 0.2) return true;
    return false;
}
bool AutoEnterDown() { return AutoKeyDown(false); }

// Unattended test runs must not inherit keys pressed by the desktop user or a
// second agent. This gates only physical SDL keyboard state; the in-process
// TEMP-AUTOENTER / TEMP-HOLD_* drivers below remain active.
bool TempInputOnly() {
    static const bool enabled = [] {
        const char* e = std::getenv("MCLA_TEMP_INPUT_ONLY");
        return e && e[0] == '1';
    }();
    return enabled;
}

// ---------------------------------------------------------------------------
// Configurable keyboard map (keymap2). Every binding lives in a CVar so the
// whole layout can be rebound at runtime (mcla.toml / settings menu) with no
// rebuild. The defaults below reproduce the previous hardcoded map exactly,
// including the extra drive keys (LShift/LCtrl/R/H/F/P): with no config change
// this is byte-for-byte the same set of SDL edges as before.
// Each value is a comma-separated list of key names; "W", "Up", "LShift",
// "Return", "Space", "F1" ... "F12", "Numpad0" ... "Numpad9" are accepted.
// ---------------------------------------------------------------------------
namespace {
MCLA_CVAR_STRING(mcla_key_throttle, "W", "Controls", "Keyboard: throttle (right stick up)");
MCLA_CVAR_STRING(mcla_key_brake, "S", "Controls", "Keyboard: brake (right stick down)");
MCLA_CVAR_STRING(mcla_key_steer_left, "A", "Controls", "Keyboard: steer left");
MCLA_CVAR_STRING(mcla_key_steer_right, "D", "Controls", "Keyboard: steer right");
MCLA_CVAR_STRING(mcla_key_dpad_up, "Up", "Controls", "Keyboard: menu / DPAD up");
MCLA_CVAR_STRING(mcla_key_dpad_down, "Down", "Controls", "Keyboard: menu / DPAD down");
MCLA_CVAR_STRING(mcla_key_dpad_left, "Left", "Controls", "Keyboard: menu / DPAD left");
MCLA_CVAR_STRING(mcla_key_dpad_right, "Right", "Controls", "Keyboard: menu / DPAD right");
MCLA_CVAR_STRING(mcla_key_a, "Return,Space", "Controls", "Keyboard: A (confirm)");
MCLA_CVAR_STRING(mcla_key_b, "Escape,Backspace", "Controls", "Keyboard: B (cancel)");
MCLA_CVAR_STRING(mcla_key_x, "C,LShift", "Controls", "Keyboard: X (handbrake)");
MCLA_CVAR_STRING(mcla_key_y, "V", "Controls", "Keyboard: Y (camera)");
MCLA_CVAR_STRING(mcla_key_back, "B,F", "Controls", "Keyboard: BACK (reset)");
MCLA_CVAR_STRING(mcla_key_start, "Tab,Return,P", "Controls", "Keyboard: START (pause)");
MCLA_CVAR_STRING(mcla_key_left_shoulder, "Q", "Controls", "Keyboard: left shoulder");
MCLA_CVAR_STRING(mcla_key_right_shoulder, "E,LCtrl", "Controls", "Keyboard: right shoulder (nitro)");
MCLA_CVAR_STRING(mcla_key_left_thumb, "1,H", "Controls", "Keyboard: left thumb (horn)");
MCLA_CVAR_STRING(mcla_key_right_thumb, "2,R", "Controls", "Keyboard: right thumb (camera)");
MCLA_CVAR_STRING(mcla_key_lt, "U", "Controls", "Keyboard: left trigger");
MCLA_CVAR_STRING(mcla_key_rt, "O", "Controls", "Keyboard: right trigger");
MCLA_CVAR_STRING(mcla_key_ry_up, "I", "Controls", "Keyboard: right stick up");
MCLA_CVAR_STRING(mcla_key_ry_down, "K", "Controls", "Keyboard: right stick down");
MCLA_CVAR_STRING(mcla_key_rx_left, "J", "Controls", "Keyboard: right stick left");
MCLA_CVAR_STRING(mcla_key_rx_right, "L", "Controls", "Keyboard: right stick right");
MCLA_CVAR_BOOL(mcla_keymap_log, false, "Controls", "Log the resolved keyboard map whenever it changes");

enum KeyAction : size_t
{
    kActThrottle, kActBrake, kActSteerLeft, kActSteerRight,
    kActDpadUp, kActDpadDown, kActDpadLeft, kActDpadRight,
    kActA, kActB, kActX, kActY, kActBack, kActStart,
    kActLeftShoulder, kActRightShoulder, kActLeftThumb, kActRightThumb,
    kActLT, kActRT, kActRYUp, kActRYDown, kActRXLeft, kActRXRight,
    kActCount
};

struct KeyActionDef { const char* cvar; KeyAction action; };
static const KeyActionDef kKeyActionDefs[] = {
    {"mcla_key_throttle", kActThrottle}, {"mcla_key_brake", kActBrake},
    {"mcla_key_steer_left", kActSteerLeft}, {"mcla_key_steer_right", kActSteerRight},
    {"mcla_key_dpad_up", kActDpadUp}, {"mcla_key_dpad_down", kActDpadDown},
    {"mcla_key_dpad_left", kActDpadLeft}, {"mcla_key_dpad_right", kActDpadRight},
    {"mcla_key_a", kActA}, {"mcla_key_b", kActB}, {"mcla_key_x", kActX}, {"mcla_key_y", kActY},
    {"mcla_key_back", kActBack}, {"mcla_key_start", kActStart},
    {"mcla_key_left_shoulder", kActLeftShoulder}, {"mcla_key_right_shoulder", kActRightShoulder},
    {"mcla_key_left_thumb", kActLeftThumb}, {"mcla_key_right_thumb", kActRightThumb},
    {"mcla_key_lt", kActLT}, {"mcla_key_rt", kActRT},
    {"mcla_key_ry_up", kActRYUp}, {"mcla_key_ry_down", kActRYDown},
    {"mcla_key_rx_left", kActRXLeft}, {"mcla_key_rx_right", kActRXRight},
};

struct KeyActionBinding
{
    std::string raw;
    std::vector<SDL_Keycode> keys;
};
static std::array<KeyActionBinding, kActCount> g_keyBindings{};
static bool g_keyMapLogged = false;

static void TrimView(std::string_view& v)
{
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
}

static SDL_Keycode KeyCodeFromName(std::string_view name)
{
    TrimView(name);
    if (name.empty()) return SDLK_UNKNOWN;
    if (name.size() == 1)
    {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<SDL_Keycode>(c);
        return SDLK_UNKNOWN;
    }
    struct Named { std::string_view name; SDL_Keycode code; };
    static const Named kNamed[] = {
        {"Up", SDLK_UP}, {"Down", SDLK_DOWN}, {"Left", SDLK_LEFT}, {"Right", SDLK_RIGHT},
        {"Return", SDLK_RETURN}, {"Enter", SDLK_RETURN}, {"Space", SDLK_SPACE},
        {"Escape", SDLK_ESCAPE}, {"Backspace", SDLK_BACKSPACE}, {"Tab", SDLK_TAB},
        {"LShift", SDLK_LSHIFT}, {"Shift", SDLK_LSHIFT}, {"RShift", SDLK_RSHIFT},
        {"LCtrl", SDLK_LCTRL}, {"Ctrl", SDLK_LCTRL}, {"RCtrl", SDLK_RCTRL},
        {"LAlt", SDLK_LALT}, {"RAlt", SDLK_RALT},
        {"Home", SDLK_HOME}, {"End", SDLK_END}, {"PageUp", SDLK_PAGEUP}, {"PageDown", SDLK_PAGEDOWN},
        {"Insert", SDLK_INSERT}, {"Delete", SDLK_DELETE}, {"Pause", SDLK_PAUSE},
        {"F1", SDLK_F1}, {"F2", SDLK_F2}, {"F3", SDLK_F3}, {"F4", SDLK_F4},
        {"F5", SDLK_F5}, {"F6", SDLK_F6}, {"F7", SDLK_F7}, {"F8", SDLK_F8},
        {"F9", SDLK_F9}, {"F10", SDLK_F10}, {"F11", SDLK_F11}, {"F12", SDLK_F12},
        {"Numpad0", SDLK_KP_0}, {"Numpad1", SDLK_KP_1}, {"Numpad2", SDLK_KP_2},
        {"Numpad3", SDLK_KP_3}, {"Numpad4", SDLK_KP_4}, {"Numpad5", SDLK_KP_5},
        {"Numpad6", SDLK_KP_6}, {"Numpad7", SDLK_KP_7}, {"Numpad8", SDLK_KP_8},
        {"Numpad9", SDLK_KP_9},
    };
    for (const auto& n2 : kNamed) if (name == n2.name) return n2.code;
    return SDLK_UNKNOWN;
}

// In-process equivalent of holding one configured key. Used with
// MCLA_TEMP_INPUT_ONLY=1 so runtime tests exercise the real binding without
// inheriting desktop keystrokes.
bool TempKeyDown(SDL_Keycode sym) {
    static const std::string keyName = [] {
        const char* e = std::getenv("MCLA_TEMP_HOLD_KEY");
        return e ? std::string(e) : std::string{};
    }();
    static const SDL_Keycode key = KeyCodeFromName(keyName);
    static const double at = [] {
        const char* e = std::getenv("MCLA_TEMP_HOLD_KEY_AT");
        return e ? std::atof(e) : 52.0;
    }();
    static const auto t0 = std::chrono::steady_clock::now();
    return key != SDLK_UNKNOWN && sym == key && at >= 0.0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= at;
}

// Re-resolve only when a CVar string actually changed. Returns true if any
// binding changed this call (the caller logs the resulting map once).
static bool RefreshKeyBindings(std::string* dumpOut)
{
    bool changed = false;
    for (const auto& def : kKeyActionDefs)
    {
        // Copy immediately: GetString's view is protected by the CVar mutex only
        // during the call; a concurrent SetString would invalidate it.
        const std::string v(mcla::cvar::CVarSystem::Instance().GetString(def.cvar));
        auto& bind = g_keyBindings[def.action];
        if (bind.raw == v) continue;
        bind.raw.assign(v);
        bind.keys.clear();
        size_t pos = 0;
        while (pos <= bind.raw.size())
        {
            const size_t comma = bind.raw.find(',', pos);
            const std::string_view part(bind.raw.data() + pos,
                (comma == std::string::npos ? bind.raw.size() : comma) - pos);
            const SDL_Keycode code = KeyCodeFromName(part);
            if (code != SDLK_UNKNOWN) bind.keys.push_back(code);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        changed = true;
    }
    if (changed && dumpOut)
    {
        std::string d;
        for (const auto& def : kKeyActionDefs)
        {
            d += def.cvar; d += "="; d += g_keyBindings[def.action].raw; d += "; ";
        }
        *dumpOut = std::move(d);
    }
    return changed;
}
} // namespace

Sample SampleKeyboard()
{
    Sample s;
    s.source = PadSource::Keyboard;
    s.name   = "host keyboard (labelled fallback)";

    int nkeys = 0;
    const bool* k = TempInputOnly() ? nullptr : SDL_GetKeyboardState(&nkeys);
    if ((k && nkeys > 0) || TempInputOnly())
    {
        auto down = [&](SDL_Keycode sym) -> bool {
            if (TempKeyDown(sym)) return true;
            if (!k) return false;
            const SDL_Scancode sc = SDL_GetScancodeFromKey(sym, nullptr);
            if (sc == SDL_SCANCODE_UNKNOWN || static_cast<int>(sc) >= nkeys)
                return false;
            return k[sc];
        };

        // Rebindable map: resolved from CVars, cached until a value changes.
        std::string mapDump;
        const bool mapChanged = RefreshKeyBindings(&mapDump);
        if (mapChanged && (mcla::cvar::CVarSystem::Instance().GetBool("mcla_keymap_log") || !g_keyMapLogged))
        {
            MCLA_LOG_ERROR("KEYMAP-CONFIG resolved: {}", mapDump);
            g_keyMapLogged = true;
        }
        auto act = [&](KeyAction a) -> bool {
            for (SDL_Keycode code : g_keyBindings[a].keys) if (down(code)) return true;
            return false;
        };

        // Racing layout, isolated: throttle/brake use right-stick Y and steering
        // uses left-stick X; no D-pad bits. Arrow keys are menu-only (D-pad).
        // Steering is a ramp so a key acts like a stick, not a switch.
        if (act(kActDpadUp))    { s.buttons |= XAMINPUT_GAMEPAD_DPAD_UP;   s.ly = 32767;  }
        if (act(kActDpadDown))  { s.buttons |= XAMINPUT_GAMEPAD_DPAD_DOWN; s.ly = -32768; }
        if (act(kActDpadLeft))  s.buttons |= XAMINPUT_GAMEPAD_DPAD_LEFT;
        if (act(kActDpadRight)) s.buttons |= XAMINPUT_GAMEPAD_DPAD_RIGHT;
        // The guest's active driving scheme reads throttle/brake from right-stick
        // Y (binding type 9, codes 26/27), not from the trigger bytes. W/S must
        // therefore drive that axis; sending RT/LT alone reached the pad but
        // never reached the car controls.
        if (act(kActThrottle)) s.ry = 32767;
        if (act(kActBrake)) s.ry = -32768;
        {
            const bool kl = act(kActSteerLeft), kr = act(kActSteerRight);
            static float steer = 0.f; static auto t0 = std::chrono::steady_clock::now();
            const auto t1 = std::chrono::steady_clock::now();
            const float dt = std::min(0.1f, std::chrono::duration<float>(t1 - t0).count()); t0 = t1;
            const float tgt = kl == kr ? 0.f : (kr ? 1.f : -1.f);
            const float rate = (tgt == 0.f || (tgt * steer) < 0.f) ? 6.f : 2.5f; // fast recentre, ~0.4 s to full lock
            const float d = tgt - steer, step = rate * dt;
            steer += d > step ? step : (d < -step ? -step : d);
            if (steer != 0.f) s.lx = static_cast<int16_t>(steer * 32767.f);
        }
        if (act(kActA)) s.buttons |= XAMINPUT_GAMEPAD_A;
        if (act(kActB)) s.buttons |= XAMINPUT_GAMEPAD_B;
        if (act(kActX)) s.buttons |= XAMINPUT_GAMEPAD_X;   // WEST on an Xbox pad
        if (act(kActY)) s.buttons |= XAMINPUT_GAMEPAD_Y;   // NORTH on an Xbox pad
        if (act(kActBack)) s.buttons |= XAMINPUT_GAMEPAD_BACK;
        if (act(kActLeftThumb)) s.buttons |= XAMINPUT_GAMEPAD_LEFT_THUMB;
        if (act(kActRightThumb)) s.buttons |= XAMINPUT_GAMEPAD_RIGHT_THUMB;
        if (act(kActStart)) s.buttons |= XAMINPUT_GAMEPAD_START;
        if (act(kActLeftShoulder)) s.buttons |= XAMINPUT_GAMEPAD_LEFT_SHOULDER;
        if (act(kActRightShoulder)) s.buttons |= XAMINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (act(kActRYUp)) s.ry = 32767;
        if (act(kActRYDown)) s.ry = -32768;
        if (act(kActRXLeft)) s.rx = -32768;
        if (act(kActRXRight)) s.rx = 32767;
        if (act(kActLT)) s.ltrigger = 255;
        if (act(kActRT)) s.rtrigger = 255;
    }
    if (AutoEnterDown()) s.buttons |= XAMINPUT_GAMEPAD_A | XAMINPUT_GAMEPAD_START;
    if (AutoKeyDown(true)) { s.buttons |= XAMINPUT_GAMEPAD_DPAD_DOWN; s.ly = -32768; }

    // TEMP-CMDFILE: MCLA_TEMP_CMDFILE=<path>. An external driver (python watching the dumped frames) writes
    // one word (A B X Y START BACK UP DOWN LEFT RIGHT RT) to the file; it is held 250 ms then the file is
    // deleted. State-driven input, no timing. In-process only, never OS keystrokes.
    {
        static const char* path = std::getenv("MCLA_TEMP_CMDFILE");
        static auto until = std::chrono::steady_clock::time_point{};
        static decltype(s.buttons) held = 0; static int polls = 0;
        static std::atomic<uint32_t> consumed{0};
        const auto now = std::chrono::steady_clock::now();
        if (path && now >= until && (++polls & 7) == 0) {
            held = 0;
            if (FILE* f = std::fopen(path, "rb")) {
                char w[16] = {}; std::fread(w, 1, 15, f); std::fclose(f); std::remove(path);
                const std::string k(w);
                if (k.rfind("A", 0) == 0)          held = XAMINPUT_GAMEPAD_A;
                else if (k.rfind("BACK", 0) == 0)  held = XAMINPUT_GAMEPAD_BACK;
                else if (k.rfind("B", 0) == 0)     held = XAMINPUT_GAMEPAD_B;
                else if (k.rfind("X", 0) == 0)     held = XAMINPUT_GAMEPAD_X;
                else if (k.rfind("Y", 0) == 0)     held = XAMINPUT_GAMEPAD_Y;
                else if (k.rfind("START", 0) == 0) held = XAMINPUT_GAMEPAD_START;
                else if (k.rfind("UP", 0) == 0)    held = XAMINPUT_GAMEPAD_DPAD_UP;
                else if (k.rfind("DOWN", 0) == 0)  held = XAMINPUT_GAMEPAD_DPAD_DOWN;
                else if (k.rfind("LEFT", 0) == 0)  held = XAMINPUT_GAMEPAD_DPAD_LEFT;
                else if (k.rfind("RIGHT", 0) == 0) held = XAMINPUT_GAMEPAD_DPAD_RIGHT;
                if (held) {
                    until = now + std::chrono::milliseconds(250);
                    const uint32_t n = consumed.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (n <= 8) MCLA_LOG_ERROR("TEMP-CMDFILE consumed #{} word={}", n, k.c_str());
                }
            }
        }
        if (path && now < until) s.buttons |= held;
    }

    return s;
}

void ApplyPendingRumble(uint32_t port, const Pad& p)
{
    if (!g_rumblePending[port].exchange(false, std::memory_order_acquire))
        return;

    const uint32_t req = g_rumbleReq[port].load(std::memory_order_relaxed);
    const Uint16 lo = static_cast<Uint16>(req & 0xFFFFu);
    const Uint16 hi = static_cast<Uint16>(req >> 16);

    // duration_ms 0 == "run until changed or stopped", which is exactly the
    // XAMINPUT_VIBRATION contract: each call cancels the previous effect and
    // a 0/0 pair is the stop.
    if (!SDL_RumbleGamepad(p.pad, lo, hi, 0) && (lo || hi))
    {
        static std::atomic<uint32_t> s_fail{0};
        const uint32_t n = s_fail.fetch_add(1) + 1;
        if (n <= 4 || (n % 1000) == 0)
            MCLA_LOG_WARN("XamInputSetState[{}] SDL_RumbleGamepad(port={} lo={} hi={}) failed: {}",
                          n, port, lo, hi, SDL_GetError());
    }
}

uint32_t FindFreePort()
{
    for (uint32_t port = 0; port < kNumPorts; ++port)
    {
        if (!g_pads[port].pad)
            return port;
    }
    return kNumPorts;
}

void OpenPad(SDL_JoystickID id)
{
    if (SDL_GetGamepadFromID(id))
        return;                                   // already open on some port

    const uint32_t port = FindFreePort();
    if (port == kNumPorts)
    {
        MCLA_LOG_WARN("XamInputAdd: controller id={} ignored - all {} guest ports are taken",
                      id, kNumPorts);
        return;
    }

    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad)
    {
        MCLA_LOG_WARN("XamInputAdd: SDL_OpenGamepad(id={}) failed: {}", id, SDL_GetError());
        return;
    }

    Pad& p = g_pads[port];
    p.pad = pad;
    p.id  = id;

    const char* name = SDL_GetGamepadName(pad);
    strncpy(p.name, name ? name : "unknown gamepad", sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = 0;
    // This SDL build has no SDL_GetGamepadGUID(SDL_Gamepad*) - only the
    // ForID form (third_party/SDL3/include/SDL3/SDL_gamepad.h:626) - so the
    // GUID is taken from the instance id, which is the same device.
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), p.guid, sizeof(p.guid));
    p.guid[sizeof(p.guid) - 1] = 0;

    // This SDL build exposes no SDL_GamepadHasRumble / SDL_GamepadHasLED
    // (neither appears anywhere under third_party/SDL3/include/SDL3/), so
    // capability is discovered by issuing the real call. A zero rumble is
    // SDL's "stop", which is a no-op on hardware that cannot do it, and the
    // Xbox 360 ring colour is this player's slot.
    p.rumbleOk = SDL_RumbleGamepad(pad, 0, 0, 0);
    p.ledOk    = SDL_SetGamepadLED(pad, kPortLed[port][0], kPortLed[port][1], kPortLed[port][2]);

    MCLA_LOG_INFO("XamInputAdd: port={} id={} name=\"{}\" guid={} sdl_type={} rumble={} led={} "
                  "(ring set to this slot's 360 player colour)",
                  port, id, p.name, p.guid,
                  static_cast<int>(SDL_GetGamepadType(pad)), p.rumbleOk, p.ledOk);
}

void ClosePad(SDL_JoystickID id)
{
    for (uint32_t port = 0; port < kNumPorts; ++port)
    {
        Pad& p = g_pads[port];
        if (!p.pad || p.id != id)
            continue;

        MCLA_LOG_INFO("XamInputRemove: port={} id={} name=\"{}\" - that port now reports "
                      "DEVICE_NOT_CONNECTED", port, id, p.name);
        SDL_CloseGamepad(p.pad);
        p = Pad{};

        // A rumble request must not survive an unplug: the device that asked
        // for it is gone, and the next pad to land on this port has to start
        // still.
        g_rumblePending[port].store(false, std::memory_order_relaxed);
        Publish(port, Sample{});
        return;
    }

    MCLA_LOG_WARN("XamInputRemove: no guest port holds controller id={}", id);
}

// Xenia's kInput trampolines pin user 0xFF and XINPUT_FLAG_ANY_USER to port 0
// and refuse a query whose low flag byte excludes gamepads
// (.research/xenia/src/xenia/kernel/xam/xam_input.cc:93-102). XamInputSetState
// takes `unk` in the flags slot and does not apply the flag-byte test there
// (xam_input.cc:110-124), so it is opt-in per caller.
bool ResolvePort(uint32_t userIndex, uint32_t flags, bool honourFlagByte, uint32_t& outPort)
{
    if (honourFlagByte && (flags & 0xFF) && (flags & kXInputFlagGamepad) == 0)
        return false;                        // "some device class, not a pad"

    uint32_t port = userIndex & 0xFF;
    if (port == 0xFF || (flags & kXInputFlagAnyUser))
        port = 0;                            // ANY_USER / wildcard user == player 0

    if (port >= kNumPorts)
        return false;                        // no such controller port exists

    outPort = port;
    return true;
}

// TEMP-HOLDRT/LT are test-driver overrides, not keyboard bindings. Apply them
// to whichever source actually owns the port, including the separately
// published keyboard fallback port. Otherwise a focused browser or a connected
// pad can make an unattended run sit still while the log claims throttle was
// requested.
void ApplyTempDriveOverrides(uint32_t port, Sample& s) {
    if (s.source == PadSource::None) return;
    static const double at = [] { const char* e = std::getenv("MCLA_TEMP_HOLD_RT"); return e ? std::atof(e) : -1.0; }();
    static const double atL = [] { const char* e = std::getenv("MCLA_TEMP_HOLD_LT"); return e ? std::atof(e) : -1.0; }();
    static const auto t0 = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const bool rt = at >= 0.0 && elapsed >= at;
    const bool lt = atL >= 0.0 && elapsed >= atL;
    if (rt) s.rtrigger = 255;
    if (lt) s.ltrigger = 255;
    if (rt || lt) {
        static std::atomic<uint32_t> logged{0};
        const uint32_t n = logged.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8)
            MCLA_LOG_ERROR("TEMP-DRIVE-OVERRIDE #{} port={} src={} elapsed={:.1f}s rt={} lt={}",
                           n, port, s.source == PadSource::Gamepad ? "gamepad" : "keyboard", elapsed, rt ? 1 : 0, lt ? 1 : 0);
    }
}

} // anonymous namespace

// §2.4: true when a real gamepad was active after `sinceMs`. Defined AFTER the
// anonymous namespace so it has external linkage for app.cpp; its members
// (g_lastGamepadActivityMs) remain visible via the implicit using-directive.
bool XamInputHadRecentGamepadActivity(uint64_t sinceMs)
{
    return g_lastGamepadActivityMs.load(std::memory_order_relaxed) > sinceMs;
}

// §2.1: SDL already reports the pad type in OpenPad's log (xam.cpp:1580).
// Expose it so a caller on the SDL/main thread can pick PS glyphs without a
// separate env token. Keyboard fallback is never a gamepad and is skipped.
bool XamInputActiveGamepadIsPlayStation()
{
    for (const Pad& p : g_pads)
    {
        if (!p.pad) continue;
        const SDL_GamepadType type = SDL_GetGamepadType(p.pad);
        return type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    }
    return false;
}

void XamInputInit()
{
    // MAIN THREAD, after SDL_Init(SDL_INIT_GAMEPAD) and before the guest boot
    // thread starts. App::Initialize used to hand the guest a window in which
    // nothing had ever been sampled, so a title that probes its controller
    // port in the first few milliseconds would have been told "no controller".
    SDL_UpdateGamepads();

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids)
    {
        for (int i = 0; i < count; ++i)
            OpenPad(ids[i]);
        SDL_free(ids);
    }

    XamInputPoll();

    uint32_t open = 0;
    for (const Pad& p : g_pads)
    {
        if (p.pad)
            open++;
    }

    MCLA_LOG_INFO("XamInputInit: {} controller(s) open across {} guest ports",
                  open, kNumPorts);

    // The port map, dumped once so every soak records which physical
    // controller ended up on which guest bay - the assignment is dynamic
    // (lowest free slot at plug time) and a plug after boot moves it.
    for (uint32_t port = 0; port < kNumPorts; ++port)
    {
        const PortSnapshot s = ReadPort(port);
        MCLA_LOG_INFO("XamInputPortMap: port={} source={} instance={} dev=\"{}\" guid={}",
                      port,
                      s.source == PadSource::Gamepad ? "gamepad"
                          : s.source == PadSource::Keyboard ? "keyboard-fallback" : "empty",
                      s.instance, s.name, s.guid);
    }
}

void XamInputShutdown()
{
    for (Pad& p : g_pads)
    {
        if (!p.pad)
            continue;

        if (p.rumbleOk)
            SDL_RumbleGamepad(p.pad, 0, 0, 0);     // leave the motors off
        SDL_CloseGamepad(p.pad);
        p = Pad{};
    }

    for (uint32_t port = 0; port < kNumPorts; ++port)
        Publish(port, Sample{});
}

void XamInputGamepadAdded(uint32_t sdlJoystickId)
{
    OpenPad(static_cast<SDL_JoystickID>(sdlJoystickId));
}

void XamInputGamepadRemoved(uint32_t sdlJoystickId)
{
    ClosePad(static_cast<SDL_JoystickID>(sdlJoystickId));
}

void XamInputPoll()
{
    // MAIN THREAD ONLY. Call after the SDL_PollEvent drain - SDL_PollEvent is
    // what runs SDL_UpdateGamepads, so a sample taken before it is a frame
    // behind.
    for (uint32_t port = 0; port < kNumPorts; ++port)
    {
        Sample s;
        if (g_pads[port].pad && !TempInputOnly())
        {
            ApplyPendingRumble(port, g_pads[port]);
            s = SampleGamepad(g_pads[port], port);
        }
        else if (port != kKeyboardFallbackPort)
        {
            // Some guest UI (notably the sign-in/save alert) polls a specific
            // user index such as 3 while no pad is connected; publishing an
            // empty sample there makes the alert ignore all keyboard input.
            static const bool s_kbAllPorts = [] {
                const char* e = std::getenv("MCLA_KEYBOARD_ALL_PORTS");
                return !(e && e[0] == '0');
            }();
            if (s_kbAllPorts) s = SampleKeyboard();
        }
        ApplyTempDriveOverrides(port, s);
        Publish(port, s);
    }

    // Published exactly once per tick. The fallback port is skipped in the
    // loop above on purpose: publishing an empty sample and then the keyboard
    // sample would cross the None boundary twice per frame, and each crossing
    // is a dwPacketNumber increment the guest never earned.
    // MCLA_INPUT_PROBE=1 (2026-11-20): ERROR-level, 1 Hz. Shows whether the
    // keyboard fallback port is active and whether SDL sees arrows / A keys.
    {
        static const bool s_inputProbe = [] {
            const char* e = std::getenv("MCLA_INPUT_PROBE");
            return e && e[0] == '1';
        }();
        if (s_inputProbe) {
            static uint64_t s_lastMs = 0;
            const uint64_t nowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            if (nowMs - s_lastMs >= 1000u) {
                s_lastMs = nowMs;
                int nk = 0; const bool* ks = SDL_GetKeyboardState(&nk);
                auto kd = [&](SDL_Scancode sc) { return ks && int(sc) < nk && ks[sc] ? 1 : 0; };
                MCLA_LOG_ERROR("INPUT-PROBE fallbackPad={} nkeys={} focused={} up={} down={} left={} right={} "
                               "enter={} space={} autoEnter={}",
                               g_pads[kKeyboardFallbackPort].pad ? 1 : 0, nk,
                               SDL_GetKeyboardFocus() ? 1 : 0,
                               kd(SDL_SCANCODE_UP), kd(SDL_SCANCODE_DOWN),
                               kd(SDL_SCANCODE_LEFT), kd(SDL_SCANCODE_RIGHT),
                               kd(SDL_SCANCODE_RETURN), kd(SDL_SCANCODE_SPACE),
                               AutoEnterDown() ? 1 : 0);
            }
        }
    }
    if (!g_pads[kKeyboardFallbackPort].pad || TempInputOnly())
    {
        // b-3: feed keystroke edges from the host keyboard path into the keystroke queue.
        static int s_ksEnabled = -2;
        if (s_ksEnabled == -2) {
            const char* e = std::getenv("MCLA_KEYSTROKE_QUEUE");
            s_ksEnabled = (e && *e && std::atoi(e) == 0) ? 0 : 1;
            if (s_ksEnabled) MCLA_LOG_INFO("KEYSTROKE-QUEUE ON (MCLA_KEYSTROKE_QUEUE!=0)");
        }
        if (s_ksEnabled) {
            int nkeys = 0;
            const bool* k = TempInputOnly() ? nullptr : SDL_GetKeyboardState(&nkeys);
            if (k || TempInputOnly()) {
                constexpr size_t kNumKeys = sizeof(kKeyMappings)/sizeof(kKeyMappings[0]);
                for (size_t i = 0; i < kNumKeys; ++i) {
                    const bool physicalDown = k && static_cast<int>(kKeyMappings[i].scancode) < nkeys
                        ? k[kKeyMappings[i].scancode] : false;
                    const bool down = physicalDown ||
                                      (kKeyMappings[i].scancode == SDL_SCANCODE_RETURN && AutoEnterDown()) ||
                                      (kKeyMappings[i].scancode == SDL_SCANCODE_DOWN && AutoKeyDown(true));
                    if (down != s_prevKeyState[i]) {
                        s_prevKeyState[i] = down;
                        XAMINPUT_KEYSTROKE_HOST ks{};
                        ks.vk = kKeyMappings[i].vk;
                        ks.unicode = 0;
                        ks.flags = down ? 0x0001u : 0x0002u; // KEYDOWN / KEYUP
                        ks.user_index = 0;
                        ks.hid_code = 0;
                        XamInputKeystrokeEnqueue(ks);
                        MCLA_LOG_ERROR("KEYSTROKE-ENQ vk=0x{:04X} {}", ks.vk, down ? "DOWN" : "UP");
                    }
                }
            }
        }
        Sample fallback = SampleKeyboard();
        ApplyTempDriveOverrides(kKeyboardFallbackPort, fallback);
        Publish(kKeyboardFallbackPort, fallback);
    }
}

uint32_t XamInputPortCount()
{
    return kNumPorts;
}

// The port map, for anything that needs to tell the title (or a native UI)
// which physical controller ended up on which guest port. Returns true only
// for a real device; the keyboard fallback reports false with a name, so a
// caller can still render "port 0: <host keyboard fallback>".
bool XamInputPortDevice(uint32_t port, char* outName, size_t cap, uint32_t* pInstance)
{
    if (port >= kNumPorts)
        return false;

    const PortSnapshot s = ReadPort(port);
    if (s.source == PadSource::None)
        return false;

    if (outName && cap)
    {
        strncpy(outName, s.name, cap - 1);
        outName[cap - 1] = 0;
    }
    if (pInstance)
        *pInstance = (s.source == PadSource::Gamepad) ? s.instance : 0;

    return s.source == PadSource::Gamepad;
}

uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* caps)
{
    static std::atomic<uint32_t> s_calls{0};
    const uint32_t n = s_calls.fetch_add(1) + 1;

    // `unk` is the leading word the Ex variant carries and the non-Ex one does
    // not; Xenia's entry point ignores it (xam_input.cc:65-84) and so does this
    // F-804: arity corrected to match the guest (3 args, not 4).


    if (!caps)
        return STATUS_INVALID_PARAMETER;   // Xenia returns X_ERROR_BAD_ARGUMENTS
                                           // here (xam_input.cc:45-47)

    if (!GuestRangeOk(caps, sizeof(XAMINPUT_CAPABILITIES)))
    {
        MCLA_LOG_WARN("XamInputGetCapabilities[{}] caps={:p} is outside the guest window "
                      "- nothing written", n, static_cast<const void*>(caps));
        return STATUS_INVALID_PARAMETER;
    }

    uint32_t port = kNumPorts;
    if (!ResolvePort(userIndex, flags, /*honourFlagByte*/ true, port))
        return kXErrorDeviceNotConnected;

    const PortSnapshot s = ReadPort(port);
    if (s.source == PadSource::None)
    {
        // Pre-F-609 this returned success with a zeroed struct for every port
        // including 1..3 and including ports that do not exist, so a title
        // could not tell "no pad" from "pad at rest" (pre-fix
        // src/kernel/xam.cpp:248-259). The struct is left untouched, exactly as
        // on the GetState path.
        if (n <= 8 || (n % 200) == 0)
            MCLA_LOG_INFO("XamInputGetCapabilities[{}] ui={:08X} flags={:08X} port={} "
                          "DEVICE_NOT_CONNECTED (nothing open on that bay)",
                          n, userIndex, flags, port);
        return kXErrorDeviceNotConnected;
    }

    caps->Type    = XAMINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XAMINPUT_DEVSUBTYPE_GAMEPAD;
    // dwFlags: a wired 360 pad reports 0. There is no guest-visible name for
    // "has an LED ring", so it stays 0 rather than inventing a capability bit
    // the kInput contract never defined.
    caps->Flags   = 0;
    caps->Gamepad.wButtons     = Bswap16(s.buttons);
    caps->Gamepad.bLeftTrigger  = s.ltrigger;
    caps->Gamepad.bRightTrigger = s.rtrigger;
    caps->Gamepad.sThumbLX      = Bswap16s(s.lx);
    caps->Gamepad.sThumbLY      = Bswap16s(s.ly);
    caps->Gamepad.sThumbRX      = Bswap16s(s.rx);
    caps->Gamepad.sThumbRY      = Bswap16s(s.ry);

    // On real hardware Gamepad and Vibration are the *current* state, not a
    // capability list, so Vibration mirrors the motor speeds this port was
    // last driven with instead of a hardcoded zero pair.
    const uint32_t last = g_rumbleReq[port].load(std::memory_order_relaxed);
    caps->Vibration.wLeftMotorSpeed  = Bswap16(static_cast<uint16_t>(last & 0xFFFFu));
    caps->Vibration.wRightMotorSpeed = Bswap16(static_cast<uint16_t>(last >> 16));

    if (n <= 8 || (n % 500) == 0)
    {
        MCLA_LOG_INFO("XamInputGetCapabilities[{}] port={} dev=\"{}\" guid={} rumble={} led={}"
                      "{}{}",
                      n, port, s.name, s.guid, s.rumble, s.led,
                      s.source == PadSource::Keyboard ? " KBD_FALLBACK" : "",
                      s.source == PadSource::Keyboard ? " (no motors)" : "");
    }

    return 0;
}

uint32_t XamInputGetState(uint32_t userIndex, uint32_t flags, XAMINPUT_STATE* state)
{
    static std::atomic<uint32_t> s_calls{0};
    const uint32_t callCount = s_calls.fetch_add(1) + 1;
    if (callCount == 1)
        MCLA_LOG_ERROR("XINPUT-FIRST-POLL ui={:08X} flags={:08X}", userIndex, flags);
    {
        static const bool s_alertInput = [] {
            const char* e = std::getenv("MCLA_ALERT_INPUT");
            return e && e[0] == '1';
        }();
        if (s_alertInput && (callCount <= 30u || (callCount % 500u) == 0u))
            MCLA_LOG_ERROR("ALERT-GETSTATE #{} ui={:08X} flags={:08X} state={:08X}",
                           callCount, userIndex, flags,
                           static_cast<uint32_t>(reinterpret_cast<uintptr_t>(state)));
    }
    // TEMP-EC9C: post-load frame loop reads global 0x8286EC9C (object made by
    // sub_822C7930) without a null check; print it ~every 150 polls.
    if (callCount == 1 || (callCount % 150) == 0) {
        uint32_t g = 0xFFFFFFFFu;
        const bool okg = mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(0x8286EC9Cu, &g);
        MCLA_LOG_ERROR("TEMP-EC9C poll={} u32[8286EC9C]={:08X} ok={}", callCount, g, okg ? 1 : 0);
    }

    // TEMP-DRIVE (opt-in MCLA_TEMP_DRIVE=1, INPUT agent): what the GUEST made of our packets. Every 150 polls print
    // pad0's guest-side fields (sub_82614D30 writes them: +8 button mask, +44 connected, +20 LT raw, +21 RT raw),
    // the per-player flag words read by sub_822A2370 (0x8288EDC4+4*k) and key table idx 17 (W) / 31 (S).
    // Read-only, checked helper; remove after one race boot.
    static const bool s_driveOn = [] { const char* e = std::getenv("MCLA_TEMP_DRIVE"); return e && e[0] == '1'; }();
    if (s_driveOn && (callCount % 150) == 0) {
        auto& h = mcla::kernel::GuestMemoryHeap::Instance();
        uint32_t p8 = 0, p20 = 0, p44 = 0, kidx = 0, kt = 0, f0 = 0, f1 = 0, a550c = 0, a5518 = 0;
        h.ReadU32BE(0x82905390u, &p8);  h.ReadU32BE(0x8290539Cu, &p20); h.ReadU32BE(0x829053B4u, &p44);
        h.ReadU32BE(0x82905838u, &kidx);
        h.ReadU32BE(0x82905538u + 256u * (kidx & 1u) + 16u, &kt);
        h.ReadU32BE(0x8288EDC4u, &f0);  h.ReadU32BE(0x8288EDC8u, &f1);
        h.ReadU32BE(0x8290550Cu, &a550c); h.ReadU32BE(0x82905518u, &a5518);
        MCLA_LOG_ERROR("TEMP-DRIVE poll={} pad0+8={:08X} pad0+20={:08X} pad0+44={:08X} k={} key16={:08X} "
                       "flag[0]={:08X} flag[1]={:08X} a550c={:08X} a5518={:08X}",
                       callCount, p8, p20, p44, kidx, kt, f0, f1, a550c, a5518);
    }

    // TEMP-DRIVE2 (opt-in MCLA_TEMP_DRIVE2=1): ~1 s cadence. Gates on the guest car-input path:
    //  sub_822A2988 (per-frame controls update) blocks input when G+4!=0 && G+10==0 (G=u32[828CCEA0]) or the
    //  v10 test fails (u32[82874374]+568 obj / u32[828CCFC4]+76 bits 0x80|0x40); sub_822A2988 zeroes
    //  throttle/brake when G+6!=0. Bindings: sub_822A4BB8 reads rows (type,code) at u32[8287438C+4*port]+8*ctl;
    //  rows come from mcControlConfigMgr (u32[828CD0EC]; +4 array of 608-byte schemes, +8 count) built by
    //  sub_82397BC0 from tune/controller/xenon_controls.maps. Controls object C = u32[player+896],
    //  player = u32[u32[82874374]+4*2] (or +4*34): C+12 throttle f32, +16 brake f32, +84 steer f32.
    static const bool s_drive2On = [] { const char* e = std::getenv("MCLA_TEMP_DRIVE2"); return e && e[0] == '1'; }();
    if (s_drive2On) {
        static auto s_last = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last >= std::chrono::seconds(1)) {
            s_last = now;
            auto& h = mcla::kernel::GuestMemoryHeap::Instance();
            auto U32 = [&](uint32_t a) { uint32_t v = 0xDEADBEEFu; (void)h.ReadU32BE(a, &v); return v; };
            auto U8 = [&](uint32_t a) { uint8_t v = 0xEE; (void)h.ReadU8(a, &v); return v; };
            auto F32 = [&](uint32_t a) { float v = -12345.f; (void)h.ReadF32BE(a, &v); return v; };
            const uint32_t G = U32(0x828CCEA0u), cf = U32(0x828CCFC4u), p374 = U32(0x82874374u), mgr = U32(0x828CD0EC);
            MCLA_LOG_ERROR("TEMP-DRIVE2 gate G={:08X} +4={:02X} +6={:02X} +10={:02X} cf+76={:02X} obj568={:08X} "
                           "mgr={:08X} mgr+4={:08X} mgr+8(count)={} bindptr0={:08X} bindOverride0={:02X}",
                           G, U8(G + 4), U8(G + 6), U8(G + 10), U8(cf + 76), U32(p374 + 568),
                           mgr, U32(mgr + 4), U32(mgr + 8), U32(0x8287438Cu), U8(0x82874391u));
            const uint32_t b0 = U32(0x8287438Cu);
            MCLA_LOG_ERROR("TEMP-DRIVE2 bind0 rows(type,code) 0:{:X},{:X} 1:{:X},{:X} 2:{:X},{:X} 3:{:X},{:X} 6:{:X},{:X} 7:{:X},{:X}",
                           U32(b0), U32(b0 + 4), U32(b0 + 8), U32(b0 + 12), U32(b0 + 16), U32(b0 + 20),
                           U32(b0 + 24), U32(b0 + 28), U32(b0 + 48), U32(b0 + 52), U32(b0 + 56), U32(b0 + 60));
            const uint32_t tbl = U32(U32(0x8288E5ACu)); // player-state obj of idx 0
            MCLA_LOG_ERROR("TEMP-DRIVE2 optflag(table[(pl+12)+297])={:08X}", U32(0x8288E920u + 4u * (U32(tbl + 12) + 297u)));
            for (uint32_t slot : {2u, 34u}) {
                const uint32_t pl = U32(p374 + 4u * slot), C = U32(pl + 896);
                if (!pl || !C) continue;
                const uint32_t bindObj = U32(C + 116);
                const uint32_t scheme = U32(bindObj + 4);
                const uint32_t vals = U32(bindObj + 12);
                const uint32_t binds = scheme < 64u ? U32(0x8287438Cu + 4u * scheme) : 0u;
                const uint32_t pad3 = 0x82905388u + 3u * 64u;
                MCLA_LOG_ERROR("TEMP-DRIVE2 slot={} pl={:08X} C={:08X} thr={:.3f} brk={:.3f} +20={:.3f} steer={:.3f} "
                               "C+96={:08X} C+120={:08X} C+72={:08X} C+76={:08X} C+80={:08X} port={:04X} C+116={:08X} "
                               "bindObj={:08X} scheme={} binds={:08X} vals={:08X} ctl2={:08X}/{:08X}/{:08X} ctl3={:08X}/{:08X}/{:08X} "
                               "pad3btn={:08X} pad3lt={} pad3rt={} pad3ry={} pad3conn={}",
                               slot, pl, C, F32(C + 12), F32(C + 16), F32(C + 20), F32(C + 84),
                               U32(C + 96), U32(C + 120), U32(C + 72), U32(C + 76), U32(C + 80),
                               (unsigned)(U32(C + 8) & 0xFFFF), U32(C + 116), bindObj, scheme, binds, vals,
                               U32(vals + 24), U32(vals + 28), U32(vals + 32),
                               U32(vals + 36), U32(vals + 40), U32(vals + 44),
                               U32(pad3 + 8), (unsigned)U8(pad3 + 20), (unsigned)U8(pad3 + 21),
                               (unsigned)U8(pad3 + 19), (unsigned)U8(pad3 + 44));
            }
        }
    }

    // TEMP-VEH (opt-in MCLA_TEMP_VEH=1; MCLA_TEMP_VEHFIX=1 adds a one-shot diagnostic write): ~1 s, read-only probe of
    // the vehicle behind the controls object C (u32[pl+896]). Chain (all measured from IDA/raw words):
    //  C+72=W (sub_822A2988 a1+72); W+8=V (vehicle, base vtbl off_8207D214, ctor sub_82558040); W+16=R (racer; R+1928 =
    //  state, init 4 at sub_8234DE50:+0x?; <4 = countdown); V+16=D (D+46 u16 = GEAR, 0 = neutral); V+20=T (drivetrain);
    //  V+128 = physics inst; V+288 = position row; V+224 = speed. sub_822A1058 copies C+24/12/16/20/28 -> V+132/136/140/144/148
    //  and sub_8232EA78 (per-frame vehicle input stage) recopies them then: V+592&1 or racer state<4 => throttle 0 / brake 1;
    //  gear (D+46)==0 && state>=4 => V+136 forced 0.0 (sub_8232EA78 LABEL_65, `else V+136 = 0.0`).
    static const bool s_vehOn = [] { const char* e = std::getenv("MCLA_TEMP_VEH"); return e && e[0] == '1'; }();
    static const bool s_vehFix = [] { const char* e = std::getenv("MCLA_TEMP_VEHFIX"); return e && e[0] == '1'; }();
    if (s_vehOn || s_vehFix) {
        static auto s_vl = std::chrono::steady_clock::time_point{};
        const auto vnow = std::chrono::steady_clock::now();
        if (vnow - s_vl >= std::chrono::seconds(1)) {
            s_vl = vnow;
            auto& h = mcla::kernel::GuestMemoryHeap::Instance();
            auto U32 = [&](uint32_t a) { uint32_t v = 0xDEADBEEFu; (void)h.ReadU32BE(a, &v); return v; };
            auto U16 = [&](uint32_t a) { uint16_t v = 0xEEEEu; (void)h.ReadU16BE(a, &v); return v; };
            auto U8 = [&](uint32_t a) { uint8_t v = 0xEE; (void)h.ReadU8(a, &v); return v; };
            auto F32 = [&](uint32_t a) { float v = -12345.f; (void)h.ReadF32BE(a, &v); return v; };
            const uint32_t p374 = U32(0x82874374u);
            const uint32_t pl = U32(p374 + 4u * 2u), C = U32(pl + 896);
            const uint32_t W = C ? U32(C + 72) : 0, V = W ? U32(W + 8) : 0, R = W ? U32(W + 16) : 0;
            if (V) {
                const uint32_t D = U32(V + 16), T = U32(V + 20), TN = U32(V + 368), E = U32(V + 400), BODY = U32(V + 128);
                MCLA_LOG_ERROR("TEMP-VEH A C={:08X} W={:08X} V={:08X} R={:08X} Rstate={} | C thr={:.3f} brk={:.3f} | V in steer={:.3f} "
                               "thr={:.3f} brk={:.3f} clu={:.3f} +148={:.3f} | V+592={:08X} V+624={:08X} gear(D+46)={} D={:08X} "
                               "D+76={:.3f} spd(V+224)={:.3f} T={:08X} T+20={} T+32={:02X} V+664+32={:02X} V+818={:02X} V+816={:02X}",
                               C, W, V, R, (int)U32(R + 1928), F32(C + 12), F32(C + 16), F32(V + 132), F32(V + 136), F32(V + 140),
                               F32(V + 144), F32(V + 148), U32(V + 592), U32(V + 624), (unsigned)U16(D + 46), D, F32(D + 76),
                               F32(V + 224), T, (int)U32(T + 20), U8(T + 32), U8(U32(V + 664) + 32), U8(V + 818), U8(V + 816));
                MCLA_LOG_ERROR("TEMP-VEH B pos(V+288)=({:.2f},{:.2f},{:.2f}) prev(V+352)=({:.2f},{:.2f},{:.2f}) fwd(V+272)=({:.2f},{:.2f},{:.2f}) "
                               "body={:08X} V+112={:.3f}/{:.3f}/{:.3f}/{:.3f} V+396={:.3f} V+572={:.3f} wheels={} wheelArr={:08X}",
                               F32(V + 288), F32(V + 292), F32(V + 296), F32(V + 352), F32(V + 356), F32(V + 360), F32(V + 272),
                               F32(V + 276), F32(V + 280), BODY, F32(V + 112), F32(V + 116), F32(V + 120), F32(V + 124), F32(V + 396),
                               F32(V + 572), (int)U32(V + 24), U32(V + 28));
                MCLA_LOG_ERROR("TEMP-VEH C engine E={:08X} E+20={} E+24={:08X} E+48={:.3f} (E+24)+420={:.3f} E[16..64]={:08X} {:08X} {:08X} {:08X} "
                               "{:08X} {:08X} {:08X} {:08X} | tune TN={:08X} +56={:.3f} +224={:.3f} +228={:.3f} +300={:.3f} V+404={:08X} "
                               "V+400->20={}",
                               E, (int)U32(E + 20), U32(E + 24), F32(E + 48), F32(U32(E + 24) + 420), U32(E + 16), U32(E + 20), U32(E + 24),
                               U32(E + 28), U32(E + 32), U32(E + 36), U32(E + 40), U32(E + 44), TN, F32(TN + 56), F32(TN + 224),
                               F32(TN + 228), F32(TN + 300), U32(V + 404), (int)U32(E + 20));
                const uint32_t wa = U32(V + 28), wn = U32(V + 24);
                for (uint32_t i = 0; i < wn && i < 4; ++i) {
                    const uint32_t Wh = U32(wa + 4 * i);
                    MCLA_LOG_ERROR("TEMP-VEH D wheel{} ptr={:08X} f[0..11]={:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} "
                                   "{:.2f} {:.2f} {:.2f} u8@32={:02X}", i, Wh, F32(Wh), F32(Wh + 4), F32(Wh + 8), F32(Wh + 12),
                                   F32(Wh + 16), F32(Wh + 20), F32(Wh + 24), F32(Wh + 28), F32(Wh + 32), F32(Wh + 36), F32(Wh + 40),
                                   F32(Wh + 44), U8(Wh + 32));
                }
                {   // TEMP-VEH E/F: is the object the guest reads (sub_8232EA78:23-30) the same C? body kinematics, sim gates.
                    static float s_px = 0, s_py = 0, s_pz = 0;
                    const float px = F32(V + 288), py = F32(V + 292), pz = F32(V + 296);
                    const float dpos = std::sqrt((px - s_px) * (px - s_px) + (py - s_py) * (py - s_py) + (pz - s_pz) * (pz - s_pz));
                    s_px = px; s_py = py; s_pz = pz;
                    const uint32_t v4 = U32(V + 4), v3 = v4 ? U32(v4 + 4) : 0, Wh0 = U32(wa);
                    MCLA_LOG_ERROR("TEMP-VEH E V+4={:08X} v3={:08X} v3==C:{} u16(v3+8)={:04X} | v3 thr={:.3f} brk={:.3f} +20={:.3f} | V in +132..148={:.3f} {:.3f} {:.3f} {:.3f} {:.3f}",
                                   v4, v3, (int)(v3 == C), v3 ? (unsigned)U16(v3 + 8) : 0xDEADu, F32(v3 + 12), F32(v3 + 16), F32(v3 + 20),
                                   F32(V + 132), F32(V + 136), F32(V + 140), F32(V + 144), F32(V + 148));
                    MCLA_LOG_ERROR("TEMP-VEH F body={:08X} b+288..296={:.3f} {:.3f} {:.3f} b+272..280={:.3f} {:.3f} {:.3f} dpos/s={:.4f} wheel0raw={:08X} {:08X} "
                                   "| 7508={:.5f} 8288E614={:08X} 8288E6F0={:08X} R+1928={} D+46={} V+592={:08X}",
                                   BODY, F32(BODY + 288), F32(BODY + 292), F32(BODY + 296), F32(BODY + 272), F32(BODY + 276), F32(BODY + 280), dpos,
                                   U32(Wh0), U32(Wh0 + 12), F32(0x827D7508u), U32(0x8288E614u), U32(0x8288E6F0u), (int)U32(R + 1928),
                                   (unsigned)U16(D + 46), U32(V + 592));
                }
                // MCLA_TEMP_CARS2_VEH=1: wheel drive chain. sub_82565320 (wheel+480 = tune[+116]*wheel[+428]*wheel[+420]*a2) is what turns
                // throttle into drive force; sub_825652B8 fills wheel+480/+492 from +484/+488. E[+24..] = driven wheels, count E+20.
                static const bool s_c2 = [] { const char* e = std::getenv("MCLA_TEMP_CARS2_VEH"); return e && e[0] == '1'; }();
                if (s_c2) {
                    for (uint32_t i = 0; i < wn && i < 4; ++i) {
                        const uint32_t Wh = U32(wa + 4 * i), WT = U32(Wh + 4);
                        MCLA_LOG_ERROR("TEMP-CARS2 wheel{} tune={:08X} tune+116={:.3f} tune+40={:.3f} tune+68={:.3f} w+384={:.3f} w+420={:.3f} w+428={:.3f} "
                                       "w+480={:.3f} w+484={:.3f} w+488={:.3f} w+492={:.3f}", i, WT, F32(WT + 116), F32(WT + 40), F32(WT + 68),
                                       F32(Wh + 384), F32(Wh + 420), F32(Wh + 428), F32(Wh + 480), F32(Wh + 484), F32(Wh + 488), F32(Wh + 492));
                    }
                    MCLA_LOG_ERROR("TEMP-CARS2 V+132..148={:.3f} {:.3f} {:.3f} {:.3f} {:.3f} V+160(vel?)={:.3f} {:.3f} {:.3f} V+224={:.3f} V+576={:04X} V+584={:.3f} "
                                   "D+72..80={:.3f} {:.3f} {:.3f} T+216..232={:.3f} {:.3f} {:.3f} {:.3f}",
                                   F32(V + 132), F32(V + 136), F32(V + 140), F32(V + 144), F32(V + 148), F32(V + 160), F32(V + 164), F32(V + 168),
                                   F32(V + 224), (unsigned)U16(V + 576), F32(V + 584), F32(D + 72), F32(D + 76), F32(D + 80),
                                   F32(T + 216), F32(T + 220), F32(T + 224), F32(T + 228));
                }
                // Diagnostic write (MCLA_TEMP_VEHFIX=1): game thread races us for one u16; test-only. Racing state (>=4) with
                // gear 0 makes sub_8232EA78 zero V+136 every frame, and the auto-box cannot leave neutral without throttle.
                if (s_vehFix && D && U16(D + 46) == 0 && F32(C + 12) > 0.1f && (int)U32(R + 1928) >= 4) {
                    static int s_n = 0;
                    const bool wr = h.WriteU16BE(D + 46, 1);
                    if (s_n++ < 5) MCLA_LOG_ERROR("TEMP-VEHFIX gear 0->1 at {:08X} wr={}", D + 46, (int)wr);
                }
            } else {
                MCLA_LOG_ERROR("TEMP-VEH no vehicle: pl={:08X} C={:08X} W={:08X}", pl, C, W);
            }
        }
    }

    // A NULL state pointer is upstream's documented probe case: "Games call
    // this with a NULL state ptr, probably as a query"
    // (.research/xenia/src/xenia/kernel/xam/xam_input.cc:91). It is answered
    // here with 0 = ERROR_SUCCESS and nothing written - NOT an error code,
    // and deliberately answered before the port is resolved: the probe asks
    // whether the subsystem exists, not whether a pad is plugged in.
    // X_ERROR_EMPTY is 0x10D2 and X_ERROR_DEVICE_NOT_CONNECTED is 0x48F
    // (.research/xenia/src/xenia/xbox.h:86,100,106).
    if (!state) {
        MCLA_LOG_INFO("XINPUT-DEBUG: XamInputGetState probe (NULL state) ui={:08X} flags={:08X}", userIndex, flags);
        return 0;
    }

    if (!GuestRangeOk(state, sizeof(XAMINPUT_STATE)))
    {
        MCLA_LOG_WARN("XamInputGetState[{}] state={:p} is outside the guest window "
                      "- nothing written", callCount, static_cast<const void*>(state));
        return STATUS_INVALID_PARAMETER;
    }

    uint32_t port = kNumPorts;
    if (!ResolvePort(userIndex, flags, /*honourFlagByte*/ true, port))
        return kXErrorDeviceNotConnected;

    const PortSnapshot s = ReadPort(port);
    if (s.source == PadSource::None)
    {
        // Per-port honesty (F-609). Pre-F-609 every port - 0, 1, 2, 3 and
        // ports that do not exist - returned ERROR_SUCCESS carrying an
        // identical keyboard state, so a title that queries one specific
        // player slot could not distinguish a phantom pad from a real one and
        // could not implement "press A to continue". Ports with nothing
        // behind them now answer 0x48F and leave the overlay untouched, which
        // is what a retail Xbox 360 answers for an empty controller bay.
        if (callCount <= 8 || (callCount % 2000) == 0)
            MCLA_LOG_INFO("XamInputGetState[{}] port={} DEVICE_NOT_CONNECTED "
                          "(nothing open on that bay)", callCount, port);
        return kXErrorDeviceNotConnected;
    }

    // Guest is big-endian PPC and `state` is a host view of guest RAM, so
    // store the multi-byte fields swapped. dwPacketNumber comes from the
    // main-thread snapshot, never from reading back the overlay: the pre-fix
    // code incremented whatever uninitialised host word the caller's buffer
    // happened to hold (pre-fix src/kernel/xam.cpp:314), so the first poll
    // after boot reported a packet number built out of stack garbage. The
    // snapshot counter is seeded to 1 and then only ever increases on this
    // port.
    state->dwPacketNumber          = Bswap32(s.packet);
    state->Gamepad.wButtons        = Bswap16(s.buttons);
    state->Gamepad.bLeftTrigger    = s.ltrigger;
    state->Gamepad.bRightTrigger   = s.rtrigger;
    state->Gamepad.sThumbLX        = Bswap16s(s.lx);
    state->Gamepad.sThumbLY        = Bswap16s(s.ly);
    state->Gamepad.sThumbRX        = Bswap16s(s.rx);
    state->Gamepad.sThumbRY        = Bswap16s(s.ry);

    // TEMP input-path proof for unattended driving: print only trigger transitions
    // (and a sparse heartbeat) so a held throttle can be traced through the guest
    // poll instead of inferring delivery from the main-thread override log.
    static const bool s_inputStateLog = [] {
        const char* e = std::getenv("MCLA_INPUT_STATE_LOG");
        return e && e[0] == '1';
    }();
    if (s_inputStateLog) {
        static uint8_t s_lastRt[4] = {};
        static uint8_t s_lastLt[4] = {};
        static uint32_t s_lastHeartbeat = 0;
        const bool triggerEdge = s_lastRt[port] != s.rtrigger || s_lastLt[port] != s.ltrigger;
        const bool heartbeat = s.rtrigger && (callCount - s_lastHeartbeat >= 5000u);
        if (triggerEdge || heartbeat) {
            s_lastRt[port] = s.rtrigger;
            s_lastLt[port] = s.ltrigger;
            if (heartbeat) s_lastHeartbeat = callCount;
            MCLA_LOG_ERROR("INPUT-STATE poll={} ui={:08X} flags={:08X} port={} src={} packet={} "
                           "buttons={:04X} lt={} rt={} edge={} heartbeat={}",
                           callCount, userIndex, flags, port,
                           s.source == PadSource::Gamepad ? "gamepad" : "keyboard",
                           s.packet, s.buttons, s.ltrigger, s.rtrigger,
                           triggerEdge ? 1 : 0, heartbeat ? 1 : 0);
        }
    }

    if (s.buttons != 0)
    {
        static std::atomic<uint32_t> s_btnDelivered{0};
        const uint32_t bCount = s_btnDelivered.fetch_add(1) + 1;
        if (bCount <= 8)
        {
            MCLA_LOG_ERROR("XINPUT-BUTTON-DELIVERED[{}] port={} packet={} buttons=0x{:04X}",
                           bCount, port, s.packet, s.buttons);
        }
    }

    if (callCount <= 8 || (callCount % 2000) == 0)
    {
        // ui/flags are printed raw so a soak can prove the Xenia flag decode
        // above is not silently rejecting this title's queries - it was
        // ignored outright before F-609, so there is no prior measurement of
        // what this guest actually passes in the flags slot.
        MCLA_LOG_INFO("XamInputGetState[{}] ui={:08X} flags={:08X} port={} src={} packet={} "
                      "buttons={:04X} lt={} rt={} lx={} ly={} rx={} ry={}{}",
                      callCount, userIndex, flags, port,
                      s.source == PadSource::Gamepad ? "gamepad" : "keyboard",
                      s.packet, s.buttons, s.ltrigger, s.rtrigger,
                      s.lx, s.ly, s.rx, s.ry,
                      s.source == PadSource::Keyboard ? " KBD_FALLBACK" : "");
    }

    return 0; // ERROR_SUCCESS
}

uint32_t XamInputSetState(uint32_t userIndex, uint32_t flags, XAMINPUT_VIBRATION* vibration)
{
    static std::atomic<uint32_t> s_calls{0};
    const uint32_t n = s_calls.fetch_add(1) + 1;

    // Pre-F-609 this was a bare `return 0`: motor commands were accepted and
    // dropped, so no rumble ever reached a controller.
    uint32_t port = kNumPorts;
    if (!ResolvePort(userIndex, flags, /*honourFlagByte*/ false, port))
        return kXErrorDeviceNotConnected;

    if (!vibration)
        return STATUS_INVALID_PARAMETER;

    if (!GuestRangeOk(vibration, sizeof(XAMINPUT_VIBRATION)))
    {
        MCLA_LOG_WARN("XamInputSetState[{}] vibration={:p} is outside the guest window "
                      "- ignored", n, static_cast<const void*>(vibration));
        return STATUS_INVALID_PARAMETER;
    }

    // Guest RAM is big-endian, so the motor speeds are read back through the
    // checked, endian-explicit helper (src/kernel/memory.h:96) rather than
    // trusting the little-endian host view of the same bytes.
    const uint32_t guestVA = mcla::kernel::g_memory.MapVirtual(vibration);
    uint16_t left = 0, right = 0;
    mcla::kernel::GuestMemoryHeap& heap = mcla::kernel::GuestMemoryHeap::Instance();
    if (!heap.ReadU16BE(guestVA, &left) || !heap.ReadU16BE(guestVA + 2, &right))
    {
        MCLA_LOG_WARN("XamInputSetState[{}] XAMINPUT_VIBRATION @{:08X} is not a committed "
                      "guest range - ignored", n, guestVA);
        return STATUS_INVALID_PARAMETER;
    }

    // Wave 3 (§2.2): mcla_vibration off forces BOTH motors to zero. The zeros
    // are still queued below, which also stops a motor that is already spinning
    // when the CVar is toggled off mid-drive (XamInputPoll calls
    // SDL_RumbleGamepad unconditionally at xam.cpp:1451). No new guest hook:
    // every rumble source funnels through this export.
    if (!MCLA_CVAR_GET_BOOL(mcla_vibration))
    {
        if (n <= 8 || (n % 500) == 0)
            MCLA_LOG_INFO("XamInputSetState[{}] port={} VIB-GATE off: forced left=0 right=0 "
                          "(guest asked {}/{})", n, port, left, right);
        left = 0;
        right = 0;
    }

    const PortSnapshot s = ReadPort(port);
    if (s.source != PadSource::Gamepad)
    {
        // A keyboard-fallback port accepts and drops the command: the zeros it
        // advertises are honest, because there are no motors there. A port
        // with nothing at all behind it is a hard device error.
        if (s.source == PadSource::Keyboard)
        {
            if (n <= 8 || (n % 500) == 0)
                MCLA_LOG_INFO("XamInputSetState[{}] port={} KBD_FALLBACK lo={} hi={} dropped "
                              "(labelled fallback has no motors)", n, port, left, right);
            return 0;
        }
        return kXErrorDeviceNotConnected;
    }

    // Hand the request to the main thread rather than calling SDL here: the
    // SDL_Gamepad* it would use is main-thread-owned and can be closed by an
    // unplug at any moment. One main-loop tick of latency is irrelevant next
    // to a real pad's motor spin-up.
    g_rumbleReq[port].store(static_cast<uint32_t>(left) | (static_cast<uint32_t>(right) << 16),
                            std::memory_order_relaxed);
    g_rumblePending[port].store(true, std::memory_order_release);

    if (n <= 8 || (n % 500) == 0)
        MCLA_LOG_INFO("XamInputSetState[{}] port={} dev=\"{}\" left={} right={} queued",
                      n, port, s.name, left, right);

    return 0;
}

// ---------------------------------------------------------------------------
// XamInputGetUserVibrationLevel -- xam.xex ordinal 442 (F-952, 2026-11-15).
//
// WHY IT EXISTS HERE. build/v2.log: the guest resolves this at boot through its
// own runtime resolver (sub_8275C7B8, lr=8275C7F8 in the XEX-GPA rows) and the
// host answered 0xC0000263 X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND, so the
// resolved pointer stayed 0 and the guest skipped the call. The console's
// export table names ordinal 0x1BA (xam_table.inc:239); its signature is the
// XAM one, (dwUserIndex, dwFlags, XAMINPUT_VIBRATION*), the same out struct
// XamInputSetState takes as input.
//
// WHAT IT REPORTS. The motor speeds this port was last driven with - the same
// g_rumbleReq word XamInputGetCapabilities already mirrors into its Vibration
// field (xam.cpp:1399-1401), i.e. what the host actually commanded, not a
// fabricated value. A port with nothing behind it is DEVICE_NOT_CONNECTED,
// exactly like GetState/GetCapabilities/SetState.
//
// FALSIFIER: one XEX-GPA-THUNK row for ordinal 442, then a
// XINPUT-VIBLEVEL row naming the port and the two speeds; the guest's resolved
// pointer must be non-zero. If the guest never calls it, no row appears.
uint32_t XamInputGetUserVibrationLevel(uint32_t userIndex, uint32_t flags,
                                       XAMINPUT_VIBRATION* vibration)
{
    static std::atomic<uint32_t> s_calls{0};
    const uint32_t n = s_calls.fetch_add(1) + 1;

    uint32_t port = kNumPorts;
    if (!ResolvePort(userIndex, flags, /*honourFlagByte*/ true, port))
        return kXErrorDeviceNotConnected;

    if (!vibration)
        return STATUS_INVALID_PARAMETER;

    if (!GuestRangeOk(vibration, sizeof(XAMINPUT_VIBRATION)))
    {
        MCLA_LOG_WARN("XamInputGetUserVibrationLevel[{}] vibration={:p} is "
                      "outside the guest window - nothing written",
                      n, static_cast<const void*>(vibration));
        return STATUS_INVALID_PARAMETER;
    }

    const PortSnapshot s = ReadPort(port);
    if (s.source == PadSource::None)
    {
        if (n <= 8 || (n % 500) == 0)
            MCLA_LOG_INFO("XamInputGetUserVibrationLevel[{}] ui={:08X} "
                          "flags={:08X} port={} DEVICE_NOT_CONNECTED",
                          n, userIndex, flags, port);
        return kXErrorDeviceNotConnected;
    }

    const uint32_t level = g_rumbleReq[port].load(std::memory_order_relaxed);
    const uint16_t left = static_cast<uint16_t>(level & 0xFFFFu);
    const uint16_t right = static_cast<uint16_t>(level >> 16);

    // Guest RAM is big-endian: write through the checked helper, never through
    // the host view of the struct (the same rule XamInputSetState reads under).
    const uint32_t guestVA = mcla::kernel::g_memory.MapVirtual(vibration);
    mcla::kernel::GuestMemoryHeap& heap = mcla::kernel::GuestMemoryHeap::Instance();
    if (!heap.WriteU16BE(guestVA, left) || !heap.WriteU16BE(guestVA + 2, right))
    {
        MCLA_LOG_WARN("XamInputGetUserVibrationLevel[{}] XAMINPUT_VIBRATION "
                      "@{:08X} is not a committed guest range - nothing written",
                      n, guestVA);
        return STATUS_INVALID_PARAMETER;
    }

    if (n <= 8 || (n % 500) == 0)
        MCLA_LOG_INFO("XamInputGetUserVibrationLevel[{}] port={} dev=\"{}\" "
                      "left={} right={} src={}",
                      n, port, s.name, left, right,
                      s.source == PadSource::Gamepad ? "gamepad" : "keyboard");

    return 0;
}

// ---------------------------------------------------------------------------
// XamUserGetDeviceContext -- xam.xex ordinal 520 (F-952, 2026-11-15).
//
// Reference behaviour, measured in .research/xenia/src/xenia/kernel/xam/
// xam_input.cc:182-195 (DECLARE_XAM_EXPORT1(..., kStub)): the out word is
// zeroed on BOTH paths, user 0 (or 0xFF = "any user") answers X_E_SUCCESS and
// every other index answers X_E_DEVICE_NOT_CONNECTED. The guest masks the
// result, so a success with 0 is what a title without a camera/device expects.
//
// FALSIFIER: one XEX-GPA-THUNK row for ordinal 520 and, on the guest's call, a
// XAMUSER-DEVCTX row; the out word in guest memory must read 0 afterwards.
uint32_t XamUserGetDeviceContext(uint32_t userIndex, uint32_t unk,
                                 uint32_t* outDeviceContext)
{
    static std::atomic<uint32_t> s_calls{0};
    const uint32_t n = s_calls.fetch_add(1) + 1;

    if (!outDeviceContext)
        return STATUS_INVALID_PARAMETER;

    if (!GuestRangeOk(outDeviceContext, sizeof(uint32_t)))
    {
        MCLA_LOG_WARN("XamUserGetDeviceContext[{}] out={:p} is outside the "
                      "guest window - nothing written",
                      n, static_cast<const void*>(outDeviceContext));
        return STATUS_INVALID_PARAMETER;
    }

    const uint32_t guestVA = mcla::kernel::g_memory.MapVirtual(outDeviceContext);
    (void)mcla::kernel::GuestMemoryHeap::Instance().WriteU32BE(guestVA, 0);

    const bool anyUser = (userIndex & 0xFFu) == 0xFFu;
    const uint32_t status =
        (userIndex == 0 || anyUser) ? 0u : kXErrorDeviceNotConnected;

    if (n <= 8 || (n % 500) == 0)
        MCLA_LOG_INFO("XamUserGetDeviceContext[{}] ui={:08X} unk={:08X} -> "
                      "{:08X} (out word written 0 @ {:08X})",
                      n, userIndex, unk, status, guestVA);

    return status;
}

void XamInputKeystrokeEnqueue(const XAMINPUT_KEYSTROKE_HOST& ks)
{
    std::lock_guard<std::mutex> lock(g_ksLock);
    if (g_ksQueue.size() >= kKsQueueCap)
        g_ksQueue.pop_front();
    g_ksQueue.push_back(ks);
}

bool XamInputKeystrokeDequeue(XAMINPUT_KEYSTROKE_HOST* out)
{
    if (!out) return false;
    std::lock_guard<std::mutex> lock(g_ksLock);
    if (g_ksQueue.empty()) return false;
    *out = g_ksQueue.front();
    g_ksQueue.pop_front();
    return true;
}
