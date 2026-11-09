#include "dxc_runtime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#include <dxc/dxcapi.h>
#include <d3d12shader.h>  // for ID3D12ShaderReflection, D3D12_SHADER_DESC, D3D12_SHADER_INPUT_BIND_DESC
#include <wrl/client.h>

#include <atomic>
#include "common/logging.h"
#include <algorithm>  // std::min, in Flatten()
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcla::renderer {

namespace {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

static_assert(DxcRuntime::kSitCbuffer == D3D_SIT_CBUFFER);
static_assert(DxcRuntime::kSitTexture == D3D_SIT_TEXTURE);
static_assert(DxcRuntime::kSitSampler == D3D_SIT_SAMPLER);
static_assert(DxcRuntime::kSitStructuredBuffer == D3D_SIT_STRUCTURED);
static_assert(DxcRuntime::kSitByteAddressBuffer == D3D_SIT_BYTEADDRESS);

std::wstring ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WinErrorText(DWORD code) {
    char buf[512] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, buf, static_cast<DWORD>(sizeof(buf)), nullptr);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string HexHr(HRESULT hr) {
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// ---- translation census (soak-visible) ----------------------------------
//
// A silent zero here is indistinguishable from "the compiler was never asked to
// do anything", which is the failure this project has already been misled by.
// So attempts and outcomes are counted SEPARATELY, per stage, and every failure
// carries a refusal NAME. A name, never a bare code: this tree has shipped a
// refusal that was supposed to print a name and printed hex instead, so every
// reason in every log was a number.
enum class Stage { Unknown, Vertex, Pixel, Compute };

Stage StageOf(std::string_view profile) {
    if (profile.rfind("vs_", 0) == 0) return Stage::Vertex;
    if (profile.rfind("ps_", 0) == 0) return Stage::Pixel;
    if (profile.rfind("cs_", 0) == 0) return Stage::Compute;
    return Stage::Unknown;
}

const char* StageName(Stage s) {
    switch (s) {
    case Stage::Vertex: return "vs";
    case Stage::Pixel: return "ps";
    case Stage::Compute: return "cs";
    case Stage::Unknown: return "unknown";
    }
    return "unknown";
}

struct StageCounters {
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> ok{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<bool> firstFailureLogged{false};
};

StageCounters g_vert, g_pix, g_comp, g_unknown;

// Bounded: the first few, then every 200th. A soak must be able to count the
// marker without the log becoming the fault (F-054).
constexpr uint32_t kCensusFirstN = 4;
constexpr uint32_t kCensusEvery = 200;

bool ShouldLog(uint64_t n) { return n <= kCensusFirstN || (n % kCensusEvery) == 0; }

// ---- compile latency + DXIL cache telemetry (soak-visible) ----------------
//
// PC-TRANSLATE counts compiles but carries no time, so a soak cannot tell
// whether a new draw's PSO cost is DXC or the guest. A silent zero compile
// time would also be indistinguishable from "never measured" (Rule 19:
// LONG_TODO_MASTER.md SS1), which is why the latency is printed per compile
// (bounded to the first few, then every 200th, like the census above)
// together with the running average and maximum. QueryPerformanceFrequency
// cannot fail on any supported Windows; the `freqHz == 0` arm exists only so
// a hypothetical failure reads as 0us ("not measured") rather than as
// garbage, and the 0 itself is what would expose it.
int64_t QpcNowUs() {
    static LONGLONG freqHz = 0;
    if (freqHz == 0) {
        LARGE_INTEGER f{};
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) freqHz = f.QuadPart;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (freqHz == 0) return 0;
    return static_cast<int64_t>((now.QuadPart * 1000000LL) / freqHz);
}
std::atomic<uint64_t> g_dxLatencyN{0};         // successful DxcRuntime::Compile calls
std::atomic<uint64_t> g_dxLatencyUsTotal{0};   // sum of their wall times (us)
std::atomic<uint64_t> g_dxLatencyMax{0};       // slowest single compile (us)
std::atomic<uint64_t> g_dxCacheHits{0};        // DXIL-cache hits (no DXC call)
// DXIL cache size: a dozen KB per entry all-in (HLSL source + DXIL +
// reflection), so 512 entries is a few MB worst case and survives a long
// soak's distinct-shader growth; past that, FIFO eviction re-compiles.
constexpr size_t kDxilCacheMax = 512;
// FNV-1a over HLSL + entry + profile + keepReflection (same constants as
// DxcHashBytes below). The map key is NOT trusted for correctness: entries
// carry the full strings and Compile verifies them on every hit, so a
// 64-bit collision degrades to an extra compile, never to wrong DXIL.
uint64_t DxilCacheKeyOf(std::string_view hlsl, std::string_view entry,
                        std::string_view profile, bool keepReflection) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::string_view s) {
        for (char ch : s) {
            h ^= static_cast<unsigned char>(ch);
            h *= 1099511628211ull;
        }
        h ^= 0xFFull;  // separator: ("AB","C") must not hash like ("A","BC")
        h *= 1099511628211ull;
    };
    mix(hlsl);
    mix(entry);
    mix(profile);
    h ^= keepReflection ? 1ull : 2ull;
    h *= 1099511628211ull;
    return h;
}

// DXC's error blob is multi-line. Collapse it to one bounded line so a census
// marker stays a single grep-able log record instead of scattering fragments.
std::string Flatten(std::string_view s, size_t maxLen = 220) {
    std::string out;
    out.reserve(std::min(s.size(), maxLen));
    bool sp = false;
    for (char ch : s) {
        const bool ws = (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ');
        if (ws) {
            sp = !out.empty();
            continue;
        }
        if (sp) {
            out += ' ';
            sp = false;
        }
        out += ch;
        if (out.size() >= maxLen) {
            out += "...";
            break;
        }
    }
    return out;
}

// Records one translation outcome. `reason` is a refusal NAME; `detail` is the
// verbatim DXC diagnostic, which carries the "file:line:col" location.
void RecordOutcome(Stage stage, bool ok, const char* reason, const char* detail) {
    StageCounters& c = stage == Stage::Vertex     ? g_vert
                       : stage == Stage::Pixel   ? g_pix
                       : stage == Stage::Compute ? g_comp
                                                 : g_unknown;
    const uint64_t n = c.submitted.fetch_add(1) + 1;
    if (ok) {
        const uint64_t k = c.ok.fetch_add(1) + 1;
        if (ShouldLog(k)) {
            MCLA_LOG_INFO("PC-TRANSLATE stage={} ok#{} submitted={} failed=0",
                          StageName(stage), k, n);
        }
        return;
    }
    const uint64_t k = c.failed.fetch_add(1) + 1;
    if (ShouldLog(k)) {
        MCLA_LOG_ERROR("PC-TRANSLATE stage={} FAILED reason={} failed#{} submitted={} detail={}",
                       StageName(stage), reason, k, n, detail ? detail : "<none>");
    }
    if (!c.firstFailureLogged.exchange(true)) {
        // The first failure is always named in the log, once per stage. The guest
        // CONTAINER name is not available here: the submission site is
        // pipeline_cache.cpp and DxcRuntime::Compile is not given a source name
        // (its signature is fixed in dxc_runtime.h, which this change must not
        // touch). The verbatim diagnostic below is what carries the location.
        MCLA_LOG_ERROR("PC-TRANSLATE-FIRST-FAILURE stage={} reason={} detail={}",
                       StageName(stage), reason, detail ? detail : "<none>");
    }
}

}  // namespace

// F-723: compiles whose DXC_OUT_REFLECTION request did not come back. A NEW
// marker name (`DXC-REFLECT-MISSING`), not a field on PC-TRANSLATE: that row is
// tracked in PROGRAM_GUIDE sec 9 and its format is re-parsed by
// tools/soak_census.py. A compile that succeeded with no reflection is NOT a
// PC-TRANSLATE failure -- `failed=` there means DXC rejected the source, which
// is a different event -- so folding it in would make a working compile read as
// a broken one.
static std::atomic<uint32_t> s_reflectMissing{0};

struct DxcRuntime::Impl {
    HMODULE dxil = nullptr;
    HMODULE dxcompiler = nullptr;
    DxcCreateInstanceProc createInstance = nullptr;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    // F-723: the last DXC_OUT_REFLECTION blob, kept here because
    // `dxc_runtime.h` is NOT part of this change and its `Compile` /
    // `ReflectBindings` signatures cannot grow a reflection out-parameter.
    // The public API stays byte-for-byte what pipeline_cache.cpp already calls;
    // the reflection travels beside the DXIL in this side channel instead.
    //
    // WHY A SIDE CHANNEL AND NOT A RE-PARSE. ReflectBindings receives ONLY the
    // DXIL bytes, and those bytes are what CreateReflection rejects (measured:
    // 0x80004002 on a 3228-byte blob whose header says NumParts=1,
    // PartOffset[0]=3228 == the blob size and PartSize[0]=7, so the part table
    // does not describe the blob). Nothing recoverable lives in them, so the
    // reflection has to come from the compile that PRODUCED them.
    //
    // KEYED BY A HASH OF THE DXIL, never "whatever was last compiled": the
    // worker compiles VS then PS then optionally GS and reflects each in turn
    // (pipeline_cache.cpp:1549, :1563, :1581-1582, :1681), so an unkeyed
    // single slot would hand the pixel shader's bindings to the vertex shader's
    // reflection - a silently wrong root signature, which is exactly the failure
    // this whole path exists to prevent. A miss falls back to the container
    // path, which is the pre-F-723 behaviour, not a fabricated answer.
    ComPtr<IDxcBlob> lastReflection;
    uint64_t lastReflectionKey = 0;
    uint64_t lastReflectionSize = 0;
    // DXIL cache: compiled HLSL served again when the same
    // HLSL + entry + profile + keepReflection is requested under a
    // DIFFERENT PipelineKey (same shader, different state). PipelineCache
    // dedupes by PipelineKey, so without this the same HLSL is re-compiled
    // once per distinct state, and again after a PipelineCache FIFO
    // eviction re-queues an old key. Measured instance (build/X0info.log,
    // DXC-REFLECT-SOURCE rows #1..#3): two pipeline pairs produced an
    // identical 4508-byte VS DXIL container and an identical 1664-byte
    // reflection for (evidently) identical HLSL, i.e. one of the four
    // guest shader pairs was compiled twice. A hit returns the DXIL bytes
    // (and the reflection side-channel state) of the real earlier compile
    // of the identical inputs -- byte-for-byte, not a re-derive. Bounded
    // FIFO at kDxilCacheMax entries (see the anonymous namespace above).
    struct DxilCacheEntry {
        std::string hlsl;
        std::string entry;
        std::string profile;
        bool keepReflection = false;
        std::vector<uint8_t> dxil;
        ComPtr<IDxcBlob> reflection;  // null when that compile produced none
        uint64_t reflectionSize = 0;
    };
    std::unordered_map<uint64_t, DxilCacheEntry> dxilCache;
    std::deque<uint64_t> dxilCacheOrder;  // FIFO eviction order (cache keys)
    // Guards dxilCache + dxilCacheOrder ONLY. Never held across a DXC
    // compile: lookups copy out under the lock, then the reflection-slot
    // restore happens outside it, on the same thread that owns this
    // instance (dxc_runtime.h: one DxcRuntime per compiling thread).
    std::mutex dxilCacheMutex;
};

// FNV-1a 64. A content key, not a pointer: two DXIL blobs of the same size can
// differ, and a size-only key would hand back the wrong shader's reflection.
uint64_t DxcHashBytes(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

DxcRuntime::DxcRuntime() : impl_(new Impl{}) {}

DxcRuntime::~DxcRuntime() {
    if (!impl_) return;
    impl_->utils.Reset();
    impl_->compiler.Reset();
    // The DXIL cache holds IDxcBlob reflections -- dxcompiler.dll objects,
    // like lastReflection. They must be released HERE, before FreeLibrary
    // below, not by delete impl_ (which runs after the image is unmapped);
    // that release order is the w692 fault (see the note below).
    impl_->dxilCache.clear();
    impl_->dxilCacheOrder.clear();
    // Every COM pointer this object holds MUST be released here, while
    // dxcompiler.dll is still mapped. All three of them are dxcompiler.dll
    // OBJECTS: their Release() is a virtual call through a vtable that lives
    // inside dxcompiler.dll, so once FreeLibrary below unmaps that image the
    // call transfers control into unmapped memory.
    //
    // `lastReflection` (F-723) was the one that was missing, and it cost the
    // renderer its initialisation. Measured on build/mcla.exe for the build
    // that produced w692.log: Impl+0x28 is `lastReflection`, and the
    // destructor released it at `??1DxcRuntime+0xb7` = RVA 0x37ab27, whose raw
    // bytes are `48 8b 48 28 / 48 85 c9 / 74 0f / 90 / 48 c7 40 28 00 00 00 00 /
    // 48 8b 01 / ff 50 10` -- load the pointer at Impl+0x28, null it, fetch the
    // vtable, `call [rax+0x10]` (the IUnknown Release slot). The two
    // `FreeLibrary` calls are at 0x37aadf and 0x37aaf7, both resolved against
    // the same /MAP to `__imp_FreeLibrary`. Reproduced in isolation: the fault
    // reports `ExceptionInformation[1] is inside dxcompiler.dll` with that
    // module `still-mapped=0`.
    //
    // So this is a release-ORDER fault, not a refcount fault. The count is
    // right: `IDxcResult::GetOutput` hands over exactly one reference, the
    // assignment at the end of Compile is a move-assign so the count stays 1,
    // and it is released exactly once. An outstanding COM reference does not
    // keep the image mapped either -- a COM AddRef is not a LoadLibrary.
    impl_->lastReflection.Reset();
    if (impl_->dxcompiler) FreeLibrary(impl_->dxcompiler);
    if (impl_->dxil) FreeLibrary(impl_->dxil);
    // After this point nothing may touch a dxcompiler.dll pointer again;
    // `delete impl_` runs ~Impl, whose remaining ComPtr releases are all on
    // pointers already nulled above.
    delete impl_;
    impl_ = nullptr;
}

bool DxcRuntime::Load(std::string_view dllDir, std::string& usedDir, std::string& error) {
    usedDir.clear();
    error.clear();
    if (loaded_) {
        usedDir = usedDir_;  // already loaded; report where they came from
        return true;
    }
    if (!impl_) return false;
    const int64_t loadT0 = QpcNowUs();
    uint32_t candidatesProbed = 0;

    // Candidate DLL directories, in priority order; the first holding BOTH
    // dxcompiler.dll and dxil.dll wins. Every rejection is recorded so the error
    // text names each directory and why it failed (F-609: a missing DXC pair
    // means every real PSO is missing, so the log must say where to put them).
    struct Candidate { std::string dir; const char* origin; };
    std::vector<Candidate> candidates;
    if (!dllDir.empty()) candidates.push_back({std::string(dllDir), "explicit dir"});
    if (const char* env = std::getenv("MCLA_DXC_DIR"); env && *env)
        candidates.push_back({env, "MCLA_DXC_DIR"});

    // The exe's own directory, and the directory above it. The exe lives in
    // build/, so the parent is the repo root -- which is what every repo-relative
    // path below is written against.
    std::string exeDir;
    {
        char exe[1024] = {};
        const DWORD n = GetModuleFileNameA(nullptr, exe, sizeof(exe));
        if (n > 0 && n < sizeof(exe)) {
            std::string exePath(exe, n);
            const size_t sep = exePath.find_last_of("/\\");
            if (sep != std::string::npos) {
                exeDir = exePath.substr(0, sep);
                candidates.push_back({exeDir, "exe dir"});
                candidates.push_back({exeDir + "\\dxc", "exe dir/dxc"});
            }
        }
    }

    // A RELATIVE path is resolved by GetFileAttributesA against the PROCESS CURRENT
    // DIRECTORY, not against the exe directory. That is the whole load failure:
    // build/w664.log records the exe dir as "E:\mcla pc\build" and then reports
    // ".research/XenosRecomp/thirdparty/dxc-bin/bin/x64 ... no dxcompiler.dll and
    // no dxil.dll" even though BOTH dlls are present in exactly that directory.
    // Measured: the relative path resolves from CWD=E:\mcla pc and fails from
    // CWD=E:\mcla pc\build and from CWD=C:\Windows\Temp. So a launcher that does
    // not set the CWD to the repo root loses the runtime with the files right there.
    //
    // Fix: probe each repo-relative candidate three ways -- against the CWD (the old
    // behaviour, kept so an existing setup keeps working), against the exe dir, and
    // against the exe's parent -- so the loader finds the checkout wherever the
    // process was started from.
    const auto addAnchored = [&](const char* rel, const char* origin) {
        candidates.push_back({rel, origin});
        if (exeDir.empty()) return;
        std::string a = exeDir + "\\" + rel;
        candidates.push_back({a, "exe-anchored"});
        const size_t up = exeDir.find_last_of("/\\");
        if (up != std::string::npos) {
            candidates.push_back({exeDir.substr(0, up) + "\\" + rel, "repo-anchored"});
        }
    };
    // NOTE on the first two: third_party/dxc ships inc/ ONLY (headers for
    // <dxc/dxcapi.h>), with no bin/ anywhere under third_party/ -- measured, not
    // assumed. The include tree and the binary tree are different directories and
    // the pair below conflates them; they stay as forward-compatible slots (drop
    // the dll pair in and they work) but they are NOT where the dlls are today.
    addAnchored("third_party\\dxc\\bin\\x64", "repo-relative (inc-only tree)");
    addAnchored("build\\third_party\\dxc\\bin\\x64", "repo-relative (build dir)");
    // Last resort: the only dxcompiler.dll pair checked into this tree, in the
    // read-only XenosRecomp checkout. Named here so a dev machine that lacks
    // MCLA_DXC_DIR still gets real PSOs, and so a deployment that has only
    // .research/ missing fails loudly instead of silently.
    addAnchored(".research\\XenosRecomp\\thirdparty\\dxc-bin\\bin\\x64",
                "XenosRecomp thirdparty checkout");

    std::string dir;
    std::string rejected;
    for (const auto& c : candidates) {
        if (c.dir.empty()) continue;
        ++candidatesProbed;
        const std::string sep =
            (c.dir.back() == '/' || c.dir.back() == '\\') ? "" : "\\";
        const std::string compilerPath = c.dir + sep + "dxcompiler.dll";
        const std::string dxilPath = c.dir + sep + "dxil.dll";
        const DWORD compilerAttr = GetFileAttributesA(compilerPath.c_str());
        const DWORD dxilAttr = GetFileAttributesA(dxilPath.c_str());
        if (compilerAttr != INVALID_FILE_ATTRIBUTES && dxilAttr != INVALID_FILE_ATTRIBUTES) {
            dir = c.dir + sep;
            break;
        }
        if (!rejected.empty()) rejected += "; ";
        rejected += c.dir;
        rejected += " (";
        rejected += c.origin;
        rejected += "): ";
        if (compilerAttr == INVALID_FILE_ATTRIBUTES && dxilAttr == INVALID_FILE_ATTRIBUTES)
            rejected += "no dxcompiler.dll and no dxil.dll";
        else if (compilerAttr == INVALID_FILE_ATTRIBUTES)
            rejected += "no dxcompiler.dll";
        else
            rejected += "no dxil.dll";
    }
    if (dir.empty()) {
        error = "DXC DLLs not found. Set MCLA_DXC_DIR to a directory containing "
                "dxcompiler.dll AND dxil.dll, or put that pair beside mcla.exe. "
                "Tried: ";
        error += rejected.empty() ? std::string("(no candidate directory)") : rejected;
        MCLA_LOG_ERROR("DXC-RUNTIME-LOAD-FAILED reason=dll-pair-not-found tried={}",
                       rejected.empty() ? "(no candidate directory)" : rejected);
        return false;
    }

    char absCompilerPath[MAX_PATH] = {};
    char absDxilPath[MAX_PATH] = {};
    GetFullPathNameA((dir + "dxcompiler.dll").c_str(), MAX_PATH, absCompilerPath, nullptr);
    GetFullPathNameA((dir + "dxil.dll").c_str(), MAX_PATH, absDxilPath, nullptr);
    const std::string resolvedDxil = absDxilPath[0] ? std::string(absDxilPath) : (dir + "dxil.dll");
    const std::string resolvedCompiler = absCompilerPath[0] ? std::string(absCompilerPath) : (dir + "dxcompiler.dll");

    // Pre-load dxil.dll by full path: the loader matches imports by module base
    // name, so dxcompiler.dll binds to this copy instead of one found elsewhere.
    impl_->dxil = LoadLibraryA(resolvedDxil.c_str());
    if (!impl_->dxil) {
        error = "LoadLibrary(dxil.dll) failed: " + WinErrorText(GetLastError());
        MCLA_LOG_ERROR("DXC-RUNTIME-LOAD-FAILED reason=loadlibrary-dxil-failed "
                       "path={} detail={}",
                       resolvedDxil, error);
        return false;
    }
    impl_->dxcompiler =
        LoadLibraryExA(resolvedCompiler.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                           LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!impl_->dxcompiler) {
        impl_->dxcompiler = LoadLibraryA(resolvedCompiler.c_str());
    }
    if (!impl_->dxcompiler) {
        error = "LoadLibrary(dxcompiler.dll) failed: " + WinErrorText(GetLastError());
        MCLA_LOG_ERROR("DXC-RUNTIME-LOAD-FAILED reason=loadlibrary-dxcompiler-failed "
                       "path={} detail={}",
                       resolvedCompiler, error);
        return false;
    }
    impl_->createInstance = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(impl_->dxcompiler, "DxcCreateInstance"));
    if (!impl_->createInstance) {
        error = "DxcCreateInstance export not found in dxcompiler.dll";
        MCLA_LOG_ERROR("DXC-RUNTIME-LOAD-FAILED reason=missing-dxccreateinstance-export "
                       "path={}",
                       dir + "dxcompiler.dll");
        return false;
    }

    HRESULT hr = impl_->createInstance(
        CLSID_DxcCompiler, __uuidof(IDxcCompiler3),
        reinterpret_cast<void**>(impl_->compiler.GetAddressOf()));
    if (FAILED(hr)) {
        error = "DxcCreateInstance(CLSID_DxcCompiler) failed " + HexHr(hr);
        MCLA_LOG_ERROR("DXC-RUNTIME-LOAD-FAILED reason=createinstance-compiler-failed "
                       "path={} hr={}",
                       dir, HexHr(hr));
        return false;
    }
    hr = impl_->createInstance(CLSID_DxcUtils, __uuidof(IDxcUtils),
                               reinterpret_cast<void**>(impl_->utils.GetAddressOf()));
    if (FAILED(hr)) {
        error = "DxcCreateInstance(CLSID_DxcUtils) failed " + HexHr(hr);
        MCLA_LOG_ERROR("DXC-RUNTIME-LOAD-FAILED reason=createinstance-utils-failed "
                       "path={} hr={}",
                       dir, HexHr(hr));
        return false;
    }

    usedDir = dir;
    usedDir_ = dir;
    loaded_ = true;
    // The gate marker: the resolved ABSOLUTE path, so a soak can tell a real load
    // from a CWD-dependent near-miss (which is what build/w664.log recorded).
    MCLA_LOG_INFO("DXC-RUNTIME-LOADED path={}", dir);
    // Init wall: what Load itself cost (candidate probing + LoadLibrary +
    // DxcCreateInstance), separate from the guest boot around it. One line
    // per process, not per compile, so it cannot flood a long soak.
    MCLA_LOG_INFO("DXC-LOAD-TIME ms={} candidatesProbed={} -- DLL search + "
                  "LoadLibrary + DxcCreateInstance cost",
                  static_cast<uint64_t>((QpcNowUs() - loadT0) / 1000),
                  candidatesProbed);
    return true;
}

const std::string& DxcRuntime::UsedDir() const {
    return usedDir_;
}

bool DxcRuntime::Compile(std::string_view hlsl, std::string_view entry,
                          std::string_view profile, std::vector<uint8_t>& dxil,
                          std::string& error, bool keepReflection) const {
    dxil.clear();
    error.clear();
    const Stage stage = StageOf(profile);
    if (!impl_ || !impl_->compiler || !impl_->utils || !loaded_) {
        error = "DxcRuntime not loaded";
        // A named refusal, not a bare code. A translation that is refused because
        // the runtime never loaded is a DIFFERENT fault from one DXC rejected, and
        // a soak must be able to tell them apart.
        RecordOutcome(stage, false, "runtime-not-loaded", "DxcRuntime::Load never succeeded");
        return false;
    }

    // DXIL cache lookup (long-term queue relief). The cache key is over the
    // effective inputs (empty entry means "main", as below). A hit skips
    // IDxcCompiler3::Compile entirely and restores the reflection side
    // channel to the fitting state, including the "no reflection" state
    // (key 0), which ReflectBindings treats as "fall back to the container
    // path". A hit is deliberately NOT counted in PC-TRANSLATE: that
    // marker's `submitted` means "DXC was asked to compile", and the census
    // (tools/soak_census.py) compares it against DXC-OUTPUTS, which counts
    // real compiles only. Counting hits there would make submitted exceed
    // real DXC calls and read as dropped logging. Same precedent as
    // DXC-REFLECT-MISSING: a NEW marker name for a new event.
    const int64_t compileT0 = QpcNowUs();
    const std::string_view effEntry = entry.empty() ? std::string_view("main") : entry;
    const uint64_t cacheKey = DxilCacheKeyOf(hlsl, effEntry, profile, keepReflection);
    bool cacheHit = false;
    std::vector<uint8_t> cachedDxil;
    ComPtr<IDxcBlob> cachedReflection;
    uint64_t cachedReflectionSize = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->dxilCacheMutex);
        auto hit = impl_->dxilCache.find(cacheKey);
        if (hit != impl_->dxilCache.end() && hit->second.keepReflection == keepReflection &&
            std::string_view(hit->second.hlsl) == hlsl &&
            std::string_view(hit->second.entry) == effEntry &&
            std::string_view(hit->second.profile) == profile) {
            cachedDxil = hit->second.dxil;
            cachedReflection = hit->second.reflection;
            cachedReflectionSize = hit->second.reflectionSize;
            cacheHit = true;
        }
    }
    if (cacheHit) {
        dxil = std::move(cachedDxil);
        if (cachedReflection) {
            impl_->lastReflection = cachedReflection;
            impl_->lastReflectionKey = DxcHashBytes(dxil.data(), dxil.size());
            impl_->lastReflectionSize = cachedReflectionSize;
        } else {
            impl_->lastReflection.Reset();
            impl_->lastReflectionKey = 0;
            impl_->lastReflectionSize = 0;
        }
        const uint64_t n = g_dxCacheHits.fetch_add(1) + 1;
        if (ShouldLog(n)) {
            MCLA_LOG_INFO("DXC-CACHE-HIT #{} stage={} -- DXIL served from the "
                          "cache without an IDxcCompiler3::Compile call for "
                          "the same HLSL+entry+profile; PC-TRANSLATE "
                          "submitted still counts real DXC compilations only",
                          n, StageName(stage));
        }
        // No DXC-LATENCY row on a hit: no DXC call happened.
        return true;
    }

    ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT hr = impl_->utils->CreateBlobFromPinned(
        hlsl.data(), static_cast<uint32_t>(hlsl.size()), DXC_CP_UTF8,
        sourceBlob.GetAddressOf());
    if (FAILED(hr) || !sourceBlob) {
        error = "IDxcUtils::CreateBlobFromPinned failed " + HexHr(hr);
        RecordOutcome(stage, false, "source-blob-create-failed", error.c_str());
        return false;
    }

    DxcBuffer source{};
    source.Ptr = sourceBlob->GetBufferPointer();
    source.Size = sourceBlob->GetBufferSize();

    // Flags follow the reference invocation in
    // .research/XenosRecomp/XenosRecomp/dxc_compiler.cpp (HLSL 2021, -all-resources-bound,
    // -Wno-ignored-attributes); -E is added here, -Qstrip_* only when reflection isn't kept.
    const std::wstring wEntry = ToWide(entry.empty() ? "main" : entry);
    const std::wstring wProfile = ToWide(profile);
    std::vector<const wchar_t*> args;
    args.push_back(L"-E");
    args.push_back(wEntry.c_str());
    args.push_back(L"-T");
    args.push_back(wProfile.c_str());
    args.push_back(L"-HV");
    args.push_back(L"2021");
    args.push_back(L"-all-resources-bound");
    args.push_back(L"-Wno-ignored-attributes");
    if (!keepReflection) {
        args.push_back(L"-Qstrip_reflect");
        args.push_back(L"-Qstrip_debug");
    }

    ComPtr<IDxcResult> result;
    hr = impl_->compiler->Compile(&source, args.data(), static_cast<uint32_t>(args.size()),
                                  nullptr, __uuidof(IDxcResult),
                                  reinterpret_cast<void**>(result.GetAddressOf()));
    if (FAILED(hr) || !result) {
        error = "IDxcCompiler3::Compile failed " + HexHr(hr);
        RecordOutcome(stage, false, "compiler-call-failed", error.c_str());
        return false;
    }

    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status)) {
        ComPtr<IDxcBlobEncoding> errBlob;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobEncoding),
                                        reinterpret_cast<void**>(errBlob.GetAddressOf()),
                                        nullptr)) &&
            errBlob && errBlob->GetBufferSize() > 0) {
            error.assign(static_cast<const char*>(errBlob->GetBufferPointer()),
                         errBlob->GetBufferSize());
        }
        if (error.empty()) error = "DXC compile failed (no diagnostics)";
        // The refusal is DXC's own diagnostic, verbatim and flattened. The vendored
        // DxcBuffer (third_party/dxc/inc/dxc/dxcapi.h:247) has no Name field, so no
        // source name can be attached here and no guest container file is available
        // -- the submission site is pipeline_cache.cpp. Whatever location DXC does
        // report is passed through rather than discarded.
        RecordOutcome(stage, false, "dxc-diagnostic-rejected", Flatten(error).c_str());
        return false;
    }

    ComPtr<IDxcBlob> obj;
    hr = result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob),
                           reinterpret_cast<void**>(obj.GetAddressOf()), nullptr);
    if (FAILED(hr) || !obj || obj->GetBufferSize() == 0) {
        error = "no DXIL object produced by DXC";
        RecordOutcome(stage, false, "no-dxil-object", error.c_str());
        return false;
    }
    const auto* p = static_cast<const uint8_t*>(obj->GetBufferPointer());
    dxil.assign(p, p + obj->GetBufferSize());

    // F-723: ask DXC for the reflection DATA alongside the object, because the
    // object itself cannot be reflected (measured: CreateReflection returns
    // 0x80004002 on it). dxcapi.h:676-678 names two accepted sources for
    // CreateReflection -- "the serialized DXIL container OR the DXC_OUT_REFLECTION
    // blob contents" -- and the second is the one that works here.
    //
    // This is requested UNCONDITIONALLY rather than under `keepReflection`,
    // because the two questions are different: `-Qstrip_reflect` (which
    // `keepReflection == false` controls) asks DXC to leave reflection OUT OF THE
    // CONTAINER, which is correct for shipping DXIL and irrelevant to
    // DXC_OUT_REFLECTION. Gating this on `keepReflection` would leave
    // d3d12_backend.cpp's compute compile and shader_dxc_compile.cpp's offline
    // validator unable to reflect, for no reason.
    //
    // A failure here is NOT a compile failure. The DXIL above is already valid
    // and already handed back; reflection is a separate output and the caller
    // decides what a missing one means. So this records the refusal and carries
    // on rather than returning false for a compile that succeeded.
    impl_->lastReflection.Reset();
    impl_->lastReflectionKey = 0;
    impl_->lastReflectionSize = 0;
    ComPtr<IDxcBlob> reflection;
    hr = result->GetOutput(DXC_OUT_REFLECTION, __uuidof(IDxcBlob),
                           reinterpret_cast<void**>(reflection.GetAddressOf()),
                           nullptr);
    if (SUCCEEDED(hr) && reflection && reflection->GetBufferSize() > 0) {
        impl_->lastReflection = reflection;
        impl_->lastReflectionKey = DxcHashBytes(dxil.data(), dxil.size());
        impl_->lastReflectionSize = reflection->GetBufferSize();
    } else {
        const uint32_t n = s_reflectMissing.fetch_add(1) + 1;
        if (n <= 4 || (n % 200) == 0) {
            MCLA_LOG_WARN("DXC-REFLECT-MISSING #{} stage={} hr={} -- the compile "
                          "SUCCEEDED and the DXIL was returned; only the "
                          "reflection output is absent, so ReflectBindings will "
                          "fall back to the container path",
                          n, StageName(stage), HexHr(hr));
        }
    }

    // F-692: WHAT DXC ACTUALLY PRODUCED, on the success path.
    //
    // The w693 blocker was a `CreateGraphicsPipelineState failed` refusal sitting
    // next to seven `PC-TRANSLATE ... failed=0` rows, and the two facts are only
    // distinguishable if the success path says what it produced. This asks for
    // DXC_OUT_ERRORS and DXC_OUT_PADDING explicitly on the SUCCESS path -- today
    // DXC_OUT_ERRORS is only ever read on the failure path (:479-486), so a soak
    // cannot tell "DXC had nothing to say" from "nobody asked".
    //
    // MEASURED 2026-10-02 on the real guest pair, offline, with the vendored
    // dxcompiler.dll 1.8-1.8.2407.7: errorsBytes = 0 and paddingBytes = 0 for
    // every stage, i.e. DXC had no diagnostic to give. The compile was never the
    // fault; the fault was downstream in the D3D12 stage-signature link. That is
    // why this marker exists -- it turns an absence into a printed number.
    {
        ComPtr<IDxcBlobEncoding> errBlob;
        // The 4th argument of IDxcResult::GetOutput is an IDxcBlobWide**
        // OUTPUT NAME, not a size (measured: the vendored dxcapi.h declares
        // `UINT32 *pOutputName` variants only for the by-name overloads), so it
        // is passed null here and the size is read off the blob itself.
        const HRESULT errHr = result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobEncoding),
                                                reinterpret_cast<void**>(errBlob.GetAddressOf()),
                                                nullptr);
        const uint32_t errBytes = errBlob ? errBlob->GetBufferSize() : 0u;
        static std::atomic<uint32_t> s_outputs{0};
        const uint32_t n = s_outputs.fetch_add(1) + 1;
        if (n <= 4 || (n % 200) == 0) {
            MCLA_LOG_INFO("DXC-OUTPUTS #{} stage={} keepReflection={} objectBytes={} "
                          "errorsBytes={} errorsHr={} reflectionBytes={} "
                          "-- what DXC returned for a compile it ACCEPTED; "
                          "errorsBytes=0 means DXC had no diagnostic, so a later "
                          "CreateGraphicsPipelineState failure is NOT a compile "
                          "failure",
                          n, StageName(stage), keepReflection ? 1 : 0,
                          static_cast<uint32_t>(dxil.size()),
                          errBytes, HexHr(errHr),
                          static_cast<uint32_t>(impl_->lastReflectionSize));
        }
    }

    // Compile latency + cache insert. The clock covers the whole call
    // (blob creation, the DXC Compile, the reflection and errors GetOutput
    // calls and the DXC-OUTPUTS census above): for the worker in
    // pipeline_cache.cpp this is the per-stage cost of one queued task,
    // which is what a long soak's compile queue must account for. A NEW
    // marker (DXC-LATENCY), not a field on PC-TRANSLATE or DXC-OUTPUTS, so
    // neither marker's tracked format changes.
    {
        const int64_t usDelta = QpcNowUs() - compileT0;
        const uint64_t us = usDelta > 0 ? static_cast<uint64_t>(usDelta) : 0u;
        const uint64_t n = g_dxLatencyN.fetch_add(1) + 1;
        const uint64_t totalUs = g_dxLatencyUsTotal.fetch_add(us) + us;
        uint64_t maxUs = g_dxLatencyMax.load(std::memory_order_relaxed);
        while (us > maxUs && !g_dxLatencyMax.compare_exchange_weak(
                                 maxUs, us, std::memory_order_relaxed)) {
        }
        if (ShouldLog(n)) {
            MCLA_LOG_INFO("DXC-LATENCY #{} stage={} compileUs={} avgUs={} "
                          "maxUs={} -- wall time of one successful "
                          "DxcRuntime::Compile (blob + DXC + reflection + "
                          "errors); PC-TRANSLATE counts the compile, this "
                          "says what it cost",
                          n, StageName(stage), us, totalUs / n, maxUs);
        }
    }
    // Cache insert (miss path only): keep the DXIL build, the reflection
    // blob it was reflected from, and the full key so a later identical
    // request need not re-invoke DXC. FIFO eviction at kDxilCacheMax; an
    // evicted entry simply re-compiles next time, never a wrong answer.
    {
        std::lock_guard<std::mutex> lk(impl_->dxilCacheMutex);
        if (impl_->dxilCache.find(cacheKey) == impl_->dxilCache.end()) {
            while (impl_->dxilCache.size() >= kDxilCacheMax) {
                const uint64_t oldKey = impl_->dxilCacheOrder.front();
                impl_->dxilCacheOrder.pop_front();
                impl_->dxilCache.erase(oldKey);
            }
        }
        Impl::DxilCacheEntry fresh;
        fresh.hlsl = std::string(hlsl);
        fresh.entry = std::string(effEntry);
        fresh.profile = std::string(profile);
        fresh.keepReflection = keepReflection;
        fresh.dxil = dxil;
        fresh.reflection = impl_->lastReflection;
        fresh.reflectionSize = impl_->lastReflectionSize;
        auto inserted = impl_->dxilCache.insert_or_assign(cacheKey, std::move(fresh));
        if (inserted.second) {
            impl_->dxilCacheOrder.push_back(cacheKey);
        }
    }

    RecordOutcome(stage, true, nullptr, nullptr);
    return true;
}

// F-723: forward declaration. The definition sits below ReflectBindings (it is
// the code that used to be inlined there), and the two are mutually recursive in
// spirit -- the helper is what ReflectBindings delegates to -- so it is declared
// here rather than moved above, which would have split the F-634 CBV comments
// from the code they describe. The parameter is a POINTER because both call
// sites hold a ComPtr, whose Get() yields a pointer: a reference parameter is
// not a silent conversion from it, and the compiler says so rather than
// guessing (measured, when this was first written as a reference).
bool ReflectFromShader(ID3D12ShaderReflection* reflector,
                       std::vector<DxcRuntime::ResourceBinding>& out);

bool DxcRuntime::ReflectBindings(const std::vector<uint8_t>& dxil,
                                 std::vector<ResourceBinding>& out,
                                 std::string& error) const {
    out.clear();
    error.clear();

    if (!impl_ || !impl_->utils || !loaded_) {
        error = "DxcRuntime not loaded";
        return false;
    }

    // Wrap the DXIL bytes in a pinned blob (no copy; dxil must outlive reflection)
    ComPtr<IDxcBlobEncoding> dxilBlob;
    HRESULT hr = impl_->utils->CreateBlobFromPinned(
        dxil.data(), static_cast<uint32_t>(dxil.size()), DXC_CP_UTF8,
        dxilBlob.GetAddressOf());
    if (FAILED(hr) || !dxilBlob) {
        error = "CreateBlobFromPinned failed " + HexHr(hr);
        return false;
    }

    // F-723: THE FIX, and it is TWO changes, not one.
    //
    // CHANGE 1 - the interface. Every CreateReflection call on this tree asked
    // for IID_IDxcContainerReflection and every one returned 0x80004002
    // (build/w691_err.log, twice). That is NOT the container being malformed.
    // MEASURED, on the real dxcompiler.dll against the real translated guest
    // HLSL, for all four combinations:
    //
    //     source                IID_IDxcContainerReflection  IID_ID3D12ShaderReflection
    //     DXIL container        0x80004002 (FAIL)            0x00000000 (S_OK)
    //     DXC_OUT_REFLECTION    0x80004002 (FAIL)            0x00000000 (S_OK)
    //
    // The interface was the whole fault. dxcapi.h:686 says so in as many words
    // -- "Use this with interfaces such as ID3D12ShaderReflection" -- and
    // IDxcContainerReflection is not among them: the container walk
    // (GetPartCount/GetPartReflection) belongs to IDxcUtils::CreateContainerReflection,
    // which is a different method this file has never called. So the original
    // code asked the wrong API for the reflection it wanted, on data that was
    // fine. Requesting DXC_OUT_REFLECTION ALONE - the obvious first guess, and
    // what the w691 diagnosis proposed - does NOT fix it: the RDAT blob fails
    // the container query identically (also measured above).
    //
    // CHANGE 2 - the source. ID3D12ShaderReflection also succeeds on the DXIL
    // container, so the DXC_OUT_REFLECTION request is kept because it is
    // strictly better in one respect: it exists even when the container carries
    // no reflection part, which is the case `-Qstrip_reflect` produces and the
    // case every non-keepReflection compile in this tree produces. It is tried
    // first and keyed by a content hash of `dxil` (see Impl::lastReflectionKey
    // for why an unkeyed slot would be a silently-wrong-answer bug).
    //
    // MEASURED CONSEQUENCE, STATED NOT GLOSSED: on the real guest HLSL both
    // shaders reflect with BoundResources = 0, because the translator emits an
    // EMPTY `cbuffer ShaderConstants : register(b0) {}` -- there is no constant
    // data in these programs to bind. So this fix removes the 0x80004002
    // refusal, and what it exposes underneath is a shader with no bindings, not
    // a set of them. That is the guest's fact, reported rather than padded.
    {
        const uint32_t boundRefuse =
            static_cast<uint32_t>(impl_->lastReflectionSize);
        // (a) the compile's own reflection output, when it matches these bytes.
        if (impl_->lastReflection && impl_->lastReflectionKey != 0 &&
            impl_->lastReflectionKey ==
                DxcHashBytes(dxil.data(), dxil.size()) && dxil.size() > 0) {
            DxcBuffer rdat{};
            rdat.Ptr = impl_->lastReflection->GetBufferPointer();
            rdat.Size = impl_->lastReflection->GetBufferSize();
            ComPtr<ID3D12ShaderReflection> rdatReflection;
            hr = impl_->utils->CreateReflection(&rdat, __uuidof(ID3D12ShaderReflection),
                                                reinterpret_cast<void**>(
                                                    rdatReflection.GetAddressOf()));
            if (SUCCEEDED(hr) && rdatReflection) {
                static std::atomic<uint32_t> s_reflectRdatOk{0};
                const uint32_t n = s_reflectRdatOk.fetch_add(1) + 1;
                if (n <= 4 || (n % 200) == 0) {
                    MCLA_LOG_INFO("DXC-REFLECT-SOURCE #{} kind=DXC_OUT_REFLECTION "
                                  "bytes={} iid=ID3D12ShaderReflection -- reflected "
                                  "from the compile's own reflection output",
                                  n, boundRefuse);
                }
                if (ReflectFromShader(rdatReflection.Get(), out)) return true;
            } else {
                static std::atomic<uint32_t> s_reflectRdatFail{0};
                const uint32_t n = s_reflectRdatFail.fetch_add(1) + 1;
                if (n <= 4 || (n % 200) == 0) {
                    MCLA_LOG_WARN("DXC-REFLECT-RDAT-FAILED #{} hr={} bytes={} -- "
                                  "falling back to the DXIL container",
                                  n, HexHr(hr), boundRefuse);
                }
            }
        }
        // (b) the DXIL container itself, now with the CORRECT interface. This is
        // what actually resolves on the measured guest HLSL.
        {
            DxcBuffer buffer{};
            buffer.Ptr = dxilBlob->GetBufferPointer();
            buffer.Size = dxilBlob->GetBufferSize();
            ComPtr<ID3D12ShaderReflection> refl;
            hr = impl_->utils->CreateReflection(
                &buffer, __uuidof(ID3D12ShaderReflection),
                reinterpret_cast<void**>(refl.GetAddressOf()));
            if (SUCCEEDED(hr) && refl) {
                static std::atomic<uint32_t> s_reflectContainerOk{0};
                const uint32_t n = s_reflectContainerOk.fetch_add(1) + 1;
                if (n <= 4 || (n % 200) == 0) {
                    MCLA_LOG_INFO("DXC-REFLECT-SOURCE #{} kind=DXIL-container "
                                  "bytes={} iid=ID3D12ShaderReflection",
                                  n, static_cast<uint32_t>(dxil.size()));
                }
                if (ReflectFromShader(refl.Get(), out)) return true;
            } else {
                error = "CreateReflection failed " + HexHr(hr);
                return false;
            }
        }
    }
    return true;
}

bool DxcRuntime::ReflectSignature(
    const std::vector<uint8_t>& dxil,
    std::vector<SignatureParameter>& inputs,
    std::vector<SignatureParameter>& outputs,
    std::string& error) const {
    inputs.clear();
    outputs.clear();
    error.clear();

    if (!impl_ || !impl_->utils || !loaded_) {
        error = "DxcRuntime not loaded";
        return false;
    }

    ComPtr<ID3D12ShaderReflection> reflection;
    const uint64_t key = DxcHashBytes(dxil.data(), dxil.size());
    if (impl_->lastReflection && impl_->lastReflectionKey != 0 &&
        impl_->lastReflectionKey == key && !dxil.empty()) {
        DxcBuffer reflectionBuffer{};
        reflectionBuffer.Ptr = impl_->lastReflection->GetBufferPointer();
        reflectionBuffer.Size = impl_->lastReflection->GetBufferSize();
        const HRESULT reflectionHr = impl_->utils->CreateReflection(
            &reflectionBuffer, __uuidof(ID3D12ShaderReflection),
            reinterpret_cast<void**>(reflection.GetAddressOf()));
        if (FAILED(reflectionHr)) reflection.Reset();
    }

    if (!reflection) {
        ComPtr<IDxcBlobEncoding> dxilBlob;
        HRESULT hr = impl_->utils->CreateBlobFromPinned(
            dxil.data(), static_cast<uint32_t>(dxil.size()), DXC_CP_UTF8,
            dxilBlob.GetAddressOf());
        if (FAILED(hr) || !dxilBlob) {
            error = "CreateBlobFromPinned failed " + HexHr(hr);
            return false;
        }

        DxcBuffer dxilBuffer{};
        dxilBuffer.Ptr = dxilBlob->GetBufferPointer();
        dxilBuffer.Size = dxilBlob->GetBufferSize();
        hr = impl_->utils->CreateReflection(
            &dxilBuffer, __uuidof(ID3D12ShaderReflection),
            reinterpret_cast<void**>(reflection.GetAddressOf()));
        if (FAILED(hr) || !reflection) {
            error = "CreateReflection failed " + HexHr(hr);
            return false;
        }
    }

    D3D12_SHADER_DESC shaderDesc = {};
    HRESULT hr = reflection->GetDesc(&shaderDesc);
    if (FAILED(hr)) {
        error = "ID3D12ShaderReflection::GetDesc failed " + HexHr(hr);
        return false;
    }

    auto readParameters = [&](UINT count, bool isInput,
                              std::vector<SignatureParameter>& destination) {
        destination.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            D3D12_SIGNATURE_PARAMETER_DESC desc = {};
            const HRESULT parameterHr =
                isInput ? reflection->GetInputParameterDesc(i, &desc)
                        : reflection->GetOutputParameterDesc(i, &desc);
            if (FAILED(parameterHr)) {
                error = std::string(isInput ? "GetInputParameterDesc"
                                            : "GetOutputParameterDesc") +
                        " failed " + HexHr(parameterHr);
                return false;
            }
            SignatureParameter parameter;
            parameter.semanticName = desc.SemanticName ? desc.SemanticName : "";
            parameter.semanticIndex = desc.SemanticIndex;
            parameter.systemValueType =
                static_cast<uint32_t>(desc.SystemValueType);
            parameter.componentType = static_cast<uint32_t>(desc.ComponentType);
            parameter.register_ = desc.Register;
            parameter.mask = desc.Mask;
            parameter.readWriteMask = desc.ReadWriteMask;
            parameter.stream = desc.Stream;
            destination.push_back(std::move(parameter));
        }
        return true;
    };

    if (!readParameters(shaderDesc.InputParameters, true, inputs) ||
        !readParameters(shaderDesc.OutputParameters, false, outputs)) {
        inputs.clear();
        outputs.clear();
        return false;
    }
    return true;
}

// F-723: walk ONE ID3D12ShaderReflection into the binding list.
//
// This used to be an IDxcContainerReflection part-walk (GetPartCount ->
// GetPartReflection -> per-part ID3D12ShaderReflection). That whole outer layer
// is gone because it can never succeed here: the container query returns
// 0x80004002 on every input measured (see ReflectBindings' F-723 note for the
// two-source, two-interface matrix). What remains -- the binding loop and the
// F-634 CBV-size logic -- is unchanged, so the F-634 comments stay attached to
// the code they describe.
//
// The one semantic change: a shader with no parts can no longer be reached, so
// this returns what the shader itself declares. `out` is APPENDED to and never
// cleared here; ReflectBindings clears it once on entry.
bool ReflectFromShader(ID3D12ShaderReflection* reflector,
                       std::vector<DxcRuntime::ResourceBinding>& out) {
    using ResourceBinding = DxcRuntime::ResourceBinding;
    if (reflector == nullptr) return false;

    D3D12_SHADER_DESC shaderDesc;
    HRESULT hr = reflector->GetDesc(&shaderDesc);
    if (FAILED(hr)) return false;

    // Every shader-visible resource, not just CBVs: the worker needs to
    // REFUSE a shader that binds t4+ or s1, because the shared root
    // signature (d3d12_backend.cpp CreateTestRootSignature) declares t0..t3
    // and s0 only, and a PSO created against a root signature that lacks a
    // declared register either fails or reads a descriptor that is not
    // bound at draw time.
    for (UINT i = 0; i < shaderDesc.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC bindDesc;
        hr = reflector->GetResourceBindingDesc(i, &bindDesc);
        if (FAILED(hr)) continue;

        ResourceBinding rb;
        rb.type = static_cast<uint32_t>(bindDesc.Type);
        rb.register_ = bindDesc.BindPoint;
        rb.space_ = bindDesc.Space;

        if (bindDesc.Type == D3D_SIT_CBUFFER) {
            // F-634: the vendored DXC reflection interface
            // (`third_party/dxc/inc/dxc/d3d12shader.h`) does NOT have
            // `GetVariableDesc`, `GetVariableCount` or
            // `GetVariableByIndex` on `ID3D12ShaderReflection`. What it has
            // is `GetConstantBufferByIndex(UINT)` /
            // `GetConstantBufferByName(LPCSTR)`, and
            // `ID3D12ShaderReflectionConstantBuffer::GetDesc` which returns
            // a `D3D12_SHADER_BUFFER_DESC` carrying the buffer's real `Size`
            // plus its `VariableCount`. `GetVariableByIndex`/
            // `GetVariableByName` (and the variable's own
            // `GetDesc(D3D12_SHADER_VARIABLE_DESC*)`) live on the *constant
            // buffer* interface, not the shader one.
            //
            // So the CBV byte size comes from the constant buffer, which is
            // the direct answer rather than a proxy -- D3D12_SHADER_INPUT_
            // BIND_DESC genuinely has no size field.
            //
            // Assumption, stated because it is one: for a cbuffer the buffer
            // index within its space is taken to be `BindPoint`. When DXC
            // disagrees the size is left 0 and the draw path refuses, so a
            // wrong guess cannot become a wrong upload.
            D3D12_SHADER_BUFFER_DESC cbDesc = {};
            ID3D12ShaderReflectionConstantBuffer* cbuf =
                reflector->GetConstantBufferByIndex(bindDesc.BindPoint);
            // F-634: the vendored `D3D12_SHADER_BUFFER_DESC` has no
            // `VariableCount` field, so the first variable is fetched
            // defensively instead -- `GetVariableByIndex(0)` returns null
            // when the buffer has none, which is the check that matters.
            if (cbuf != nullptr && SUCCEEDED(cbuf->GetDesc(&cbDesc)) &&
                cbDesc.Size != 0) {
                rb.byteSize = cbDesc.Size;
                rb.firstVariableRegister = 0;  // b0 unless reflection says else
                if (ID3D12ShaderReflectionVariable* rv = cbuf->GetVariableByIndex(0)) {
                    D3D12_SHADER_VARIABLE_DESC varDesc = {};
                    if (SUCCEEDED(rv->GetDesc(&varDesc)) &&
                        varDesc.StartOffset != 0xFFFFFFFF)
                        rb.firstVariableRegister = varDesc.StartOffset;
                }
            } else {
                static std::atomic<uint32_t> s_cbvNoBuf{0};
                const uint32_t n = s_cbvNoBuf.fetch_add(1) + 1;
                if (n <= 4 || (n % 200) == 0)
                    MCLA_LOG_WARN("DXC-CBV-NOSIZE #{} bind b{} s{} -- byteSize left 0",
                                  n, bindDesc.BindPoint, bindDesc.Space);
            }
        }
        out.push_back(rb);
    }

    return true;
}

bool DxcRuntime::ReflectCbvBindings(const std::vector<uint8_t>& dxil,
                                    std::vector<CbvBinding>& outCbvs,
                                    std::string& error) const {
    outCbvs.clear();
    std::vector<ResourceBinding> all;
    if (!ReflectBindings(dxil, all, error)) return false;
    for (const auto& rb : all) {
        if (rb.type != static_cast<uint32_t>(D3D_SIT_CBUFFER)) continue;
        outCbvs.push_back(CbvBinding{rb.register_, rb.space_, rb.byteSize,
                                     rb.firstVariableRegister});
    }
    return true;
}

}  // namespace mcla::renderer
