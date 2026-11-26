#include "diag_threads.h"
#include "d3d12_backend.h"
#include "gpu_native/gnh_p2.h"
#include "renderer/precompiled_shaders.h"
#include "renderer/test_shaders.h"
#include "renderer/vertex_decode.h"
#include "renderer/pipeline_cache.h"
#include "renderer/resource_cache.h"
#include "renderer/shader_translator.h"
#include "renderer/texture_decode.h"
#include "gpu_cp.h"
#include "guest_memory.h"
#include "logging.h"
#include "common/cvar.h"
// F-XXX: the host's single Xenos->DXGI colour-format map, which the guest
// render target's own format is taken from. It is DEFINED in
// src/render_thread.cpp and DECLARED in src/render_thread.h; this include is
// what makes the backend use that one table instead of a second copy of it.
#include "render_thread.h"
// F-726: std::numeric_limits<double>::quiet_NaN() -- the guard that makes a mean
// above its own maximum unprintable without clamping a value into plausibility.
#include <limits>
// F-729: the colour/shape census in ReportDrawReadback builds its bounded
// distinct-value table, its per-row/per-column coverage counters and its
// pre-rendered top-N text out of these. std::snprintf is the same renderer the
// input-stream dump above already uses (src/d3d12_backend.cpp:3107), so the
// numbers in that row and the numbers in this row cannot be formatted by two
// different code paths.
#include <cstdio>
#include <string>
#include <vector>
// F-761: the late-frame census below uses std::stable_sort for the top-N
// histogram tie-break and std::strcmp for the ledger's write-site label. The
// sort has to be stable and has to be named here: an unstable sort would make
// the printed histogram differ between two runs of the same surface, which is
// the exact defect F-729 records for the draw-path census.
#include <algorithm>
#include <set>
#include <cstdlib> // F-918: getenv for MCLA_ATLAS_TESTPAT toggle
#include <cstring>
#include <ctime>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <thread>
static bool TempPerfStagesEnabled() {
    static const bool v = [] {
        const char* e = std::getenv("MCLA_TEMP_PERF_STAGES");
        const char* p = std::getenv("MCLA_PERF_STAGES");
        return (e && e[0] == '1') || (p && p[0] == '1');
    }();
    return v;
}
// PERF-FRAMES histogram alone, without the stage/producer timers MCLA_PERF_STAGES also turns on.
static bool PerfFramesOnly() {
    static const bool v = [] { const char* e = std::getenv("MCLA_PERF_FRAMES"); return e && e[0] == '1'; }();
    return v;
}
namespace mcla::native { void PerfOnPresent(); }  // render_thread.cpp: PERF-FRAMES histogram + PERF-SLOW-FRAME
static int64_t g_dcLast = 0, g_dcAcc[10] = {};
// PERF-STAGES tb[]: texbind split (render thread only): 0 bindTex 1 resolve 2 commit 3 sampler 4 decoded; counts 5 memoHit 6 memoMiss
static int64_t g_tbAcc[7] = {};
static uint64_t g_tbMissWhy[5] = {};
static int64_t DcNowNs() {
    return TempPerfStagesEnabled()
        ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
        : 0;
}
static int64_t g_dcMissNs = 0, g_dcHashNs = 0, g_dcExpandNs = 0; static uint64_t g_dcMiss = 0, g_dcMissBytes = 0, g_dcHashBytes = 0, g_dcMemoHit = 0;
static const mcla::native::TextureCache* g_dcCache = nullptr;
static uint64_t g_dcMissNew = 0, g_dcMissSame = 0, g_dcMissChanged = 0;
static std::atomic<uint64_t> g_dcUpBytes{0}, g_dcBarriers{0}, g_dcSrv{0}, g_dcDraws{0}, g_dcDropped{0};
static std::atomic<uint64_t> g_occDrawMono{0};  // MD-12 census: monotonic issued-draw count (g_dcDraws is reset per stat window)
static std::unordered_map<uint32_t, uint64_t> g_occBeginDraws;  // queryVa -> g_occDrawMono at BeginQuery (backend mutex held)

// PIXPROBE (MCLA_PIXPROBE=1): per-draw pixel attribution for the flat-grey-box
// problem (#4). File-scope so this TU needs minimal header coupling; every
// writer and reader is gated by PixProbeEnabled(), so with the var unset
// nothing is captured and no row is printed -- the default log is unchanged.
static bool PixProbeEnabled() {
    static const bool v = [] { const char* e = std::getenv("MCLA_PIXPROBE"); return e && e[0] == '1'; }();
    return v;
}
enum class PixProbePass { kBoth = 0, k3d = 1, kPost = 2 };
static PixProbePass PixProbePassMode() {
    static const PixProbePass m = [] {
        const char* e = std::getenv("MCLA_PIXPROBE_PASS");
        if (e && (std::strcmp(e, "3d") == 0 || std::strcmp(e, "3D") == 0)) return PixProbePass::k3d;
        if (e && std::strcmp(e, "post") == 0) return PixProbePass::kPost;
        return PixProbePass::kBoth;
    }();
    return m;
}
static void PixProbeXY(uint32_t& outX, uint32_t& outY) {
    static const uint32_t s_px = [] {
        const char* e = std::getenv("MCLA_PIXPROBE_X");
        return e && *e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 110u;
    }();
    static const uint32_t s_py = [] {
        const char* e = std::getenv("MCLA_PIXPROBE_Y");
        return e && *e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 420u;
    }();
    outX = s_px;
    outY = s_py;
}
static uint64_t s_pixprobePsHash = 0;            // psHash (constant bank) of the attributed post/final draw
static uint64_t s_pixprobePsProgHash = 0;        // psProgramHash of the attributed post/final draw
static uint64_t s_pixprobeDrawSeq = 0;           // m_cachePipelineDraws ordinal of that draw
static uint32_t s_pixprobeStagedSlots[32] = {};  // staged SRV index per shader slot (kShaderTextureSlotCount = 32); 0 = null SRV

// PS-TEX-JOIN (2026-11-20, log-only): captured guest texture descriptors for
// the PIXPROBE-attributed draw, so a flat/smeared pixel can be joined to the
// concrete base/fmt/dims/mip/packed state. Filled only under PixProbeEnabled().
struct PixprobeTex {
    uint32_t slot = 0, base = 0, fmt = 0, w = 0, h = 0;
    uint32_t packed = 0, mips = 0, swz = 0, gamma = 0;
};
static PixprobeTex s_pixprobeTex[16] = {};
static uint32_t s_pixprobeTexCount = 0;
static bool s_pixprobeCurSamplesSceneColor = false;

// Memo-cached texture descriptors for the main TexBindMemo so memo hits still
// refresh s_pixprobeTex / s_pixprobeStagedSlots accurately under MCLA_PIXPROBE=1.
static PixprobeTex s_pixprobeMemoTex[16] = {};
static uint32_t s_pixprobeMemoTexCount = 0;
static uint32_t s_pixprobeMemoStagedSlots[32] = {};
static bool s_pixprobeMemoSamplesSceneColor = false;
static uint64_t s_pixprobeMemoFrameBegins = 0;

// Separate snapshot for the last 3D scene-pass draw before fullscreen post-process resolves.
struct Pixprobe3dSnapshot {
    bool valid = false;
    uint64_t psHash = 0;
    uint64_t psProgHash = 0;
    uint64_t drawSeq = 0;
    uint32_t rtBase = 0;
    uint32_t rtW = 0, rtH = 0;
    uint32_t primVerts = 0;
    bool indexed = false;
    bool hasDsv = false;
    uint32_t stagedSlots[32] = {};
    PixprobeTex tex[16] = {};
    uint32_t texCount = 0;
};
struct Pixprobe3dRingEntry {
    uint64_t psProgHash = 0;
    uint64_t psHash = 0;
    uint64_t lastDrawSeq = 0;
    uint32_t draws = 0;
    uint32_t texCount = 0;
    uint32_t firstTexBase = 0;
};
static Pixprobe3dSnapshot s_pixprobe3d{};
static constexpr uint32_t kPixprobe3dRingCap = 8;
static Pixprobe3dRingEntry s_pixprobe3dRing[kPixprobe3dRingCap] = {};
static uint32_t s_pixprobe3dRingCount = 0;
static uint32_t s_pixprobeFrame3dCount = 0;
static bool s_pixprobe3dRbCapturedThisFrame = false;
static uint32_t s_pixprobe3dRbSlot = 0;

static float PixProbeHalfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x03FFu;
    uint32_t bits = 0;
    if (exp == 0u) {
        if (mant == 0u) {
            bits = sign;
        } else {
            uint32_t m = mant;
            int e = -14;
            while ((m & 0x0400u) == 0u) { m <<= 1; --e; }
            m &= 0x03FFu;
            bits = sign | (uint32_t(e + 127) << 23) | (m << 13);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// F9 OWNER PIXEL PROBE (2026-11-23). State: 0 idle, 1 requested (cursor stored), 2 recording the
// current frame (armed in BeginFrame), finalised in PresentOwnedFrame. Idle cost = one relaxed atomic
// load per draw. Every draw whose viewport+scissor contain the probed pixel gets a 1x1 copy of its RT0
// texel before and after the draw (D3D12 occlusion queries are per-draw, not per-pixel, so a texel diff
// is the cheapest exact "did this draw change the pixel" test; a draw that rewrites the same value or
// fails depth shows changed=0 and is still listed).
static std::atomic<int> g_f9State{0};
static std::atomic<float> g_f9ReqU{0.0f}, g_f9ReqV{0.0f};
static float g_f9BbX = 0, g_f9BbY = 0;  // back-buffer pixel of the cursor
static float g_f9U = 0, g_f9V = 0;      // position inside the presented guest picture, 0..1
static constexpr uint32_t kF9RbSlots = 4096, kF9RbSlotBytes = 512;  // 512 = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
static uint32_t g_f9RbUsed = 0;
struct F9Draw {
    uint64_t seq = 0, vsHash = 0, psHash = 0, psBank = 0;
    uint32_t prim = 0, verts = 0, rtBase = 0, rtW = 0, rtH = 0, samples = 1, hx = 0, hy = 0;
    uint32_t blend = 0, mask = 0, depth = 0, stencilRef = 0, rbSlot = UINT32_MAX, bpp = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool indexed = false, guestRt = false, hasDsv = false;
    D3D12_VIEWPORT vp{};
    D3D12_RECT sc{};
    std::string tex;
};
static std::vector<F9Draw> g_f9Draws;
static std::mutex g_f9HlslMu;
static std::unordered_map<uint64_t, std::string> g_f9Hlsl;

// P1 FOLIAGE-BIND (2026-11-20): env-gated per-draw descriptor -> staged-SRV dump
// at the point guest textures are decoded/bound (ResolveGuestTexturesToStaging).
// File-scope so this TU needs no d3d12_backend.h edit (another owner may own it).
// Default off: the getenv runs once but nothing is captured or printed, so the
// log is byte-for-byte unchanged. psHash is read from the PIXPROBE capture above
// and is only populated when MCLA_PIXPROBE=1; the always-live draw ordinal
// m_cachePipelineDraws is logged beside it as the drawSeq-equivalent join key.
// MCLA_NO_LASTCOLOUR=1 (2026-11-20 diagnostic): skip the documented format-blind
// m_resolvedLastColour rebind (PATCH_FLICKER C1). Log-only intent — a gated A/B
// decides whether the green tower stripes / flat grey box come from a stale
// resolved colour being bound to a different fetch.
static std::atomic<bool> s_noLastColourFileCached{false};
static void RefreshNoLastColour() {
    // Runtime toggle for matched-view A/B inside ONE boot: create/delete
    // build/no_lastcolour at a known time. Refreshed once per BeginFrame (250ms cadence).
    static std::atomic<uint64_t> lastMs{0};
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    uint64_t prev = lastMs.load(std::memory_order_relaxed);
    if (now >= prev + 250u && lastMs.compare_exchange_strong(prev, now)) {
        std::error_code ec;
        s_noLastColourFileCached.store(std::filesystem::exists("no_lastcolour", ec), std::memory_order_relaxed);
    }
}
static bool NoLastColour() {
    static const bool envOff = [] {
        const char* e = std::getenv("MCLA_NO_LASTCOLOUR");
        return e && e[0] == '1';
    }();
    return envOff || s_noLastColourFileCached.load(std::memory_order_relaxed);
}
static bool FoliageBindEnabled() {
    static const bool v = [] { const char* e = std::getenv("MCLA_FOLIAGE_BIND"); return e && e[0] == '1'; }();
    return v;
}

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// F-XXX: the project's VEH opt-out (src/kernel/xdm.h:297, defined there as a
// global-scope inline thread_local; src/boot_host.cpp:42 declares it the same
// way). Without this flag the process-wide VEH at boot_host.cpp:3130 logs
// "VEH W0: parking thread" and Sleep(1000)s forever BEFORE any __except can
// run -- measured on build/w11.log:2733, where the guard below was on the
// stack (crash_dump frame [08] = TryOMSetRenderTargets) and still never
// returned. Setting the flag is the VEH's own sanctioned bypass: its
// boot_host.cpp:3111 branch then logs "VEH SEHPASS" and continues the search.
extern thread_local bool t_inSafeExcept;

// Wave 3 (§3.5): -1=auto (on for AMD 0x1002), 0=off, 1=force. Evaluated once
// in Initialize(); the guard has no draw-path effect while every render
// target is SampleDesc.Count=1 (which is the current tree at every RT site).
MCLA_CVAR_INT(mcla_d3d12_msaa_line_guard, -1, "Renderer",
              "AMD MSAA line guard: -1=auto (AMD 0x1002), 0=off, 1=force");

// Wave 3 (§1.4/§2.3): the picture aspect exposed as a live menu row. "auto"
// maps to 16:9 in the backend; the guest-side constant write is worker 2's
// env-gated MclaApplyAspectRatio(), so an unset config changes nothing.
MCLA_CVAR_STRING(mcla_aspect_ratio, "auto", "Display",
                 "Guest picture aspect: auto (16:9), 16:9, 43:18");

namespace {
// Rule 1b: one new file-scope atomic + one registrar object in this TU. No
// captured `this`: the callback can run on the SDL thread at any time.
std::atomic<float> s_wave3LiveAspect{0.0f};  // 0 = no live menu change yet
float AspectFromSetting(std::string_view v) {
    if (v == "43:18") return 43.0f / 18.0f;
    if (v == "16:10") return 16.0f / 10.0f;
    return 16.0f / 9.0f;  // "auto", "16:9", and anything else: unchanged
}
struct AspectCvarCallbackRegistrar {
    AspectCvarCallbackRegistrar() {
        mcla::cvar::CVarSystem::Instance().AddChangeCallback(
            "mcla_aspect_ratio", [](std::string_view, std::string_view v) {
                s_wave3LiveAspect.store(AspectFromSetting(v), std::memory_order_relaxed);
            });
    }
} g_aspectCvarCallbackRegistrar;
} // namespace

#include <psapi.h>
#include <chrono>
// A resolve target older than this many presents is not bound as a texture any more (default ~0 / disabled; MCLA_TEXRACE_MAXAGE=N sets max age, 0 disables).
static uint64_t MaxResolveAgeFrames() {
    static const uint64_t v = [] { const char* e = std::getenv("MCLA_TEXRACE_MAXAGE"); if (!e) return ~uint64_t(0); const unsigned long x = std::strtoul(e, nullptr, 10); return x ? uint64_t(x) : ~uint64_t(0); }();
    return v;
}
namespace mcla::native { extern std::atomic<size_t> g_memUntileBytes, g_memVsCache, g_memPsCache; }
namespace mcla::native {

static std::atomic<uint64_t> s_sdlWindowFlags{0};
void SetSdlWindowFlags(uint64_t flags) { s_sdlWindowFlags.store(flags, std::memory_order_relaxed); }
uint64_t GetSdlWindowFlags() { return s_sdlWindowFlags.load(std::memory_order_relaxed); }

namespace {

static D3D12Backend g_d3d12Backend;

// F-642: texture_decode.h:485-518 restates the D3D12 filter/address/compare
// enums as plain numbers so the decode module can stay SDK-free, and hands the
// backend static_asserts to keep them honest. The asserts were written, and they
// DO NOT ALL HOLD against the real d3d12.h (Windows Kits 10.0.26100.0,
// d3d12.h:3535-3600 for D3D12_FILTER, :1586-1594 for D3D12_COMPARISON_FUNC,
// :3654-3658 for the address modes). Measured, not from memory:
//
//   ENUM                          texture_decode.h   real d3d12.h
//   ADDRESS_MODE_WRAP .. _MIRROR_ONCE  1..5  = 1..5  OK (all five)
//   FILTER_MIN_MAG_MIP_POINT         0x000 = 0x000  OK
//   FILTER_ANISOTROPIC               0x055 = 0x055  OK
//   FILTER_COMPARISON_MIN_MAG_MIP_POINT 0x080 = 0x080  OK
//   FILTER_COMPARISON_ANISOTROPIC    0x0D5 = 0x0D5  OK
//   FILTER_MIN_MAG_MIP_LINEAR        0x034 vs 0x15  DRIFT
//   FILTER_MIN_MAG_POINT_MIP_LINEAR  0x038 vs 0x01  DRIFT
//   FILTER_MIN_POINT_MAG_MIP_LINEAR  0x03C vs 0x05  DRIFT
//   FILTER_MIN_POINT_MAG_MIP_POINT   0x030 vs 0x04  DRIFT (name is MIN_POINT_MAG_LINEAR_MIP_POINT)
//   FILTER_MIN_LINEAR_MAG_MIP_LINEAR 0x044 vs 0x11  DRIFT
//   FILTER_MIN_LINEAR_MAG_MIP_POINT  0x020 vs 0x10  DRIFT
//   FILTER_COMPARISON_MIN_MAG_MIP_LINEAR 0x0B4 vs 0x95  DRIFT
//   COMPARISON_FUNC_NONE             (absent) vs 0
//   COMPARISON_FUNC_NEVER            0     vs 1    DRIFT
//   COMPARISON_FUNC_ALWAYS           7     vs 8    DRIFT  (the whole
//                                      NEVER..ALWAYS run is off by one)
//   D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK = 1, OPAQUE_WHITE = 2 (d3d12.h:4086-4088)
//
// The five that agree are asserted. The drifting ones are RECORDED, not
// asserted, because asserting them would fail the build for a file this change
// does not own; MCLA_ASSERT_TEXTURE_DECODE_TABLE turns the record into hard
// asserts the moment the texture agent corrects texture_decode.h:503-517, and
// nothing else has to change here.
//
// This drift is not academic. HostSamplerDesc::filter and ::comparisonFunc are
// consumed verbatim by BindGuestSampler, so a hard static_cast of 0x034 into
// D3D12_FILTER is filter 0x34 = D3D12_FILTER_MINIMUM_MIN_POINT_MAG_MIP_LINEAR -
// a MINIMUM reduction filter, i.e. silently a different filtering mode. That is
// why IsLegalD3D12Filter below exists: the backend validates the decoded value
// against the REAL enum set and REFUSES anything that is not a legal
// D3D12_FILTER, instead of casting a number that is known to be wrong.
static_assert(static_cast<uint32_t>(native::kD3D12AddressModeWrap) ==
                  D3D12_TEXTURE_ADDRESS_MODE_WRAP, "texture_decode.h address enum drift");
static_assert(static_cast<uint32_t>(native::kD3D12AddressModeMirror) ==
                  D3D12_TEXTURE_ADDRESS_MODE_MIRROR, "texture_decode.h address enum drift");
static_assert(static_cast<uint32_t>(native::kD3D12AddressModeClamp) ==
                  D3D12_TEXTURE_ADDRESS_MODE_CLAMP, "texture_decode.h address enum drift");
static_assert(static_cast<uint32_t>(native::kD3D12AddressModeBorder) ==
                  D3D12_TEXTURE_ADDRESS_MODE_BORDER, "texture_decode.h address enum drift");
static_assert(static_cast<uint32_t>(native::kD3D12AddressModeMirrorOnce) ==
                  D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, "texture_decode.h address enum drift");
static_assert(static_cast<uint32_t>(native::kD3D12FilterMinMagMipPoint) ==
                  D3D12_FILTER_MIN_MAG_MIP_POINT, "texture_decode.h filter enum drift");
static_assert(static_cast<uint32_t>(native::kD3D12FilterAnisotropic) == D3D12_FILTER_ANISOTROPIC,
              "texture_decode.h filter enum drift");
static_assert(static_cast<uint32_t>(native::kD3D12FilterComparisonMinMagMipPoint) ==
                  D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, "texture_decode.h filter enum drift");
static_assert(static_cast<uint32_t>(native::kD3D12FilterComparisonAnisotropic) ==
                  D3D12_FILTER_COMPARISON_ANISOTROPIC, "texture_decode.h filter enum drift");

#ifdef MCLA_ASSERT_TEXTURE_DECODE_TABLE
static_assert(native::kD3D12FilterMinMagMipLinear == D3D12_FILTER_MIN_MAG_MIP_LINEAR,
              "texture_decode.h:503 D3D12_FILTER_MIN_MAG_MIP_LINEAR is 0x034, d3d12.h:3548 says 0x15");
static_assert(native::kD3D12FilterMinMagPointMipLinear ==
                  D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR,
              "texture_decode.h:507 D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR is 0x038, d3d12.h:3542 says 0x01");
static_assert(native::kD3D12FilterMinPointMagMipLinear == D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR,
              "texture_decode.h:506 is 0x03C, d3d12.h:3544 says 0x05");
static_assert(native::kD3D12FilterMinPointMagMipPoint == D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,
              "texture_decode.h:508 name/0x030 does not exist; d3d12.h:3543 D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x04");
static_assert(native::kD3D12FilterMinLinearMagMipLinear == D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
              "texture_decode.h:509 is 0x044, d3d12.h:3546 says 0x11");
static_assert(native::kD3D12FilterMinLinearMagMipPoint == D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT,
              "texture_decode.h:510 is 0x020, d3d12.h:3545 says 0x10");
static_assert(native::kD3D12FilterComparisonMinMagMipLinear ==
                  D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
              "texture_decode.h:512 is 0x0B4, d3d12.h:3558 says 0x95");
static_assert(native::kD3D12ComparisonFuncNever == D3D12_COMPARISON_FUNC_NEVER,
              "texture_decode.h:514 is 0, d3d12.h:1587 says 1 (0 is COMPARE_FUNC_NONE)");
static_assert(native::kD3D12ComparisonFuncAlways == D3D12_COMPARISON_FUNC_ALWAYS,
              "texture_decode.h:515 is 7, d3d12.h:1594 says 8 (the whole run is off by one)");
#endif  // MCLA_ASSERT_TEXTURE_DECODE_TABLE

// Is `filter` one of the values d3d12.h actually defines? This is the guard that
// keeps F-642's drift from reaching a D3D12_SAMPLER_DESC. The set is the
// non-MINIMUM/MINIMUM/MAGNIFICATION family plus the COMPARISON family, read off
// d3d12.h:3541-3600; anything outside it is a number no D3D12 driver accepts as
// a filter, so binding it would be undefined rather than merely wrong.
bool IsLegalD3D12Filter(uint32_t filter) {
    switch (filter) {
        case D3D12_FILTER_MIN_MAG_MIP_POINT:
        case D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR:
        case D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT:
        case D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR:
        case D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT:
        case D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
        case D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT:
        case D3D12_FILTER_MIN_MAG_MIP_LINEAR:
        case D3D12_FILTER_MIN_MAG_ANISOTROPIC_MIP_POINT:
        case D3D12_FILTER_ANISOTROPIC:
        case D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT:
        case D3D12_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR:
        case D3D12_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT:
        case D3D12_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR:
        case D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT:
        case D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
        case D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT:
        case D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR:
        case D3D12_FILTER_COMPARISON_MIN_MAG_ANISOTROPIC_MIP_POINT:
        case D3D12_FILTER_COMPARISON_ANISOTROPIC:
        case D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT:
        case D3D12_FILTER_MINIMUM_MIN_MAG_POINT_MIP_LINEAR:
        case D3D12_FILTER_MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:
        case D3D12_FILTER_MINIMUM_MIN_POINT_MAG_MIP_LINEAR:
        case D3D12_FILTER_MINIMUM_MIN_LINEAR_MAG_MIP_POINT:
        case D3D12_FILTER_MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
        case D3D12_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT:
        case D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR:
        case D3D12_FILTER_MINIMUM_MIN_MAG_ANISOTROPIC_MIP_POINT:
        case D3D12_FILTER_MINIMUM_ANISOTROPIC:
        case D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_POINT:
        case D3D12_FILTER_MAXIMUM_MIN_MAG_POINT_MIP_LINEAR:
        case D3D12_FILTER_MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:
        case D3D12_FILTER_MAXIMUM_MIN_POINT_MAG_MIP_LINEAR:
        case D3D12_FILTER_MAXIMUM_MIN_LINEAR_MAG_MIP_POINT:
        case D3D12_FILTER_MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
        case D3D12_FILTER_MAXIMUM_MIN_MAG_LINEAR_MIP_POINT:
        case D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR:
        case D3D12_FILTER_MAXIMUM_MIN_MAG_ANISOTROPIC_MIP_POINT:
        case D3D12_FILTER_MAXIMUM_ANISOTROPIC:
            return true;
        default:
            return false;
    }
}

// F-638: bytes per BLOCK for the block-compressed host formats the import gate
// now accepts, plus the scalar forms. GetCopyableFootprints supplies the
// DESTINATION row pitch; this table supplies the SOURCE row pitch, which is a
// property of the format alone: width*bytesPerBlock for a scalar format,
// ceil(width/4)*bytesPerBlock for a 4x4 block format. Computing it from the
// destination pitch instead would silently shear every compressed upload, which
// looks like a wrong image rather than an error, so it is a named table.
struct HostFormatFootprint {
    DXGI_FORMAT format;
    uint32_t blockWidth;
    uint32_t blockHeight;
    uint32_t bytesPerBlock;
};
constexpr HostFormatFootprint kHostFormatTable[] = {
    { DXGI_FORMAT_R8G8B8A8_UNORM,     1, 1,  4 },
    { DXGI_FORMAT_B8G8R8A8_UNORM,     1, 1,  4 },
    { DXGI_FORMAT_R8_UNORM,           1, 1,  1 },
    { DXGI_FORMAT_R8G8_UNORM,         1, 1,  2 },
    { DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1,  8 },
    // F-900: R11G11B10_FLOAT (26). Xenos formats 16/55 (k_10_11_11) plan to this
    // DXGI format (texture_decode.cpp:105/144) and PlanTextureImport passes them
    // as ScalarCopy/4 B-per-texel -- they died ONLY here, at FindHostFormat:
    // measured 52,000 attempts in build/g962.log, every one refused
    // "NoHostFormat: plan wants dxgiFormat 26" (the ONLY refusal reason in that
    // soak). D3D12 feature-level 11 hardware is required to and does support
    // this format for Texture2D + Shader sample + Mip (Microsoft format-support
    // matrix), and the project already hands DXGI 26 to D3D12 for VERTEX buffers
    // (vertex_decode.cpp:54). OPEN QUESTION, measured by the next soak: Xenia
    // models k_10_11_11 as packed UNORM (pixel_formats.xesli:472-486, integer
    // field extraction v<<5|v>>6) and never uses DXGI 26 (0 hits in .research),
    // so if the payload bits are unorm-coded the import will be COLOR-WRONG
    // (float-decoded noise), not crash -- (b) import fires + (d) colors sane are
    // BOTH required before this is called fixed. If unorm is confirmed, replace
    // this row with a CpuExpand path and the Gate 2 import-vs-native fix.
    { DXGI_FORMAT_R11G11B10_FLOAT,    1, 1,  4 },
    { DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 16 },
    { DXGI_FORMAT_BC1_UNORM,          4, 4,  8 },
    { DXGI_FORMAT_BC1_UNORM_SRGB,     4, 4,  8 },
    { DXGI_FORMAT_BC2_UNORM,          4, 4, 16 },
    { DXGI_FORMAT_BC2_UNORM_SRGB,     4, 4, 16 },
    { DXGI_FORMAT_BC3_UNORM,          4, 4, 16 },
    { DXGI_FORMAT_BC3_UNORM_SRGB,     4, 4, 16 },
    { DXGI_FORMAT_BC4_UNORM,          4, 4,  8 },
    { DXGI_FORMAT_BC5_UNORM,          4, 4, 16 },
    { DXGI_FORMAT_R32G32_FLOAT,       1, 1,  8 },
    { DXGI_FORMAT_R32_FLOAT,          1, 1,  4 },
    { DXGI_FORMAT_R32_UINT,           1, 1,  4 },
    { DXGI_FORMAT_R16G16B16A16_UNORM, 1, 1,  8 },
    { DXGI_FORMAT_R16G16_FLOAT,       1, 1,  4 },
    { DXGI_FORMAT_R16G16_UNORM,       1, 1,  4 },
    { DXGI_FORMAT_R16_FLOAT,          1, 1,  2 },
    { DXGI_FORMAT_R16_UNORM,          1, 1,  2 },
    { DXGI_FORMAT_R10G10B10A2_UNORM,  1, 1,  4 },
};
// Rows 0..kHostFormatBaseCount-1 are the pre-patch table.
constexpr uint32_t kHostFormatBaseCount = 15;
constexpr uint32_t kHostFormatTableCount =
    static_cast<uint32_t>(sizeof(kHostFormatTable) / sizeof(kHostFormatTable[0]));

// Returns the table row for `format`, or nullptr when this backend cannot import
// it. nullptr is a REFUSAL, never a fallback to R8G8B8A8_UNORM.
const HostFormatFootprint* FindHostFormat(DXGI_FORMAT format) {
    for (uint32_t i = 0; i < kHostFormatTableCount; ++i) {
        if (kHostFormatTable[i].format == format) return &kHostFormatTable[i];
    }
    return nullptr;
}

inline uint32_t BlocksFor(uint32_t texels, uint32_t blockTexels) {
    if (blockTexels <= 1) return texels;
    return (texels + blockTexels - 1) / blockTexels;
}

// F-637/F-638/F-639/F-640: bounded, process-wide refusal counters. A soak needs
// the totals, and the process holds exactly one backend (g_d3d12Backend above),
// so a file-scope array is enough and costs no per-draw allocation.
constexpr uint32_t kMaxCountedPrimTypes = 32;
uint64_t g_primTypeRefusals[kMaxCountedPrimTypes] = {};
uint64_t g_primTypeRefusalOther = 0;
uint64_t g_textureImportRefusals[32] = {};
uint64_t g_textureImportRefusalOther = 0;
uint64_t g_resolveRefusals = 0;
uint64_t g_samplerRefusals = 0;
uint64_t g_textureBindRefusals = 0;
uint64_t g_layoutRefusals = 0;

// F-649: the import census. Two counters and one bucket array, all bumped
// unconditionally -- the throttle below gates the TEXT, never the count, because
// a suppressed row that is not counted is how a soak reads "nothing happened"
// out of something that happened 616,824 times (F-054 / LOG QUALITY POISONED).
uint64_t g_textureImportPlans = 0;   // every plan that reached CreateDecodedTexture
uint64_t g_textureImportOk = 0;      // ... and produced a resident texture
uint64_t g_textureImportBackendRefusals[static_cast<uint32_t>(
    D3D12Backend::TextureBackendRefusal::kCount)] = {};

// Bounded log throttle: one line for the first kRefuseLogFirst events of a kind,
// then every kRefuseLogEvery-th. House pattern (n <= K || n % 200 == 0) so a
// refusal storm cannot turn the log into the bottleneck it is measuring.
constexpr uint64_t kRefuseLogFirst = 8;
constexpr uint64_t kRefuseLogEvery = 200;
inline bool ShouldLogRefusal(uint64_t n) {
    return n <= kRefuseLogFirst || (n % kRefuseLogEvery) == 0;
}

// ---------------------------------------------------------------------------
// CLIPEXTENT-PUBLISHED -- publishes the render-target extent that the
// clip_disable rescale is built from, AT THE POINT THE EXTENT IS FIRST KNOWN.
//
// MEASURED 2026-10-04 on build/w707.log, by line number. The publication used to
// live on the draw path, and that is one translation too late: the constants are
// BAKED at translation time -- renderer/shader_translator.cpp:1134 reads the
// process global inside AppendEpilogue and :1194-1199 formats it into the
// emitted HLSL -- so the first translation of a process wins for the life of
// that process. The ordering, w707.log:
//     144   D3D12Backend: Initializing backend        <- extent already known
//     386   D3D12Backend: Resizing swap chain         <- and again, still at init
//    7479   DRAW_CAPTURED #1                         <- first draw reaches the
//    7505   draw #1's programs TRANSLATED               render thread
//    8027   VS-CLIPSPACE-PROBE #1 ... rescaleValid=0     IDENTITY, permanently
//    8031   the publication that used to be inside the draw
//    8074   draw #2's programs translated               valid -> the real rescale
// Both draws target the same 1280x720 target, so the only thing that differed
// between an identity bake and a correct one was WHERE the publication sat.
//
// WHY ONE ROW IS ENOUGH TO READ THE ORDER OFF. Two clock readings from two
// threads prove nothing by themselves; the ORDER OF LINES in one append-only log
// does. This row is written at the instant of publication, so a soak compares
// its line number against the line number of the first translation row and reads
// the order directly instead of inferring it from two timestamps. `validBefore=0`
// on the first row additionally states that no extent had EVER been accepted,
// which is exactly the condition under which the translator emits the identity.
//
// NOT A DEFAULT AND NOT A GUESS. `extentX`/`extentY` are this backend's own
// swapchain dimensions at the instant they are published. A zero extent is
// REFUSED by the translator (shader_translator.cpp:1806) rather than substituted,
// and that refusal logs at ERROR under this same token, so the token's count can
// never read 1 while the shader kept the identity. Nothing here fabricates an
// extent to fill the gap.
//
// BOTH CALL SITES ARE IN THIS FILE AND THE LIST IS EXHAUSTIVE: the extent is
// assigned in exactly two places (`m_width`/`m_height` -- every occurrence in
// this file was enumerated, not sampled), and each now publishes immediately
// after the assignment and before anything that can translate:
//   Initialize -- ahead of CreateSwapChain, and therefore ahead of the caller
//                starting the pipeline worker (src/render_thread.cpp:121),
//                which is the only ordering the translator's own contract asks
//                for (shader_translator.h:192-194 "call this before translating
//                any vertex shader, and again whenever the render target is
//                resized"; shader_translator.cpp:1800-1804).
//   Resize     -- after ResizeBuffers has SUCCEEDED, so the published extent is
//                always one the swapchain really has.
//
// `drawsIssued` is the backend's own draw ordinal at the publication instant. It
// is reported so a reader can see whether any draw had already been issued here;
// it is NOT offered as proof of the translation order, because a translation is
// reachable only from inside a draw command (measured: the translation call
// sites are src/render_thread.cpp:1289, :1291, :3471 and :3537, all inside
// processCommand's draw cases, and those cases break unless the backend finished
// initializing) -- the log line order is the proof, this number is the context.
static void PublishClipDisableExtent(const char* site, uint32_t extentX,
                                     uint32_t extentY, uint64_t drawsIssued) {
    static std::atomic<uint64_t> s_publications{0};
    const uint64_t n = s_publications.fetch_add(1, std::memory_order_relaxed) + 1;

    renderer::ClipDisableRescaleConstants before{};
    const bool validBefore = renderer::QueryClipDisableRescale(&before);

    const bool accepted = renderer::SetClipDisableViewportExtent(extentX, extentY);

    renderer::ClipDisableRescaleConstants after{};
    const bool validAfter = renderer::QueryClipDisableRescale(&after);

    if (!accepted || !validAfter) {
        // Loud, and loud under the SAME token: a refusal logged under a different
        // name would leave a reader of the throttled row above believing in a
        // rescale that is not there.
        MCLA_LOG_ERROR(
            "CLIPEXTENT-PUBLISHED #{} site={} extent={}x{} accepted={} "
            "validBefore={} validAfter={} drawsAtPublish={} "
            "ndcScale=({},{},{}) ndcOffset=({},{},{}) -- REFUSED: no usable "
            "extent was installed, so a vertex shader translated from here on "
            "bakes the identity form. Nothing was substituted for the zero.",
            n, site, extentX, extentY, accepted ? 1 : 0, validBefore ? 1 : 0,
            validAfter ? 1 : 0, drawsIssued, after.scaleX, after.scaleY,
            after.scaleZ, after.offsetX, after.offsetY, after.offsetZ);
        return;
    }

    // Bounded by the house throttle, not by "the first row only": a later row is
    // what shows a resize re-publishing a DIFFERENT extent, and a one-shot row
    // cannot be compared against a later one in the same soak. The counter above
    // is incremented unconditionally either way, so a suppressed row still counts.
    if (!ShouldLogRefusal(n)) return;
    MCLA_LOG_INFO(
        "CLIPEXTENT-PUBLISHED #{} site={} extent={}x{} accepted={} "
        "validBefore={} validAfter={} drawsAtPublish={} "
        "ndcScale=({},{},{}) ndcOffset=({},{},{})",
        n, site, extentX, extentY, accepted ? 1 : 0, validBefore ? 1 : 0,
        validAfter ? 1 : 0, drawsIssued, after.scaleX, after.scaleY,
        after.scaleZ, after.offsetX, after.offsetY, after.offsetZ);
}

// ---------------------------------------------------------------------------
// F-650 (Task 2): the back-buffer clear colour, as ONE named constant.
//
// NOTHING in the guest or in any reference establishes this value. Measured
// 2026-09-30 against build/w662.log and the working tree: the Xenos command
// capture path carries no clear colour at all -- `clearColor`, `ClearColor`,
// `CLEAR_RENDER_TARGET` and `kClear` have ZERO occurrences in src/gpu_cp.cpp --
// and the only clear in the whole 797,718-line soak is the host's own
// "RenderThread: initial clear+present (dark blue)" at 00:43:44.017. The guest
// never asked for this colour; on a real console the first draw of a frame
// covers whatever the previous frame left, and no clear is required at all.
//
// So this is a HOST PLACEHOLDER, kept deliberately, named so it cannot hide:
// before F-650 the same near-black sat inline in the draw path as a literal
// {0.02, 0.03, 0.05, 1.0}, which is how a frame ends up the wrong shade
// forever with no line to edit. It is now one constant with this comment
// attached, and FRAME-OWNER counts how many presented frames it was the
// background of.
//
// WHAT WOULD REPLACE IT, in the order the evidence would arrive:
//   1. The guest's own PM4 CLEAR_RENDER_TARGET colour, once the CP path decodes
//      the packet's clear value and carries it on the draw descriptor. Then the
//      clear is the guest's and this constant should be DELETED, not assigned.
//   2. If the guest is measured never to clear -- i.e. every frame's first draw
//      covers every pixel -- the right value is not a colour at all: the clear
//      should be removed, and this constant with it.
// Neither is measured, so it stays. What must not happen is it drifting back
// into being an inline literal at a second call site.
// ---------------------------------------------------------------------------
constexpr float kPlaceholderBackBufferClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

// F-XXX: THE GUARDED RENDER-TARGET BIND.
//
// MEASURED, build/w10.log:4910-4937 -- the render thread died at 20:29:07.992
// with C0000005 reading address 0x28 inside d3d12.dll. The faulting thread's
// return address is 0x1402F4909, which llvm-symbolizer resolves against
// build/mcla.pdb to d3d12_backend.cpp:5118 -- the OMSetRenderTargets call.
// The VEH handler then PARKED the render thread ("VEH W0: parking thread
// 19288"), so all rendering stopped at t=41s while the guest kept running.
//
// SEH IS THE PROBE, NOT A SWALLOW. A wrapper that only returned false would
// turn "the render thread crashed" into "draws silently went missing", which
// is the exact class rule 19 forbids. The call site prints the command list,
// both descriptor handles, both heaps, the frame index and -- the two values
// that separate a bad-handle fault from an already-dead device --
// m_lastPresentHr and GetDeviceRemovedReason().
//
// __try/__except cannot live in DrawDynamicMeshWithPipeline: MSVC rejects it in
// any function that needs C++ unwinding (C2712), and that function owns
// ComPtrs, vectors and a lock_guard. So the guarded call is its own POD-only
// free function. noinline guarantees the SEH frame stays here.
static __declspec(noinline) bool TryOMSetRenderTargets(
    ID3D12GraphicsCommandList* list, UINT numRtvs,
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv, unsigned long* codeOut) {
    // t_inSafeExcept is the VEH's documented opt-out, not a global switch: it
    // is thread_local, and the previous value is restored on BOTH paths so a
    // nested guarded block keeps its own state.
    const bool prevSafeExcept = t_inSafeExcept;
    t_inSafeExcept = true;
    __try {
        list->OMSetRenderTargets(numRtvs, rtvs, single, dsv);
        t_inSafeExcept = prevSafeExcept;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_inSafeExcept = prevSafeExcept;
        *codeOut = static_cast<unsigned long>(GetExceptionCode());
        return false;
    }
}
static std::atomic<uint32_t> s_omsetFaults{0};
static std::atomic<bool> s_omsetLatchLogged{false};
}  // namespace

// CBV/viewport hoist: forward declaration, defined next to the caches at
// ~4750 (DrawDynamicMeshWithPipeline hoist). Call sites in the RT/depth/blit
// helpers above forward to it.
static void InvalidateDrawCaches();
// Per-command-list redundant-state skip (AUDIT_PERF_DRAWCALL C/D). Valid only
// within one open list; cleared by InvalidateDrawCaches() (list reset/close)
// and at every non-draw site that binds other heaps/root sig/PSO/topology.
static bool s_heapsBound = false;
static ID3D12RootSignature* s_boundRootSig = nullptr;
static ID3D12PipelineState* s_boundPso = nullptr;
static int32_t s_boundStencilRef = -1;
static std::atomic<uint32_t> g_stencilDraws{0}, g_stencilRefSets{0};
static D3D12_PRIMITIVE_TOPOLOGY s_boundTopo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
static D3D12_GPU_DESCRIPTOR_HANDLE s_boundSrvTableGpu = {};
static D3D12_GPU_DESCRIPTOR_HANDLE s_boundSamplerTableGpu = {};
static size_t s_uploadPeakBytes = 0;  // MEM-CACHES: max per-frame upload-region use since the last row
static void InvalidateBoundState() {
    s_heapsBound = false;
    s_boundRootSig = nullptr;
    s_boundPso = nullptr;
    s_boundStencilRef = -1;
    s_boundTopo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    s_boundSrvTableGpu = {};
    s_boundSamplerTableGpu = {};
}

D3D12Backend* GetD3D12Backend() {
    return &g_d3d12Backend;
}

// F-637: Xenos VGT_DRAW_INITIATOR.prim_type (xenos.h:44-59) -> D3D12 topology.
// The value space is the architected enum, NOT DrawPacket::primType's own stale
// comment ("0=TriList, 1=TriStrip" at trace_types.h:74), which is why this is
// spelled out per code rather than as an offset into an array.
bool D3D12Backend::MapXenosPrimType(uint32_t primType, D3D_PRIMITIVE_TOPOLOGY& out,
                                    bool hasGeometryStage) {
    switch (primType) {
        case 0x01: {  // kPointList
            // MD-19 census (log-only): Xenia expands points to sprites; we draw 1px points.
            static std::atomic<uint64_t> s_points{0};
            const uint64_t n = s_points.fetch_add(1, std::memory_order_relaxed) + 1;
            if (ShouldLogRefusal(n))
                MCLA_LOG_WARN("PC-POINTLIST #{} drawn as raw 1px points (no sprite expansion)", n);
            out = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; return true;
        }
        case 0x02: out = D3D_PRIMITIVE_TOPOLOGY_LINELIST;      return true;  // kLineList
        case 0x03: out = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;     return true;  // kLineStrip
        case 0x04: out = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  return true;  // kTriangleList
        case 0x05: out = D3D_PRIMITIVE_TOPOLOGY_TRIANGLEFAN;   return true;  // kTriangleFan
        case 0x06: out = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; return true;  // kTriangleStrip
        // Refused, each with its own reason in the log:
        //  0x00 kNone              -- no primitive; Xenia's translation is a
        //                             draw of zero indices, not a geometry type
        //  0x07 kTriangleWithWFlags -- per-triangle W flag; D3D12 carries that
        //                             in a PS, not in a topology
        // F-650: 0x08 gets its OWN case, not a line in the comment block, so the
        // one prim_type the guest is measured to actually issue is a named refusal
        // with a stated reason rather than an anonymous fallthrough.
        //  0x08 kRectangleList     -- the console's "quads". Xenos emits two
        //                             triangles per rectangle, so TRIANGLELIST
        //                             would draw them in the wrong order with no
        //                             indication anything was dropped. It is NOT
        //                             substituted here on purpose: expanding a
        //                             rect list into 2N triangles is the
        //                             producer's job, because only the producer
        //                             knows the index stride and base. MEASURED in
        //                             build/w662.log: DRAW-CP-DRAWN #20600
        //                             prim=8 vertices=3 indexed=0 -- every draw
        //                             that reached the producer was a rect list.
        //
        // F-653 + the hasGeometryStage arm: a rectangle list is three guest
        // vertices that only become a quad inside the geometry stage, so the
        // topology is TRIANGLELIST IF AND ONLY IF the PSO about to be used has
        // that stage bound. The mapping is the reference's own, measured:
        // d3d12_command_processor.cc:2308-2310 sets
        // D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST for the rectangle path and
        // :2330-2339 issues D3DDrawInstanced(count, 1, 0, 0) with no index
        // buffer, and d3d12/pipeline_cache.cc:1402-1405 keys the pipeline with
        // geometry_shader = kRectangleList. The stage itself is built by
        // BuildRectangleGeometryShaderHlsl (F-653, pipeline_cache.cpp) with
        // input primitive = Triangle, input vertex count 3, output strip and
        // max_output_vertex_count = 4 (pipeline_cache.cc:2298-2303), which is
        // why three vertices in is the right count and not an off-by-one.
        //
        // The refusal arm below is UNCHANGED and still counts, so a soak reading
        // PrimTypeRefusalCount(0x08) sees "rectangle draws refused because the
        // PSO in hand carried no expansion stage" rather than nothing at all.
        case 0x08:
            if (hasGeometryStage) {
                out = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                return true;
            }
            break;  // counted below, with its own reason
        //  0x0C kLineLoop          -- D3D12 has no loop topology
        //  0x0D/0x0E kQuadList/kQuadStrip, 0x0F kPolygon -- no equivalent
        default: break;
    }
    // F-650: the single exit every refusal takes, counted per prim_type so the
    // rectangle list is visible as its own row rather than hiding in a fallthrough.
    // g_primTypeRefusals[] was written NOWHERE in the tree before this (read at
    // :293 and :299; the only write in the backend was the unrelated patchlist
    // bucket), so PrimTypeRefusalCount(0x08) read 0 forever and a refusal could
    // not be told from a call that never happened. Counting here rather than at
    // the call site is the point: no caller can absorb a refusal by not reporting it.
    const uint64_t n = (primType < kMaxCountedPrimTypes)
                           ? ++g_primTypeRefusals[primType]
                           : ++g_primTypeRefusalOther;
    if (ShouldLogRefusal(n)) {
        // F-716: THE CLAUSE BELOW WAS WRONG AND NAMED A BLOCKER THAT DOES NOT
        // EXIST. It used to read "D3D12 has no equivalent topology; the Xenos
        // rectangle list 0x08 must be expanded to 2 triangles per rect by the
        // producer, never substituted here". Every clause of that is
        // contradicted by the reference, measured just now:
        //   * the reference DOES map kRectangleList to
        //     D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST --
        //     d3d12_command_processor.cc:2307-2310;
        //   * it expands in a GEOMETRY SHADER, not "by the producer" and not in
        //     the vertex shader: d3d12_primitive_processor.cc:31 passes
        //     `rectangle_lists_supported_without_vs_expansion = true` (6th
        //     argument, order at primitive_processor.h:249-260), so
        //     primitive_processor.cc:125-126 sets expand_rectangle_lists_in_vs_
        //     = false and the VS-expansion branch at primitive_processor.cc:
        //     392-397 is SKIPPED. host_vertex_shader_type stays kVertex (:377);
        //   * it issues D3DDrawInstanced(host_draw_vertex_count, 1, 0, 0) with
        //     host_draw_vertex_count == vgt_draw_initiator.num_indices
        //     (primitive_processor.cc:440) -- for this game's rectangle draws,
        //     the 3 of DRAW-CP-REFUSE -- with NO index buffer
        //     (d3d12_command_processor.cc:2330-2339).
        // So "D3D12 has no equivalent topology" is false and the draw is NOT
        // refused for want of a topology. It is refused because THE PSO IN HAND
        // CARRIES NO EXPANSION STAGE -- and that is the one clause worth
        // printing, because it is the one a soak can act on.
        //
        // The prefix (marker name and both numbers) is byte-identical to before;
        // only the `--` explanation changed, so a grep on
        // "prim_type 0x08 unmappable, refused" still finds every occurrence.
        MCLA_LOG_WARN("D3D12Backend: prim_type 0x{:02X} unmappable, refused #{} "
                      "(total={}) -- the TOPOLOGY is not the problem: D3D12 has "
                      "exactly the topology the reference uses for this "
                      "prim_type (TRIANGLELIST, d3d12_command_processor.cc:"
                      "2307-2310). The blocker is that the PSO in hand carries "
                      "NO geometry stage, so the rectangle's 4th vertex would "
                      "never be generated and TRIANGLELIST would draw ONE of "
                      "its TWO triangles. The stage is built and compiled by "
                      "PipelineCache for a key that names kGeomStageRectangleList "
                      "(hasGeometryStage, above); it is bound only when "
                      "GetOrCompile returns THAT PSO. Counted separately in the "
                      "cache: PC-RECT-OFFERED (asked), geomStageCreated (built), "
                      "and PipelineCache::Stats().geomStageWaited/geomStageWaitFailed",
                      primType, n, PrimTypeRefusalTotal());
    }
    return false;
}

uint64_t D3D12Backend::PrimTypeRefusalCount(uint32_t primType) {
    return primType < kMaxCountedPrimTypes ? g_primTypeRefusals[primType]
                                           : g_primTypeRefusalOther;
}

uint64_t D3D12Backend::PrimTypeRefusalTotal() {
    uint64_t total = g_primTypeRefusalOther;
    for (uint32_t i = 0; i < kMaxCountedPrimTypes; ++i) total += g_primTypeRefusals[i];
    return total;
}

// F-638: refusal buckets. Index 0..31 mirrors TextureRefusal so a caller can
// report plan->refusal directly. The "plan was fine but THIS backend could not
// carry it" cases (no host-format row, zero extent, D3D12 create failure, short
// payload) are not plan refusals and get their own total, g_textureImport-
// RefusalOther, rather than being filed under TextureRefusal::None where they
// would read as "no reason given".
uint64_t D3D12Backend::TextureImportRefusals(uint32_t reason) {
    return reason < 32 ? g_textureImportRefusals[reason] : g_textureImportRefusalOther;
}
uint64_t D3D12Backend::TextureImportRefusalTotal() {
    uint64_t total = g_textureImportRefusalOther;
    for (uint32_t i = 0; i < 32; ++i) total += g_textureImportRefusals[i];
    return total;
}
uint64_t D3D12Backend::ResolveRefusalCount() { return g_resolveRefusals; }

// F-649: the census accessors. Declared next to the F-638 refusal accessors
// above because they are the same accounting read from the other end: `total`
// counts what was attempted, `accepted` counts what got through, and the
// difference is attributable.
uint64_t D3D12Backend::TextureImportPlanCount() { return g_textureImportPlans; }
uint64_t D3D12Backend::TextureImportSuccessCount() { return g_textureImportOk; }

const char* D3D12Backend::TextureBackendRefusalName(TextureBackendRefusal reason) {
    switch (reason) {
        case TextureBackendRefusal::Unlisted: return "unlisted";
        case TextureBackendRefusal::LegacyUnsupportedDxgi: return "legacy_unsupported_dxgi";
        case TextureBackendRefusal::LegacyBadArgs: return "legacy_bad_args";
        case TextureBackendRefusal::NoHostFormat: return "no_host_format";
        case TextureBackendRefusal::ZeroExtent: return "zero_extent";
        case TextureBackendRefusal::MipCountOutOfRange: return "mip_count_out_of_range";
        case TextureBackendRefusal::MipChainAndGenerate: return "mip_chain_and_generate";
        case TextureBackendRefusal::MipLevelHasNoPayload: return "mip_level_has_no_payload";
        case TextureBackendRefusal::MipZeroHasNoPayload: return "mip_zero_has_no_payload";
        case TextureBackendRefusal::SrvSlotOutOfRange: return "srv_slot_out_of_range";
        case TextureBackendRefusal::ShortPayload: return "short_payload";
        case TextureBackendRefusal::MipgenFailed: return "mipgen_failed";
        case TextureBackendRefusal::kCount: return "kCount";
    }
    return "unlisted";
}

uint64_t D3D12Backend::TextureBackendRefusals(TextureBackendRefusal reason) {
    const uint32_t i = static_cast<uint32_t>(reason);
    return i < static_cast<uint32_t>(TextureBackendRefusal::kCount)
               ? g_textureImportBackendRefusals[i]
               : g_textureImportBackendRefusals[0];
}

uint64_t D3D12Backend::TextureBackendRefusalTotal() {
    uint64_t total = 0;
    for (uint32_t i = 0;
         i < static_cast<uint32_t>(TextureBackendRefusal::kCount); ++i) {
        total += g_textureImportBackendRefusals[i];
    }
    return total;
}


D3D12Backend::D3D12Backend() = default;

D3D12Backend::~D3D12Backend() {
    Shutdown();
}

bool D3D12Backend::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    if (m_initialized) return true;
    mcla::diag::RegisterGrowth("d3d12-backend", [this] {  // TEMP-GROWTH, racy reads, diag only
        return fmt::format("texCache n={} MB={} resCache n={} pso n={} texPoolSlots={} poolFree={} srvStagingNext={} srvFree={} frameUploadRes={} rtCache={} dsCache={} resolved={} resolvedAlt={} resolvedLastColour={} retiredTargets={} resourceStates={} texProbes={} samplerCache={} blend={} ds={} rast={} fbHost={}MB stencilDraws={} stencilRefSets={}",
            m_textureCache.Size(), m_textureCache.ResidentBytes() >> 20, m_resourceCache.Size(), m_pipelineCache.Size(),
            m_texturePoolSlots.size(), m_texturePoolFree.size(), m_srvStagingNext, m_srvStagingFree.size(), m_frameUploadResources.size(),
            m_rtCache.size(), m_dsCache.size(), m_resolved.size(), m_resolvedAlt.size(), m_resolvedLastColour.size(), m_retiredTargets.size(),
            m_resourceStates.size(), m_texProbes.size(), m_samplerCache.size(), m_blendStateCache.size(), m_depthStencilStateCache.size(),
            m_rasterizerStateCache.size(), m_fbHost.size() >> 20, g_stencilDraws.load(), g_stencilRefSets.load());
    });
    if (!hwnd) {
        MCLA_LOG_ERROR("D3D12Backend: Null HWND passed to Initialize");
        return false;
    }

    m_hwnd = hwnd;
    m_width = (width > 0) ? width : 1280;
    m_height = (height > 0) ? height : 720;
    m_isMinimized = false;

    // The extent is FIRST KNOWN right here, so this is where the clip_disable
    // rescale is published. It is deliberately ahead of CreateSwapChain and
    // therefore ahead of the caller starting the pipeline worker
    // (src/render_thread.cpp:121): the constants are baked into generated HLSL at
    // translation time, so a publication that waits for the first draw arrives
    // after that draw's vertex shader has already been translated and has already
    // baked the identity. See PublishClipDisableExtent above and the measured
    // ordering in build/w707.log.
    PublishClipDisableExtent("init", m_width, m_height, m_cachePipelineDraws);

    MCLA_LOG_INFO("D3D12Backend: Initializing backend for HWND 0x{:X} ({}x{})",
                reinterpret_cast<uintptr_t>(hwnd), m_width, m_height);
    DWORD winThread = GetWindowThreadProcessId(hwnd, nullptr);
    DWORD curThread = GetCurrentThreadId();
    MCLA_LOG_INFO("D3D12Backend: window owner thread={} calling thread={} {}",
                winThread, curThread, winThread == curThread ? "(MATCH)" : "(MISMATCH)");
    {
        char cls[256] = {};
        GetClassNameA(hwnd, cls, 256);
        MCLA_LOG_INFO("D3D12Backend: window class='{}' visible={}",
                    cls, IsWindowVisible(hwnd) ? 1 : 0);
    }

    if (!CreateDevice()) return false;
    if (!CreateCommandObjects()) return false;
    if (!CreateCopyQueue()) return false;
    if (!CreateSwapChain(hwnd, m_width, m_height)) return false;
    if (!CreateRenderTargets()) return false;
    if (!CreateSyncObjects()) return false;
    if (!CreateUploadHeap()) return false;
    if (!CreateTestRootSignature()) return false;
    // Pre-compiled shader root signature (Step 6). Non-fatal: the live path
    // still uses m_rootSignature; this only enables pre-compiled PSOs.
    if (!CreatePrecompiledRootSignature()) {
        MCLA_LOG_WARN("D3D12Backend: precompiled root signature unavailable; "
                      "pre-compiled shaders will not be substitutable yet");
    }
    if (!CreatePrecompiledDescriptorHeaps()) {
        MCLA_LOG_WARN("D3D12Backend: precompiled descriptor heaps unavailable; "
                      "pre-compiled shaders will not be substitutable yet");
    }
    if (!CreatePrecompiledSharedConstantsBuffer()) {
        MCLA_LOG_WARN("D3D12Backend: precompiled SharedConstants buffer unavailable; "
                      "pre-compiled shaders will not be substitutable yet");
    }
    if (!CreatePrecompiledTestPipeline()) {
        MCLA_LOG_WARN("D3D12Backend: precompiled test PSO unavailable; "
                      "pre-compiled shaders will not be substitutable yet");
    }
    if (!CreateTestPipeline()) return false;
    if (!CreateStaticIndexBuffer()) return false;
    if (!CreateSrvHeap()) return false;
    if (!CreateStagingSrvHeap()) return false;
    if (!CreateSamplerHeap()) return false;

    // F-643: the format table's isResolvable column and Xenia's ColorFormat set
    // must be the same set, or every BuildResolveDesc refusal reason in
    // texture_decode.cpp is wrong in the same direction. One shot at init, not a
    // per-resolve cost (texture_decode.h:467 asks for exactly this).
    {
        uint32_t offender = 0;
        if (!native::VerifyResolvableColumnMatchesXeniaColorFormat(&offender)) {
            MCLA_LOG_ERROR("D3D12Backend: format table isResolvable disagrees with Xenia "
                           "ColorFormat at xenosFormat {} ({}) -- resolve refusals are untrusted",
                           offender, native::TextureFormatName(offender));
        }
    }

    // F-641: warm the mipgen CS compile here, at init, rather than inside
    // GenerateMipmaps. It is a DXC Load + fxc compile; leaving it on the draw
    // path meant the first draw needing a mip chain stalled between MapUpload
    // and DrawIndexedInstanced. Failure here is NOT fatal to Initialize: no
    // guest texture has needed mip generation yet, and GenerateMipmaps refuses
    // loudly with the recorded reason when one does.
    if (!EnsureMipGenPipeline()) {
        MCLA_LOG_ERROR("D3D12Backend: mipgen PSO warm-up failed at init; GenerateMipmaps will "
                       "refuse every mip chain until this is fixed");
    }

    // Initialize pipeline cache worker thread and set fallback PSO (test pipeline)
    m_pipelineCache.StartWorker(m_device, m_rootSignature);
    m_pipelineCache.SetFallbackPipeline(m_testPipeline);

    // §3.5: decide the AMD MSAA line guard once. No PSOs are compiled and no
    // line draws are diverted in this increment: every render target in this
    // tree is SampleDesc.Count=1, so the guard stays zero-cost by construction.
    // §2.3: pick up the aspect value that LoadConfig() installed before
    // InitD3D12 ran (config loads are silent by design, cvar.h:277-284).
    m_graphicsSettings.guestAspectRatio = AspectFromSetting(
        mcla::cvar::CVarSystem::Instance().GetString("mcla_aspect_ratio"));

    const int msaaGuardMode =
        mcla::cvar::CVarSystem::Instance().GetInt("mcla_d3d12_msaa_line_guard");
    m_msaaLineGuardEnabled =
        (msaaGuardMode == 1) || (msaaGuardMode < 0 && m_adapterVendorId == 0x1002u);
    MCLA_LOG_INFO("MSAA-LINE-GUARD vendor=0x{:04X} mode={} active={} "
                  "guardPSOs=NONE rtSamples=1 drawsDiverted=0",
                  m_adapterVendorId, msaaGuardMode,
                  m_msaaLineGuardEnabled ? 1 : 0);

    m_initialized = true;
    MCLA_LOG_INFO("D3D12Backend: Successfully initialized D3D12 native renderer skeleton + Phase 3 test pipeline");
    return true;
}

bool D3D12Backend::CreateDevice() {
    UINT dxgiFactoryFlags = 0;
    bool debugLayerRequested = false;
    bool infoQueueRequested = false;

    {
        char debugLayerSetting[2] = {};
        const DWORD debugLayerSettingSize = GetEnvironmentVariableA(
            "MCLA_D3D12_DEBUG_LAYER", debugLayerSetting,
            static_cast<DWORD>(sizeof(debugLayerSetting)));
#if !defined(NDEBUG)
        constexpr bool kDebugLayerEnabledByDefault = true;
#else
        constexpr bool kDebugLayerEnabledByDefault = false;
#endif
        // F-XXX: MCLA_D3D12_INFOQUEUE=1 also requests the debug layer, so
        // the validation messages below have something to print. The driver
        // faults silently in release (OMSET C0000005, device alive), so the
        // info queue is the only way to learn the exact misuse it trips on.
        char infoQueueSetting[2] = {};
        const DWORD infoQueueSettingSize = GetEnvironmentVariableA(
            "MCLA_D3D12_INFOQUEUE", infoQueueSetting,
            static_cast<DWORD>(sizeof(infoQueueSetting)));
        infoQueueRequested =
            (infoQueueSettingSize == 1 && infoQueueSetting[0] == '1');
        debugLayerRequested =
            kDebugLayerEnabledByDefault || infoQueueRequested ||
            (debugLayerSettingSize == 1 && debugLayerSetting[0] == '1');
        if (debugLayerRequested) {
            Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
            const HRESULT debugHr =
                D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
            if (SUCCEEDED(debugHr)) {
                debugController->EnableDebugLayer();
                dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                MCLA_LOG_INFO("D3D12Backend: Direct3D 12 Debug Layer enabled");
            } else {
                MCLA_LOG_WARN(
                    "D3D12Backend: debug layer requested but "
                    "D3D12GetDebugInterface failed with hr=0x{:08X}",
                    static_cast<uint32_t>(debugHr));
            }
        }
    }

    HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateDXGIFactory2 failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> hardwareAdapter;
    bool adapterSelected = false;
    for (UINT adapterIndex = 0; SUCCEEDED(m_factory->EnumAdapters1(adapterIndex, &hardwareAdapter)); ++adapterIndex) {
        DXGI_ADAPTER_DESC1 desc;
        hardwareAdapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (SUCCEEDED(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
            // §3.5: cache the vendor of the adapter actually selected.
            m_adapterVendorId = desc.VendorId;
            char descBuf[128] = {};
            size_t converted = 0;
            wcstombs_s(&converted, descBuf, sizeof(descBuf) - 1, desc.Description, _TRUNCATE);
            MCLA_LOG_INFO("D3D12Backend: Selected GPU adapter: {}", descBuf);
            adapterSelected = true;
            break;
        }
    }

    if (!adapterSelected) {
        // Loop always leaves hardwareAdapter non-null after the first iteration,
        // so clear it explicitly before the WARP fallback.
        MCLA_LOG_WARN("D3D12Backend: No hardware D3D12 adapter found, attempting WARP software adapter");
        hardwareAdapter = nullptr;
        m_factory->EnumWarpAdapter(IID_PPV_ARGS(&hardwareAdapter));
        if (hardwareAdapter) {
            DXGI_ADAPTER_DESC1 warpDesc{};
            hardwareAdapter->GetDesc1(&warpDesc);
            m_adapterVendorId = warpDesc.VendorId;
        }
    }

    if (!hardwareAdapter) {
        MCLA_LOG_ERROR("D3D12Backend: No D3D12-capable adapter (hardware or WARP) available");
        return false;
    }

    // F-653: the requested level is a NAMED value because it is also the
    // measurement below. `ID3D12Device::GetFeatureLevel()` DOES NOT EXIST in
    // either installed Windows SDK -- measured, not recalled: 0 occurrences of
    // the name in `um\d3d12.h` for 10.0.26100.0 and for 10.0.22621.0, and
    // neither SDK's D3D12_FEATURE_DATA_D3D12_OPTIONS3 carries a
    // GeometryShaderTier field either. A SUCCESSFUL create at a requested level
    // is itself the fact: the runtime returns a device at that level or fails,
    // and the failure path is the very next line. The requested level reached
    // successfully IS the device's level.
    constexpr D3D_FEATURE_LEVEL kRequestedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    hr = D3D12CreateDevice(hardwareAdapter.Get(), kRequestedFeatureLevel, IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: D3D12CreateDevice failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    // F-XXX: D3D12 INFO QUEUE (pull model) -- the driver does not explain
    // itself by default, and our fault (C0000005 inside the vendor driver
    // during OMSetRenderTargets, device alive, presents all S_OK) is silent
    // without it. The stored messages are dumped into the OMSET-FAULT row,
    // so the driver's own validation complaint is next to the fault it
    // precedes. Opt-in with MCLA_D3D12_INFOQUEUE=1 (which also requests the
    // debug layer above); OFF in normal runs.
    if (debugLayerRequested) {
        const HRESULT iqHr = m_device.As(&m_infoQueue);
        if (SUCCEEDED(iqHr) && m_infoQueue) {
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            m_infoQueue->ClearStoredMessages();
            m_infoQueue->AddApplicationMessage(
                D3D12_MESSAGE_SEVERITY_INFO, "MCLA info-queue self-test");
            const uint64_t selfN = m_infoQueue->GetNumStoredMessages();
            alignas(8) uint8_t selfBuf[1024];
            SIZE_T selfLen = 0;
            const HRESULT selfHr = m_infoQueue->GetMessage(
                0u, reinterpret_cast<D3D12_MESSAGE*>(selfBuf), &selfLen);
            m_infoQueue->ClearStoredMessages();
            MCLA_LOG_ERROR("D3D12-INFOQUEUE-SELFTEST stored={} getHr=0x{:08X} "
                           "len={} -- 1 means the queue really arms and "
                           "0 means the debug layer is a no-op on this machine",
                           selfN, static_cast<uint32_t>(selfHr),
                           static_cast<unsigned long long>(selfLen));
        } else {
            MCLA_LOG_WARN("D3D12Backend: debug layer on but ID3D12InfoQueue "
                          "unavailable hr=0x{:08X}", static_cast<uint32_t>(iqHr));
        }
    }

    // Push the debug-layer messages (ERROR/CORRUPTION, capped) into the log:
    // the pull-model dump above only fires on the OMSET fault path.
    if (debugLayerRequested && m_infoQueue) {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue1> iq1;
        if (SUCCEEDED(m_infoQueue.As(&iq1))) {
            DWORD cookie = 0;
            const HRESULT cbHr = iq1->RegisterMessageCallback(
                [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id,
                   LPCSTR desc, void*) {
                    // per-id cap: one noisy id (527) used to eat the whole budget (gt1.log)
                    static std::atomic<uint8_t> perId[4096]{};
                    const uint32_t slot = static_cast<uint32_t>(id) & 4095u;
                    if (sev <= D3D12_MESSAGE_SEVERITY_ERROR && perId[slot].fetch_add(1) < 3)
                        MCLA_LOG_ERROR("D3D12-DEBUGLAYER sev={} id={} {}", static_cast<int>(sev),
                                       static_cast<int>(id), desc ? desc : "");
                },
                D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
            MCLA_LOG_INFO("D3D12Backend: InfoQueue1 message callback hr=0x{:08X}",
                          static_cast<uint32_t>(cbHr));
        }
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {};
    shaderModel.HighestShaderModel = D3D_SHADER_MODEL_6_0;
    const HRESULT shaderModelHr = m_device->CheckFeatureSupport(
        D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
    if (SUCCEEDED(shaderModelHr)) {
        MCLA_LOG_INFO("D3D12Backend: shader model capability query requested=0x{:X} "
                      "highestSupported=0x{:X}",
                      static_cast<uint32_t>(D3D_SHADER_MODEL_6_0),
                      static_cast<uint32_t>(shaderModel.HighestShaderModel));
    } else {
        MCLA_LOG_WARN("D3D12Backend: shader model capability query failed "
                      "hr=0x{:08X}",
                      static_cast<uint32_t>(shaderModelHr));
    }

    // F-653: MEASURE the feature level instead of assuming the one requested.
    //
    // The rectangle-list expansion is a GEOMETRY STAGE (pipeline_cache.cpp,
    // BuildRectangleGeometryShaderHlsl), and a D3D12 geometry stage requires
    // feature level 11_0. 11_0 is what is requested above; a driver is free to
    // hand back a lower level for an adapter it decided was software, and it has
    // already done so or failed by the time control reaches here -- so the
    // reached level is a fact and not an assumption.
    //
    // This is the check that answers Task 1's first question at runtime, and it
    // is deliberately a MEASUREMENT and not a gate: the device is not refused
    // here, because a device below 11_0 can still run every non-rectangle draw.
    // Rectangle draws take the refusal, in the cache, with the name
    // `geom-stage-unsupported-at-this-feature-level` -- so a soak sees a
    // counted reason instead of a silently black rectangle area.
    {
        const D3D_FEATURE_LEVEL actual = kRequestedFeatureLevel;
        m_featureLevel = actual;
        const bool gsCapable = actual >= D3D_FEATURE_LEVEL_11_0;
        m_geometryStageSupported = gsCapable;
        // F-653: hand the MEASUREMENT to the pipeline cache, so a rectangle draw
        // is refused by the thing that compiles it rather than only described in
        // this one log line. The cache's default is false, so this assignment is
        // what makes rectangles possible at all on a capable device.
        m_pipelineCache.SetGeometryStageSupported(gsCapable);
        MCLA_LOG_INFO("D3D12Backend: feature level 0x{:04X} ({}) geometry-stage "
                      "capable = {} -- F-653",
                      static_cast<uint32_t>(actual),
                      actual == D3D_FEATURE_LEVEL_12_0 ? "12.0"
                      : actual == D3D_FEATURE_LEVEL_11_1 ? "11.1"
                      : actual == D3D_FEATURE_LEVEL_11_0 ? "11.0"
                      : actual == D3D_FEATURE_LEVEL_10_1 ? "10.1"
                      : actual == D3D_FEATURE_LEVEL_10_0 ? "10.0"
                                                         : "9.x",
                      gsCapable ? "yes" : "NO (rectangle draws will refuse)");
    }

    return true;
}

uint32_t D3D12Backend::ConfiguredMtWorkerCount() {
    static const uint32_t s_workers = []() -> uint32_t {
        const char* noMt = std::getenv("MCLA_NO_MT_BURSTS");
        if (noMt && noMt[0] == '1') return 1u;
        auto parseCount = [](const char* e, uint32_t& out) -> bool {
            if (!e || !e[0]) return false;
            char* end = nullptr;
            const unsigned long v = std::strtoul(e, &end, 10);
            if (end == e) return false;
            out = (v <= 1) ? 1u : std::min<uint32_t>(static_cast<uint32_t>(v), kMaxMtRecorders);
            return true;
        };
        uint32_t n = 1;
        if (parseCount(std::getenv("MCLA_MT_WORKERS"), n)) return n;
        const unsigned hw = std::thread::hardware_concurrency();
        return (hw >= 8u) ? 4u : ((hw >= 4u) ? 2u : 1u);
    }();
    return s_workers;
}

bool D3D12Backend::LookupPersistentGeo(uint64_t key, uint64_t contentHash,
                                       D3D12_GPU_VIRTUAL_ADDRESS& outGpuVa,
                                       uint32_t& outSizeBytes) const {
    outGpuVa = 0;
    outSizeBytes = 0;
    if (key == 0 || !m_initialized) return false;
    auto it = m_persistentGeoCache.find(key);
    if (it == m_persistentGeoCache.end() || !it->second.resource) return false;
    if (contentHash != 0 && it->second.contentHash != contentHash) return false;
    outGpuVa = it->second.gpuVa;
    outSizeBytes = it->second.sizeBytes;
    return outGpuVa != 0;
}

bool D3D12Backend::UploadPersistentGeo(uint64_t key, uint64_t contentHash,
                                       const void* data, uint32_t sizeBytes,
                                       D3D12_RESOURCE_STATES targetState,
                                       D3D12_GPU_VIRTUAL_ADDRESS& outGpuVa,
                                       std::vector<uint64_t>* outEvictedKeys) {
    outGpuVa = 0;
    if (outEvictedKeys) outEvictedKeys->clear();
    if (key == 0 || !data || sizeBytes == 0 || sizeBytes > (64u << 20)) return false;
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || !m_device || !m_commandList || !m_mainListOpen) return false;

    auto existing = m_persistentGeoCache.find(key);
    if (existing != m_persistentGeoCache.end()) {
        if (existing->second.resource && existing->second.contentHash == contentHash &&
            existing->second.sizeBytes == sizeBytes) {
            existing->second.lastUsedSeq = ++m_persistentGeoUseSeq;
            outGpuVa = existing->second.gpuVa;
            return true;
        }
        if (existing->second.resource) RetireTarget(existing->second.resource);
        m_persistentGeoBytes -= existing->second.sizeBytes;
        m_persistentGeoCache.erase(existing);
    }

    while (!m_persistentGeoCache.empty() &&
           (m_persistentGeoCache.size() >= kMaxPersistentGeoEntries ||
            m_persistentGeoBytes + sizeBytes > kMaxPersistentGeoBytes)) {
        auto victim = m_persistentGeoCache.begin();
        for (auto it = m_persistentGeoCache.begin(); it != m_persistentGeoCache.end(); ++it) {
            if (it->second.lastUsedSeq < victim->second.lastUsedSeq) victim = it;
        }
        if (outEvictedKeys) outEvictedKeys->push_back(victim->first);
        if (victim->second.resource) RetireTarget(victim->second.resource);
        m_persistentGeoBytes -= victim->second.sizeBytes;
        m_persistentGeoCache.erase(victim);
    }

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = sizeBytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&res))) || !res) {
        return false;
    }

    ID3D12Resource* uploadRes = nullptr;
    uint8_t* uploadCpu = nullptr;
    uint64_t uploadOffset = 0;
    if (!AllocateTexUploadSlice(sizeBytes, uploadRes, uploadCpu, uploadOffset) || !uploadRes || !uploadCpu) {
        RetireTarget(res);
        return false;
    }
    std::memcpy(uploadCpu, data, sizeBytes);
    m_commandList->CopyBufferRegion(res.Get(), 0, uploadRes, uploadOffset, sizeBytes);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = targetState;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &b);

    PersistentGeoEntry ent;
    ent.gpuVa = res->GetGPUVirtualAddress();
    ent.resource = res;
    ent.contentHash = contentHash;
    ent.sizeBytes = sizeBytes;
    ent.lastUsedSeq = ++m_persistentGeoUseSeq;
    m_persistentGeoBytes += sizeBytes;
    outGpuVa = ent.gpuVa;
    m_persistentGeoCache.emplace(key, std::move(ent));
    return outGpuVa != 0;
}

void D3D12Backend::ReleasePersistentGeo(uint64_t key) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_persistentGeoCache.find(key);
    if (it == m_persistentGeoCache.end()) return;
    if (it->second.resource) RetireTarget(it->second.resource);
    m_persistentGeoBytes -= it->second.sizeBytes;
    m_persistentGeoCache.erase(it);
}

static std::atomic<uint32_t> s_frameSrvRingCursor{0};

bool D3D12Backend::CreateCommandObjects() {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateCommandQueue failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    for (uint32_t i = 0; i < kBufferCount; ++i) {
        hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i]));
        if (FAILED(hr)) {
            MCLA_LOG_ERROR("D3D12Backend: CreateCommandAllocator[{}] failed with hr=0x{:08X}", i, static_cast<uint32_t>(hr));
            return false;
        }
    }

    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateCommandList failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    m_commandList->Close();
    m_mainListOpen = false;

    // Parallel command recorders for Milestone 2 (MCLA_MT_WORKERS=N, default auto).
    {
        const uint32_t n = ConfiguredMtWorkerCount();
        m_mtRecorderCount = n;
        const uint32_t ringCap = SrvRingTablesPerFrame();
        static const bool s_noSrvRingExpand = [] {
            const char* e = std::getenv("MCLA_NO_SRV_RING_EXPAND");
            return e && e[0] == '1';
        }();
        if (m_mtRecorderCount > 1) {
            if (!s_noSrvRingExpand) {
                // Dynamic allocation from full ringCap via atomic cursor: main list and all MT
                // recorders share the full 10,240 tables/frame without rigid 2048-table clamping.
                m_mainSrvRingTables = ringCap;
                m_mtSrvRingTablesPerRecorder = ringCap;
                for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
                    m_mtSrvRingBase[r] = 0;
                    m_mtSrvRingOffset[r] = 0;
                    m_mtRecorderState[r] = DrawRecorder{};
                    m_mtRecorderState[r].index = r;
                    for (uint32_t f = 0; f < kBufferCount; ++f) {
                        hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_mtAllocators[f][r]));
                        if (FAILED(hr)) {
                            MCLA_LOG_ERROR("D3D12Backend: CreateCommandAllocator MT[f={} r={}] failed hr=0x{:08X}", f, r, static_cast<uint32_t>(hr));
                            m_mtRecorderCount = 1;
                            m_mainSrvRingTables = ringCap;
                            m_mtSrvRingTablesPerRecorder = ringCap;
                            return true;
                        }
                    }
                    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_mtAllocators[0][r].Get(), nullptr, IID_PPV_ARGS(&m_mtCommandLists[r]));
                    if (FAILED(hr)) {
                        MCLA_LOG_ERROR("D3D12Backend: CreateCommandList MT[r={}] failed hr=0x{:08X}", r, static_cast<uint32_t>(hr));
                        m_mtRecorderCount = 1;
                        m_mainSrvRingTables = ringCap;
                        m_mtSrvRingTablesPerRecorder = ringCap;
                        return true;
                    }
                    m_mtCommandLists[r]->Close();
                    m_mtRecorderState[r].list = m_mtCommandLists[r].Get();
                    m_mtRecorderState[r].listOpen = false;
                }
                MCLA_LOG_INFO("D3D12Backend: SRV ring expand active: dynamic cursor over full ringCap={} tables/frame (W={}, main srvTables={}, worker srvTables={})",
                              ringCap, m_mtRecorderCount, m_mainSrvRingTables, m_mtSrvRingTablesPerRecorder);
            } else {
                // Legacy partitioned path: Slice 0 (m_mainSrvRingTables) belongs exclusively to m_commandList;
                // recorders 0..m_mtRecorderCount-1 own disjoint contiguous slices after it.
                m_mtSrvRingTablesPerRecorder = ringCap / (m_mtRecorderCount + 1);
                m_mainSrvRingTables = ringCap - m_mtRecorderCount * m_mtSrvRingTablesPerRecorder;
                for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
                    m_mtSrvRingBase[r] = m_mainSrvRingTables + r * m_mtSrvRingTablesPerRecorder;
                    m_mtSrvRingOffset[r] = 0;
                    m_mtRecorderState[r] = DrawRecorder{};
                    m_mtRecorderState[r].index = r;
                    for (uint32_t f = 0; f < kBufferCount; ++f) {
                        hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_mtAllocators[f][r]));
                        if (FAILED(hr)) {
                            MCLA_LOG_ERROR("D3D12Backend: CreateCommandAllocator MT[f={} r={}] failed hr=0x{:08X}", f, r, static_cast<uint32_t>(hr));
                            m_mtRecorderCount = 1;
                            m_mainSrvRingTables = ringCap;
                            m_mtSrvRingTablesPerRecorder = ringCap;
                            return true; // fall back to single-recorder path
                        }
                    }
                    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_mtAllocators[0][r].Get(), nullptr, IID_PPV_ARGS(&m_mtCommandLists[r]));
                    if (FAILED(hr)) {
                        MCLA_LOG_ERROR("D3D12Backend: CreateCommandList MT[r={}] failed hr=0x{:08X}", r, static_cast<uint32_t>(hr));
                        m_mtRecorderCount = 1;
                        m_mainSrvRingTables = ringCap;
                        m_mtSrvRingTablesPerRecorder = ringCap;
                        return true;
                    }
                    m_mtCommandLists[r]->Close();
                    m_mtRecorderState[r].list = m_mtCommandLists[r].Get();
                    m_mtRecorderState[r].listOpen = false;
                }
                MCLA_LOG_INFO("D3D12Backend: MT recorders enabled W={} (main srvTables={}, worker srvTables={})",
                              m_mtRecorderCount, m_mainSrvRingTables, m_mtSrvRingTablesPerRecorder);
            }
        } else {
            m_mainSrvRingTables = ringCap;
            m_mtSrvRingTablesPerRecorder = ringCap;
            MCLA_LOG_INFO("D3D12Backend: MT workers disabled (W=1, main srvTables={})", m_mainSrvRingTables);
        }
    }
    return true;
}

bool D3D12Backend::CreateCopyQueue() {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_copyQueue));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateCopyQueue failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&m_copyAllocator));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateCopyAllocator failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_copyAllocator.Get(), nullptr, IID_PPV_ARGS(&m_copyCommandList));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateCopyCommandList failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    m_copyCommandList->Close();

    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_copyFence));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateCopyFence failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    m_copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_copyFenceEvent) {
        MCLA_LOG_ERROR("D3D12Backend: CreateEvent(copy fence) failed");
        return false;
    }

    MCLA_LOG_INFO("D3D12Backend: Copy queue created (type=COPY)");
    return true;
}

bool D3D12Backend::CreateSwapChain(HWND hwnd, uint32_t width, uint32_t height) {
    const uint32_t requestedW = width, requestedH = height;  // window/display mode before any small-swap override
    // PERF/FS: a 1280x720 back buffer kept the scan-out blit cheap, but in
    // *exclusive* fullscreen DXGI needs the swap-chain back buffer to match the
    // display mode; a mismatched size drops exclusive mode (title bar/taskbar
    // reappear — P6 round 2). Default is now the full display size, so the
    // default path is visually correct. Opt into the cheap small back buffer
    // with MCLA_SMALL_FS_SWAP=1; use it with borderless_fullscreen, where DXGI
    // stretches the 1280x720 back buffer to the full-screen window.
    const char* dispMode = std::getenv("MCLA_DISPLAY_MODE");
    static const bool s_smallFsSwap = [] {
        const char* opt = std::getenv("MCLA_SMALL_FS_SWAP");
        return opt && opt[0] == '1';
    }();
    if (s_smallFsSwap) { width = 1280; height = 720; }
    m_swapW = width;
    m_swapH = height;
    MCLA_LOG_ERROR("FS-DISPLAY-FIX swapchain mode={} small={} backbuffer={}x{}",
                   dispMode ? dispMode : "(default)", s_smallFsSwap ? 1 : 0, width, height);
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = kBufferCount;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    {   // Uncapped presents need tearing support (flip model + vsync off), else Present(0) still waits.
        Microsoft::WRL::ComPtr<IDXGIFactory5> f5;
        BOOL allow = FALSE;
        if (SUCCEEDED(m_factory.As(&f5)) &&
            SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            m_tearingSupported = allow != FALSE;
        if (m_tearingSupported) swapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        // Native build: no 60 Hz cap by default; MCLA_VSYNC=1 restores vsync.
        const char* e = std::getenv("MCLA_VSYNC");
        m_graphicsSettings.vsync = e && e[0] == '1';
        MCLA_LOG_INFO("D3D12Backend: tearing={} vsync={}", m_tearingSupported ? 1 : 0, m_graphicsSettings.vsync ? 1 : 0);
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    HRESULT hr = m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain);

    // 2026-10-02: NOT modified for the post-draw readback. A first attempt
    // asked for `DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_COPY_SRC` on the
    // theory that CopyTextureRegion out of a swap-chain back buffer needs a
    // copy usage. It does not, and the theory was wrong in a way worth
    // recording: measured against the toolchain this build uses
    // (Windows Kits 10.0.26100.0), `shared/dxgi.h:168-174` defines exactly
    // seven DXGI_USAGE_* macros -- SHADER_INPUT, RENDER_TARGET_OUTPUT,
    // BACK_BUFFER, SHARED, READ_ONLY, DISCARD_ON_PRESENT, UNORDERED_ACCESS --
    // and there is no COPY_SRC among them, nor anywhere in any dxgi*.h in the
    // SDK. `D3D12_RESOURCE_FLAG_ALLOW_COPY_SRC` does not exist either
    // (`um/d3d12.h` has ALLOW_RENDER_TARGET at :3070 and no copy-source flag):
    // every D3D12 resource is copyable unless DENY_SHADER_RESOURCE is set, so a
    // flip-model back buffer needs no extra usage and none is requested here.
    // The legality of the copy is therefore PROBED at runtime instead of
    // assumed -- see the readback's own gate.

    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateSwapChainForHwnd failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    hr = swapChain.As(&m_swapChain);
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: Query IDXGISwapChain3 interface failed");
        return false;
    }

    // P6 review round 2: SDL_WINDOW_FULLSCREEN only changes the Win32 window state.
    // No code in src/ ever called IDXGISwapChain::SetFullscreenState, and the
    // MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER) above also blocks Alt+Enter, so
    // DXGI stayed in windowed flip-model and dxcam captured a title bar/taskbar.
    // Engage a real DXGI exclusive-fullscreen transition here (default ON for
    // MCLA_DISPLAY_MODE=fullscreen; MCLA_NO_EXCLUSIVE_FS=1 opts out). On failure the
    // SDL borderless-fullscreen window remains, i.e. the borderless fallback is
    // preserved and still covers the display; the ERROR line names the failure.
    static const bool s_tryExclusiveFs = [] {
        const char* mode = std::getenv("MCLA_DISPLAY_MODE");
        const char* off = std::getenv("MCLA_NO_EXCLUSIVE_FS");
        return mode && std::strcmp(mode, "fullscreen") == 0 && !(off && off[0] == '1');
    }();
    constexpr uint64_t kSdlWindowFullscreen = 0x1;  // SDL_video.h: SDL_WINDOW_FULLSCREEN
    const uint64_t sdlFlags = GetSdlWindowFlags();
    if (s_tryExclusiveFs) {
        HRESULT fsHr = m_swapChain->SetFullscreenState(TRUE, nullptr);  // nullptr: DXGI picks hwnd's output
        BOOL active = FALSE;
        Microsoft::WRL::ComPtr<IDXGIOutput> actualOut;
        HRESULT getHr = m_swapChain->GetFullscreenState(&active, actualOut.GetAddressOf());
        // IDXGIOutput has no GetDisplayMode; report the confirmed backbuffer size.
        uint32_t actualW = SUCCEEDED(getHr) ? width : 0u;
        uint32_t actualH = SUCCEEDED(getHr) ? height : 0u;
        (void)actualOut;
        MCLA_LOG_ERROR("FULLSCREEN-STATE requested=exclusive setHr=0x{:08X} active={} getHr=0x{:08X} "
                       "req={}x{} backbuffer={}x{} actual={}x{} sdlFlags=0x{:X} sdlFullscreen={}",
                       static_cast<uint32_t>(fsHr), active ? 1 : 0, static_cast<uint32_t>(getHr),
                       requestedW, requestedH, width, height, actualW, actualH,
                       sdlFlags, (sdlFlags & kSdlWindowFullscreen) ? 1 : 0);
    } else {
        MCLA_LOG_ERROR("FULLSCREEN-STATE requested=none (mode={} or MCLA_NO_EXCLUSIVE_FS=1) "
                       "req={}x{} backbuffer={}x{} sdlFlags=0x{:X} sdlFullscreen={}",
                       dispMode ? dispMode : "(default)", requestedW, requestedH, width, height,
                       sdlFlags, (sdlFlags & kSdlWindowFullscreen) ? 1 : 0);
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    // F-761: the chain has just been created, so all kBufferCount back buffers
    // are new resources with undefined contents. Recorded rather than left to the
    // member initialiser, because Initialize() calls this on a backend whose
    // members may already have been written by an earlier failed attempt.
    ResetBufferLedger("CreateSwapChain");
    return true;
}

bool D3D12Backend::CreateRenderTargets() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kBufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateDescriptorHeap (RTV) failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    for (uint32_t i = 0; i < kBufferCount; ++i) {
        hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (FAILED(hr)) {
            MCLA_LOG_ERROR("D3D12Backend: GetBuffer[{}] failed with hr=0x{:08X}", i, static_cast<uint32_t>(hr));
            return false;
        }
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    return true;
}

// MCLA_RENDER_SCALE (integer 1..3, default 1): internal resolution multiplier applied ONLY to the
// physical host colour/depth targets and everything that copies between them (resolve boxes, resolved
// textures, clears with rects, draw viewport/scissor). Every cache key, GuestRtEntry/GuestDsEntry/ResolvedTex
// .w/.h and guest register value stays in GUEST pixels; each guest-pixel -> host-pixel conversion multiplies
// by this exactly once. 1 = the unmodified code path. Guest-memory readbacks that assume 1:1 pixels are off
// when > 1.
static uint32_t RenderScale() {
    static const uint32_t s = [] {
        const char* e = std::getenv("MCLA_RENDER_SCALE");
        const unsigned long v = e ? std::strtoul(e, nullptr, 10) : 1ul;
        const uint32_t r = (v >= 1ul && v <= 3ul) ? static_cast<uint32_t>(v) : 1u;
        if (r > 1u) MCLA_LOG_INFO("RENDER-SCALE {}x: host render targets/depth/resolves are {}x guest size", r, r);
        return r;
    }();
    return s;
}

static bool ResolvedMipsDisabled() {  // MD-09 off-switch
    static const bool s = [] { const char* e = std::getenv("MCLA_NO_RESOLVED_MIPS"); return e && e[0] == '1'; }();
    return s;
}

// ---------------------------------------------------------------------------
// F-XXX: the GUEST'S RENDER TARGET ("EDRAM").
//
// The console renders into EDRAM and copies it out to the scan-out surface;
// this host rendered straight into the scan-out surface, so the two were the
// same object and the console's copy step did not exist. The citations for
// the guest's own registers (RB_SURFACE_INFO, RB_COLOR_INFO, the screen
// scissor, RB_MODECONTROL and the four RB_COPY_* registers) are in
// src/gpu_cp.h, with the PE-image bytes and the running values.
//
// THE TEXTURE. One committed 2D texture in the DEFAULT heap, RENDER_TARGET +
// COPY_SRC (COPY_SRC because the resolve COPIES it into the scan-out
// surface; a swap-chain back buffer cannot be the copy source, so the copy
// direction is fixed by what the hardware allows).
//
// THE REFUSAL IS COUNTED AND NAMED, because "the guest's render target was
// not used" is a fact a reader needs and an absence cannot carry.
// ---------------------------------------------------------------------------
void D3D12Backend::GuestRenderTargetRefused(const char* why, uint32_t hr) {
    const uint64_t n = ++m_guestRtRefusals;
    if (ShouldLogRefusal(n)) {
        MCLA_LOG_WARN(
            "GUEST-RT-REFUSED #{} why={} hr=0x{:08X} -- the guest's render target "
            "was NOT used and the guest's draws went into the swap-chain back "
            "buffer exactly as they did before this existed. hr=0xFFFFFFFF means "
            "the refusal was not an HRESULT (no D3D12 call was made). totals "
            "refusals={} created={} resolves={}",
            static_cast<uint32_t>(n), why, hr,
            m_guestRtRefusals,
            m_guestRtCreated,
            m_guestRtResolves);
    }
}

// TEMP-RMCHK: first call site at which GetDeviceRemovedReason is non-S_OK.
#define TEMP_RMCHK(where) do { static bool s_rm = false; if (!s_rm && m_device) {     const HRESULT rmr = m_device->GetDeviceRemovedReason(); if (rmr != S_OK) { s_rm = true;     MCLA_LOG_ERROR("TEMP-RMCHK first removed at {} reason=0x{:08X} retired={} drawsThisFrame={} binds={}",         where, static_cast<uint32_t>(rmr), m_retiredTargets.size(), m_drawsThisFrame, m_textureBindPasses); } } } while (0)

void D3D12Backend::RetireTarget(Microsoft::WRL::ComPtr<ID3D12Pageable> res) {
    if (!res) return;
    {   // AUDIT_MEMORY item 1: a reused address must not inherit a dead resource's barrier state.
        Microsoft::WRL::ComPtr<ID3D12Resource> asRes;
        if (SUCCEEDED(res.As(&asRes))) m_resourceStates.erase(asRes.Get());
    }
    // The open list may reference it, so its fence value is unknown until the
    // present path executes that list and Signals (it stamps UINT64_MAX entries).
    m_retiredTargets.emplace_back(UINT64_MAX, std::move(res));
}

void D3D12Backend::PruneRetiredTargets() {
    if (!m_fence) return;
    if (m_retiredTargets.empty() && m_retiredPlacedRanges.empty() && m_texUploadChunks.empty()) return;
    const uint64_t done = m_fence->GetCompletedValue();
    std::erase_if(m_retiredTargets, [done](const auto& r) { return r.first <= done; });
    for (size_t i = 0; i < m_retiredPlacedRanges.size(); ) {
        if (m_retiredPlacedRanges[i].fenceValue <= done) {
            FreeTextureHeapRange(m_retiredPlacedRanges[i].heapIndex,
                                 m_retiredPlacedRanges[i].offset,
                                 m_retiredPlacedRanges[i].size);
            m_retiredPlacedRanges[i] = m_retiredPlacedRanges.back();
            m_retiredPlacedRanges.pop_back();
        } else {
            ++i;
        }
    }
    for (auto& chunk : m_texUploadChunks) {
        if (!chunk.inCurrentFrame && chunk.fenceValue <= done) {
            chunk.used = 0;
        }
    }
}

void D3D12Backend::ActivateGuestRt(GuestRtEntry& e) {
    const bool changed = (m_activeRt != &e) || (m_guestRt.Get() != e.res.Get());
    if (m_activeRt && m_activeRt != &e) SyncActiveRtState();
    m_activeRt = &e;
    m_guestRt = e.res;
    m_guestRtW = e.w;
    m_guestRtH = e.h;
    m_guestRtFormat = e.fmt;
    m_guestRtv = e.rtv;
    // Re-activating the SAME live resource must keep the working state: e.state is only
    // synced on a switch, so copying it back reverted RENDER_TARGET to the stale COMMON
    // (D3D12 debug layer id=527 on every list after the first).
    if (changed) {
        m_guestRtState = e.state;
        m_guestRtDrawn = e.drawn;
    }
    m_guestRtValid = true;
    if (changed) {
        m_boundTargetsValid = false;
        InvalidateDrawCaches();
        BumpRtPassEpoch();
    }
}

bool D3D12Backend::EnsureGuestRenderTarget(uint32_t width, uint32_t height,
                                           DXGI_FORMAT format, uint64_t key) {
    if (!m_device || width == 0 || height == 0 ||
        format == DXGI_FORMAT_UNKNOWN) {
        GuestRenderTargetRefused("bad-geometry");
        return false;
    }
    if (key == 0)
        key = (1ull << 63) | (uint64_t(format) << 40) | (uint64_t(width) << 16) | height;
    GuestRtEntry* ent = nullptr;
    {
        auto it = m_rtCache.find(key);
        if (it != m_rtCache.end()) {
            ent = &it->second;
            if (ent->res && ent->w == width && ent->fmt == format && ent->h >= height) {
                ActivateGuestRt(*ent);
                return true;
            }
            // Growth only: a taller request recreates the target once (contents lost).
            if (ent->h > height) height = ent->h;
        } else if (m_rtCache.size() >= kMaxGuestRt) {
            GuestRenderTargetRefused("rt-cache-full");
            return false;
        }
    }

    D3D12_HEAP_PROPERTIES props = {};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = uint64_t(width) * RenderScale();
    desc.Height = height * RenderScale();
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // NOTE ON FLAGS: there is no D3D12_RESOURCE_FLAG_ALLOW_COPY_SRC. This file
    // says so at :860-867 (um/d3d12.h has ALLOW_RENDER_TARGET and no copy-source
    // flag): every D3D12 resource is copyable unless DENY_SHADER_RESOURCE is
    // set, so ALLOW_RENDER_TARGET alone makes this texture a legal copy source
    // AND a legal render target.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // F-XXX: desc.Format WAS NEVER SET, AND THAT IS WHY EVERY CREATION FAILED.
    //
    // MEASURED (build/run845.txt -- the direct run of the previous binary):
    //   :9671  Vectored exception: code=0xE06D7363 ... thread=360
    //   :9694  [ 5] CxxThrowException +0x96
    //   :9696  [ 7] D3D12GetInterface +0x3AE6C     (inside the D3D12 module)
    //   :9700  [ 9] mcla::native::D3D12Backend::EnsureGuestRenderTarget +0x139
    //   :9714  GUEST-RT-REFUSED #1 why=create-failed ... created=0 resolves=0
    // The frame's own return address is 0x140292099, and the disassembly of
    // that binary (llvm-objdump -d build/mcla.exe; EnsureGuestRenderTarget is
    // at RVA 0x291f60 per build/mcla.map:3631) puts the vtable call to
    // ID3D12Device::CreateCommittedResource at 0x140292096 -- vtable slot
    // 0xd8, the same slot a standalone probe of the same header uses for the
    // same call. So THE FAILING CALL IS THIS CALL, and the 0x139 in the log
    // is its return address, not an inference.
    //
    // THE DESC THIS CODE BUILT, read out of that same disassembly: the writes
    // into the D3D12_RESOURCE_DESC are at desc+0x00 (Dimension=3), +0x10
    // (Width), +0x18 (Height), +0x1c (DepthOrArraySize=1, MipLevels=1), +0x24
    // (SampleDesc.Count=1) and +0x30 (Flags=1). Windows Kits 10.0.26100.0
    // um/d3d12.h's D3D12_RESOURCE_DESC puts DXGI_FORMAT Format at +0x20 --
    // AND +0x20 IS NEVER WRITTEN. desc.Format therefore stayed 0 =
    // DXGI_FORMAT_UNKNOWN, which is not a format a texture may have (it is
    // the "no format" value, legal for a buffer only), so the runtime
    // rejected the desc and this function refused every time. The guest's
    // render target did not exist and the draws went into the back buffer
    // exactly as they did before it was written.
    //
    // PROVED, NOT INFERRED: a standalone probe of the identical call
    // (C:/Users/abdullah/AppData/Local/Temp/opencode/d3d12probe/probe.cpp --
    // same adapter, same D3D_FEATURE_LEVEL_11_0, same desc) returns
    //   Format = DXGI_FORMAT_UNKNOWN     -> hr = 0x80070057 (E_INVALIDARG)
    //   Format = DXGI_FORMAT_R8G8B8A8_UNORM -> hr = 0x00000000
    // 600 calls on two threads, and the probe never threw; the emulator's
    // process logged a C++ exception at the same call, so the probe
    // reproduces the FAILURE and not the throw. The mechanism that made the
    // runtime raise 0xE06D7363 instead of returning E_INVALIDARG in that
    // process is NOT established, and nothing in this fix depends on it: the
    // desc was invalid in exactly one field and that field is now set.
    desc.Format = format;

    Microsoft::WRL::ComPtr<ID3D12Resource> rt;
    HRESULT hr = m_device->CreateCommittedResource(
        &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&rt));
    if (FAILED(hr) || !rt) {
        // The HRESULT is NAMED, not just the reason: "create-failed" without
        // it says the create failed and not why, which is the same silence
        // this row exists to end.
        GuestRenderTargetRefused("create-failed", static_cast<uint32_t>(hr));
        return false;
    }

    if (!m_guestRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = kMaxGuestRt + kMaxGuestNullRtvs;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hhr = m_device->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&m_guestRtvHeap));
        if (FAILED(hhr) || !m_guestRtvHeap) {
            GuestRenderTargetRefused("rtv-heap-failed");
            return false;
        }
    }

    // A texture the GPU may still be reading must not be released under it,
    // so the one being replaced is kept until the fence says the GPU is done.
    if (!ent) {
        ent = &m_rtCache[key];
        ent->slot = static_cast<uint32_t>(m_rtCache.size() - 1);
    } else if (ent->res) {
        if (ent == m_activeRt) SyncActiveRtState();
        RetireTarget(ent->res);
        MCLA_LOG_INFO("TEMP-RT-CACHE grow key={:X} {}x{} -> {}x{}", key, ent->w, ent->h, width, height);
    }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_guestRtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<size_t>(ent->slot) * m_rtvDescriptorSize;
        m_device->CreateRenderTargetView(rt.Get(), nullptr, h);
        ent->rtv = h;
    }

    // Write guest RT SRV into dedicated slot in the primary SRV heap
    if (m_srvHeap) {
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        srvCpu.ptr += static_cast<size_t>(kGuestRtSrvSlot) * m_srvDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = rt->GetDesc().Format;  // the view must match the resource it is created on
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        ResetStaticSrvDedupe();  // slot 17 is inside the static 0..31 range
        m_device->CreateShaderResourceView(rt.Get(), &srvDesc, srvCpu);

        m_guestRtSrvGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        m_guestRtSrvGpu.ptr += static_cast<size_t>(kGuestRtSrvSlot) * m_srvDescriptorSize;
        m_guestRtSrvValid = true;
    }
    ent->res = rt;
    ent->w = width;
    ent->h = height;
    ent->fmt = format;
    ent->state = D3D12_RESOURCE_STATE_COMMON;
    ent->stateAtFrameStart = D3D12_RESOURCE_STATE_COMMON;
    ent->drawn = false;
    ent->lastClearList = 0;
    ActivateGuestRt(*ent);
    m_boundTargetsValid = false;
    InvalidateDrawCaches();
    BumpRtPassEpoch();
    const uint64_t n = ++m_guestRtCreated;
    {
        const mcla::gpu::CpGuestRenderTarget gt = mcla::gpu::CpGuestRenderTargetInfo();
        MCLA_LOG_INFO(
            "GUEST-RT-CREATED #{} {}x{} dxgiFormat={} -- the guest's render "
            "target (the console's EDRAM). It is sized and formatted from the "
            "guest's OWN Xenos registers; the guest's draws go into it and the "
            "resolve copies it into the scan-out surface. The registers it was "
            "built from: RB_SURFACE_INFO pitch={} (msaa_samples={}) "
            "RB_COLOR_INFO color_format={} (dxgi={}) "
            "PA_SC_SCREEN_SCISSOR_BR br_y={}. totals created={} resolves={} "
            "refusals={}",
            static_cast<uint32_t>(n), width, height,
            static_cast<uint32_t>(format),
            gt.surfacePitch, gt.msaaSamples, gt.colorFormat,
            static_cast<uint32_t>(format), gt.scissorBrY,
            m_guestRtCreated,
            m_guestRtResolves,
            m_guestRtRefusals);
    }
    return true;
}

bool D3D12Backend::EnsureGuestDepthStencil(uint32_t width, uint32_t height,
                                           DXGI_FORMAT format, uint64_t key) {
    if (!m_device || width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    if (key == 0)
        key = (1ull << 63) | (uint64_t(format) << 40) | (uint64_t(width) << 16) | height;
    GuestDsEntry* ent = nullptr;
    auto activate = [&](GuestDsEntry& e) {
        const bool changed = m_activeDs != &e || m_guestDepth.Get() != e.res.Get();
        m_activeDs = &e;
        m_guestDepth = e.res;
        m_guestDepthW = e.w;
        m_guestDepthH = e.h;
        m_guestDepthFormat = e.fmt;
        m_guestDsv = e.dsv;
        m_guestDepthValid = true;
        if (changed) { m_boundTargetsValid = false; InvalidateDrawCaches(); BumpRtPassEpoch(); }
    };
    {
        auto it = m_dsCache.find(key);
        if (it != m_dsCache.end()) {
            ent = &it->second;
            if (ent->res && ent->w == width && ent->fmt == format && ent->h >= height) {
                activate(*ent);
                return true;
            }
            if (ent->h > height) height = ent->h;
        } else if (m_dsCache.size() >= kMaxGuestRt / 2) {
            // MD-26 census (log-only): this refusal used to be silent and the caller ignores it.
            static std::atomic<uint64_t> s_dsFull{0};
            const uint64_t n = s_dsFull.fetch_add(1, std::memory_order_relaxed) + 1;
            if (ShouldLogRefusal(n))
                MCLA_LOG_WARN("GUEST-DS-REFUSED #{} why=ds-cache-full cap={} w={} h={} fmt={}",
                              n, kMaxGuestRt / 2, width, height, static_cast<unsigned>(format));
            return false;
        }
    }

    D3D12_HEAP_PROPERTIES props = {};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = uint64_t(width) * RenderScale();
    desc.Height = height * RenderScale();
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    // RT-CACHE: typeless so the depth copy a Resolve makes (R24G8_TYPELESS) is sampleable.
    desc.Format = format == DXGI_FORMAT_D24_UNORM_S8_UINT ? DXGI_FORMAT_R24G8_TYPELESS
                : format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? DXGI_FORMAT_R32G8X24_TYPELESS : format;  // MD-04: sampleable copy source
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = format;
    clearVal.DepthStencil.Depth = 1.0f;
    clearVal.DepthStencil.Stencil = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> ds;
    HRESULT hr = m_device->CreateCommittedResource(
        &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearVal, IID_PPV_ARGS(&ds));
    if (FAILED(hr) || !ds) {
        MCLA_LOG_WARN("D3D12Backend: EnsureGuestDepthStencil create failed hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    if (!m_guestDsvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = kMaxGuestRt / 2;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hhr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_guestDsvHeap));
        if (FAILED(hhr) || !m_guestDsvHeap) {
            return false;
        }
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = format;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    dsvDesc.Texture2D.MipSlice = 0;

    if (!ent) {
        ent = &m_dsCache[key];
        ent->slot = static_cast<uint32_t>(m_dsCache.size() - 1);
    } else if (ent->res) {
        RetireTarget(ent->res);
    }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_guestDsvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<size_t>(ent->slot) * m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        m_device->CreateDepthStencilView(ds.Get(), &dsvDesc, h);
        ent->dsv = h;
    }
    ent->res = ds;
    ent->w = width;
    ent->h = height;
    ent->fmt = format;
    ent->lastClearList = 0;
    ent->hasPendingClear = false;
    ent->pendingClearZ = 1.0f;
    ent->pendingClearStencil = 0;
    ent->pendingClearFlags = static_cast<D3D12_CLEAR_FLAGS>(0);
    ent->isMirrored = native::DepthCompareMirrored();
    activate(*ent);
    m_boundTargetsValid = false;
    InvalidateDrawCaches();
    BumpRtPassEpoch();
    SetTrackedState(ds.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    MCLA_LOG_INFO("D3D12Backend: created guest depth-stencil buffer {}x{} dxgi={}", width, height, static_cast<uint32_t>(format));
    if (format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
        static std::atomic<uint64_t> s_d32{0};
        const uint64_t dn = s_d32.fetch_add(1) + 1;
        if (dn <= 8 || (dn % 500) == 0) MCLA_LOG_WARN("D32-DEPTH-ON #{} float24 depth surface {}x{} key={:X} -> D32_FLOAT_S8X24_UINT (off: unset MCLA_D32_DEPTH)", dn, width, height, key);
    }
    return true;
}

// ---------------------------------------------------------------------------
// F-XXX: THE RESOLVE -- the console's EDRAM -> scan-out copy.
//
// The console's copy is a DRAW with RB_MODECONTROL.edram_mode = kCopy
// (xenos.h:911-915). On the host the equivalent is a CopyTextureRegion from
// the guest's render target into the scan-out surface (the swap-chain back
// buffer), and it is recorded on the SAME command list as the draws, so the
// copy sees the draws' output.
//
// THE GUEST'S COPY REGISTERS ARE READ HERE AND NAMED. They are the guest's
// statement of where the copy is going (RB_COPY_DEST_BASE, in the guest's
// own address space); this host's scan-out surface is the window's back
// buffer, so the two are different objects and the row says so rather than
// pretending they are the same.
// ---------------------------------------------------------------------------
bool D3D12Backend::EnsureBlitPipeline() {
    if (m_blitPipeline && m_blitRootSignature) return true;
    if (!m_device) return false;

    if (!m_blitRootSignature) {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;  // t0
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER rootParam = {};
        rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParam.DescriptorTable.NumDescriptorRanges = 1;
        rootParam.DescriptorTable.pDescriptorRanges = &srvRange;
        rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplerDesc.MipLODBias = 0.0f;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        samplerDesc.ShaderRegister = 0; // s0
        samplerDesc.RegisterSpace = 0;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &rootParam;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &samplerDesc;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> sigBlob, errBlob;
        HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &sigBlob, &errBlob);
        if (FAILED(hr)) {
            MCLA_LOG_ERROR("D3D12Backend: EnsureBlitPipeline SerializeRootSignature failed hr=0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }
        hr = m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_blitRootSignature));
        if (FAILED(hr)) {
            MCLA_LOG_ERROR("D3D12Backend: EnsureBlitPipeline CreateRootSignature failed hr=0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }
    }

    const char* vsHlsl =
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut main(uint id : SV_VertexID) {\n"
        "    VSOut o;\n"
        "    o.uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    return o;\n"
        "}\n";

    const char* psHlsl =
        "Texture2D g_tex : register(t0);\n"
        "SamplerState g_sampler : register(s0);\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "float4 main(VSOut i) : SV_Target {\n"
        "    return g_tex.Sample(g_sampler, i.uv);\n"
        "}\n";

    renderer::DxcRuntime dxc;
    std::string usedDir, error;
    if (!dxc.Load("", usedDir, error)) {
        MCLA_LOG_ERROR("D3D12Backend: EnsureBlitPipeline failed to load DXC runtime: {}", error);
        return false;
    }

    std::vector<uint8_t> vsBlob, psBlob;
    if (!dxc.Compile(vsHlsl, "main", "vs_6_0", vsBlob, error)) {
        MCLA_LOG_ERROR("D3D12Backend: EnsureBlitPipeline VS compilation failed: {}", error);
        return false;
    }
    if (!dxc.Compile(psHlsl, "main", "ps_6_0", psBlob, error)) {
        MCLA_LOG_ERROR("D3D12Backend: EnsureBlitPipeline PS compilation failed: {}", error);
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_blitRootSignature.Get();
    psoDesc.VS = { vsBlob.data(), vsBlob.size() };
    psoDesc.PS = { psBlob.data(), psBlob.size() };
    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.NumRenderTargets = 1;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = FALSE;
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_blitPipeline));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: EnsureBlitPipeline CreateGraphicsPipelineState failed hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    MCLA_LOG_INFO("D3D12Backend: blit pipeline created successfully");
    return true;
}

bool D3D12Backend::GammaLutActive() {
    static const bool s_off = [] { const char* e = std::getenv("MCLA_NO_GAMMA_LUT"); return e && e[0] == '1'; }();
    if (s_off) return false;
    uint32_t lut[256];
    return mcla::gpu::CpGammaLut256(lut) != 0;
}

bool D3D12Backend::EnsureGammaLutPipeline() {
    if (m_gammaLutPso) return true;
    if (!m_device || !EnsureBlitPipeline()) return false;
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {};
    rd.NumParameters = 2; rd.pParameters = rp;
    Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &sig, &err)) ||
        FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_gammaLutRootSig)))) {
        MCLA_LOG_ERROR("GAMMA-LUT root signature failed");
        return false;
    }
    // Xenia apply_gamma_table.xesli:60-71: UNORM index round(c*255), output = 10-bit entry / 1023 per channel; Load = no filtering.
    const char* vsHlsl =
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut main(uint id : SV_VertexID) { VSOut o; o.uv = float2((id << 1) & 2, id & 2);\n"
        "  o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); return o; }\n";
    const char* psHlsl =
        "Texture2D g_tex : register(t0);\n"
        "cbuffer Lut : register(b0) { uint4 g_lut[64]; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "uint E(uint i) { return g_lut[i >> 2][i & 3]; }\n"
        "float4 main(VSOut i) : SV_Target {\n"
        "  uint w, h; g_tex.GetDimensions(w, h);\n"
        "  float4 c = g_tex.Load(int3(min(uint2(i.uv * float2(w, h)), uint2(w - 1, h - 1)), 0));\n"
        "  uint3 x = uint3(saturate(c.rgb) * 255.0 + 0.5);\n"
        "  return float4(float((E(x.r) >> 20) & 1023u), float((E(x.g) >> 10) & 1023u), float(E(x.b) & 1023u), 1023.0) / 1023.0;\n"
        "}\n";
    renderer::DxcRuntime dxc;
    std::string usedDir, error;
    std::vector<uint8_t> vsBlob, psBlob;
    if (!dxc.Load("", usedDir, error) || !dxc.Compile(vsHlsl, "main", "vs_6_0", vsBlob, error) ||
        !dxc.Compile(psHlsl, "main", "ps_6_0", psBlob, error)) {
        MCLA_LOG_ERROR("GAMMA-LUT shader compile failed: {}", error);
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_gammaLutRootSig.Get();
    pd.VS = { vsBlob.data(), vsBlob.size() };
    pd.PS = { psBlob.data(), psBlob.size() };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.NumRenderTargets = 1;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_gammaLutPso)))) {
        MCLA_LOG_ERROR("GAMMA-LUT PSO failed");
        return false;
    }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 1024ull * kBufferCount; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&m_gammaLutCb))) ||
        FAILED(m_gammaLutCb->Map(0, nullptr, reinterpret_cast<void**>(&m_gammaLutCbMap)))) {
        m_gammaLutPso.Reset();
        MCLA_LOG_ERROR("GAMMA-LUT constant buffer failed");
        return false;
    }
    return true;
}

bool D3D12Backend::BlitGuestRenderTargetToScanout() {
    if (!m_guestRtValid || !m_guestRt || !m_commandList || !m_guestRtSrvValid) return false;
    if (m_width == 0 || m_height == 0 || m_guestRtW == 0 || m_guestRtH == 0) return false;
    ID3D12Resource* back = m_renderTargets[m_frameIndex].Get();
    if (!back) return false;

    if (!EnsureBlitPipeline()) {
        GuestRenderTargetRefused("blit-pipeline-failed");
        return false;
    }
    {   // RT-CACHE: rewrite the shared blit SRV for the target being presented.
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        srvCpu.ptr += static_cast<size_t>(kGuestRtSrvSlot) * m_srvDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = m_guestRt->GetDesc().Format;  // mirrored m_guestRtFormat can belong to another cache entry (gt2: id=28 -> device removed)
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        ResetStaticSrvDedupe();  // slot 17 is inside the static 0..31 range
        m_device->CreateShaderResourceView(m_guestRt.Get(), &sd, srvCpu);
    }

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = m_guestRt.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = back;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    (++g_dcBarriers, m_commandList)->ResourceBarrier(2, barriers);

    D3D12_CPU_DESCRIPTOR_HANDLE backRtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    backRtv.ptr += static_cast<size_t>(m_frameIndex) * m_rtvDescriptorSize;
    // F-XXX: guarded bind (auditor-measured in w13: the draw-path guard had
    // latched after 32 faults, then the render thread parked at the NEXT
    // present's blit bind -- this site was the only unguarded OMSet left in
    // the present path). On fault, skip the blit instead of letting the VEH
    // park the render thread.
    unsigned long blitOmsetCode = 0;
    if (!TryOMSetRenderTargets(m_commandList.Get(), 1, &backRtv, FALSE, nullptr,
                               &blitOmsetCode)) {
        static std::atomic<uint32_t> s_blitOmsetFaults{0};
        const uint32_t fn =
            s_blitOmsetFaults.fetch_add(1, std::memory_order_relaxed) + 1;
        const HRESULT removed =
            m_device ? m_device->GetDeviceRemovedReason() : E_FAIL;
        if (fn <= 8 || (fn % 200) == 0) {
            MCLA_LOG_ERROR(
                "BLIT-OMSET-FAULT #{} code=0x{:08X} frameIndex={} presented={} "
                "lastPresentHr={:08X} deviceRemoved=0x{:08X}",
                fn, static_cast<unsigned>(blitOmsetCode), m_frameIndex,
                m_presentedFrames, static_cast<uint32_t>(m_lastPresentHr),
                static_cast<uint32_t>(removed));
        }
        return false;
    }
    m_boundTargetsValid = false;  // same slot, new resource; see cache decl
    InvalidateDrawCaches();

    D3D12_VIEWPORT vp = m_graphicsSettings.aspectCorrection
        ? GetAspectCorrectViewport(static_cast<float>(m_guestRtW) / static_cast<float>(m_guestRtH))
        : D3D12_VIEWPORT{ 0.0f, 0.0f, static_cast<float>(m_swapW), static_cast<float>(m_swapH), 0.0f, 1.0f };
    m_commandList->RSSetViewports(1, &vp);

    D3D12_RECT rc = { 0, 0, static_cast<LONG>(m_swapW), static_cast<LONG>(m_swapH) };
    m_commandList->RSSetScissorRects(1, &rc);

    // Keep primary heaps bound
    if (m_srvHeap && m_samplerHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get(), m_samplerHeap.Get() };
        m_commandList->SetDescriptorHeaps(2, heaps);
    } else if (m_srvHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
    }

    if (GammaLutActive() && EnsureGammaLutPipeline() && m_frameIndex < kBufferCount) {
        uint32_t lut[256];
        const uint32_t gen = mcla::gpu::CpGammaLut256(lut);
        std::memcpy(m_gammaLutCbMap + 1024u * m_frameIndex, lut, sizeof(lut));  // slice per frame index: no in-flight overwrite
        static uint32_t s_loggedGen = 0;
        if (gen != s_loggedGen && s_loggedGen < 4u) {
            s_loggedGen = gen;
            std::string row;
            for (uint32_t k : {0u, 1u, 2u, 4u, 8u, 16u, 32u, 48u, 64u, 96u, 128u, 160u, 192u, 224u, 255u})
                row += fmt::format(" [{}]={}/{}/{}", k, (lut[k] >> 20) & 1023u, (lut[k] >> 10) & 1023u, lut[k] & 1023u);
            MCLA_LOG_WARN("GAMMA-LUT applied gen={} r/g/b of 1023:{}", gen, row);
        }
        m_commandList->SetGraphicsRootSignature(m_gammaLutRootSig.Get());
        m_commandList->SetGraphicsRootDescriptorTable(0, m_guestRtSrvGpu);
        m_commandList->SetGraphicsRootConstantBufferView(1, m_gammaLutCb->GetGPUVirtualAddress() + 1024u * m_frameIndex);
        m_commandList->SetPipelineState(m_gammaLutPso.Get());
    } else {
        m_commandList->SetGraphicsRootSignature(m_blitRootSignature.Get());
        m_commandList->SetGraphicsRootDescriptorTable(0, m_guestRtSrvGpu);
        m_commandList->SetPipelineState(m_blitPipeline.Get());
    }
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_commandList->DrawInstanced(3, 1, 0, 0);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    (++g_dcBarriers, m_commandList)->ResourceBarrier(2, barriers);

    // Immediately restore primary graphics root signature and descriptor heaps
    if (m_rootSignature) {
        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    }
    if (m_srvHeap && m_samplerHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get(), m_samplerHeap.Get() };
        m_commandList->SetDescriptorHeaps(2, heaps);
    } else if (m_srvHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
    }

    static uint32_t s_blitCount = 0;
    if (++s_blitCount <= 8 || (s_blitCount % 200) == 0) {
        MCLA_LOG_ERROR("BLIT-GUEST-RT-SCANOUT #{} {}x{} -> {}x{}",
                       s_blitCount, m_guestRtW, m_guestRtH, m_width, m_height);
    }

    return true;
}

// Xenos resolve exp bias: dst *= 2^bias. Fixed-function: ZERO * src + dst * BLEND_FACTOR, so no descriptors/root params are needed.
// Leaves dst in COPY_DEST; the caller's cached draw state is invalidated (same as the scan-out blit).
bool D3D12Backend::ApplyResolveExpBias(ID3D12Resource* res, DXGI_FORMAT fmt, int32_t bias, int32_t x, int32_t y, int32_t w, int32_t h) {
    static Microsoft::WRL::ComPtr<ID3D12RootSignature> s_rs;
    static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> s_heap;
    static std::map<DXGI_FORMAT, Microsoft::WRL::ComPtr<ID3D12PipelineState>> s_pso;
    static std::vector<uint8_t> s_vs, s_ps;
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1) + 1;
    if (!res || w <= 0 || h <= 0) return false;
    if (!s_rs) {
        std::string dir, err;
        renderer::DxcRuntime dxc;
        const char* vs = "float4 main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }\n";
        const char* ps = "float4 main() : SV_Target { return float4(0, 0, 0, 0); }\n";
        if (!dxc.Load("", dir, err) || !dxc.Compile(vs, "main", "vs_6_0", s_vs, err) || !dxc.Compile(ps, "main", "ps_6_0", s_ps, err)) {
            MCLA_LOG_ERROR("EXPBIAS-PASS shader compile failed: {}", err);
            return false;
        }
        D3D12_ROOT_SIGNATURE_DESC rd = {};
        Microsoft::WRL::ComPtr<ID3DBlob> blob, eb;
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &eb)) ||
            FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s_rs)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_heap)))) { s_rs.Reset(); return false; }
    }
    auto& pso = s_pso[fmt];
    if (!pso) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = s_rs.Get();
        pd.VS = { s_vs.data(), s_vs.size() };
        pd.PS = { s_ps.data(), s_ps.size() };
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.RTVFormats[0] = fmt; pd.NumRenderTargets = 1;
        pd.SampleDesc.Count = 1; pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        auto& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ZERO; b.DestBlend = D3D12_BLEND_BLEND_FACTOR; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO; b.DestBlendAlpha = D3D12_BLEND_BLEND_FACTOR; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
            MCLA_LOG_ERROR("EXPBIAS-PASS PSO create failed fmt={}", static_cast<uint32_t>(fmt));
            return false;
        }
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = s_heap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(res, nullptr, rtv);
    D3D12_RESOURCE_BARRIER bar = {};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = res;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &bar);
    const float f = std::ldexp(1.0f, bias);
    const float bf[4] = { f, f, f, f };
    const D3D12_VIEWPORT vp = { float(x), float(y), float(w), float(h), 0.0f, 1.0f };
    const D3D12_RECT sc = { x, y, x + w, y + h };
    m_commandList->SetGraphicsRootSignature(s_rs.Get());
    m_commandList->SetPipelineState(pso.Get());
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_commandList->OMSetBlendFactor(bf);
    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &sc);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);
    std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
    (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &bar);
    m_boundTargetsValid = false;
    InvalidateDrawCaches();
    if (n <= 8 || (n % 500) == 0) MCLA_LOG_WARN("EXPBIAS-PASS #{} bias={} factor={} rect=({},{} {}x{}) fmt={}", n, bias, f, x, y, w, h, static_cast<uint32_t>(fmt));
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::EnsureGuestNullRtv(DXGI_FORMAT fmt) {
    if (!m_device) return {};
    if (!m_guestRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = kMaxGuestRt + kMaxGuestNullRtvs;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_guestRtvHeap))) || !m_guestRtvHeap) {
            return {};
        }
    }
    const DXGI_FORMAT targetFmt = (fmt != DXGI_FORMAT_UNKNOWN) ? fmt : DXGI_FORMAT_R8G8B8A8_UNORM;
    for (uint32_t i = 0; i < kMaxGuestNullRtvs; ++i) {
        if (m_guestNullRtvs[i].ptr != 0 && m_guestNullRtvFmts[i] == targetFmt) {
            return m_guestNullRtvs[i];
        }
        if (m_guestNullRtvs[i].ptr == 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_guestRtvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<size_t>(kMaxGuestRt + i) * m_rtvDescriptorSize;
            D3D12_RENDER_TARGET_VIEW_DESC nullDesc = {};
            nullDesc.Format = targetFmt;
            nullDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            m_device->CreateRenderTargetView(nullptr, &nullDesc, h);
            m_guestNullRtvFmts[i] = targetFmt;
            m_guestNullRtvs[i] = h;
            return h;
        }
    }
    return m_guestNullRtvs[0];
}

// Mirrored-depth resolve: the guest stores reversed depth (z' = 1 - z) but we hold host z, so a depth texture handed to the
// guest must carry 1 - z. Full-screen pass: samples the DS (R24_UNORM_X8 view) and writes 1 - z into an R32_FLOAT target.
// Caller puts src in PIXEL_SHADER_RESOURCE and dst in RENDER_TARGET.
// stencilToColour: read the stencil plane instead and write stencil/255 to all four channels of an R8G8B8A8_UNORM target
// (the guest "stencil" texture = its D24S8 resolved as k_8_8_8_8, whose X byte is the stencil; see ExecuteGuestResolve).
bool D3D12Backend::MirrorDepthResolvePass(ID3D12Resource* src, ID3D12Resource* dst, uint32_t w, uint32_t h,
                                          uint32_t dstX, uint32_t dstY, bool clearDst, bool stencilToColour,
                                          bool invertDepth) {
    static Microsoft::WRL::ComPtr<ID3D12RootSignature> s_rs;
    static Microsoft::WRL::ComPtr<ID3D12PipelineState> s_psoModes[3];
    static std::vector<uint8_t> s_vs, s_ps[3];
    if (!src || !dst || !w || !h) return false;
    const uint32_t modeIdx = stencilToColour ? 1u : (invertDepth ? 0u : 2u);
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& s_pso = s_psoModes[modeIdx];
    if (!s_pso) {
        std::string dir, err;
        renderer::DxcRuntime dxc;
        const char* vs = "float4 main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }\n";
        const char* ps = (modeIdx == 1)
            ? "cbuffer DstOff : register(b0) { uint2 dstOff; };\nTexture2D<uint2> t : register(t0);\nfloat4 main(float4 p : SV_Position) : SV_Target { return (t.Load(int3(int2(p.xy) - int2(dstOff), 0)).y / 255.0).xxxx; }\n"
            : (modeIdx == 0
                ? "cbuffer DstOff : register(b0) { uint2 dstOff; };\nTexture2D<float> t : register(t0);\nfloat main(float4 p : SV_Position) : SV_Target { return 1.0 - t.Load(int3(int2(p.xy) - int2(dstOff), 0)); }\n"
                : "cbuffer DstOff : register(b0) { uint2 dstOff; };\nTexture2D<float> t : register(t0);\nfloat main(float4 p : SV_Position) : SV_Target { return t.Load(int3(int2(p.xy) - int2(dstOff), 0)); }\n");
        if (!dxc.Load("", dir, err) || (s_vs.empty() && !dxc.Compile(vs, "main", "vs_6_0", s_vs, err)) || !dxc.Compile(ps, "main", "ps_6_0", s_ps[modeIdx], err)) {
            MCLA_LOG_ERROR("DEPTH-MIRROR-PASS shader compile failed: {}", err);
            return false;
        }
        D3D12_DESCRIPTOR_RANGE rg = {};
        rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 1; rg.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER rp[2] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &rg;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[1].Constants.ShaderRegister = 0; rp[1].Constants.Num32BitValues = 2;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd = {};
        rd.NumParameters = 2; rd.pParameters = rp;
        Microsoft::WRL::ComPtr<ID3DBlob> blob, eb;
        if (!s_rs && (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &eb)) ||
            FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s_rs))))) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = s_rs.Get();
        pd.VS = { s_vs.data(), s_vs.size() };
        pd.PS = { s_ps[modeIdx].data(), s_ps[modeIdx].size() };
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.RTVFormats[0] = stencilToColour ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_FLOAT; pd.NumRenderTargets = 1;
        pd.SampleDesc.Count = 1; pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&s_pso)))) {
            MCLA_LOG_ERROR("DEPTH-MIRROR-PASS PSO create failed (mode={})", modeIdx);
            return false;
        }
    }
    // Per-call heaps: the recorded draw reads them at execution time, so they are retired, not reused (as GenerateMipmaps does).
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sh, rh;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sh)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rh)))) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    const bool srcD32 = src->GetDesc().Format == DXGI_FORMAT_R32G8X24_TYPELESS;
    sd.Format = stencilToColour ? (srcD32 ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT)
                                : (srcD32 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
    sd.Texture2D.PlaneSlice = stencilToColour ? 1u : 0u;
    m_device->CreateShaderResourceView(src, &sd, sh->GetCPUDescriptorHandleForHeapStart());
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rh->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(dst, nullptr, rtv);
    ID3D12DescriptorHeap* heaps[] = { sh.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetGraphicsRootSignature(s_rs.Get());
    m_commandList->SetPipelineState(s_pso.Get());
    m_commandList->SetGraphicsRootDescriptorTable(0, sh->GetGPUDescriptorHandleForHeapStart());
    const uint32_t dstOff[2] = { dstX, dstY };
    m_commandList->SetGraphicsRoot32BitConstants(1, 2, dstOff, 0);
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    if (clearDst) {
        const float zbg[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        m_commandList->ClearRenderTargetView(rtv, zbg, 0, nullptr);
    }
    const D3D12_VIEWPORT vp = { float(dstX), float(dstY), float(w), float(h), 0.0f, 1.0f };
    const D3D12_RECT sc = { LONG(dstX), LONG(dstY), LONG(dstX + w), LONG(dstY + h) };
    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &sc);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);
    if (m_srvHeap && m_samplerHeap) {
        ID3D12DescriptorHeap* hs[] = { m_srvHeap.Get(), m_samplerHeap.Get() };
        m_commandList->SetDescriptorHeaps(2, hs);
    } else if (m_srvHeap) {
        ID3D12DescriptorHeap* hs[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, hs);
    }
    RetireTarget(std::move(sh));
    RetireTarget(std::move(rh));
    m_boundTargetsValid = false;
    InvalidateBoundState();
    InvalidateDrawCaches();
    return true;
}

bool D3D12Backend::ResolveGuestRenderTarget() {
    if (!m_guestRtValid || !m_guestRtDrawn || !m_commandList) return false;
    if (m_width == 0 || m_height == 0 || m_guestRtW == 0 || m_guestRtH == 0) return false;
    ID3D12Resource* back = m_renderTargets[m_frameIndex].Get();
    if (!back) {
        GuestRenderTargetRefused("no-scanout-surface");
        return false;
    }

    const DXGI_FORMAT scanoutFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (RenderScale() == 1 && m_guestRtW == m_width && m_guestRtH == m_height && m_guestRtFormat == scanoutFormat && !GammaLutActive()) {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = m_guestRt.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = back;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        (++g_dcBarriers, m_commandList)->ResourceBarrier(2, barriers);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = back;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = m_guestRt.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        (++g_dcBarriers, m_commandList)->ResourceBarrier(2, barriers);
    } else {
        if (!BlitGuestRenderTargetToScanout()) {
            return false;
        }
    }

    // The back buffer has now been written -- by the resolve, which is the
    // only thing that writes it while the draws land in the guest's render
    // target. The ledger describes the BACK BUFFER, so the write is recorded
    // HERE and not at the draw.
    NoteBufferWrite("guest-rt-resolve");
    m_guestRtDrawn = false;
    BumpRtPassEpoch();

    const uint64_t n = ++m_guestRtResolves;
    const mcla::gpu::CpGuestRenderTarget gt = mcla::gpu::CpGuestRenderTargetInfo();
    if (n <= 8 || (n % 200) == 0) {
        MCLA_LOG_ERROR(
            "GUEST-RT-RESOLVE #{} src=guest-rt {}x{} dxgiFormat={} -> "
            "dst=scanout {}x{} changedPx={} modalRgba=({},{},{},{}) "
            "modalCount={} -- the guest's render target reached "
            "the scan-out surface. changedPx is the number of NON-MODAL pixels "
            "the last post-draw readback measured IN THE GUEST RENDER TARGET "
            "(the same count the D3D12-DRAW-PIXEL row prints): it is the "
            "evidence that what was copied was not blank, and it lags the "
            "resolve by one frame because the readback's buffer can only be "
            "mapped after the present that executed the copy. The guest's own "
            "copy registers at this moment: "
            "RB_COPY_CONTROL={:08X} RB_COPY_DEST_BASE={:08X} "
            "RB_COPY_DEST_PITCH={:08X} RB_COPY_DEST_INFO={:08X} "
            "RB_MODECONTROL={:08X} (edramMode={}) RB_SURFACE_INFO pitch={}. "
            "The guest's copy DESTINATION is an address in the guest's own "
            "memory; this host's scan-out surface is the window's back buffer, "
            "so the copy lands in a different object than the guest asked for "
            "-- that difference is named here rather than hidden. "
            "RB_COPY_DEST_PITCH decodes (registers.h:887-897, 14 bits each) as "
            "pitch={} height={}, i.e. the console's front buffer is {}x{}; it "
            "is a TILED 2D surface (xenia xenos.h:911-915: the EDRAM copy "
            "produces a tiled 2D texture), so this host does not write it: a "
            "linear write there would not be the console's bytes.",
            static_cast<uint32_t>(n), m_guestRtW, m_guestRtH,
            static_cast<uint32_t>(m_guestRtFormat), m_width, m_height,
            m_guestRtChangedPx,
            static_cast<unsigned>(m_guestRtModalRgba & 0xFFu),
            static_cast<unsigned>((m_guestRtModalRgba >> 8) & 0xFFu),
            static_cast<unsigned>((m_guestRtModalRgba >> 16) & 0xFFu),
            static_cast<unsigned>((m_guestRtModalRgba >> 24) & 0xFFu),
            m_guestRtModalCount,
            gt.copyControl, gt.copyDestBase, gt.copyDestPitch, gt.copyDestInfo,
            gt.edramMode, gt.edramMode, gt.surfacePitch,
            static_cast<uint32_t>(gt.copyDestPitch & 0x3FFFu),
            static_cast<uint32_t>((gt.copyDestPitch >> 16) & 0x3FFFu),
            static_cast<uint32_t>(gt.copyDestPitch & 0x3FFFu),
            static_cast<uint32_t>((gt.copyDestPitch >> 16) & 0x3FFFu));
    }
    return true;
}

bool D3D12Backend::CreateSyncObjects() {
    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateFence failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    m_fenceCompletedLB = 0;

    // The fence is created at value 0. Pre-arm the per-frame wait values to 0
    // so the FIRST BeginFrame does not wait on a value that was never signaled
    // (the fence is only advanced by the Signal() after each submit). A slot
    // only becomes non-zero once its frame has been submitted and fenced.
    m_fenceValues[m_frameIndex] = 0;
    m_currentFenceValue = 0;

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        MCLA_LOG_ERROR("D3D12Backend: CreateEvent failed");
        return false;
    }

    return true;
}

bool D3D12Backend::CreateUploadHeap() {
    // F-900: 256 MiB total upload arena (was 64 MiB). Measured: a guest burst
    // carries ~4,000+ draws at ~4,352 B each (4096 B root CBV + 256 B VB) plus
    // vertex/index buffers. In bursts exceeding 3,800 draws, 21 MB per region
    // was exhausted, dropping draws and causing all-black frames.
    // 256 MiB provides 85.3 MiB per frame region, preventing all upload arena drops.
    // ed50/ed52 measured: a world frame carries ~20 KB/draw x 3-5k draws ~ 100 MB > the 89 MB region -> "upload arena region exhausted ... frame draw dropped" (4765 rows in ed50, 0 in ed48/ed55).
    constexpr size_t kUploadSize = 864ull * 1024 * 1024;  // 3 x 288 MiB; abc131 with mip chains: uploadPeakAllMB=191 of 192 (144 drops)

    D3D12_HEAP_PROPERTIES props = {};
    props.Type = D3D12_HEAP_TYPE_UPLOAD;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kUploadSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_device->CreateCommittedResource(
        &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_uploadHeap));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateCommittedResource(upload) failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    m_uploadCapacity = kUploadSize;
    // F-900: align region stride to 256 so CBV GPU VAs stay 256-aligned on all
    // frames. Unaligned 1398101 gave frames 1/2 offsets 85/170 (measured).
    // Align DOWN to 1398016 (total 4194048, 256 B slack in the 4 MiB heap).
    m_uploadRegionSize = (kUploadSize / kBufferCount) & ~static_cast<size_t>(255);
    if (m_mtRecorderCount > 1) {
        m_mtUploadSubRegionSize = (m_uploadRegionSize / (m_mtRecorderCount + 1)) & ~static_cast<size_t>(255);
        m_mainUploadRegionSize = (m_uploadRegionSize - m_mtRecorderCount * m_mtUploadSubRegionSize) & ~static_cast<size_t>(255);
    } else {
        m_mtUploadSubRegionSize = m_uploadRegionSize;
        m_mainUploadRegionSize = m_uploadRegionSize;
    }
    m_uploadOffset = 0; ++m_uploadEpoch;
    m_uploadMap = nullptr;
    hr = m_uploadHeap->Map(0, nullptr, reinterpret_cast<void**>(&m_uploadMap));
    if (FAILED(hr) || !m_uploadMap) {
        MCLA_LOG_ERROR("D3D12Backend: upload heap Map failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    MCLA_LOG_INFO("D3D12Backend: created {} MiB upload arena", kUploadSize / (1024 * 1024));
    return true;
}

bool D3D12Backend::CreateStaticIndexBuffer() {
    // 6 R16_UINT indices for the deterministic triangle, written once into a
    // dedicated upload resource and cached by ResourceCache. Later frames bind
    // the cached GPU address instead of re-uploading unchanged data (Phase 3
    // gate: "replaying the same frame twice does not reupload unchanged data").
    const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    constexpr uint32_t kIndexBytes = sizeof(indices);

    D3D12_HEAP_PROPERTIES props = {};
    props.Type = D3D12_HEAP_TYPE_UPLOAD;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kIndexBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_device->CreateCommittedResource(
        &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_staticIndexBuffer));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateCommittedResource(static ib) failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    void* mapped = nullptr;
    hr = m_staticIndexBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) {
        MCLA_LOG_ERROR("D3D12Backend: static index buffer Map failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    std::memcpy(mapped, indices, kIndexBytes);
    m_staticIndexBuffer->Unmap(0, nullptr);

    const uint64_t ibGpu = m_staticIndexBuffer->GetGPUVirtualAddress();
    const ResourceKey ibKey = { ResourceKind::Buffer, 0 /*synthetic test addr*/,
                                kIndexBytes, DXGI_FORMAT_R16_UINT, 1 };
    m_resourceCache.Insert(ibKey, ResourceEntry{ ibGpu, 1, false });
    m_stats.cacheMisses++;

    MCLA_LOG_INFO("D3D12Backend: static index buffer cached ({} bytes, gpu=0x{:X})", kIndexBytes, ibGpu);
    return true;
}

uint8_t* D3D12Backend::MapUpload(size_t size, size_t alignment, D3D12_GPU_VIRTUAL_ADDRESS& gpu) {
    if (!m_uploadMap || size == 0) return nullptr;
    if (alignment == 0) alignment = 256;

    // Offset is relative to this frame's region (region = back-buffer index),
    // so the fence wait in BeginFrame() guarantees the GPU is done reading this
    // region before we write it again. Region reuse requires offset < region.
    m_uploadOffset = (m_uploadOffset + alignment - 1) & ~(alignment - 1);
    const size_t mainCap = m_mainUploadRegionSize ? m_mainUploadRegionSize : m_uploadRegionSize;
    if (m_uploadOffset + size > mainCap) {
        MCLA_LOG_WARN("D3D12Backend: upload arena region exhausted (need {} bytes, {} free of {}); frame draw dropped",
                    size, mainCap - m_uploadOffset, mainCap);
        return nullptr;
    }

    const size_t regionBase = static_cast<size_t>(m_frameIndex) * m_uploadRegionSize;
    gpu = m_uploadHeap->GetGPUVirtualAddress() + regionBase + m_uploadOffset;
    uint8_t* dst = m_uploadMap + regionBase + m_uploadOffset;
    m_uploadOffset += size;
    m_stats.uploadsBytes += size;
    if (m_uploadOffset > s_uploadPeakBytes) s_uploadPeakBytes = m_uploadOffset;
    m_stats.bytesThisFrame += size;
    return dst;
}

uint8_t* D3D12Backend::MapUpload(size_t size, size_t alignment, D3D12_GPU_VIRTUAL_ADDRESS& gpu, DrawRecorder& rec) {
    // Phase 3 MT: allocate from this recorder's sub-region of the frame's upload region.
    if (!m_uploadMap || size == 0) return nullptr;
    if (alignment == 0) alignment = 256;
    const size_t subSize = rec.uploadSubRegionSize ? rec.uploadSubRegionSize : m_uploadRegionSize;
    rec.uploadOffset = (rec.uploadOffset + alignment - 1) & ~(alignment - 1);
    if (rec.uploadOffset + size > subSize) {
        MCLA_LOG_WARN("D3D12Backend: MT upload sub-region exhausted (rec={} need {} free {} of {}); draw dropped",
                      rec.index, size, subSize - rec.uploadOffset, subSize);
        return nullptr;
    }
    const size_t base = rec.uploadRegionBase + rec.uploadOffset;
    gpu = m_uploadHeap->GetGPUVirtualAddress() + base;
    uint8_t* dst = m_uploadMap + base;
    rec.uploadOffset += size;
    rec.uploadsBytes += size;
    rec.bytesThisFrame += size;
    return dst;
}

bool D3D12Backend::CreateSrvHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    // F-639: 17 slots, not 4. The root signature still binds only the first
    // kShaderTextureSlotCount (t0..t3) -- the heap being deeper does not make t4
    // reachable, because the translated HLSL has no t4 and the pipeline worker
    // refuses reflected registers >= kShaderTextureSlotCount. The extra slots are
    // the writable homes for DrawPacket::textureDescriptors[4..15], so binding
    // index i lands at kTexture2DSlot + i instead of aliasing a live SRV.
    m_srvHeapDescriptorCount = SrvHeapDescriptorCount();
    heapDesc.NumDescriptors = m_srvHeapDescriptorCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateDescriptorHeap (CBV/SRV/UAV) failed with hr=0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // D3D12 requires every descriptor in a bound table to be initialized.
    // Prefill ALL slots with null SRVs: an unbound texture slot must read as a
    // null SRV (a defined sample) and never as whatever the previous pass left
    // behind, and CreateDecodedTexture/BindGuestTextures overwrite individual
    // slots from here.
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;
    ResetStaticSrvDedupe();  // new heap: every static slot is null again
    for (uint32_t i = 0; i < m_srvHeapDescriptorCount; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
        h.ptr += static_cast<size_t>(i) * m_srvDescriptorSize;
        m_device->CreateShaderResourceView(nullptr, &nullSrv, h);
    }
    return true;
}

bool D3D12Backend::CreateStagingSrvHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = kSrvStagingHeapSize;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvStagingHeap));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateStagingSrvHeap failed hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    m_srvStagingNext = 1;
    m_srvStagingFree.clear();
    m_srvTableEpoch = 1;
    m_mainSrvTableCache.Clear();
    for (auto& c : m_mtSrvTableCaches) c.Clear();

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(nullptr, &nullSrv, m_srvStagingHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::StagingSrvCpu(uint32_t slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvStagingHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<size_t>(slot) * m_srvDescriptorSize;
    return h;
}

uint32_t D3D12Backend::AcquireStagingSrvSlot() {
    if (!m_srvStagingHeap) return UINT32_MAX;
    if (!m_srvStagingFree.empty()) {
        const uint32_t s = m_srvStagingFree.back();
        m_srvStagingFree.pop_back();
        return s;
    }
    if (m_srvStagingNext < kSrvStagingHeapSize) {
        return m_srvStagingNext++;
    }
    return UINT32_MAX;
}

void D3D12Backend::ReleaseStagingSrvSlot(uint32_t slot) {
    if (slot == 0 || slot >= kSrvStagingHeapSize) return;
    m_srvStagingFree.push_back(slot);
}

static uint64_t HashSrvDesc64(const D3D12_SHADER_RESOURCE_VIEW_DESC& d) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&d);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(d); ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint32_t D3D12Backend::GetOrCreateResidentStagingSrv(ResidentTexture* rt, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc) {
    if (!m_srvStagingHeap || !rt || !rt->texture) return UINT32_MAX;
    const uint64_t dh = HashSrvDesc64(srvDesc);
    for (const auto& entry : rt->stagingSrvs) {
        if (entry.descHash == dh && std::memcmp(&entry.desc, &srvDesc, sizeof(srvDesc)) == 0) {
            return entry.slot;
        }
    }
    const uint32_t slot = AcquireStagingSrvSlot();
    if (slot == UINT32_MAX) return UINT32_MAX;
    (++g_dcSrv, m_device)->CreateShaderResourceView(rt->texture.Get(), &srvDesc, StagingSrvCpu(slot));
    ResidentTexture::StagingSrv s;
    s.descHash = dh;
    s.desc = srvDesc;
    s.slot = slot;
    rt->stagingSrvs.push_back(s);
    return slot;
}

uint32_t D3D12Backend::GetOrCreateResolvedStagingSrv(ResolvedTex& rt, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc) {
    if (!m_srvStagingHeap || !rt.res) return UINT32_MAX;
    // view key = format | component mapping, so two fetch swizzles of one resolve do not share a stale view
    const uint32_t fmtU32 = static_cast<uint32_t>(srvDesc.Format) | (srvDesc.Shader4ComponentMapping << 8);
    if (rt.srvStagingSlot != UINT32_MAX && rt.srvStagingRes == rt.res.Get() && rt.srvStagingFmt == fmtU32) {
        return rt.srvStagingSlot;
    }
    if (rt.srvStagingSlot == UINT32_MAX) {
        rt.srvStagingSlot = AcquireStagingSrvSlot();
        if (rt.srvStagingSlot == UINT32_MAX) return UINT32_MAX;
    }
    (++g_dcSrv, m_device)->CreateShaderResourceView(rt.res.Get(), &srvDesc, StagingSrvCpu(rt.srvStagingSlot));
    rt.srvStagingRes = rt.res.Get();
    rt.srvStagingFmt = fmtU32;
    ++m_srvTableEpoch;
    return rt.srvStagingSlot;
}

// MD-09: copy one resolved mip level into the parent's mipped host texture (full subresource; barriers restored).
void D3D12Backend::ResolvedMipsCopyLevel(ResMipChain& c, uint32_t l, ResolvedTex& s, ID3D12GraphicsCommandList* cl) {
    if (!c.created || !c.tex.res || !s.res || !cl || l >= c.levels) return;
    static std::atomic<uint64_t> s_cn{0};
    const D3D12_RESOURCE_DESC cd = c.tex.res->GetDesc(), sd = s.res->GetDesc();
    if (sd.Format != cd.Format || sd.Width != std::max<UINT64>(1, cd.Width >> l) || sd.Height != std::max<UINT>(1u, cd.Height >> l)) {
        c.filledMask &= ~(1u << l);  // level changed shape: chain is incomplete until a matching resolve arrives
        const uint64_t n = s_cn.fetch_add(1) + 1;
        if (n <= 8 || (n % 5000) == 0)
            MCLA_LOG_WARN("RESOLVED-MIPS-COPY-SKIP #{} parent={:08X} level={} src {}x{} fmt={} want {}x{} fmt={}", n, c.base[0], l,
                          sd.Width, sd.Height, uint32_t(sd.Format), std::max<UINT64>(1, cd.Width >> l), std::max<UINT>(1u, cd.Height >> l), uint32_t(cd.Format));
        return;
    }
    D3D12_RESOURCE_BARRIER b[2] = {};
    UINT nb = 0;
    auto tr = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        b[nb] = {};
        b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[nb].Transition.pResource = res;
        b[nb].Transition.StateBefore = from;
        b[nb].Transition.StateAfter = to;
        b[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++nb;
    };
    const D3D12_RESOURCE_STATES sPrev = s.state;
    if (c.tex.state != D3D12_RESOURCE_STATE_COPY_DEST) tr(c.tex.res.Get(), c.tex.state, D3D12_RESOURCE_STATE_COPY_DEST);
    if (sPrev != D3D12_RESOURCE_STATE_COPY_SOURCE) tr(s.res.Get(), sPrev, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (nb) cl->ResourceBarrier(nb, b);
    D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
    dl.pResource = c.tex.res.Get(); dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = l;
    sl.pResource = s.res.Get(); sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    nb = 0;
    tr(c.tex.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (sPrev != D3D12_RESOURCE_STATE_COPY_SOURCE) tr(s.res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, sPrev);
    cl->ResourceBarrier(nb, b);
    c.tex.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const bool wasFull = c.filledMask == c.fullMask;
    c.filledMask |= (1u << l);
    if (!wasFull && c.filledMask == c.fullMask) ++m_srvTableEpoch;  // bind memos must re-resolve onto the mipped SRV
    const uint64_t n = s_cn.fetch_add(1) + 1;
    if (n <= 12 || (n % 5000) == 0)
        MCLA_LOG_WARN("RESOLVED-MIPS-COPY #{} parent={:08X} level={}/{} {}x{} filled={:X}/{:X}", n, c.base[0], l, c.levels,
                      uint32_t(sd.Width), sd.Height, c.filledMask, c.fullMask);
}

// MD-09: returns the complete mipped host texture for resolved base `gb`, or nullptr (not found / incomplete / disabled).
// Creation and discovery happen only with a command list (main bind path); worker binds only look up.
D3D12Backend::ResMipChain* D3D12Backend::ResolvedMipsFind(uint32_t gb, ResolvedTex& b, uint32_t mipAddr, uint32_t mipMax, ID3D12GraphicsCommandList* cl) {
    if (!mipAddr || !m_device || !b.res || b.isDepth || b.w == 0 || b.h == 0) return nullptr;
    uint32_t fit = 1;
    for (uint32_t d = std::max(b.w, b.h); d > 1; d >>= 1) ++fit;
    const uint32_t levels = std::min<uint32_t>(std::min<uint32_t>(mipMax + 1u, fit), 13u);
    if (levels < 2) return nullptr;
    auto it = m_resMips.find(gb);
    if (it != m_resMips.end()) {
        ResMipChain& o = it->second;
        if (o.mipAddr != mipAddr || o.levels < levels || o.tex.w != b.w  /* fewer levels requested: reuse the larger chain (r4a: 1300x recreate cycling 2->3->4) */ || o.tex.h != b.h || o.tex.fmt != b.fmt) {
            if (!cl) return nullptr;
            if (o.tex.srvStagingSlot != UINT32_MAX) ReleaseStagingSrvSlot(o.tex.srvStagingSlot);
            if (o.tex.res) RetireTarget(o.tex.res);
            m_resMips.erase(it);
            ++m_srvTableEpoch;
            it = m_resMips.end();
        }
    }
    if (it == m_resMips.end()) {
        if (!cl) return nullptr;
        ResMipChain& n = m_resMips[gb];
        n.mipAddr = mipAddr; n.levels = levels; n.base[0] = gb; n.fullMask = (1u << levels) - 1u;
        n.tex.w = b.w; n.tex.h = b.h; n.tex.fmt = b.fmt; n.tex.srvFmt = b.fmt;
        m_resMipOwner[gb] = (uint64_t(gb) << 8) | 0u;
        it = m_resMips.find(gb);
    }
    ResMipChain& c = it->second;
    if (c.bad) return nullptr;
    const auto levelTex = [&](uint32_t base) -> ResolvedTex* {
        const auto lc = m_resolvedLastColour.find(base);
        if (lc == m_resolvedLastColour.end()) return nullptr;
        const auto r = m_resolved.find(lc->second);
        return r == m_resolved.end() ? nullptr : &r->second;
    };
    if (cl && !c.created && (c.attempts++ < 8 || (c.attempts & 63u) == 0)) {
        // Per-level guest base is not derivable from a contiguous-chain formula (measured 1F144000 -> 1F244000/1F284000/1F294000, r3a_90.log),
        // so find the resolved entry of exactly the level's size at/after mipAddr (level 1) or after the previous level's base.
        bool all = true;
        for (uint32_t l = 1; l < c.levels; ++l) {
            if (c.base[l]) continue;
            const uint32_t lw = std::max(1u, b.w >> l), lh = std::max(1u, b.h >> l);
            const uint32_t lo = l == 1 ? mipAddr : c.base[l - 1] + 1u;
            uint32_t best = 0;
            for (const auto& kv : m_resolvedLastColour) {
                if (kv.first < lo || kv.first >= mipAddr + 0x200000u || kv.first == gb || (best && kv.first >= best)) continue;
                const ResolvedTex* r = levelTex(kv.first);
                if (r && r->res && r->valid && !r->isDepth && r->w == lw && r->h == lh && r->fmt == b.fmt) best = kv.first;
            }
            if (!best) { all = false; break; }
            c.base[l] = best;
            m_resMipOwner[best] = (uint64_t(gb) << 8) | l;
        }
        if (all) {
            const UINT rs = RenderScale();
            D3D12_RESOURCE_DESC d = {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = uint64_t(b.w) * rs; d.Height = b.h * rs; d.DepthOrArraySize = 1; d.MipLevels = static_cast<UINT16>(c.levels);
            d.Format = b.fmt; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            for (uint32_t l = 1; l < c.levels; ++l)  // D3D12 mip dims must equal the separately resolved (scaled) level sizes
                if (std::max<UINT64>(1, d.Width >> l) != uint64_t(std::max(1u, b.w >> l)) * rs || std::max<UINT>(1u, d.Height >> l) != std::max(1u, b.h >> l) * rs) c.bad = true;
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            Microsoft::WRL::ComPtr<ID3D12Resource> r;
            if (!c.bad && SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r))) && r) {
                c.tex.res = r; c.tex.state = D3D12_RESOURCE_STATE_COMMON; c.tex.stateAtFrameStart = c.tex.state; c.tex.valid = true;
                c.created = true;
                MCLA_LOG_WARN("RESOLVED-MIPS-CREATE base={:08X} {}x{} levels={} fmt={} mipBases={:08X},{:08X},{:08X} scale={}", gb, b.w, b.h, c.levels,
                              uint32_t(b.fmt), c.base[1], c.levels > 2 ? c.base[2] : 0u, c.levels > 3 ? c.base[3] : 0u, rs);
            } else {
                c.bad = true;
                MCLA_LOG_WARN("RESOLVED-MIPS-CREATE-FAILED base={:08X} {}x{} levels={} (mip-size mismatch at scale {} or device error)", gb, b.w, b.h, c.levels, rs);
            }
        }
    }
    if (!c.created) return nullptr;
    if (cl) {
        for (uint32_t l = 0; l < c.levels; ++l) {  // initial fill from whatever each level already holds
            if (c.filledMask & (1u << l)) continue;
            ResolvedTex* s = l == 0 ? &b : levelTex(c.base[l]);
            if (s && s->valid && s->res) ResolvedMipsCopyLevel(c, l, *s, cl);
        }
    }
    return c.filledMask == c.fullMask ? &c : nullptr;
}

// Phase 6: Create shader-visible sampler descriptor heap
bool D3D12Backend::GuestSamplerTableEnabled() {
    static const bool s_on = [] {
        const char* e = std::getenv("MCLA_TEMP_GUEST_SAMPLER");
        return !(e && e[0] == '0');
    }();
    return s_on;
}

static D3D12_SAMPLER_DESC NeutralSamplerDesc() {
    D3D12_SAMPLER_DESC neutral = {};
    neutral.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    neutral.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    neutral.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    neutral.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    neutral.MipLODBias = 0.0f;
    neutral.MaxAnisotropy = 1;
    neutral.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    // D3D12_SAMPLER_DESC::BorderColor is a FLOAT[4]; the enum
    // D3D12_STATIC_BORDER_COLOR_* only exists on D3D12_STATIC_SAMPLER_DESC,
    // which this root signature no longer has.
    neutral.BorderColor[0] = 0.0f;
    neutral.BorderColor[1] = 0.0f;
    neutral.BorderColor[2] = 0.0f;
    neutral.BorderColor[3] = 0.0f;
    neutral.MinLOD = 0.0f;
    neutral.MaxLOD = D3D12_FLOAT32_MAX;
    return neutral;
}

bool D3D12Backend::CreateSamplerHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = kSamplerHeapSize;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_samplerHeap));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateDescriptorHeap (Sampler) failed with hr=0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }
    m_samplerDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    m_activeSamplerHeapIdx = 0;
    m_samplerHeapSwitched = false;

    // F-644: slot 0 is the NEUTRAL sampler, not a magic constant. Every
    // descriptor table must be initialised before the list executes. In table mode
    // (G0, MCLA_TEMP_GUEST_SAMPLER=1), initialize all 16 entries at offset 0 and start
    // m_samplerHeapOffset at 16 so every 16-entry block remains properly aligned.
    {
        const D3D12_SAMPLER_DESC neutral = NeutralSamplerDesc();
        const uint32_t neutralCount = GuestSamplerTableEnabled() ? renderer::kShaderSamplerCount : 1u;
        for (uint32_t h = 0; h < kSamplerHeapCount; ++h) {
            if (h == 0) {
                m_samplerHeaps[0].heap = m_samplerHeap;
            } else {
                HRESULT hrExtra = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_samplerHeaps[h].heap));
                if (FAILED(hrExtra)) {
                    MCLA_LOG_WARN("D3D12Backend: CreateDescriptorHeap (Sampler extra heap {}) failed hr=0x{:08X}",
                                  h, static_cast<uint32_t>(hrExtra));
                    m_samplerHeaps[h].heap.Reset();
                    continue;
                }
            }
            for (uint32_t i = 0; i < neutralCount; ++i) {
                D3D12_CPU_DESCRIPTOR_HANDLE slot(m_samplerHeaps[h].heap->GetCPUDescriptorHandleForHeapStart());
                slot.ptr += static_cast<SIZE_T>(i) * m_samplerDescriptorSize;
                m_device->CreateSampler(&neutral, slot);
            }
            m_samplerHeaps[h].heapOffset = neutralCount;
            m_samplerHeaps[h].blockIndex.clear();
            m_samplerHeaps[h].blocks.fill({});
            m_samplerHeaps[h].blockIndex.emplace(0, 0);  // key 0 = all-neutral block at base 0
        }
        m_samplerHeapOffset = neutralCount;
        m_guestSamplerIndex.clear();
        m_guestSamplerBlockIndex = m_samplerHeaps[0].blockIndex;
        m_samplerBlocks = m_samplerHeaps[0].blocks;
        m_samplerTableGpu = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        MCLA_LOG_INFO("D3D12Backend: Created sampler descriptor heap ({} descriptors x {} heaps, {} neutral at 0, tableMode={})",
                      kSamplerHeapSize, kSamplerHeapCount, neutralCount, GuestSamplerTableEnabled() ? 1 : 0);
    }
    return true;
}

// Phase 6: Get or create sampler descriptor in the shader-visible heap
D3D12Backend::SamplerDescriptor D3D12Backend::GetOrCreateSamplerDescriptor(const D3D12_SAMPLER_DESC& desc) {
    SamplerCacheKey key{ desc };
    auto it = m_samplerDescriptorIndex.find(key);
    if (it != m_samplerDescriptorIndex.end()) {
        uint32_t index = it->second;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_samplerHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += static_cast<SIZE_T>(index) * m_samplerDescriptorSize;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        gpuHandle.ptr += static_cast<SIZE_T>(index) * m_samplerDescriptorSize;
        return { cpuHandle, gpuHandle };
    }

    // Allocate new descriptor
    if (m_samplerHeapOffset >= kSamplerHeapSize) {
        MCLA_LOG_ERROR("D3D12Backend: Sampler descriptor heap exhausted ({} slots)", kSamplerHeapSize);
        return { {}, {} };
    }

    uint32_t index = m_samplerHeapOffset++;
    m_samplerDescriptorIndex.emplace(key, index);

    // Create the sampler in the heap
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_samplerHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(index) * m_samplerDescriptorSize;
    m_device->CreateSampler(&desc, cpuHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
    gpuHandle.ptr += static_cast<SIZE_T>(index) * m_samplerDescriptorSize;

    return { cpuHandle, gpuHandle };
}

// Bind the shader-visible sampler descriptor heap to the command list.
// D3D12 unbinds previously bound heaps when a new SetDescriptorHeaps call names a
// different set, so the SRV heap must be named here too or root param 0's SRV
// table would reference an unbound heap.
void D3D12Backend::BindSamplerHeap() {
    if (!m_samplerHeap) return;
    if (m_srvHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get(), m_samplerHeap.Get() };
        m_commandList->SetDescriptorHeaps(2, heaps);
    } else {
        ID3D12DescriptorHeap* heaps[] = { m_samplerHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
    }
}

// Phase 7: Render Graph implementation
D3D12Backend::RenderGraphBuilder D3D12Backend::CreateRenderGraph() {
    return RenderGraphBuilder(this);
}

D3D12Backend::RenderGraphBuilder::PassHandle D3D12Backend::RenderGraphBuilder::AddPass(
    std::wstring name,
    std::vector<ResourceAccess> resources,
    std::function<void(ID3D12GraphicsCommandList*)> execute) {
    RenderGraphPass pass;
    pass.name = std::move(name);
    pass.resources = std::move(resources);
    pass.execute = std::move(execute);
    PassHandle handle{ static_cast<uint32_t>(m_backend->m_renderGraphPasses.size()) };
    m_backend->m_renderGraphPasses.push_back(std::move(pass));
    return handle;
}

ID3D12Resource* D3D12Backend::RenderGraphBuilder::GetTransientResource(const TransientResourceDesc& desc) {
    // Search for existing reusable resource
    for (auto& entry : m_backend->m_transientResources) {
        if (!entry.inUse && entry.desc.Width == desc.desc.Width &&
            entry.desc.Height == desc.desc.Height &&
            entry.desc.Format == desc.desc.Format &&
            entry.desc.DepthOrArraySize == desc.desc.DepthOrArraySize &&
            entry.desc.MipLevels == desc.desc.MipLevels &&
            entry.desc.SampleDesc.Count == desc.desc.SampleDesc.Count &&
            entry.desc.Flags == desc.desc.Flags) {
            entry.inUse = true;
            entry.lastUsedFrame = m_backend->m_frameIndex;
            return entry.resource.Get();
        }
    }

    // Create new resource
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_backend->m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc.desc,
        D3D12_RESOURCE_STATE_COMMON,
        &desc.clearValue, IID_PPV_ARGS(&resource));

    if (FAILED(hr)) {
        MCLA_LOG_ERROR("RenderGraph: Failed to create transient resource '{}', hr=0x{:08X}",
                     std::string(desc.name.begin(), desc.name.end()).c_str(),
                     static_cast<uint32_t>(hr));
        return nullptr;
    }

    TransientResourceEntry entry;
    entry.name = desc.name;
    entry.desc = desc.desc;
    entry.clearValue = desc.clearValue;
    entry.resource = resource;
    entry.lastUsedFrame = m_backend->m_frameIndex;
    entry.inUse = true;

    m_backend->m_transientResources.push_back(std::move(entry));
    return m_backend->m_transientResources.back().resource.Get();
}

bool D3D12Backend::RenderGraphBuilder::Build() {
    auto& passes = m_backend->m_renderGraphPasses;
    auto& resources = m_backend->m_transientResources;
    auto& executionOrder = m_backend->m_renderGraphExecutionOrder;

    // Build dependency graph based on resource access patterns
    for (uint32_t i = 0; i < passes.size(); ++i) {
        for (uint32_t j = i + 1; j < passes.size(); ++j) {
            const auto& passA = passes[i];
            const auto& passB = passes[j];

            // Check for resource dependencies
            bool hasDependency = false;
            for (const auto& resA : passA.resources) {
                for (const auto& resB : passB.resources) {
                    if (resA.resource == resB.resource) {
                        // Same resource accessed - check for write dependency
                        if (resA.usage == ResourceUsage::Write || resB.usage == ResourceUsage::Write ||
                            resA.usage == ResourceUsage::ReadWrite || resB.usage == ResourceUsage::ReadWrite) {
                            hasDependency = true;
                            break;
                        }
                    }
                }
                if (hasDependency) break;
            }
            if (hasDependency) {
                passes[i].dependents.push_back(j);
                passes[j].dependencies.push_back(i);
            }
        }
    }

    // Topological sort (Kahn's algorithm)
    std::vector<uint32_t> inDegree(passes.size(), 0);
    for (const auto& pass : passes) {
        for (uint32_t dep : pass.dependencies) {
            inDegree[dep]++;
        }
    }

    std::queue<uint32_t> q;
    for (uint32_t i = 0; i < passes.size(); ++i) {
        if (inDegree[i] == 0) q.push(i);
    }

    executionOrder.clear();
    while (!q.empty()) {
        uint32_t u = q.front();
        q.pop();
        executionOrder.push_back(u);
        for (uint32_t v : passes[u].dependents) {
            if (--inDegree[v] == 0) {
                q.push(v);
            }
        }
    }

    if (executionOrder.size() != passes.size()) {
        MCLA_LOG_ERROR("RenderGraph: Cycle detected in pass dependencies");
        return false;
    }

    // Allocate transient resources
    for (auto& res : resources) {
        if (!res.inUse) {
            // Already allocated or will be allocated on first use
        }
    }

    m_backend->m_renderGraphBuilt = true;
    return true;
}

void D3D12Backend::RenderGraphBuilder::Execute() {
    if (!m_backend->m_renderGraphBuilt) return;

    auto& passes = m_backend->m_renderGraphPasses;
    auto& executionOrder = m_backend->m_renderGraphExecutionOrder;

    for (uint32_t idx : executionOrder) {
        const auto& pass = passes[idx];

        // Emit barriers for resource state transitions
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (const auto& res : pass.resources) {
            if (res.resource) {
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = res.resource;
                barrier.Transition.StateBefore = res.beforeState;
                barrier.Transition.StateAfter = res.afterState;
                barrier.Transition.Subresource = res.subresource;
                barriers.push_back(barrier);
            }
        }
        if (!barriers.empty()) {
            m_backend->m_commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }

        // Execute the pass
        if (pass.execute) {
            pass.execute(m_backend->m_commandList.Get());
        }

        // Emit UAV barriers for resources written
        std::vector<D3D12_RESOURCE_BARRIER> uavBarriers;
        for (const auto& res : pass.resources) {
            if (res.resource && (res.usage == ResourceUsage::Write || res.usage == ResourceUsage::ReadWrite)) {
                D3D12_RESOURCE_BARRIER uavBarrier = {};
                uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uavBarrier.UAV.pResource = res.resource;
                uavBarriers.push_back(uavBarrier);
            }
        }
        if (!uavBarriers.empty()) {
            m_backend->m_commandList->ResourceBarrier(static_cast<UINT>(uavBarriers.size()), uavBarriers.data());
        }
    }

    // Mark transient resources as no longer in use
    for (auto& res : m_backend->m_transientResources) {
        res.inUse = false;
    }
}

void D3D12Backend::RenderGraphBuilder::Reset() {
    m_backend->m_renderGraphPasses.clear();
    m_backend->m_renderGraphExecutionOrder.clear();
    m_backend->m_renderGraphBuilt = false;
    // Note: transient resources are kept for reuse, only marked as not in use
    for (auto& res : m_backend->m_transientResources) {
        res.inUse = false;
    }
}
// ============================================================================
// Phase 4 texture import: plan-driven (F-638)
// ============================================================================
//
// Phase 4 uploads host-linear decoded pixel bytes (already untiled by
// texture_decode) into a DEFAULT-heap texture and exposes it as a shader-visible
// SRV. The backend never touches guest memory here.
//
// F-638 replaced the old two-case `switch (format) { R8G8B8A8, R8 }` gate with
// the plan's own nativeDxgiFormat plus a block-format bytes-per-BLOCK table, so
// the BC1/BC2/BC3/BC4/BC5 forms PlanTextureImport already declares as
// DirectBlockCopy are importable zero-copy. The gate is now "is the format in
// the table", and a format that is not is a refusal with a name, never a
// substituted R8G8B8A8.

bool D3D12Backend::CreateDecodedTexture(const uint8_t* linearPixels, uint32_t width,
                                        uint32_t height, uint32_t dxgiFormat) {
    // Legacy 4-arg form. tools/validators/texture_import_validator.cpp:379 calls
    // it with a scalar RGBA payload, so the forwarder synthesises the scalar
    // plan that PlanTextureImport would have produced and hands it to the
    // plan-driven path. This is NOT a new format gate: the switch below is the
    // same two cases as before, so the validator's observable behaviour is
    // unchanged.
    switch (static_cast<DXGI_FORMAT>(dxgiFormat)) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8_UNORM:
            break;
        default:
            ++g_textureImportRefusalOther;
            ++g_textureImportBackendRefusals[static_cast<uint32_t>(
                TextureBackendRefusal::LegacyUnsupportedDxgi)];
            MCLA_LOG_ERROR("D3D12Backend: CreateDecodedTexture(legacy) unsupported dxgi format {}",
                           dxgiFormat);
            return false;
    }
    if (!linearPixels || width == 0 || height == 0) {
        // F-649: this branch used to be a SILENT count bump with no log at all --
        // the one refusal site in this file that produced neither a marker nor a
        // reason, which is exactly how a row becomes uncountable by a soak that
        // only reads lines. Now named and bucketed.
        ++g_textureImportRefusalOther;
        ++g_textureImportBackendRefusals[static_cast<uint32_t>(
            TextureBackendRefusal::LegacyBadArgs)];
        MCLA_LOG_ERROR("D3D12Backend: CreateDecodedTexture(legacy) refused: pixels={} {}x{}",
                       linearPixels ? "present" : "NULL", width, height);
        return false;
    }

    TextureImportDesc imp = {};
    native::TextureImportPlan plan = {};
    plan.ok = true;
    plan.mode = native::TextureUploadMode::ScalarCopy;
    plan.nativeDxgiFormat = dxgiFormat;
    plan.importDxgiFormat = dxgiFormat;
    plan.blockWidth = 1;
    plan.blockHeight = 1;
    plan.texelBytes = (dxgiFormat == DXGI_FORMAT_R8_UNORM) ? 1u : 4u;
    plan.blockBytes = plan.texelBytes;
    plan.blocksX = width;
    plan.blocksY = height;
    plan.formatName = "legacy-scalar";
    plan.linearByteSize = static_cast<uint32_t>(width) * height * plan.texelBytes;

    imp.plan = &plan;
    imp.mipPayloads[0] = linearPixels;
    imp.mipBytes[0] = plan.linearByteSize;
    imp.mipCount = 1;
    imp.width = width;
    imp.height = height;
    imp.srvSlot = kTexture2DSlot;
    return CreateDecodedTexture(imp);
}

bool D3D12Backend::AllocateTextureHeapRange(uint64_t size, uint64_t alignment,
                                            uint32_t& heapIndexOut, uint64_t& offsetOut) {
    if (!m_device || size == 0 || size > kTextureHeapMaxPlacedSize) return false;
    if (alignment == 0) alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    auto tryFind = [&](uint32_t startHeap) -> bool {
        for (uint32_t hi = startHeap; hi < static_cast<uint32_t>(m_textureHeaps.size()); ++hi) {
            TextureHeap& heap = m_textureHeaps[hi];
            for (auto it = heap.freeRanges.begin(); it != heap.freeRanges.end(); ++it) {
                const uint64_t rangeStart = it->first;
                const uint64_t rangeEnd = rangeStart + it->second;
                const uint64_t aligned = (rangeStart + alignment - 1) & ~(alignment - 1);
                if (aligned >= rangeStart && aligned <= rangeEnd && size <= (rangeEnd - aligned)) {
                    const uint64_t prefix = aligned - rangeStart;
                    const uint64_t suffix = rangeEnd - (aligned + size);
                    const uint64_t suffixStart = aligned + size;
                    heap.freeRanges.erase(it);
                    if (prefix > 0) heap.freeRanges.emplace(rangeStart, prefix);
                    if (suffix > 0) heap.freeRanges.emplace(suffixStart, suffix);
                    heap.used += size;
                    heapIndexOut = hi;
                    offsetOut = aligned;
                    return true;
                }
            }
        }
        return false;
    };
    if (tryFind(0)) return true;
    if (!m_retiredPlacedRanges.empty()) {
        PruneRetiredTargets();
        if (tryFind(0)) return true;
    }
    if (size > kTextureHeapSize || alignment > kTextureHeapSize) return false;
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = kTextureHeapSize;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    TextureHeap nh;
    const HRESULT hr = m_device->CreateHeap(&hd, IID_PPV_ARGS(&nh.heap));
    if (FAILED(hr) || !nh.heap) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            MCLA_LOG_WARN("D3D12Backend: placed texture CreateHeap(256MB) failed hr=0x{:08X}; "
                          "falling back to committed textures",
                          static_cast<uint32_t>(hr));
        }
        return false;
    }
    nh.freeRanges.emplace(0, kTextureHeapSize);
    const uint32_t newIdx = static_cast<uint32_t>(m_textureHeaps.size());
    m_textureHeaps.push_back(std::move(nh));
    return tryFind(newIdx);
}

void D3D12Backend::FreeTextureHeapRange(uint32_t heapIndex, uint64_t offset, uint64_t size) {
    if (heapIndex >= m_textureHeaps.size() || size == 0) return;
    TextureHeap& heap = m_textureHeaps[heapIndex];
    heap.used = (heap.used >= size) ? (heap.used - size) : 0;
    auto [it, inserted] = heap.freeRanges.emplace(offset, size);
    if (!inserted) {
        it->second = std::max(it->second, size);
    }
    auto next = std::next(it);
    if (next != heap.freeRanges.end() && it->first + it->second == next->first) {
        it->second += next->second;
        heap.freeRanges.erase(next);
    }
    if (it != heap.freeRanges.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            heap.freeRanges.erase(it);
        }
    }
}

bool D3D12Backend::AllocateTexUploadSlice(uint64_t totalBytes, ID3D12Resource*& outResource,
                                          uint8_t*& outCpuPtr, uint64_t& outOffset) {
    if (!m_device || totalBytes == 0) return false;
    const uint64_t done = m_fence ? m_fence->GetCompletedValue() : 0;
    auto tryChunks = [&]() -> bool {
        for (auto& chunk : m_texUploadChunks) {
            if (!chunk.buffer || !chunk.mapped) continue;
            if (!chunk.inCurrentFrame && chunk.fenceValue <= done) {
                chunk.used = 0;
            }
            const uint64_t aligned = (chunk.used + (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)) &
                                     ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
            if (aligned <= chunk.capacity && totalBytes <= chunk.capacity - aligned) {
                chunk.used = aligned + totalBytes;
                chunk.inCurrentFrame = true;
                outResource = chunk.buffer.Get();
                outCpuPtr = chunk.mapped + aligned;
                outOffset = aligned;
                return true;
            }
        }
        return false;
    };
    if (tryChunks()) return true;

    constexpr uint64_t kDefaultChunkBytes = UINT64_C(16) << 20;
    const uint64_t cap = std::max(kDefaultChunkBytes, totalBytes);
    D3D12_HEAP_PROPERTIES upProps = {};
    upProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = cap;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    TexUploadChunk nc;
    nc.capacity = cap;
    HRESULT hr = m_device->CreateCommittedResource(
        &upProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&nc.buffer));
    if (FAILED(hr) || !nc.buffer) return false;
    void* mapped = nullptr;
    hr = nc.buffer->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) return false;
    nc.mapped = static_cast<uint8_t*>(mapped);
    nc.used = totalBytes;
    nc.inCurrentFrame = true;
    outResource = nc.buffer.Get();
    outCpuPtr = nc.mapped;
    outOffset = 0;
    m_texUploadChunks.push_back(std::move(nc));
    return true;
}

bool D3D12Backend::CreateDecodedTexture(const TextureImportDesc& desc) {
    return CreateDecodedTexture(desc, m_commandList.Get(), true);
}

bool D3D12Backend::CreateDecodedTexture(const TextureImportDesc& desc, ID3D12GraphicsCommandList* recordCl, bool allowMipgen) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || !m_srvHeap) return false;
    ID3D12GraphicsCommandList* cl = recordCl ? recordCl : m_commandList.Get();
    if (!cl) return false;

    const native::TextureImportPlan& plan = *desc.plan;

    // --- Gate 0 (F-649): THE MARKER. `TEX-IMPORT` did not exist as a source
    // literal before this line, so its absence in every historical soak measured
    // the absence of a log call and nothing else. It is placed BEFORE Gate 1 and
    // OUTSIDE the refusal branch so it fires on every plan -- accepted or
    // refused -- because a refusal that does not print is a refusal nobody
    // learns from, and "plans attempted" is the row that distinguishes "the
    // title never reached the import path" from "it reached it and every plan
    // died here". The counter is bumped unconditionally; the house bound
    // (n <= kRefuseLogFirst || n % kRefuseLogEvery == 0) gates the TEXT only.
    const uint64_t planNo = ++g_textureImportPlans;
    if (ShouldLogRefusal(planNo)) {
        MCLA_LOG_INFO("TEX-IMPORT #{} fmt={} {} dxgi={} mode={} {}x{} mips={} blockB={} ok={} "
                      "refusal={} slot={}",
                      planNo, plan.xenosFormat, plan.formatName,
                      plan.importDxgiFormat ? plan.importDxgiFormat : plan.nativeDxgiFormat,
                      (uint32_t)plan.mode, desc.width, desc.height, desc.mipCount,
                      plan.blockBytes, plan.ok ? 1 : 0,
                      native::TextureRefusalName(plan.refusal), desc.srvSlot);
    }

    // --- Gate 1: the plan itself. A plan with ok == false carries a named
    // refusal; it is logged by that name and counted in its own bucket, so a
    // soak can attribute every missing texture to a reason instead of noticing
    // "the texture is black" much later.
    if (!plan.ok) {
        const uint64_t n = ++g_textureImportRefusals[static_cast<uint32_t>(plan.refusal)];
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_ERROR("D3D12Backend: texture import refused #{} reason={} (xenosFmt={} {} "
                           "tiling) -- NULL SRV bound at slot {}",
                           n, native::TextureRefusalName(plan.refusal), plan.xenosFormat,
                           plan.formatName, desc.srvSlot);
        }
        return false;
    }

    // --- Gate 2: can THIS backend carry the payload's REAL post-expansion
    // format? F-900 HIGH-3 HALF 2: for CpuExpand plans the payload that
    // reaches the copy is in importDxgiFormat (RGBA8/R8 after
    // ExpandTextureToImportBytes), NOT nativeDxgiFormat (the BC form) -- using
    // native here built a BC3 resource, sized the footprint from BC3, and then
    // copied RGBA8 bytes under a BC3 source pitch (first 1024 of 4096 B), while
    // the cache-hit SRV was created with key.importDxgiFormat over that same BC3
    // resource = an illegal RGBA8-over-BC3 view (undefined behaviour; debug layer
    // was off, so nothing named it). For DirectBlockCopy/ScalarCopy plans
    // importDxgiFormat == native, so those paths are byte-identical; behaviour
    // changes only for CpuExpand (18/19/20/51/52/53 -> RGBA8, 58/59 -> R8).
    // FindHostFormat returning nullptr is the "no table row" refusal. It is
    // deliberately not a fallback: sampling a payload under a format it was not
    // produced in is a different image, not a blurrier one.
    const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(
        plan.importDxgiFormat ? plan.importDxgiFormat : plan.nativeDxgiFormat);
    const HostFormatFootprint* fp = FindHostFormat(format);
    if (static_cast<uint32_t>(format) == 0 || !fp) {
        const uint64_t n = ++g_textureImportRefusalOther;
        ++g_textureImportBackendRefusals[static_cast<uint32_t>(
            TextureBackendRefusal::NoHostFormat)];
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_ERROR("D3D12Backend: texture import refused #{} reason=NoHostFormat: "
                           "plan wants dxgiFormat {} (native={} xenosFmt={} {}), which is not "
                           "in the backend import table -- NULL SRV bound at slot {}",
                           n, static_cast<uint32_t>(format), plan.nativeDxgiFormat,
                           plan.xenosFormat, plan.formatName,
                           desc.srvSlot);
        }
        return false;
    }
    if (desc.width == 0 || desc.height == 0) {
        ++g_textureImportRefusalOther;
        ++g_textureImportBackendRefusals[static_cast<uint32_t>(TextureBackendRefusal::ZeroExtent)];
        MCLA_LOG_ERROR("D3D12Backend: texture import refused: zero extent ({}x{})", desc.width,
                       desc.height);
        return false;
    }

    // --- Gate 3: the mip chain. Three mutually exclusive shapes, and the
    // caller must say which: one uploaded level, N uploaded levels, or N levels
    // derived by the mipgen PSO. A caller that supplies neither a payload per
    // level nor generateMipChain is refused rather than given MipLevels = 1
    // with the guest's level count silently discarded.
    static const bool s_noCpuExpandMips = [] {
        const char* e = std::getenv("MCLA_NO_CPUEXPAND_MIPS");
        return e && e[0] == '1';
    }();
    const bool clampCpuExpandMips =
        (plan.mode == native::TextureUploadMode::CpuExpand && desc.mipCount > 1 && s_noCpuExpandMips);
    const uint32_t mipCount = clampCpuExpandMips ? 1u : desc.mipCount;
    if (mipCount == 0 || mipCount > native::kTextureMaxMips) {
        ++g_textureImportRefusalOther;
        ++g_textureImportBackendRefusals[static_cast<uint32_t>(
            TextureBackendRefusal::MipCountOutOfRange)];
        MCLA_LOG_ERROR("D3D12Backend: texture import refused: mipCount {} outside 1..{}",
                       mipCount, native::kTextureMaxMips);
        return false;
    }
    if (mipCount > 1 && desc.generateMipChain) {
        ++g_textureImportRefusalOther;
        ++g_textureImportBackendRefusals[static_cast<uint32_t>(
            TextureBackendRefusal::MipChainAndGenerate)];
        MCLA_LOG_ERROR("D3D12Backend: texture import refused: {} mips AND generateMipChain; the "
                       "chain is either supplied or generated, never both", mipCount);
        return false;
    }
    if (mipCount > 1) {
        // Mip N's texel extent is max(1, extent >> N); D3D12 requires the full
        // chain down to 1x1, so a chain that stops short of it is refused here
        // rather than left as undefined subresource content.
        for (uint32_t i = 0; i < mipCount; ++i) {
            if (!desc.mipPayloads[i] || desc.mipBytes[i] == 0) {
                ++g_textureImportRefusalOther;
                ++g_textureImportBackendRefusals[static_cast<uint32_t>(
                    TextureBackendRefusal::MipLevelHasNoPayload)];
                MCLA_LOG_ERROR("D3D12Backend: texture import refused: mip {} of {} has no payload "
                               "(the chain was declared but not supplied)",
                               i, mipCount);
                return false;
            }
        }
    }
    if (desc.mipPayloads[0] == nullptr || desc.mipBytes[0] == 0) {
        ++g_textureImportRefusalOther;
        ++g_textureImportBackendRefusals[static_cast<uint32_t>(
            TextureBackendRefusal::MipZeroHasNoPayload)];
        MCLA_LOG_ERROR("D3D12Backend: texture import refused: mip 0 has no payload");
        return false;
    }
    if (desc.srvSlot >= m_srvHeapDescriptorCount) {
        ++g_textureImportRefusalOther;
        ++g_textureImportBackendRefusals[static_cast<uint32_t>(
            TextureBackendRefusal::SrvSlotOutOfRange)];
        MCLA_LOG_ERROR("D3D12Backend: texture import refused: SRV slot {} outside 0..{}",
                       desc.srvSlot, m_srvHeapDescriptorCount - 1);
        return false;
    }

    // --- CpuExpand: the payload is not the import form yet.
    //
    // texture_decode.h:398-406 makes this the CPU twin of Xenia's load shaders
    // and REFUSES a ragged payload rather than partially writing, so the check is
    // one call. The output lands in a member scratch buffer so the draw path
    // does not allocate per texture.
    //
    // F-914 tofu Fix 1: SKIPPED when the caller already handed post-expand
    // bytes (desc.payloadAlreadyExpanded, set by BindGuestTextures' MISS path
    // -- those bytes are the very buffer the content hash, halo fix and
    // testpat ran on, so hash and upload finally agree). Re-expanding them
    // would alias m_importScratch against itself (F-900, :2541-2548) and
    // upload zeros under a cache key that claims real pixels.
    const uint8_t* level0 = desc.mipPayloads[0];
    uint32_t level0Bytes = desc.mipBytes[0];
    uint32_t mipExpandedOff[native::kTextureMaxMips] = {};
    uint32_t mipExpandedBytes[native::kTextureMaxMips] = {};
    bool mipExpandedInScratch[native::kTextureMaxMips] = {};
    if (plan.mode == native::TextureUploadMode::CpuExpand) {
        if (!desc.payloadAlreadyExpanded) {
            native::TextureRefusal expandRefusal = native::TextureRefusal::None;
            if (!native::ExpandTextureToImportBytes(plan, level0, level0Bytes, m_importScratch,
                                                    &expandRefusal)) {
                const uint64_t n = ++g_textureImportRefusals[static_cast<uint32_t>(expandRefusal)];
                if (ShouldLogRefusal(n)) {
                    MCLA_LOG_ERROR("D3D12Backend: texture expand refused #{} reason={} (xenosFmt={} "
                                   "{} payload={}B) -- NULL SRV bound at slot {}",
                                   n, native::TextureRefusalName(expandRefusal), plan.xenosFormat,
                                   plan.formatName, level0Bytes, desc.srvSlot);
                }
                return false;
            }
            level0 = m_importScratch.data();
            level0Bytes = static_cast<uint32_t>(m_importScratch.size());
        }
        // Multi-mip CpuExpand (#15): expand levels 1..mipCount-1 into m_importMipScratch, leaving
        // m_importScratch intact (desc.payloadAlreadyExpanded aliases m_importScratch for level 0)
        // and cropping sub-block/NPOT mips (e.g. 2x2/1x1) from expanded block width to mipW.
        if (mipCount > 1 && !desc.generateMipChain) {
            m_importMipScratch.clear();
            for (uint32_t i = 1; i < mipCount; ++i) {
                const uint32_t mipW = std::max<uint32_t>(1u, desc.width >> i);
                const uint32_t mipH = std::max<uint32_t>(1u, desc.height >> i);
                native::TextureImportPlan mipPlan = plan;
                mipPlan.blocksX = BlocksFor(mipW, plan.blockWidth);
                mipPlan.blocksY = BlocksFor(mipH, plan.blockHeight);
                mipPlan.linearByteSize = static_cast<uint32_t>(
                    uint64_t(mipPlan.blocksX) * mipPlan.blocksY * plan.blockBytes * mipPlan.arraySize);
                native::TextureRefusal expandRefusal = native::TextureRefusal::None;
                if (!native::ExpandTextureToImportBytes(mipPlan, desc.mipPayloads[i], desc.mipBytes[i],
                                                        m_importMipLevelTmp, &expandRefusal)) {
                    const uint64_t n = ++g_textureImportRefusals[static_cast<uint32_t>(expandRefusal)];
                    if (ShouldLogRefusal(n)) {
                        MCLA_LOG_ERROR("D3D12Backend: texture mip {} expand refused #{} reason={} "
                                       "(xenosFmt={} {} {}x{} payload={}B) -- NULL SRV at slot {}",
                                       i, n, native::TextureRefusalName(expandRefusal),
                                       plan.xenosFormat, plan.formatName, mipW, mipH,
                                       desc.mipBytes[i], desc.srvSlot);
                    }
                    return false;
                }
                const uint32_t expandedW = mipPlan.blocksX * plan.blockWidth;
                const uint64_t expandedStride = static_cast<uint64_t>(expandedW) * fp->bytesPerBlock;
                const uint64_t tightRowBytes = static_cast<uint64_t>(mipW) * fp->bytesPerBlock;
                const uint64_t tightTotal = tightRowBytes * mipH;
                const uint64_t minExpandedBytes =
                    (mipH > 0) ? (static_cast<uint64_t>(mipH - 1u) * expandedStride + tightRowBytes) : 0ull;
                if (m_importMipLevelTmp.size() < minExpandedBytes) {
                    ++g_textureImportRefusalOther;
                    MCLA_LOG_ERROR("D3D12Backend: texture mip {} expanded buffer short ({}B < {}B)",
                                   i, m_importMipLevelTmp.size(), minExpandedBytes);
                    return false;
                }
                const uint32_t off = static_cast<uint32_t>(m_importMipScratch.size());
                m_importMipScratch.resize(static_cast<size_t>(off) + static_cast<size_t>(tightTotal));
                uint8_t* dstMip = m_importMipScratch.data() + off;
                if (expandedStride == tightRowBytes) {
                    std::memcpy(dstMip, m_importMipLevelTmp.data(), static_cast<size_t>(tightTotal));
                } else {
                    for (uint32_t y = 0; y < mipH; ++y) {
                        std::memcpy(dstMip + static_cast<size_t>(y) * tightRowBytes,
                                    m_importMipLevelTmp.data() + static_cast<size_t>(y) * expandedStride,
                                    static_cast<size_t>(tightRowBytes));
                    }
                }
                mipExpandedOff[i] = off;
                mipExpandedBytes[i] = static_cast<uint32_t>(tightTotal);
                mipExpandedInScratch[i] = true;
            }
        }
    } else if (plan.mode == native::TextureUploadMode::Refused) {
        const uint64_t n = ++g_textureImportRefusals[static_cast<uint32_t>(plan.refusal)];
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_ERROR("D3D12Backend: texture import refused #{}: plan.mode == Refused "
                           "(reason={}) -- NULL SRV bound at slot {}",
                           n, native::TextureRefusalName(plan.refusal), desc.srvSlot);
        }
        return false;
    }

    // --- Resource creation.
    D3D12_HEAP_PROPERTIES defProps = {};
    defProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    defProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = desc.width;
    texDesc.Height = desc.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = mipCount;
    texDesc.SampleDesc.Count = 1;
    texDesc.Format = format;
    // RGBA8/BC1-3 are created TYPELESS so each bind can view them UNORM or _SRGB (Xenos gamma
    // fetch sign, docs/AUDIT_GAMMA_SRGB.md 3A). A generated mip chain keeps the typed format (UAV).
    if (!desc.generateMipChain) {
        switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: texDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; break;
            case DXGI_FORMAT_BC1_UNORM: texDesc.Format = DXGI_FORMAT_BC1_TYPELESS; break;
            case DXGI_FORMAT_BC2_UNORM: texDesc.Format = DXGI_FORMAT_BC2_TYPELESS; break;
            case DXGI_FORMAT_BC3_UNORM: texDesc.Format = DXGI_FORMAT_BC3_TYPELESS; break;
            default: break;
        }
    }
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // F-641: the UAV flag is the contract GenerateMipmaps checks at
    // :1389 (texture_decode's own note and the old code agreed: CreateDecoded-
    // Texture set MipLevels = 1 with NO UAV flag, then GenerateMipmaps errored
    // "lacks ALLOW_UNORDERED_ACCESS" for the same resource). Now the flag and
    // the mip count are decided by the same `desc`, so the two can no longer
    // disagree: a single-mip texture is created WITHOUT the flag and must not be
    // followed by GenerateMipmaps; a generated chain is created WITH it.
    if (desc.generateMipChain) {
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    static const bool s_noPlacedTex = [] {
        const char* e = std::getenv("MCLA_NO_PLACED_TEX");
        return e && e[0] == '1';
    }();

    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    uint32_t placedHeapIndex = UINT32_MAX;
    uint64_t placedHeapOffset = 0;
    uint64_t placedHeapSize = 0;
    HRESULT hr = E_FAIL;

    if (!s_noPlacedTex) {
        const D3D12_RESOURCE_ALLOCATION_INFO allocInfo =
            m_device->GetResourceAllocationInfo(0, 1, &texDesc);
        if (allocInfo.SizeInBytes > 0 && allocInfo.SizeInBytes <= kTextureHeapMaxPlacedSize &&
            AllocateTextureHeapRange(allocInfo.SizeInBytes, allocInfo.Alignment,
                                     placedHeapIndex, placedHeapOffset)) {
            hr = m_device->CreatePlacedResource(
                m_textureHeaps[placedHeapIndex].heap.Get(), placedHeapOffset,
                &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
            if (FAILED(hr) || !texture) {
                FreeTextureHeapRange(placedHeapIndex, placedHeapOffset, allocInfo.SizeInBytes);
                placedHeapIndex = UINT32_MAX;
                placedHeapOffset = 0;
            } else {
                placedHeapSize = allocInfo.SizeInBytes;
            }
        }
    }

    if (!texture) {
        hr = m_device->CreateCommittedResource(
            &defProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&texture));
    }
    if (FAILED(hr) || !texture) {
        ++g_textureImportRefusalOther;
        // F-900: ONE-SHOT DEVICE-REMOVED REASON PROBE. The first failure in
        // build/g961.log was hr=0x887A0005 thrown from inside this call
        // (CreateDecodedTexture+0x8D6 <- BindGuestTextures+0xF22), and NVIDIA
        // drivers are known to report DXGI_ERROR_DEVICE_REMOVED from creation
        // APIs as a generic creation-failure code -- GetDeviceRemovedReason
        // distinguishes "the device really died" (S_OK would be impossible) from
        // "a creation path failed while reporting 0x887A0005". S_OK here proves
        // the device was never removed. Nothing in src/ called this before
        // (grep: 0 hits), so no soak could tell the two apart.
        static bool s_removedReasonLogged = false;
        if (!s_removedReasonLogged) {
            s_removedReasonLogged = true;
            const HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : E_FAIL;
            MCLA_LOG_ERROR("D3D12Backend: DEVICE-REMOVED-REASON #1 hr=0x{:08X} "
                           "removedReason=0x{:08X} -- reason={} (S_OK: the device "
                           "was NEVER removed; 0x887A0005 here is a creation-path "
                           "failure reporting the device-removed code)",
                           static_cast<uint32_t>(hr), static_cast<uint32_t>(reason),
                           reason == S_OK ? "DEVICE_OK" : "DEVICE_REALLY_REMOVED");
        }
        MCLA_LOG_ERROR("D3D12Backend: texture import refused: CreateCommittedResource "
                       "{}x{}x{} mips fmt={} failed hr=0x{:08X}",
                       desc.width, desc.height, mipCount, static_cast<uint32_t>(format),
                       static_cast<uint32_t>(hr));
        return false;
    }

    auto releaseUnusedPlaced = [&]() {
        if (placedHeapIndex != UINT32_MAX) {
            texture.Reset();
            FreeTextureHeapRange(placedHeapIndex, placedHeapOffset, placedHeapSize);
            placedHeapIndex = UINT32_MAX;
            placedHeapOffset = 0;
            placedHeapSize = 0;
        }
    };

    // F-900: RECORD THE TRACKED STATE AT CREATION. The resource is created in
    // COPY_DEST (the CreateCommittedResource initial state above), but the
    // tracker had no entry for it, so TrackedState() returned COMMON for this
    // "untracked" resource and the post-copy barrier emitted
    // COMMON -> PIXEL_SHADER_RESOURCE on a resource the driver actually held in
    // COPY_DEST -- a spec violation (the comment at the barrier site asserting
    // "Common is what CreateCommittedResource left it in" is false for this
    // path; the debug layer was off in g961/g962, so it never said so).
    SetTrackedState(texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_COPY_DEST);

    if (!m_inFrame) {
        if (!BeginFrame()) {
            releaseUnusedPlaced();
            return false;
        }
    }

    // --- Upload. One staging buffer for the whole chain, laid out by
    // GetCopyableFootprints per subresource, so the destination row pitch comes
    // from the driver and only the SOURCE pitch has to be derived from the
    // format. For a 4x4 block format that is
    // blocksX * bytesPerBlock, not width * bytesPerBlock -- the distinction the
    // old bytesPerPixel = 4-per-texel assumption could not express.
    //
    // GetCopyableFootprints needs the destination buffer's SIZE to place the
    // layouts, so it is called twice: once with no layouts to learn the total,
    // then again with a base of 0 so Footprint.Offset is a byte offset into that
    // same buffer and can be used directly as the per-level staging offset. (The
    // old code asked for one footprint of one subresource and threw the rest
    // away, which is why there was no way to express a chain at all.)
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[native::kTextureMaxMips] = {};
    UINT64 totalBytes = 0;
    const UINT numFootprints = desc.generateMipChain ? 1u : mipCount;
    {
        UINT rows = 0;
        UINT64 rowSize = 0;
        m_device->GetCopyableFootprints(&texDesc, 0, numFootprints, 0, nullptr, nullptr,
                                        &rowSize, &totalBytes);
    }
    if (totalBytes == 0) {
        releaseUnusedPlaced();
        ++g_textureImportRefusalOther;
        MCLA_LOG_ERROR("D3D12Backend: texture import refused: GetCopyableFootprints returned 0 "
                       "bytes for {}x{} mips={}",
                       desc.width, desc.height, numFootprints);
        // 2026-10-02: second `m_inFrame` leak, same shape as the draw body's.
        // BeginFrame() was called at :1522 and set the latch; this return is
        // before anything is presented, so the frame is dropped and released.
        // Without this the texture-import refusal wedges every later frame.
        AbandonFrame("texture-import:zero-footprint");
        return false;
    }
    {
        UINT rows = 0;
        UINT64 rowSize = 0;
        m_device->GetCopyableFootprints(&texDesc, 0, numFootprints, 0, footprints, &rows,
                                        &rowSize, &totalBytes);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    ID3D12Resource* uploadRes = nullptr;
    uint8_t* uploadCpuBase = nullptr;
    uint64_t uploadSliceOffset = 0;
    if (!s_noPlacedTex && AllocateTexUploadSlice(totalBytes, uploadRes, uploadCpuBase, uploadSliceOffset)) {
        // Reused pooled upload staging slice; no bespoke CreateCommittedResource / Map / Unmap.
    } else {
        D3D12_HEAP_PROPERTIES upProps = {};
        upProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        upProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        upProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = totalBytes;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = m_device->CreateCommittedResource(&upProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&staging));
        if (FAILED(hr)) {
            releaseUnusedPlaced();
            ++g_textureImportRefusalOther;
            MCLA_LOG_ERROR("D3D12Backend: texture import refused: staging ({}B) failed hr=0x{:08X}",
                           static_cast<uint32_t>(totalBytes), static_cast<uint32_t>(hr));
            return false;
        }

        void* mapped = nullptr;
        hr = staging->Map(0, nullptr, &mapped);
        if (FAILED(hr) || !mapped) {
            releaseUnusedPlaced();
            ++g_textureImportRefusalOther;
            MCLA_LOG_ERROR("D3D12Backend: texture import refused: staging Map failed hr=0x{:08X}",
                           static_cast<uint32_t>(hr));
            return false;
        }
        uploadRes = staging.Get();
        uploadCpuBase = static_cast<uint8_t*>(mapped);
        uploadSliceOffset = 0;
    }

    uint64_t copyLevels = 0;
    if (!desc.generateMipChain) {
        for (uint32_t i = 0; i < mipCount; ++i) {
            const uint32_t mipW = std::max<uint32_t>(1, desc.width >> i);
            const uint32_t mipH = std::max<uint32_t>(1, desc.height >> i);
            const uint32_t blocksX = BlocksFor(mipW, fp->blockWidth);
            const uint32_t blocksY = BlocksFor(mipH, fp->blockHeight);
            const uint64_t srcPitch =
                static_cast<uint64_t>(blocksX) * fp->bytesPerBlock;
            const uint8_t* src = desc.mipPayloads[i];
            uint32_t srcBytes = desc.mipBytes[i];
            uint64_t srcRowStride = srcPitch;
            if (i == 0 && level0 != desc.mipPayloads[0]) {
                src = level0;
                srcBytes = level0Bytes;
            }
            if (i == 0 && plan.mode == native::TextureUploadMode::CpuExpand && plan.blockWidth > 1u) {
                const uint64_t expandedW =
                    static_cast<uint64_t>(BlocksFor(mipW, plan.blockWidth)) * plan.blockWidth;
                const uint64_t expandedStride = expandedW * fp->bytesPerBlock;
                if (expandedStride > srcPitch) srcRowStride = expandedStride;
            } else if (mipExpandedInScratch[i]) {
                src = m_importMipScratch.data() + mipExpandedOff[i];
                srcBytes = mipExpandedBytes[i];
            }
            // A short payload is refused, not clipped: copying only the rows
            // that happen to be present would upload a half-texture and leave
            // the rest as whatever the heap contained.
            const uint64_t need = (blocksY > 0 && srcRowStride > srcPitch)
                ? ((static_cast<uint64_t>(blocksY - 1u) * srcRowStride) + srcPitch)
                : (srcPitch * blocksY);
            if (srcBytes < need) {
                if (staging) staging->Unmap(0, nullptr);
                releaseUnusedPlaced();
                ++g_textureImportRefusalOther;
                ++g_textureImportBackendRefusals[static_cast<uint32_t>(
                    TextureBackendRefusal::ShortPayload)];
                MCLA_LOG_ERROR("D3D12Backend: texture import refused: mip {} payload {}B < "
                               "{} rows x {}B pitch = {}B needed",
                               i, srcBytes, blocksY, srcPitch, need);
                return false;
            }
            uint8_t* dst = uploadCpuBase + footprints[i].Offset;
            const UINT64 dstPitch = footprints[i].Footprint.RowPitch;
            // TEMP DIAGNOSTIC (F-900): is the payload actually non-zero? If the
            // full-screen quad samples a texture of zeros, oC0 = 0*coord = 0 and
            // the draws paint BLACK over the (navy) clear -- indistinguishable
            // from "clear never executed" in the readback. Count nonzero dwords
            // of the source bytes this upload is about to stage.
            {
                uint32_t nonzero = 0, dwords = srcBytes / 4;
                const uint32_t* sp = reinterpret_cast<const uint32_t*>(src);
                for (uint32_t w = 0; w < dwords; ++w) if (sp[w] != 0) ++nonzero;
                static uint32_t s_payloadDiag = 0;
                const uint32_t pd = ++s_payloadDiag;
                if (pd <= 6 || (pd % 200) == 0) {
                    MCLA_LOG_INFO("PAYLOAD-CONTENT #{} {}x{} fmt={} bytes={} "
                                  "nonzeroDwords={}/{} first32={:08X} {:08X} "
                                  "{:08X} {:08X}",
                                  pd, desc.width, desc.height,
                                  static_cast<uint32_t>(format), srcBytes,
                                  nonzero, dwords,
                                  dwords > 0 ? sp[0] : 0u,
                                  dwords > 1 ? sp[1] : 0u,
                                  dwords > 2 ? sp[2] : 0u,
                                  dwords > 3 ? sp[3] : 0u);
                    // F-900: row map of non-zero texels (which rows have content?).
                    // Quad UVs cluster at v 0.03-0.16 (rows ~8-41 of 256); if the
                    // content lives elsewhere the UVs are systematically wrong.
                    if (desc.width == 256 && desc.height == 256 && dwords == 65536) {
                        char bands[9] = {};
                        for (int b = 0; b < 8; ++b) {
                            uint32_t bnz = 0;
                            for (uint32_t r = b * 32; r < (uint32_t)(b + 1) * 32; ++r) {
                                for (uint32_t x = 0; x < 256; ++x) {
                                    if (sp[r * 256 + x] != 0) { bnz = 1; break; }
                                }
                                if (bnz) break;
                            }
                            bands[b] = bnz ? 'X' : '.';
                        }
                        bands[8] = 0;
                        MCLA_LOG_INFO("PAYLOAD-ROWMAP #{} 256x256 bands32={} "
                                      "(each char = 32 rows, X=has-nonzero)",
                                      pd, bands);
                        // TEMP F-900 GRID16 (remove with ROWMAP after UV/content
                        // overlap is settled): 16x16 cells, X=cell has nonzero.
                        // Distinguishes "top rows filled" from "small rect filled".
                        for (int gy = 0; gy < 16; ++gy) {
                            char row[17] = {};
                            for (int gx = 0; gx < 16; ++gx) {
                                uint32_t hit = 0;
                                for (uint32_t r = gy * 16; r < (uint32_t)(gy + 1) * 16 && !hit; ++r) {
                                    for (uint32_t x = gx * 16; x < (uint32_t)(gx + 1) * 16; ++x) {
                                        if (sp[r * 256 + x] != 0) { hit = 1; break; }
                                    }
                                }
                                row[gx] = hit ? 'X' : '.';
                            }
                            row[16] = 0;
                            MCLA_LOG_INFO("PAYLOAD-GRID #{} gy={} {}", pd, gy, row);
                        }
                    }
                }
            }
            for (uint32_t y = 0; y < blocksY; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * dstPitch,
                            src + static_cast<size_t>(y) * srcRowStride,
                            static_cast<size_t>(srcPitch));
            }
            // D3D12_PLACED_SUBRESOURCE_FOOTPRINT has no TotalBytes field
            // (d3d12.h: Offset + D3D12_SUBRESOURCE_FOOTPRINT{Format,Width,Height,
            // Depth,RowPitch}), so the level's byte extent is RowPitch * Height.
            // For a 2D texture that is exactly the block-row pitch times the
            // block-row count, which is what blocksY is.
            ++copyLevels;
        }
    }
    if (staging) staging->Unmap(0, nullptr);

    // --- Copies, with a PER-SUBRESOURCE transition on the way in.
    //
    // F-641: the old code emitted one COPY_DEST->PIXEL_SHADER_RESOURCE barrier
    // on ALL_SUBRESOURCES after a single mip-0 copy, which is only correct for a
    // one-mip texture. With a real chain each level is transitioned on its own,
    // from the state the tracker holds for that subresource (COMMON for a fresh
    // resource, which is what CreateCommittedResource actually left it in).
    for (uint32_t i = 0; i < copyLevels; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = texture.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = i;
        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = uploadRes;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprints[i];
        srcLoc.PlacedFootprint.Offset += uploadSliceOffset;
        cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }

    TEMP_RMCHK("import-post-copy");
    if (copyLevels == mipCount && mipCount > 0) {
        for (uint32_t i = 0; i < mipCount; ++i) {
            TransitionResource(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, i, cl);
        }
    }

    // --- The generated chain. GenerateMipmaps needs a frame, the GPU fence and
    // its own descriptor heap, and it returns false if the UAV flag is absent --
    // so a failure here is reported and the texture is left with an uploaded
    // mip 0 rather than being presented as if the whole chain existed.
    if (desc.generateMipChain && allowMipgen) {
        if (!GenerateMipmaps(texture.Get(), mipCount)) {
            RetireTarget(std::move(texture));
            if (placedHeapIndex != UINT32_MAX) {
                m_retiredPlacedRanges.push_back({ UINT64_MAX, placedHeapIndex,
                                                  placedHeapOffset, placedHeapSize });
            }
            if (staging) RetireTarget(std::move(staging));
            ++g_textureImportRefusalOther;
            ++g_textureImportBackendRefusals[static_cast<uint32_t>(
                TextureBackendRefusal::MipgenFailed)];
            MCLA_LOG_ERROR("D3D12Backend: texture import: mipgen failed for {}x{} chain of {} "
                           "levels; mip 0 is resident, the rest are undefined",
                           desc.width, desc.height, mipCount);
            return false;
        }
    }

    // --- SRV at the requested slot.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = mipCount;
    srvDesc.Texture2D.MostDetailedMip = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    srvHandle.ptr += static_cast<size_t>(desc.srvSlot) * m_srvDescriptorSize;
    TEMP_RMCHK("import-pre-srv");
    ResetStaticSrvDedupe();  // publishes into any static slot (kTexture2DSlot = 1 and up)
    m_device->CreateShaderResourceView(texture.Get(), &srvDesc, srvHandle);
    TEMP_RMCHK("import-post-srv");
    m_decodedTextureSrvGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    m_decodedTextureSrvGpu.ptr += static_cast<size_t>(desc.srvSlot) * m_srvDescriptorSize;

    // --- Commit. The copy is enqueued into the current frame's command list
    // above, so the staging buffer must outlive the draw; the previous staging is
    // replaced only now that everything succeeded.
    if (desc.outHeapIndex) *desc.outHeapIndex = placedHeapIndex;
    if (desc.outHeapOffset) *desc.outHeapOffset = placedHeapOffset;
    if (desc.outHeapSize) *desc.outHeapSize = placedHeapSize;
    if (desc.outTexture) {
        *desc.outTexture = texture;
        if (staging) RetireTarget(std::move(staging));
    } else {
        RetireTarget(std::move(m_decodedTexture));
        if (m_decodedTextureHeapIndex != UINT32_MAX) {
            m_retiredPlacedRanges.push_back({ UINT64_MAX, m_decodedTextureHeapIndex,
                                              m_decodedTextureHeapOffset, m_decodedTextureHeapSize });
        }
        RetireTarget(std::move(m_decodedTextureUpload));
        m_decodedTexture = texture;
        m_decodedTextureHeapIndex = placedHeapIndex;
        m_decodedTextureHeapOffset = placedHeapOffset;
        m_decodedTextureHeapSize = placedHeapSize;
        m_decodedTextureUpload = staging;
    }

    if (desc.outSrvGpu) *desc.outSrvGpu = m_decodedTextureSrvGpu;

    // F-649: the success counter, on the ONE success exit. Measured, not
    // assumed: `CreateDecodedTexture(const TextureImportDesc&)` contains
    // exactly one `return true` statement -- the one below -- and 18
    // `return false`. Every other path (Gate 1, Gate 2, the five Gate-3
    // checks, the short-payload check and the mipgen failure) refuses.
    // Counted unconditionally; the house bound gates the text only.
    //
    // NOTE (F-649, checked against renderer/texture_decode.h:513-515 rather than
    // assumed): `NoteTextureImport` is deliberately NOT called here. It is
    // already invoked from PlanTextureImport's single success exit
    // (renderer/texture_decode.cpp:1059), and its own header states that a
    // consumer must not call it directly or the census double-counts. The brief's
    // instruction to "call NoteTextureImport(plan) on the accepted path" is
    // therefore already satisfied upstream; calling it here would inflate
    // TextureImportAcceptedTotal by every backend-side import. The names in the
    // brief are also stale: the accessors are TextureImportAcceptedCount /
    // TextureImportAcceptedTotal / TextureImportLastMode
    // (renderer/texture_decode.h:527-532), not TextureImportCountAccepted /
    // TextureImportCountLastMode.
    const uint64_t n = ++g_textureImportOk;
    if (n <= kRefuseLogFirst || (n % kRefuseLogEvery) == 0) {
        MCLA_LOG_INFO("TEX-IMPORT #{} fmt={} {} dxgi={} mode={} {}x{} mips={} blockB={}",
            n, plan.xenosFormat, plan.formatName, static_cast<uint32_t>(format),
            (uint32_t)plan.mode, desc.width, desc.height, plan.mipCount, plan.blockBytes);
    }

    static std::atomic<uint64_t> s_texImpLog{0};  // MD-32
    if (const uint64_t ti = ++s_texImpLog; ti <= 200 || (ti % 100) == 0)
    MCLA_LOG_INFO("D3D12Backend: texture imported {}x{} mips={} fmt={} ({}B/block) slot=t{}",
                  desc.width, desc.height, mipCount, static_cast<uint32_t>(format),
                  fp->bytesPerBlock, desc.srvSlot);
    return true;
}

// ============================================================================
// F-639: DrawPacket::textureDescriptors[] -> resident textures -> SRVs
// ============================================================================

// Resident-pool handle <-> ResidentTexture. handle is (pool index + 1) so 0 can
// mean "no resource". Kept out of line in the header comment: the pool's only
// invariant is that a slot's ComPtrs outlive every command list that could still
// read them, which is why eviction (not a frame tick) is what frees a slot.
uint32_t D3D12Backend::AcquireTexturePoolSlot() {
    if (!m_texturePoolFree.empty()) {
        const uint32_t idx = m_texturePoolFree.back();
        m_texturePoolFree.pop_back();
        m_texturePoolByHandle[idx] = &m_texturePoolSlots[idx];
        // TEMP-MEMO-GEN: recycled index -> new occupant -> bump the generation so a memo
        // recorded against the previous occupant can be detected and rejected.
        ++m_texturePoolSlots[idx].gen;
        return idx + 1;
    }
    m_texturePoolSlots.emplace_back();
    ++m_texturePoolSlots.back().gen;
    m_texturePoolByHandle.push_back(&m_texturePoolSlots.back());
    // F-900 (2026-10-28): emplace_back may REALLOCATE m_texturePoolSlots,
    // dangling every ResidentTexture* stored in m_texturePoolByHandle by
    // earlier calls. MEASURED: BIND-S stage trace (build/g998.log) shows the
    // last row before the AV is pass=3007 st=hit-srv-in -- the HIT path's
    // CreateShaderResourceView dereferences
    // m_texturePoolByHandle[hit.handle-1]->texture, i.e. a pointer written
    // before a later pool growth moved the slots vector. g994 read 0x8F900,
    // g996 read 0x7FF700000010, g997 executed heap: three signatures, one
    // use-after-realloc. Rebuild the table after any growth (released slots
    // stay null). handle-1 == slot index always (slots are never removed,
    // only reset in place), so this is exact, not a heuristic.
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_texturePoolSlots.size()); ++i) {
        if (m_texturePoolByHandle[i] != nullptr)
            m_texturePoolByHandle[i] = &m_texturePoolSlots[i];
    }
    return static_cast<uint32_t>(m_texturePoolSlots.size());
}

static void InvalidateFrameTexturesForHandle(uint32_t handle);

void D3D12Backend::ReleaseTexturePoolSlot(uint64_t handle) {
    if (handle == 0 || handle > m_texturePoolByHandle.size()) return;
    const uint32_t idx = static_cast<uint32_t>(handle - 1);
    m_texturePoolByHandle[idx] = nullptr;
    static const bool s_noFrameMemoInvalidateFix = [] {
        const char* e = std::getenv("MCLA_NO_FRAMEMEMO_INVALIDATE_FIX");
        return e && e[0] == '1';
    }();
    if (!s_noFrameMemoInvalidateFix) {
        InvalidateFrameTexturesForHandle(static_cast<uint32_t>(handle));
        ++m_texturePoolSlots[idx].gen;
    }
    if (!m_texturePoolSlots[idx].stagingSrvs.empty()) {
        for (const auto& s : m_texturePoolSlots[idx].stagingSrvs) {
            ReleaseStagingSrvSlot(s.slot);
        }
        m_texturePoolSlots[idx].stagingSrvs.clear();
        ++m_srvTableEpoch;
    }
    // Evicted mid-frame: the open list may already sample it, so defer the release.
    RetireTarget(std::move(m_texturePoolSlots[idx].texture));
    if (m_texturePoolSlots[idx].heapIndex != UINT32_MAX) {
        m_retiredPlacedRanges.push_back({ UINT64_MAX,
                                          m_texturePoolSlots[idx].heapIndex,
                                          m_texturePoolSlots[idx].heapOffset,
                                          m_texturePoolSlots[idx].heapSize });
        m_texturePoolSlots[idx].heapIndex = UINT32_MAX;
        m_texturePoolSlots[idx].heapOffset = 0;
        m_texturePoolSlots[idx].heapSize = 0;
    }
    RetireTarget(std::move(m_texturePoolSlots[idx].upload));
    m_texturePoolFree.push_back(idx);
}

static uint64_t s_loggedTextureHashes[32] = {};
static uint32_t s_loggedTextureHashCount = 0;

// Render thread only. O(1) direct-mapped epoch table so high-draw 3D scenes
// with hundreds of resident textures skip Plan/Find/Touch/Expand per draw.
struct FrameTextureMemo {
    uint64_t epoch = 0;
    uint8_t descriptorBytes[24];
    uint32_t handle;
    uint32_t dxgiFormat;
    uint32_t mipCount = 1;
    uint64_t gen = 0;        // TEMP-MEMO-GEN: generation of the pool occupant at record time
    uint64_t contentId = 0;
};
static constexpr uint32_t kFrameTextureMemoSlots = 1024;
static FrameTextureMemo s_frameTextures[kFrameTextureMemoSlots];
static uint64_t s_frameTextureEpoch = 1;

static inline uint32_t HashFrameTexDesc(const uint8_t* descriptorBytes) {
    uint64_t w0, w1, w2;
    std::memcpy(&w0, descriptorBytes + 0, 8);
    std::memcpy(&w1, descriptorBytes + 8, 8);
    std::memcpy(&w2, descriptorBytes + 16, 8);
    uint64_t h = 14695981039346656037ull;
    h ^= w0; h *= 1099511628211ull;
    h ^= w1; h *= 1099511628211ull;
    h ^= w2; h *= 1099511628211ull;
    return static_cast<uint32_t>((h ^ (h >> 32)) & (kFrameTextureMemoSlots - 1u));
}

static const FrameTextureMemo* FindFrameTexture(const uint8_t* descriptorBytes, uint64_t contentId = 0,
                                                uint32_t mipCount = 0, bool hasPayload = false) {
    if (contentId == 0 && hasPayload) return nullptr;
    const FrameTextureMemo& m = s_frameTextures[HashFrameTexDesc(descriptorBytes)];
    if (m.epoch != s_frameTextureEpoch) return nullptr;
    if (std::memcmp(m.descriptorBytes, descriptorBytes, 24) != 0) return nullptr;
    if (contentId != 0 && m.contentId != contentId) return nullptr;
    if (mipCount != 0 && m.mipCount != mipCount) return nullptr;
    return &m;
}

static void RecordFrameTexture(const uint8_t* descriptorBytes, uint32_t handle,
                               uint32_t dxgiFormat, uint64_t gen, uint64_t contentId,
                               uint32_t mipCount) {
    FrameTextureMemo& m = s_frameTextures[HashFrameTexDesc(descriptorBytes)];
    m.epoch = s_frameTextureEpoch;
    std::memcpy(m.descriptorBytes, descriptorBytes, 24);
    m.handle = handle;
    m.dxgiFormat = dxgiFormat;
    m.mipCount = mipCount;
    m.gen = gen;
    m.contentId = contentId;
}

static void InvalidateFrameTexturesForHandle(uint32_t handle) {
    if (handle == 0) return;
    for (uint32_t i = 0; i < kFrameTextureMemoSlots; ++i) {
        if (s_frameTextures[i].handle == handle) {
            s_frameTextures[i].epoch = 0;
            s_frameTextures[i].handle = 0;
            s_frameTextures[i].gen = 0;
            std::memset(s_frameTextures[i].descriptorBytes, 0, sizeof(s_frameTextures[i].descriptorBytes));
        }
    }
}

bool D3D12Backend::HasFrameTexture(const uint8_t* descriptorBytes) {
    return FindFrameTexture(descriptorBytes) != nullptr;
}

void D3D12Backend::ClearFrameTextures() {
    ++s_frameTextureEpoch;
    if (s_frameTextureEpoch == 0) {
        for (auto& m : s_frameTextures) m.epoch = 0;
        s_frameTextureEpoch = 1;
    }
}

// PERF 2026-11-15 REVERTED (perf2.log / perf3.log A/B): the create-once SRV
// view cache was a net LOSS and has been removed. Measured 150 s city runs,
// same harness: perf1 7.70 fps / texbind 8.68 us per draw; perf2 (cache +
// CopyDescriptorsSimple) 6.31 fps / 15.8 us; perf3 (cache + memcpy of the
// 32-byte descriptor) 5.86 fps / 22.9 us. The map lookup -- an unordered_map
// whose key carries a ComPtr and a 24-byte desc, 12 times per draw -- cost
// more than the CreateShaderResourceView it replaced, and memcpy did not
// recover it. Do not re-add without a counter that proves the lookup is
// cheaper than the create on this machine.
uint32_t D3D12Backend::ResolveGuestTexturesToStaging(
    const GuestTextureEntry* entries, uint32_t count,
    ID3D12GraphicsCommandList* uploadCl, bool allowMipgen,
    uint32_t (&outStagedSlots)[kShaderTextureSlotCount],
    uint64_t& outTableHash, std::vector<FallbackSrv>* outFallbackSrvs) {
    for (uint32_t s = 0; s < kShaderTextureSlotCount; ++s) outStagedSlots[s] = 0;
    outTableHash = 0;
    if (outFallbackSrvs) outFallbackSrvs->clear();
    if (!m_srvHeap || !m_device) return 0;
    if (count > kMaxGuestTextureDescriptors) count = kMaxGuestTextureDescriptors;
    if (count == 0 || entries == nullptr) return 0;

    uint32_t bound = 0;
    ++m_textureBindPasses;
    const bool capturePixProbe = PixProbeEnabled() && (outFallbackSrvs != &m_stage2FallbackSrvs);
    if (capturePixProbe) {
        s_pixprobeTexCount = 0;
        s_pixprobeCurSamplesSceneColor = false;
    }
    uint32_t (&stagedSlots)[kShaderTextureSlotCount] = outStagedSlots;
    std::vector<FallbackSrv> localFallbackSrvs;
    std::vector<FallbackSrv>& fallbackSrvs = outFallbackSrvs ? *outFallbackSrvs : localFallbackSrvs;

    // F-900 CRASH-FORENSICS (2026-10-28): render thread AVs inside this
    // function (g994: C0000005 reading 0x8F900, rsi=r8=0x8F900, rdi=0) after
    // ~3258 binds. Log the entry pointer/count plus the first entry's raw
    // fields so the next soak names the wild value instead of guessing.
    if (m_textureBindPasses <= 4 || (m_textureBindPasses % 500) == 0) {
        // NOTE: entries[0] is deliberately NOT read here: if the caller
        // handed us a wild pointer, the read below must happen inside the
        // loop's named refusal path, not inside a diagnostic.
        MCLA_LOG_INFO("BIND-PHASE #{} entries={} count={}",
                      m_textureBindPasses, static_cast<const void*>(entries), count);
    }

    for (uint32_t e = 0; e < count; ++e) {
        const GuestTextureEntry& entry = entries[e];
        // Fetch-constant swizzle (dword 3 bits 1..12, 3 bits per component: 0..3 = X..W, 4 = 0, 5 = 1;
        // the same encoding as D3D12's component mapping). Applied only where the host format keeps the
        // guest XYZW order (RGBA8, BC1-3); measured: the START glyph's alpha is forced to 1 by it.
        // Xenos gamma fetch (d0 sign bits 2..7 == 3 for x,y,z): view the TYPELESS resource as _SRGB.
        // shortcut: sRGB approximates the Xenos PWL curve (max 9/255, AUDIT_GAMMA_SRGB.md); exact PWL needs the shader.
        const bool gammaView = [&] {
            const uint8_t* b = entry.descriptorBytes;
            const uint32_t d0 = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
            return ((d0 >> 2) & 63u) == 63u;
        }();
        // Xenos sign per channel, same 2-bit fields as the gamma test above
        // (x at bits 2..3, y at 4..5, z at 6..7; GuestTextureSign 1 = Signed).
        // 21 = 0b010101 = x,y,z all Signed. Only formats that HAVE an _SNORM
        // view are converted; everything else keeps its UNORM view.
        const bool signedView = [&] {
            const uint8_t* b = entry.descriptorBytes;
            const uint32_t d0 = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
            return ((d0 >> 2) & 63u) == 21u;
        }();
        // Typed view for a guest texture: TYPELESS resources need an explicit UNORM/_SRGB format.
        const auto viewFormat = [&](uint32_t dxgi) -> DXGI_FORMAT {
            switch (dxgi) {
                case 27u: case 28u: case 29u:
                    return gammaView ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                     : (signedView ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
                case 70u: case 71u: case 72u: return gammaView ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
                case 73u: case 74u: case 75u: return gammaView ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM;
                case 76u: case 77u: case 78u: return gammaView ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
                default: return static_cast<DXGI_FORMAT>(dxgi);
            }
        };
        // components 6/7 map to 0/1 as Xenia does (texture_cache.cc:270-273, v & 0b101).
        // also apply the swizzle to R8 (61), R8G8 (49) and BC4 (80) with Xenia's host
        // composition (texture_cache.cc:264-281): R8 host swizzle is RRRR, R8G8 is RGGG.
        // Both are opt-in until a boot proves them; unset = the old path byte-for-byte.
        // guestMapping defined below, after gtd decode. (No texture-side R/B swap: k_8_8_8_8 carries .zyxw in guest swizzle and BC1-3 decode as RGBA.)
        const uint32_t idx = entry.descriptorIndex;
        const uint32_t slot = idx;
        // F-900 CRASH-FORENSICS: per-entry values at the same cadence as
        // BIND-PHASE. entries[] is proven readable (BIND-PHASE fired 1000+
        // times on a stable pointer in g996); this names a wild idx/slot or
        // payload pointer BEFORE the D3D12 call that AVs on it.
        //
        // BIND-ODD fires EVERY pass whose entry is out of contract (no
        // cadence gate): a wild idx/slot means CreateShaderResourceView gets
        // a wild DestDescriptor; a wild payload means the hash/expand reads
        // garbage. Silence here + a crash means the entry is sane and the
        // fault is in the upload/SRV path, not the data.
        {
            const bool oddIdx = (idx >= kShaderTextureSlotCount);
            const bool oddPtr = (entry.mipPayloads[0] != nullptr &&
                                 (entry.mipPayloads[0] < reinterpret_cast<const uint8_t*>(0x100000u) ||
                                  entry.mipBytes[0] == 0 || entry.mipBytes[0] > 16777216u));
            // descriptorBytes are big-endian: dword_0 = bytes[0..3] with the
            // type field (bits 1:0) in the LOW bits = bytes[3], not bytes[0].
            const bool oddType = ((entry.descriptorBytes[3] & 3u) != 2u);
            if (oddIdx || oddPtr || oddType) {
                MCLA_LOG_ERROR("BIND-ODD pass={} e={} idx={} slot={} pay={} bytes={} mips={} "
                               "d0={:02X}{:02X}{:02X}{:02X} oddIdx={} oddPtr={} oddType={}",
                               m_textureBindPasses, e, idx, slot,
                               static_cast<const void*>(entry.mipPayloads[0]),
                               entry.mipBytes[0], entry.mipCount,
                               entry.descriptorBytes[0], entry.descriptorBytes[1],
                               entry.descriptorBytes[2], entry.descriptorBytes[3],
                               oddIdx ? 1 : 0, oddPtr ? 1 : 0, oddType ? 1 : 0);
            }
        }
        static std::atomic<uint64_t> s_bindEntryLog{0};  // MD-32: one pass logs every entry; cap rows
        if ((m_textureBindPasses <= 4 || (m_textureBindPasses % 500) == 0) && ShouldLogRefusal(++s_bindEntryLog)) {
            MCLA_LOG_INFO("BIND-ENTRY pass={} e={} idx={} slot={} pay={} bytes={} mips={} d0={:02X}{:02X}{:02X}{:02X}",
                          m_textureBindPasses, e, idx, slot,
                          static_cast<const void*>(entry.mipPayloads[0]),
                          entry.mipBytes[0], entry.mipCount,
                          entry.descriptorBytes[0], entry.descriptorBytes[1],
                          entry.descriptorBytes[2], entry.descriptorBytes[3]);
        }
        // Every index that this pass does not end up giving a real texture gets
        // a null SRV, so a refused or out-of-range descriptor samples a defined
        // colour instead of whatever the previous pass left in the heap.
        auto writeNullSrv = [&]() {
            if (idx < kShaderTextureSlotCount) {
                stagedSlots[idx] = 0;  // staging slot 0 is the shared Null SRV
            }
        };

        if (idx >= kShaderTextureSlotCount) {
            // F-639: the heap has the slot, but the ROOT SIGNATURE binds only
            // t0..t3, so a descriptor at t4+ cannot be sampled by any shader the
            // translator produces. Writing a real texture here would look
            // successful and sample nothing. Counted and named; when the table
            // grows to 16 this gate moves with it.
            const uint64_t n = ++g_textureBindRefusals;
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_ERROR("D3D12Backend: t{} refused #{}: root signature binds t0..t{} only "
                               "(heap holds {} slots); NULL SRV at slot {}",
                               idx, n, kShaderTextureSlotCount - 1, m_srvHeapDescriptorCount, slot);
            }
            writeNullSrv();
            continue;
        }

        // --- Decode the 24-byte fetch-constant snapshot. Pure function of those
        // bytes (explicit big-endian shifts inside), so this is NOT a guest read.
        native::GuestTextureDesc gtd = {};
        if (!native::DecodeGuestTextureDescriptor(entry.descriptorBytes, gtd)) {
            const uint64_t n = ++g_textureBindRefusals;
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_ERROR("D3D12Backend: t{} descriptor refused #{}: {} -- NULL SRV at slot {}",
                               idx, n, gtd.refusal ? gtd.refusal : "decode-failed", slot);
            }
            writeNullSrv();
            continue;
        }

        const auto guestMapping = [&](uint32_t dxgi) -> UINT {
            const bool rgbaOrBc = dxgi == 28u || dxgi == 29u || dxgi == 71u || dxgi == 72u || dxgi == 74u || dxgi == 75u || dxgi == 77u || dxgi == 78u;
            const bool hostSwz = (dxgi == 61u || dxgi == 49u || dxgi == 80u);
            if (!rgbaOrBc && !hostSwz) return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            const uint8_t* b = entry.descriptorBytes;
            const uint32_t d3 = (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16) | (uint32_t(b[14]) << 8) | b[15];
            const uint32_t swz = (d3 >> 1) & 0xFFFu;
            uint32_t c[4];
            for (uint32_t i = 0; i < 4; ++i) {
                const uint32_t v = (swz >> (3 * i)) & 7u;
                if (v >= 4u) c[i] = v & 5u;      // 4/5 stay, 6 -> 0, 7 -> 1
                else c[i] = v <= 5u ? v : i;                               // original behaviour
                if (hostSwz && v < 4u) c[i] = (dxgi == 49u && v != 0u) ? 1u : 0u;  // host RRRR / RGGG
            }
            return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(c[0], c[1], c[2], c[3]);
        };
        // --- Plan. `tiled`/`packedMips`/dimension are all already in gtd.
        {   // RT-CACHE phase 3: a texture whose base is a Resolve destination binds the
            // host texture that resolve wrote, instead of importing stale guest memory.
            const uint32_t gb = ((uint32_t(entry.descriptorBytes[4]) << 24) | (uint32_t(entry.descriptorBytes[5]) << 16) |
                                 (uint32_t(entry.descriptorBytes[6]) << 8) | uint32_t(entry.descriptorBytes[7])) & 0xFFFFF000u;
            const uint32_t fetchFmt = entry.descriptorBytes[7] & 0x3Fu;
            const bool wantsDepth = fetchFmt == 22u || fetchFmt == 23u;  // k_24_8 / k_24_8_FLOAT
            uint32_t colourKey = gb;
            // ExecuteGuestResolve keys colour resolves by the source EDRAM RT DXGI format
            // (never R8_UNORM=61); k_8 (fetchFmt=2) resolves originate from k_8_8_8_8
            // EDRAM (dxgi=28) with RB_COPY_DEST_INFO.copy_dest_format=2, so pref=28u lets
            // Priority 1/3 match them directly (even with MCLA_NO_LASTCOLOUR=1).
            const uint32_t pref = wantsDepth ? 0u : ((fetchFmt == 2u || fetchFmt == 6u) ? 28u :
                                  (fetchFmt == 26u || fetchFmt == 29u || fetchFmt == 32u) ? 10u : 0u);
            const auto isColorDxgi = [](uint32_t f) {
                return f == 10u || f == 28u || f == 24u || f == 11u;
            };
            // A1: swap any parked multi-size variant matching the requested fetch dims into the primary slot.
            const auto preferSize = [&](uint32_t key, bool wantDepth) {
                auto pi = m_resolved.find(key);
                if (pi == m_resolved.end() || pi->second.isDepth != wantDepth ||
                    (pi->second.w == gtd.width && pi->second.h == gtd.height)) return;
                auto ai = m_resolvedAlt.find(key);
                if (ai == m_resolvedAlt.end()) return;
                for (ResolvedTex& alt : ai->second) {
                    if (alt.valid && alt.res && alt.isDepth == wantDepth &&
                        alt.w == gtd.width && alt.h == gtd.height) {
                        std::swap(pi->second, alt);
                        return;
                    }
                }
            };
            bool colourMatched = false;
            const auto isFreshColour = [&](auto it) {
                return it != m_resolved.end() && it->second.valid && it->second.res && !it->second.isDepth &&
                       (m_presentedFrames - it->second.lastPresent <= MaxResolveAgeFrames());
            };
            if (!wantsDepth) {
                // Priority 1: exact preferred format key gb | (pref << 2).
                if (pref != 0u) {
                    const uint32_t pk = gb | (pref << 2);
                    preferSize(pk, false);
                    auto pm = m_resolved.find(pk);
                    const bool dimAgree = pm != m_resolved.end() &&
                        pm->second.w == gtd.width && pm->second.h == gtd.height;
                    if (isFreshColour(pm) && dimAgree) {
                        colourKey = pk;
                        colourMatched = true;
                    }
                }
                // Priority 2: m_resolvedLastColour[gb] (allows compatible colour DXGI format).
                if (!colourMatched && !NoLastColour()) {
                    auto lc = m_resolvedLastColour.find(gb);
                    if (lc != m_resolvedLastColour.end()) {
                        preferSize(lc->second, false);
                        auto it = m_resolved.find(lc->second);
                        const bool fmtAgree = (pref == 0u || (it != m_resolved.end() && static_cast<uint32_t>(it->second.fmt) == pref));
                        const bool dimAgree = it != m_resolved.end() &&
                            it->second.w == gtd.width && it->second.h == gtd.height;
                        if (isFreshColour(it) && dimAgree &&
                            (fmtAgree || isColorDxgi(pref))) {
                            colourKey = lc->second;
                            colourMatched = true;
                        }
                    }
                }
                // Priority 3: fallback other colour keys at gb (e.g. 1280x720 fmt=10 for fetchFmt=6 pref=28).
                if (!colourMatched && isColorDxgi(pref)) {
                    for (uint32_t cf : { 10u, 28u, 24u, 11u }) {
                        const uint32_t ck = gb | (cf << 2);
                        preferSize(ck, false);
                        auto cit = m_resolved.find(ck);
                        if (isFreshColour(cit) && cit->second.w == gtd.width && cit->second.h == gtd.height) {
                            colourKey = ck;
                            colourMatched = true;
                            break;
                        }
                    }
                }
                // Priority 4: when no exact-dimension resolve matched at gb (e.g. split-tile resolve or minimap/road
                // resolve whose written rect differs from the fetch descriptor), prefer binding the fresh resolved
                // colour texture at gb rather than falling through to payload==nullptr -> NULL SRV (opaque black).
                static const bool s_noP4 = [] { const char* e = std::getenv("MCLA_NO_RBIND_P4"); return e && e[0] == '1'; }();
                if (!colourMatched && !NoLastColour() && !s_noP4) {
                    auto lc = m_resolvedLastColour.find(gb);
                    if (lc != m_resolvedLastColour.end()) {
                        preferSize(lc->second, false);
                        auto it = m_resolved.find(lc->second);
                        if (isFreshColour(it)) {
                            colourKey = lc->second;
                            colourMatched = true;
                        }
                    }
                    if (!colourMatched) {
                        for (uint32_t cf : { 10u, 28u, 24u, 11u }) {
                            const uint32_t ck = gb | (cf << 2);
                            preferSize(ck, false);
                            auto cit = m_resolved.find(ck);
                            if (isFreshColour(cit)) {
                                colourKey = ck;
                                colourMatched = true;
                                break;
                            }
                        }
                    }
                    if (colourMatched) {   // RBIND-P4 census: dims/format the fetch asked for vs the resolve it got
                        static std::atomic<uint64_t> s_p4{0};
                        static std::set<uint64_t> s_p4Seen;
                        auto pit = m_resolved.find(colourKey);
                        const uint64_t pn = s_p4.fetch_add(1) + 1;
                        if (pit != m_resolved.end() && s_p4Seen.size() < 128 &&
                            s_p4Seen.insert((uint64_t(gb) << 20) ^ (uint64_t(pit->second.w) << 8) ^ fetchFmt).second)
                            MCLA_LOG_WARN("RBIND-P4 #{} base={:08X} fetchFmt={} fetch={}x{} -> resolve fmt={} {}x{} dimMismatch={} (off: MCLA_NO_RBIND_P4=1)",
                                          pn, gb, fetchFmt, gtd.width, gtd.height, uint32_t(pit->second.fmt), pit->second.w, pit->second.h,
                                          (pit->second.w != gtd.width || pit->second.h != gtd.height) ? 1 : 0);
                    }
                }
            }
            preferSize(wantsDepth ? (gb | 1u) : colourKey, wantsDepth);
            auto rit = m_resolved.find(wantsDepth ? (gb | 1u) : colourKey);
            if (!wantsDepth && !colourMatched)
                rit = m_resolved.end();
            if (rit != m_resolved.end() && !wantsDepth && m_presentedFrames - rit->second.lastPresent > MaxResolveAgeFrames())
                rit = m_resolved.end();  // stale resolve target: the guest has since put a CPU texture at this base (TEXRACE: 128x128 at age 43)
            {   // TEMP-RBIND2: unique (base, fetchFmt, hit) for fetches of a resolve destination; m_mutex is held.
                static std::set<uint64_t> s_seen;
                if (s_seen.size() < 300) {
                    const bool known = rit != m_resolved.end() || m_resolvedLastColour.count(gb) || m_resolved.count(gb);
                    const int hit = rit != m_resolved.end() && rit->second.valid && rit->second.res ? 1 : (rit != m_resolved.end() ? 2 : 0);
                    if (known && s_seen.insert((uint64_t(gb) << 16) | (fetchFmt << 4) | uint64_t(hit)).second)
                        MCLA_LOG_ERROR("TEMP-RBIND2 base={:08X} fetchFmt={} {}x{} hit={} key={:08X} hostFmt={} srvFmt={} {}x{} rt={}",
                                       gb, fetchFmt, gtd.width, gtd.height, hit, wantsDepth ? (gb | 1u) : colourKey,
                                       rit != m_resolved.end() ? uint32_t(rit->second.fmt) : 0u,
                                       rit != m_resolved.end() ? uint32_t(rit->second.srvFmt) : 0u,
                                       rit != m_resolved.end() ? rit->second.w : 0u, rit != m_resolved.end() ? rit->second.h : 0u,
                                       m_activeRt ? int(m_activeRt->base) : -1);
                }
            }
            const bool fmtOk = wantsDepth || colourMatched || pref == 0u ||
                (rit != m_resolved.end() && rit->second.fmt == static_cast<DXGI_FORMAT>(pref)) ||
                (rit != m_resolved.end() && !rit->second.isDepth &&
                 isColorDxgi(pref) && isColorDxgi(static_cast<uint32_t>(rit->second.fmt)) &&
                 rit->second.w == gtd.width && rit->second.h == gtd.height);
            const bool dimOk = wantsDepth || colourMatched || rit == m_resolved.end() ||
                (rit->second.w == gtd.width && rit->second.h == gtd.height);
            if (rit != m_resolved.end() && rit->second.valid && rit->second.res && fmtOk && dimOk) {
                D3D12_SHADER_RESOURCE_VIEW_DESC rsd = {};
                rsd.Format = rit->second.srvFmt != DXGI_FORMAT_UNKNOWN ? rit->second.srvFmt : rit->second.fmt;
                rsd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                rsd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                {   // Honour the fetch swizzle's FORCED constants (4/6 -> 0, 5/7 -> 1) on resolved textures; channel order stays
                    // identity (a resolve is already in host order), EXCEPT k_8 (fetchFmt=2): hardware k_8 resolves extract
                    // only .r (.b if copy_dest_swap=1), so non-constant swizzle nibbles replicate that scalar channel.
                    static const bool s_off = [] { const char* e = std::getenv("MCLA_NO_RESOLVED_SWZ"); return e && e[0] == '1'; }();
                    const uint8_t* b = entry.descriptorBytes;
                    const uint32_t d3 = (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16) | (uint32_t(b[14]) << 8) | b[15];
                    const uint32_t swz = (d3 >> 1) & 0xFFFu;
                    // MD-40: a depth resolve (fetchFmt 22 k_24_8 / 23 k_24_8_FLOAT) is R32_FLOAT here = (d,0,0,1); Xenia binds it with host
                    // swizzle RRRR (d3d12_texture_cache.cc:202-208) so every guest channel reads d. MCLA_NO_DEPTH_RRRR=1 = identity.
                    static const bool s_noRrrr = [] { const char* e = std::getenv("MCLA_NO_DEPTH_RRRR"); return e && e[0] == '1'; }();
                    const bool k8Scalar = (fetchFmt == 2u) || (wantsDepth && !s_noRrrr);
                    const uint32_t k8Src = (!wantsDepth && rit->second.destSwap) ? 2u : 0u;
                    uint32_t c[4]; bool forced = false;
                    for (uint32_t i = 0; i < 4; ++i) {
                        const uint32_t v = (swz >> (3 * i)) & 7u;
                        c[i] = v >= 4u ? (v & 5u) : (k8Scalar ? k8Src : i);
                        forced |= (v >= 4u) || k8Scalar;
                    }
                    if (!s_off && forced) rsd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(c[0], c[1], c[2], c[3]);
                    // SHADOW-CONST (experiment, MCLA_DEPTH_SRV_CONST=0|1[,<hexbase>]): every depth-resolve fetch (or only <hexbase>) reads
                    // a constant 0 or 1 in all channels. Tells whether road darkness comes from depth/atlas sampling at all.
                    static const int s_dc = [] { const char* e = std::getenv("MCLA_DEPTH_SRV_CONST"); return (e && (e[0] == '0' || e[0] == '1')) ? e[0] - '0' : -1; }();
                    static const uint32_t s_dcBase = [] { const char* e = std::getenv("MCLA_DEPTH_SRV_CONST"); const char* p = e ? std::strchr(e, ',') : nullptr; return p ? static_cast<uint32_t>(std::strtoul(p + 1, nullptr, 16)) & 0x1FFFFFFFu : 0u; }();
                    if (s_dc >= 0 && (wantsDepth || s_dcBase != 0u) && (s_dcBase == 0u || (gb & 0x1FFFFFFFu) == s_dcBase)) {
                        const UINT k = s_dc ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
                        rsd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(k, k, k, k);
                        static std::atomic<uint32_t> s_dcn{0};
                        if (s_dcn.fetch_add(1) < 8) MCLA_LOG_WARN("SHADOW-CONST base={:08X} fetchFmt={} value={} swz={:03X}", gb, fetchFmt, s_dc, swz);
                    }
                }
                ResolvedTex* srvTex = &rit->second;
                ResMipChain* mipChain = nullptr;
                rsd.Texture2D.MipLevels = 1;
                if (gtd.mipMaxLevel > 0 && !wantsDepth && !ResolvedMipsDisabled()) {  // MD-09: bind the assembled mip chain when complete
                    const uint8_t* mb = entry.descriptorBytes;
                    const uint32_t md5 = (uint32_t(mb[20]) << 24) | (uint32_t(mb[21]) << 16) | (uint32_t(mb[22]) << 8) | mb[23];
                    mipChain = ResolvedMipsFind(gb, rit->second, (md5 >> 12) << 12, gtd.mipMaxLevel, allowMipgen ? uploadCl : nullptr);
                    if (mipChain) { srvTex = &mipChain->tex; rsd.Texture2D.MipLevels = mipChain->levels; }
                }
                if (srvTex->w != gtd.width || srvTex->h != gtd.height) {  // Phase 0 diagnostic; rebind policy unchanged
                    static std::atomic<uint64_t> s_dimMismatch{0};
                    static std::mutex s_dimMismatchMu;
                    static std::set<std::array<uint32_t, 5>> s_dimMismatchSeen;
                    const uint64_t n = s_dimMismatch.fetch_add(1, std::memory_order_relaxed) + 1;
                    const std::array<uint32_t, 5> mismatchKey = {
                        rit->first, gtd.width, gtd.height, srvTex->w, srvTex->h
                    };
                    std::lock_guard<std::mutex> mismatchLock(s_dimMismatchMu);
                    if (s_dimMismatchSeen.size() < 64 && s_dimMismatchSeen.insert(mismatchKey).second)
                        MCLA_LOG_WARN("RESOLVED-BIND-DIM-MISMATCH #{} key={:08X} slot={} fetch={}x{} resolved={}x{} (diagnostic; rebind unchanged)",
                                      n, rit->first, idx, gtd.width, gtd.height, srvTex->w, srvTex->h);
                }
                const uint32_t stagingSlot = GetOrCreateResolvedStagingSrv(*srvTex, rsd);
                if (stagingSlot != UINT32_MAX) {
                    stagedSlots[idx] = stagingSlot;
                } else {
                    fallbackSrvs.push_back({ idx, srvTex->res.Get(), rsd });
                }
                ++bound;
                if (capturePixProbe && !wantsDepth && rit->second.w >= 640u && rit->second.h >= 360u) {
                    s_pixprobeCurSamplesSceneColor = true;
                }
                static std::atomic<uint64_t> s_rb{0};
                const uint64_t rn = s_rb.fetch_add(1) + 1;
                if (rn <= 16 || (rn % 5000) == 0)
                    MCLA_LOG_WARN("TEMP-RESOLVED-BIND #{} base={:08X} idx={} {}x{} fmt={} resolvesSoFar={}", rn, gb, idx,
                                  rit->second.w, rit->second.h, static_cast<uint32_t>(rit->second.fmt), rit->second.resolves);
                if (gtd.mipMaxLevel > 0) {  // MD-09 census: assembled=1 once the mipped host texture is complete and bound
                    static std::atomic<uint64_t> s_rma{0};
                    const uint64_t an = s_rma.fetch_add(1) + 1;
                    if (an <= 8 || (an % 5000) == 0)
                        MCLA_LOG_WARN("RESOLVED-MIPS-ASSEMBLED #{} base={:08X} {}x{} mipMax={} packed={} assembled={} levels={} filled={:X}/{:X} (off: MCLA_NO_RESOLVED_MIPS=1)",
                                      an, gb, rit->second.w, rit->second.h, gtd.mipMaxLevel, gtd.packedMips ? 1 : 0, mipChain ? 1 : 0,
                                      mipChain ? mipChain->levels : 0u, mipChain ? mipChain->filledMask : 0u, mipChain ? mipChain->fullMask : 0u);
                }
                {   // EXPADJ-CENSUS: net exposure of a resolved texture = 2^(resolve bias + fetch-constant exp_adjust)
                    static std::set<uint64_t> s_seen;
                    if (s_seen.size() < 256) {
                        const uint8_t* db = entry.descriptorBytes;
                        int32_t fa = static_cast<int32_t>(((uint32_t(db[12]) << 24 | uint32_t(db[13]) << 16 | uint32_t(db[14]) << 8 | db[15]) >> 13) & 0x3Fu);
                        if (fa >= 32) fa -= 64;
                        if (s_seen.insert((uint64_t(gb) << 16) | (uint16_t(rit->second.expBias) << 8) | uint8_t(fa)).second)
                            MCLA_LOG_WARN("EXPADJ-CENSUS base={:08X} resolveBias={} fetchAdjust={} net={}", gb, rit->second.expBias, fa, rit->second.expBias + fa);
                    }
                }
                continue;
            }
        }
        const bool is3D = gtd.dimension == native::GuestTextureDimension::ThreeD;
        if (gtd.expAdjust != 0) {   // EXPADJ-CPU census: a non-resolved texture whose fetch asks for x 2^exp_adjust, which no host path applies
            static std::atomic<uint64_t> s_ea{0};
            const uint64_t en = s_ea.fetch_add(1, std::memory_order_relaxed) + 1;
            if (en <= 16 || (en % 20000) == 0)
                MCLA_LOG_WARN("EXPADJ-CPU #{} xenosFmt={} {}x{} expAdjust={} (not applied)", en, static_cast<uint32_t>(gtd.xenosFormat),
                              gtd.width, gtd.height, gtd.expAdjust);
        }
        {   // MD-16 census (log-only; every SRV is created as TEXTURE2D). Counts ALL dims (2D = positive control, rule 19);
            // dimension is decoded from fetch dword_5 bits 9..10 (texture_decode.cpp:1544; .research/xenia/src/xenia/gpu/xenos.h:1280).
            static std::atomic<uint64_t> s_dimN[4]{};
            static std::atomic<uint64_t> s_dimTot{0};
            const uint32_t di2 = static_cast<uint32_t>(gtd.dimension) & 3u;
            const uint64_t dn2 = s_dimN[di2].fetch_add(1, std::memory_order_relaxed) + 1;
            const uint64_t tot = s_dimTot.fetch_add(1, std::memory_order_relaxed) + 1;
            if (gtd.dimension != native::GuestTextureDimension::TwoDOrStacked && (dn2 <= 4 || (dn2 % 20000) == 0))
                MCLA_LOG_WARN("TEXDIM-CENSUS dim={} (0=1D 2=3D 3=Cube) #{} {}x{}x{} xenosFmt={} -> bound as Texture2D", di2, dn2, gtd.width, gtd.height, gtd.depthOrArraySize, static_cast<uint32_t>(gtd.xenosFormat));
            if (tot == 1 || (tot % 50000) == 0)
                MCLA_LOG_WARN("TEXDIM-TOTAL n={} dim0(1D)={} dim1(2D)={} dim2(3D)={} dim3(Cube)={}", tot,
                              s_dimN[0].load(), s_dimN[1].load(), s_dimN[2].load(), s_dimN[3].load());
        }
        const bool isDepthStencil = gtd.dimension == native::GuestTextureDimension::TwoDOrStacked &&
                                    gtd.stacked && gtd.depth > 1;
        native::TextureImportPlan plan = native::PlanTextureImport(
            gtd.xenosFormat, gtd.width, gtd.height, gtd.depthOrArraySize, gtd.mipMaxLevel,
            gtd.endianness, gtd.tiled, gtd.packedMips, is3D, isDepthStencil);
        if (!plan.ok) {
            const uint64_t n = ++g_textureImportRefusals[static_cast<uint32_t>(plan.refusal)];
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_ERROR("D3D12Backend: t{} import refused #{}: {} (xenosFmt={} {} "
                               "{}x{}) -- NULL SRV at slot {}",
                               idx, n, native::TextureRefusalName(plan.refusal), plan.xenosFormat,
                               plan.formatName, gtd.width, gtd.height, slot);
            }
            writeNullSrv();
            continue;
        }

        // P2b: the producer supplies every level (entry.mipCount); the chain is identified only by the
        // producer's content id, so a byte-hash request without one is bound as mip 0.
        uint32_t mipCount = (entry.mipCount != 0) ? entry.mipCount : plan.mipCount;
        if (mipCount > 1 && entry.contentId == 0) mipCount = 1;

        const auto bindResident = [&](uint32_t handle, uint32_t dxgiFormat) {
            ResidentTexture* const rtPtr = m_texturePoolByHandle[handle - 1];
            ID3D12Resource* const res = rtPtr->texture.Get();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
            // a contentId cache hit can hand back a texture created with another format;
            // a view that mismatches its resource removes the device (gt2/gt3: id=28 -> id=232).
            if (res) { const DXGI_FORMAT rf = res->GetDesc().Format; if (rf != srvDesc.Format && rf != DXGI_FORMAT_R24G8_TYPELESS) srvDesc.Format = rf; }
            srvDesc.Format = viewFormat(static_cast<uint32_t>(srvDesc.Format));
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = guestMapping(static_cast<uint32_t>(srvDesc.Format));
            srvDesc.Texture2D.MipLevels = res ? static_cast<UINT>(res->GetDesc().MipLevels) : 1u;
            static const bool s_mipMinFix = [] {
                const char* e = std::getenv("MCLA_NO_MIP_MINLEVEL_FIX");
                return !(e && e[0] == '1');
            }();
            if (s_mipMinFix && gtd.mipMinLevel > 0u) {
                srvDesc.Texture2D.ResourceMinLODClamp = static_cast<float>(gtd.mipMinLevel);
            }
            const uint32_t stagingSlot = GetOrCreateResidentStagingSrv(rtPtr, srvDesc);
            if (stagingSlot != UINT32_MAX) {
                stagedSlots[idx] = stagingSlot;
            } else {
                fallbackSrvs.push_back({ idx, res, srvDesc });
            }
            ++bound;
        };
        static const bool s_noFrameMemo = [] { const char* e = std::getenv("MCLA_TEMP_NO_FRAMEMEMO"); return e && e[0] == '1'; }();
        if (const FrameTextureMemo* memo = s_noFrameMemo ? nullptr : FindFrameTexture(entry.descriptorBytes, entry.contentId, mipCount, entry.mipPayloads[0] != nullptr)) {
            if (memo->handle <= m_texturePoolByHandle.size() &&
                m_texturePoolByHandle[memo->handle - 1] != nullptr &&
                m_texturePoolByHandle[memo->handle - 1]->texture) {
                // TEMP-MEMO-GEN (log-only): the key is the 24 descriptor bytes, but the value is
                // a pool INDEX with no generation. ReleaseTexturePoolSlot frees the index and
                // AcquireTexturePoolSlot can hand the SAME index to another texture, so the memo
                // can bind a different image under an identical descriptor. Generation is the
                // only discriminator.
                const bool genOk = memo->gen == m_texturePoolByHandle[memo->handle - 1]->gen;
                const bool dimsFmtOk = (m_texturePoolByHandle[memo->handle - 1]->width == gtd.width &&
                                        m_texturePoolByHandle[memo->handle - 1]->height == gtd.height &&
                                        m_texturePoolByHandle[memo->handle - 1]->xenosFormat == static_cast<uint32_t>(gtd.xenosFormat));
                static const bool s_noFrameMemoInvalidateFix = [] {
                    const char* e = std::getenv("MCLA_NO_FRAMEMEMO_INVALIDATE_FIX");
                    return e && e[0] == '1';
                }();
                const bool validMemo = genOk && (s_noFrameMemoInvalidateFix ? true : dimsFmtOk);
                {
                    static std::atomic<uint64_t> s_hit{0}, s_mism{0};
                    const uint64_t h = s_hit.fetch_add(1, std::memory_order_relaxed) + 1;
                    const uint64_t m = validMemo ? 0 : s_mism.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (h <= 32 || (h % 100000) == 0 || m)
                        MCLA_LOG_WARN("TEMP-MEMO-IDENT hit={} validMemo={} genOk={} dimsFmtOk={} handle={} memoGen={} slotGen={} want={}x{} fmt={} have={}x{} fmt={}",
                                      h, validMemo ? 1 : 0, genOk ? 1 : 0, dimsFmtOk ? 1 : 0, memo->handle, memo->gen,
                                      m_texturePoolByHandle[memo->handle - 1]->gen, gtd.width, gtd.height,
                                      static_cast<uint32_t>(gtd.xenosFormat),
                                      m_texturePoolByHandle[memo->handle - 1]->width,
                                      m_texturePoolByHandle[memo->handle - 1]->height,
                                      m_texturePoolByHandle[memo->handle - 1]->xenosFormat);
                }
                // refuse a stale memo hit and import normally
                if (!validMemo) {
                    // stale occupant or dimension/format mismatch: fall through to the normal import path
                } else {
                    bindResident(memo->handle, memo->dxgiFormat);
                    continue;
                }
            }
        }

        // --- Hash the UPLOAD payload (post-expand, not pre-expand): a cache hit
        // must be provably the same bytes the GPU would receive.
        const uint8_t* payload = entry.mipPayloads[0];
        uint32_t payloadBytes = entry.mipBytes[0];
        // Dump one decoded texture payload by guest base (MCLA_DUMP_TEX_BASE=<hex>).
        // Log-only: writes w/h/fmt + payload to texdump/<base>.bin once.
        {
            static const uint32_t s_dumpTexBase = [] {
                const char* e = std::getenv("MCLA_DUMP_TEX_BASE");
                return e && *e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
            }();
            static bool s_dumpedTex = false;
            static const int s_dumpTexSlot = [] {
                const char* e = std::getenv("MCLA_DUMP_TEX_SLOT");
                return e && *e ? std::atoi(e) : -1;
            }();
            if (payload != nullptr && payloadBytes > 0 &&
                ((s_dumpTexBase != 0 && !s_dumpedTex &&
                  (gtd.baseAddress & 0xFFFFF000u) == (s_dumpTexBase & 0xFFFFF000u)) ||
                 (s_dumpTexSlot >= 0 && static_cast<int>(idx) == s_dumpTexSlot && !s_dumpedTex))) {
                std::error_code ec; std::filesystem::create_directories("texdump", ec);
                char fn[128];
                std::snprintf(fn, sizeof fn, "texdump/%08X.bin", s_dumpTexBase);
                if (std::FILE* f = std::fopen(fn, "wb")) {
                    const uint32_t hdr[3] = { gtd.width, gtd.height, gtd.xenosFormat };
                    std::fwrite(hdr, 1, sizeof hdr, f);
                    std::fwrite(payload, 1, payloadBytes, f);
                    std::fclose(f);
                    s_dumpedTex = true;
                    MCLA_LOG_WARN("DUMP-TEX base={:08X} fmt={} {}x{} bytes={}",
                                  gtd.baseAddress, gtd.xenosFormat, gtd.width,
                                  gtd.height, payloadBytes);
                }
            }
        }
        // F-900 CRASH-GUARD: refuse wild host pointers/sizes instead of
        // hashing them. A valid payload is a heap vector (>>16 MiB on Win64);
        // anything below 1 MiB is a truncated/garbage pointer, and >64 MiB
        // is a corrupt size. Both previously went straight into FNV/Hash and
        // ExpandTexture reads (the g994 AV read 0x8F900 = 587 KiB).
        if (payload != nullptr &&
            (payload < reinterpret_cast<const uint8_t*>(0x100000u) || payloadBytes > 67108864u)) {
            const uint64_t n = ++g_textureBindRefusals;
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_ERROR("D3D12Backend: t{} refused #{}: wild payload ptr={} bytes={} -- NULL SRV at slot {}",
                               idx, n, static_cast<const void*>(payload), payloadBytes, slot);
            }
            writeNullSrv();
            continue;
        }
        // F-900 HIGH-3: keep the ORIGINAL (un-expanded) payload for the upload
        // path. The pre-expansion below is for the HASH ONLY -- CreateDecodedTexture
        // expands CpuExpand plans itself (Gate 5), so passing it the already-
        // expanded bytes made it expand a SECOND time with out==the same
        // m_importScratch that owned the source: ExpandTextureToImportBytes
        // out.assign() zero-filled the buffer in place, the second decode read
        // all zeros, and a solid-black BC3 was uploaded while the cache key
        // (hashed over the correct first expansion) claimed real pixels.
        const uint8_t* rawPayload = entry.mipPayloads[0];
        uint32_t rawPayloadBytes = entry.mipBytes[0];
        if (payload == nullptr || payloadBytes == 0) {
            const uint64_t n = ++g_textureBindRefusals;
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_ERROR("D3D12Backend: t{} refused #{}: no untiled payload from the "
                               "producer ({}x{}) -- NULL SRV at slot {}",
                               idx, n, gtd.width, gtd.height, slot);
            }
            writeNullSrv();
            continue;
        }
        // PERF: producer-supplied content id -> cache lookup before expand/hash.
        // Key space is separate from the byte-hash keys (hostFlags bit 30).
        const bool useContentId = entry.contentId != 0;
        static std::unordered_set<uint64_t> s_refusedIds;  // contentIds CreateDecodedTexture refused
        if (useContentId && !s_refusedIds.empty() && s_refusedIds.count(entry.contentId)) {
            writeNullSrv();
            continue;
        }
        const auto fillIdKey = [&](TextureCacheKey& k) {
            k = {};
            k.contentHash = entry.contentId;
            k.guestAddress = gtd.baseAddress;
            k.payloadBytes = rawPayloadBytes;
            k.xenosFormat = plan.xenosFormat;
            k.importDxgiFormat = plan.importDxgiFormat ? plan.importDxgiFormat : plan.nativeDxgiFormat;
            k.width = gtd.width;
            k.height = gtd.height;
            k.depthOrArraySize = 1;
            k.mipCount = mipCount;
            k.hostFlags = 0x40000000u;
            k.dataVersion = 0;
        };
        if (useContentId) {
            TextureCacheKey idKey;
            fillIdKey(idKey);
            TextureCacheEntry idHit = {};
            if (m_textureCache.Find(idKey, idHit) && idHit.residency != TextureResidency::Absent &&
                idHit.handle != 0 && idHit.handle <= m_texturePoolByHandle.size() &&
                m_texturePoolByHandle[idHit.handle - 1] != nullptr &&
                m_texturePoolByHandle[idHit.handle - 1]->texture) {
                bindResident(idHit.handle, idKey.importDxgiFormat);
                RecordFrameTexture(entry.descriptorBytes, idHit.handle, idKey.importDxgiFormat,
                                   m_texturePoolByHandle[idHit.handle - 1]->gen, entry.contentId, mipCount);
                continue;
            }
        }
        const int64_t dcE0 = DcNowNs();
        struct DcEx { int64_t t; ~DcEx() { if (TempPerfStagesEnabled()) g_dcExpandNs += DcNowNs() - t; } } dcEx{dcE0};
        if (plan.mode == native::TextureUploadMode::CpuExpand) {
            native::TextureRefusal expandRefusal = native::TextureRefusal::None;
            if (!native::ExpandTextureToImportBytes(plan, payload, payloadBytes, m_importScratch,
                                                    &expandRefusal)) {
                const uint64_t n = ++g_textureImportRefusals[static_cast<uint32_t>(expandRefusal)];
                if (ShouldLogRefusal(n)) {
                    MCLA_LOG_ERROR("D3D12Backend: t{} expand refused #{}: {} ({}B) -- NULL SRV at "
                                   "slot {}",
                                   idx, n, native::TextureRefusalName(expandRefusal), payloadBytes,
                                   slot);
                }
                writeNullSrv();
                continue;
            }
            payload = m_importScratch.data();
            payloadBytes = static_cast<uint32_t>(m_importScratch.size());
        }

            // F-914 (c): the legals font atlas has a white-transparent halo (RGB=255,
            // alpha=0) around every glyph stroke. In ga058 (video), raw BC2 white
            // was kept, rendering visible text. Rewriting white to black causes quads
            // sampling the halo to render black (disappearing text). Default OFF (0);
            // enable with MCLA_HALO_FIX=1.
            if (payload != nullptr && payloadBytes > 0) {
                static int s_enableHaloFix = -1;
                if (s_enableHaloFix < 0) {
                    const char* e = std::getenv("MCLA_HALO_FIX");
                    s_enableHaloFix = (e && e[0] == '1') ? 1 : 0;
                }
                uint32_t haloFixed = 0;
                uint8_t minA = 255, maxA = 0;
                uint32_t whiteTransp = 0;
                // PERF: the scan is diagnostic-only unless the fix is on; run it only on the
                // rows HALO-DIAG actually prints (same cadence as s_haloDiagRows below).
                static std::atomic<uint32_t> s_haloPeek{0};
                const uint32_t hpk = s_haloPeek.fetch_add(1, std::memory_order_relaxed) + 1u;
                const bool doHaloScan = s_enableHaloFix == 1 || hpk <= kRefuseLogFirst || (hpk % kRefuseLogEvery) == 0u;
                for (uint32_t k = 0; doHaloScan && k + 3 < payloadBytes; k += 4) {
                    if (payload[k+3] < minA) minA = payload[k+3];
                    if (payload[k+3] > maxA) maxA = payload[k+3];
                    if (payload[k] == 255 && payload[k+1] == 255 &&
                        payload[k+2] == 255 && payload[k+3] == 0) {
                        ++whiteTransp;
                        if (s_enableHaloFix == 1) {
                            const_cast<uint8_t*>(payload)[k] = 0;
                            const_cast<uint8_t*>(payload)[k+1] = 0;
                            const_cast<uint8_t*>(payload)[k+2] = 0;
                            ++haloFixed;
                        }
                    }
                }
                // F-914: both rows are PER-BIND -- ga062 carries 166,456
                // HALO-DIAG (#3 marker) and 91,322 HALO-FIX (#5) rows, ~13% of
                // the log together. House cadence (kRefuseLogFirst=8 /
                // kRefuseLogEvery=200, :266-269): first 8 then every 200th, so
                // the census still fires (rule 19: a silent marker's zero is
                // vacuous). HALO-DIAG's format is kept BYTE-IDENTICAL --
                // `HALO-DIAG guest=` is cited verbatim in
                // docs/analysis-20261028/random-glyphs.md. HALO-FIX previously
                // printed a hardcoded `#1` on all 91,322 rows; it now prints its
                // true counter.
                static std::atomic<uint32_t> s_haloDiagRows{0};
                const uint32_t hdN =
                    s_haloDiagRows.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (hdN <= kRefuseLogFirst || (hdN % kRefuseLogEvery) == 0u) {
                    MCLA_LOG_WARN("HALO-DIAG guest={:08X} {}x{} bytes={} minA={} maxA={} whiteTransp={} fixed={}",
                                  gtd.baseAddress, gtd.width, gtd.height, payloadBytes, minA, maxA, whiteTransp, haloFixed);
                }
                if (haloFixed > 0) {
                    static std::atomic<uint32_t> s_haloFixRows{0};
                    const uint32_t hfN =
                        s_haloFixRows.fetch_add(1, std::memory_order_relaxed) + 1u;
                    if (hfN <= kRefuseLogFirst || (hfN % kRefuseLogEvery) == 0u) {
                        MCLA_LOG_WARN("HALO-FIX #{} fixed {} white-transparent pixels", hfN, haloFixed);
                    }
                }
            }

            // F-918 ATLAS-TESTPAT: the font atlas decodes to real glyphs (viewed:
            // read bleeding ABCDEF/0123 row bands), so the tofu must be in the
            // quads. Paint a 2-texel-row rainbow over every alpha>0 texel of the
            // atlas ONLY, keeping alpha. Then every glyph block on screen shows the
            // row band its quad samples in colour -- the user's readout of v.
            // Must run BEFORE the content hash so the cache key matches what is
            // uploaded. Toggle off with env MCLA_ATLAS_TESTPAT=0.
            {
                static const bool s_testPat = [](){
                    const char* e = std::getenv("MCLA_ATLAS_TESTPAT");
                    return (e != nullptr && e[0] == '1');
                }();
                if (s_testPat && gtd.baseAddress == 0x13BCC000u &&
                    gtd.width == 256u && gtd.height == 256u && payloadBytes == 262144u) {
                    static const uint8_t kBands[7][3] = {
                        {255, 64, 64},   // red
                        {64, 255, 64},   // green
                        {64, 64, 255},   // blue
                        {255, 255, 64},  // yellow
                        {255, 64, 255},  // magenta
                        {64, 255, 255},  // cyan
                        {255, 255, 255}, // white
                    };
                    for (uint32_t y = 0u; y < 256u; ++y) {
                        const uint8_t* band = kBands[(y / 2u) % 7u];
                        uint8_t* row = const_cast<uint8_t*>(payload) + static_cast<size_t>(y) * 1024u;
                        for (uint32_t x = 0u; x < 256u; ++x) {
                            uint8_t* t = row + static_cast<size_t>(x) * 4u;
                            if (t[3] != 0) {
                                t[0] = band[0];
                                t[1] = band[1];
                                t[2] = band[2];
                            }
                        }
                    }
                    static std::atomic<uint32_t> s_testPatCount{0};
                    const uint32_t tn = s_testPatCount.fetch_add(1) + 1;
                    if (tn <= 4 || (tn % 5000) == 0) {
                        MCLA_LOG_WARN("ATLAS-TESTPAT #{} painted 256x256 atlas (2-row bands, "
                                      "7 colours) -- alpha preserved",
                                      tn);
                    }
                }
            }
        // --- Cache key: content hash + the geometry that determines the host
        // resource (resource_cache.h:112-133). dataVersion is left 0: the
        // producer has no version counter today, and the content hash already
        // discriminates a changed payload -- which is the point of keying on it.
        const int64_t dcH0 = (!useContentId && TempPerfStagesEnabled()) ? DcNowNs() : 0;
        const uint64_t contentHash = useContentId ? entry.contentId : HashTextureContent(payload, payloadBytes);
        if (!useContentId && TempPerfStagesEnabled()) { g_dcHashNs += DcNowNs() - dcH0; g_dcHashBytes += payloadBytes; }
        TextureCacheKey key = {};
        key.contentHash = contentHash;
        key.guestAddress = gtd.baseAddress;
        key.payloadBytes = payloadBytes;
        key.xenosFormat = plan.xenosFormat;
        key.importDxgiFormat = plan.importDxgiFormat ? plan.importDxgiFormat
                                                     : plan.nativeDxgiFormat;
        key.width = gtd.width;
        key.height = gtd.height;
        key.depthOrArraySize = 1;  // Refuse(Cube/3D) means only 2D reaches here
        key.mipCount = mipCount;  // chain level count; the chain's identity is contentHash
        key.hostFlags = 0;  // no UAV
        key.dataVersion = 0;
        if (useContentId) fillIdKey(key);  // miss inserts under the id key the pre-check looks up

        TextureCacheEntry hit = {};
        if (m_textureCache.Find(key, hit) && hit.residency != TextureResidency::Absent &&
            hit.handle != 0 && hit.handle <= m_texturePoolByHandle.size() &&
            m_texturePoolByHandle[hit.handle - 1] != nullptr &&
            m_texturePoolByHandle[hit.handle - 1]->texture) {
            bindResident(hit.handle, key.importDxgiFormat);
            RecordFrameTexture(entry.descriptorBytes, hit.handle, key.importDxgiFormat,
                               m_texturePoolByHandle[hit.handle - 1]->gen, entry.contentId, mipCount);
            continue;
        }

        // --- MISS: upload once, then intern. Note the deliberate split from
        // CreateDecodedTexture(TextureImportDesc): that entry point owns a
        // single bootstrap texture and commits it as m_decodedTexture, whereas a
        // cached texture must outlive the frame, so it goes into the resident
        // pool with its staging buffer attached and is freed only by eviction.
        TextureImportDesc imp = {};
        imp.plan = &plan;
        if (plan.mode == native::TextureUploadMode::CpuExpand) {
            imp.mipPayloads[0] = payload;
            imp.mipBytes[0] = payloadBytes;
            imp.payloadAlreadyExpanded = true;
        } else {
            imp.mipPayloads[0] = rawPayload;
            imp.mipBytes[0] = rawPayloadBytes;
        }
        imp.mipCount = mipCount;
        for (uint32_t i = 1; i < mipCount; ++i) {   // mips 1..N-1 are raw (CpuExpand expands them inside CreateDecodedTexture)
            imp.mipPayloads[i] = entry.mipPayloads[i];
            imp.mipBytes[i] = entry.mipBytes[i];
        }
        imp.width = gtd.width;
        imp.height = gtd.height;
        imp.srvSlot = slot;

        Microsoft::WRL::ComPtr<ID3D12Resource> resident;
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = {};
        uint32_t impHeapIdx = UINT32_MAX;
        uint64_t impHeapOffset = 0;
        uint64_t impHeapSize = 0;
        imp.outTexture = &resident;
        imp.outSrvGpu = &srvGpu;
        imp.outHeapIndex = &impHeapIdx;
        imp.outHeapOffset = &impHeapOffset;
        imp.outHeapSize = &impHeapSize;
        ++g_dcMiss; g_dcMissBytes += payloadBytes; g_dcCache = &m_textureCache;
        if (TempPerfStagesEnabled()) {   // TEMP-DC-TIMERS: why a miss? never seen / same content again (evicted or key mismatch) / content changed
            static std::unordered_map<uint64_t, uint64_t> s_lastContent;
            const uint64_t mk = (uint64_t(gtd.baseAddress) << 24) ^ (uint64_t(gtd.width) << 12) ^ gtd.height ^ (uint64_t(plan.xenosFormat) << 50) ^ (uint64_t(rawPayloadBytes) << 32);
            auto lit = s_lastContent.find(mk);
            if (lit == s_lastContent.end()) { ++g_dcMissNew; if (s_lastContent.size() < 200000) s_lastContent[mk] = contentHash; }
            else if (lit->second == contentHash) ++g_dcMissSame;
            else { ++g_dcMissChanged; lit->second = contentHash; }
        }
        const int64_t dcM0 = DcNowNs();
        const bool dcOk = CreateDecodedTexture(imp, uploadCl, allowMipgen);
        if (TempPerfStagesEnabled()) g_dcMissNs += DcNowNs() - dcM0;
        if (!dcOk || !resident) {
            // CreateDecodedTexture already named and counted the reason.
            if (useContentId) { if (s_refusedIds.size() > 4096) s_refusedIds.clear(); s_refusedIds.insert(entry.contentId); }  // static refusal (e.g. NoHostFormat): do not re-expand/hash every draw
            writeNullSrv();
            continue;
        }

        const uint64_t handle = AcquireTexturePoolSlot();
        if (handle == 0 || handle > m_texturePoolByHandle.size() ||
            m_texturePoolByHandle[handle - 1] == nullptr) {
            RetireTarget(std::move(resident));
            if (impHeapIdx != UINT32_MAX) {
                m_retiredPlacedRanges.push_back({ UINT64_MAX, impHeapIdx, impHeapOffset, impHeapSize });
            }
            writeNullSrv();
            continue;
        }
        ResidentTexture& rt = *m_texturePoolByHandle[handle - 1];
        rt.texture = resident;
        rt.heapIndex = impHeapIdx;
        rt.heapOffset = impHeapOffset;
        rt.heapSize = impHeapSize;
        // No rt.upload: CreateDecodedTexture already retires each upload buffer to the frame fence
        // (:3334); pinning it for the texture's whole life doubled the cache's real memory.
        rt.width = gtd.width;
        rt.height = gtd.height;
        rt.mipLevels = mipCount;
        rt.xenosFormat = plan.xenosFormat;
        rt.importDxgiFormat = key.importDxgiFormat;

        TextureCacheEntry ent = {};
        ent.handle = handle;
        ent.uploadedVersion = 0;
        ent.descriptorIndex = slot;
        ent.residentBytes = payloadBytes;
        ent.payloadBytes = payloadBytes;
        ent.residency = TextureResidency::Resident;
        ent.inFlight = false;  // released at EndFrame by the caller that owns it

        // Insert may evict. The returned entries are already out of the map and
        // are MINE to release before adding our own staging SRV.
        std::vector<TextureCache::EvictedTexture> evicted = m_textureCache.Insert(key, ent);
        if (!evicted.empty()) {
            for (const TextureCache::EvictedTexture& v : evicted) {
                ReleaseTexturePoolSlot(v.entry.handle);
            }
        }
        bindResident(static_cast<uint32_t>(handle), key.importDxgiFormat);
        RecordFrameTexture(entry.descriptorBytes, static_cast<uint32_t>(handle), key.importDxgiFormat,
                           rt.gen, entry.contentId, mipCount);
    }

    // P1 FOLIAGE-BIND (MCLA_FOLIAGE_BIND=1): per-draw 24-byte descriptor ->
    // staged-SRV dump so a drawn-but-invisible impostor can be joined to its PS
    // and host SRV. stagedSlots[] is fully resolved here, so this is the honest
    // "what this draw actually bound" view, unlike TREE-GATE's guest gate byte.
    // Bounded: passes 1..8 then every 1000th. psHash/pcapDrawSeq come from the
    // PIXPROBE capture in DrawGuest and are 0 unless MCLA_PIXPROBE=1; drawSeq
    // (m_cachePipelineDraws) is always live and is the join key with PIXPROBE off.
    if (FoliageBindEnabled() &&
        (m_cachePipelineDraws <= 20 || (m_cachePipelineDraws % 100) == 0)) {
        std::string slots;
        for (uint32_t e = 0; e < count; ++e) {
            const GuestTextureEntry& ge = entries[e];
            const uint8_t* b = ge.descriptorBytes;
            const uint32_t d1 = (uint32_t(b[4]) << 24) | (uint32_t(b[5]) << 16) |
                                (uint32_t(b[6]) << 8) | uint32_t(b[7]);
            const uint32_t d3 = (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16) |
                                (uint32_t(b[14]) << 8) | uint32_t(b[15]);
            const uint32_t idx = ge.descriptorIndex;
            char t[96];
            std::snprintf(t, sizeof t, "%st%u=%08X/f%02X/s%03X/r%u", e ? ";" : "",
                          idx, d1 & 0xFFFFF000u, d1 & 0x3Fu, (d3 >> 1) & 0xFFFu,
                          idx < kShaderTextureSlotCount ? stagedSlots[idx] : 0u);
            slots.append(t);
        }
        MCLA_LOG_ERROR("FOLIAGE-BIND pass={} drawSeq={} psHash={:016X} pcapDrawSeq={} "
                       "pixprobeOn={} cnt={} slots=[{}] -- psHash/pcapDrawSeq need MCLA_PIXPROBE=1; "
                       "drawSeq is m_cachePipelineDraws (always live); join to TEMP-TEXBIND ps=/tex= "
                       "and to TREE-GATE by foliage-draw order",
                       m_textureBindPasses, static_cast<uint64_t>(m_cachePipelineDraws),
                       s_pixprobePsHash, s_pixprobeDrawSeq,
                       PixProbeEnabled() ? 1 : 0, count, slots.c_str());
    }

    uint64_t tableHash = 14695981039346656037ull;
    for (uint32_t s = 0; s < kShaderTextureSlotCount; ++s) {
        tableHash ^= stagedSlots[s];
        tableHash *= 1099511628211ull;
    }
    outTableHash = tableHash;
    return bound;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Backend::CommitSrvTableFromStaging(
    const uint32_t (&stagedSlots)[kShaderTextureSlotCount], uint64_t tableHash,
    DrawRecorder* rec, const std::vector<FallbackSrv>* fallbackSrvs,
    uint32_t boundCount, uint64_t* outCacheHits, bool* outClamped) {
    if (outClamped) *outClamped = false;
    if (!m_srvHeap || !m_device) return {};
    const uint32_t frameSlot = (m_frameIndex < kBufferCount) ? m_frameIndex : 0u;
    const uint32_t globalRingCap = SrvRingTablesPerFrame();
    const bool mtRec = (rec != nullptr);
    const bool noFallbacks = (!fallbackSrvs || fallbackSrvs->empty());
    SrvTableCache& cache = (mtRec && rec->index < kMaxMtRecorders)
                             ? m_mtSrvTableCaches[rec->index]
                             : m_mainSrvTableCache;
    static_assert((kSrvTableCacheSlots & (kSrvTableCacheSlots - 1u)) == 0u,
                  "kSrvTableCacheSlots must be a power of two");
    const uint32_t cacheIdx =
        static_cast<uint32_t>((tableHash ^ (tableHash >> 32)) & (kSrvTableCacheSlots - 1u));
    SrvTableCacheEntry& ce = cache.entries[cacheIdx];
    static const bool s_noSrvCache = std::getenv("MCLA_NO_SRV_TABLE_CACHE") != nullptr;  // A/B kill-switch
    if (!s_noSrvCache && noFallbacks && ce.valid && ce.epoch == m_srvTableEpoch &&
        ce.frameSlot == frameSlot && ce.keyHash == tableHash &&
        std::memcmp(ce.slotVers, stagedSlots, sizeof(stagedSlots)) == 0) {
        if (mtRec) rec->srvTableGpu = ce.gpu; else m_currentSrvTableGpu = ce.gpu;
        if (outCacheHits) *outCacheHits += boundCount;
        else if (!mtRec) m_stats.cacheHits += boundCount;
        return ce.gpu;
    }

    static const bool s_noSrvRingExpand = [] {
        const char* e = std::getenv("MCLA_NO_SRV_RING_EXPAND");
        return e && e[0] == '1';
    }();
    uint32_t tableIdx = 0;
    bool clamped = false;
    uint32_t ringSliceCap = 0;
    if (!s_noSrvRingExpand) {
        ringSliceCap = globalRingCap;
        const uint32_t rawIdx = s_frameSrvRingCursor.fetch_add(1, std::memory_order_relaxed);
        clamped = (rawIdx >= ringSliceCap);
        tableIdx = !clamped ? rawIdx : (ringSliceCap ? (ringSliceCap - 1u) : 0u);
        if (mtRec) rec->srvRingOffset = rawIdx + 1;
        else m_srvRingOffset = rawIdx + 1;
    } else {
        uint32_t& ringOffsetRef = mtRec ? rec->srvRingOffset : m_srvRingOffset;
        ringSliceCap = mtRec ? rec->srvRingTables
                             : (m_mainSrvRingTables ? m_mainSrvRingTables : globalRingCap);
        const uint32_t ringSliceBase = mtRec ? rec->srvRingBase : 0u;
        clamped = (ringOffsetRef >= ringSliceCap);
        const uint32_t ringIdxLocal = !clamped
                                     ? ringOffsetRef++
                                     : (ringSliceCap ? (ringSliceCap - 1u) : 0u);
        tableIdx = ringSliceBase + ringIdxLocal;
    }

    if (clamped && ringSliceCap > 0) {
        if (mtRec) {
            ++rec->srvRingClampedCount;
        } else {
            ++m_srvRingClampPassesFrame;
            if (!m_srvRingFrameHadClamp) {
                m_srvRingFrameHadClamp = true;
                ++m_srvRingClampedFrames;
            }
        }
        static std::atomic<uint32_t> s_ringClamp{0};
        const uint32_t n = s_ringClamp.fetch_add(1) + 1;
        if (n <= 3 || (n % 500) == 0)
            MCLA_LOG_ERROR("SRV-RING-CLAMP #{} frame bind passes exceeded {} tables/frame{}; "
                           "extra passes alias the last table (silent texture corruption)",
                           n, ringSliceCap, mtRec ? " (mt-recorder)" : "");
    }

    const uint32_t tableBaseSlot =
        kSrvHeapBaseCount +
        (frameSlot * globalRingCap + tableIdx) * kShaderTextureSlotCount;
    D3D12_CPU_DESCRIPTOR_HANDLE dstStart(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    dstStart.ptr += static_cast<size_t>(tableBaseSlot) * m_srvDescriptorSize;
    if (m_srvStagingHeap) {
        D3D12_CPU_DESCRIPTOR_HANDLE srcHandles[kShaderTextureSlotCount];
        for (uint32_t s = 0; s < kShaderTextureSlotCount; ++s) {
            srcHandles[s] = StagingSrvCpu(stagedSlots[s]);
        }
        const UINT dstRangeSize = kShaderTextureSlotCount;
        m_device->CopyDescriptors(1, &dstStart, &dstRangeSize,
                                  kShaderTextureSlotCount, srcHandles, nullptr,
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
        nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels = 1;
        for (uint32_t s = 0; s < kShaderTextureSlotCount; ++s) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = dstStart;
            h.ptr += static_cast<size_t>(s) * m_srvDescriptorSize;
            m_device->CreateShaderResourceView(nullptr, &nullSrv, h);
        }
    }
    if (fallbackSrvs) {
        for (const FallbackSrv& fb : *fallbackSrvs) {
            D3D12_CPU_DESCRIPTOR_HANDLE hRing = dstStart;
            hRing.ptr += static_cast<size_t>(fb.idx) * m_srvDescriptorSize;
            (++g_dcSrv, m_device)->CreateShaderResourceView(fb.res, &fb.desc, hRing);
        }
    }

    D3D12_GPU_DESCRIPTOR_HANDLE tableGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    tableGpu.ptr += static_cast<size_t>(tableBaseSlot) * m_srvDescriptorSize;
    if (mtRec) rec->srvTableGpu = tableGpu; else m_currentSrvTableGpu = tableGpu;
    if (!clamped && noFallbacks) {
        ce.epoch = m_srvTableEpoch;
        ce.frameSlot = frameSlot;
        ce.keyHash = tableHash;
        std::memcpy(ce.slotVers, stagedSlots, sizeof(stagedSlots));
        ce.gpu = tableGpu;
        ce.valid = true;
    }

    if (clamped && outClamped) *outClamped = true;
    if (outCacheHits) *outCacheHits += boundCount;
    else if (!mtRec) m_stats.cacheHits += boundCount;
    return tableGpu;
}

extern std::atomic<bool> g_moviePhase;  // set in patches.cpp on 'activemovie'

uint32_t D3D12Backend::BindGuestTextures(const GuestTextureEntry* entries, uint32_t count, DrawRecorder* rec) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    return BindGuestTexturesUnlocked(entries, count, rec);
}

uint32_t D3D12Backend::BindGuestTexturesUnlocked(const GuestTextureEntry* entries, uint32_t count, DrawRecorder* rec) {
    if (!m_srvHeap || !m_device) return 0;
    if (count > kMaxGuestTextureDescriptors) count = kMaxGuestTextureDescriptors;
    if (count == 0 || entries == nullptr) return 0;

    // MCLA_FACE_TEX=1 (2026-11-20, log-only): unique bound-texture census with
    // the decoded descriptor fields, so the cutscene face texture can be named
    // without guessing. Capped and de-duplicated; no guest byte is touched.
    static const bool s_faceTex = [] {
        const char* e = std::getenv("MCLA_FACE_TEX");
        return e && e[0] == '1';
    }();
    if (s_faceTex && g_moviePhase.load(std::memory_order_relaxed)) {
        static std::unordered_set<uint64_t> s_seenTex;
        for (uint32_t i = 0; i < count; ++i) {
            native::GuestTextureDesc gd{};
            if (!native::DecodeGuestTextureDescriptor(entries[i].descriptorBytes, gd)) continue;
            const uint64_t key = (uint64_t(gd.baseAddress) << 24) ^
                                 (uint64_t(gd.xenosFormat) << 16) ^
                                 (uint64_t(gd.width) << 4) ^ gd.height;
            if (s_seenTex.size() >= 4096 || !s_seenTex.insert(key).second) continue;
            MCLA_LOG_ERROR("FACE-TEX base={:08X} fmt={} {}x{} depth={} stacked={} tiled={} "
                           "packed={} swz={:03X} clampU={} clampV={} mips={}",
                           gd.baseAddress, gd.xenosFormat, gd.width, gd.height,
                           gd.depthOrArraySize, gd.stacked ? 1 : 0, gd.tiled ? 1 : 0,
                           gd.packedMips ? 1 : 0, gd.swizzle,
                           static_cast<unsigned>(gd.clampU), static_cast<unsigned>(gd.clampV),
                           gd.mipMaxLevel);
        }
    }

    if (!m_inFrame) {
        if (!BeginFrame()) return 0;
    }

    const bool mtRec = (rec != nullptr);
    if (!mtRec) ++m_srvRingPassesFrame;

    static const bool s_noTexBindSkip = [] {
        const char* e = std::getenv("MCLA_NO_TEXBIND_SKIP");
        return e && e[0] == '1';
    }();
    // TEXSET: main-thread memo widened from "previous draw" to a 32-way set keyed by the
    // texture set, so A-B-A-B alternation hits too. Same validity witnesses per entry.
    // MCLA_NO_TEXSET=1 restores the single memo. PixProbe keeps the single memo (its
    // hit path restores the last-bind snapshot only).
    static const bool s_noTexSet = [] { const char* e = std::getenv("MCLA_NO_TEXSET"); return e && e[0] == '1'; }();
    TexBindMemo* memoP = mtRec ? &rec->texBindMemo : &m_mainTexBindMemo;
    if (!mtRec && !s_noTexSet && !PixProbeEnabled()) {
        uint64_t h = 0xcbf29ce484222325ull ^ count;
        for (uint32_t i = 0; i < count; ++i)
            h = ((h ^ entries[i].contentId) * 0x100000001b3ull ^ entries[i].descriptorIndex) * 0x9E3779B97F4A7C15ull;
        // 256 ways measured no better (seed 0x9 memoMiss 46-51k/2 s either way): misses are per-frame/per-pass compulsory (missWhy).
        memoP = &m_texBindSet[(h >> 32) & (kTexBindSetWays - 1u)];
    }
    TexBindMemo& memo = *memoP;
    const uint64_t rtEpoch = m_rtPassEpoch.load(std::memory_order_acquire);
    if (!s_noTexBindSkip && memo.valid &&
        memo.frameBegins == m_beginFrameBegins &&
        memo.frameIndex == m_frameIndex &&
        memo.srvTableEpoch == m_srvTableEpoch &&
        memo.rtPassEpoch == rtEpoch &&
        memo.count == count &&
        memo.srvTableGpu.ptr != 0) {
        bool match = true;
        for (uint32_t i = 0; i < count; ++i) {
            const GuestTextureEntry& e = entries[i];
            const TexBindMemoSlot& m = memo.slots[i];
            if (e.descriptorIndex != m.descriptorIndex ||
                e.mipCount != m.mipCount ||
                e.contentId != m.contentId ||
                (e.contentId == 0 && e.mipPayloads[0] != nullptr) ||
                std::memcmp(e.descriptorBytes, m.descriptorBytes, 24) != 0) {
                match = false;
                break;
            }
        }
        if (match) {
            if (mtRec) {
                rec->srvTableGpu = memo.srvTableGpu;
                rec->cacheHits += memo.boundCount;
            } else {
                m_currentSrvTableGpu = memo.srvTableGpu;
                m_stats.cacheHits += memo.boundCount;
                if (PixProbeEnabled() && s_pixprobeMemoFrameBegins == m_beginFrameBegins) {
                    std::memcpy(s_pixprobeStagedSlots, s_pixprobeMemoStagedSlots, sizeof(s_pixprobeStagedSlots));
                    s_pixprobeTexCount = s_pixprobeMemoTexCount;
                    for (uint32_t i = 0; i < s_pixprobeMemoTexCount; ++i) {
                        s_pixprobeTex[i] = s_pixprobeMemoTex[i];
                    }
                    s_pixprobeCurSamplesSceneColor = s_pixprobeMemoSamplesSceneColor;
                }
            }
            if (!mtRec) ++g_tbAcc[5];
            return memo.boundCount;
        }
    }
    if (!mtRec) {
        ++g_tbAcc[6];
        // PERF-STAGES missWhy: invalid/frame/srvEpoch/rtEpoch/content (first failing witness).
        const int why = !memo.valid ? 0 : (memo.frameBegins != m_beginFrameBegins || memo.frameIndex != m_frameIndex) ? 1
                      : memo.srvTableEpoch != m_srvTableEpoch ? 2 : memo.rtPassEpoch != rtEpoch ? 3 : 4;
        ++g_tbMissWhy[why];
    }
    const int64_t tbT0 = mtRec ? 0 : DcNowNs();

    uint32_t stagedSlots[kShaderTextureSlotCount] = {};
    uint64_t tableHash = 0;
    std::vector<FallbackSrv> localFallbackSrvs;
    std::vector<FallbackSrv>& fallbackSrvs = mtRec ? localFallbackSrvs : m_mainFallbackSrvs;
    fallbackSrvs.clear();
    ID3D12GraphicsCommandList* uploadCl = rec ? rec->list : m_commandList.Get();
    const uint32_t bound = ResolveGuestTexturesToStaging(
        entries, count, uploadCl, !mtRec, stagedSlots, tableHash, &fallbackSrvs);
    // PIXPROBE: capture the staged SRV index and decoded descriptors on the real
    // bind path (and snapshot into s_pixprobeMemo* so subsequent memo hits stay accurate).
    if (PixProbeEnabled() && !mtRec) {
        for (uint32_t s = 0; s < 32; ++s) s_pixprobeStagedSlots[s] = stagedSlots[s];
        s_pixprobeTexCount = 0;
        for (uint32_t i = 0; i < count && s_pixprobeTexCount < 16; ++i) {
            native::GuestTextureDesc gd{};
            if (!native::DecodeGuestTextureDescriptor(entries[i].descriptorBytes, gd)) continue;
            PixprobeTex& t = s_pixprobeTex[s_pixprobeTexCount++];
            t.slot = entries[i].descriptorIndex;
            t.base = gd.baseAddress; t.fmt = gd.xenosFormat;
            t.w = gd.width; t.h = gd.height;
            t.packed = gd.packedMips ? 1u : 0u;
            t.mips = gd.mipMaxLevel; t.swz = gd.swizzle;
            const uint8_t* b = entries[i].descriptorBytes;
            const uint32_t d0 = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
            t.gamma = (((d0 >> 2) & 63u) == 63u) ? 1u : 0u;
        }
        std::memcpy(s_pixprobeMemoStagedSlots, s_pixprobeStagedSlots, sizeof(s_pixprobeStagedSlots));
        s_pixprobeMemoTexCount = s_pixprobeTexCount;
        for (uint32_t i = 0; i < s_pixprobeTexCount; ++i) {
            s_pixprobeMemoTex[i] = s_pixprobeTex[i];
        }
        s_pixprobeMemoSamplesSceneColor = s_pixprobeCurSamplesSceneColor;
        s_pixprobeMemoFrameBegins = m_beginFrameBegins;
    }
    bool clamped = false;
    const int64_t tbT1 = mtRec ? 0 : DcNowNs();
    const D3D12_GPU_DESCRIPTOR_HANDLE tableGpu = CommitSrvTableFromStaging(
        stagedSlots, tableHash, rec, &fallbackSrvs, bound,
        mtRec ? &rec->cacheHits : nullptr, &clamped);
    if (!mtRec) { g_tbAcc[1] += tbT1 - tbT0; g_tbAcc[2] += DcNowNs() - tbT1; }
    if (!s_noTexBindSkip && !clamped && tableGpu.ptr != 0) {
        memo.valid = true;
        memo.frameBegins = m_beginFrameBegins;
        memo.frameIndex = m_frameIndex;
        memo.srvTableEpoch = m_srvTableEpoch;
        memo.rtPassEpoch = rtEpoch;
        memo.count = count;
        memo.boundCount = bound;
        memo.srvTableGpu = tableGpu;
        for (uint32_t i = 0; i < count; ++i) {
            memo.slots[i].descriptorIndex = entries[i].descriptorIndex;
            memo.slots[i].mipCount = entries[i].mipCount;
            memo.slots[i].contentId = entries[i].contentId;
            std::memcpy(memo.slots[i].descriptorBytes, entries[i].descriptorBytes, 24);
        }
    } else {
        memo.Invalidate();
    }
    return bound;
}

void D3D12Backend::BindDecodedTexture() {
    BindDecodedTextureInternal(m_commandList.Get(), m_currentSrvTableGpu, s_heapsBound,
                               m_samplerTableGpu, s_boundSrvTableGpu, s_boundSamplerTableGpu,
                               s_boundRootSig);
}

void D3D12Backend::BindDecodedTexture(ID3D12GraphicsCommandList* cl, D3D12_GPU_DESCRIPTOR_HANDLE srvTableGpuIn, bool& heapsBound, D3D12_GPU_DESCRIPTOR_HANDLE samplerTableGpu) {
    D3D12_GPU_DESCRIPTOR_HANDLE dummySrv = {};
    D3D12_GPU_DESCRIPTOR_HANDLE dummySmp = {};
    BindDecodedTextureInternal(cl, srvTableGpuIn, heapsBound, samplerTableGpu,
                               dummySrv, dummySmp, nullptr);
}

static bool SamplerHeapFixEnabled() {
    static const bool s_on = [] {
        const char* e = std::getenv("MCLA_NO_SAMPLER_HEAP_FIX");
        return !(e && e[0] == '1');
    }();
    return s_on;
}

void D3D12Backend::BindDecodedTextureInternal(ID3D12GraphicsCommandList* cl,
                                              D3D12_GPU_DESCRIPTOR_HANDLE srvTableGpuIn,
                                              bool& heapsBound,
                                              D3D12_GPU_DESCRIPTOR_HANDLE samplerTableGpu,
                                              D3D12_GPU_DESCRIPTOR_HANDLE& boundSrvTableGpu,
                                              D3D12_GPU_DESCRIPTOR_HANDLE& boundSamplerTableGpu,
                                              ID3D12RootSignature* boundRootSig) {
    if (!m_srvHeap || !cl) return;
    static const bool s_noTexBindSkip = [] {
        const char* e = std::getenv("MCLA_NO_TEXBIND_SKIP");
        return e && e[0] == '1';
    }();
    // Bind BOTH heaps simultaneously: D3D12 requires all descriptor heaps that
    // are referenced by descriptor tables to be bound at draw time.
    ID3D12DescriptorHeap* activeSmp = (SamplerHeapFixEnabled() && m_samplerHeaps[m_activeSamplerHeapIdx].heap)
        ? m_samplerHeaps[m_activeSamplerHeapIdx].heap.Get()
        : m_samplerHeap.Get();
    if (m_srvHeap && activeSmp) {
        if (!heapsBound) {
            ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get(), activeSmp };
            cl->SetDescriptorHeaps(2, heaps);
            heapsBound = true;
            boundSrvTableGpu = {};
            boundSamplerTableGpu = {};
        }
    } else if (m_srvHeap) {
        cl->SetDescriptorHeaps(1, m_srvHeap.GetAddressOf());
        heapsBound = false;
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE srvTableGpu =
        (srvTableGpuIn.ptr != 0) ? srvTableGpuIn
                                 : m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    if (s_noTexBindSkip || !heapsBound || boundRootSig != m_rootSignature.Get() ||
        boundSrvTableGpu.ptr != srvTableGpu.ptr) {
        cl->SetGraphicsRootDescriptorTable(0, srvTableGpu);
        boundSrvTableGpu = srvTableGpu;
    }

    if (m_samplerHeap) {
        const D3D12_GPU_DESCRIPTOR_HANDLE smpGpu =
            (samplerTableGpu.ptr != 0) ? samplerTableGpu
                                       : m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        if (s_noTexBindSkip || !heapsBound || boundRootSig != m_rootSignature.Get() ||
            boundSamplerTableGpu.ptr != smpGpu.ptr) {
            cl->SetGraphicsRootDescriptorTable(1, smpGpu);
            boundSamplerTableGpu = smpGpu;
        }
    }
}

// The HostSamplerDesc -> D3D12_SAMPLER_DESC copy (field-for-field; see the notes in BindGuestSampler).
static D3D12_SAMPLER_DESC ToD3D12SamplerDesc(const native::HostSamplerDesc& h) {
    D3D12_SAMPLER_DESC sd = {};
    sd.Filter = static_cast<D3D12_FILTER>(h.filter);
    sd.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(h.addressU);
    sd.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(h.addressV);
    sd.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(h.addressW);
    sd.MipLODBias = h.mipLodBias;
    sd.MaxAnisotropy = h.maxAnisotropy;
    sd.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(h.comparisonFunc);
    for (UINT c = 0; c < 4; ++c) sd.BorderColor[c] = h.borderColor[c];
    sd.MinLOD = h.minLod;
    sd.MaxLOD = h.maxLod;
    return sd;
}

// G0: the two legality checks BindGuestSampler makes, as a predicate (no logging).
static bool GuestSamplerLegal(const native::HostSamplerDesc& s) {
    return IsLegalD3D12Filter(s.filter) &&
           (s.comparisonFunc == 0 ||
            static_cast<uint32_t>(s.comparisonFunc) == static_cast<uint32_t>(D3D12_COMPARISON_FUNC_NONE));
}

// G0: bind one sampler per fetch constant at root parameter 1 (s0..s15). Slots that are
// not decoded or not legal stay neutral. Blocks are deduped by content. A new block needs
// kSamplerBlockSize free heap entries. Shortcut: the sampler heap is never reset, so a boot gets about
// (kSamplerHeapSize - 16) / 16 distinct tables; past that the draw binds the neutral block
// and logs it. Upgrade: a per-frame ring with a fence before reuse.
bool D3D12Backend::BindGuestSamplerTable(const native::HostSamplerDesc* table, uint32_t mask) {
    return BindGuestSamplerTableInternal(table, mask, m_commandList.Get(), s_heapsBound,
                                         m_samplerTableGpu, s_boundSamplerTableGpu, s_boundRootSig);
}

bool D3D12Backend::InternGuestSamplerTable(const native::HostSamplerDesc* table, uint32_t mask,
                                           D3D12_GPU_DESCRIPTOR_HANDLE& samplerTableGpuOut,
                                           uint32_t activeMask) {
    if (!m_samplerHeap || !m_device) return false;
    if (SamplerHeapFixEnabled() && activeMask != 0) {
        mask &= activeMask;
    }
    uint32_t legal = 0;
    if (table && mask != 0) {
        for (uint32_t i = 0; i < kSamplerBlockSize; ++i) {
            if (!(mask & (1u << i))) continue;
            if (!GuestSamplerLegal(table[i])) {
                const uint64_t n = ++g_samplerRefusals;
                if (ShouldLogRefusal(n))
                    MCLA_LOG_ERROR("D3D12Backend: guest sampler table slot {} refused: filter 0x{:X} "
                                   "comparisonFunc {}; slot stays neutral", i, table[i].filter, table[i].comparisonFunc);
                continue;
            }
            legal |= 1u << i;
        }
    }

    ID3D12DescriptorHeap* activeHeap = m_samplerHeaps[m_activeSamplerHeapIdx].heap
        ? m_samplerHeaps[m_activeSamplerHeapIdx].heap.Get()
        : m_samplerHeap.Get();

    if (legal == 0) {
        samplerTableGpuOut = activeHeap->GetGPUDescriptorHandleForHeapStart();
        return true;
    }

    uint64_t key = 14695981039346656037ull ^ legal;
    for (uint32_t i = 0; i < kSamplerBlockSize; ++i) {
        if (!(legal & (1u << i))) continue;
        key *= 1099511628211ull;
        key ^= (static_cast<uint64_t>(i) << 56) ^ table[i].hash;
    }
    if (key == 0) key = 1;  // key 0 is reserved exclusively for all-neutral base 0

    auto tryAllocOrRecycle = [&](SamplerHeapInstance& sh) -> uint32_t {
        if (sh.heapOffset + kSamplerBlockSize <= kSamplerHeapSize) {
            uint32_t b = sh.heapOffset / kSamplerBlockSize;
            sh.heapOffset += kSamplerBlockSize;
            return b;
        }
        const uint64_t completed = m_fence ? m_fence->GetCompletedValue() : UINT64_MAX;
        uint32_t doneVictim = UINT32_MAX;
        uint64_t doneOldestFrame = UINT64_MAX;
        uint32_t waitVictim = UINT32_MAX;
        uint64_t waitSmallestFence = UINT64_MAX;
        for (uint32_t b = 1u; b < kMaxSamplerBlocks; ++b) {
            const SamplerBlockSlot& sb = sh.blocks[b];
            if (sb.lastBeginFrame == m_beginFrameBegins) continue;
            if (sb.fenceValue != UINT64_MAX && sb.fenceValue <= completed) {
                if (sb.lastBeginFrame < doneOldestFrame) {
                    doneOldestFrame = sb.lastBeginFrame;
                    doneVictim = b;
                }
            } else if (sb.fenceValue != UINT64_MAX) {
                if (sb.fenceValue < waitSmallestFence) {
                    waitSmallestFence = sb.fenceValue;
                    waitVictim = b;
                }
            }
        }
        if (doneVictim != UINT32_MAX) {
            sh.blockIndex.erase(sh.blocks[doneVictim].key);
            return doneVictim;
        }
        if (waitVictim != UINT32_MAX && m_fence && m_fenceEvent) {
            if (m_fence->GetCompletedValue() < waitSmallestFence) {
                m_fence->SetEventOnCompletion(waitSmallestFence, m_fenceEvent);
                WaitForSingleObject(m_fenceEvent, INFINITE);
            }
            if (waitSmallestFence > m_fenceCompletedLB) m_fenceCompletedLB = waitSmallestFence;
            sh.blockIndex.erase(sh.blocks[waitVictim].key);
            return waitVictim;
        }
        return UINT32_MAX;
    };

    uint32_t base = 0;
    bool ok = true;
    SamplerHeapInstance* curHeap = &m_samplerHeaps[m_activeSamplerHeapIdx];
    if (!curHeap->heap) {
        m_activeSamplerHeapIdx = 0;
        curHeap = &m_samplerHeaps[0];
    }

    auto it = curHeap->blockIndex.find(key);
    if (it != curHeap->blockIndex.end()) {
        base = it->second;
        const uint32_t blk = base / kSamplerBlockSize;
        if (blk < kMaxSamplerBlocks) {
            curHeap->blocks[blk].lastBeginFrame = m_beginFrameBegins;
            curHeap->blocks[blk].fenceValue = UINT64_MAX;
        }
    } else {
        uint32_t targetBlk = tryAllocOrRecycle(*curHeap);
        if (targetBlk == UINT32_MAX && SamplerHeapFixEnabled()) {
            for (uint32_t step = 1; step < kSamplerHeapCount; ++step) {
                const uint32_t nextIdx = (m_activeSamplerHeapIdx + step) % kSamplerHeapCount;
                if (!m_samplerHeaps[nextIdx].heap) continue;
                SamplerHeapInstance* nh = &m_samplerHeaps[nextIdx];
                auto nhIt = nh->blockIndex.find(key);
                if (nhIt != nh->blockIndex.end()) {
                    m_activeSamplerHeapIdx = nextIdx;
                    curHeap = nh;
                    m_samplerHeap = curHeap->heap;
                    m_samplerHeapSwitched = true;
                    base = nhIt->second;
                    const uint32_t blk = base / kSamplerBlockSize;
                    if (blk < kMaxSamplerBlocks) {
                        curHeap->blocks[blk].lastBeginFrame = m_beginFrameBegins;
                        curHeap->blocks[blk].fenceValue = UINT64_MAX;
                    }
                    targetBlk = UINT32_MAX - 1;
                    break;
                }
                uint32_t b = tryAllocOrRecycle(*nh);
                if (b != UINT32_MAX) {
                    m_activeSamplerHeapIdx = nextIdx;
                    curHeap = nh;
                    m_samplerHeap = curHeap->heap;
                    m_samplerHeapSwitched = true;
                    targetBlk = b;
                    break;
                }
            }
        }

        if (targetBlk == UINT32_MAX - 1) {
            // Already cached in target heap; base is valid.
        } else if (targetBlk != UINT32_MAX && targetBlk < kMaxSamplerBlocks) {
            base = targetBlk * kSamplerBlockSize;
            const D3D12_SAMPLER_DESC neutral = NeutralSamplerDesc();
            const D3D12_CPU_DESCRIPTOR_HANDLE heapCpu = curHeap->heap->GetCPUDescriptorHandleForHeapStart();
            for (uint32_t i = 0; i < kSamplerBlockSize; ++i) {
                D3D12_CPU_DESCRIPTOR_HANDLE dst = heapCpu;
                dst.ptr += static_cast<SIZE_T>(base + i) * m_samplerDescriptorSize;
                if (legal & (1u << i)) {
                    const D3D12_SAMPLER_DESC sd = ToD3D12SamplerDesc(table[i]);
                    m_device->CreateSampler(&sd, dst);
                } else {
                    // D3D12 forbids CopyDescriptorsSimple from a SHADER_VISIBLE heap; create directly.
                    m_device->CreateSampler(&neutral, dst);
                }
            }
            curHeap->blocks[targetBlk] = { key, m_beginFrameBegins, UINT64_MAX };
            curHeap->blockIndex.emplace(key, base);
            D3D12_GPU_DESCRIPTOR_HANDLE reusedGpu = curHeap->heap->GetGPUDescriptorHandleForHeapStart();
            reusedGpu.ptr += static_cast<SIZE_T>(base) * m_samplerDescriptorSize;
            if (s_boundSamplerTableGpu.ptr == reusedGpu.ptr) s_boundSamplerTableGpu = {};
        } else {
            const uint64_t n = ++g_samplerRefusals;
            if (ShouldLogRefusal(n))
                MCLA_LOG_ERROR("D3D12Backend: sampler heap full ({} entries, all blocks in current frame); "
                               "fetch-constant sampler table refused, neutral block bound", kSamplerHeapSize);
            ok = false;
        }
    }

    if (m_activeSamplerHeapIdx == 0) {
        m_samplerBlocks = curHeap->blocks;
        m_samplerHeapOffset = curHeap->heapOffset;
        m_guestSamplerBlockIndex = curHeap->blockIndex;
    }

    samplerTableGpuOut = curHeap->heap->GetGPUDescriptorHandleForHeapStart();
    samplerTableGpuOut.ptr += static_cast<SIZE_T>(base) * m_samplerDescriptorSize;
    return ok;
}

bool D3D12Backend::BindGuestSamplerTable(const native::HostSamplerDesc* table, uint32_t mask, ID3D12GraphicsCommandList* cl, bool& heapsBound, D3D12_GPU_DESCRIPTOR_HANDLE& samplerTableGpuOut) {
    if (cl == m_commandList.Get()) {
        return BindGuestSamplerTableInternal(table, mask, cl, heapsBound, samplerTableGpuOut,
                                             s_boundSamplerTableGpu, s_boundRootSig);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE dummyBound = {};
    return BindGuestSamplerTableInternal(table, mask, cl, heapsBound, samplerTableGpuOut,
                                         dummyBound, nullptr);
}

bool D3D12Backend::BindGuestSamplerTableInternal(const native::HostSamplerDesc* table, uint32_t mask,
                                                 ID3D12GraphicsCommandList* cl, bool& heapsBound,
                                                 D3D12_GPU_DESCRIPTOR_HANDLE& samplerTableGpuOut,
                                                 D3D12_GPU_DESCRIPTOR_HANDLE& boundSamplerTableGpu,
                                                 ID3D12RootSignature* boundRootSig,
                                                 uint32_t activeMask,
                                                 D3D12_GPU_DESCRIPTOR_HANDLE* boundSrvTableGpu) {
    if (!m_samplerHeap || !cl) return false;
    const bool ok = InternGuestSamplerTable(table, mask, samplerTableGpuOut, activeMask);
    static const bool s_noTexBindSkip = [] {
        const char* e = std::getenv("MCLA_NO_TEXBIND_SKIP");
        return e && e[0] == '1';
    }();
    const bool wasHeapsBound = heapsBound;
    bool needHeapRebind = !heapsBound || m_samplerHeapSwitched;
    ID3D12DescriptorHeap* activeSamplerHeap = m_samplerHeaps[m_activeSamplerHeapIdx].heap
        ? m_samplerHeaps[m_activeSamplerHeapIdx].heap.Get()
        : m_samplerHeap.Get();
    if (m_srvHeap) {
        if (needHeapRebind) {
            ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get(), activeSamplerHeap };
            cl->SetDescriptorHeaps(2, heaps);
            heapsBound = true;
            boundSamplerTableGpu = {};
            if (boundSrvTableGpu) {
                *boundSrvTableGpu = {};
            }
            s_boundSrvTableGpu = {};
            m_samplerHeapSwitched = false;
            static std::atomic<uint32_t> s_swLog{0};
            const uint32_t swN = s_swLog.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (swN <= 8u || (swN % 100u) == 0u) {
                MCLA_LOG_INFO("SAMPLER-HEAP-SWITCH #{} switched to heap {} (new descriptor heap bound on command list)",
                              swN, m_activeSamplerHeapIdx);
            }
        }
    } else {
        if (needHeapRebind) {
            cl->SetDescriptorHeaps(1, &activeSamplerHeap);
            heapsBound = false;
            boundSamplerTableGpu = {};
            if (boundSrvTableGpu) {
                *boundSrvTableGpu = {};
            }
            s_boundSrvTableGpu = {};
            m_samplerHeapSwitched = false;
        }
    }
    if (heapsBound && boundRootSig == m_rootSignature.Get()) {
        if (s_noTexBindSkip || !wasHeapsBound ||
            boundSamplerTableGpu.ptr != samplerTableGpuOut.ptr) {
            cl->SetGraphicsRootDescriptorTable(1, samplerTableGpuOut);
            boundSamplerTableGpu = samplerTableGpuOut;
        }
    } else {
        boundSamplerTableGpu = {};
    }
    return ok;
}

// F-644: bind the guest's decoded sampler state at root parameter 1.
bool D3D12Backend::BindGuestSampler(const native::HostSamplerDesc& hostSampler) {
    return BindGuestSamplerInternal(hostSampler, m_commandList.Get(), s_heapsBound,
                                    m_samplerTableGpu, s_boundSamplerTableGpu, s_boundRootSig);
}

bool D3D12Backend::InternGuestSampler(const native::HostSamplerDesc& hostSampler,
                                      D3D12_GPU_DESCRIPTOR_HANDLE& samplerTableGpuOut) {
    if (GuestSamplerTableEnabled()) {
        native::HostSamplerDesc singleTab[renderer::kShaderSamplerCount] = {};
        singleTab[0] = hostSampler;
        return InternGuestSamplerTable(singleTab, 1u, samplerTableGpuOut);
    }
    if (!m_samplerHeap || !m_device) return false;

    // F-642: validate the decoded filter against the REAL D3D12_FILTER value set
    // before it reaches a D3D12_SAMPLER_DESC. HostSamplerDesc::filter comes from
    // texture_decode.h's restated enum, and 6 of its 11 filter values (plus the
    // whole comparison run) do NOT match d3d12.h -- see the table at the top of
    // this file. Casting 0x034 straight into D3D12_FILTER would land on
    // D3D12_FILTER_MINIMUM_MIN_POINT_MAG_MIP_LINEAR, a MINIMUM reduction mode,
    // which is a legal filter and therefore a SILENTLY wrong image. Refusing is
    // the only honest option until texture_decode.h:503-517 is corrected.
    if (!IsLegalD3D12Filter(hostSampler.filter)) {
        const uint64_t n = ++g_samplerRefusals;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_ERROR("D3D12Backend: guest sampler refused #{}: filter 0x{:X} is not a "
                           "D3D12_FILTER (texture_decode.h:503-517 has not been reconciled with "
                           "d3d12.h:3541-3600); refusing rather than binding a wrong filter",
                           n, hostSampler.filter);
        }
        return false;
    }
    // F-642: same for the comparison func. D3D12 requires NONE or ALWAYS for a
    // legal descriptor; texture_decode.h's run is off by one (its 0 = NEVER, but
    // d3d12.h:1587 has NEVER = 1 and 0 = NONE), so only a decoded 0 is safe to
    // forward verbatim today. Anything else is refused, not remapped, because
    // remapping would need the (absent) knowledge of which Xenos value it came
    // from.
    if (hostSampler.comparisonFunc != 0 &&
        static_cast<uint32_t>(hostSampler.comparisonFunc) !=
            static_cast<uint32_t>(D3D12_COMPARISON_FUNC_NONE)) {
        const uint64_t n = ++g_samplerRefusals;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_ERROR("D3D12Backend: guest sampler refused #{}: comparisonFunc {} is not "
                           "verifiable against d3d12.h:1586-1594 (texture_decode.h:514-515 is "
                           "off by one); a non-comparison sampler is refused, not guessed",
                           n, hostSampler.comparisonFunc);
        }
        return false;
    }

    uint32_t index = 0;
    auto it = m_guestSamplerIndex.find(hostSampler.hash);
    if (it != m_guestSamplerIndex.end()) {
        index = it->second;
        samplerTableGpuOut = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        samplerTableGpuOut.ptr += static_cast<SIZE_T>(index) * m_samplerDescriptorSize;
    } else {
        if (m_samplerHeapOffset >= kSamplerHeapSize) {
            const uint64_t n = ++g_samplerRefusals;
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_ERROR("D3D12Backend: sampler heap exhausted at {} entries; refused guest "
                               "sampler #{} filter=0x{:X} addr={}/{} aniso={}",
                               kSamplerHeapSize, n, hostSampler.filter, hostSampler.addressU,
                               hostSampler.addressV, hostSampler.maxAnisotropy);
            }
            return false;
        }
        index = m_samplerHeapOffset++;
        m_guestSamplerIndex.emplace(hostSampler.hash, index);

        // HostSamplerDesc's numeric fields are the D3D12 enums restated by
        // texture_decode.h (checked by the static_asserts at the top of this
        // file), so this is a straight field-for-field copy. `comparison` is
        // NOT a D3D12 field: it selects the filter enum, which DecodeGuestSampler-
        // State already chose (kD3D12FilterComparison* carry the 0x80 bit), so
        // there is nothing to translate here and nothing to drop.
        D3D12_SAMPLER_DESC sd = {};
        sd.Filter = static_cast<D3D12_FILTER>(hostSampler.filter);
        sd.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(hostSampler.addressU);
        sd.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(hostSampler.addressV);
        sd.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(hostSampler.addressW);
        sd.MipLODBias = hostSampler.mipLodBias;
        sd.MaxAnisotropy = hostSampler.maxAnisotropy;
        sd.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(hostSampler.comparisonFunc);
        // HostSamplerDesc::borderColor is ALREADY a FLOAT[4]
        // (texture_decode.h:533), not the Xenos BorderColor enum: the decoder
        // has already resolved kBorderBlack/kBorderWhite and refused the two
        // YCbCr blacks it cannot express (texture_decode.h:548-549), so this is
        // a straight field copy with nothing to translate and nothing to drop.
        for (UINT c = 0; c < 4; ++c) sd.BorderColor[c] = hostSampler.borderColor[c];
        sd.MinLOD = hostSampler.minLod;
        sd.MaxLOD = hostSampler.maxLod;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu(m_samplerHeap->GetCPUDescriptorHandleForHeapStart());
        cpu.ptr += static_cast<SIZE_T>(index) * m_samplerDescriptorSize;
        m_device->CreateSampler(&sd, cpu);

        samplerTableGpuOut = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        samplerTableGpuOut.ptr += static_cast<SIZE_T>(index) * m_samplerDescriptorSize;
    }
    return true;
}

bool D3D12Backend::BindGuestSampler(const native::HostSamplerDesc& hostSampler, ID3D12GraphicsCommandList* cl, bool& heapsBound, D3D12_GPU_DESCRIPTOR_HANDLE& samplerTableGpuOut) {
    if (cl == m_commandList.Get()) {
        return BindGuestSamplerInternal(hostSampler, cl, heapsBound, samplerTableGpuOut,
                                        s_boundSamplerTableGpu, s_boundRootSig);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE dummyBound = {};
    return BindGuestSamplerInternal(hostSampler, cl, heapsBound, samplerTableGpuOut,
                                    dummyBound, nullptr);
}

bool D3D12Backend::BindGuestSamplerInternal(const native::HostSamplerDesc& hostSampler,
                                            ID3D12GraphicsCommandList* cl, bool& heapsBound,
                                            D3D12_GPU_DESCRIPTOR_HANDLE& samplerTableGpuOut,
                                            D3D12_GPU_DESCRIPTOR_HANDLE& boundSamplerTableGpu,
                                            ID3D12RootSignature* boundRootSig) {
    if (!m_samplerHeap || !cl) return false;
    if (!InternGuestSampler(hostSampler, samplerTableGpuOut)) return false;

    static const bool s_noTexBindSkip = [] {
        const char* e = std::getenv("MCLA_NO_TEXBIND_SKIP");
        return e && e[0] == '1';
    }();
    const bool wasHeapsBound = heapsBound;
    // Bind BOTH heaps: the SRV table (root param 0) must stay accessible while
    // the sampler table is set, and D3D12 unbinds previously bound heaps when a
    // new SetDescriptorHeaps call names a different set.
    if (m_srvHeap) {
        if (!heapsBound) {
            ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get(), m_samplerHeap.Get() };
            cl->SetDescriptorHeaps(2, heaps);
            heapsBound = true;
        }
    } else {
        cl->SetDescriptorHeaps(1, m_samplerHeap.GetAddressOf());
        heapsBound = false;
    }
    if (heapsBound && boundRootSig == m_rootSignature.Get()) {
        if (s_noTexBindSkip || !wasHeapsBound ||
            boundSamplerTableGpu.ptr != samplerTableGpuOut.ptr) {
            cl->SetGraphicsRootDescriptorTable(1, samplerTableGpuOut);
            boundSamplerTableGpu = samplerTableGpuOut;
        }
    } else {
        boundSamplerTableGpu = {};
    }
    return true;
}

bool D3D12Backend::CreateTestRootSignature() {
    // Shared root signature for every pipeline (test fallback AND pipeline-cache
    // worker PSOs). It mirrors the binding contract of shader_translator's
    // generated HLSL:
    //   - root param 0: SRV descriptor table t0..t3 (t1D/t2D/t3D/tCube),
    //     visibility ALL (Xenos vertex shaders can tfetch too).
    //   - root param 1: SAMPLER descriptor table, s0, 1 descriptor,
    //     visibility ALL.
    //   - root param 2: root CBV at b0 (cbuffer ShaderConstants, 256 float4
    //     constants), visibility ALL (VS and PS share register b0).
    //
    // F-644: the sampler used to be a D3D12_STATIC_SAMPLER_DESC at s0 with
    // NumStaticSamplers = 1, which is why the draw path could not honour a
    // guest fetch constant: a static sampler is baked into the root signature at
    // create time, so clamp mode, filter, anisotropy, LOD bias and the border
    // colour of every Xenos sampler were discarded and the frame was sampled
    // clamp + linear + aniso 1. A DESCRIPTOR_TABLE is the only form D3D12 offers
    // for a per-draw sampler.
    //
    // The CBV therefore MOVES from root parameter 1 to root parameter 2. Both
    // this file and renderer/pipeline_cache.cpp's ResolveConstantBankBinding
    // name that index; they must change together or every real PSO binds a
    // descriptor table the command list never set. See the report.
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = kShaderTextureSlotCount;  // t0..t15
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE samplerRange = {};
    samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplerRange.NumDescriptors = GuestSamplerTableEnabled() ? renderer::kShaderSamplerCount : 1u;
    samplerRange.BaseShaderRegister = 0;
    samplerRange.RegisterSpace = 0;
    samplerRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParams[5] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &samplerRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 0;  // b0
    rootParams[2].Descriptor.RegisterSpace = 0;
    // F-932: both translated stages declare `cbuffer ShaderConstants :
    // register(b0)` (shader_translator.cpp), so b0 is split by VISIBILITY:
    // param 2 = the vertex bank, param 3 = the pixel bank.
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[3].Descriptor.ShaderRegister = 0;  // b0, pixel stage
    rootParams[3].Descriptor.RegisterSpace = 0;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // param 4 = root SRV t0,space1 (VS only), the raw guest VB as a ByteAddressBuffer.
    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[4].Descriptor.ShaderRegister = 0;
    rootParams[4].Descriptor.RegisterSpace = 1;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = _countof(rootParams);
    rootDesc.pParameters = rootParams;
    // F-644: zero static samplers. A static sampler at s0 AND a descriptor-table
    // range at s0 in the same root signature is a D3D12 root-signature
    // validation error (duplicate shader register), so the static one has to go,
    // not merely be shadowed.
    rootDesc.NumStaticSamplers = 0;
    rootDesc.pStaticSamplers = nullptr;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                             &signatureBlob, &errorBlob);
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: D3D12SerializeRootSignature failed with hr=0x{:08X} "
                       "(root params: 0=t0..t3, 1=s0 sampler table, 2=b0 CBV)",
                       static_cast<uint32_t>(hr));
        return false;
    }

    hr = m_device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
                                       signatureBlob->GetBufferSize(),
                                       IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateRootSignature failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    return true;
}

bool D3D12Backend::CreatePrecompiledRootSignature() {
    // Root signature matching tools/xenosrecomp/XenosRecomp/shader_common.h's
    // resource model, which every pre-compiled DXIL blob is built against:
    //
    //   Texture2D<float4>   g_Texture2DDescriptorHeap[] : register(t0, space0);  // unbounded
    //   Texture3D<float4>   g_Texture3DDescriptorHeap[] : register(t0, space1);  // unbounded
    //   TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2); // unbounded
    //   SamplerState        g_SamplerDescriptorHeap[]   : register(s0, space3);  // unbounded
    //   cbuffer VertexShaderConstants : register(b0, space4)
    //   cbuffer PixelShaderConstants  : register(b1, space4)
    //   cbuffer SharedConstants      : register(b2, space4)
    //
    // This is Step 6's first concrete piece. It does not replace the live
    // CreateTestRootSignature (which the runtime-translated path still uses);
    // it sits alongside it so pre-compiled PSOs can be built without touching
    // the boot path. Unbounded ranges use D3D12_DESCRIPTOR_RANGE_FLAG_
    // DESCRIPTORS_VOLATILE, which is what makes a UINT_MAX-sized range legal.
    //
    // IMPORTANT: the unbounded SRV/Sampler tables all bind at shader register 0
    // of their space, so each range's BaseShaderRegister is 0 and the space
    // distinguishes them. This mirrors the generated HLSL exactly.
    constexpr uint32_t kUnbounded = 0xFFFFFFFFu;

    D3D12_DESCRIPTOR_RANGE1 ranges[4] = {};
    // t0, space0 (Texture2D)
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = kUnbounded;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    // t0, space1 (Texture3D)
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = kUnbounded;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    // t0, space2 (TextureCube)
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = kUnbounded;
    ranges[2].BaseShaderRegister = 0;
    ranges[2].RegisterSpace = 2;
    ranges[2].OffsetInDescriptorsFromTableStart = 0;
    ranges[2].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    // s0, space3 (Sampler)
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[3].NumDescriptors = kUnbounded;
    ranges[3].BaseShaderRegister = 0;
    ranges[3].RegisterSpace = 3;
    ranges[3].OffsetInDescriptorsFromTableStart = 0;
    ranges[3].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    // Four descriptor tables: t0/space0, t0/space1, t0/space2, s0/space3.
    // All visible to all stages (Xenos vertex shaders can tfetch).
    D3D12_ROOT_PARAMETER1 rootParams[7] = {};
    for (int i = 0; i < 4; i++) {
        rootParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[i].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        rootParams[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // b0, space4 (vertex constants) - root CBV
    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[4].Descriptor.ShaderRegister = 0;
    rootParams[4].Descriptor.RegisterSpace = 4;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // b1, space4 (pixel constants) - root CBV
    rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[5].Descriptor.ShaderRegister = 1;
    rootParams[5].Descriptor.RegisterSpace = 4;
    rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // b2, space4 (shared constants) - root CBV
    rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[6].Descriptor.ShaderRegister = 2;
    rootParams[6].Descriptor.RegisterSpace = 4;
    rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC1 rootDesc = {};
    rootDesc.NumParameters = _countof(rootParams);
    rootDesc.pParameters = rootParams;
    rootDesc.NumStaticSamplers = 0;
    rootDesc.pStaticSamplers = nullptr;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc = {};
    versionedDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionedDesc.Desc_1_1 = rootDesc;

    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&versionedDesc, &signatureBlob, &errorBlob);
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreatePrecompiledRootSignature serialize failed "
                       "hr=0x{:08X}", static_cast<uint32_t>(hr));
        if (errorBlob) {
            MCLA_LOG_ERROR("  {}", static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = m_device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
                                       signatureBlob->GetBufferSize(),
                                       IID_PPV_ARGS(&m_precompiledRootSignature));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreatePrecompiledRootSignature create failed "
                       "hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    MCLA_LOG_INFO("D3D12Backend: precompiled root signature created "
                  "(t0/space0..2, s0/space3, b0..b2/space4)");
    return true;
}

bool D3D12Backend::CreatePrecompiledDescriptorHeaps() {
    // Shader-visible descriptor heaps backing the pre-compiled root signature's
    // unbounded ranges:
    //   t0/space0, t0/space1, t0/space2  -> one CBV_SRV_UAV heap (SRVs of any
    //                                      dimension live in the same heap type)
    //   s0/space3                        -> one SAMPLER heap
    //
    // The b0/b1/b2 space4 CBVs are ROOT CBVs (SetGraphicsRootConstantBufferView),
    // so they are not heap entries -- only the texture SRVs and samplers live
    // here. This is Step 6 piece 2: it gives pre-compiled PSOs a concrete,
    // bindable shader-visible heap. It is sized for the guest's texture count and
    // prefilled so every bound descriptor table entry is initialized, per the
    // D3D12 rule that all bound descriptors must be valid.
    //
    // Sized generously (not "unbounded" in the D3D12 sense -- the shader can
    // index any slot below NumDescriptors; a real unbounded range needs a heap
    // large enough for the working set). Prefill matches CreateSrvHeap's
    // pattern (null SRVs) so an unbound slot reads as a defined sample.

    // --- CBV/SRV/UAV heap (textures) ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = kPrecompiledSrvHeapSize;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_precompiledSrvHeap));
        if (FAILED(hr)) {
            MCLA_LOG_ERROR("D3D12Backend: CreateDescriptorHeap (precompiled CBV/SRV/UAV) "
                           "failed hr=0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }
        m_precompiledSrvDescriptorSize =
            m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
        nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE base = m_precompiledSrvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < kPrecompiledSrvHeapSize; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = base;
            h.ptr += static_cast<size_t>(i) * m_precompiledSrvDescriptorSize;
            m_device->CreateShaderResourceView(nullptr, &nullSrv, h);
        }
    }

    // --- Sampler heap ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = kPrecompiledSamplerHeapSize;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_precompiledSamplerHeap));
        if (FAILED(hr)) {
            MCLA_LOG_ERROR("D3D12Backend: CreateDescriptorHeap (precompiled Sampler) "
                           "failed hr=0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }
        m_precompiledSamplerDescriptorSize =
            m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

        // Slot 0 = neutral sampler (mirrors CreateSamplerHeap so an unbound
        // sampler slot reads deterministically).
        D3D12_SAMPLER_DESC neutral = {};
        neutral.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        neutral.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        neutral.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        neutral.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        neutral.MipLODBias = 0.0f;
        neutral.MaxAnisotropy = 1;
        neutral.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        neutral.BorderColor[0] = 0.0f;
        neutral.BorderColor[1] = 0.0f;
        neutral.BorderColor[2] = 0.0f;
        neutral.BorderColor[3] = 0.0f;
        neutral.MinLOD = 0.0f;
        neutral.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE slot0(m_precompiledSamplerHeap->GetCPUDescriptorHandleForHeapStart());
        m_device->CreateSampler(&neutral, slot0);
    }

    MCLA_LOG_INFO("D3D12Backend: precompiled descriptor heaps created "
                  "(SRV {} (null-filled), Sampler {} (slot0 neutral))",
                  kPrecompiledSrvHeapSize, kPrecompiledSamplerHeapSize);
    return true;
}

bool D3D12Backend::CreatePrecompiledSharedConstantsBuffer() {
    // Upload-heap buffer backing the pre-compiled root signature's b2/space4
    // (SharedConstants) root CBV. Persistently mapped so per-draw index writes
    // are a memcpy, not a re-map. Sized to the SharedConstants layout from
    // shader_common.h (DXIL arm):
    //   16 samplers * 4 uint (Tex2D/Tex3D/TexCube/Sampler) = 64 uint = 256 bytes
    //   g_Booleans, g_SwappedTexcoords, g_HalfPixelOffset, g_AlphaThreshold
    //   = 4 uint = 16 bytes (at +256)
    //   total 272 bytes, rounded up to the 256-byte CBV alignment = 512.
    //
    // Initialized to all zeros: every descriptor index = 0 (null SRV slot) and
    // every SharedConstant default = 0. A per-draw writer (piece 3's consumer)
    // overwrites the indices from the bound texture set; until that is wired
    // into the draw path the buffer stays neutral, which is safe (null SRV
    // samples a defined value).
    constexpr size_t kSharedConstantsSize = 512;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = kSharedConstantsSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                                                  &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&m_precompiledSharedConstantsBuffer));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateCommittedResource (precompiled SharedConstants) "
                       "failed hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    D3D12_RANGE readRange = { 0, 0 };  // CPU will not read back
    hr = m_precompiledSharedConstantsBuffer->Map(0, &readRange,
                                                reinterpret_cast<void**>(&m_precompiledSharedConstantsMap));
    if (FAILED(hr) || m_precompiledSharedConstantsMap == nullptr) {
        MCLA_LOG_ERROR("D3D12Backend: Map (precompiled SharedConstants) failed "
                       "hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    // Neutral initial contents.
    memset(m_precompiledSharedConstantsMap, 0, kSharedConstantsSize);
    m_precompiledSharedConstantsGpu = m_precompiledSharedConstantsBuffer->GetGPUVirtualAddress();

    MCLA_LOG_INFO("D3D12Backend: precompiled SharedConstants buffer created "
                  "({} bytes, GPU VA 0x{:X})", kSharedConstantsSize,
                  static_cast<unsigned long long>(m_precompiledSharedConstantsGpu));
    return true;
}

bool D3D12Backend::CreatePrecompiledTestPipeline() {
    // Proof-of-concept substitution: load two cached DXIL blobs (one VS, one PS)
    // from the pre-compiled cache and build a PSO against the pre-compiled root
    // signature. This is the first end-to-end exercise of
    //   cache hash -> DXIL bytes -> CreateGraphicsPipelineState
    // with the shader_common.h-shaped root signature. It does not yet route
    // draws through the PSO; it proves the substitution path is viable.
    using namespace mcla::renderer;

    PrecompiledShaderView vsView{}, psView{};
    const uint64_t vsHash = 0x54D610C6CFDFA7D1ull;  // guest VS (derived inputs)
    const uint64_t psHash = 0x7F80F81B7D9D6C81ull;  // guest PS (tfetch2D * r1)

    if (!FindPrecompiledShader(vsHash, vsView) || !vsView.dxil || vsView.dxilSize == 0) {
        MCLA_LOG_WARN("D3D12Backend: precompiled test PSO: VS hash 0x{:016X} not in cache",
                      vsHash);
        return false;
    }
    if (!FindPrecompiledShader(psHash, psView) || !psView.dxil || psView.dxilSize == 0) {
        MCLA_LOG_WARN("D3D12Backend: precompiled test PSO: PS hash 0x{:016X} not in cache",
                      psHash);
        return false;
    }

    // Input layout matching the derived VS inputs (Position0 + TexCoord0).
    // Derived inputs use DeclUsage::Position -> "POSITION" and
    // DeclUsage::TexCoord -> "TEXCOORD" (see ExportRegisterToSemantic in
    // xenos_shader_ir.cpp). Formats are the Xenos vertex-element defaults.
    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_precompiledRootSignature.Get();
    psoDesc.VS = { vsView.dxil, vsView.dxilSize };
    psoDesc.PS = { psView.dxil, psView.dxilSize };
    psoDesc.InputLayout = { inputElements, _countof(inputElements) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.NumRenderTargets = 1;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < 8; ++i) {
        psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_precompiledTestPipeline));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateGraphicsPipelineState(precompiled) failed "
                       "hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    MCLA_LOG_INFO("D3D12Backend: precompiled test PSO created (VS 0x{:016X}, PS 0x{:016X})",
                  vsHash, psHash);

    // Create the active draw PSO: 0x015E705A375C3C99 (36-byte stride VS) + 0x7F80F81B7D9D6C81 (PS)
    // with input layout matching DRAW-CP-LAYOUT #2:
    //   elem 0: POSITION0 off=0  dxgi=6 (R32G32B32_FLOAT)
    //   elem 1: TEXCOORD1 off=24 dxgi=28 (R8G8B8A8_UNORM)
    //   elem 2: TEXCOORD0 off=28 dxgi=16 (R32G32_FLOAT)
    const uint64_t drawVsHash = 0x015E705A375C3C99ull;
    PrecompiledShaderView drawVsView{};
    if (FindPrecompiledShader(drawVsHash, drawVsView) && drawVsView.dxil && drawVsView.dxilSize > 0) {
        D3D12_INPUT_ELEMENT_DESC drawInputElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R8G8B8A8_UNORM,     0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC drawPsoDesc = psoDesc;
        drawPsoDesc.VS = { drawVsView.dxil, drawVsView.dxilSize };
        drawPsoDesc.InputLayout = { drawInputElements, _countof(drawInputElements) };
        // Enable alpha blending for UI draws (SrcAlpha / InvSrcAlpha)
        drawPsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        drawPsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        drawPsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        drawPsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        drawPsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        drawPsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        drawPsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

        HRESULT dhr = m_device->CreateGraphicsPipelineState(&drawPsoDesc, IID_PPV_ARGS(&m_precompiledDrawPipeline));
        if (SUCCEEDED(dhr)) {
            MCLA_LOG_INFO("D3D12Backend: precompiled draw PSO created (VS 0x{:016X}, PS 0x{:016X}, alpha-blend enabled)",
                          drawVsHash, psHash);
        } else {
            MCLA_LOG_WARN("D3D12Backend: CreateGraphicsPipelineState(precompiled draw) failed hr=0x{:08X}",
                          static_cast<uint32_t>(dhr));
        }
    }
    return true;
}

void D3D12Backend::UpdatePrecompiledSharedConstants(const uint32_t* indices, size_t count) {
    if (!m_precompiledSharedConstantsMap || !indices || count == 0) return;
    // Layout: up to 16 samplers * 4 uints = 64 uints = 256 bytes.
    // Each sampler slot has 4 indices: Texture2D, Texture3D, TextureCube, Sampler.
    // The indices array provides Texture2D descriptor indices per sampler slot.
    size_t copyCount = (count > 64) ? 64 : count;
    memcpy(m_precompiledSharedConstantsMap, indices, copyCount * sizeof(uint32_t));
}

bool D3D12Backend::BindPrecompiledDrawResources(ID3D12GraphicsCommandList* cmdList) {
    if (!cmdList || !m_precompiledRootSignature || !m_precompiledSrvHeap ||
        !m_precompiledSamplerHeap || m_precompiledSharedConstantsGpu == 0) {
        return false;
    }

    // Bind heaps
    ID3D12DescriptorHeap* heaps[] = {
        m_precompiledSrvHeap.Get(),
        m_precompiledSamplerHeap.Get()
    };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    // Set precompiled root signature
    cmdList->SetGraphicsRootSignature(m_precompiledRootSignature.Get());
    InvalidateBoundState();

    // Root parameters:
    // param 0: t0, space0 (Texture2D descriptor table)
    // param 1: t0, space1 (Texture3D descriptor table)
    // param 2: t0, space2 (TextureCube descriptor table)
    // param 3: s0, space3 (Sampler descriptor table)
    // param 4: b0, space4 (VertexShaderConstants CBV)
    // param 5: b1, space4 (PixelShaderConstants CBV)
    // param 6: b2, space4 (SharedConstants CBV)
    D3D12_GPU_DESCRIPTOR_HANDLE srvStart = m_precompiledSrvHeap->GetGPUDescriptorHandleForHeapStart();
    cmdList->SetGraphicsRootDescriptorTable(0, srvStart);
    cmdList->SetGraphicsRootDescriptorTable(1, srvStart);
    cmdList->SetGraphicsRootDescriptorTable(2, srvStart);

    D3D12_GPU_DESCRIPTOR_HANDLE samplerStart = m_precompiledSamplerHeap->GetGPUDescriptorHandleForHeapStart();
    cmdList->SetGraphicsRootDescriptorTable(3, samplerStart);

    // Bind SharedConstants CBV at param 6
    cmdList->SetGraphicsRootConstantBufferView(6, m_precompiledSharedConstantsGpu);

    static bool s_loggedBindOnce = false;
    if (!s_loggedBindOnce) {
        s_loggedBindOnce = true;
        MCLA_LOG_INFO("D3D12Backend: BindPrecompiledDrawResources successfully set root signature, descriptor tables (params 0..3), and SharedConstants CBV (param 6)");
    }
    return true;
}

bool D3D12Backend::CreateTestPipeline() {
    // Input layout matching the Phase 3 test shader (POSITION float3, COLOR float4).
    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { GetTestVsBlob(), GetTestVsBlobSize() };
    psoDesc.PS = { GetTestPsBlob(), GetTestPsBlobSize() };
    psoDesc.InputLayout = { inputElements, _countof(inputElements) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.NumRenderTargets = 1;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < 8; ++i) {
        psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_testPipeline));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateGraphicsPipelineState(test) failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    MCLA_LOG_INFO("D3D12Backend: Phase 3 test pipeline created (DXIL {} vs / {} ps bytes)",
                GetTestVsBlobSize(), GetTestPsBlobSize());
    return true;
}

// Create mipmap generation compute pipeline (root signature + PSO)
bool D3D12Backend::CreateMipGenPipeline() {
    // Root signature: SRV (t0) + UAV (u0) + CBV (b0) for params
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;  // t0
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;  // u0
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParams[3] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 0;  // b0
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = _countof(rootParams);
    rootDesc.pParameters = rootParams;
    rootDesc.NumStaticSamplers = 0;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                             &signatureBlob, &errorBlob);
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: MipGen SerializeRootSignature failed hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    hr = m_device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
                                       signatureBlob->GetBufferSize(),
                                       IID_PPV_ARGS(&m_mipGenRootSignature));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: MipGen CreateRootSignature failed hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    MCLA_LOG_INFO("D3D12Backend: MipGen root signature created");
    return true;
}

// F-641: the DXC compile of the mipgen CS, split out of GenerateMipmaps so the
// LoadLibrary + fxc pass happens once at Initialize (D3D12Backend::Initialize
// calls this) instead of between MapUpload and DrawIndexedInstanced on
// whichever draw first needed a chain. Idempotent: the second call is a single
// pointer test.
bool D3D12Backend::EnsureMipGenPipeline() {
    if (m_mipGenPipeline) return true;
    if (!m_device) return false;

    if (!m_mipGenRootSignature && !CreateMipGenPipeline()) return false;

    const char* csHlsl = GetMipGenCsHlsl();
    if (!csHlsl || !*csHlsl) {
        MCLA_LOG_ERROR("D3D12Backend: EnsureMipGenPipeline missing CS HLSL source");
        return false;
    }

    renderer::DxcRuntime dxc;
    std::string usedDir, error;
    if (!dxc.Load("", usedDir, error)) {
        MCLA_LOG_ERROR("D3D12Backend: EnsureMipGenPipeline failed to load DXC runtime: {}", error);
        return false;
    }

    std::vector<uint8_t> csBlob;
    if (!dxc.Compile(csHlsl, "main", "cs_6_0", csBlob, error)) {
        MCLA_LOG_ERROR("D3D12Backend: EnsureMipGenPipeline CS compilation failed: {}", error);
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_mipGenRootSignature.Get();
    psoDesc.CS = { csBlob.data(), csBlob.size() };
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_mipGenPipeline));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: EnsureMipGenPipeline CreateComputePipelineState failed hr=0x{:08X}",
                       static_cast<uint32_t>(hr));
        return false;
    }
    MCLA_LOG_INFO("D3D12Backend: MipGen compute pipeline created (CS blob {} bytes)", csBlob.size());
    return true;
}

// Generate mipmaps using compute shader downsampling
bool D3D12Backend::GenerateMipmaps(ID3D12Resource* texture, uint32_t mipLevels) {
    if (!texture || mipLevels <= 1) {
        MCLA_LOG_WARN("D3D12Backend: GenerateMipmaps invalid args (texture={}, mipLevels={})",
                    texture != nullptr, mipLevels);
        return false;
    }

    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        MCLA_LOG_ERROR("D3D12Backend: GenerateMipmaps texture lacks ALLOW_UNORDERED_ACCESS flag");
        return false;
    }
    if (desc.MipLevels < mipLevels) {
        MCLA_LOG_ERROR("D3D12Backend: GenerateMipmaps texture has {} mips, requested {}",
                     desc.MipLevels, mipLevels);
        return false;
    }

    // F-641: the mipgen PSO is built by EnsureMipGenPipeline, which
    // Initialize calls. Reaching here without one means the warm-up failed, and
    // that is a refusal with a reason rather than a 300 ms compile in the middle
    // of a frame.
    if (!m_mipGenPipeline && !EnsureMipGenPipeline()) {
        MCLA_LOG_ERROR("D3D12Backend: GenerateMipmaps refused: no mipgen PSO (init warm-up failed)");
        return false;
    }

    // Create SRV heap for source mips (read) and UAV heap for destination mips (write)
    // We'll use the existing SRV heap for source and create UAVs for each mip level
    // For simplicity, we'll use the same descriptor heap with different offsets

    D3D12_DESCRIPTOR_HEAP_DESC srvUavHeapDesc = {};
    srvUavHeapDesc.NumDescriptors = mipLevels * 2;  // SRV + UAV per mip
    srvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvUavHeap;
    HRESULT hr = m_device->CreateDescriptorHeap(&srvUavHeapDesc, IID_PPV_ARGS(&srvUavHeap));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: GenerateMipmaps CreateDescriptorHeap failed hr=0x{:08X}",
                     static_cast<uint32_t>(hr));
        return false;
    }

    uint32_t descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Create SRVs and UAVs for each mip level
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srvHandles(mipLevels);
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> uavHandles(mipLevels);

    D3D12_CPU_DESCRIPTOR_HANDLE srvStart = srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE uavStart;
    uavStart.ptr = srvStart.ptr + static_cast<SIZE_T>(mipLevels) * descriptorSize;

    for (uint32_t i = 0; i < mipLevels; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = i;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.PlaneSlice = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = srvStart;
        srvHandle.ptr += static_cast<SIZE_T>(i) * descriptorSize;
        m_device->CreateShaderResourceView(texture, &srvDesc, srvHandle);
        srvHandles[i] = srvHandle;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = desc.Format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = i;
        uavDesc.Texture2D.PlaneSlice = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = uavStart;
        uavHandle.ptr += static_cast<SIZE_T>(i) * descriptorSize;
        m_device->CreateUnorderedAccessView(texture, nullptr, &uavDesc, uavHandle);
        uavHandles[i] = uavHandle;
    }

    // (MipGenParams is declared inside the dispatch loop below; the copy here
    // was dead and shadowed it.)

    // Wait for GPU to be ready for this frame's upload region
    if (!WaitForCurrentFrameGpu()) return false;

    // Begin frame if not already in frame
    if (!m_inFrame) {
        if (!BeginFrame()) return false;
    }

    // Transition all mips to appropriate states and generate each level.
    //
    // F-641: every transition below goes through TransitionResource with an
    // explicit subresource, and TransitionResource now reads the TRACKED state
    // of THAT subresource. The old code asserted D3D12_RESOURCE_STATE_COMMON as
    // the prior state of mip i-1 and mip i on every iteration, which is only
    // true the first time; on the second pass D3D12 saw
    // COMMON->NON_PIXEL_SHADER_RESOURCE for a mip that was already
    // PIXEL_SHADER_RESOURCE, and the tracker's single-state-per-resource model
    // could not even name which mip it was talking about.
    for (uint32_t i = 1; i < mipLevels; ++i) {
        uint32_t srcMip = i - 1;
        uint32_t dstMip = i;

        // Calculate dimensions. A mip's texel extent halves; for a block-
        // compressed format the CS samples texels so this stays in texels.
        uint32_t srcWidth = std::max<uint32_t>(1, desc.Width >> srcMip);
        uint32_t srcHeight = std::max<uint32_t>(1, desc.Height >> srcMip);
        uint32_t dstWidth = std::max<uint32_t>(1, desc.Width >> dstMip);
        uint32_t dstHeight = std::max<uint32_t>(1, desc.Height >> dstMip);

        // Begin frame if needed
        if (!m_inFrame) {
            if (!BeginFrame()) return false;
        }

        // src mip -> SRV-readable, dst mip -> UAV, each against tracked state.
        TransitionResource(texture, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, srcMip);
        TransitionResource(texture, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, dstMip);

        // Bind pipeline and descriptors
        m_commandList->SetPipelineState(m_mipGenPipeline.Get());
        m_commandList->SetComputeRootSignature(m_mipGenRootSignature.Get());

        // The compute descriptor tables below reference srvUavHeap, so that heap
        // must be the active descriptor heap when they are set (and at dispatch
        // time). Without this bind the tables reference an unbound heap -- a
        // D3D12 validation error and an undefined read.
        ID3D12DescriptorHeap* mipGenHeaps[] = { srvUavHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, mipGenHeaps);
        InvalidateBoundState();

        // Bind SRV table (src mip)
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle = srvUavHeap->GetGPUDescriptorHandleForHeapStart();
        srvGpuHandle.ptr += static_cast<SIZE_T>(srcMip) * descriptorSize;
        m_commandList->SetComputeRootDescriptorTable(0, srvGpuHandle);

        // Bind UAV table (dst mip)
        D3D12_GPU_DESCRIPTOR_HANDLE uavGpuHandle = srvUavHeap->GetGPUDescriptorHandleForHeapStart();
        uavGpuHandle.ptr += static_cast<SIZE_T>(mipLevels + dstMip) * descriptorSize;
        m_commandList->SetComputeRootDescriptorTable(1, uavGpuHandle);

        // Bind constants via CBV (root param 1 of the MIPGEN root signature,
        // which is its own signature and is unrelated to the graphics root
        // parameter renumbering in CreateTestRootSignature).
        struct MipGenParams {
            uint32_t srcWidth;
            uint32_t srcHeight;
            uint32_t dstWidth;
            uint32_t dstHeight;
            uint32_t srcMipLevel;
            uint32_t padding[3];
        } params = { srcWidth, srcHeight, dstWidth, dstHeight, srcMip, {0, 0, 0} };

        m_commandList->SetComputeRoot32BitConstants(1, sizeof(MipGenParams) / 4, &params, 0);

        // Dispatch
        uint32_t dispatchX = (dstWidth + 7) / 8;
        uint32_t dispatchY = (dstHeight + 7) / 8;
        m_commandList->Dispatch(dispatchX, dispatchY, 1);

        // UAV barrier for this mip
        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = texture;
        (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &uavBarrier);

        // dst mip -> SRV-readable so the NEXT iteration can read it as srcMip.
        TransitionResource(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, dstMip);
    }

    // Final transition: every mip to the combined PS|non-PS shader-resource
    // state a graphics pass can read. Levels that GenerateMipmaps did not touch
    // (mip 0) are transitioned from their tracked state, which after
    // CreateDecodedTexture is PIXEL_SHADER_RESOURCE -- so this emits no barrier
    // for it rather than a bogus COMMON->... one.
    for (uint32_t i = 0; i < mipLevels; ++i) {
        TransitionResource(texture, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           i);
    }

    if (m_srvHeap && m_samplerHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get(), m_samplerHeap.Get() };
        m_commandList->SetDescriptorHeaps(2, heaps);
    } else if (m_srvHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
    }

    InvalidateBoundState();
    RetireTarget(std::move(srvUavHeap));  // the recorded dispatches reference it until execution
    MCLA_LOG_INFO("D3D12Backend: Generated {} mip levels for texture", mipLevels);
    return true;
}

bool D3D12Backend::CreatePipelineFromDxil(const std::vector<uint8_t>& vsDxil,
                                          const std::vector<uint8_t>& psDxil,
                                          const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout,
                                          const renderer::PipelineState& state,
                                          Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPso) {
    if (vsDxil.empty() || psDxil.empty()) {
        MCLA_LOG_ERROR("D3D12Backend: CreatePipelineFromDxil: empty DXIL blob");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vsDxil.data(), vsDxil.size() };
    psoDesc.PS = { psDxil.data(), psDxil.size() };
    psoDesc.InputLayout = { inputLayout.data(), static_cast<UINT>(inputLayout.size()) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // RTV formats from PipelineState
    for (uint32_t i = 0; i < renderer::kMaxRenderTargets; ++i) {
        if (state.targetFormats[i] != 0) {
            psoDesc.RTVFormats[i] = static_cast<DXGI_FORMAT>(state.targetFormats[i]);
        }
    }
    psoDesc.NumRenderTargets = 1;  // default; could be extended
    psoDesc.SampleDesc.Count = state.sampleCount;
    psoDesc.SampleMask = UINT_MAX;

    // Rasterizer state
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // Blend state
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < 8; ++i) {
        psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    // Depth/stencil state
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: CreateGraphicsPipelineState(translated) failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    MCLA_LOG_INFO("D3D12Backend: Live pipeline created from DXIL (VS={} bytes, PS={} bytes)", vsDxil.size(), psDxil.size());
    return true;
}

// Phase 3 test draw: an indexed triangle whose colors pulse per frame.
// Exercises the upload arena, index/vertex buffer binding, viewport, and
// primitive topology on the native path. The draw body is shared with the
// captured-geometry path via DrawDynamicMesh so both go through identical
// command-list handling.
bool D3D12Backend::DrawTestMeshedTriangle(uint32_t frame) {
    // Deterministic rotating color so the frame visibly changes.
    const float t = static_cast<float>(frame) * 0.1f;
    const float cr = (std::sin(t) + 1.0f) * 0.5f;
    const float cg = (std::sin(t + 2.0f) + 1.0f) * 0.5f;
    const float cb = (std::sin(t + 4.0f) + 1.0f) * 0.5f;

    // A meshed quad: 4 vertices, 6 indices (two triangles).
    struct TestVertex { float pos[3]; float color[4]; };
    const TestVertex vertices[4] = {
        { { -0.8f, -0.8f, 0.0f }, { cr, cg, cb, 1.0f } },
        { {  0.8f, -0.8f, 0.0f }, { cg, cb, cr, 1.0f } },
        { {  0.8f,  0.8f, 0.0f }, { cb, cr, cg, 1.0f } },
        { { -0.8f,  0.8f, 0.0f }, { cr, cb, cg, 1.0f } },
    };
    const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };

    DynamicMeshDesc desc = {};
    desc.vertexBytes = vertices;
    desc.vertexBytesSize = sizeof(vertices);
    desc.vertexStride = sizeof(TestVertex);  // 28 bytes
    desc.vertexCount = 4;
    desc.indexed = true;
    desc.indexFormat = DXGI_FORMAT_R16_UINT;
    desc.indexCount = 6;

    // Reuse the static cached index buffer (uploaded once at Initialize).
    const ResourceKey ibKey = { ResourceKind::Buffer, 0 /*synthetic test addr*/,
                                sizeof(indices), DXGI_FORMAT_R16_UINT, 1 };
    ResourceEntry ibEntry;
    if (m_resourceCache.Find(ibKey, ibEntry)) {
        desc.cachedIndexGpu = ibEntry.handle;
        desc.cachedIndexBytesSize = sizeof(indices);
        m_stats.cacheHits++;
    } else {
        // Should not happen (initialized before draw), but recover safely by
        // falling back to a per-frame upload of the same bytes.
        desc.indexBytes = indices;
        desc.indexBytesSize = sizeof(indices);
        m_stats.cacheMisses++;
    }

    // F-653: the test PSO is passed EXPLICITLY here rather than resolved by
    // DrawDynamicMesh. This is the sanctioned use of it -- the caller wants the
    // test pipeline, says so, and the draw is bounded (one quad, the first few
    // frames only). It is not a substitution, and keeping it explicit is what
    // lets the no-pipeline entry point refuse.
    const bool ok = DrawDynamicMeshWithPipeline(desc, m_testPipeline.Get());
    if (frame <= 3) {
        MCLA_LOG_INFO("D3D12Backend: DrawTestMeshedTriangle frame={} present_hr=0x{:08X}",
                    frame, static_cast<uint32_t>(m_lastPresentHr));
    }
    return ok;
}

// Generic dynamic-geometry draw. Uploads the caller's vertex bytes (and index
// bytes when the caller has no cached IB) into the per-frame upload arena and
// issues one indexed/non-indexed draw. The caller is responsible for producing
// host data from validated captures; the backend never interprets guest
// layouts. The vertex stride must match the test PSO's input layout
// (Position:float3 + Color:float4 = 28 bytes) Ã¢â‚¬ï¿½? anything else is refused so we
// never draw with a mismatched layout.
// F-653: THIS IS NO LONGER A PASS-THROUGH TO THE TEST PSO.
//
// It used to be:
//     bool D3D12Backend::DrawDynamicMesh(const DynamicMeshDesc& desc) {
//         return DrawDynamicMeshWithPipeline(desc, m_testPipeline.Get());
//     }
// which meant that any caller with no pipeline of its own got the fixed test
// PSO -- and since DrawDynamicMeshWithPipeline now REFUSES a null pipeline
// rather than substituting one (see its body), the two together were the whole
// bypass. One of the two had to go, and it is this one, because the honest
// behaviour for a draw with no pipeline is to refuse it.
//
// WHAT THIS COSTS, MEASURED RATHER THAN GUESSED. Three call sites reach this
// function, and they are not equivalent:
//   - d3d12_backend.cpp, DrawTestMeshedTriangle: a TEST draw, and it now passes
//     the test PSO explicitly at its own call site, which is a caller choosing a
//     PSO rather than this function substituting one. Unaffected.
//   - render_thread.cpp:937 and :2042: real guest draws, taken on the paths
//     where no pipeline was derived (render_thread.cpp:2033-2041 says so
//     outright: "there is no longer any path that reaches this line with a
//     program it could not translate"). Those draws now REFUSE, counted as
//     `no-pipeline-bypass-removed`. They were previously being drawn through a
//     28-byte POSITION float3 + COLOR float4 layout the guest never declared, so
//     this trades untraceable wrong pixels for a countable absence -- but it IS
//     a behaviour change, and it is a visible one, and render_thread.cpp is not
//     this file. Wiring those two sites to a real PSO is the follow-up and it
//     belongs to whoever owns render_thread.cpp.
//   - render_thread.cpp:2099: the host marker-quad overlay, which will also
//     begin refusing for the same reason.
//
// The alternative -- keeping the substitution and only deleting the duplicate --
// would have left the bypass in place through the other door, which is the
// outcome the charter forbids.
bool D3D12Backend::DrawDynamicMesh(const DynamicMeshDesc& desc) {
    (void)desc;
    const uint64_t n = ++m_noPipelineBypassRefusals;
    if (ShouldLogRefusal(n)) {
        MCLA_LOG_WARN("D3D12Backend: DrawDynamicMesh refused #{} -- F-653: this "
                      "entry no longer resolves to the test PSO. A draw with no "
                      "pipeline is a refusal, not a substitution. Callers that "
                      "want the test PSO must pass it explicitly; guest draws must "
                      "pass a cache PSO.",
                      n);
    }
    return false;
}

// 2026-10-02: the BODY for the declaration at d3d12_backend.h:184-185. The
// previous session wrote the declaration and the three call sites
// (:2734 / :2744 / :2805) and never this function, so the tree compiled all 14
// TUs and then failed at the link on one undefined symbol. MEASURED, not
// inferred: the qualified form `D3D12Backend::RecordSilentDrawReturn` had ZERO
// occurrences anywhere under src/ before this edit.
//
// WHAT IT REPORTS, and only that. `gate` is the stable token the call site
// already had in hand. `a`/`b`/`c` are that gate's OWN inputs, copied verbatim
// at the call site and NOT interpreted here (F-609: if this row had to guess
// what a/b/c mean it would invent the fault it exists to name). The per-gate
// meaning, measured at each call site:
//   device-not-initialised : none passed; a=b=c=0, the header's "0 where
//                            meaningless"
//   no-vertex-data        : a = (desc.vertexBytes ? 1u : 0u),
//                            b = desc.vertexBytesSize, c = desc.vertexCount
//   no-index-data         : a = (desc.indexed ? 1u : 0u),
//                            b = desc.indexBytesSize, c = desc.indexCount
// `returned=false` is not a claim about intent: all three call sites carry a
// literal `return false` on the line immediately after the call (:2735, :2747,
// :2807), so false is what every caller actually returned. It is stated as a
// literal because a 3-of-3 measurement is not a prediction.
//
// BOUNDING. `n` is incremented UNCONDITIONALLY, before the gate. That is the
// F-649 rule at :211-214 -- "the throttle gates the TEXT, never the count,
// because a suppressed row that is not counted is how a soak reads 'nothing
// happened' out of something that happened 616,824 times". The TEXT is
// throttled by ShouldLogRefusal(), the helper this file already uses at 31
// other refusal sites (:223-227): first kRefuseLogFirst=8, then every
// kRefuseLogEvery=200th. A draw path that refused on every frame would
// otherwise be a per-frame log line, which is the LOG-QUALITY poisoning F-054
// records. Chosen over the gpu_device.cpp kR3Head/kR3RowStride census shape
// because this file has exactly one bounding idiom and this is it; and chosen
// over the sibling PC-PSO-BOUND row's inlined `n <= 8 || (n % 200) == 0`
// (:3091) because the two are numerically identical today and the named
// helper survives a change to the constants.
//
// NEW ROW NAME, NO EXISTING FORMAT TOUCHED. `D3D12-DRAW-RETURN` appeared in
// zero of this file's 135 MCLA_LOG_ format literals before this edit, so
// nothing is appended to and none of the 244 markers tools/soak_census.py
// tracks changes shape.
void D3D12Backend::RecordSilentDrawReturn(const char* gate, uint32_t a,
                                          uint32_t b, uint32_t c) {
    const uint64_t n = ++m_silentDrawReturns;
    if (!ShouldLogRefusal(n)) {
        return;
    }
    MCLA_LOG_INFO("D3D12-DRAW-RETURN #{} gate={} a={} b={} c={} returned=false -- "
                  "a/b/c are the refusing gate's own inputs, copied verbatim at the "
                  "call site and not interpreted; device-not-initialised passes "
                  "none (0 where meaningless), no-vertex-data passes "
                  "(vertexBytes!=0, vertexBytesSize, vertexCount), no-index-data "
                  "passes (indexed, indexBytesSize, indexCount). Counted on every "
                  "refusal, printed for the first 8 and every 200th.",
                  n, gate, a, b, c);
}

// 2026-10-02: the bound-render-target format, and the draw's own output.
//
// WHY THIS FILE HAD NO PIXEL MEASUREMENT AT ALL. build/w696.log (sha256
// 3D21DE0E..7F1893, 55,669 lines) carries `DRAW-CP-DRAWN #1` at 03:30:04.948
// and `#2` at 03:30:04.961, and its two `PRESENT-FB sample` rows are
// timestamped 03:30:04.926 and 03:30:04.927 -- 21 ms and 34 ms BEFORE the
// draws. `FRAME-OWNER owner=draw` appears 0 times and `D3D12Backend: draw #N
// issued through a cache-produced PSO` 0 times, so the only two rows that
// look like a closed draw chain are not evidence that a draw call was ever
// issued, and no line in the tree could report what a draw PRODUCED. The
// two functions below are the minimum that makes the next soak able to
// answer the question instead of re-deriving it.

// F-727: the draw-path ordering flag that RecordDrawReadback samples to report
// copyAfterDraw= as an OBSERVATION rather than a restatement of where this
// variable happens to sit. Disarmed at the top of the draw path, armed again
// immediately after the draw call.
//
// Declared HERE, above its first use, because a file-scope static used above its
// own definition does not compile. Its full rationale, and its two companion
// statics s_readbackCopyAfterDraw / s_readbackDrawSeq, are declared in the block
// immediately above RecordDrawReadback further down this file.
//
// The ordering this flag exists to attest is the F-727 defect: the readback copy
// used to be recorded ABOVE the draw call, under a comment claiming it was
// "between the draw and the PRESENT transition". It captured the placeholder
// clear, not the draw -- and that is exactly the failure mode a re-derivation by
// statement order walks straight back into, which is why the flag is an
// observation in the log and not only a comment.
static bool s_drawIssuedOnList = false;

// ===========================================================================
// CBV-VIEWPORT HOIST CACHES (see DrawDynamicMeshWithPipeline CBV block).
//
// Storage is file-scope statics (not D3D12Backend members) because d3d12_backend.h
// is owned separately; all draw/Resize/BeginFrame/Present access is serialized
// under D3D12Backend::m_mutex (recursive), exactly like m_boundTargetsValid,
// so plain statics are safe here.
// Invalidation rule: InvalidateDrawCaches() is called at every
// `m_boundTargetsValid = false` site (BeginFrame Reset, AbandonFrame,
// Present close-fail/success close, guest RT/depth recreation, blit resolve,
// BeginRenderPass) plus Resize() -- i.e. whenever the command list is closed/
// reset or the target size changed. A hit is legal only within one open list
// (one batch) with matching hashes/viewport values.
// ===========================================================================
uint64_t D3D12Backend::HashConstantBank(const void* guestBytes, uint32_t guestSize) {
    // FNV-1a 64 over the FULL 4096 effective bank bytes; null/short hashes as
    // zeros to 4096 without OOB read. No sampling/prefix-only reuse.
    constexpr uint64_t kOffset = 14695981039346656037ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    constexpr size_t kBank = 4096;
    uint64_t h = kOffset;
    const uint8_t* p = static_cast<const uint8_t*>(guestBytes);
    const size_t avail = (p && guestSize > 0) ? (std::min<size_t>(guestSize, kBank)) : 0;
    // FASTBANKHASH: 4 independent lanes break the 512-step dependent multiply chain (~0.7 us)
    // into 4x128 steps that pipeline; in-run cache key only. MCLA_NO_FASTBANKHASH=1 = serial chain.
    static const bool s_fast = [] { const char* e = std::getenv("MCLA_NO_FASTBANKHASH"); return !(e && e[0] == '1'); }();
    if (s_fast) {
        alignas(8) uint8_t pad[kBank];
        const uint8_t* src = p;
        if (avail != kBank) {
            if (avail) std::memcpy(pad, p, avail);
            std::memset(pad + avail, 0, kBank - avail);
            src = pad;
        }
        uint64_t l[4] = {kOffset, kOffset ^ 0x9E3779B97F4A7C15ull, kOffset ^ 0xBF58476D1CE4E5B9ull, kOffset ^ 0x94D049BB133111EBull};
        for (size_t i = 0; i < kBank; i += 32) {
            for (size_t k = 0; k < 4; ++k) {
                uint64_t w;
                std::memcpy(&w, src + i + k * 8, 8);
                l[k] = (l[k] ^ w) * kPrime;
                l[k] ^= l[k] >> 29;
            }
        }
        for (size_t k = 0; k < 4; ++k) { h = (h ^ l[k]) * kPrime; h ^= h >> 29; }
        return h;
    }
    // 8-byte steps (same mixing as HashTextureContent); cache key only.
    if (avail == kBank) {  // common case: hash in place, no 4 KB bounce copy (same mixing, same result)
        for (size_t i = 0; i < kBank; i += 8) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            h = (h ^ w) * kPrime;
            h ^= h >> 29;
        }
        return h;
    }
    alignas(8) uint8_t buf[kBank];
    if (avail) std::memcpy(buf, p, avail);
    std::memset(buf + avail, 0, kBank - avail);
    for (size_t i = 0; i < kBank; i += 8) {
        uint64_t w;
        std::memcpy(&w, buf + i, 8);
        h = (h ^ w) * kPrime;
        h ^= h >> 29;
    }
    return h;
}

static uint64_t FnvaBankHash4096(const void* guestBytes, uint32_t guestSize) {
    // PERF: consecutive draws mostly re-present a bank with identical bytes (the PS bank especially). A memcmp
    // against the last bytes seen at this buffer (early-out, ~0.1 us for 4 KB) replaces the 512-step dependent
    // multiply chain (~0.7 us) on a hit; a miss costs the hash plus a 4 KB copy. Content-compared, so the cached
    // hash can never be stale. thread_local: safe with the MT recorders.
    constexpr size_t kBank = 4096;
    static const bool s_noBankMemo = [] { const char* e = std::getenv("MCLA_TEMP_NO_CBCACHE"); return e && e[0] == '1'; }();
    if (s_noBankMemo || !guestBytes || guestSize < kBank) return D3D12Backend::HashConstantBank(guestBytes, guestSize);
    struct Entry { const void* key = nullptr; uint64_t hash = 0; alignas(64) uint8_t bytes[kBank]; };
    static thread_local Entry tab[4];
    static thread_local uint32_t next = 0;
    for (Entry& e : tab) {
        if (e.key == guestBytes && std::memcmp(e.bytes, guestBytes, kBank) == 0) return e.hash;
    }
    Entry* slot = nullptr;
    for (Entry& e : tab) if (e.key == guestBytes) { slot = &e; break; }
    if (!slot) slot = &tab[next++ & 3u];
    slot->hash = D3D12Backend::HashConstantBank(guestBytes, guestSize);
    std::memcpy(slot->bytes, guestBytes, kBank);
    slot->key = guestBytes;
    return slot->hash;
}

uint64_t D3D12Backend::HashConstantBankMemo(const void* guestBytes, uint32_t guestSize) {
    return FnvaBankHash4096(guestBytes, guestSize);
}

struct ConstBankCacheEntry {
    bool valid = false;
    uint32_t frameIndex = UINT32_MAX;
    uint64_t hash = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    renderer::ConstantBankPack pack{};
    bool zeroPath = false;  // PS only: miss took memset + F932-PS-BANK-ZERO
    std::array<uint8_t, 4096> cpuBytes{};  // gated PSBANK snapshot: lets psHit path dump without GPU readback
};

static ConstBankCacheEntry s_cbBankCache{};
static ConstBankCacheEntry s_psBankCache{};

static bool s_viewportValid = false;
static D3D12_VIEWPORT s_lastViewport{};
static D3D12_RECT s_lastScissor{};

static void InvalidateDrawCaches() {
    InvalidateBoundState();
    s_cbBankCache = ConstBankCacheEntry{};
    s_psBankCache = ConstBankCacheEntry{};
    s_viewportValid = false;
    s_lastViewport = D3D12_VIEWPORT{};
    s_lastScissor = D3D12_RECT{};
}

// RB_DEPTH_CLEAR's depth (bits 8-31) is float20e4 when RB_DEPTH_INFO.depth_format (bit 16) is D24FS8, else unorm24.
// Port of xenia xenos.cc:155-171 Float20e4To32 (1.0 = 0xF00000, which the old unorm decode read as 0.9375).
static float GuestDepthClearToFloat(uint32_t depthClear, uint32_t depthInfo) {
    const uint32_t d = depthClear >> 8;
    if (((depthInfo >> 16) & 1u) == 0u) return static_cast<float>(d) / 16777215.0f;
    if (!d) return 0.0f;
    uint32_t mantissa = d & 0xFFFFFu, exponent = d >> 20;
    if (!exponent) {
        uint32_t lz = 0;
        while (!(mantissa & 0x100000u)) { mantissa <<= 1; ++lz; }
        exponent = uint32_t(1 - int32_t(lz));
        mantissa &= 0xFFFFFu;
    }
    const uint32_t bits = ((exponent + 112u) << 23) | (mantissa << 3);
    float f; std::memcpy(&f, &bits, 4);
    return std::clamp(f, 0.0f, 1.0f);
}

// MCLA_BMP_EVERY=N: present spacing of draw readbacks / BMP dumps (default 25; 1 = consecutive frames).
static uint64_t BmpEvery() {
    static const uint64_t s = [] { const char* e = std::getenv("MCLA_BMP_EVERY"); const uint64_t v = e ? std::strtoull(e, nullptr, 10) : 0; return v ? v : 25ull; }();
    return s;
}

// MCLA_BMP_EVERY_MS=N: wall-clock dump spacing (0 = use the present-count rule
// above). At low FPS a present-count interval produces 2-4 s gaps, so a sheet
// labelled "one frame per second" was not actually sampled at 1 Hz. When set,
// this gate dumps on a 1000 ms wall clock regardless of present rate.
static uint64_t BmpEveryMs() {
    static const uint64_t s = [] {
        const char* e = std::getenv("MCLA_BMP_EVERY_MS");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    return s;
}

// MCLA_BMP_SKIP=N: start dumping only after N presents (to catch the world pass).
static uint64_t BmpSkip() {
    static const uint64_t s = [] {
        const char* e = std::getenv("MCLA_BMP_SKIP");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    return s;
}

// Mirrored depth compare (DepthCompareMirrored): a guest depth value v is stored as 1 - v.
static float DrawPathClearValue() { return mcla::gnh::LastGuestZClear(); }
static float MirrorDepth(float v) { return DepthCompareMirrored() ? 1.0f - v : v; }

static bool StdZShadowFixEnabled() {
    static const bool s_enabled = []() {
        const char* e = std::getenv("MCLA_NO_STDZ_SHADOW_FIX");
        return !(e && e[0] == '1');
    }();
    return s_enabled;
}

// Draw-path depth clear: the guest's RB_DEPTH_CLEAR when the CP path presented it (default ON), else DrawPathClearValue().
// A non-finite or out-of-range decode falls back.
static float DrawClearForDesc(bool valid, uint32_t depthClear, uint32_t depthInfo) {
    if (!valid) return DrawPathClearValue();
    const float v = GuestDepthClearToFloat(depthClear, depthInfo);
    return (std::isfinite(v) && v >= 0.0f && v <= 1.0f) ? v : DrawPathClearValue();
}

// MCLA_BMP_MAX=N: how many frames MCLA_BMP_DUMP writes (default 24; a recording uses hundreds).
static uint32_t BmpMax() {
    static const uint32_t s = [] { const char* e = std::getenv("MCLA_BMP_MAX"); const unsigned long v = e ? std::strtoul(e, nullptr, 10) : 0; return v ? static_cast<uint32_t>(v) : 24u; }();
    return s;
}

static bool ViewportEquals(const D3D12_VIEWPORT& a, const D3D12_VIEWPORT& b) {
    return a.TopLeftX == b.TopLeftX && a.TopLeftY == b.TopLeftY && a.Width == b.Width &&
           a.Height == b.Height && a.MinDepth == b.MinDepth && a.MaxDepth == b.MaxDepth;
}

static bool ScissorEquals(const D3D12_RECT& a, const D3D12_RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

static void ComputeDrawViewportAndScissor(bool intoGuestRt, uint32_t targetW, uint32_t targetH,
                                          const D3D12Backend::DynamicMeshDesc& desc,
                                          const D3D12Backend::GraphicsSettings& gs,
                                          uint32_t bbW, uint32_t bbH,
                                          const D3D12_VIEWPORT& aspectVp,
                                          D3D12_VIEWPORT& viewport, D3D12_RECT& scissor) {
    if (intoGuestRt) {
        viewport = { 0.0f, 0.0f, static_cast<float>(targetW), static_cast<float>(targetH), 0.0f, 1.0f };
        scissor = { 0, 0, static_cast<LONG>(targetW), static_cast<LONG>(targetH) };
        if (desc.guestVpValid) {
            viewport = { desc.guestVpX, desc.guestVpY, desc.guestVpW, desc.guestVpH, desc.guestVpMinZ, desc.guestVpMaxZ };
            scissor.left = std::max<LONG>(0, static_cast<LONG>(desc.guestVpX));
            scissor.top = std::max<LONG>(0, static_cast<LONG>(desc.guestVpY));
            scissor.right = std::min<LONG>(static_cast<LONG>(targetW), static_cast<LONG>(desc.guestVpX + desc.guestVpW));
            scissor.bottom = std::min<LONG>(static_cast<LONG>(targetH), static_cast<LONG>(desc.guestVpY + desc.guestVpH));
            if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
                scissor = { 0, 0, static_cast<LONG>(targetW), static_cast<LONG>(targetH) };
        } else if (desc.clipExtW && desc.clipExtH) {
            viewport = { 0.0f, 0.0f, static_cast<float>(desc.clipExtW), static_cast<float>(desc.clipExtH), 0.0f, 1.0f };
        }
        if (desc.winScissorValid) {
            D3D12_RECT w = scissor;
            w.left = std::max(w.left, static_cast<LONG>(desc.winScissorL));
            w.top = std::max(w.top, static_cast<LONG>(desc.winScissorT));
            w.right = std::min(w.right, static_cast<LONG>(desc.winScissorR));
            w.bottom = std::min(w.bottom, static_cast<LONG>(desc.winScissorB));
            if (w.right > w.left && w.bottom > w.top) scissor = w;
        }
        // Everything above is in guest pixels; the host target is RenderScale() times larger.
        if (const LONG S = static_cast<LONG>(RenderScale()); S > 1) {
            viewport.TopLeftX *= S; viewport.TopLeftY *= S; viewport.Width *= S; viewport.Height *= S;
            scissor.left *= S; scissor.top *= S; scissor.right *= S; scissor.bottom *= S;
        }
    } else if (gs.aspectCorrection) {
        viewport = aspectVp;
        scissor.left = static_cast<LONG>(viewport.TopLeftX);
        scissor.top = static_cast<LONG>(viewport.TopLeftY);
        scissor.right = static_cast<LONG>(viewport.TopLeftX + viewport.Width);
        scissor.bottom = static_cast<LONG>(viewport.TopLeftY + viewport.Height);
    } else {
        viewport = { 0.0f, 0.0f, static_cast<float>(bbW), static_cast<float>(bbH), 0.0f, 1.0f };
        scissor = { 0, 0, static_cast<LONG>(bbW), static_cast<LONG>(bbH) };
    }
}

static bool BindOrUploadConstantBanks(D3D12Backend& be, const D3D12Backend::DynamicMeshDesc& desc,
                                      uint64_t cbHash, uint64_t psHash,
                                      ID3D12GraphicsCommandList* cl,
                                      D3D12Backend::DrawRecorder* rec,
                                      uint32_t frameIndex, bool hasUploadMap,
                                      bool intoGuestRt, bool hasActiveRt,
                                      bool emitMainLogsAndProbes,
                                      uint32_t* constBankDrawsMainOut,
                                      uint64_t psProgHash = 0) {
    static const bool s_noCbHoist = [] { const char* e = std::getenv("MCLA_NO_CB_HOIST"); const char* e2 = std::getenv("MCLA_TEMP_NO_CBCACHE"); return (e && e[0] == '1') || (e2 && e2[0] == '1'); }();
    bool& cbValidRef = rec ? rec->cbValid : s_cbBankCache.valid;
    uint32_t& cbFrameRef = rec ? rec->cbFrameIndex : s_cbBankCache.frameIndex;
    uint64_t& cbHashRef = rec ? rec->cbHash : s_cbBankCache.hash;
    D3D12_GPU_VIRTUAL_ADDRESS& cbGpuRef = rec ? rec->cbGpu : s_cbBankCache.gpu;
    renderer::ConstantBankPack& cbPackRef = rec ? rec->cbPack : s_cbBankCache.pack;

    bool& psValidRef = rec ? rec->psValid : s_psBankCache.valid;
    uint32_t& psFrameRef = rec ? rec->psFrameIndex : s_psBankCache.frameIndex;
    uint64_t& psHashRef = rec ? rec->psHash : s_psBankCache.hash;
    D3D12_GPU_VIRTUAL_ADDRESS& psGpuRef = rec ? rec->psGpu : s_psBankCache.gpu;
    bool& psZeroRef = rec ? rec->psZeroPath : s_psBankCache.zeroPath;

    const bool cbHit = !s_noCbHoist && (cbValidRef &&
                        cbFrameRef == frameIndex &&
                        cbHashRef == cbHash && cbGpuRef != 0 &&
                        hasUploadMap);
    const bool psHit = !s_noCbHoist && (psValidRef &&
                        psFrameRef == frameIndex &&
                        psHashRef == psHash && psGpuRef != 0 &&
                        hasUploadMap);
    D3D12_GPU_VIRTUAL_ADDRESS cbGpu = 0;
    uint8_t* cbDst = nullptr;
    renderer::ConstantBankPack pack{};
    if (cbHit) {
        cbGpu = cbGpuRef;
        pack = cbPackRef;
    } else {
        cbDst = rec ? be.MapUpload(renderer::kShaderConstantBankBytes, 256, cbGpu, *rec)
                    : be.MapUpload(renderer::kShaderConstantBankBytes, 256, cbGpu);
        if (cbDst) {
            if (desc.guestConstBytes != nullptr && desc.guestConstBytesSize > 0) {
                pack = renderer::PackXenosConstantBank(
                    static_cast<const uint8_t*>(desc.guestConstBytes),
                    desc.guestConstBytesSize, cbDst, renderer::kShaderConstantBankBytes);
            } else {
                std::memset(cbDst, 0, renderer::kShaderConstantBankBytes);
            }
        }
    }
    if (!cbHit && !cbDst) {
        return false;
    }

    if (!cbHit && cbDst) {
        cbValidRef = true;
        cbFrameRef = frameIndex;
        cbHashRef = cbHash;
        cbGpuRef = cbGpu;
        cbPackRef = pack;
    }
    if (rec) {
        ++rec->constBankDraws;
    } else if (constBankDrawsMainOut) {
        const uint32_t n = ++(*constBankDrawsMainOut);
        if (emitMainLogsAndProbes && (n <= 8 || (n % 200) == 0)) {
            MCLA_LOG_WARN(
                "CONST-BANK #{} src={} regs={} guestBytes={} zeroFilled={} "
                "truncated={} ok={}",
                n, desc.guestConstBytes != nullptr ? "guest" : "ZEROS",
                pack.registersPacked, desc.guestConstBytesSize,
                pack.zeroFilledBytes, pack.truncated ? 1 : 0, pack.ok ? 1 : 0);
            if (n == 1 && pack.ok && pack.registersPacked > 12 && cbDst) {
                float c[16];
                std::memcpy(c, cbDst + 8u * 16u, sizeof(c));
                MCLA_LOG_WARN(
                    "CONST-BANK-VALUES #1 "
                    "c8 =({:.4f},{:.4f},{:.4f},{:.4f}) "
                    "c9 =({:.4f},{:.4f},{:.4f},{:.4f}) "
                    "c10=({:.4f},{:.4f},{:.4f},{:.4f}) "
                    "c11=({:.4f},{:.4f},{:.4f},{:.4f})",
                    c[0],c[1],c[2],c[3], c[4],c[5],c[6],c[7],
                    c[8],c[9],c[10],c[11], c[12],c[13],c[14],c[15]);
            }
        }
    }

    D3D12_GPU_VIRTUAL_ADDRESS psGpu = 0;
    uint8_t* psDst = nullptr;
    bool psHaveGpu = false;
    static std::atomic<uint32_t> s_psZero{0};
    if (psHit) {
        psGpu = psGpuRef;
        psHaveGpu = true;
        if (psZeroRef) {
            const uint32_t z = s_psZero.fetch_add(1) + 1;
            if (z <= 4 || (z % 1000u) == 0u)
                MCLA_LOG_ERROR("F932-PS-BANK-ZERO #{} (no pixel bank from the producer)", z);
        }
    } else {
        psDst = rec ? be.MapUpload(renderer::kShaderConstantBankBytes, 256, psGpu, *rec)
                    : be.MapUpload(renderer::kShaderConstantBankBytes, 256, psGpu);
        if (psDst) {
            bool zeroPath = false;
            if (desc.guestPsConstBytes != nullptr && desc.guestPsConstBytesSize > 0) {
                (void)renderer::PackXenosConstantBank(
                    static_cast<const uint8_t*>(desc.guestPsConstBytes),
                    desc.guestPsConstBytesSize, psDst, renderer::kShaderConstantBankBytes);
                // Gated raw guest PS bank dump, keyed to the target program, to diff
                // against the packed host bank byte-for-byte.
                static const uint64_t s_rawPsProg = [] {
                    const char* e = std::getenv("MCLA_DUMP_PS_BANK_PROG");
                    return e && *e ? std::strtoull(e, nullptr, 16) : 0ULL;
                }();
                static std::atomic<uint32_t> s_rawN{0};
                if (s_rawPsProg != 0 && psProgHash == s_rawPsProg && s_rawN.load() < 4) {
                    char rfn[160];
                    std::snprintf(rfn, sizeof rfn, "psbank/RAW_%02u_%016llX.bin",
                                  s_rawN.load(), (unsigned long long)psProgHash);
                    if (std::FILE* f = std::fopen(rfn, "wb")) {
                        std::fwrite(desc.guestPsConstBytes, 1,
                                    desc.guestPsConstBytesSize, f);
                        std::fclose(f);
                        s_rawN.fetch_add(1);
                    }
                }
            }
            // DUMP-PSBANK (gated): dump the packed PS constant bank (host cRel[]) when
            // the target shader draws, to compare c64..c67 against the guest's raw bank.
            {
                static const bool s_dumpPsBank = [] {
                    const char* e = std::getenv("MCLA_DUMP_PS_BANK");
                    return e && e[0] == '1';
                }();
                static std::atomic<uint32_t> s_psDumpN{0};
                const uint32_t psDumpN = s_psDumpN.load(std::memory_order_relaxed);
                static const uint64_t s_dumpPsProg = [] {
                    const char* e = std::getenv("MCLA_DUMP_PS_BANK_PROG");
                    return e && *e ? std::strtoull(e, nullptr, 16) : 0ULL;
                }();
                if (s_dumpPsBank && psDst != nullptr &&
                    ((s_dumpPsProg != 0 && psProgHash == s_dumpPsProg) ||
                     (s_dumpPsProg == 0 && g_moviePhase.load(std::memory_order_relaxed))) &&
                    psDumpN < 64) {
                    std::error_code ec; std::filesystem::create_directories("psbank", ec);
                    char fn[160];
                    std::snprintf(fn, sizeof fn, "psbank/bank_%02u_%016llX.bin",
                                  psDumpN, (unsigned long long)psHash);
                    if (std::FILE* f = std::fopen(fn, "wb")) {
                        std::fwrite(psDst, 1, renderer::kShaderConstantBankBytes, f);
                        std::fclose(f);
                        s_psDumpN.fetch_add(1);
                        MCLA_LOG_WARN("DUMP-PSBANK n={} bankHash={:016X} bytes={} guestSrc={}",
                                      psDumpN, psHash, renderer::kShaderConstantBankBytes,
                                      desc.guestPsConstBytesSize);
                    }
                }
            }
            if (desc.guestPsConstBytes == nullptr || desc.guestPsConstBytesSize == 0) {
                std::memset(psDst, 0, renderer::kShaderConstantBankBytes);
                zeroPath = true;
                const uint32_t z = s_psZero.fetch_add(1) + 1;
                if (z <= 4 || (z % 1000u) == 0u)
                    MCLA_LOG_ERROR("F932-PS-BANK-ZERO #{} (no pixel bank from the producer)", z);
            }
            if (emitMainLogsAndProbes && intoGuestRt && hasActiveRt && !zeroPath) {
                static std::atomic<uint32_t> s_pc{0};
                const uint32_t q = s_pc.fetch_add(1) + 1;
                if (q <= 24 || (q % 2000) == 0) {
                    const float* f = reinterpret_cast<const float*>(psDst);
                    uint32_t nzc = 0;
                    for (uint32_t k = 0; k < 256; ++k) nzc += (f[k*4]!=0.f||f[k*4+1]!=0.f||f[k*4+2]!=0.f||f[k*4+3]!=0.f);
                    MCLA_LOG_WARN("TEMP-PS-CONST-PROBE #{} nonZeroRegs={}/256 c0=({:.3f},{:.3f},{:.3f},{:.3f}) c1=({:.3f},{:.3f},{:.3f},{:.3f}) c2=({:.3f},{:.3f},{:.3f},{:.3f}) c3=({:.3f},{:.3f},{:.3f},{:.3f}) c15=({:.3f},{:.3f},{:.3f},{:.3f})",
                        q, nzc, f[0],f[1],f[2],f[3], f[4],f[5],f[6],f[7], f[8],f[9],f[10],f[11], f[12],f[13],f[14],f[15], f[60],f[61],f[62],f[63]);
                }
            }
            psHaveGpu = true;
            psValidRef = true;
            psFrameRef = frameIndex;
            psHashRef = psHash;
            psGpuRef = psGpu;
            psZeroRef = zeroPath;
            // gated PSBANK snapshot: only read back from write-combined upload heap when dump is enabled.
            static const bool s_dumpPsBankSnap = [] {
                const char* e = std::getenv("MCLA_DUMP_PS_BANK");
                return e && e[0] == '1';
            }();
            if (s_dumpPsBankSnap && psDst != nullptr) {
                uint8_t* snap = rec ? rec->psCpuBytes.data() : s_psBankCache.cpuBytes.data();
                std::memcpy(snap, psDst, renderer::kShaderConstantBankBytes);
            }
        }
    }
    // Dump the bank on BOTH paths (miss and psHit) when the target program draws.
    {
        static const uint64_t s_dumpPsProg2 = [] {
            const char* e = std::getenv("MCLA_DUMP_PS_BANK_PROG");
            return e && *e ? std::strtoull(e, nullptr, 16) : 0ULL;
        }();
        static const bool s_dumpPsBank2 = [] {
            const char* e = std::getenv("MCLA_DUMP_PS_BANK");
            return e && e[0] == '1';
        }();
        static std::atomic<uint32_t> s_psDumpN2{0};
        static std::atomic<uint32_t> s_psProgLog{0};
        if (s_dumpPsBank2 && g_moviePhase.load(std::memory_order_relaxed) &&
            (s_psProgLog.fetch_add(1) % 500) == 0) {
            MCLA_LOG_WARN("PSPROG-OBSERVE prog={:016X} bankHash={:016X}", psProgHash, psHash);
        }
        if (s_dumpPsBank2 && s_dumpPsProg2 != 0 && psProgHash == s_dumpPsProg2 &&
            psHaveGpu && s_psDumpN2.load(std::memory_order_relaxed) < 8) {
            // Snapshot the CPU copy from the miss path into a stable static before any
            // other bank overwrites the cache (psHit may follow a different bank).
            static std::array<uint8_t, 4096> s_targetBank{};
            s_targetBank = (rec ? rec->psCpuBytes : s_psBankCache.cpuBytes);
            const uint8_t* snap = rec ? rec->psCpuBytes.data() : s_psBankCache.cpuBytes.data();
            char fn[160];
            std::snprintf(fn, sizeof fn, "psbank/PROG_%02u_%016llX.bin",
                          s_psDumpN2.load(std::memory_order_relaxed),
                          (unsigned long long)psProgHash);
            if (std::FILE* f = std::fopen(fn, "wb")) {
                std::fwrite(s_targetBank.data(), 1, renderer::kShaderConstantBankBytes, f);
                std::fclose(f);
                const uint32_t n = s_psDumpN2.fetch_add(1) + 1;
                MCLA_LOG_WARN("DUMP-PROGBANK n={} prog={:016X} bankHash={:016X} zeroPath={}",
                              n, psProgHash, psHash, psZeroRef);
            }
        }
    }

    if (!desc.usePrecompiledPipeline) {
        cl->SetGraphicsRootConstantBufferView(2, cbGpu);
        if (psHaveGpu) cl->SetGraphicsRootConstantBufferView(3, psGpu);
    } else {
        be.BindPrecompiledDrawResources(cl);
        cl->SetGraphicsRootConstantBufferView(4, cbGpu);
        cl->SetGraphicsRootConstantBufferView(5, psHaveGpu ? psGpu : cbGpu);
    }
    return true;
}

// Overload that accepts a specific PSO to use instead of the default test pipeline.
// TEMP-DC-TIMERS (render thread only): time between marks charged to the named stage; row printed with TEMP-PERF-STAGES.
std::atomic<uint64_t> g_dcCapBytes{0};
static const char* const kDcNames[10] = {"lock","fencewait","upload","beginframe","rt","viewport+ia","const","pso+state","texbind","draw+tail"};
static void DcMark(int i) { if (!TempPerfStagesEnabled()) return; const int64_t n = DcNowNs(); g_dcAcc[i] += n - g_dcLast; g_dcLast = n; }
std::string TempDcTimersRow() {
    std::string r = "tb[bind=" + std::to_string(g_tbAcc[0] / 1000) + "us resolve=" + std::to_string(g_tbAcc[1] / 1000) +
        "us commit=" + std::to_string(g_tbAcc[2] / 1000) + "us sampler=" + std::to_string(g_tbAcc[3] / 1000) +
        "us decoded=" + std::to_string(g_tbAcc[4] / 1000) + "us memoHit=" + std::to_string(g_tbAcc[5]) +
        " memoMiss=" + std::to_string(g_tbAcc[6]) + " missWhy=" + std::to_string(g_tbMissWhy[0]) + "/" + std::to_string(g_tbMissWhy[1]) + "/" +
        std::to_string(g_tbMissWhy[2]) + "/" + std::to_string(g_tbMissWhy[3]) + "/" + std::to_string(g_tbMissWhy[4]) + "] dc[";
    for (auto& v : g_tbMissWhy) v = 0;
    for (auto& v : g_tbAcc) v = 0;
    for (int i = 0; i < 10; ++i) { r += std::string(kDcNames[i]) + "=" + std::to_string(g_dcAcc[i] / 1000) + "us "; g_dcAcc[i] = 0; }
    r += "texMiss=" + std::to_string(g_dcMiss) + "/" + std::to_string(g_dcMissBytes) + "B/" + std::to_string(g_dcMissNs / 1000000) + "ms hash=" + std::to_string(g_dcHashBytes) + "B/" + std::to_string(g_dcHashNs / 1000000) + "ms afterId=" + std::to_string(g_dcExpandNs / 1000000) + "ms ";
    if (g_dcCache) { const auto st = g_dcCache->Stats(); r += "cache[entries=" + std::to_string(st.entries) + " resMB=" + std::to_string(st.residentBytes >> 20) + " evict=" + std::to_string(st.evictions) + " pinSkip=" + std::to_string(st.pinnedSkips) + " budgetRej=" + std::to_string(st.budgetRejects) + " hits=" + std::to_string(st.hits) + " lookups=" + std::to_string(st.lookups) + "] "; }
    r += "missNew/Same/Changed=" + std::to_string(g_dcMissNew) + "/" + std::to_string(g_dcMissSame) + "/" + std::to_string(g_dcMissChanged) + " ";
    g_dcMissNew = g_dcMissSame = g_dcMissChanged = 0;
    g_dcMiss = g_dcMissBytes = g_dcHashBytes = 0; g_dcMissNs = g_dcHashNs = g_dcExpandNs = 0;
    r += "upBytes=" + std::to_string(g_dcUpBytes.exchange(0)) + " capBytes=" + std::to_string(g_dcCapBytes.exchange(0)) +
         " barriers=" + std::to_string(g_dcBarriers.exchange(0)) + " srvCreates=" + std::to_string(g_dcSrv.exchange(0)) +
         " draws=" + std::to_string(g_dcDraws.exchange(0)) + " dropped=" + std::to_string(g_dcDropped.exchange(0)) + "]";
    return r;
}
void D3D12Backend::ComputeGuestPassKeys(const DynamicMeshDesc& desc,
                                        uint64_t& outRtKey, uint64_t& outDsKey,
                                        uint32_t& outGuestH) const {
    static const int s_directScanout = [] {
        const char* e = std::getenv("MCLA_DIRECT_BACKBUFFER");
        return (e && e[0] == '1') ? 1 : 0;
    }();
    outRtKey = 0;
    outDsKey = 0;
    outGuestH = 0;
    if (s_directScanout == 1 || !desc.hasRt) return;
    const uint32_t guestW = desc.rtSurfaceInfo & 0x3FFFu;
    const uint32_t msaa = (desc.rtSurfaceInfo >> 16) & 3u;
    const uint32_t scissorBrY = (desc.rtScissorBr >> 16) & 0x7FFFu;
    const uint32_t guestH = (scissorBrY > 0 && scissorBrY <= 4096u) ? scissorBrY : 720u;
    outGuestH = guestH;
    outDsKey = (1ull << 62) | (uint64_t(desc.rtDepthInfo & 0x7FFu) << 32) |
               (uint64_t(guestW) << 16) | ((desc.rtDepthInfo >> 16) & 1u);
    if (desc.rtCount == 0 || desc.rtActiveMask == 0) {
        outRtKey = (1ull << 61);
        return;
    }
    const uint32_t guestColorFormat = (desc.rtColorInfo >> 16) & 0xFu;
    const uint32_t guestDxgiRaw = MapXenosColorFormatToDxgi(guestColorFormat);
    const DXGI_FORMAT guestDxgi = static_cast<DXGI_FORMAT>(guestDxgiRaw);
    if (guestW == 0 || guestDxgi == DXGI_FORMAT_UNKNOWN) {
        return;
    }
    uint64_t k = (1ull << 62) | (uint64_t(desc.rtColorInfo & 0x7FFu) << 32) |
                 (uint64_t(guestW) << 16) | (uint64_t(msaa) << 8) |
                 uint64_t(guestDxgiRaw & 0xFFu);
    if (desc.rtCount > 1u || desc.rtActiveMask != 1u) {
        uint64_t mrtHash = 14695981039346656037ull;
        auto mix = [&](uint64_t v) { mrtHash ^= v; mrtHash *= 1099511628211ull; };
        mix(desc.rtCount);
        mix(desc.rtActiveMask);
        for (uint32_t i = 0; i < 4u; ++i) {
            if (desc.rtActiveMask & (1u << i)) mix(desc.rtColorInfoMrt[i]);
        }
        k ^= (mrtHash & 0x00FFFFFFFFFFFFFFull);
    }
    outRtKey = k;
}

void D3D12Backend::ResolveGuestPassTargets(const DynamicMeshDesc& desc, ID3D12GraphicsCommandList* cl,
                                           uint32_t overrideMinH, GuestPassTargets& out) {
    out = GuestPassTargets{};
    m_drawsIntoGuestRt = false;
    m_hadBackBufferDraw = false;

    static const bool s_noPassMemo = [] {
        const char* e = std::getenv("MCLA_NO_PASS_MEMO");
        return e && e[0] == '1';
    }();
    static int s_directScanout = -1;
    if (s_directScanout < 0) {
        const char* e = std::getenv("MCLA_DIRECT_BACKBUFFER");
        s_directScanout = (e && e[0] == '1') ? 1 : 0;
    }

    const uint8_t effectiveRtCount = (desc.hasRt && desc.rtCount <= 4u) ? desc.rtCount : 1u;
    const uint8_t effectiveMask = desc.hasRt ? desc.rtActiveMask : 1u;
    const uint64_t passEpoch = m_rtPassEpoch.load(std::memory_order_acquire);
    if (!s_noPassMemo && s_directScanout != 1 && desc.hasRt && overrideMinH == 0 &&
        m_passMemo.valid &&
        m_passMemo.rtPassEpoch == passEpoch &&
        m_passMemo.beginFrameBegins == m_beginFrameBegins &&
        m_passMemo.rtColorInfo == desc.rtColorInfo &&
        m_passMemo.rtDepthInfo == desc.rtDepthInfo &&
        m_passMemo.rtSurfaceInfo == desc.rtSurfaceInfo &&
        m_passMemo.rtCount == effectiveRtCount &&
        m_passMemo.rtActiveMask == effectiveMask &&
        std::memcmp(m_passMemo.rtColorInfoMrt, desc.rtColorInfoMrt, sizeof(desc.rtColorInfoMrt)) == 0 &&
        m_passMemo.targets.intoGuestRt &&
        (!m_passMemo.targets.hasDsv || (m_activeDs && m_activeDs->lastClearList == m_beginFrameBegins)) &&
        (!StdZShadowFixEnabled() || !m_activeDs || (!m_activeDs->hasPendingClear && m_activeDs->isMirrored == desc.depthMirrored))) {
        bool mrtAllReady = true;
        if (effectiveRtCount == 0) {
            mrtAllReady = (m_activeDs != nullptr);
        } else {
            for (uint32_t i = 0; i < effectiveRtCount; ++i) {
                if (!(effectiveMask & (1u << i))) continue;
                GuestRtEntry* e = m_passMemo.targets.rtEntries[i];
                if (!e || !e->res || e->lastClearList != m_beginFrameBegins ||
                    e->state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                    mrtAllReady = false;
                    break;
                }
            }
        }
        const uint32_t scY = (desc.rtScissorBr >> 16) & 0x7FFFu;
        const uint32_t reqH = (scY > 0 && scY <= 4096u) ? scY : 720u;
        if (mrtAllReady && reqH <= m_passMemo.targets.targetH) {
            out = m_passMemo.targets;
            m_drawsIntoGuestRt = true;
            m_guestRtDrawn = true;
            if (effectiveRtCount > 0 && (effectiveMask & 1u) &&
                static_cast<uint32_t>(m_guestRtFormat) != desc.psoRtvFormat) {
                static std::atomic<uint32_t> s_m{0};
                const uint32_t q = s_m.fetch_add(1) + 1;
                if (q <= 20 || (q % 5000) == 0)
                    MCLA_LOG_WARN("TEMP-PSO-RTV #{} path={} pso={} rt={} intoGuestRt=1 hasRt=1 rtColorInfo={:08X}",
                                  q, desc.psoPath, desc.psoRtvFormat, static_cast<uint32_t>(m_guestRtFormat), desc.rtColorInfo);
            }
            MayWriteBackBuffer(FrameOwner::Draw);
            for (uint32_t i = 0; i < effectiveRtCount; ++i) {
                if ((effectiveMask & (1u << i)) && out.rtEntries[i]) {
                    ++out.rtEntries[i]->draws;
                    ++out.rtEntries[i]->drawsSinceClear;
                }
            }
            if (m_activeDs) ++m_activeDs->draws;
            if ((++m_rtTabDraws % 100000) == 0) LogRtCacheTable();
            return;
        }
    }

    ID3D12Resource* drawTarget = m_renderTargets[m_frameIndex].Get();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvHandle.ptr += static_cast<size_t>(m_frameIndex) * m_rtvDescriptorSize;

    mcla::gpu::CpGuestRenderTarget gt = mcla::gpu::CpGuestRenderTargetInfo();
    if (desc.hasRt) {
        gt.surfacePitch = desc.rtSurfaceInfo & 0x3FFFu;
        gt.msaaSamples = (desc.rtSurfaceInfo >> 16) & 3u;
        gt.scissorBrY = (desc.rtScissorBr >> 16) & 0x7FFFu;
    }
    const uint32_t guestW = gt.surfacePitch;
    uint32_t guestH = 0;
    if (gt.scissorBrY > 0 && gt.scissorBrY <= 4096u) guestH = gt.scissorBrY;
    if (guestH == 0) guestH = (!desc.hasRt && m_guestRtValid && m_guestRtH) ? m_guestRtH : 720u;
    const uint32_t effectiveH = (overrideMinH > guestH) ? overrideMinH : guestH;

    uint32_t colorVal = 0;
    uint32_t colorPresent = 0;
    const uint32_t colorIdx = mcla::gpu::kCpRegRbColorInfo;
    (void)mcla::gpu::CpReadGuestRegisters(&colorIdx, 1u, &colorVal, &colorPresent);
    if (desc.hasRt) {
        colorVal = (desc.rtColorInfoMrt[0] != 0) ? desc.rtColorInfoMrt[0] : desc.rtColorInfo;
        colorPresent = 1;
    }
    const bool colorStated = colorPresent != 0u;
    const uint32_t guestColorFormat = (colorVal >> 16) & 0xFu;
    const uint32_t guestDxgiRaw = MapXenosColorFormatToDxgi(guestColorFormat);
    const DXGI_FORMAT guestDxgi = static_cast<DXGI_FORMAT>(guestDxgiRaw);

    const uint64_t rtKey = desc.hasRt
        ? ((1ull << 62) | (uint64_t(colorVal & 0x7FFu) << 32) |
           (uint64_t(guestW) << 16) | (uint64_t(gt.msaaSamples) << 8) |
           uint64_t(guestDxgiRaw & 0xFFu))
        : 0ull;

    const bool depthOnlyGuestPass = (s_directScanout != 1 && desc.hasRt && effectiveRtCount == 0 && guestW > 0);
    GuestRtEntry* slotEntries[4] = {};
    if (s_directScanout == 1) {
        GuestRenderTargetRefused("draws-target-scanout-directly");
        m_hadBackBufferDraw = true;
    } else if (guestW == 0) {
        GuestRenderTargetRefused("guest-stated-no-pitch");
    } else if (depthOnlyGuestPass) {
        m_drawsIntoGuestRt = true;
        out.rtvCount = 0;
        out.rtActiveMask = 0;
        drawTarget = nullptr;
        rtvHandle = {};
    } else if (!colorStated) {
        GuestRenderTargetRefused("guest-stated-no-color-info");
    } else if ((effectiveMask & 1u) && guestDxgi == DXGI_FORMAT_UNKNOWN) {
        GuestRenderTargetRefused("guest-color-format-unmapped");
    } else {
        bool primaryOk = true;
        uint32_t resolvedRtvCount = 0;
        DXGI_FORMAT slotFmts[4] = {};
        for (int s = static_cast<int>(effectiveRtCount) - 1; s >= 0; --s) {
            const uint32_t slot = static_cast<uint32_t>(s);
            if (!(effectiveMask & (1u << slot))) {
                out.drawTargets[slot] = nullptr;
                out.rtvHandles[slot] = {};
                out.rtEntries[slot] = nullptr;
                out.passRtKeys[slot] = 0;
                out.rtvFormats[slot] = DXGI_FORMAT_UNKNOWN;
                continue;
            }
            const uint32_t cInfo = (slot == 0) ? colorVal : desc.rtColorInfoMrt[slot];
            const uint32_t cFmt = (cInfo >> 16) & 0xFu;
            const uint32_t cDxgiRaw = MapXenosColorFormatToDxgi(cFmt);
            const DXGI_FORMAT cDxgi = static_cast<DXGI_FORMAT>(cDxgiRaw);
            slotFmts[slot] = cDxgi;
            const uint64_t slotKey = desc.hasRt
                ? ((1ull << 62) | (uint64_t(cInfo & 0x7FFu) << 32) |
                   (uint64_t(guestW) << 16) | (uint64_t(gt.msaaSamples) << 8) |
                   uint64_t(cDxgiRaw & 0xFFu))
                : 0ull;
            if (cDxgi == DXGI_FORMAT_UNKNOWN || !EnsureGuestRenderTarget(guestW, effectiveH, cDxgi, slotKey)) {
                if (slot == 0) { primaryOk = false; break; }
                out.drawTargets[slot] = nullptr;
                out.rtvHandles[slot] = {};
                out.rtEntries[slot] = nullptr;
                out.passRtKeys[slot] = 0;
                out.rtvFormats[slot] = DXGI_FORMAT_UNKNOWN;
                continue;
            }
            slotEntries[slot] = m_activeRt;
            out.drawTargets[slot] = m_guestRt.Get();
            out.rtvHandles[slot] = m_guestRtv;
            out.rtEntries[slot] = m_activeRt;
            out.passRtKeys[slot] = slotKey;
            out.rtvFormats[slot] = m_guestRtFormat;
            resolvedRtvCount = std::max(resolvedRtvCount, slot + 1u);
            if (slot > 0 && m_activeRt && m_activeRt->res) {
                if (m_activeRt->state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                    D3D12_RESOURCE_BARRIER bMrt = {};
                    bMrt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    bMrt.Transition.pResource = m_activeRt->res.Get();
                    bMrt.Transition.StateBefore = m_activeRt->state;
                    bMrt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    bMrt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    (++g_dcBarriers, cl)->ResourceBarrier(1, &bMrt);
                    m_activeRt->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
                if (m_activeRt->lastClearList != m_beginFrameBegins) {
                    m_activeRt->lastClearList = m_beginFrameBegins;
                    m_activeRt->drawsSinceClear = 0;
                    cl->ClearRenderTargetView(m_activeRt->rtv, kPlaceholderBackBufferClear, 0, nullptr);
                }
            }
        }
        if (primaryOk && resolvedRtvCount > 0) {
            for (uint32_t s = 0; s < resolvedRtvCount; ++s) {
                if (out.rtvHandles[s].ptr == 0) {
                    const DXGI_FORMAT nullFmt = (s < 4u && slotFmts[s] != DXGI_FORMAT_UNKNOWN)
                                                ? slotFmts[s]
                                                : DXGI_FORMAT_R8G8B8A8_UNORM;
                    out.rtvHandles[s] = EnsureGuestNullRtv(nullFmt);
                }
            }
            if (slotEntries[0] != nullptr) {
                drawTarget = slotEntries[0]->res.Get();
                rtvHandle = slotEntries[0]->rtv;
                if (m_activeRt != slotEntries[0]) ActivateGuestRt(*slotEntries[0]);
            } else {
                for (uint32_t s = 0; s < effectiveRtCount; ++s) {
                    if (slotEntries[s] && slotEntries[s]->res) {
                        drawTarget = slotEntries[s]->res.Get();
                        rtvHandle = slotEntries[s]->rtv;
                        if (m_activeRt != slotEntries[s]) ActivateGuestRt(*slotEntries[s]);
                        break;
                    }
                }
            }
            if (guestW != m_width || effectiveH != m_height) {
                static uint32_t s_logNot1to1 = 0;
                if (++s_logNot1to1 <= 4) {
                    MCLA_LOG_INFO("GUEST-RT-SCALING: guest {}x{} scaling to scanout {}x{} dxgi={}",
                                  guestW, effectiveH, m_width, m_height, static_cast<uint32_t>(guestDxgi));
                }
            }
            out.rtvCount = resolvedRtvCount;
            out.rtActiveMask = effectiveMask;
            m_drawsIntoGuestRt = true;
        }
    }

    if (!depthOnlyGuestPass && drawTarget && m_drawsIntoGuestRt) {
        const uint32_t rtFmt = static_cast<uint32_t>(m_guestRtFormat);
        if ((effectiveMask & 1u) && rtFmt != desc.psoRtvFormat) {
            static std::atomic<uint32_t> s_m{0};
            const uint32_t q = s_m.fetch_add(1) + 1;
            if (q <= 20 || (q % 5000) == 0)
                MCLA_LOG_WARN("TEMP-PSO-RTV #{} path={} pso={} rt={} intoGuestRt={} hasRt={} rtColorInfo={:08X}", q,
                              desc.psoPath, desc.psoRtvFormat, rtFmt, m_drawsIntoGuestRt ? 1 : 0, desc.hasRt ? 1 : 0,
                              desc.rtColorInfo);
        }
    }

    if (!depthOnlyGuestPass && drawTarget) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = drawTarget;
        barrier.Transition.StateBefore =
            m_drawsIntoGuestRt ? m_guestRtState : D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (!m_drawsIntoGuestRt || m_guestRtState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            (++g_dcBarriers, cl)->ResourceBarrier(1, &barrier);
            if (m_drawsIntoGuestRt) m_guestRtState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        {
            static uint32_t s_barDiag = 0;
            const uint32_t bd = ++s_barDiag;
            const bool recorded =
                barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (m_drawsIntoGuestRt && (recorded || bd <= 6 || (bd % 500) == 0)) {
                MCLA_LOG_INFO("GUEST-RT-BARRIER #{} state={} recorded={}",
                              bd, static_cast<unsigned>(barrier.Transition.StateBefore),
                              recorded ? 1 : 0);
            }
        }
    }

    MayWriteBackBuffer(FrameOwner::Draw);

    const uint32_t targetW = depthOnlyGuestPass ? guestW : (m_drawsIntoGuestRt ? m_guestRtW : m_width);
    const uint32_t targetH = depthOnlyGuestPass ? effectiveH : (m_drawsIntoGuestRt ? m_guestRtH : m_height);
    const uint64_t dsKey = (desc.hasRt && m_drawsIntoGuestRt)
        ? ((1ull << 62) | (uint64_t(desc.rtDepthInfo & 0x7FFu) << 32) |
           (uint64_t(targetW) << 16) | ((desc.rtDepthInfo >> 16) & 1u))
        : 0ull;
    EnsureGuestDepthStencil(
        targetW, std::max(targetH, overrideMinH), GuestDepthDxgiFormat(desc.hasRt, desc.rtDepthInfo), dsKey);

    if (m_drawsIntoGuestRt && (depthOnlyGuestPass || m_activeRt)) {
        if (!depthOnlyGuestPass && m_activeRt) {
            if (overrideMinH == 0) {
                for (uint32_t s = 0; s < out.rtvCount && s < 4u; ++s) {
                    if (out.rtEntries[s]) {
                        ++out.rtEntries[s]->draws;
                        ++out.rtEntries[s]->drawsSinceClear;
                    }
                }
            }
            if (m_activeRt->lastClearList != m_beginFrameBegins) {
                m_activeRt->lastClearList = m_beginFrameBegins;
                {
                    static std::atomic<uint32_t> s_ac{0};
                    const uint32_t q = s_ac.fetch_add(1) + 1;
                    if (m_activeRt->base == 1024 && (q <= 40 || (q % 100) == 0))
                        MCLA_LOG_WARN("TEMP-RT-AUTOCLEAR #{} base={} list={} wipedDraws={} totalDraws={}", q, m_activeRt->base,
                                      m_beginFrameBegins, m_activeRt->drawsSinceClear, m_activeRt->draws);
                }
                m_activeRt->drawsSinceClear = 0;
                cl->ClearRenderTargetView(rtvHandle, kPlaceholderBackBufferClear, 0, nullptr);
            }
        }
        if (m_guestDepthValid && m_activeDs) {
            if (StdZShadowFixEnabled()) {
                m_activeDs->isMirrored = desc.depthMirrored;
                if (m_activeDs->hasPendingClear) {
                    const float finalZ = m_activeDs->isMirrored ? MirrorDepth(m_activeDs->pendingClearZ) : m_activeDs->pendingClearZ;
                    SetTrackedState(m_activeDs->res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    cl->ClearDepthStencilView(
                        m_activeDs->dsv, static_cast<D3D12_CLEAR_FLAGS>(m_activeDs->pendingClearFlags), finalZ, m_activeDs->pendingClearStencil, 0, nullptr);
                    m_activeDs->hasPendingClear = false;
                    static std::atomic<uint32_t> s_logStdz{0};
                    const uint32_t lcount = s_logStdz.fetch_add(1) + 1;
                    if (lcount <= 24 || (lcount % 4000) == 0) {
                        MCLA_LOG_INFO("STDZ-SHADOW-FIX #{} flush-draw ds={}x{} mirrored={} rawZ={:.3f} finalZ={:.3f} fmt={}",
                                      lcount, m_activeDs->w, m_activeDs->h, m_activeDs->isMirrored ? 1 : 0,
                                      m_activeDs->pendingClearZ, finalZ, static_cast<uint32_t>(m_activeDs->fmt));
                    }
                }
            }
            if (m_activeDs->lastClearList != m_beginFrameBegins) {
                m_activeDs->lastClearList = m_beginFrameBegins;
                const float rawZ = DrawClearForDesc(desc.guestDepthClearValid, desc.guestDepthClear, desc.hasRt ? desc.rtDepthInfo : 0u);
                const float cz = (StdZShadowFixEnabled() && !desc.depthMirrored) ? rawZ : MirrorDepth(rawZ);
                cl->ClearDepthStencilView(
                    m_guestDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, cz,
                    desc.guestDepthClearValid ? static_cast<UINT8>(desc.guestDepthClear & 0xFFu) : UINT8(0), 0, nullptr);
            }
        }
        if (overrideMinH == 0 && m_activeDs) ++m_activeDs->draws;
        if (overrideMinH == 0 && (++m_rtTabDraws % 100000) == 0) LogRtCacheTable();
    } else if (!m_batchDrawFrames || !m_batchDrawFrameHasDraw) {
        cl->ClearRenderTargetView(rtvHandle, kPlaceholderBackBufferClear, 0, nullptr);
        if (m_guestDepthValid) {
            cl->ClearDepthStencilView(
                m_guestDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, MirrorDepth(DrawPathClearValue()), 0, 0, nullptr);
        }
        out.rtvCount = 1;
        out.rtActiveMask = 1;
        out.drawTargets[0] = drawTarget;
        out.rtvHandles[0] = rtvHandle;
    } else {
        out.rtvCount = 1;
        out.rtActiveMask = 1;
        out.drawTargets[0] = drawTarget;
        out.rtvHandles[0] = rtvHandle;
    }
    if (!depthOnlyGuestPass && out.rtvCount == 1 && !out.drawTargets[0]) {
        out.drawTargets[0] = drawTarget;
        out.rtvHandles[0] = rtvHandle;
        out.rtEntries[0] = m_drawsIntoGuestRt ? m_activeRt : nullptr;
    }
    {
        static uint32_t s_clearDiag = 0;
        const uint32_t cd = ++s_clearDiag;
        if (cd <= 4 || (cd % 500) == 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE bbRtv(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
            bbRtv.ptr += static_cast<size_t>(m_frameIndex) * m_rtvDescriptorSize;
            MCLA_LOG_INFO("CLEAR-TARGET #{} intoGuestRt={} isGuestRtv={} isBackRtv={} "
                          "guestRtv={:X} backRtv={:X} used={:X}",
                          cd, m_drawsIntoGuestRt ? 1 : 0,
                          (rtvHandle.ptr == m_guestRtv.ptr) ? 1 : 0,
                          (rtvHandle.ptr == bbRtv.ptr) ? 1 : 0,
                          static_cast<unsigned>(m_guestRtv.ptr & 0xFFFFFFFFu),
                          static_cast<unsigned>(bbRtv.ptr & 0xFFFFFFFFu),
                          static_cast<unsigned>(rtvHandle.ptr & 0xFFFFFFFFu));
        }
    }
    if (m_drawsIntoGuestRt) m_guestRtDrawn = true;
    if (!m_drawsIntoGuestRt) {
        NoteBufferWrite("draw-path-clear");
    }

    static const int s_noDsv = [] {
        const char* e = std::getenv("MCLA_NO_DSV");
        return (e && e[0] == '1') ? 1 : 0;
    }();
    const bool hasDsv = (m_guestDepthValid && s_noDsv == 0);
    if (drawTarget) {
        const D3D12_RESOURCE_DESC backDesc = drawTarget->GetDesc();
        out.boundRtvFormat = backDesc.Format;
        const bool first = (m_boundRtvFormat == 0);
        m_boundRtvFormat = static_cast<uint32_t>(backDesc.Format);
        const uint32_t rf = ++m_rtvFormatRows;
        if (first || ShouldLogRefusal(rf)) {
            MCLA_LOG_INFO("D3D12-RTV-FORMAT bound={} (0x{:08X}) from the {}; the "
                          "PSO's RTVFormats[0] is set by the GUEST's "
                          "RB_COLOR_INFO, NOT by this value -- pair with "
                          "CP-COLORFMT-BOUND. A disagreement rasterizes nothing "
                          "without an error.",
                          static_cast<uint32_t>(backDesc.Format),
                          static_cast<uint32_t>(backDesc.Format),
                          m_drawsIntoGuestRt ? "GUEST'S RENDER TARGET"
                                             : "swapchain back buffer");
        }
    }

    out.intoGuestRt = m_drawsIntoGuestRt;
    out.drawTarget = drawTarget;
    out.rtvHandle = rtvHandle;
    out.hasDsv = hasDsv;
    out.dsvHandle = hasDsv ? m_guestDsv : D3D12_CPU_DESCRIPTOR_HANDLE{};
    out.targetW = targetW;
    out.targetH = targetH;
    out.passRtKey = rtKey;
    out.passDsKey = dsKey;
    out.passH = std::max(targetH, effectiveH);
    bool memoOk = (!s_noPassMemo && s_directScanout != 1 && desc.hasRt && overrideMinH == 0 &&
                   out.intoGuestRt &&
                   (!out.hasDsv || (m_activeDs && m_activeDs->lastClearList == m_beginFrameBegins)));
    if (memoOk && !depthOnlyGuestPass) {
        if (!m_activeRt || m_guestRtState != D3D12_RESOURCE_STATE_RENDER_TARGET) memoOk = false;
        for (uint32_t s = 0; s < out.rtvCount && memoOk; ++s) {
            if (!(out.rtActiveMask & (1u << s))) continue;
            if (!out.rtEntries[s] || out.rtEntries[s]->lastClearList != m_beginFrameBegins ||
                out.rtEntries[s]->state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                memoOk = false;
            }
        }
    }
    if (memoOk) {
        m_passMemo.valid = true;
        m_passMemo.rtPassEpoch = m_rtPassEpoch.load(std::memory_order_acquire);
        m_passMemo.beginFrameBegins = m_beginFrameBegins;
        m_passMemo.rtColorInfo = desc.rtColorInfo;
        m_passMemo.rtDepthInfo = desc.rtDepthInfo;
        m_passMemo.rtSurfaceInfo = desc.rtSurfaceInfo;
        m_passMemo.rtCount = static_cast<uint8_t>(out.rtvCount);
        m_passMemo.rtActiveMask = out.rtActiveMask;
        std::memcpy(m_passMemo.rtColorInfoMrt, desc.rtColorInfoMrt, sizeof(desc.rtColorInfoMrt));
        m_passMemo.targets = out;
    } else {
        m_passMemo.valid = false;
    }
}

bool D3D12Backend::BeginBurstSession() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return false;
    if (!WaitForCurrentFrameGpu()) return false;
    if (!m_inFrame && !BeginFrame()) return false;
    return true;
}

bool D3D12Backend::BeginPreparedPass(const DynamicMeshDesc& desc, uint32_t overrideMinH) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!BeginBurstSession()) return false;
    ResolveGuestPassTargets(desc, m_commandList.Get(), overrideMinH, m_activePassTargets);
    return m_activePassTargets.intoGuestRt;
}

bool D3D12Backend::PrepareDrawStage2(PreparedDraw& prep) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || !m_inFrame || !m_activePassTargets.intoGuestRt) {
        prep.forceMainThread = true;
        return false;
    }
    if (s_omsetFaults.load(std::memory_order_relaxed) >= 32u) {
        prep.forceMainThread = true;
        return false;
    }
    {   // MD-12: BeginQuery/EndQuery are recorded on m_commandList (ExecuteQueryOp); a draw recorded on a worker
        // list between them is not counted by the query (zpass=0 with the sentinel present). Keep those draws on main.
        static const bool s_noOccMain = [] { const char* e = std::getenv("MCLA_NO_OCC_MAIN"); return e && e[0] == '1'; }();
        if (!s_noOccMain && !m_activeOccSlots.empty()) {
            static std::atomic<uint32_t> s_occMain{0};
            const uint32_t n = s_occMain.fetch_add(1) + 1;
            if (n <= 8 || (n % 2000) == 0)
                MCLA_LOG_WARN("OCC-FORCE-MAIN #{} active_queries={} (draw kept on main list inside a query)", n, m_activeOccSlots.size());
            prep.forceMainThread = true;
            return false;
        }
    }

    prep.intoGuestRt = m_activePassTargets.intoGuestRt;
    prep.rtvHandle = m_activePassTargets.rtvHandle;
    prep.rtvCount = m_activePassTargets.rtvCount;
    for (uint32_t i = 0; i < 4u; ++i) {
        prep.rtvHandles[i] = (i < prep.rtvCount) ? m_activePassTargets.rtvHandles[i] : D3D12_CPU_DESCRIPTOR_HANDLE{};
    }
    prep.hasDsv = m_activePassTargets.hasDsv;
    prep.dsvHandle = m_activePassTargets.dsvHandle;
    prep.targetW = m_activePassTargets.targetW;
    prep.targetH = m_activePassTargets.targetH;
    {
        uint64_t rtk = 14695981039346656037ull ^ prep.rtvCount;
        for (uint32_t i = 0; i < prep.rtvCount && i < 4u; ++i) {
            rtk *= 1099511628211ull;
            rtk ^= static_cast<uint64_t>(prep.rtvHandles[i].ptr);
        }
        prep.rtvBindKey = (prep.rtvCount == 1u) ? static_cast<uint64_t>(prep.rtvHandles[0].ptr) : rtk;
    }
    prep.dsvBindKey = prep.hasDsv ? static_cast<uint64_t>(prep.dsvHandle.ptr) : 0ull;
    prep.boundRtvFormat = m_activePassTargets.boundRtvFormat;

    if (prep.rtvCount > 0 && (m_activePassTargets.rtActiveMask & 1u)) {
        const uint32_t rtFmt = static_cast<uint32_t>(prep.boundRtvFormat);
        if (rtFmt != prep.desc.psoRtvFormat) {
            static std::atomic<uint32_t> s_m{0};
            const uint32_t q = s_m.fetch_add(1) + 1;
            if (q <= 20 || (q % 5000) == 0)
                MCLA_LOG_WARN("TEMP-PSO-RTV #{} path={} pso={} rt={} intoGuestRt=1 hasRt={} rtColorInfo={:08X}",
                              q, prep.desc.psoPath, prep.desc.psoRtvFormat, rtFmt,
                              prep.desc.hasRt ? 1 : 0, prep.desc.rtColorInfo);
        }
    }

    prep.hasSampler = false;
    prep.resolvedSamplerGpu = {};
    prep.resolvedSamplerHeap = nullptr;
    if (GuestSamplerTableEnabled() && prep.desc.hasGuestSamplerTable) {
        uint32_t activeMask = prep.desc.activeSamplerMask;
        if (activeMask == 0 && prep.desc.guestTextures && prep.desc.guestTextureCount > 0) {
            for (uint32_t ti = 0; ti < prep.desc.guestTextureCount; ++ti) {
                if (prep.desc.guestTextures[ti].mipPayloads[0] != nullptr || prep.desc.guestTextures[ti].contentId != 0) {
                    activeMask |= (1u << prep.desc.guestTextures[ti].descriptorIndex);
                }
            }
        }
        (void)InternGuestSamplerTable(prep.desc.guestSamplerTable, prep.desc.guestSamplerMask, prep.resolvedSamplerGpu, activeMask);
        prep.hasSampler = (prep.resolvedSamplerGpu.ptr != 0);
    } else if (prep.desc.hasGuestSampler) {
        prep.hasSampler = InternGuestSampler(prep.desc.guestSampler, prep.resolvedSamplerGpu);
    }
    if (prep.hasSampler) {  // Phase 0 diagnostic: retain the heap that owns this draw's handle
        prep.resolvedSamplerHeap = m_samplerHeaps[m_activeSamplerHeapIdx].heap
            ? m_samplerHeaps[m_activeSamplerHeapIdx].heap.Get()
            : m_samplerHeap.Get();
    }

    prep.stagedSlots.fill(0);
    prep.srvTableHash = 0;
    prep.boundTexCount = 0;
    prep.hasFallbackSrvs = false;
    prep.bindTextures = (prep.desc.guestTextures != nullptr && prep.desc.guestTextureCount > 0);
    if (prep.bindTextures) {
        ++m_srvRingPassesFrame;
        m_stage2FallbackSrvs.clear();
        uint32_t rawSlots[kShaderTextureSlotCount] = {};
        prep.boundTexCount = ResolveGuestTexturesToStaging(
            prep.desc.guestTextures, prep.desc.guestTextureCount, m_commandList.Get(), true,
            rawSlots, prep.srvTableHash, &m_stage2FallbackSrvs);
        for (uint32_t s = 0; s < kShaderTextureSlotCount; ++s) {
            prep.stagedSlots[s] = rawSlots[s];
        }
        prep.hasFallbackSrvs = !m_stage2FallbackSrvs.empty();
    }

    prep.stage2Ready = !prep.hasFallbackSrvs;
    return true;
}

void D3D12Backend::FoldMtRecorderStats() {
    if (m_mtRecorderCount <= 1) return;
    for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
        DrawRecorder& st = m_mtRecorderState[r];
        if (st.drawsIssued > 0) {
            m_stats.drawsIssued += st.drawsIssued;
            m_drawsThisFrame += static_cast<uint32_t>(st.drawsIssued);
            if (m_batchDrawFrames) m_batchDrawFrameHasDraw = true;
        }
        if (st.uploadsBytes > 0) {
            m_stats.uploadsBytes += st.uploadsBytes;
            m_stats.bytesThisFrame += st.bytesThisFrame;
        }
        m_stats.cacheHits += st.cacheHits;
        m_cachePipelineDraws += st.cachePipelineDraws;
        m_psoBoundDraws += st.psoBoundDraws;
        m_constBankDraws += static_cast<uint32_t>(st.constBankDraws);
        m_omSetCalls += st.omSetCalls;
        m_omSetSkips += st.omSetSkips;
        if (st.srvRingClampedCount > 0) {
            m_srvRingClampPassesFrame += st.srvRingClampedCount;
            if (!m_srvRingFrameHadClamp) {
                m_srvRingFrameHadClamp = true;
                ++m_srvRingClampedFrames;
            }
        }
        if (st.drewIntoGuestRt) {
            m_drawsIntoGuestRt = true;
            m_guestRtDrawn = true;
        }
        if (st.wroteBackBuffer) {
            NoteBufferWrite("draw");
        }
        st.ResetFrameAccumulators();
        st.ResetListBindState();
    }
}

bool D3D12Backend::FlushActiveCommandLists() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || !m_inFrame || !m_commandQueue || !m_commandList) {
        return false;
    }

    if (!m_activeOccSlots.empty()) {  // MD-12 census (log-only): an open occlusion query is split across command lists by this flush
        static std::atomic<uint32_t> s_os{0};
        const uint32_t on = s_os.fetch_add(1) + 1;
        if (on <= 8 || (on % 500) == 0)
            MCLA_LOG_WARN("OCC-SPLIT #{} active={}", on, m_activeOccSlots.size());
    }
    std::vector<ID3D12CommandList*> execLists;
    execLists.reserve(1 + kMaxMtRecorders);
    uint64_t submittedFenceValue = 0;  // successful Signal for this submission; 0 = no new signal

    if (m_mainListOpen) {
        CloseActiveOcclusionQueriesBeforeClose();
        const HRESULT hr = m_commandList->Close();
        m_mainListOpen = false;
        if (FAILED(hr)) {
            MCLA_LOG_ERROR("MT-FLUSH-MAIN-CLOSE-FAILED hr=0x{:08X}", static_cast<uint32_t>(hr));
            if (m_mtRecorderCount > 1) {
                for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
                    if (m_mtCommandLists[r] && m_mtRecorderState[r].listOpen) {
                        (void)m_mtCommandLists[r]->Close();
                        m_mtRecorderState[r].listOpen = false;
                    }
                    m_mtRecorderState[r].ResetFrameAccumulators();
                    m_mtRecorderState[r].ResetListBindState();
                }
            }
            return false;
        }
        execLists.push_back(m_commandList.Get());
    }

    if (m_mtRecorderCount > 1) {
        for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
            DrawRecorder& st = m_mtRecorderState[r];
            if (!m_mtCommandLists[r] || !st.listOpen) continue;
            const HRESULT hr = m_mtCommandLists[r]->Close();
            st.listOpen = false;
            if (FAILED(hr)) {
                MCLA_LOG_ERROR("MT-FLUSH-REC-CLOSE-FAILED rec={} hr=0x{:08X}", r, static_cast<uint32_t>(hr));
            } else if (st.drawIssued) {
                execLists.push_back(m_mtCommandLists[r].Get());
            }
        }
        FoldMtRecorderStats();
    }

    if (!execLists.empty()) {
        m_commandQueue->ExecuteCommandLists(static_cast<UINT>(execLists.size()), execLists.data());
        bool hasUnfencedOcc = false;
        for (const auto& q : m_pendingOccQueries) {
            if (q.fenceValue == UINT64_MAX) { hasUnfencedOcc = true; break; }
        }
        bool hasUnfencedSampler = false;
        for (uint32_t h = 0; h < kSamplerHeapCount; ++h) {
            for (const auto& sb : m_samplerHeaps[h].blocks) {
                if (sb.fenceValue == UINT64_MAX) { hasUnfencedSampler = true; break; }
            }
            if (hasUnfencedSampler) break;
        }
        if ((hasUnfencedOcc || hasUnfencedSampler) && m_fence) {
            ++m_currentFenceValue;
            if (SUCCEEDED(m_commandQueue->Signal(m_fence.Get(), m_currentFenceValue))) {
                submittedFenceValue = m_currentFenceValue;
                for (auto& q : m_pendingOccQueries) {
                    if (q.fenceValue == UINT64_MAX) q.fenceValue = m_currentFenceValue;
                }
                for (auto& [va, ac] : m_autoClosedOccSlots) {
                    if (ac.fenceValue == UINT64_MAX) ac.fenceValue = m_currentFenceValue;
                }
                for (uint32_t h = 0; h < kSamplerHeapCount; ++h) {
                    for (auto& sb : m_samplerHeaps[h].blocks) {
                        if (sb.fenceValue == UINT64_MAX) sb.fenceValue = m_currentFenceValue;
                    }
                }
                m_samplerBlocks = m_samplerHeaps[0].blocks;
            }
        }
        // Mid-frame flush committed GPU transitions/draws; advance frame-start snapshots
        // so any later AbandonFrame() does not roll back already-executed GPU state.
        m_guestRtStateAtFrameStart = m_guestRtState;
        SyncActiveRtState();
        for (auto& kv : m_rtCache) kv.second.stateAtFrameStart = kv.second.state;
        for (auto& kv : m_resolved) kv.second.stateAtFrameStart = kv.second.state;
        for (auto& kv : m_resMips) kv.second.tex.stateAtFrameStart = kv.second.tex.state;
        for (auto& kv : m_resolvedAlt) {
            for (ResolvedTex& r : kv.second) r.stateAtFrameStart = r.state;
        }
    }

    static std::atomic<uint64_t> s_allocResetFenceLog{0};
    const uint64_t allocResetN = s_allocResetFenceLog.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t completedFenceValue = m_fence ? m_fence->GetCompletedValue() : 0;
    if (allocResetN <= 8 || (allocResetN % 500) == 0)
        MCLA_LOG_WARN("ALLOC-LIFETIME-FENCE #{} phase=main lists={} submittedFence={} completedFence={} currentFence={} frameSlot={} (diagnostic; no wait)",
                      allocResetN, execLists.size(), submittedFenceValue, completedFenceValue,
                      m_currentFenceValue, (m_frameIndex < kBufferCount) ? m_frameIndex : 0u);

    const uint32_t frameSlot = (m_frameIndex < kBufferCount) ? m_frameIndex : 0u;
    const HRESULT rhr = m_commandList->Reset(m_commandAllocators[frameSlot].Get(), nullptr);
    if (FAILED(rhr)) {
        m_mainListOpen = false;
        m_boundTargetsValid = false;
        InvalidateDrawCaches();
        MCLA_LOG_ERROR("MT-FLUSH-MAIN-RESET-FAILED hr=0x{:08X}", static_cast<uint32_t>(rhr));
        return false;
    }
    m_mainListOpen = true;
    m_boundTargetsValid = false;
    InvalidateDrawCaches();
    BumpRtPassEpoch();

    if (m_mtRecorderCount > 1) {
        const uint64_t mtCompletedFenceValue = m_fence ? m_fence->GetCompletedValue() : 0;
        if (allocResetN <= 8 || (allocResetN % 500) == 0)
            MCLA_LOG_WARN("ALLOC-LIFETIME-FENCE #{} phase=mt lists={} submittedFence={} completedFence={} currentFence={} recorders={} (diagnostic; no wait)",
                          allocResetN, execLists.size(), submittedFenceValue, mtCompletedFenceValue,
                          m_currentFenceValue, m_mtRecorderCount);
        for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
            DrawRecorder& st = m_mtRecorderState[r];
            if (m_mtCommandLists[r] && m_mtAllocators[frameSlot][r]) {
                const HRESULT mhr = m_mtCommandLists[r]->Reset(m_mtAllocators[frameSlot][r].Get(), nullptr);
                st.listOpen = SUCCEEDED(mhr);
                if (FAILED(mhr)) {
                    MCLA_LOG_ERROR("MT-FLUSH-REC-RESET-FAILED rec={} hr=0x{:08X}", r, static_cast<uint32_t>(mhr));
                }
            }
            st.ResetListBindState();
        }
    }
    return true;
}

bool D3D12Backend::DrawPreparedStage3(const PreparedDraw& prep, DrawRecorder& rec) {
    // Lock-free parallel Stage 3: operates strictly on disjoint DrawRecorder state +
    // disjoint command list + disjoint upload/SRV sub-regions.
    ID3D12GraphicsCommandList* cl = rec.list;
    if (!m_initialized || !cl || !rec.listOpen || !prep.pso || !prep.stage2Ready ||
        prep.forceMainThread || prep.desc.usePrecompiledPipeline || !m_uploadMap) {
        rec.failedAbort = true;
        return false;
    }

    const DynamicMeshDesc& desc = prep.desc;
    const uint32_t streamCount = (desc.streamCount > 0 && desc.streamCount <= 4u) ? desc.streamCount : 1u;
    D3D12_VERTEX_BUFFER_VIEW vbViews[4] = {};
    for (uint32_t s = 0; s < streamCount; ++s) {
        if (desc.residentVbGpu[s] != 0 && desc.residentVbBytes[s] > 0) {
            vbViews[s].BufferLocation = desc.residentVbGpu[s];
            vbViews[s].SizeInBytes = desc.residentVbBytes[s];
            vbViews[s].StrideInBytes = desc.residentVbStride[s]
                                           ? desc.residentVbStride[s]
                                           : ((desc.streamCount > 0) ? desc.streams[s].stride : desc.vertexStride);
            continue;
        }
        const void* srcBytes = (desc.streamCount > 0) ? desc.streams[s].bytes : desc.vertexBytes;
        const uint32_t srcSize = (desc.streamCount > 0) ? desc.streams[s].bytesSize : desc.vertexBytesSize;
        const uint32_t srcStride = (desc.streamCount > 0) ? desc.streams[s].stride : desc.vertexStride;
        if (!srcBytes || srcSize == 0) {
            if (s == 0) { rec.failedAbort = true; return false; }
            vbViews[s] = {};
            continue;
        }
        D3D12_GPU_VIRTUAL_ADDRESS vbGpu = 0;
        uint8_t* vbDst = MapUpload(srcSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, vbGpu, rec);
        if (!vbDst) {
            g_dcDropped.fetch_add(1, std::memory_order_relaxed);
            rec.failedAbort = true;
            return false;
        }
        std::memcpy(vbDst, srcBytes, srcSize);
        g_dcUpBytes.fetch_add(srcSize, std::memory_order_relaxed);
        vbViews[s].BufferLocation = vbGpu;
        vbViews[s].SizeInBytes = srcSize;
        vbViews[s].StrideInBytes = srcStride;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ibGpu = desc.residentIbGpu ? desc.residentIbGpu : desc.cachedIndexGpu;
    uint32_t ibBytesSize = desc.residentIbGpu
                               ? desc.residentIbBytes
                               : (desc.cachedIndexGpu ? desc.cachedIndexBytesSize : desc.indexBytesSize);
    if (desc.indexed && !ibGpu) {
        if (!desc.indexBytes || desc.indexBytesSize == 0) {
            rec.failedAbort = true;
            return false;
        }
        uint8_t* ibDst = MapUpload(desc.indexBytesSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, ibGpu, rec);
        if (!ibDst) {
            g_dcDropped.fetch_add(1, std::memory_order_relaxed);
            rec.failedAbort = true;
            return false;
        }
        std::memcpy(ibDst, desc.indexBytes, desc.indexBytesSize);
        g_dcUpBytes.fetch_add(desc.indexBytesSize, std::memory_order_relaxed);
        ibBytesSize = desc.indexBytesSize;
    }

    if (!rec.boundTargetsValid || rec.boundRtvKey != prep.rtvBindKey ||
        rec.boundDsvKey != prep.dsvBindKey) {
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDsv = prep.hasDsv ? &prep.dsvHandle : nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE safeRtvs[4] = {};
        if (prep.rtvCount > 0) {
            for (uint32_t i = 0; i < prep.rtvCount && i < 4u; ++i) {
                safeRtvs[i] = prep.rtvHandles[i].ptr ? prep.rtvHandles[i] : EnsureGuestNullRtv(DXGI_FORMAT_R8G8B8A8_UNORM);
            }
        }
        unsigned long omsetCode = 0;
        if (!TryOMSetRenderTargets(cl, prep.rtvCount, prep.rtvCount ? safeRtvs : nullptr,
                                   FALSE, pDsv, &omsetCode)) {
            s_omsetFaults.fetch_add(1, std::memory_order_relaxed);
            rec.failedAbort = true;
            return false;
        }
        ++rec.omSetCalls;
        rec.boundTargetsValid = true;
        rec.boundRtvKey = prep.rtvBindKey;
        rec.boundDsvKey = prep.dsvBindKey;
    } else {
        ++rec.omSetSkips;
    }

    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    const D3D12_VIEWPORT aspectVp = (!prep.intoGuestRt && m_graphicsSettings.aspectCorrection)
        ? GetAspectCorrectViewport(GuestAspectRatio()) : D3D12_VIEWPORT{};
    ComputeDrawViewportAndScissor(prep.intoGuestRt, prep.targetW, prep.targetH, desc,
                                  m_graphicsSettings, m_width, m_height, aspectVp, viewport, scissor);
    if (!rec.viewportValid || !ViewportEquals(rec.lastViewport, viewport) ||
        !ScissorEquals(rec.lastScissor, scissor)) {
        cl->RSSetViewports(1, &viewport);
        cl->RSSetScissorRects(1, &scissor);
        rec.lastViewport = viewport;
        rec.lastScissor = scissor;
        rec.viewportValid = true;
    }

    if (rec.boundTopo != desc.topology) {
        cl->IASetPrimitiveTopology(desc.topology);
        rec.boundTopo = desc.topology;
    }
    cl->IASetVertexBuffers(0, streamCount, vbViews);

    if (desc.indexed) {
        D3D12_INDEX_BUFFER_VIEW ibView = {};
        ibView.BufferLocation = ibGpu;
        ibView.SizeInBytes = ibBytesSize;
        ibView.Format = desc.indexFormat;
        cl->IASetIndexBuffer(&ibView);
    }

    if (rec.boundRootSig != m_rootSignature.Get()) {
        cl->SetGraphicsRootSignature(m_rootSignature.Get());
        rec.boundRootSig = m_rootSignature.Get();
        rec.boundSrvTableGpu = {};
        rec.boundSamplerTableGpu = {};
    }
    if (desc.rawVb && vbViews[0].BufferLocation) {
        cl->SetGraphicsRootShaderResourceView(4, vbViews[0].BufferLocation);
    }

    if (!BindOrUploadConstantBanks(*this, desc, prep.vsConstHash, prep.psConstHash, cl, &rec,
                                   m_frameIndex, m_uploadMap != nullptr,
                                   prep.intoGuestRt, prep.intoGuestRt,
                                   false, nullptr, desc.psProgramHash)) {
        rec.failedAbort = true;
        return false;
    }

    if (rec.boundPso != prep.pso) {
        cl->SetPipelineState(prep.pso);
        rec.boundPso = prep.pso;
    }
    if (desc.stencilOn) {
        g_stencilDraws.fetch_add(1, std::memory_order_relaxed);
        if (rec.boundStencilRef != int32_t(desc.stencilRef)) {
            cl->OMSetStencilRef(desc.stencilRef);
            rec.boundStencilRef = int32_t(desc.stencilRef);
            g_stencilRefSets.fetch_add(1, std::memory_order_relaxed);
        }
    }

    D3D12_GPU_DESCRIPTOR_HANDLE srvTableGpu = {};
    if (prep.bindTextures) {
        bool clamped = false;
        uint32_t rawSlots[kShaderTextureSlotCount] = {};
        std::memcpy(rawSlots, prep.stagedSlots.data(), sizeof(rawSlots));
        srvTableGpu = CommitSrvTableFromStaging(
            rawSlots, prep.srvTableHash, &rec, nullptr,
            prep.boundTexCount, &rec.cacheHits, &clamped);
        (void)clamped;
    } else if (m_srvHeap) {
        srvTableGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE smpGpu = prep.hasSampler ? prep.resolvedSamplerGpu
                                                               : D3D12_GPU_DESCRIPTOR_HANDLE{};
    if (prep.hasSampler) {  // Phase 0 diagnostic: Stage 2's owner versus Stage 3's active heap
        ID3D12DescriptorHeap* activeSamplerHeap =
            (SamplerHeapFixEnabled() && m_samplerHeaps[m_activeSamplerHeapIdx].heap)
                ? m_samplerHeaps[m_activeSamplerHeapIdx].heap.Get()
                : m_samplerHeap.Get();
        if (activeSamplerHeap != prep.resolvedSamplerHeap) {
            static std::atomic<uint64_t> s_samplerOwnerMismatch{0};
            const uint64_t n = s_samplerOwnerMismatch.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8 || (n % 1000) == 0)
                MCLA_LOG_WARN("SAMPLER-HEAP-OWNER-MISMATCH #{} drawSeq={} rec={} owner={:016X} active={:016X} activeHeapIdx={} smp={:016X} (diagnostic)",
                              n, prep.drawSeq, rec.index,
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(prep.resolvedSamplerHeap)),
                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(activeSamplerHeap)),
                              m_activeSamplerHeapIdx, smpGpu.ptr);
        }
    }
    BindDecodedTextureInternal(cl, srvTableGpu, rec.heapsBound, smpGpu,
                               rec.boundSrvTableGpu, rec.boundSamplerTableGpu, rec.boundRootSig);

    if (desc.indexed) {
        cl->DrawIndexedInstanced(desc.indexCount, 1, desc.startIndexLocation,
                                 desc.baseVertexLocation, 0);
    } else {
        cl->DrawInstanced(desc.vertexCount, 1,
                          static_cast<UINT>(desc.baseVertexLocation > 0 ? desc.baseVertexLocation : 0),
                          0);
    }

    g_dcDraws.fetch_add(1, std::memory_order_relaxed);
    g_occDrawMono.fetch_add(1, std::memory_order_relaxed);
    rec.drawIssued = true;
    ++rec.drawsIssued;
    ++rec.cachePipelineDraws;
    ++rec.psoBoundDraws;
    if (prep.intoGuestRt) rec.drewIntoGuestRt = true;
    else rec.wroteBackBuffer = true;
    return true;
}

bool D3D12Backend::ExecuteFoldedClearRect(const PreparedDraw& prep) {
    if (!prep.foldedClearRect || prep.clearRectCount == 0) return false;
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return false;
    if (!WaitForCurrentFrameGpu()) return false;
    if (!m_inFrame && !BeginFrame()) return false;

    GuestPassTargets passTargets;
    ResolveGuestPassTargets(prep.desc, m_commandList.Get(), 0, passTargets);

    const LONG rs = static_cast<LONG>(RenderScale());
    const LONG maxW = static_cast<LONG>(passTargets.targetW) * rs;
    const LONG maxH = static_cast<LONG>(passTargets.targetH) * rs;
    D3D12_RECT scaledRects[8] = {};
    uint32_t validRects = 0;
    for (uint32_t i = 0; i < prep.clearRectCount && i < 8u; ++i) {
        D3D12_RECT r = {
            std::max<LONG>(0, prep.clearRects[i].left * rs),
            std::max<LONG>(0, prep.clearRects[i].top * rs),
            std::min<LONG>(maxW, prep.clearRects[i].right * rs),
            std::min<LONG>(maxH, prep.clearRects[i].bottom * rs)
        };
        if (r.left < r.right && r.top < r.bottom) {
            scaledRects[validRects++] = r;
        }
    }
    if (validRects == 0) return true;  // clipped clear rect

    if ((prep.clearFlags & 1u) != 0u && passTargets.rtvCount > 0) {
        for (uint32_t s = 0; s < passTargets.rtvCount && s < 4u; ++s) {
            if (passTargets.rtvHandles[s].ptr != 0) {
                m_commandList->ClearRenderTargetView(passTargets.rtvHandles[s], prep.clearColor, validRects, scaledRects);
            }
        }
        if (!passTargets.intoGuestRt) NoteBufferWrite("draw");
    }
    if ((prep.clearFlags & 2u) != 0u && passTargets.hasDsv) {
        const float cz = (StdZShadowFixEnabled() && !prep.desc.depthMirrored) ? prep.clearDepth : MirrorDepth(prep.clearDepth);
        m_commandList->ClearDepthStencilView(
            passTargets.dsvHandle, D3D12_CLEAR_FLAG_DEPTH, cz, 0, validRects, scaledRects);
    }

    ++m_stats.drawsIssued;
    ++m_drawsThisFrame;
    if (m_batchDrawFrames) m_batchDrawFrameHasDraw = true;
    static std::atomic<uint64_t> s_clearRectFolded{0};
    const uint64_t cf = s_clearRectFolded.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cf <= 12 || (cf % 1000) == 0) {
        MCLA_LOG_INFO("CLEAR-RECT-FOLDED #{} rects={} flags={:X} col=({:.3f},{:.3f},{:.3f},{:.3f}) depth={:.4f} vs={:016X} ps={:016X} rect0=({},{},{},{})",
                      cf, validRects, prep.clearFlags,
                      prep.clearColor[0], prep.clearColor[1], prep.clearColor[2], prep.clearColor[3],
                      prep.clearDepth, prep.clearVsHash, prep.clearPsHash,
                      scaledRects[0].left, scaledRects[0].top, scaledRects[0].right, scaledRects[0].bottom);
    }
    return true;
}

bool D3D12Backend::DrawDynamicMeshWithPipeline(const DynamicMeshDesc& desc,
                                               ID3D12PipelineState* pipeline,
                                               DrawRecorder* rec) {
    // Guard the whole body (including MapUpload / m_frameIndex / m_stats /
    // m_inFrame mutation) so it cannot race with Resize()/Shutdown() from the
    // window/app thread. BeginFrame() re-locks the same recursive mutex.
    if (TempPerfStagesEnabled()) g_dcLast = DcNowNs();
    ++g_dcDraws;
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    DcMark(0);
    m_drawRefuseLine = 0;
    ID3D12GraphicsCommandList* cl = rec ? rec->list : m_commandList.Get();
    // Per-recorder draw-state cache refs (file-scope globals for legacy path)
    ID3D12RootSignature*& boundRootSigRef = rec ? rec->boundRootSig : s_boundRootSig;
    ID3D12PipelineState*& boundPsoRef = rec ? rec->boundPso : s_boundPso;
    D3D12_PRIMITIVE_TOPOLOGY& boundTopoRef = rec ? rec->boundTopo : s_boundTopo;
    bool& viewportValidRef = rec ? rec->viewportValid : s_viewportValid;
    D3D12_VIEWPORT& lastViewportRef = rec ? rec->lastViewport : s_lastViewport;
    D3D12_RECT& lastScissorRef = rec ? rec->lastScissor : s_lastScissor;
    bool& drawIssuedRef = rec ? rec->drawIssued : s_drawIssuedOnList;
    if (!m_initialized) {
        RecordSilentDrawReturn("device-not-initialised");
        { m_drawRefuseLine = __LINE__; return false; }
    }
    // ---- VS-CLIPSPACE-PROBE ------------------------------------------------
    // WHY. Two suppressors have been MEASURED for a correct draw that produces
    // nothing visible, and they look identical from the outside:
    //   (a) a non-finite value anywhere in the vertex stream -- the shipped GS
    //       discards on it by design, so the whole primitive dies with NO log
    //       line (pipeline_cache.cpp:2408-2409; harness rows GS_NAN_DISCARD and
    //       GS_NAN_CONTROL in %TEMP%\opencode\d3d12_subagent\probe_clean.txt,
    //       both 0 green pixels);
    //   (b) a wrong clip-space/viewport convention -- 2,475 pixels in a corner
    //       on this device, invisible to a whole-surface census.
    // This marker separates them by reporting the stream itself.
    //
    // WHAT IT PRINTS, AND WHAT IT DELIBERATELY DOES NOT DO. It prints the raw
    // float lanes of the vertex buffer and their exact bit patterns. It does NOT
    // clamp, sanitise, replace or default any of them: a NaN in the guest's
    // vertex stream is a fact about the guest's program, and hiding it would turn
    // "the rasterizer went silent for a reason" into "the frame is the placeholder
    // colour", which is the exact confusion this project exists to avoid.
    //
    // IT DOES NOT CLAIM THESE LANES ARE POSITION. It reports "float lanes at
    // byte offset N of the vertex stream", which is what it actually read; which
    // lane is POSITION is the input layout's business and is not re-derived here.
    {
        static std::atomic<uint64_t> s_probeRows{0};
        const uint64_t row = s_probeRows.fetch_add(1) + 1;
        // Bounded by the house throttle rather than by "first draw only": a
        // one-shot row cannot be compared against a later one in the same soak.
        if (ShouldLogRefusal(row)) {
            renderer::ClipDisableRescaleConstants rc;
            const bool haveRc = renderer::QueryClipDisableRescale(&rc);
            const uint32_t stride = desc.vertexStride;
            const uint32_t lanes = stride / 4u;
            // Scan whole float lanes, not just the first three: the suppressor is
            // a non-finite value ANYWHERE, and a POSITION-only read would miss it.
            const uint32_t scanVerts = desc.vertexCount < 64u ? desc.vertexCount : 64u;
            uint32_t nonFinite = 0;
            uint32_t firstBadVert = 0xFFFFFFFFu;
            uint32_t firstBadLane = 0xFFFFFFFFu;
            float firstVal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            uint32_t firstBits[4] = {0, 0, 0, 0};
            if (desc.vertexBytes && stride >= 16u) {
                const auto* base = static_cast<const uint8_t*>(desc.vertexBytes);
                for (uint32_t v = 0; v < scanVerts; ++v) {
                    const uint8_t* vp = base + static_cast<size_t>(v) * stride;
                    for (uint32_t l = 0; l < lanes; ++l) {
                        float f;
                        memcpy(&f, vp + static_cast<size_t>(l) * 4u, 4);
                        if (v == 0 && l < 4u) {
                            firstVal[l] = f;
                            uint32_t b;
                            memcpy(&b, vp + static_cast<size_t>(l) * 4u, 4);
                            firstBits[l] = b;
                        }
                        if (!std::isfinite(f)) {
                            ++nonFinite;
                            if (firstBadVert == 0xFFFFFFFFu) {
                                firstBadVert = v;
                                firstBadLane = l;
                            }
                        }
                    }
                }
            }
            MCLA_LOG_TRACE(
                "VS-CLIPSPACE-PROBE #{} vertices={} stride={} floatLanes={} scanned={} "
                "nonFiniteLanes={} firstBadVert={} firstBadLane={} "
                "rescaleValid={} ndcScale=({},{},{}) ndcOffset=({},{},{}) "
                "v0lane0=({},{},{},{}) v0lane0Bits=0x{:08X},0x{:08X},0x{:08X},0x{:08X} "
                "-- values are the guest's, printed as-is: a NaN here is a guest fact, "
                "not clamped or defaulted, and it kills the primitive with no other log line. "
                "READ THIS ROW AS THE INPUT, NOT AS CLIP SPACE: v0lane0 is four lanes of "
                "vertex 0 of the VERTEX BUFFER, byte-for-byte what the input assembler "
                "feeds the vertex stage, so this row says what the pipeline was HANDED and "
                "nothing about what the vertex shader EMITTED. One vertex of four lanes "
                "cannot tell offscreen from degenerate from collinear either; the row below "
                "prints every vertex and every dword of the same bytes",
                row, desc.vertexCount, stride, lanes, scanVerts, nonFinite,
                firstBadVert, firstBadLane,
                static_cast<int>(haveRc),
                rc.scaleX, rc.scaleY, rc.scaleZ, rc.offsetX, rc.offsetY, rc.offsetZ,
                firstVal[0], firstVal[1], firstVal[2], firstVal[3],
                firstBits[0], firstBits[1], firstBits[2], firstBits[3]);
        }
    }
    // ---- VS-INPUT-STREAM ---------------------------------------------------
    // WHAT THIS ROW IS, STATED SO NOBODY CAN MISREAD IT.
    //
    // The row above reads `desc.vertexBytes`. Those bytes are memcpy'd into the
    // upload arena at :3196 (`std::memcpy(vbDst, desc.vertexBytes,
    // desc.vertexBytesSize)`) and bound with IASetVertexBuffers at :3325, so
    // they are byte-for-byte what the vertex stage's INPUT ASSEMBLER reads. They
    // are the pipeline's INPUT. They are NOT what the vertex shader emitted.
    //
    // There is no API on this path that reports the latter, and that is a
    // property of D3D12 rather than a gap in this file:
    //   * D3D12 has NO TRANSFORM FEEDBACK. It was removed in D3D11, so there is
    //     no stream-out from a vertex stage and nothing to capture a register
    //     into.
    //   * ID3D12PipelineState exposes no stage introspection at all -- the only
    //     pipeline method on the interface is GetCachedBlob, and the only
    //     reflection interface this tree holds (d3d12_backend.h:141-149) is a
    //     BLOBs no reflection.
    //   * This file's one readback, RecordDrawReadback, copies the back buffer
    //     and reports PIXELS. A rasteriser that emits nothing reports nothing,
    //     which is the silence this row exists beside.
    // So "where does the guest's geometry land after the vertex shader" cannot
    // be answered from the command list, and NO row here will answer it by
    // computing a position on the host: a position this file derived would be
    // this file's arithmetic wearing the pipeline's name, which is the
    // fabrication F-609 exists to prevent.
    //
    // WHAT IS PRINTED INSTEAD, and why it is the closest honest thing:
    // the COMPLETE byte image of the stream -- every vertex, every dword -- as
    // host little-endian 32-bit words. Each dword is printed with %08x and
    // nothing else, so a float lane is recoverable by bit_cast and no lane is
    // reformatted, rounded, clamped, substituted or defaulted. The bytes are
    // post-swap: the guest's own fetch constant carries a byte-order selector
    // (render_thread.cpp:2235-2243 applies CpGpuSwap32 per dword), so this is
    // the order the hardware would read, not the order the guest stored.
    //
    // WHY ALL VERTICES. A rectangle draw is three guest vertices expanded to
    // four by the geometry stage, so the primitive is fully determined by all
    // three; vertex 0 alone cannot separate "offscreen" from "degenerate" from
    // "collinear", and the row above prints four lanes of it and nothing else.
    //
    // BOUNDS, and the refusal of silence. kVsInputPrintVerts/kVsInputPrintLanes
    // cap the row; vertsOmitted / dwordsOmitted name the shortfall on the same
    // line, which is the CP-SHADER-BODY truncation discipline
    // (gpu_cp.cpp:2234-2242): no path on which a clamped dump can read as a
    // complete one. A vertex whose bytes run past vertexBytesSize is printed
    // only as far as the bytes exist and `shortDwords` counts the rest -- the
    // row never reads a byte the draw would not have read.
    {
        // WHY THESE TWO NUMBERS, and why they are named here rather than at the
        // top of the file: they are the row's bounds, so they belong beside the
        // sentence that says what happens when a vertex or a lane exceeds them.
        // kVsInputPrintVerts covers every rectangle draw this project has
        // measured (3 guest vertices, both draws in build/w709.log) with one to
        // spare, and kVsInputPrintLanes covers the widest measured stride's lane
        // count (9 lanes at stride 36; was 8, which silently dropped lane 8 of
        // every stride-36 draw WITHOUT incrementing dwordsOmitted -- the
        // row's own falsifier was blind past 32 bytes. F-914: lane 8 is the
        // second half of TEXCOORD0 for the legals text draws). A draw that
        // exceeds either is not silently truncated: vertsOmitted /
        // dwordsOmitted say so on the same line, which is the falsifier for
        // both constants.
        constexpr uint32_t kVsInputPrintVerts = 6u;
        constexpr uint32_t kVsInputPrintLanes = 9u;
        const uint64_t n = ++m_vsInputStreamRows;
        if (ShouldLogRefusal(n)) {
            const uint32_t stride = desc.vertexStride;
            const uint32_t lanes = stride / 4u;
            const bool haveBytes =
                (desc.vertexBytes != nullptr) && (desc.vertexBytesSize > 0u);
            const uint32_t scanVerts =
                (haveBytes && desc.vertexCount < kVsInputPrintVerts)
                    ? desc.vertexCount
                    : kVsInputPrintVerts;
            const uint32_t printLanes =
                (lanes < kVsInputPrintLanes) ? lanes : kVsInputPrintLanes;
            uint32_t dwordsPrinted = 0;
            uint32_t dwordsOmitted = 0;
            std::string vhex[kVsInputPrintVerts];
            for (uint32_t v = 0; v < scanVerts; ++v) {
                // Bytes this vertex actually has, from the descriptor's own
                // length rather than from `stride` alone.
                const uint32_t vOff = v * stride;
                uint32_t availLanes = 0;
                if (haveBytes && vOff < desc.vertexBytesSize) {
                    availLanes = (desc.vertexBytesSize - vOff) / 4u;
                }
                const uint32_t take = (availLanes < printLanes) ? availLanes
                                                               : printLanes;
                const auto* vp = haveBytes
                                     ? (static_cast<const uint8_t*>(desc.vertexBytes) +
                                        vOff)
                                     : nullptr;
                vhex[v].reserve(static_cast<size_t>(take) * 9u);
                for (uint32_t l = 0; l < take; ++l) {
                    uint32_t w = 0;
                    std::memcpy(&w, vp + static_cast<size_t>(l) * 4u, 4);
                    char word[9];
                    std::snprintf(word, sizeof(word), "%08x", w);
                    if (l != 0u) {
                        vhex[v].push_back(' ');
                    }
                    vhex[v].append(word, 8);
                }
                dwordsPrinted += take;
                dwordsOmitted += printLanes - take;
            }
            dwordsOmitted += (desc.vertexCount > scanVerts)
                                 ? (desc.vertexCount - scanVerts) *
                                       (printLanes > 0u ? printLanes : 1u)
                                 : 0u;
            const uint32_t vertsOmitted =
                (desc.vertexCount > scanVerts) ? (desc.vertexCount - scanVerts) : 0u;
            MCLA_LOG_WARN(
                "VS-INPUT-STREAM #{} prevCacheDraws={} haveBytes={} vertices={} stride={} "
                "floatLanes={} bytes={} vertsPrinted={} vertsOmitted={} "
                "dwordsPrinted={} dwordsOmitted={} v0=[{}] v1=[{}] v2=[{}] v3=[{}] v4=[{}] v5=[{}] "
                "-- the byte image of the vertex buffer this draw binds at slot 0, "
                "every vertex and every dword, host little-endian words after the "
                "guest's own fetch-constant byte-order swap; a float lane is "
                "recoverable by bit_cast and nothing is reformatted, clamped or "
                "defaulted. This is the pipeline's INPUT, not a shader output: a "
                "D3D12 command list has no transform feedback and no "
                "shader-output readback, so no row can report where the geometry "
                "lands after the vertex stage, and none here computes one",
                n, m_cachePipelineDraws, haveBytes ? 1u : 0u, desc.vertexCount,
                stride, lanes, desc.vertexBytesSize, scanVerts, vertsOmitted,
                dwordsPrinted, dwordsOmitted, vhex[0], vhex[1], vhex[2], vhex[3], vhex[4], vhex[5]);
        }
    }
    const bool hasStream0 = (desc.residentVbGpu[0] != 0 && desc.residentVbBytes[0] > 0) ||
                            (desc.streamCount > 0 && desc.streams[0].bytes && desc.streams[0].bytesSize > 0) ||
                            (desc.vertexBytes && desc.vertexBytesSize > 0);
    if (!hasStream0 || desc.vertexCount == 0) {
        // 2026-10-02: this gate returned false with NO log line. It is the
        // single most likely reason a captured draw produced nothing while
        // every surrounding marker still looked healthy, because
        // `DRAW-CP-DRAWN` is printed by the render thread AFTER this call
        // returns and its bool result is discarded (render_thread.cpp:3578).
        // The three numbers are the gate's own inputs, reported verbatim.
        RecordSilentDrawReturn("no-vertex-data",
                               desc.vertexBytes ? 1u : (desc.residentVbGpu[0] ? 2u : 0u),
                               desc.vertexBytesSize ? desc.vertexBytesSize : desc.residentVbBytes[0],
                               desc.vertexCount);
        { m_drawRefuseLine = __LINE__; return false; }
    }

    // F-653: refuse a null pipeline HERE, at the top, before any upload-arena
    // mapping, m_inFrame mutation or m_frameIndex advance. The duplicate check
    // further down (at the SetPipelineState call) exists as a second line of
    // defence, but by that point this function has already taken the frame's
    // upload arena and marked the frame in flight; refusing there would leave
    // that state consumed by a draw that never happened.
    if (!pipeline) {
        const uint64_t n = ++m_nullPipelineRefusals;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_WARN("D3D12Backend: DrawDynamicMeshWithPipeline refused #{}: no "
                          "pipeline. F-653: the `pipeline ? pipeline : "
                          "m_testPipeline.Get()` fallback is DELETED; a draw with no "
                          "PSO is a refusal, because the test PSO is a specific wrong "
                          "answer (28-byte POSITION float3 + COLOR float4 layout the "
                          "guest never declared), not a safe default.",
                          n);
        }
        { m_drawRefuseLine = __LINE__; return false; }
    }

    // When using the test PSO, enforce the test layout stride (28 bytes).
    // When a real PSO is provided, accept any stride Ã¢â‚¬ï¿½? the PSO's input layout
    // defines the correct vertex format.
    // F-653: the stride gate now applies ONLY when the caller passed the test
    // PSO on purpose. The `|| !pipeline` arm is gone: a null pipeline is refused
    // further down with its own named counter, and leaving it here would have
    // reported a 28-byte-stride mismatch for a draw whose real problem is that
    // it had no pipeline at all -- the F-650 log named the wrong cause.
    if (pipeline == m_testPipeline.Get()) {
        constexpr uint32_t kLayoutStride = 28;
        if (desc.vertexStride != kLayoutStride) {
            // F-650: counted and bounded. This refusal used to log every single
            // occurrence with no counter, which is why build/w662.log carries
            // 4,940 lines of it and nothing else that says how many draws the
            // test layout rejected. The MISMATCH is deliberately still refused --
            // the draws that pass this check are read through a POSITION float3 +
            // COLOR float4 layout the guest never declared, so accepting a
            // mismatched stride would trade a counted refusal for pixels read at
            // the wrong offsets. The real fix is a translated PSO carrying the
            // guest's declared input layout, which is pipeline work, not a
            // backend change: render_thread.cpp:1298 routes every DRAW_CAPTURED
            // here with psoHash 0, so DrawDynamicMesh always resolves to the test
            // PSO (d3d12_backend.cpp:2478) and no real layout is ever consulted.
            const uint64_t n = ++m_testLayoutStrideRefusals;
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_WARN("D3D12Backend: DrawDynamicMesh refused vertex stride "
                              "{} (test layout is {}) #{} -- the guest declared no "
                              "layout to the test PSO; needs a translated PSO",
                              desc.vertexStride, kLayoutStride, n);
            }
            { m_drawRefuseLine = __LINE__; return false; }
        }
    }
    if (desc.indexed && !desc.cachedIndexGpu && !(desc.residentIbGpu && desc.residentIbBytes > 0) &&
        (!desc.indexBytes || desc.indexBytesSize == 0)) {
        RecordSilentDrawReturn("no-index-data", desc.indexed ? 1u : 0u,
                               desc.indexBytesSize, desc.indexCount);
        { m_drawRefuseLine = __LINE__; return false; }
    }
    if (desc.indexed && desc.cachedIndexGpu && desc.cachedIndexBytesSize == 0) {
        MCLA_LOG_WARN("D3D12Backend: DrawDynamicMesh refused cached IB with size 0");
        { m_drawRefuseLine = __LINE__; return false; }
    }

    // F-637: the topology gate. desc.topology is already a D3D12 topology (the
    // producer mapped it with MapXenosPrimType, which refuses the Xenos
    // prim_types D3D12 cannot express), so this only has to reject a value that
    // is not a D3D12_PRIMITIVE_TOPOLOGY at all. Before this the call was a
    // literal D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, so a LINE_STRIP capture drew
    // a triangle strip of garbage and nothing said so; the gate exists so the
    // un-mappable case is a counted refusal instead of a substitution.
    switch (desc.topology) {
        // F-650: UNDEFINED is NOT a patchlist. It is the zero-initialised
        // default of a DynamicMeshDesc nobody filled in: the producer's
        // `D3D12Backend::DynamicMeshDesc desc = {}` (render_thread.cpp:1253 for
        // DRAW_CAPTURED, :1338 for DRAW_MARKER) never assigns desc.topology at
        // all, so every draw reaches here as 0. Before F-650 that fell into the
        // patchlist case below and was reported as a tessellation-domain
        // problem, naming a fault in this file for a bug in another one.
        // MEASURED consequence: the guest's actual prim_type (8, rectangle list,
        // per DRAW-CP-DRAWN #20600) never reaches MapXenosPrimType at all, so
        // the per-prim_type refusal counter cannot see it. This row is the
        // evidence that the field is unset, which is the producer-side bug to
        // report; the rect-list refusal itself is counted in MapXenosPrimType.
        case D3D_PRIMITIVE_TOPOLOGY_UNDEFINED: {
            const uint64_t n = ++m_topologyUnsetRefusals;
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_ERROR("D3D12Backend: draw refused #{}: desc.topology is "
                               "UNDEFINED -- the producer never set a topology, so "
                               "the guest's real prim_type is unknown here. NOT a "
                               "patchlist. Producer-side: render_thread.cpp builds "
                               "DynamicMeshDesc desc = {{}} without assigning "
                               "desc.topology.",
                               n);
            }
            { m_drawRefuseLine = __LINE__; return false; }
        }
        case D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST:
        case D3D_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST: {
            // A control-point patchlist against a graphics PSO that has no
            // tessellation domain set is a D3D12 validation error, not a
            // degraded draw. Refused.
            const uint64_t n = ++g_primTypeRefusalOther;
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_ERROR("D3D12Backend: draw refused #{}: topology {} is a patchlist and "
                               "this PSO has no tessellation domain", n,
                               static_cast<uint32_t>(desc.topology));
            }
            { m_drawRefuseLine = __LINE__; return false; }
        }
        default:
            break;
    }
    // TRIANGLESTRIP/TRIANGLEFAN/LINESTRIP need a restart index to be re-issued
    // correctly; a captured draw that uses a strip/fan with a non-zero start
    // index is legal D3D12 (it simply continues the strip from the adjacent
    // primitive in the buffer), so there is nothing to refuse here. What IS
    // worth counting is a non-zero start/base at all, because before F-637 both
    // were hardcoded 0 and nobody could tell whether any capture needed them.
    if (desc.indexed && (desc.startIndexLocation != 0 || desc.baseVertexLocation != 0)) {
        const uint32_t n = ++m_offsetDraws;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_INFO("D3D12Backend: draw with non-zero offsets #{} startIndex={} baseVertex={} "
                          "topology={} count={}",
                          n, desc.startIndexLocation, desc.baseVertexLocation,
                          static_cast<uint32_t>(desc.topology), desc.indexCount);
        }
    }

    if (!rec) m_stats.bytesThisFrame = 0;

    // Wait for the GPU to finish the previous frame that used this back
    // buffer's upload region before we write into it again. This must happen
    // before MapUpload, not at BeginFrame time (which is after staging), or
    // the CPU can overwrite bytes the GPU is still reading.
    if (!WaitForCurrentFrameGpuUnlocked()) { m_drawRefuseLine = __LINE__; return false; }
    DcMark(1);

    // BeginFrame resets m_uploadOffset to 0, so it MUST run before the first
    // MapUpload of the frame (else the first draw's VB/IB get overwritten).
    if (!m_inFrame) {
        if (!BeginFrame()) { m_drawRefuseLine = __LINE__; return false; }
    }
    const uint32_t streamCount = (desc.streamCount > 0 && desc.streamCount <= 4u) ? desc.streamCount : 1u;
    D3D12_VERTEX_BUFFER_VIEW vbViews[4] = {};
    D3D12_GPU_VIRTUAL_ADDRESS vbGpu = 0;
    D3D12_GPU_VIRTUAL_ADDRESS ibGpu = desc.residentIbGpu ? desc.residentIbGpu : desc.cachedIndexGpu;
    const uint32_t effectiveIbSize = desc.residentIbGpu
                                         ? desc.residentIbBytes
                                         : (desc.cachedIndexGpu ? desc.cachedIndexBytesSize : desc.indexBytesSize);
    {   // VB-DEDUPE (serial path only, slot 0): the same guest VB drawn again in this upload epoch reuses its upload.
        static const bool s_dedupe = [] { const char* e = std::getenv("MCLA_NO_VB_DEDUPE"); return !(e && e[0] == '1'); }();
        for (uint32_t s = 0; s < streamCount; ++s) {
            if (desc.residentVbGpu[s] != 0 && desc.residentVbBytes[s] > 0) {
                vbViews[s].BufferLocation = desc.residentVbGpu[s];
                vbViews[s].SizeInBytes = desc.residentVbBytes[s];
                vbViews[s].StrideInBytes = desc.residentVbStride[s]
                                               ? desc.residentVbStride[s]
                                               : ((desc.streamCount > 0) ? desc.streams[s].stride : desc.vertexStride);
                if (s == 0) vbGpu = vbViews[0].BufferLocation;
                continue;
            }
            const void* slotBytes = (desc.streamCount > 0) ? desc.streams[s].bytes : desc.vertexBytes;
            const uint32_t slotSize = (desc.streamCount > 0) ? desc.streams[s].bytesSize : desc.vertexBytesSize;
            const uint32_t slotStride = (desc.streamCount > 0) ? desc.streams[s].stride : desc.vertexStride;
            if (!slotBytes || slotSize == 0) {
                if (s == 0) { ++g_dcDropped; m_drawRefuseLine = __LINE__; return false; }
                vbViews[s] = {};
                continue;
            }
            D3D12_GPU_VIRTUAL_ADDRESS slotGpu = 0;
            if (s == 0 && s_dedupe && !rec && desc.vbDedupeKey && streamCount == 1u) {
                if (m_vbDedupe.size() > 65536) m_vbDedupe.clear();
                auto it = m_vbDedupe.find(desc.vbDedupeKey);
                if (it != m_vbDedupe.end() && it->second.first == m_uploadEpoch) slotGpu = it->second.second;
            }
            if (!slotGpu) {
                uint8_t* vbDst = rec ? MapUpload(slotSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, slotGpu, *rec)
                                     : MapUpload(slotSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, slotGpu);
                if (!vbDst) { ++g_dcDropped; m_drawRefuseLine = __LINE__; return false; }
                std::memcpy(vbDst, slotBytes, slotSize);
                g_dcUpBytes += slotSize;
                if (s == 0 && s_dedupe && !rec && desc.vbDedupeKey && streamCount == 1u) {
                    m_vbDedupe[desc.vbDedupeKey] = {m_uploadEpoch, slotGpu};
                }
            }
            vbViews[s].BufferLocation = slotGpu;
            vbViews[s].SizeInBytes = slotSize;
            vbViews[s].StrideInBytes = slotStride;
            if (s == 0) vbGpu = slotGpu;
        }
    }

    if (desc.indexed && !ibGpu) {
        uint8_t* ibDst = rec ? MapUpload(desc.indexBytesSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, ibGpu, *rec)
                         : MapUpload(desc.indexBytesSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, ibGpu);
        if (!ibDst) { m_drawRefuseLine = __LINE__; return false; }
        std::memcpy(ibDst, desc.indexBytes, desc.indexBytesSize);
        g_dcUpBytes += desc.indexBytesSize;
    }
    DcMark(2);
    DcMark(3);

    GuestPassTargets passTargets;
    ResolveGuestPassTargets(desc, cl, 0, passTargets);
    ID3D12Resource* drawTarget = passTargets.drawTarget;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = passTargets.rtvHandle;
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDsv = passTargets.hasDsv ? &passTargets.dsvHandle : nullptr;
    const uint32_t targetW = passTargets.targetW;
    const uint32_t targetH = passTargets.targetH;
    {
        static std::atomic<uint64_t> s_bn{0};
        const uint64_t q = s_bn.fetch_add(1) + 1;
        if (q <= 3 || (q % 50000) == 0)
            MCLA_LOG_WARN("TEMP-3DP-BIND #{} hostRt={:X} {}x{} fmt={} rtvPtr={:X} | depthValid={} hostDepth={:X} {}x{} fmt={} dsvPtr={:X}",
                q, reinterpret_cast<uintptr_t>(m_guestRt.Get()), m_guestRtW, m_guestRtH, static_cast<uint32_t>(m_guestRtFormat),
                static_cast<uint64_t>(rtvHandle.ptr), m_guestDepthValid ? 1 : 0, reinterpret_cast<uintptr_t>(m_guestDepth.Get()),
                m_guestDepthW, m_guestDepthH, static_cast<uint32_t>(m_guestDepthFormat), pDsv ? static_cast<uint64_t>(pDsv->ptr) : 0ull);
    }
    uint64_t rtvKey = 0;
    if (passTargets.rtvCount == 1u) {
        rtvKey = static_cast<uint64_t>(passTargets.rtvHandles[0].ptr ? passTargets.rtvHandles[0].ptr : rtvHandle.ptr);
    } else {
        rtvKey = 14695981039346656037ull ^ passTargets.rtvCount;
        for (uint32_t i = 0; i < passTargets.rtvCount && i < 4u; ++i) {
            rtvKey *= 1099511628211ull;
            rtvKey ^= static_cast<uint64_t>(passTargets.rtvHandles[i].ptr);
        }
    }
    const uint64_t dsvKey = pDsv ? static_cast<uint64_t>(pDsv->ptr) : 0u;
    bool& boundTargetsValidRef = rec ? rec->boundTargetsValid : m_boundTargetsValid;
    uint64_t& boundRtvKeyRef = rec ? rec->boundRtvKey : m_boundRtvKey;
    uint64_t& boundDsvKeyRef = rec ? rec->boundDsvKey : m_boundDsvKey;
    if (boundTargetsValidRef && boundRtvKeyRef == rtvKey &&
        boundDsvKeyRef == dsvKey) {
        ++m_omSetSkips;
    } else {
    if (s_omsetFaults.load(std::memory_order_relaxed) >= 32u) {
        if (!s_omsetLatchLogged.exchange(true, std::memory_order_relaxed)) {
            MCLA_LOG_ERROR(
                "OMSET-FAULT-LATCHED after 32 faults -- every later draw is "
                "refused WITHOUT calling OMSetRenderTargets; the first 32 "
                "OMSET-FAULT rows carry the arguments and device state");
        }
        { m_drawRefuseLine = __LINE__; return false; }
    }
    unsigned long omsetCode = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE safeRtvs[4] = {};
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvArray = nullptr;
    if (passTargets.rtvCount > 0) {
        for (uint32_t i = 0; i < passTargets.rtvCount && i < 4u; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = passTargets.rtvHandles[i];
            if (h.ptr == 0 && i == 0 && passTargets.rtvCount == 1u) h = rtvHandle;
            if (h.ptr == 0) h = EnsureGuestNullRtv(passTargets.rtvFormats[i]);
            safeRtvs[i] = h;
        }
        rtvArray = safeRtvs;
    }
    if (!TryOMSetRenderTargets(cl, passTargets.rtvCount, rtvArray, FALSE, pDsv,
                               &omsetCode)) {
        const uint32_t fn =
            s_omsetFaults.fetch_add(1, std::memory_order_relaxed) + 1;
        const HRESULT removed =
            m_device ? m_device->GetDeviceRemovedReason() : E_FAIL;
        if (fn <= 8 || (fn % 200) == 0) {
            MCLA_LOG_ERROR(
                "OMSET-FAULT #{} code=0x{:08X} list={} rtv={:X} dsvPtr={} "
                "dsv={:X} dsvHeap={} rtvHeap={} frameIndex={} presented={} "
                "fence={} omCalls={} omSkips={} drawsThisFrame={} maxFrame={} "
                "drawsIssued={} hasDraw={} drewGuestRt={} lastPresentHr={:08X} "
                "deviceRemoved=0x{:08X} drawverts={} drawidx={} indexed={} "
                "stride={} topo={} prim={} psoCache={} texPool={}",
                fn, static_cast<unsigned>(omsetCode),
                static_cast<const void*>(cl),
                static_cast<unsigned>(rtvHandle.ptr & 0xFFFFFFFFu),
                pDsv ? 1 : 0,
                static_cast<unsigned>(pDsv ? (pDsv->ptr & 0xFFFFFFFFu) : 0u),
                static_cast<const void*>(m_guestDsvHeap.Get()),
                static_cast<const void*>(m_rtvHeap.Get()), m_frameIndex,
                m_presentedFrames, m_currentFenceValue, m_omSetCalls,
                m_omSetSkips, m_drawsThisFrame, m_maxDrawsThisFrame,
                m_stats.drawsIssued,
                m_batchDrawFrameHasDraw ? 1 : 0,
                m_drawsIntoGuestRt ? 1 : 0,
                static_cast<uint32_t>(m_lastPresentHr),
                static_cast<uint32_t>(removed),
                desc.vertexCount, desc.indexCount, desc.indexed ? 1 : 0,
                desc.vertexStride, static_cast<unsigned>(desc.topology),
                desc.xenosPrimType, m_pipelineCache.Size(),
                m_texturePoolSlots.size());
            if (m_infoQueue && fn <= 6u) {
                const uint64_t nMsgs = m_infoQueue->GetNumStoredMessages();
                MCLA_LOG_ERROR("D3D12-INFOQUEUE-DUMP stored={} -- first {} shown",
                               nMsgs, nMsgs < 6u ? nMsgs : 6u);
                const uint64_t show = nMsgs < 6u ? nMsgs : 6u;
                for (uint64_t i = 0; i < show; ++i) {
                    alignas(8) uint8_t buf[2048];
                    SIZE_T len = 0;
                    const HRESULT gh = m_infoQueue->GetMessage(
                        static_cast<UINT64>(i), reinterpret_cast<D3D12_MESSAGE*>(buf),
                        &len);
                    if (FAILED(gh) || len < sizeof(D3D12_MESSAGE)) {
                        MCLA_LOG_ERROR("D3D12-INFOQUEUE-GET {} hr=0x{:08X} len={}",
                                       i, static_cast<uint32_t>(gh),
                                       static_cast<unsigned long long>(len));
                        continue;
                    }
                    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf);
                    const char* descPtr =
                        reinterpret_cast<const char*>(buf) + sizeof(D3D12_MESSAGE);
                    const size_t descLen = len > sizeof(D3D12_MESSAGE)
                        ? len - sizeof(D3D12_MESSAGE) : 0;
                    MCLA_LOG_ERROR(
                        "D3D12-INFOQUEUE-GET {} id={} sev={} desc={}",
                        i, static_cast<unsigned>(msg->ID),
                        static_cast<unsigned>(msg->Severity),
                        std::string(descPtr, descLen).substr(0, 900));
                }
            }
        }
        { m_drawRefuseLine = __LINE__; return false; }
    }
        ++m_omSetCalls;
        boundTargetsValidRef = true;
        boundRtvKeyRef = rtvKey;
        boundDsvKeyRef = dsvKey;
    }  // bind was needed
    {
        static std::atomic<uint32_t> s_targetLogCount{0};
        const uint32_t tlN = s_targetLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        static std::atomic<uint32_t> s_rectTargetLog{0};
        uint32_t rN = 0;
        if (desc.xenosPrimType == 8u) {
            rN = s_rectTargetLog.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        if (tlN <= 10 || (rN > 0 && (rN <= 20 || (rN % 500) == 0)) || (tlN % 2000) == 0) {
            MCLA_LOG_DEBUG("TEMP-G6-TARGET #{} (rect#{}) drawsIssued={} xenosPrim={} topo={} targetW={} targetH={} target={} rtvValid={} rtFormat={}",
                           tlN, rN, m_stats.drawsIssued, desc.xenosPrimType, static_cast<unsigned>(desc.topology),
                           targetW, targetH,
                           m_drawsIntoGuestRt ? "GUEST_RT" : "SWAPCHAIN_BACKBUFFER",
                           m_guestRtValid ? 1 : 0, static_cast<unsigned>(m_guestRtFormat));
        }
    }

    // The clip_disable rescale's render-target extent is published by
    // PublishClipDisableExtent at Initialize and at Resize -- the only two places
    // in this file that assign m_width/m_height, so the list is exhaustive. It is
    // NOT published here any more, and the reason is measured rather than
    // stylistic:
    //   (a) REDUNDANT. The translator samples the extent at TRANSLATION time
    //       (renderer/shader_translator.cpp:1134), which happens earlier than this
    //       point, on the render thread (src/render_thread.cpp:3471). Nudging the
    //       global from inside the draw cannot reach a translation that already
    //       happened, and cannot help a later one either -- that one reads whatever
    //       is current, which Resize has already refreshed.
    //   (b) THE WRONG THREAD. shader_translator.cpp:1800-1804 states the function
    //       is not atomic with respect to an in-flight translation and that the
    //       backend is expected to call it "during setup, before the pipeline
    //       worker is started, which is the only ordering the current pipeline
    //       guarantees". A per-draw call is by definition not during setup, and it
    //       runs while that worker is live -- i.e. it wrote a plain (non-atomic)
    //       4-float global on every single draw, in the one place the file
    //       documents as unsafe. Initialize publishes ahead of StartWorker
    //       (src/render_thread.cpp:121), so keeping this call would have preserved
    //       the race while fixing nothing.
    // The rescale still applies to every vertex shader; its GATE is a measured
    // constant, which the one-shot note below says once per process.
    {   // Once per process, so a reader of a soak knows the rescale's GATE is a
        // measured constant rather than a per-draw register read. Not a tracked
        // marker: it names no other marker (a tracked marker named inside another
        // marker's prose becomes uncountable, because soak_census.py counts lines).
        static bool s_gateNoteLogged = false;
        if (!s_gateNoteLogged) {
            s_gateNoteLogged = true;
            MCLA_LOG_INFO("clip_disable rescale: {}", renderer::kClipDisableRescaleGateNote);
        }
    }

    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    const D3D12_VIEWPORT aspectVp = (!m_drawsIntoGuestRt && m_graphicsSettings.aspectCorrection)
        ? GetAspectCorrectViewport(GuestAspectRatio()) : D3D12_VIEWPORT{};
    ComputeDrawViewportAndScissor(m_drawsIntoGuestRt, targetW, targetH, desc,
                                  m_graphicsSettings, m_width, m_height, aspectVp, viewport, scissor);
    DcMark(4);
    // Viewport/scissor hoist: skip re-set when unchanged in same open list
    // (same m_boundTargetsValid pattern). Cache cleared on BeginFrame Reset/
    // close (Abandon/Present), Resize, RT recreation, BeginRenderPass.
    // Value compare is the primary guard (target/guest-size change differs).
    if (viewportValidRef && ViewportEquals(lastViewportRef, viewport) &&
        ScissorEquals(lastScissorRef, scissor)) {
        // unchanged in same batch; reuse prior RSSet
    } else {
        cl->RSSetViewports(1, &viewport);
        cl->RSSetScissorRects(1, &scissor);
        lastViewportRef = viewport;
        lastScissorRef = scissor;
        viewportValidRef = true;
    }

    // F-637: the caller's topology, gated above. This used to be a literal
    // D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST.
    if (boundTopoRef != desc.topology) { cl->IASetPrimitiveTopology(desc.topology); boundTopoRef = desc.topology; }
    cl->IASetVertexBuffers(0, streamCount, vbViews);

    D3D12_INDEX_BUFFER_VIEW ibView = {};
    if (desc.indexed) {
        ibView.BufferLocation = ibGpu;
        // A D3D12 IBV with SizeInBytes == 0 is treated as unbound (the runtime
        // considers the slot null), so the cached IB must carry its real byte
        // size even though it is never re-uploaded.
        ibView.SizeInBytes = effectiveIbSize;
        ibView.Format = desc.indexFormat;
        cl->IASetIndexBuffer(&ibView);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE& boundSrvTableGpuRef = rec ? rec->boundSrvTableGpu : s_boundSrvTableGpu;
    D3D12_GPU_DESCRIPTOR_HANDLE& boundSamplerTableGpuRef = rec ? rec->boundSamplerTableGpu : s_boundSamplerTableGpu;
    if (boundRootSigRef != m_rootSignature.Get()) {
        cl->SetGraphicsRootSignature(m_rootSignature.Get());
        boundRootSigRef = m_rootSignature.Get();
        boundSrvTableGpuRef = {};
        boundSamplerTableGpuRef = {};
    }
    // RAWVB: the VB just uploaded (already endian-swapped like the IA path's) is the raw SRV.
    if (desc.rawVb && !desc.usePrecompiledPipeline) cl->SetGraphicsRootShaderResourceView(4, vbGpu);
    DcMark(5);

    // F-633: root CBV b0 (ShaderConstants) now carries the game's real constant
    // bank when the producer supplied one. Before this it bound 256 float4 of
    // zeros, which is not a neutral default: it is a specific wrong answer that a
    // translated shader consumes silently.
    //
    // The pack/unpack is renderer::PackXenosConstantBank (pipeline_cache.h:349) --
    // the single place the big-endian Xenos float4 bank becomes a native bank.
    // The producer (render_thread) owns the guest read, because that layer has the
    // validity check; this layer only consumes host bytes.
    // CBV hoist (2026-11-09): full-4096 FNV of each guest bank. Reuse the last
    // upload when the hash matches AND same m_frameIndex batch AND GPU addr
    // valid; skips MapUpload+Pack+memset but CBVs are still bound below.
    // Cache written only after the full miss-upload completes.
    // PERF: the producer already hashed these exact bytes; reuse its values
    // instead of re-running two full 4 KB FNV/memo passes per draw.
    const uint64_t cbHash = desc.vsConstPreHash
        ? desc.vsConstPreHash
        : FnvaBankHash4096(desc.guestConstBytes, desc.guestConstBytesSize);
    const uint64_t psHash = desc.psConstPreHash
        ? desc.psConstPreHash
        : FnvaBankHash4096(desc.guestPsConstBytes, desc.guestPsConstBytesSize);
    if (BindOrUploadConstantBanks(*this, desc, cbHash, psHash, cl, rec,
                                  m_frameIndex, m_uploadMap != nullptr,
                                  m_drawsIntoGuestRt, m_activeRt != nullptr,
                                  true, rec ? nullptr : &m_constBankDraws,
                                  desc.psProgramHash)) {
    } else {
        ++g_textureBindRefusals;
        MCLA_LOG_ERROR("D3D12Backend: draw refused: no upload-arena space for the {}B root CBV "
                       "(b0); the shader would read an unbound constant buffer",
                       renderer::kShaderConstantBankBytes);
        AbandonFrame("draw:no-constant-bank");
        { m_drawRefuseLine = __LINE__; return false; }
    }

    // F-653: the silent substitution is gone. This used to read:
    //     // Use the provided pipeline or fall back to test pipeline
    //     ID3D12PipelineState* pso = pipeline ? pipeline : m_testPipeline.Get();
    // A null `pipeline` became the FIXED TEST PSO -- a POSITION float3 + COLOR
    // float4, 28-byte-stride pipeline -- and the draw was issued through it. The
    // substitution was invisible: no log line, no counter, and a guest draw
    // whose bytes were read at the test layout's offsets rendered whatever those
    // offsets happened to hold. render_thread.cpp hands over `nullptr` on every
    // path where no pipeline was derived (render_thread.cpp:937), so this was
    // not an edge case, it was the common case.
    //
    // The null case is now refused at the TOP of this function, before any
    // upload-arena or frame-state mutation; there is deliberately no second
    // check here, because a branch that cannot be taken is a branch nobody
    // maintains. What remains is the plain binding of a PSO the caller supplied.
    DcMark(6);
    ID3D12PipelineState* pso = pipeline;
    if (boundPsoRef != pso) { cl->SetPipelineState(pso); boundPsoRef = pso; }
    if (desc.stencilOn) {  // Xenia d3d12_command_processor.cc:2963-2969: ref is dynamic state, set only on change
        int32_t& sr = rec ? rec->boundStencilRef : s_boundStencilRef;
        const uint32_t sn = g_stencilDraws.fetch_add(1, std::memory_order_relaxed) + 1;
        if (sn == 1 || (sn % 2000) == 0)
            MCLA_LOG_WARN("STENCIL-STATS draws_with_stencil={} ref_changes={} lastRef={}", sn, g_stencilRefSets.load(), desc.stencilRef);
        if (sr != int32_t(desc.stencilRef)) {
            cl->OMSetStencilRef(desc.stencilRef);
            sr = int32_t(desc.stencilRef);
            g_stencilRefSets.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // 2026-10-02: the PSO's EXISTENCE, on the draw path, as a row of its own.
    //
    // WHY IT IS HERE AND NOT AT THE CreateGraphicsPipelineState CALL: the PSO
    // the guest draw uses is created on the pipeline cache's worker thread, in
    // src/renderer/pipeline_cache.cpp:1972, which this stream does not own.
    // That site logs FAILURE (LogPsoCreateFailed, kRefusePsoCreateFailed) and
    // has no success row, which is why build/w696.log's four
    // "CreateGraphicsPipelineState" hits are all the trailing prose clause of
    // four DXC-OUTPUTS rows and the real creation is invisible: there is no
    // line anywhere that says a PSO came into existence. A non-null
    // ID3D12PipelineState* reaching SetPipelineState is the same fact observed
    // from the only side this file owns -- the cache is the sole producer of
    // the pointer, and a null one is refused at the top of this function, so
    // this row cannot print without a PSO having been created. It reports the
    // pointer, not a claim about its contents: a D3D12 PSO does not report its
    // own RTVFormats, so the format agreement is the CP-COLORFMT-BOUND /
    // D3D12-RTV-FORMAT pair, not this row.
    {
        static std::atomic<uint64_t> s_psoBoundLog{0};  // MD-32: m_psoBoundDraws is folded late; own counter
        const uint64_t bn = ++s_psoBoundLog;
        ++m_psoBoundDraws;
        if (ShouldLogRefusal(bn)) {
            MCLA_LOG_INFO("PC-PSO-BOUND #{} pso={:p} vertices={} indexed={} "
                          "indexCount={} stride={} -- a non-null cache-produced "
                          "PSO was bound, so one exists. NOT a claim about its "
                          "render targets; pair with CP-COLORFMT-BOUND and "
                          "D3D12-RTV-FORMAT.",
                          bn, static_cast<const void*>(pso), desc.vertexCount,
                          desc.indexed ? 1u : 0u, desc.indexCount,
                          desc.vertexStride);
        }
    }
    // F-653: draws issued through a PSO the pipeline CACHE produced (as opposed
    // to the test PSO). This is deliberately NOT called "rectangle draws" and
    // does not claim to be: a rectangle draw and a triangle draw are different
    // PSOs (PipelineState::geometryStage is key identity, pipeline_cache.h),
    // but this layer is handed a bare ID3D12PipelineState* and a PSO does not
    // report which variant it is, so per-variant attribution is not available
    // HERE without either a key or a PSO->stage map, and inventing either would
    // be a guess. The soak reads this beside renderer::RectangleDrawsOffered()
    // and PipelineCache::Stats().geomStageCreated instead, which are exact.
    ++m_cachePipelineDraws;
    // PIXPROBE: keep post/final draw hash live before BindGuestTexturesUnlocked
    // so FOLIAGE-BIND inside ResolveGuestTexturesToStaging reads the current ordinal.
    if (PixProbeEnabled()) {
        s_pixprobePsHash = psHash;
        s_pixprobePsProgHash = desc.psProgramHash;
        s_pixprobeDrawSeq = static_cast<uint64_t>(m_cachePipelineDraws);
    }

    // F-727: DISARM "a draw has been issued on this command list". The copy is
    // recorded AFTER the draw call now, not here; see the note immediately below
    // the draw. Clearing the flag here means it can only ever be observed true if
    // a draw call was issued during THIS invocation of this function, which is
    // what lets the D3D12-DRAW-PIXEL row print copyAfterDraw= as an observation
    // instead of a restatement of where this line happens to sit.
    drawIssuedRef = false;

    static std::atomic<uint64_t> s_cacheDrawLog{0};  // MD-32: m_cachePipelineDraws is folded late (MT), so it never throttled
    if (ShouldLogRefusal(++s_cacheDrawLog)) {
        MCLA_LOG_INFO("D3D12Backend: draw #{} issued through a cache-produced PSO "
                      "(not the test PSO) -- F-653; pair with PC-RECT-OFFERED and "
                      "geomStageCreated for the per-variant counts",
                      m_cachePipelineDraws);
    }
    // F-899 (2026-10-27): bind the guest's textures/sampler BEFORE the
    // descriptor tables are set. BindGuestTextures writes SRVs into heap slots
    // 1..(1+count-1) (t0..t3) and BindGuestSampler interns a sampler into the
    // sampler heap; BindDecodedTexture then binds both tables. If the producer
    // did not supply guest textures (guestTextures == nullptr), this is a
    // no-op and the heap keeps whatever single bootstrap texture it had.
    DcMark(7);
    if (!desc.usePrecompiledPipeline) {
        bool& heapsBoundRef = rec ? rec->heapsBound : s_heapsBound;
        D3D12_GPU_DESCRIPTOR_HANDLE& srvTableGpuRef = rec ? rec->srvTableGpu : m_currentSrvTableGpu;
        D3D12_GPU_DESCRIPTOR_HANDLE& samplerTableGpuRef = rec ? rec->samplerTableGpu : m_samplerTableGpu;
        if (desc.guestTextures && desc.guestTextureCount) {
            static std::atomic<uint32_t> s_tmpWb{0};  // TEMP-WORLD-BIND: indexed draws only, capped
            bool wantWbLog = false;
            uint32_t wbIdx = 0;
            uint64_t tmpRef0 = 0;
            if (desc.indexed) {
                wbIdx = s_tmpWb.fetch_add(1) + 1;
                wantWbLog = ((wbIdx - 1) % 1500) == 0;
                if (wantWbLog) {
                    for (uint32_t i = 0; i < 32; ++i) tmpRef0 += g_textureImportRefusals[i];
                    tmpRef0 += g_textureBindRefusals;
                }
            }
            const int64_t tbB0 = DcNowNs();
            const uint32_t tmpBound = BindGuestTexturesUnlocked(desc.guestTextures, desc.guestTextureCount, rec);
            g_tbAcc[0] += DcNowNs() - tbB0;
            if (wantWbLog) {
                const uint64_t tmpRef1 = [] { uint64_t t = 0; for (uint32_t i = 0; i < 32; ++i) t += g_textureImportRefusals[i]; return t + g_textureBindRefusals; }();
                std::string ts;
                for (uint32_t i = 0; i < desc.guestTextureCount && i < 8; ++i) {
                    const auto& g = desc.guestTextures[i];
                    ts += fmt::format(" [idx={} d1={:02X}{:02X}{:02X}{:02X} d2={:02X}{:02X}{:02X}{:02X} pay={} bytes={}]", g.descriptorIndex,
                        g.descriptorBytes[4], g.descriptorBytes[5], g.descriptorBytes[6], g.descriptorBytes[7],
                        g.descriptorBytes[8], g.descriptorBytes[9], g.descriptorBytes[10], g.descriptorBytes[11],
                        g.mipPayloads[0] ? 1 : 0, g.mipBytes[0]);
                }
                MCLA_LOG_WARN("TEMP-WORLD-BIND #{} count={} bound={} newRefusals={} indexCount={} srvTableGpu={:X}{}",
                    wbIdx, desc.guestTextureCount, tmpBound, tmpRef1 - tmpRef0, desc.indexCount, srvTableGpuRef.ptr, ts);
            }
        } else if (m_srvHeap) {
            srvTableGpuRef = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        }
        const int64_t tbS0 = DcNowNs();
        if (GuestSamplerTableEnabled()) {
            if (desc.hasGuestSamplerTable) {
                uint32_t activeMask = desc.activeSamplerMask;
                if (activeMask == 0 && desc.guestTextures && desc.guestTextureCount > 0) {
                    for (uint32_t ti = 0; ti < desc.guestTextureCount; ++ti) {
                        if (desc.guestTextures[ti].mipPayloads[0] != nullptr || desc.guestTextures[ti].contentId != 0) {
                            activeMask |= (1u << desc.guestTextures[ti].descriptorIndex);
                        }
                    }
                }
                if (!BindGuestSamplerTableInternal(desc.guestSamplerTable, desc.guestSamplerMask, cl,
                                                   heapsBoundRef, samplerTableGpuRef,
                                                   boundSamplerTableGpuRef, boundRootSigRef, activeMask,
                                                   &boundSrvTableGpuRef)) {
                    boundSamplerTableGpuRef = {};
                }
            } else if (desc.hasGuestSampler) {
                if (!BindGuestSamplerInternal(desc.guestSampler, cl, heapsBoundRef, samplerTableGpuRef,
                                              boundSamplerTableGpuRef, boundRootSigRef)) {
                    boundSamplerTableGpuRef = {};
                }
            }
        } else if (desc.hasGuestSampler) {
            if (!BindGuestSamplerInternal(desc.guestSampler, cl, heapsBoundRef, samplerTableGpuRef,
                                          boundSamplerTableGpuRef, boundRootSigRef)) {
                boundSamplerTableGpuRef = {};
            }
        }
        const int64_t tbS1 = DcNowNs();
        BindDecodedTextureInternal(cl, srvTableGpuRef, heapsBoundRef, samplerTableGpuRef,
                                   boundSrvTableGpuRef, boundSamplerTableGpuRef, boundRootSigRef);
        g_tbAcc[3] += tbS1 - tbS0; g_tbAcc[4] += DcNowNs() - tbS1;
    } else {
        // Precompiled pipeline: import textures into the SRV heap (slots 1..)
        // and point the SharedConstants indices at those slots.
        if (desc.guestTextures && desc.guestTextureCount) {
            BindGuestTexturesUnlocked(desc.guestTextures, desc.guestTextureCount, rec);
        }
        uint32_t indices[16] = {};
        for (uint32_t t = 0; t < desc.guestTextureCount && t < 16; ++t) {
            indices[t] = desc.guestTextures[t].descriptorIndex;
        }
        UpdatePrecompiledSharedConstants(indices, 16);
    }

    if (PixProbeEnabled() && !rec) {
        if (desc.usePrecompiledPipeline || !desc.guestTextures || desc.guestTextureCount == 0) {
            s_pixprobeTexCount = 0;
            s_pixprobeCurSamplesSceneColor = false;
            std::memset(s_pixprobeStagedSlots, 0, sizeof(s_pixprobeStagedSlots));
        }
        uint32_t px = 0, py = 0;
        PixProbeXY(px, py);
        const LONG srx = static_cast<LONG>(px * RenderScale());
        const LONG sry = static_cast<LONG>(py * RenderScale());
        const bool inScissor = (srx >= scissor.left && srx < scissor.right &&
                                sry >= scissor.top && sry < scissor.bottom);
        const uint32_t primVerts = desc.indexed ? desc.indexCount : desc.vertexCount;
        const bool is3dSceneDraw =
            m_drawsIntoGuestRt &&
            !desc.usePrecompiledPipeline &&
            passTargets.hasDsv &&
            primVerts > 6u &&
            !s_pixprobeCurSamplesSceneColor &&
            inScissor;
        if (is3dSceneDraw) {
            ++s_pixprobeFrame3dCount;
            s_pixprobe3d.valid = true;
            s_pixprobe3d.psHash = psHash;
            s_pixprobe3d.psProgHash = desc.psProgramHash;
            s_pixprobe3d.drawSeq = static_cast<uint64_t>(m_cachePipelineDraws);
            s_pixprobe3d.rtBase = m_activeRt ? m_activeRt->base : 0u;
            s_pixprobe3d.rtW = targetW;
            s_pixprobe3d.rtH = targetH;
            s_pixprobe3d.primVerts = primVerts;
            s_pixprobe3d.indexed = desc.indexed;
            s_pixprobe3d.hasDsv = passTargets.hasDsv;
            std::memcpy(s_pixprobe3d.stagedSlots, s_pixprobeStagedSlots, sizeof(s_pixprobe3d.stagedSlots));
            s_pixprobe3d.texCount = s_pixprobeTexCount;
            for (uint32_t i = 0; i < s_pixprobeTexCount; ++i) {
                s_pixprobe3d.tex[i] = s_pixprobeTex[i];
            }
            const uint32_t firstBase = (s_pixprobeTexCount > 0) ? s_pixprobeTex[0].base : 0u;
            bool ringUpdated = false;
            for (uint32_t i = 0; i < s_pixprobe3dRingCount; ++i) {
                if (s_pixprobe3dRing[i].psProgHash == desc.psProgramHash &&
                    s_pixprobe3dRing[i].psHash == psHash) {
                    s_pixprobe3dRing[i].lastDrawSeq = s_pixprobe3d.drawSeq;
                    ++s_pixprobe3dRing[i].draws;
                    s_pixprobe3dRing[i].texCount = s_pixprobeTexCount;
                    s_pixprobe3dRing[i].firstTexBase = firstBase;
                    ringUpdated = true;
                    break;
                }
            }
            if (!ringUpdated) {
                uint32_t slot = 0;
                if (s_pixprobe3dRingCount < kPixprobe3dRingCap) {
                    slot = s_pixprobe3dRingCount++;
                } else {
                    for (uint32_t k = 1; k < kPixprobe3dRingCap; ++k) {
                        s_pixprobe3dRing[k - 1] = s_pixprobe3dRing[k];
                    }
                    slot = kPixprobe3dRingCap - 1u;
                }
                s_pixprobe3dRing[slot] = { desc.psProgramHash, psHash, s_pixprobe3d.drawSeq,
                                           1u, s_pixprobeTexCount, firstBase };
            }
        }
    }

    DcMark(8);
    {   // TEMP-3D-D3D12: log-only count of recorded draws by kind
        static std::atomic<uint32_t> s_i{0}, s_n{0};
        const uint32_t c = desc.indexed ? ++s_i : ++s_n;
        if (c <= 4 || (c % (desc.indexed ? 1000u : 20000u)) == 0)
            MCLA_LOG_INFO("TEMP-3D-D3D12 {} #{} cnt={} vp={}x{}", desc.indexed ? "indexed" : "nonindexed", c,
                          desc.indexed ? desc.indexCount : desc.vertexCount, m_width, m_height);
    }
    // F9 owner pixel probe: record this draw if the probed pixel is inside its viewport+scissor.
    uint32_t f9Slot = UINT32_MAX, f9X = 0, f9Y = 0;
    // MCLA_F9_DEPTHONLY=1: also list depth-only guest passes (shadow casters); rt base = depth base, no texel readback.
    static const bool s_f9DepthOnly = [] { const char* e = std::getenv("MCLA_F9_DEPTHONLY"); return e && e[0] == '1'; }();
    const bool f9DepthOnly = s_f9DepthOnly && !drawTarget && passTargets.hasDsv && m_drawsIntoGuestRt;
    if (g_f9State.load(std::memory_order_relaxed) == 2 && (drawTarget || f9DepthOnly) && g_f9Draws.size() < 20000u) {
        D3D12_RESOURCE_DESC rd{};
        if (drawTarget) rd = drawTarget->GetDesc();
        else { rd.Width = targetW; rd.Height = targetH; rd.SampleDesc.Count = 0; }  // samples=0: skips readback
        const bool isBb = m_frameIndex < kBufferCount && drawTarget == m_renderTargets[m_frameIndex].Get();
        // Offscreen RTs: the same fraction of the picture, so a half-res bloom target maps to its own texel.
        // MCLA_F9_VPFRAC=1: offscreen RTs use the same fraction of the draw's VIEWPORT (reaches small passes, e.g. 256^2 in a 280-pitch tile).
        static const bool s_f9VpFrac = [] { const char* e = std::getenv("MCLA_F9_VPFRAC"); return e && e[0] == '1'; }();
        const float fx = isBb ? g_f9BbX : s_f9VpFrac ? viewport.TopLeftX + g_f9U * viewport.Width : g_f9U * static_cast<float>(rd.Width);
        const float fy = isBb ? g_f9BbY : s_f9VpFrac ? viewport.TopLeftY + g_f9V * viewport.Height : g_f9V * static_cast<float>(rd.Height);
        const bool inRt = fx >= 0.0f && fy >= 0.0f && fx < static_cast<float>(rd.Width) && fy < static_cast<float>(rd.Height);
        const LONG ix = static_cast<LONG>(fx), iy = static_cast<LONG>(fy);
        if (inRt && fx >= viewport.TopLeftX && fx < viewport.TopLeftX + viewport.Width &&
            fy >= viewport.TopLeftY && fy < viewport.TopLeftY + viewport.Height &&
            ix >= scissor.left && ix < scissor.right && iy >= scissor.top && iy < scissor.bottom) {
            F9Draw d;
            d.seq = static_cast<uint64_t>(m_cachePipelineDraws);
            d.vsHash = desc.vsProgramHash; d.psHash = desc.psProgramHash; d.psBank = psHash;
            d.prim = desc.xenosPrimType; d.indexed = desc.indexed;
            d.verts = desc.indexed ? desc.indexCount : desc.vertexCount;
            d.guestRt = m_drawsIntoGuestRt; d.hasDsv = passTargets.hasDsv;
            d.rtBase = f9DepthOnly ? (desc.rtDepthInfo & 0xFFFu) | 0xD0000000u  // D-prefix = depth base
                                   : (m_drawsIntoGuestRt && m_activeRt) ? m_activeRt->base : 0u;
            d.rtW = static_cast<uint32_t>(rd.Width); d.rtH = rd.Height; d.samples = rd.SampleDesc.Count; d.fmt = rd.Format;
            d.hx = static_cast<uint32_t>(ix); d.hy = static_cast<uint32_t>(iy);
            d.blend = desc.guestBlendControl0; d.mask = desc.guestColorMask; d.depth = desc.guestDepthControl;
            d.stencilRef = desc.stencilRef; d.vp = viewport; d.sc = scissor;
            static const char kSign[4] = {'U', 'S', 'B', 'G'};  // GuestTextureSign 0..3
            for (uint32_t i = 0; desc.guestTextures && i < desc.guestTextureCount; ++i) {
                native::GuestTextureDesc gd{};
                if (!native::DecodeGuestTextureDescriptor(desc.guestTextures[i].descriptorBytes, gd)) {
                    d.tex += fmt::format("\n      t{} <descriptor did not decode>", desc.guestTextures[i].descriptorIndex);
                    continue;
                }
                DXGI_FORMAT rf = DXGI_FORMAT_UNKNOWN;
                const bool resolved = GetResolvedTexture(gd.baseAddress, nullptr, &rf);
                d.tex += fmt::format("\n      t{} base={:08X} xenosFmt={} {}x{} mips={}..{} sign={}{}{}{} swz={:03X} expAdj={} "
                                     "resolved={}{} lastColourRebind={}",
                                     desc.guestTextures[i].descriptorIndex, gd.baseAddress, gd.xenosFormat, gd.width, gd.height,
                                     gd.mipMinLevel, gd.mipMaxLevel,
                                     kSign[static_cast<uint32_t>(gd.sign[0]) & 3u], kSign[static_cast<uint32_t>(gd.sign[1]) & 3u],
                                     kSign[static_cast<uint32_t>(gd.sign[2]) & 3u], kSign[static_cast<uint32_t>(gd.sign[3]) & 3u],
                                     gd.swizzle, gd.expAdjust, resolved ? 1 : 0,
                                     resolved ? fmt::format("(dxgi={})", static_cast<uint32_t>(rf)) : std::string(),
                                     m_resolvedLastColour.count(gd.baseAddress) ? 1 : 0);
            }
            if (d.samples == 1u && g_f9RbUsed + 2u <= kF9RbSlots) {  // every host RT is 1x today; MSAA would need a resolve
                f9Slot = g_f9RbUsed; g_f9RbUsed += 2u;
                d.rbSlot = f9Slot; f9X = d.hx; f9Y = d.hy;
                F9RecordPixel(cl, drawTarget, f9X, f9Y, f9Slot);
            }
            g_f9Draws.push_back(std::move(d));
        }
    }
    if (desc.indexed) {
        // F-637: the capture's start index and base vertex. Both were hardcoded
        // 0, so any draw that used them drew the wrong vertices with no log.
        cl->DrawIndexedInstanced(desc.indexCount, 1, desc.startIndexLocation,
                                            desc.baseVertexLocation, 0);
    } else {
        cl->DrawInstanced(desc.vertexCount, 1,
                          static_cast<UINT>(desc.baseVertexLocation > 0 ? desc.baseVertexLocation : 0),
                          0);
    }
    DcMark(9);
    if (f9Slot != UINT32_MAX) F9RecordPixel(cl, drawTarget, f9X, f9Y, f9Slot + 1u);
    // OCC-DRAW-ISSUED: the single-thread DRAW_CAPTURED path issues here, NOT at the
    // DrawPreparedStage3 site (~:8412), so g_occDrawMono never saw these draws.
    g_occDrawMono.fetch_add(1, std::memory_order_relaxed);
    if (!m_activeOccSlots.empty()) {
        static std::atomic<uint32_t> s_oi{0};
        const uint32_t n = ++s_oi;
        if (n <= 16 || (n % 500) == 0)
            MCLA_LOG_WARN("OCC-DRAW-ISSUED #{} active_queries={} indexed={} count={}", n,
                          m_activeOccSlots.size(), desc.indexed ? 1 : 0,
                          desc.indexed ? desc.indexCount : desc.vertexCount);
    }

    // F-761: a recorded draw call is a write to m_renderTargets[m_frameIndex] in
    // the only sense that matters for "are these contents defined": whether it
    // rasterises anything is a separate question with its own markers, and
    // conflating the two would let a draw that wrote nothing count as proof
    // that the buffer was written.
    // F-XXX: when the draws land in the GUEST'S RENDER TARGET, this draw is
    // NOT a write to the back buffer -- it is a write to the guest's render
    // target, and the resolve is what writes the back buffer. Counting it here
    // would make the ledger claim a back-buffer write that did not happen.
    if (rec) {
        rec->drawIssued = true;
        ++rec->drawsIssued;
        if (m_drawsIntoGuestRt) {
            rec->drewIntoGuestRt = true;
        } else {
            rec->wroteBackBuffer = true;
        }
    } else {
        if (m_drawsIntoGuestRt) {
            m_guestRtDrawn = true;
        } else {
            NoteBufferWrite("draw");
        }

        m_stats.drawsIssued++;
        ++m_drawsThisFrame;
    }

    // ---- DRAW-COMMANDLIST-BOUND ---------------------------------------------
    // THE STATE THAT COULD STILL SUPPRESS RASTERISATION, AS THIS FILE SETS IT.
    //
    // A correct vertex stream that produces no pixel has a short list of
    // suppressors, and every one of them is state rather than data: a viewport
    // or scissor that excludes the primitive, a rasteriser state that culls or
    // does not fill, a depth or blend state that discards the result, a
    // topology that does not assemble, a render-target format the PSO and the
    // bound RTV disagree about. None of them was observable before this row,
    // because the values live in locals that died at the end of the function
    // and in a pipeline description this file never sees.
    //
    // WHAT IS PRINTED, and where each value comes from:
    //   vp / scissor        the `viewport` and `scissor` locals handed to
    //                        RSSetViewports / RSSetScissorRects at :3409-3410,
    //                        read back out of those same locals -- so they are
    //                        the values the command list was told, not a
    //                        recomputation of them.
    //   topology/counts      the `desc` fields the draw call was issued from.
    //   vb / ib              the `vbView` and `ibView` locals handed to
    //                        IASetVertexBuffers / IASetIndexBuffer, FIELD BY
    //                        FIELD in the D3D12_VERTEX_BUFFER_VIEW order
    //                        (BufferLocation, SizeInBytes, StrideInBytes). The
    //                        order is written out because getting it backwards
    //                        reproduces silently a stride that looks plausible,
    //                        which is the exact symptom this row is here to
    //                        rule out.
    //   rtvFormat            m_boundRtvFormat, assigned this pass at :3349 from
    //                        the bound back buffer's own D3D12_RESOURCE_DESC.
    //   gs*                  renderer::PipelineCacheStats, read from THIS
    //                        OBJECT's m_pipelineCache -- the same member
    //                        render_thread.cpp:3742 calls GetOrCompile on, so
    //                        these are the totals for the cache that produced
    //                        the PSO bound above, not a global that might belong
    //                        to another cache. gsOffered is the free function's
    //                        counter because PipelineState has no field for it
    //                        (pipeline_cache.h:814-821). gsStageObtained /
    //                        gsStageRefused are that struct's geomStageWaited /
    //                        geomStageWaitFailed under names that do not
    //                        contain the word their source fields do: the
    //                        tracking keyword is a two-line match and a row
    //                        must not inflate another row's count.
    //
    // WHAT IS NOT PRINTED, AND WHY -- stated here because a gap that is only
    // described in a report is a gap that gets re-derived wrongly:
    //   psoDescReadable=0. The D3D12_GRAPHICS_PIPELINE_STATE_DESC for a guest
    //     draw is NOT built by this file. It is built at
    //     renderer/pipeline_cache.cpp:1894-1948, inside that file's worker
    //     thread, and consumed by one CreateGraphicsPipelineState call at :1972;
    //     the resulting ID3D12PipelineState exposes no stage, state or input
    //     layout, and this file's own CreatePipelineFromDxil (d3d12_backend.cpp
    //     :2785) has zero callers in src/ -- it is not the guest draw's path.
    //     So DepthEnable, DepthWriteMask, DepthFunc, StencilEnable, BlendEnable,
    //     SrcBlend, DestBlend, RenderTargetWriteMask, CullMode, FillMode,
    //     FrontCounterClockwise, DepthClipEnable, MultisampleEnable,
    //     AntialiasedLineEnable and ConservativeRaster are READABLE NOWHERE from
    //     this layer, and they are named in `unreadable=` rather than left
    //     silently absent. The pipeline cache does publish them, as
    //     psoDesc.RasterizerState/BlendState/DepthStencilState copied verbatim
    //     from the key (pipeline_cache.cpp:1949-1951), which is another stream's
    //     file.
    //   inputLayoutReadable=0. D3D12_INPUT_ELEMENT_DESC is never turned into an
    //     ID3D12InputLayout anywhere in src/ -- CreateInputLayout has zero
    //     occurrences across the tree -- because the descriptors go straight
    //     into psoDesc.InputLayout (pipeline_cache.cpp:1910-1911). This file is
    //     handed a bare ID3D12PipelineState* and a DynamicMeshDesc, and
    //     DynamicMeshDesc carries no layout member (d3d12_backend.h:68-104), so
    //     the descriptors that built the bound PSO do not reach this layer at
    //     all. The one place in THIS file that materialises a descriptor array
    //     is BuildInputLayoutFromGrcFvf (:5393), which the live draw path does
    //     not call.
    // NOTHING HERE IS FORCED. No state is overwritten to make a draw visible,
    // no field is defaulted, no check is disabled: this row only reads.
    {
        const uint64_t cln = ++m_commandListBoundRows;
        if (ShouldLogRefusal(cln)) {
            const renderer::PipelineCacheStats cst = m_pipelineCache.Stats();
            MCLA_LOG_TRACE(
                "DRAW-COMMANDLIST-BOUND #{} drawsIssued={} pso={:p} "
                "psoIsTestPso={} drawCall=issued "
                "vp=({},{},{},{},{},{}) scissor=({},{},{},{}) "
                "topology={} indexed={} vertexCount={} indexCount={} "
                "startIndex={} baseVertex={} instances=1 "
                "vb=({:016X},{},{}) ibBound={} ib=({:016X},{},{}) "
                "rtvFormat=0x{:08X} "
                "gsOffered={} gsCreated={} gsCompileFailed={} gsUnbuildable={} "
                "gsStageObtained={} gsStageRefused={} cacheHits={} cacheMisses={} "
                "cacheRefusals={} "
                "psoDescReadable=0 inputLayoutReadable=0 "
                "unreadable=DepthEnable,DepthWriteMask,DepthFunc,StencilEnable,"
                "BlendEnable,SrcBlend,DestBlend,RenderTargetWriteMask,CullMode,"
                "FillMode,FrontCounterClockwise,DepthClipEnable,MultisampleEnable,"
                "AntialiasedLineEnable,ConservativeRaster "
                "-- vp/scissor/vb/ib/topology are the exact locals handed to the "
                "command list this draw was recorded on, and vb is in "
                "D3D12_VERTEX_BUFFER_VIEW field order (BufferLocation, "
                "SizeInBytes, StrideInBytes). gs* are this object's own "
                "PipelineCache totals, read after the draw call. psoDescReadable=0 "
                "because the guest draw's pipeline description is built and "
                "consumed in another translation unit and a D3D12 pipeline state "
                "reports none of its own fields; inputLayoutReadable=0 because "
                "input element descriptors are never turned into an input-layout "
                "object in this tree and none is carried into this layer. Both "
                "zeros are absences of an observation, NOT measurements of the "
                "state. Nothing here is forced, defaulted or clamped",
                cln, m_stats.drawsIssued, static_cast<const void*>(pso),
                (pso == m_testPipeline.Get()) ? 1u : 0u,
                viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                viewport.Height, viewport.MinDepth, viewport.MaxDepth,
                scissor.left, scissor.top, scissor.right, scissor.bottom,
                static_cast<uint32_t>(desc.topology), desc.indexed ? 1u : 0u,
                desc.vertexCount, desc.indexCount, desc.startIndexLocation,
                desc.baseVertexLocation,
                vbViews[0].BufferLocation, vbViews[0].SizeInBytes, vbViews[0].StrideInBytes,
                desc.indexed ? 1u : 0u, ibView.BufferLocation,
                ibView.SizeInBytes, static_cast<uint32_t>(ibView.Format),
                m_boundRtvFormat, renderer::RectangleDrawsOffered(),
                cst.geomStageCreated, cst.geomStageCompileFailed,
                cst.geomStageUnbuildable, cst.geomStageWaited,
                cst.geomStageWaitFailed, cst.hits, cst.misses, cst.refusals);
        }
    }

    // ---------------------------------------------------------------------
    // F-727: THE COPY IS RECORDED HERE, AFTER THE DRAW. It used to be recorded
    // ABOVE, immediately before the draw call, under a comment that said it was
    // "between the draw and the PRESENT transition". Both halves of that were
    // wrong, and the measurement was wrong with them.
    //
    // MEASURED from this file's own statement order, not inferred: the copy was
    // recorded at the top of the draw path, above DrawIndexedInstanced /
    // DrawInstanced, so the only writes that had reached the back buffer when
    // that copy executed were the placeholder clear (kPlaceholderBackBufferClear)
    // and anything earlier draws on the same list had left. The draw being
    // measured had NOT rasterised yet, so the sample described the surface
    // BEFORE the draw.
    //
    // The consequence is visible in build/w701.log. D3D12-DRAW-PIXEL #2 printed
    // rgba=(5,8,13,255), and kPlaceholderBackBufferClear is {0.02f, 0.03f, 0.05f,
    // 1.0f}, which quantises to 8-bit UNORM as exactly 5, 8, 13, 255. The census
    // on that same row agrees byte for byte with a surface filled by the clear:
    // sum=23961600 = 921600 * (5+8+13), maxChannel=13, nonBlack=921600. So that
    // row -- and the DRAW-CP-PIXEL row that re-emits it -- described the PLACEHOLDER
    // CLEAR, not a draw. It was never a "first confirmed non-black pixel value"
    // from the guest. The render_thread.cpp row still calls its value "a sample
    // of the surface THIS draw wrote"; that text is now true of the MECHANISM and
    // false of those two historical rows, which were taken before this move.
    //
    // Moving the copy down here also repairs a SECOND defect the old position
    // created, which nothing had named. RecordDrawReadback finishes by
    // transitioning the back buffer COPY_SOURCE -> PRESENT, so recording it
    // BEFORE the draw left the back buffer's tracked state at PRESENT while the
    // draw was then recorded against it -- and the conditional barrier below,
    // which is skipped when rbRecorded, never restored it. The draw is now
    // recorded while the tracked state is RENDER_TARGET, as D3D12 requires.
    //
    // SAFE, on both counts the move could plausibly have broken:
    //   * BINDINGS. ResourceBarrier and CopyTextureRegion do not disturb the PSO,
    //     root signature, root constants or RTV binding, so recording the copy
    //     after the draw leaves the draw the same binding state it would have
    //     had with the copy before it.
    //   * FENCE. The copy is still inside the SAME command list, and
    //     PresentOwnedFrame still Closes, Executes and Signals that list AFTER
    //     this point, so ReportDrawReadback's wait still covers this copy.
    //     Moving a recording a few statements later within one list cannot move
    //     it past the Close.
    // ---------------------------------------------------------------------
    drawIssuedRef = true;
    const bool rbRecorded = m_batchDrawFrames ? false : RecordDrawReadback();

    // 2026-10-02 (F-727): the four dead ends below were measured on this runtime
    // and all four are real. They are recorded HERE, next to the working copy, so
    // that a reader who finds the fifth path does not have to rediscover the
    // other four -- NOT because the readback is absent from this function. It is
    // the call above the draw.
    //   * a buffer on D3D12_HEAP_TYPE_DEFAULT is not CPU-mappable -- Map returns
    //     hr=0x80070057 (E_INVALIDARG) with ppData null;
    //   * moving it to D3D12_HEAP_TYPE_UPLOAD makes it an illegal copy
    //     DESTINATION and the device is REMOVED -- 0x887A0001, which is the same
    //     failure a previous agent reported and could not diagnose;
    //   * D3D12 has no D3D12_RESOURCE_STATE_COPY_HOST (um/d3d12.h:3231-3266);
    //   * a CUSTOM heap rejects CPU_PAGE_PROPERTY_UNKNOWN and
    //     MEMORY_POOL_UNKNOWN ("D3D12_HEAP_PROPERTIES is invalid", debug id
    //     628), so its only legal settings are NOT_AVAILABLE + POOL0/POOL1,
    //     which is GPU-only.
    //
    // 2026-10-02 (F-725): EVERY ONE OF THOSE IS CORRECT AND NONE OF THEM IS
    // THE POINT. All four failed because they tried to MAP something, or to copy
    // INTO something, rather than to COPY OUT of the back buffer into a resource
    // whose defining property is that it IS mappable. The heap that D3D12 gives
    // you for exactly this job is D3D12_HEAP_TYPE_READBACK, and it was the one
    // heap none of the four tried. Measured on this machine, RTX 3070,
    // driver-reported vendor=0x10DE device=0x2484, D3D12 debug layer ON, harness
    // at build/F725_pixharness.log:
    //   PATH A  Map() on a DEFAULT-heap buffer      -> 0x80070057 (dead end #1)
    //           debug id 0x336: "Map and Unmap can not be called on a resource
    //           associated with a heap that has the CPU page properties of
    //           D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE."
    //   PATH B  UPLOAD heap as a copy destination    -> 0x80070057 AT CREATE
    //           (dead end #2, and NOT the reported device removal: this harness
    //           measured a rejected create, then a refused
    //           GENERIC_READ->COPY_DEST transition at list->Close(), and never
    //           reached 0x887A0001)
    //   PATH C  D3D12_RESOURCE_STATE_COPY_HOST       -> does not compile; 16
    //           D3D12_RESOURCE_STATE_* enumerators exist and it is not one
    //   PATH D  CUSTOM heap                          -> 0x80070057; debug id 0x27E
    //           (638, not the 628 quoted above): "D3D12_HEAP_PROPERTIES is
    //           invalid. D3D12_CPU_PAGE_PROPERTY_UNKNOWN cannot be used with
    //           D3D12_HEAP_TYPE_CUSTOM." The NOT_AVAILABLE + L0 combination IS
    //           accepted and is still GPU-only: Map -> 0x80070057 again.
    //   PATH E  own R8G8B8A8_UNORM texture -> READBACK buffer   -> WORKS
    //   PATH C2 copy out of the FLIP_DISCARD back buffer into a READBACK
    //           buffer                                 -> ALSO WORKS
    //
    // C2 is the one this file uses, and it is the cheaper of the two: the guest's
    // draw already targets the back buffer, so the readback is one extra copy in
    // a command list that is already open, with no second render target and no
    // second PSO. The measurement contradicts a claim made above it in this very
    // comment, and it is kept above rather than deleted because a flip-model back
    // buffer being a legal copy SOURCE is not a thing one should have to
    // rediscover: CreateSwapChain's own note above still asks for this legality to
    // be "PROBED at runtime instead of assumed". It is probed here.
    //
    // The values read back are printed by the new D3D12-DRAW-PIXEL row, and the
    // same measurement is re-emitted after DRAW-CP-DRAWN by the render thread's
    // DRAW-CP-PIXEL row, so that a pixel sample exists with a timestamp LATER
    // than a draw -- which, until F-725, no row in the tree had.
    // F-725: THIS BARRIER IS CONDITIONAL, and it has to be. RecordDrawReadback
    // already took the back buffer RENDER_TARGET -> COPY_SOURCE -> PRESENT, so
    // after a recorded readback the tracked state is PRESENT and asserting
    // StateBefore=RENDER_TARGET here is a state MISMATCH.
    //
    // MEASURED, not reasoned: the F-725 harness hit exactly this when the
    // harness itself recorded the copy and then transitioned unconditionally.
    // The D3D12 debug layer named it, id 0x20F: "Using ResourceBarrier ...
    // Before state (0x4: D3D12_RESOURCE_STATE_RENDER_TARGET) of resource ...
    // does not match with the state (0x800: D3D12_RESOURCE_STATE_COPY_SOURCE)
    // specified in preceding ResourceBarrier or as InitialState", and the list
    // then failed to Close. So the fix is to skip the transition when the
    // readback already performed it, not to weaken the readback.
    if (!m_drawsIntoGuestRt && !m_batchDrawFrames && !rbRecorded) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = drawTarget;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &barrier);
    }
    // F-XXX: WHEN THE DRAWS LANDED IN THE GUEST'S RENDER TARGET, the back
    // buffer was never transitioned to RENDER_TARGET, so the RENDER_TARGET ->
    // PRESENT transition above must NOT be issued: it would assert a state the
    // resource does not have (the F-725 mismatch, id 0x20F, that this file
    // already documents). The back buffer is still PRESENT and the resolve
    // (inside PresentOwnedFrame) takes it PRESENT -> COPY_DEST -> PRESENT.

    // F-650: the draw path's own present, through the one sanctioned entry point.
    // This is the frame the console's scanout is meant to show, and the FRAME-OWNER
    // row it emits is what makes that claim checkable instead of assumed.
    //
    // F-899: UNLESS THE FRAME IS BATCHED. The guest's UI frame is one BURST of
    // draw commands followed by ONE swap (measured: 553,926 enqueued commands,
    // 3,400 XE_SWAP emits, build/g919.log). Presenting per draw would end the
    // frame 11,500 times per guest frame -- and, because BeginFrame() refuses a
    // second begin, the presents never happen at all: g919 shows begins=501
    // against 553,926 draws. So in batch mode this returns WITHOUT presenting,
    // the list stays open, and FlushDrawFrame() presents it once at the swap.
    if (m_batchDrawFrames) {
        m_batchDrawFrameHasDraw = true;
        return true;
    }
    const bool presented = PresentOwnedFrame(FrameOwner::Draw);
    // F-725: the readback copy, if one was recorded, was executed and fenced by
    // the call above. Now -- and only now -- is it safe to Map.
    if (rbRecorded) {
        ReportDrawReadback();
    }
    return presented;
}

// ===========================================================================
// F-725: THE POST-DRAW READBACK. Four dead ends were measured and all four are
// real (see the note at the end of DrawDynamicMeshWithPipeline). The fifth path
// -- CopyTextureRegion OUT of the back buffer into a D3D12_HEAP_TYPE_READBACK
// buffer -- was measured working on this machine and is implemented here.
//
// WHY THE READBACK HEAP IS THE WHOLE TRICK. A READBACK-heap resource is
// mappable by definition; that is the one property the other three heap types
// refuse. Nothing about the back buffer has to change: the guest's draw already
// targets it, so this costs one copy inside a command list that is already open.
// The harness's PATH C2 measured 32.0000% non-black pixels and maxChannel=214
// reading straight out of a DXGI_SWAP_EFFECT_FLIP_DISCARD back buffer, i.e. the
// "a flip back buffer is not a legal copy source" assumption is FALSE on this
// driver and the copy is one line.
//
// WHY IT IS BOUNDED TO kDrawReadbackMaxReadbacks ATTEMPTS. Each completed
// readback costs one GPU->CPU fence wait. An unbounded per-draw fence wait is
// exactly the cost-with-no-measurement this feature was created to remove, so it
// runs a fixed number of times and then switches itself off permanently and says
// so in the log. kDrawReadbackMaxReadbacks is a compile-time constant, not a
// runtime knob, because a runtime knob is one more thing that can differ between
// two builds whose logs are being compared.
// ===========================================================================

bool D3D12Backend::EnsureDrawReadback() {
    if (m_readbackDisabled) return false;
    if (m_readbackBuffer && m_readbackW == m_width && m_readbackH == m_height) {
        return true;
    }
    if (!m_device || m_width == 0 || m_height == 0) return false;

    m_readbackBuffer.Reset();

    // A buffer, so Format MUST be DXGI_FORMAT_UNKNOWN. MEASURED (F-725 harness,
    // D3D12 debug id 0x2E2): leaving the source texture's R8G8B8A8_UNORM on a
    // D3D12_RESOURCE_DIMENSION_BUFFER descriptor makes CreateCommittedResource
    // return 0x80070057 with "D3D12_RESOURCE_DESC::Format must be
    // DXGI_FORMAT_UNKNOWN when D3D12_RESOURCE_DESC::Dimension is
    // D3D12_RESOURCE_DIMENSION_BUFFER."
    D3D12_RESOURCE_DESC rbDesc = {};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.Width = static_cast<UINT64>(m_width) * m_height * 4u;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;

    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&m_readbackBuffer));
    if (FAILED(hr)) {
        m_readbackBuffer.Reset();
        m_readbackDisabled = true;
        ++m_drawReadbackCreateFailures;
        const uint64_t n = m_drawReadbackCreateFailures;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_WARN("D3D12-DRAW-PIXEL #{} create hr=0x{:08X} -- no pixel value "
                          "will be reported for any draw from now on; this is the "
                          "fifth readback path failing after four measured dead "
                          "ends, and it is counted rather than retried",
                          n, static_cast<uint32_t>(hr));
        }
        return false;
    }
    m_readbackBuffer->SetName(L"mcla-draw-readback");
    m_readbackW = m_width;
    m_readbackH = m_height;

    // The footprint must be taken from the SAMPLED SURFACE'S OWN descriptor,
    // not from the readback buffer's. MEASURED (F-725 harness, D3D12 debug id
    // 0x356): a footprint computed from the buffer descriptor carries
    // Format=UNKNOWN and CopyTextureRegion rejects it with "D3D12_SUBRESOURCE_
    // FOOTPRINT::Format is not supported at the current feature level". The
    // existing blit path gets this right by passing the texture descriptor
    // (:1544 above).
    //
    // F-XXX: THE SAMPLED SURFACE IS NOT ALWAYS THE BACK BUFFER. When the
    // guest's draws land in the guest's render target, that is the surface
    // the readback copies out of (.cpp :4454), so its descriptor is the one
    // the footprint must come from, and the row must NAME it. (In the
    // measured run both are 1280x720 R8G8B8A8_UNORM, so the footprint is the
    // same number either way -- the row is what would have been false.)
    ID3D12Resource* sampleDescSource =
        (m_drawsIntoGuestRt && m_guestRt)
            ? m_guestRt.Get()
            : m_renderTargets[m_frameIndex].Get();
    D3D12_RESOURCE_DESC backDesc = sampleDescSource
                                        ? sampleDescSource->GetDesc()
                                        : D3D12_RESOURCE_DESC();
    const char* sampleSourceName =
        (m_drawsIntoGuestRt && m_guestRt) ? "guest-rt" : "swapchain-back-buffer";
    UINT numRows = 0;
    UINT64 rowSize = 0, totalBytes = 0;
    m_device->GetCopyableFootprints(&backDesc, 0, 1, 0, &m_readbackFootprint, &numRows,
                                    &rowSize, &totalBytes);
    m_readbackRowPitch = m_readbackFootprint.Footprint.RowPitch;

    const uint64_t n = ++m_drawReadbackCreates;
    // backDesc.Flags is D3D12_RESOURCE_FLAGS, an enum, and fmt cannot format it
    // (measured: fmt::detail::type_is_unformattable_for<d3d12_resource_flags>).
    // Cast at every use rather than adding a formatter for one enum.
    const uint32_t backFlags = static_cast<uint32_t>(backDesc.Flags);
    MCLA_LOG_INFO("D3D12-DRAW-PIXEL #{} create hr=0x{:08X} heap=READBACK bytes={} "
                  "rowPitch={} rows={} surface={}x{} flags=0x{:X} -- a "
                  "D3D12_HEAP_TYPE_READBACK buffer IS mappable, which is the one "
                  "thing the DEFAULT, UPLOAD and CUSTOM heaps all refuse. "
                  "F-XXX APPENDED FIELDS: sampleSource={} -- the surface the "
                  "footprint above was taken from, which is the surface this "
                  "readback copies OUT of: the guest's render target when the "
                  "guest's draws land in it, the swap-chain back buffer "
                  "otherwise",
                  n, static_cast<uint32_t>(hr), totalBytes, m_readbackRowPitch,
                  numRows, m_width, m_height, backFlags, sampleSourceName);
    return true;
}

// ---------------------------------------------------------------------------
// F-727: THE COPY'S IDENTITY AND ITS POSITION, MEASURED. Two pieces of state
// that make the D3D12-DRAW-PIXEL row able to say WHICH draw a sample belongs to
// and WHETHER the copy was recorded after that draw, so that neither fact has to
// be re-derived by reading statement order again -- which is exactly how the
// pre-draw recording survived review with a comment asserting the opposite.
//
// File-scope statics rather than members, for the reason s_drawCensusInconsistent
// is a function-local static (see below): this file is not allowed to edit
// d3d12_backend.h, which another owner may be changing. The single-instance
// assumption is not an assumption here -- the process holds exactly one backend,
// the g_d3d12Backend above -- so file scope IS the instance scope for them.
//
// drawIssuedRef : set false at the top of the draw path and true immediately
//   after the draw call. RecordDrawReadback samples it. It is declared ABOVE
//   DrawDynamicMeshWithPipeline, at its first use, not here.
// s_readbackCopyAfterDraw : what it sampled, i.e. what the row prints.
// s_readbackDrawSeq : the F-653 draw ordinal the sample is attributed to.
// ---------------------------------------------------------------------------
static bool s_readbackCopyAfterDraw = false;
static uint64_t s_readbackDrawSeq = 0;

// F-728: THE FENCE VALUE THE READBACK MUST WAIT ON, CAPTURED WHERE IT IS
// SIGNALLed. This exists because the previous code re-derived it by indexing
// m_fenceValues AFTER PresentOwnedFrame had already advanced m_frameIndex, so
// the index named a slot two presents old whose value is <= the value just
// signalled and the wait was therefore skipped. Measured by the F-728 harness
// at %TEMP%\opencode\pixrace\ (output in pixrace_out.txt): with the old
// arithmetic the first submission printed target=0 completed=1 waited=0 and
// the probe returned the buffer's PREVIOUS contents while the census, run
// milliseconds later on the same Map, returned the copy's output -- i.e. the
// exact shape of build/w701.log's and build/w705.log's row #1, where the probe
// read (0,0,0,0) and the census read a fully written surface. With the value
// captured at the Signal, the same harness reports probeMismatches=0 and
// censusMismatches=0 over 12 runs.
//
// It is a file-scope static for the same reason s_readbackCopyAfterDraw is.
static uint64_t s_lastSignalledFenceValue = 0;

static uint32_t s_bmpSavedFs = 0;  // frames MCLA_BMP_DUMP has written (shared with the present path; bounds RecordDrawReadback)
static uint64_t s_lastBmpFrame = 0;
static std::chrono::steady_clock::time_point s_lastBmpTime{};

// Helper to peek whether a BMP dump is due under current cadence without mutating timers/counters.
static bool IsBmpDumpDue(uint64_t presentedFrames, bool noBound = false) {
    if (!noBound && s_bmpSavedFs >= BmpMax()) {
        return false;
    }
    const uint64_t bmpSkip = BmpSkip();
    if (presentedFrames < bmpSkip) {
        return false;
    }
    if (s_bmpSavedFs == 0) {
        return true;
    }
    const uint64_t bmpEveryMs = BmpEveryMs();
    if (bmpEveryMs != 0) {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastBmpTime).count()
                   >= static_cast<int64_t>(bmpEveryMs);
    }
    return presentedFrames >= s_lastBmpFrame + BmpEvery();
}

bool D3D12Backend::RecordDrawReadback() {
    if (m_readbackDisabled) return false;
    if (RenderScale() > 1) return false;  // footprint and the probe's pixel maths assume host == guest size
    // MCLA_BMP_DUMP needs readbacks past the cap to reach late frames (movie, menus).
    static const bool s_bmpWanted = [] { const char* e = std::getenv("MCLA_BMP_DUMP"); return e && e[0] == '1'; }();
    static const bool s_diagReadback = [] { const char* e = std::getenv("MCLA_LOG_LEVEL"); return e && e[0] != '6'; }();
    if (!s_bmpWanted && !s_diagReadback) return false;
    // MD-32: the BMP bypass of the readback cap ends once MCLA_BMP_MAX frames are written; MCLA_NO_BMP_READBACK_BOUND=1 = old unbounded.
    static const bool s_noBmpBound = [] { const char* e = std::getenv("MCLA_NO_BMP_READBACK_BOUND"); return e && e[0] == '1'; }();
    static const bool s_noBmpSkipBypassFix = [] { const char* e = std::getenv("MCLA_NO_BMP_SKIP_BYPASS_FIX"); return e && e[0] == '1'; }();
    const bool bmpBypass = s_bmpWanted && (
        s_noBmpSkipBypassFix
            ? (s_noBmpBound || s_bmpSavedFs < BmpMax())
            : IsBmpDumpDue(m_presentedFrames, s_noBmpBound)
    );
    static bool s_bmpBypassActiveLogged = false;
    if (bmpBypass && !s_bmpBypassActiveLogged) {
        s_bmpBypassActiveLogged = true;
        MCLA_LOG_INFO("D3D12-BMP-BYPASS active at presented={} (skip={} everyMs={} every={})",
                      m_presentedFrames, BmpSkip(), BmpEveryMs(), BmpEvery());
    }
    if (!bmpBypass && m_drawReadbackAttempts >= D3D12Backend::kDrawReadbackMaxReadbacks) {
        if (!m_readbackExhaustedLogged) {
            m_readbackExhaustedLogged = true;
            MCLA_LOG_INFO("D3D12-DRAW-PIXEL exhausted after {} readbacks "
                          "(kDrawReadbackMaxReadbacks={}) -- recording OFF for the "
                          "rest of the process. The bound is deliberate: each "
                          "readback is a GPU->CPU fence wait, and an unbounded "
                          "per-draw wait would be exactly the cost-with-no-"
                          "measurement this path exists to remove",
                          m_drawReadbackAttempts,
                          D3D12Backend::kDrawReadbackMaxReadbacks);
        }
        return false;
    }
    if (!m_commandList || !m_inFrame) return false;
    if (m_frameIndex >= kBufferCount || !m_renderTargets[m_frameIndex]) return false;
    if (!EnsureDrawReadback()) return false;

    // F-XXX: THE SAMPLE MUST DESCRIBE THE SURFACE THIS DRAW WROTE. When the
    // draws land in the guest's render target, that surface is the GUEST
    // RENDER TARGET; sampling the back buffer would measure the PREVIOUS
    // frame's content and the row would be a lie about the draw (the row's
    // whole purpose: "a sample of the surface THIS draw wrote").
    // TEMP F-907 BACKBUFFER-PROBE (env-gated, remove after the resolve path
    // is settled): MCLA_PROBE_READBACK_BB=1 samples the SCANOUT backbuffer
    // instead of the guest RT. The window shows black while the guest-RT
    // readback shows navy+UI; sampling the backbuffer names which side of
    // the resolve the content is lost on. Black here + UI in guest RT =
    // resolve/present drops it; UI here = the window was stale, not the
    // pipeline.
    bool probeBb = false;
    {
        static int s_probeBb = -1;
        if (s_probeBb < 0) {
            const char* e = std::getenv("MCLA_PROBE_READBACK_BB");
            s_probeBb = (e && e[0] == '1') ? 1 : 0;
        }
        probeBb = (s_probeBb == 1);
        if (probeBb) {
            MCLA_LOG_WARN("FLUSH-PROBE-BB sampling the scanout backbuffer "
                          "instead of the guest RT (frameIndex={})",
                          m_frameIndex);
        }
    }
    ID3D12Resource* sampleSource =
        (m_drawsIntoGuestRt && m_guestRt && !probeBb)
            ? m_guestRt.Get()
            : m_renderTargets[m_frameIndex].Get();
    if (!sampleSource) return false;

    static const bool s_noReadbackFmtGuard = [] {
        const char* e = std::getenv("MCLA_NO_READBACK_FORMAT_GUARD");
        return e && e[0] == '1';
    }();
    if (!s_noReadbackFmtGuard) {
        const D3D12_RESOURCE_DESC srcDesc = sampleSource->GetDesc();
        const auto& fp = m_readbackFootprint.Footprint;
        const UINT64 requiredBytes = m_readbackFootprint.Offset + static_cast<UINT64>(fp.RowPitch) * fp.Height;
        const UINT64 bufBytes = m_readbackBuffer ? m_readbackBuffer->GetDesc().Width : 0;
        if (srcDesc.Format != fp.Format || srcDesc.Width != fp.Width || srcDesc.Height != fp.Height || requiredBytes > bufBytes) {
            static std::atomic<uint32_t> s_skipFmtLogged{0};
            const uint32_t n = s_skipFmtLogged.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 16 || (n % 500) == 0) {
                MCLA_LOG_WARN("D3D12-READBACK-SKIP-FMT #{} srcFmt={} fpFmt={} srcDim={}x{} fpDim={}x{} reqB={} bufB={}",
                              n, static_cast<uint32_t>(srcDesc.Format), static_cast<uint32_t>(fp.Format),
                              srcDesc.Width, srcDesc.Height, fp.Width, fp.Height, requiredBytes, bufBytes);
            }
            return false;
        }
    }

    ++m_drawReadbackAttempts;

    // F-727: sample the ordering evidence BEFORE recording anything, so the row
    // describes the copy this call is about to record rather than some later one.
    // m_cachePipelineDraws was incremented at the top of the draw path, so its
    // value here is the most recent draw's ordinal.
    s_readbackCopyAfterDraw = s_drawIssuedOnList;
    s_readbackDrawSeq = static_cast<uint64_t>(m_cachePipelineDraws);

    const D3D12_RESOURCE_STATES sampleState =
        (m_drawsIntoGuestRt && !probeBb)
            ? D3D12_RESOURCE_STATE_RENDER_TARGET
            : (m_batchDrawFrames ? D3D12_RESOURCE_STATE_PRESENT
                                 : D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = sampleSource;
    b.Transition.StateBefore = sampleState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_readbackBuffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = m_readbackFootprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = sampleSource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Restore the sampled surface's prior state. A batched back-buffer draw
    // has already transitioned it to PRESENT; guest-RT and non-batched draws
    // leave their sample source in RENDER_TARGET. The F-907 probe samples the
    // backbuffer: do not touch the guest-RT tracked state for it.
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = sampleState;
    (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &b);
    if (m_drawsIntoGuestRt && !probeBb) m_guestRtState = sampleState;
    return true;
}

void D3D12Backend::LogRtCacheTable() {
    for (auto& kv : m_rtCache) {
        const auto& e = kv.second;
        MCLA_LOG_WARN("TEMP-RT-CACHE-TAB key={:X} base={} pitch={} msaa={} fmt={} {}x{} draws={} presentSrc={}",
                      kv.first, e.base, e.pitch, e.msaa, static_cast<uint32_t>(e.fmt), e.w, e.h, e.draws,
                      m_presentRt == &e ? 1 : 0);
    }
    for (auto& kv : m_dsCache)
        MCLA_LOG_WARN("TEMP-RT-CACHE-TAB-DS key={:X} {}x{} draws={}", kv.first, kv.second.w, kv.second.h, kv.second.draws);
    for (auto& kv : m_resolved)
        MCLA_LOG_WARN("TEMP-RT-CACHE-TAB-RESOLVED base={:08X} {}x{} fmt={} resolves={}", kv.first, kv.second.w,
                      kv.second.h, static_cast<uint32_t>(kv.second.fmt), kv.second.resolves);
}

DXGI_FORMAT D3D12Backend::GuestDepthDxgiFormat(bool hasRt, uint32_t rtDepthInfo) {
    static const bool on = [] { const char* e = std::getenv("MCLA_D32_DEPTH"); return e && e[0] == '1'; }();
    return (on && hasRt && ((rtDepthInfo >> 16) & 1u)) ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
}

static bool XuvRsDisabled() {  // MD-17 off-switch (the translator reads the same variable)
    static const bool s = [] { const char* e = std::getenv("MCLA_NO_XUV_RS"); return e && e[0] == '1'; }();
    return s;
}

// shortcut: a 1024-slot x 4-way table with oldest-eviction; > ~4000 distinct resolve bases would lose some (measured ~80 bases);
// a miss only leaves that slot at XE_RS=1.0 (the pre-MD-17 behaviour).
void D3D12Backend::NoteRsResolveBase(uint32_t base) {
    if (RenderScale() <= 1u || XuvRsDisabled() || base == 0) return;
    const uint32_t h = ((base >> 12) * 2654435761u) >> 22;  // 10 bits
    uint32_t victim = 0; uint64_t oldest = ~uint64_t(0);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t s = (h + i) & 1023u;
        const uint64_t v = m_rsResolveBases[s].load(std::memory_order_relaxed);
        if (uint32_t(v) == base || v == 0) { victim = s; break; }
        if (v < oldest) { oldest = v; victim = s; }
    }
    m_rsResolveBases[victim].store((uint64_t(m_presentedFrames) << 32) | base, std::memory_order_relaxed);
}

uint32_t D3D12Backend::XuvRsMask(const GuestTextureEntry* entries, uint32_t count, uint32_t& scaleOut) const {
    scaleOut = RenderScale();
    if (scaleOut <= 1u || XuvRsDisabled() || !entries) return 0;
    uint32_t mask = 0;
    for (uint32_t e = 0; e < count && e < kMaxGuestTextureDescriptors; ++e) {
        const uint8_t* d = entries[e].descriptorBytes;
        const uint32_t fmt = d[7] & 0x3Fu;
        if (fmt == 22u || fmt == 23u || entries[e].descriptorIndex >= 32u) continue;  // depth fetch: not a scaled colour resolve
        const uint32_t gb = ((uint32_t(d[4]) << 24) | (uint32_t(d[5]) << 16) | (uint32_t(d[6]) << 8) | uint32_t(d[7])) & 0xFFFFF000u;
        if (gb == 0) continue;
        const uint32_t h = ((gb >> 12) * 2654435761u) >> 22;
        for (uint32_t i = 0; i < 4; ++i) {
            if (uint32_t(m_rsResolveBases[(h + i) & 1023u].load(std::memory_order_relaxed)) == gb) { mask |= 1u << entries[e].descriptorIndex; break; }
        }
    }
    if (mask) {
        static std::atomic<uint64_t> s_n{0};
        const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 6 || (n % 20000) == 0) MCLA_LOG_WARN("XUV-RS-MASK #{} mask={:X} scale={} (off: MCLA_NO_XUV_RS=1)", n, mask, scaleOut);
    }
    return mask;
}

bool D3D12Backend::GetResolvedTexture(uint32_t guestBase, ID3D12Resource** res, DXGI_FORMAT* fmt) const {
    if (m_resolved.empty() && m_resolvedAlt.empty()) return false;
    const auto accept = [&](const ResolvedTex& rt) -> bool {
        if (!rt.valid || !rt.res) return false;
        if (!rt.isDepth && m_presentedFrames - rt.lastPresent > MaxResolveAgeFrames()) return false;
        if (res) *res = rt.res.Get();
        if (fmt) *fmt = rt.fmt;
        return true;
    };
    const auto tryKey = [&](uint32_t key) -> bool {
        auto it = m_resolved.find(key);
        if (it != m_resolved.end() && accept(it->second)) return true;
        auto alt = m_resolvedAlt.find(key);
        if (alt != m_resolvedAlt.end()) {
            for (const ResolvedTex& rt : alt->second) {
                if (accept(rt)) return true;
            }
        }
        return false;
    };
    if (auto lc = m_resolvedLastColour.find(guestBase); lc != m_resolvedLastColour.end()) {
        if (tryKey(lc->second)) return true;
    }
    for (uint32_t key : { guestBase | (28u << 2), guestBase | (10u << 2), guestBase | (24u << 2), guestBase | (11u << 2), guestBase, guestBase | 1u }) {
        if (tryKey(key)) return true;
    }
    return false;
}

bool D3D12Backend::ExecuteGuestResolve(const native::ResolveCommand& rc) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || !m_device) return false;
    static uint32_t s_n = 0;
    const uint32_t n = ++s_n;
    auto refuse = [&](const char* why) {
        ++m_rtResolveRefused;
        const uint64_t r = m_rtResolveRefused;
        if (r <= 24 || (r % 500) == 0)
            MCLA_LOG_WARN("TEMP-RESOLVE-REFUSE #{} why={} flags={:X} surf={:08X} color={:08X} depth={:08X} dest={:08X} destPitch={:08X} destInfo={:08X} copyCtl={:08X}",
                          r, why, rc.flags, rc.surfaceInfo, rc.colorInfo, rc.depthInfo, rc.copyDestBase,
                          rc.copyDestPitch, rc.copyDestInfo, rc.copyControl);
        return false;
    };
    if ((rc.flags & 7u) == 4u) {
        // Depth source: copy the cached depth surface (whole subresource -- D3D12 allows no box on
        // depth formats) into a sampleable R24G8_TYPELESS texture keyed by the guest dest base.
        // shortcut: guest 24_8 / float24 encodings are not converted; the texture holds the host
        // depth (D24 unorm) and the guest's dest dims are not honoured.
        const uint32_t dpitch = rc.surfaceInfo & 0x3FFFu;
        const uint64_t dkey = (1ull << 62) | (uint64_t(rc.depthInfo & 0x7FFu) << 32) | (uint64_t(dpitch) << 16) |
                              ((rc.depthInfo >> 16) & 1u);
        auto di = m_dsCache.find(dkey);
        if (di == m_dsCache.end() || !di->second.res) return refuse("depth-source-never-drawn");
        if (rc.copyDestBase == 0) return refuse("no-dest-base");
        if (!m_inFrame && !BeginFrameForDraw()) return refuse("begin-frame-false");
        GuestDsEntry& ds = di->second;
        // INV_RESOLVE (a): size the host depth dest from the guest dest pitch/height (RB_COPY_DEST_PITCH: width 14 bits at 0,
        // height 14 bits at 16) when that is larger than the source depth RT; the source is then copied with a box into the
        // dest (never CopyResource with unequal dims).
        uint32_t ddw = ds.w, ddh = ds.h;
        {
            const uint32_t gw = rc.copyDestPitch & 0x3FFFu, gh = (rc.copyDestPitch >> 16) & 0x3FFFu;
            if (gw <= 4096u && gh <= 4096u) { ddw = std::max(ds.w, gw); ddh = std::max(ds.h, gh); }
        }
        // MD-02: the guest packs several shadow cascades into ONE tiled 32bpp depth texture by resolving each to a different dest BASE
        // (st1_90.log: 1B754000 and 1B768000 = +0x14000 = 20 tiles of 4096 B = x+640 at pitch 1280, both "1280x1280 (src 640x720)", and the
        // guest then fetches 1B754000 as 1280x1280). Keying by base made each cascade its own texture (and each resolve cleared its whole dest),
        // so the fetch only ever saw the first cascade. A new base inside an existing same-size depth dest is resolved into it at the tile
        // offset (32x32 tile = 4096 B, row-major tiles, pitch-wide rows). MCLA_NO_DEPTH_SUBRECT=1 = old per-base textures.
        uint32_t effBase = rc.copyDestBase;
        int32_t effDX = rc.destX, effDY = rc.destY;
        bool aliasedSub = false;
        static const bool s_noDepthSubrect = [] { const char* e = std::getenv("MCLA_NO_DEPTH_SUBRECT"); return e && e[0] == '1'; }();
        if (!s_noDepthSubrect && (rc.copyDestBase & 0xFFFu) == 0 && !m_resolved.count(rc.copyDestBase | 1u)) {
            const uint32_t pitchPx = rc.copyDestPitch & 0x3FFFu;
            const uint32_t tpr = (pitchPx + 31u) >> 5;
            // MD-02b: the atlas is 2x2 cascades of 640x640 (CShadowMap ctor sub_825F6940 stores 640/640 at +592/+596). The
            // lower row (1BA74000 = 1B754000 + 640*1280*4, 1BA88000) carries guest dest pitch 1280x640, so its own ddh (720)
            // never equalled the parent's 1280 and its 720-row source overflowed sy=640: s1.log made it a separate 1280x720
            // texture the guest never fetches. Match by pitch only, clip the copy to the parent. MCLA_NO_CASCADE_ROW2=1 = old match.
            static const bool s_oldMatch = [] { const char* e = std::getenv("MCLA_NO_CASCADE_ROW2"); return e && e[0] == '1'; }();
            for (const auto& kv : m_resolved) {
                const ResolvedTex& par = kv.second;
                if (!(kv.first & 1u) || !par.res || !par.isDepth || tpr == 0 || ((par.w + 31u) >> 5) != tpr) continue;
                if (s_oldMatch && (par.w != ddw || par.h != ddh)) continue;
                const uint32_t pb = kv.first & ~1u;
                if (rc.copyDestBase <= pb) continue;
                const uint32_t t = (rc.copyDestBase - pb) >> 12;
                const uint32_t sx = (t % tpr) * 32u, sy = (t / tpr) * 32u;
                if (sy >= par.h || sx >= par.w) continue;
                if (s_oldMatch && (sx + ds.w > ddw || sy + ds.h > ddh)) continue;
                effBase = pb; effDX = rc.destX + static_cast<int32_t>(sx); effDY = rc.destY + static_cast<int32_t>(sy);
                // SHADOW-DESTPOINT: the guest passes the API dest point (640,0)/(0,640)/(640,640) measured from the PARENT base AND a
                // tile-adjusted copy_dest_base that already holds that offset, so adding both put every cascade at 1280 -> clamped to
                // (0,0): cascades 1-3 overwrote cascade 0 and the rest of the atlas stayed 0 = the whole road shadowed. When the point
                // already contains the tile offset, the point alone is the offset. MCLA_NO_SHADOW_DESTPOINT=1 = old sum.
                static const bool s_noDp = [] { const char* e = std::getenv("MCLA_NO_SHADOW_DESTPOINT"); return e && e[0] == '1'; }();
                if (!s_noDp) {
                    if (rc.destX >= static_cast<int32_t>(sx) && rc.destX - static_cast<int32_t>(sx) < 32) effDX = rc.destX;
                    if (rc.destY >= static_cast<int32_t>(sy) && rc.destY - static_cast<int32_t>(sy) < 32) effDY = rc.destY;
                }
                ddw = par.w; ddh = par.h;  // resolve INTO the parent; never resize it to the child's dims
                aliasedSub = true;
                static std::atomic<uint32_t> s_al{0};
                const uint32_t an = s_al.fetch_add(1) + 1;
                if (an <= 4 || (an % 2000) == 0)
                    MCLA_LOG_WARN("DEPTH-SUBRECT-ALIAS #{} dest={:08X} -> parent={:08X} at ({},{}) src {}x{} in {}x{}", an, rc.copyDestBase, pb, sx, sy, ds.w, ds.h, ddw, ddh);
                break;
            }
        }
        const uint32_t depthKey = effBase | 1u;
        if (m_resolved.size() >= 256 && !m_resolved.count(depthKey)) return refuse("resolved-cache-full");  // bound (AUDIT_MEMORY U2; ~80 distinct measured)
        ResolvedTex& dd = m_resolved[depthKey];  // depth copies keyed apart: the guest reuses one dest for colour AND depth (rb2.log 18DAC000)
        static const bool s_mirrorResolve = DepthCompareMirrored() && !(std::getenv("MCLA_OLD_DEPTH_RESOLVE") && std::getenv("MCLA_OLD_DEPTH_RESOLVE")[0] == '1');
        const bool dsD32 = ds.res->GetDesc().Format == DXGI_FORMAT_R32G8X24_TYPELESS;  // MD-04
        const DXGI_FORMAT wantDdFmt = s_mirrorResolve ? DXGI_FORMAT_R32_FLOAT : (dsD32 ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS);
        if (dd.res && (!dd.isDepth || dd.w != ddw || dd.h != ddh || dd.fmt != wantDdFmt)) {
            auto& altVec = m_resolvedAlt[depthKey];
            bool swapped = false;
            for (ResolvedTex& alt : altVec) {
                if (alt.res && alt.isDepth && alt.w == ddw && alt.h == ddh &&
                    alt.fmt == wantDdFmt) {
                    std::swap(dd, alt);
                    swapped = true;
                    break;
                }
            }
            if (!swapped) {
                if (altVec.size() >= kMaxResolvedAltPerKey) {
                    if (altVec.front().srvStagingSlot != UINT32_MAX) {
                        ReleaseStagingSrvSlot(altVec.front().srvStagingSlot);
                        altVec.front().srvStagingSlot = UINT32_MAX;
                    }
                    if (altVec.front().res) RetireTarget(altVec.front().res);
                    altVec.erase(altVec.begin());
                }
                altVec.push_back(std::move(dd));
                dd = ResolvedTex{};
            }
        }
        dd.lastPresent = m_presentedFrames;
        bool ddFresh = false;
        if (!dd.res || !dd.isDepth || dd.w != ddw || dd.h != ddh || dd.fmt != wantDdFmt) {
            if (dd.res) RetireTarget(dd.res);
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d = {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = uint64_t(ddw) * RenderScale(); d.Height = ddh * RenderScale(); d.DepthOrArraySize = 1; d.MipLevels = 1;
            d.Format = wantDdFmt;
            if (s_mirrorResolve) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            Microsoft::WRL::ComPtr<ID3D12Resource> r;
            const HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                                 D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
            if (FAILED(hr) || !r) { dd.res.Reset(); dd.valid = false; return refuse("depth-dest-create-failed"); }
            dd.res = r; dd.w = ddw; dd.h = ddh; dd.fmt = d.Format;
            dd.srvFmt = s_mirrorResolve ? DXGI_FORMAT_R32_FLOAT : (dsD32 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS); dd.isDepth = true;
            dd.state = D3D12_RESOURCE_STATE_COMMON; dd.stateAtFrameStart = dd.state;
            ddFresh = true;
            MCLA_LOG_WARN("TEMP-RESOLVE-DEPTH-DEST-NEW base={:08X} {}x{} (src {}x{}) guestDestPitch={:08X} guestDestInfo={:08X}",
                          rc.copyDestBase, ddw, ddh, ds.w, ds.h, rc.copyDestPitch, rc.copyDestInfo);
        }
        const UINT rs = RenderScale();
        // MD-02b: copy extent = source, clipped to the guest resolve rect and (atlas only) to the dest from the offset, so a
        // 640x720 EDRAM source does not spill 80 rows into the cascade below / get dropped back to (0,0).
        uint32_t cw = ds.w, ch = ds.h;
        if (aliasedSub || ddw > ds.w || ddh > ds.h) {
            if (rc.hasRect && rc.rect[2] > rc.rect[0] && rc.rect[3] > rc.rect[1]) {
                cw = std::min<uint32_t>(cw, uint32_t(rc.rect[2] - rc.rect[0]));
                ch = std::min<uint32_t>(ch, uint32_t(rc.rect[3] - rc.rect[1]));
            }
            if (effDX > 0 && uint32_t(effDX) < ddw) cw = std::min<uint32_t>(cw, ddw - uint32_t(effDX));
            if (effDY > 0 && uint32_t(effDY) < ddh) ch = std::min<uint32_t>(ch, ddh - uint32_t(effDY));
        }
        const UINT ddx = (effDX > 0 && static_cast<uint32_t>(effDX) + cw <= ddw) ? static_cast<UINT>(effDX) * rs : 0u;
        const UINT ddy = (effDY > 0 && static_cast<uint32_t>(effDY) + ch <= ddh) ? static_cast<UINT>(effDY) * rs : 0u;
        if (s_mirrorResolve) {
            if (StdZShadowFixEnabled() && ds.hasPendingClear) {
                SetTrackedState(ds.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                m_commandList->ClearDepthStencilView(ds.dsv, static_cast<D3D12_CLEAR_FLAGS>(ds.pendingClearFlags), ds.pendingClearZ, ds.pendingClearStencil, 0, nullptr);
                ds.hasPendingClear = false;
                ds.isMirrored = false;
            }
            D3D12_RESOURCE_BARRIER mb[2] = {};
            auto mtr = [&](int i, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
                mb[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                mb[i].Transition.pResource = res; mb[i].Transition.StateBefore = from; mb[i].Transition.StateAfter = to;
                mb[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            };
            mtr(0, ds.res.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            mtr(1, dd.res.Get(), dd.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            (++g_dcBarriers, m_commandList)->ResourceBarrier(2, mb);
            const bool needClear = !aliasedSub && (ddFresh || ddw > ds.w || ddh > ds.h);  // an aliased cascade must not wipe its siblings
            const bool invertDepth = StdZShadowFixEnabled() ? ds.isMirrored : true;
            const bool ok = MirrorDepthResolvePass(ds.res.Get(), dd.res.Get(), cw * rs, ch * rs, ddx, ddy, needClear, false, invertDepth);
            mtr(0, ds.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            mtr(1, dd.res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            (++g_dcBarriers, m_commandList)->ResourceBarrier(2, mb);
            static std::atomic<uint32_t> s_mn{0};
            const uint32_t mc = s_mn.fetch_add(1) + 1;
            if (mc <= 8 || (mc % 500) == 0) MCLA_LOG_ERROR("DEPTH-MIRROR-RESOLVE #{} ok={} base={:08X} {}x{} copy={}x{} at ({},{}) into={:08X} {}x{} invert={}", mc, ok ? 1 : 0, rc.copyDestBase, ds.w, ds.h, cw, ch, ddx, ddy, effBase, ddw, ddh, invertDepth ? 1 : 0);
        } else {
        D3D12_RESOURCE_BARRIER db[2] = {};
        UINT dn = 0;
        auto dtr = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            db[dn] = {};
            db[dn].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            db[dn].Transition.pResource = res;
            db[dn].Transition.StateBefore = from;
            db[dn].Transition.StateAfter = to;
            db[dn].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++dn;
        };
        dtr(ds.res.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (dd.state != D3D12_RESOURCE_STATE_COPY_DEST) dtr(dd.res.Get(), dd.state, D3D12_RESOURCE_STATE_COPY_DEST);
        (++g_dcBarriers, m_commandList)->ResourceBarrier(dn, db);
        dn = 0;
        D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
        dl.pResource = dd.res.Get(); dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.pResource = ds.res.Get(); sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        if (ddw == ds.w && ddh == ds.h && ddx == 0 && ddy == 0) {
            m_commandList->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        } else {
            const D3D12_BOX dbox = { 0u, 0u, 0u, cw * rs, ch * rs, 1u };  // source (clipped, MD-02b) into the dest
            m_commandList->CopyTextureRegion(&dl, ddx, ddy, 0, &sl, &dbox);
        }
        dtr(ds.res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        dtr(dd.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        (++g_dcBarriers, m_commandList)->ResourceBarrier(dn, db);
        }
        dd.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        dd.valid = true;
        ++dd.resolves;
        ++m_rtResolvesExec;
        // STENCIL-TEX: a depth resolve whose copy_dest_format is k_8_8_8_8 (6) is the guest's "stencil" render target
        // (RAGE CreateRenderTarget("stencil") at sub_822E5D40 -> [obj+408] -> motion blur var StencilTexture via
        // sub_8260E998). The guest reads its D24S8 bits as 8888, X byte = stencil (vehicle id: StreakMotionBlur PS
        // 6FA95F4486AA80FB indexes Mc4MotionBlurVehicleMtxs with 4*floor(stencil/4)). The host depth copy above holds
        // 1 - z only and is keyed apart, so a colour fetch of this base found the previous colour resolve (shadow mask,
        // 0.09..0.5) instead: vehicle-matrix index 20..128 -> garbage matrices -> full-screen streaks. Publish the
        // stencil as the latest colour content of the base. MCLA_NO_STENCIL_TEX=1 = old behaviour.
        // shortcut: stencil is written to all four channels (the depth bytes of Y/Z/W are dropped); only the X/stencil
        // byte is consumed by the measured reader.
        // 2026-11-24: default OFF — publishing the stencil at 1B3BC000 replaced the shadow mask the world PS reads there
        // (seed 0x2 road luma 35 with it, 85 without). Opt-in MCLA_STENCIL_TEX=1 until binding is time-ordered per draw.
        static const bool s_noStencilTex = [] { const char* e = std::getenv("MCLA_STENCIL_TEX"); return !(e && e[0] == '1'); }();
        if (!s_noStencilTex && ((rc.copyDestInfo >> 7) & 0x3Fu) == 6u && !aliasedSub &&
            (m_resolved.size() < 256 || m_resolved.count(rc.copyDestBase | (28u << 2)))) {
            const uint32_t sk = rc.copyDestBase | (28u << 2);
            ResolvedTex& st = m_resolved[sk];
            const auto fits = [&](const ResolvedTex& t) {
                return t.res && !t.isDepth && t.w == ddw && t.h == ddh && t.fmt == DXGI_FORMAT_R8G8B8A8_UNORM && t.rtFlag;
            };
            if (st.res && !fits(st)) {  // keep the colour entry's other sizes resident, as the colour path does
                auto& altVec = m_resolvedAlt[sk];
                bool swapped = false;
                for (ResolvedTex& alt : altVec)
                    if (fits(alt)) { std::swap(st, alt); swapped = true; break; }
                if (!swapped) {
                    if (altVec.size() >= kMaxResolvedAltPerKey) {
                        if (altVec.front().srvStagingSlot != UINT32_MAX) {
                            ReleaseStagingSrvSlot(altVec.front().srvStagingSlot);
                            altVec.front().srvStagingSlot = UINT32_MAX;
                        }
                        if (altVec.front().res) RetireTarget(altVec.front().res);
                        altVec.erase(altVec.begin());
                    }
                    altVec.push_back(std::move(st));
                    st = ResolvedTex{};
                }
            }
            bool stFresh = false;
            if (!st.res) {
                D3D12_HEAP_PROPERTIES hp = {};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC d = {};
                d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                d.Width = uint64_t(ddw) * rs; d.Height = ddh * rs; d.DepthOrArraySize = 1; d.MipLevels = 1;
                d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
                d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                Microsoft::WRL::ComPtr<ID3D12Resource> r;
                if (SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                                                nullptr, IID_PPV_ARGS(&r))) && r) {
                    st.res = r; st.w = ddw; st.h = ddh; st.fmt = st.srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
                    st.isDepth = false; st.rtFlag = true;
                    st.state = D3D12_RESOURCE_STATE_COMMON; st.stateAtFrameStart = st.state;
                    stFresh = true;
                }
            }
            if (st.res) {
                D3D12_RESOURCE_BARRIER sb[2] = {};
                auto str = [&](int i, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
                    sb[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    sb[i].Transition.pResource = res; sb[i].Transition.StateBefore = from; sb[i].Transition.StateAfter = to;
                    sb[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                };
                str(0, ds.res.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                str(1, st.res.Get(), st.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                (++g_dcBarriers, m_commandList)->ResourceBarrier(st.state == D3D12_RESOURCE_STATE_RENDER_TARGET ? 1 : 2, sb);
                const bool ok = MirrorDepthResolvePass(ds.res.Get(), st.res.Get(), cw * rs, ch * rs, ddx, ddy,
                                                       stFresh || ddw > ds.w || ddh > ds.h, true);
                str(0, ds.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                str(1, st.res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                (++g_dcBarriers, m_commandList)->ResourceBarrier(2, sb);
                st.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                st.valid = ok;
                st.lastPresent = m_presentedFrames;
                ++st.resolves;
                st.expBias = 0;
                st.destFormat = 6u;
                st.destSwap = (rc.copyDestInfo & (1u << 24)) != 0u;
                if (ok) m_resolvedLastColour[rc.copyDestBase] = sk;
                static std::atomic<uint32_t> s_stn{0};
                const uint32_t sn = s_stn.fetch_add(1) + 1;
                if (sn <= 4 || (sn % 2000) == 0)
                    MCLA_LOG_WARN("STENCIL-TEX #{} ok={} dest={:08X} {}x{} copy={}x{} destInfo={:08X} (off: MCLA_NO_STENCIL_TEX=1)",
                                  sn, ok ? 1 : 0, rc.copyDestBase, ddw, ddh, cw, ch, rc.copyDestInfo);
            }
        }
        if (rc.flags & 0x200u) {
            const float rawZ = GuestDepthClearToFloat(rc.depthClear, rc.depthInfo);
            if (StdZShadowFixEnabled()) {
                ds.hasPendingClear = true;
                ds.pendingClearZ = rawZ;
                ds.pendingClearStencil = static_cast<UINT8>(rc.depthClear & 0xFFu);
                ds.pendingClearFlags = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
                ds.lastClearList = m_beginFrameBegins;
            } else {
                const float z = MirrorDepth(rawZ);
                m_commandList->ClearDepthStencilView(ds.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, z,
                                                     static_cast<UINT8>(rc.depthClear & 0xFFu), 0, nullptr);
            }
        }
        if (n <= 40 || (n % 1000) == 0)
            MCLA_LOG_WARN("TEMP-RESOLVE-DEPTH #{} dsKey={:X} {}x{} -> dest={:08X} flags={:X} execTotal={}", n, dkey,
                          ds.w, ds.h, rc.copyDestBase, rc.flags, m_rtResolvesExec);
        BumpRtPassEpoch();
        return true;
    }
    const uint32_t pitch = rc.surfaceInfo & 0x3FFFu;
    const uint32_t msaa = (rc.surfaceInfo >> 16) & 3u;
    const uint32_t dxgiRaw = MapXenosColorFormatToDxgi((rc.colorInfo >> 16) & 0xFu);
    if (pitch == 0 || dxgiRaw == 0) return refuse("bad-source-geometry");
    const uint64_t key = (1ull << 62) | (uint64_t(rc.colorInfo & 0x7FFu) << 32) | (uint64_t(pitch) << 16) |
                         (uint64_t(msaa) << 8) | uint64_t(dxgiRaw & 0xFFu);
    auto it = m_rtCache.find(key);
    if (it == m_rtCache.end() || !it->second.res) return refuse("source-rt-never-drawn");
    GuestRtEntry& src = it->second;
    src.base = rc.colorInfo & 0x7FFu; src.pitch = pitch; src.msaa = msaa;

    int32_t x1 = 0, y1 = 0, x2 = static_cast<int32_t>(src.w), y2 = static_cast<int32_t>(src.h);
    if (rc.hasRect) { x1 = rc.rect[0]; y1 = rc.rect[1]; x2 = rc.rect[2]; y2 = rc.rect[3]; }
    x1 = std::clamp<int32_t>(x1, 0, static_cast<int32_t>(src.w));
    x2 = std::clamp<int32_t>(x2, 0, static_cast<int32_t>(src.w));
    y1 = std::clamp<int32_t>(y1, 0, static_cast<int32_t>(src.h));
    y2 = std::clamp<int32_t>(y2, 0, static_cast<int32_t>(src.h));
    if (x2 <= x1 || y2 <= y1) return refuse("empty-rect");
    uint32_t dw = rc.copyDestPitch & 0x3FFFu, dh = (rc.copyDestPitch >> 16) & 0x3FFFu;
    if (dw == 0 || dh == 0) { dw = static_cast<uint32_t>(x2 - x1); dh = static_cast<uint32_t>(y2 - y1); }
    const int32_t dx = std::max(rc.destX, 0), dy = std::max(rc.destY, 0);
    if (dx >= static_cast<int32_t>(dw) || dy >= static_cast<int32_t>(dh)) return refuse("dest-point-outside");
    x2 = std::min<int32_t>(x2, x1 + static_cast<int32_t>(dw) - dx);
    y2 = std::min<int32_t>(y2, y1 + static_cast<int32_t>(dh) - dy);
    {   // The dest pitch width is 32-aligned (rb118 TEMP-RBIND2: fetch 16x16 / 4x4 / 1x1 bound host 32x16 / 32x4 /
        // 32x1), and the shader samples 0..1 over the WHOLE host texture, so the luminance chain read padding.
        // Size the host texture to the written width when the pitch is exactly that width's 32-alignment.
        const uint32_t written = static_cast<uint32_t>(dx + (x2 - x1));
        if (written < dw && dw == ((written + 31u) & ~31u)) dw = written;
    }
    if (rc.copyDestBase == 0) return refuse("no-dest-base");

    if (!m_inFrame && !BeginFrameForDraw()) return refuse("begin-frame-false");

    // shortcut: the host texture has the SOURCE RT's DXGI format (no format conversion,
    // exp-bias or swizzle); the guest's dest format is only logged.
    const uint32_t colourKey = rc.copyDestBase | ((dxgiRaw & 0xFFu) << 2);  // base is 4K-aligned; bit0 = depth
    if (m_resolved.size() >= 256 && !m_resolved.count(colourKey)) return refuse("resolved-cache-full");  // bound (AUDIT_MEMORY U2)
    m_resolvedLastColour[rc.copyDestBase] = colourKey;
    ResolvedTex& dst = m_resolved[colourKey];
    // copy_dest_exp_bias: bits 16-21 signed (registers.h:878). Xenia multiplies the resolved colour by 2^bias (draw_util.cc:1104,
    // resolve copy shaders). Applied after the copy by ApplyResolveExpBias (blend-factor multiply); MCLA_TEMP_NO_EXPBIAS=1 = old plain copy.
    static const bool s_noExpBias = [] { const char* e = std::getenv("MCLA_TEMP_NO_EXPBIAS"); return e && e[0] == '1'; }();
    int32_t resolveBias = static_cast<int32_t>((rc.copyDestInfo >> 16) & 0x3Fu);
    if (resolveBias >= 32) resolveBias -= 64;
    // The RT-side half (RB_COLOR_INFO bits 20-25, Xenia d3d12_command_processor.cc:3299 scales the PS output by 2^bias) is NOT applied by this
    // host (fp16 RTs hold true values), so only the net 2^(resolveBias+rtBias) is applied; measured: every nonzero resolve bias is paired with the
    // opposite RT bias (-4/+4, -2/+2), i.e. net 0 and the plain copy was already right.
    const int32_t rtBias = (int32_t(rc.colorInfo << 6) >> 26);
    const int32_t netBias = resolveBias + rtBias;
    const bool wantBias = netBias != 0 && !s_noExpBias;
    if (dst.res && (dst.isDepth || dst.w != dw || dst.h != dh || dst.fmt != static_cast<DXGI_FORMAT>(dxgiRaw) || (wantBias && !dst.rtFlag))) {
        // A1: keep up to kMaxResolvedAltPerKey distinct sizes/formats resident per base|fmt key.
        auto& altVec = m_resolvedAlt[colourKey];
        bool swapped = false;
        for (ResolvedTex& alt : altVec) {
            if (alt.res && !alt.isDepth && alt.w == dw && alt.h == dh &&
                alt.fmt == static_cast<DXGI_FORMAT>(dxgiRaw) && (!wantBias || alt.rtFlag)) {
                std::swap(dst, alt);
                swapped = true;
                break;
            }
        }
        if (!swapped) {
            if (altVec.size() >= kMaxResolvedAltPerKey) {
                if (altVec.front().srvStagingSlot != UINT32_MAX) {
                    ReleaseStagingSrvSlot(altVec.front().srvStagingSlot);
                    altVec.front().srvStagingSlot = UINT32_MAX;
                }
                if (altVec.front().res) RetireTarget(altVec.front().res);
                altVec.erase(altVec.begin());
            }
            altVec.push_back(std::move(dst));
            dst = ResolvedTex{};
        }
    }
    dst.lastPresent = m_presentedFrames;
    NoteRsResolveBase(rc.copyDestBase);  // MD-17: slots bound to this base get XE_RS_<n> = RenderScale() in the next PS variant
    if (!dst.res || dst.isDepth || dst.w != dw || dst.h != dh || dst.fmt != static_cast<DXGI_FORMAT>(dxgiRaw) || (wantBias && !dst.rtFlag)) {
        if (dst.res) RetireTarget(dst.res);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = uint64_t(dw) * RenderScale(); d.Height = dh * RenderScale(); d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = static_cast<DXGI_FORMAT>(dxgiRaw);
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (wantBias) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        Microsoft::WRL::ComPtr<ID3D12Resource> r;
        const HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                             D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        if (FAILED(hr) || !r) { dst.res.Reset(); dst.valid = false; return refuse("dest-create-failed"); }
        dst.rtFlag = wantBias;
        dst.res = r; dst.w = dw; dst.h = dh; dst.fmt = static_cast<DXGI_FORMAT>(dxgiRaw);
        dst.srvFmt = dst.fmt; dst.isDepth = false;
        dst.state = D3D12_RESOURCE_STATE_COMMON; dst.stateAtFrameStart = dst.state;
        MCLA_LOG_WARN("TEMP-RESOLVE-DEST-NEW base={:08X} {}x{} dxgi={} guestDestInfo={:08X}", rc.copyDestBase, dw, dh,
                      dxgiRaw, rc.copyDestInfo);
    }
    {
        dst.expBias = resolveBias;
        dst.destFormat = static_cast<uint8_t>((rc.copyDestInfo >> 7) & 0x3Fu);
        dst.destSwap = (rc.copyDestInfo & (1u << 24)) != 0u;
        static std::set<uint64_t> s_biasSeen;
        if (s_biasSeen.size() < 512 && s_biasSeen.insert((uint64_t(rc.copyDestBase) << 8) | uint8_t(resolveBias)).second)
            MCLA_LOG_WARN("RESOLVE-EXP-BIAS dest={:08X} bias={} (rtBias={} net={} {}) destInfo={:08X} srcFmt={} rtColorExpBias={}",
                          rc.copyDestBase, resolveBias, rtBias, netBias, wantBias ? "applied" : "not applied", rc.copyDestInfo,
                          (rc.colorInfo >> 16) & 0xFu, rtBias);
    }

    D3D12_RESOURCE_STATES& srcState = RtStateRef(src);
    D3D12_RESOURCE_BARRIER b[2] = {};
    UINT nb = 0;
    auto tr = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        b[nb] = {};
        b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[nb].Transition.pResource = res;
        b[nb].Transition.StateBefore = from;
        b[nb].Transition.StateAfter = to;
        b[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++nb;
    };
    if (srcState != D3D12_RESOURCE_STATE_COPY_SOURCE) tr(src.res.Get(), srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (dst.state != D3D12_RESOURCE_STATE_COPY_DEST) tr(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_COPY_DEST);
    if (nb) (++g_dcBarriers, m_commandList)->ResourceBarrier(nb, b);
    nb = 0;
    D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
    dl.pResource = dst.res.Get(); dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.pResource = src.res.Get(); sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const UINT rs = RenderScale();  // guest rect -> host pixels, once
    const D3D12_BOX box = { static_cast<UINT>(x1) * rs, static_cast<UINT>(y1) * rs, 0, static_cast<UINT>(x2) * rs, static_cast<UINT>(y2) * rs, 1 };
    m_commandList->CopyTextureRegion(&dl, static_cast<UINT>(dx) * rs, static_cast<UINT>(dy) * rs, 0, &sl, &box);
    // A resolve rewrites the texture at this guest address: drop the per-frame descriptor->texture memo so no later bind this frame is served stale.
    static const bool s_noMemoFlush = [] { const char* e = std::getenv("MCLA_TEMP_NO_RESOLVE_MEMO_FLUSH"); return e && e[0] == '1'; }();
    if (!s_noMemoFlush) ClearFrameTextures();
    // PIXPROBE 3D snapshot: capture (MCLA_PIXPROBE_X, MCLA_PIXPROBE_Y) from the 3D scene RT
    // right as the resolve copies it out, before post-process/tonemap overwrites the output.
    if (PixProbeEnabled() && PixProbePassMode() != PixProbePass::kPost &&
        !s_pixprobe3dRbCapturedThisFrame && s_pixprobe3d.valid &&
        m_activeRt == &src && src.msaa == 0 && dw >= 640u && dh >= 360u &&
        m_frameIndex < kBufferCount) {
        uint32_t px = 0, py = 0;
        PixProbeXY(px, py);
        if (px < src.w && py < src.h) {
            D3D12_RESOURCE_DESC pxDesc = {};
            pxDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            pxDesc.Width = 1; pxDesc.Height = 1; pxDesc.DepthOrArraySize = 1; pxDesc.MipLevels = 1;
            pxDesc.Format = src.fmt; pxDesc.SampleDesc.Count = 1;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT pxFp = {};
            UINT64 pxTotal = 0;
            m_device->GetCopyableFootprints(&pxDesc, 0, 1, 0, &pxFp, nullptr, nullptr, &pxTotal);
            const UINT64 allocBytes = std::max<UINT64>(pxTotal, 512u);
            auto& rbBuf = m_pixprobe3dRb[m_frameIndex];
            if (!rbBuf || rbBuf->GetDesc().Width < allocBytes) {
                D3D12_HEAP_PROPERTIES hpRb = {};
                hpRb.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC bdRb = {};
                bdRb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bdRb.Width = allocBytes; bdRb.Height = 1; bdRb.DepthOrArraySize = 1; bdRb.MipLevels = 1;
                bdRb.SampleDesc.Count = 1; bdRb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (rbBuf) RetireTarget(rbBuf);
                rbBuf.Reset();
                (void)m_device->CreateCommittedResource(&hpRb, D3D12_HEAP_FLAG_NONE, &bdRb,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbBuf));
            }
            if (rbBuf && pxTotal > 0) {
                D3D12_TEXTURE_COPY_LOCATION plDst = {};
                plDst.pResource = rbBuf.Get();
                plDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                plDst.PlacedFootprint = pxFp;
                D3D12_TEXTURE_COPY_LOCATION plSrc = {};
                plSrc.pResource = src.res.Get();
                plSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                const D3D12_BOX pbox = { px * rs, py * rs, 0u, px * rs + 1u, py * rs + 1u, 1u };
                m_commandList->CopyTextureRegion(&plDst, 0, 0, 0, &plSrc, &pbox);
                m_pixprobe3dRbPending[m_frameIndex] = true;
                m_pixprobe3dRbFmt[m_frameIndex] = src.fmt;
                m_pixprobe3dRbBase[m_frameIndex] = src.base;
                m_pixprobe3dRbX[m_frameIndex] = px;
                m_pixprobe3dRbY[m_frameIndex] = py;
                s_pixprobe3dRbCapturedThisFrame = true;
                s_pixprobe3dRbSlot = m_frameIndex;
            }
        }
    }
    if (wantBias) ApplyResolveExpBias(dst.res.Get(), dst.fmt, netBias, dx * int32_t(rs), dy * int32_t(rs), (x2 - x1) * int32_t(rs), (y2 - y1) * int32_t(rs));
    // TEMP-RESOLVE readback of the destination (first resolves of each dest, then every 600th).
    // For multi-tile resolves (e.g. 1280x720 fmt=10 split into y=0..512 and y=512..720), only sample
    // after the final/bottom tile completes so the full frame is read back rather than top 512 rows only.
    ++dst.resolves;
    static const bool s_rbDiag = [] { const char* e = std::getenv("MCLA_LOG_LEVEL"); const char* b = std::getenv("MCLA_SCENE_BMP"); return (e && e[0] != '6') || (b && b[0] == '1'); }();
    static const bool s_radarRb = [] { const char* e = std::getenv("MCLA_TEMP_RESOLVE_RB"); return e && e[0] == '1'; }();
    const bool isSplitTile = (y1 > 0 || dy > 0 || y2 < static_cast<int32_t>(dh));
    const bool tileComplete = (dy + (y2 - y1)) >= static_cast<int32_t>(dh) || y2 >= static_cast<int32_t>(dh);
    const uint64_t rbCount = (isSplitTile && tileComplete && dst.resolves >= 2) ? (dst.resolves / 2) : dst.resolves;
    const bool sample = s_rbDiag && RenderScale() == 1 && tileComplete && !m_rsRbPending && !dst.isDepth && (rbCount == 1 || rbCount == 40 || (rbCount % 600) == 0 || ((rc.copyDestBase == 0x194EC000u || rc.copyDestBase == 0x1E098000u || (dst.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT && dw == 1280 && dh == 720)) && (rbCount % 25) == 0) || (rc.copyDestBase == 0x1E115000u && (rbCount % 10) == 0) || (s_radarRb && ((dw == 220 && dh == 220) || (dw == 128 && dh == 128 && (dst.resolves % 20) == 1)))) && (dw >= 128 || rc.copyDestBase == 0x1E115000u);  // TEMP-COMPOSITE-INPUT
    if (sample) {
        D3D12_RESOURCE_DESC dd = dst.res->GetDesc();
        UINT64 total = 0;
        m_device->GetCopyableFootprints(&dd, 0, 1, 0, &m_rsRbFp, &m_rsRbRows, &m_rsRbRowSize, &total);
        if (!m_rsRb || m_rsRbSize < total) {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (m_rsRb) RetireTarget(m_rsRb);
            m_rsRb.Reset();
            if (SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_rsRb))))
                m_rsRbSize = total;
        }
        if (m_rsRb && m_rsRbSize >= total) {
            tr(dst.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            (++g_dcBarriers, m_commandList)->ResourceBarrier(nb, b);
            nb = 0;
            D3D12_TEXTURE_COPY_LOCATION bl = {};
            bl.pResource = m_rsRb.Get(); bl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; bl.PlacedFootprint = m_rsRbFp;
            D3D12_TEXTURE_COPY_LOCATION sl2 = {};
            sl2.pResource = dst.res.Get(); sl2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            m_commandList->CopyTextureRegion(&bl, 0, 0, 0, &sl2, nullptr);
            tr(dst.res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            (++g_dcBarriers, m_commandList)->ResourceBarrier(nb, b);
            nb = 0;
            m_rsRbPending = true; m_rsRbBase = rc.copyDestBase; m_rsRbW = dw; m_rsRbH = dh; m_rsRbFmt = dst.fmt;
            m_rsRbIdx = n;
        }
    }
    // Leave the destination readable by shaders (phase 3) and the source as a render target.
    tr(dst.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tr(src.res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    (++g_dcBarriers, m_commandList)->ResourceBarrier(nb, b);
    dst.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    srcState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (!m_resMipOwner.empty() && !ResolvedMipsDisabled()) {  // MD-09: refresh this level inside its parent's mipped texture
        const auto ow = m_resMipOwner.find(rc.copyDestBase);
        if (ow != m_resMipOwner.end()) {
            const auto pc = m_resMips.find(static_cast<uint32_t>(ow->second >> 8));
            const uint32_t lv = static_cast<uint32_t>(ow->second & 0xFFu);
            if (pc != m_resMips.end() && pc->second.created && lv < pc->second.levels && pc->second.base[lv] == rc.copyDestBase)
                ResolvedMipsCopyLevel(pc->second, lv, dst, m_commandList.Get());
        }
    }
    // Clear-after-copy (flags 0x100 colour, 0x200 depth): the guest clears what it just resolved.
    if (rc.flags & 0x100u) {
        float col[4] = {0.f, 0.f, 0.f, 0.f};
        // shortcut: only 8_8_8_8 clear values are decoded (R = low byte); other Xenos
        // formats (2_10_10_10_FLOAT, 16_16_16_16_FLOAT packed) clear to 0 -- decode when a
        // visible mismatch is measured.
        const uint32_t guestFmt = (rc.colorInfo >> 16) & 0xFu;
        static const bool s_no7e3Clear = [] { const char* e = std::getenv("MCLA_NO_7E3_CLEAR"); return e && e[0] == '1'; }();
        if (static_cast<DXGI_FORMAT>(dxgiRaw) == DXGI_FORMAT_R8G8B8A8_UNORM)
            for (int i = 0; i < 4; ++i) col[i] = static_cast<float>((rc.colorClear[0] >> (8 * i)) & 0xFFu) / 255.0f;
        else if (!s_no7e3Clear && (guestFmt == 3u || guestFmt == 12u)) {
            // MD-23: k_2_10_10_10_FLOAT(_AS_16_16_16_16) clear = packed 7e3 RGB + 2-bit A (xenia d3d12_render_target_cache.cc:5317-5324).
            // 7e3 -> f32 per the DXBC path (dxbc_shader_translator_om.cc:3136-3140: mantissa at bit 16); xenos.cc:123 `<<3` disagrees.
            auto f7e3 = [](uint32_t v) -> float {
                v &= 0x3FFu;
                if (!v) return 0.0f;
                uint32_t m = v & 0x7Fu, e = v >> 7;
                if (!e) { const uint32_t lz = static_cast<uint32_t>(__builtin_clz(m)) - 24u; e = 1u - lz; m = (m << lz) & 0x7Fu; }
                const uint32_t bits = ((e + 124u) << 23) | (m << 16);
                float f; std::memcpy(&f, &bits, 4); return f;
            };
            for (int i = 0; i < 3; ++i) col[i] = f7e3(rc.colorClear[0] >> (10 * i));
            col[3] = static_cast<float>((rc.colorClear[0] >> 30) & 3u) / 3.0f;
            static std::atomic<uint32_t> s_c7{0};
            const uint32_t c7 = s_c7.fetch_add(1) + 1;
            if (c7 <= 4 || (c7 % 2000) == 0)
                MCLA_LOG_INFO("RESOLVE-CLEAR-7E3 #{} fmt={} clr={:08X} -> ({:.4f},{:.4f},{:.4f},{:.3f})", c7, guestFmt, rc.colorClear[0], col[0], col[1], col[2], col[3]);
        } else if (rc.colorClear[0] | rc.colorClear[1]) {  // MD-23 census (log-only): non-zero clear on a format that is NOT decoded (cleared to 0)
            static std::atomic<uint32_t> s_cn{0};
            const uint32_t cn = s_cn.fetch_add(1) + 1;
            if (cn <= 8 || (cn % 500) == 0)
                MCLA_LOG_WARN("RESOLVE-CLEAR-UNDECODED #{} dxgi={} fmt={} clr={:08X}/{:08X} (cleared to 0)", cn, dxgiRaw, guestFmt, rc.colorClear[0], rc.colorClear[1]);
        }
        const D3D12_RECT cr = { x1 * LONG(rs), y1 * LONG(rs), x2 * LONG(rs), y2 * LONG(rs) };
        m_commandList->ClearRenderTargetView(src.rtv, col, 1, &cr);
    }
    if (rc.flags & 0x200u) {
        const uint64_t dkey = (1ull << 62) | (uint64_t(rc.depthInfo & 0x7FFu) << 32) | (uint64_t(pitch) << 16) |
                              ((rc.depthInfo >> 16) & 1u);
        auto di = m_dsCache.find(dkey);
        if (di != m_dsCache.end() && di->second.res) {
            const float rawZ = GuestDepthClearToFloat(rc.depthClear, rc.depthInfo);
            if (StdZShadowFixEnabled()) {
                di->second.hasPendingClear = true;
                di->second.pendingClearZ = rawZ;
                di->second.pendingClearStencil = static_cast<UINT8>(rc.depthClear & 0xFFu);
                di->second.pendingClearFlags = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
                di->second.lastClearList = m_beginFrameBegins;
            } else {
                const float z = MirrorDepth(rawZ);
                const D3D12_RECT dcr = {
                    x1 * LONG(rs), y1 * LONG(rs),
                    std::min<LONG>(x2 * LONG(rs), LONG(di->second.w * rs)),
                    std::min<LONG>(y2 * LONG(rs), LONG(di->second.h * rs))
                };
                if (dcr.right > dcr.left && dcr.bottom > dcr.top) {
                    m_commandList->ClearDepthStencilView(di->second.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                         z, static_cast<UINT8>(rc.depthClear & 0xFFu), 1, &dcr);
                }
            }
        }
    }
    if (src.base == 1024 && m_edWorld0 == 0) m_edWorld0 = m_presentedFrames;
    if (src.base == 1024) {  // TEMP-RT-RESOLVE-ORDER
        static std::atomic<uint32_t> s_ro{0};
        const uint32_t q = s_ro.fetch_add(1) + 1;
        if (q <= 40 || (q % 100) == 0)
            MCLA_LOG_WARN("TEMP-RT-RESOLVE-ORDER #{} base=1024 list={} lastClearList={} drawsSinceClear={} totalDraws={} clearFlags={:X} dest={:08X}",
                          q, m_beginFrameBegins, src.lastClearList, src.drawsSinceClear, src.draws, rc.flags, rc.copyDestBase);
        src.drawsSinceClear = 0;
    }
    dst.valid = true;
    ++m_rtResolvesExec;
    // Front-buffer class resolve (full 1280x720 colour) feeds the present.
    if (dw >= 1280 && dh >= 720) m_presentRt = &src;
    if (n <= 24 || (n % 1000) == 0)
        MCLA_LOG_WARN("TEMP-RESOLVE #{} src(base={} pitch={} msaa={} fmt={} {}x{}) rect=({},{},{},{}) -> dest={:08X} {}x{} at ({},{}) destInfo={:08X} copyCtl={:08X} flags={:X} execTotal={}",
                      n, src.base, pitch, msaa, dxgiRaw, src.w, src.h, x1, y1, x2, y2, rc.copyDestBase, dw, dh, dx, dy,
                      rc.copyDestInfo, rc.copyControl, rc.flags, m_rtResolvesExec);
    if (g_f9State.load(std::memory_order_relaxed) == 2 && g_f9Draws.size() < 20000u) {  // F9: list resolves in submission order too (prim=0xFF)
        F9Draw d;
        d.seq = static_cast<uint64_t>(m_cachePipelineDraws); d.prim = 0xFFu; d.rtBase = src.base; d.rtW = src.w; d.rtH = src.h;
        d.tex = fmt::format("\n      RESOLVE src(base={:X} fmt={} msaa={}) rect=({},{},{},{}) -> dest={:08X} {}x{} destInfo={:08X} expBias={} flags={:X}",
                            src.base, dxgiRaw, msaa, x1, y1, x2, y2, rc.copyDestBase, dw, dh, rc.copyDestInfo,
                            int32_t(rc.copyDestInfo << 10) >> 26, rc.flags);
        g_f9Draws.push_back(std::move(d));
    }
    BumpRtPassEpoch();

    // Wave 2 §2: Targeted resolve-to-guest readback rules (no stall on non-matching resolves).
    static const bool s_noResolveReadback = [] {
        const char* e = std::getenv("MCLA_NO_RESOLVE_READBACK");
        return e && e[0] == '1';
    }();
    if (!s_noResolveReadback && tileComplete && !dst.isDepth && m_device && m_commandList && m_commandQueue && m_fence) {
        struct ResolveReadbackRule {
            const char* name;
            uint32_t baseMin;
            uint32_t baseMax;
            uint32_t w;
            uint32_t h;
            uint32_t xenosFmt;
            DXGI_FORMAT dxgiFmt;
            uint32_t bpp;
        };
        static constexpr ResolveReadbackRule kResolveReadbackRules[] = {
            { "vinyl-readback",  0x051A0000u, 0x051F7FFFu, 512u, 512u,  6u, DXGI_FORMAT_R8G8B8A8_UNORM, 4u },
            { "rain-vs11aad997", 0x0561F000u, 0x0563FFFFu, 128u, 128u, 37u, DXGI_FORMAT_R32G32_FLOAT,   8u },
        };
        const uint32_t destFmt = (rc.copyDestInfo >> 7) & 0x3Fu;
        const uint32_t destEndian = rc.copyDestInfo & 0x7u;
        for (const auto& rule : kResolveReadbackRules) {
            if (rc.copyDestBase < rule.baseMin || rc.copyDestBase > rule.baseMax ||
                dw != rule.w || dh != rule.h || destFmt != rule.xenosFmt) {
                continue;
            }
            ID3D12Resource* src1xRes = dst.res.Get();
            bool converted = false;
            if (RenderScale() != 1u || dst.fmt != rule.dxgiFmt) {
                if (EnsureResolveConvertPso(rule.dxgiFmt)) {
                    const bool needNew1x = (!m_resolve1xTex || m_resolve1xW != dw || m_resolve1xH != dh || m_resolve1xFmt != rule.dxgiFmt);
                    if (needNew1x) {
                        if (m_resolve1xTex) RetireTarget(m_resolve1xTex);
                        m_resolve1xTex.Reset();
                        D3D12_HEAP_PROPERTIES hp1 = {};
                        hp1.Type = D3D12_HEAP_TYPE_DEFAULT;
                        D3D12_RESOURCE_DESC rd1 = {};
                        rd1.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                        rd1.Width = dw;
                        rd1.Height = dh;
                        rd1.DepthOrArraySize = 1;
                        rd1.MipLevels = 1;
                        rd1.Format = rule.dxgiFmt;
                        rd1.SampleDesc.Count = 1;
                        rd1.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                        rd1.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                        if (SUCCEEDED(m_device->CreateCommittedResource(&hp1, D3D12_HEAP_FLAG_NONE, &rd1,
                                                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                        IID_PPV_ARGS(&m_resolve1xTex))) && m_resolve1xTex) {
                            m_resolve1xW = dw;
                            m_resolve1xH = dh;
                            m_resolve1xFmt = rule.dxgiFmt;
                        }
                    }
                    if (m_resolve1xTex && m_resolve1xRtvHeap && m_resolve1xSrvHeap) {
                        D3D12_CPU_DESCRIPTOR_HANDLE rtvH = m_resolve1xRtvHeap->GetCPUDescriptorHandleForHeapStart();
                        m_device->CreateRenderTargetView(m_resolve1xTex.Get(), nullptr, rtvH);
                        D3D12_SHADER_RESOURCE_VIEW_DESC srvD = {};
                        srvD.Format = dst.fmt;
                        srvD.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        srvD.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srvD.Texture2D.MipLevels = 1;
                        m_device->CreateShaderResourceView(dst.res.Get(), &srvD,
                                                           m_resolve1xSrvHeap->GetCPUDescriptorHandleForHeapStart());

                        D3D12_RESOURCE_BARRIER convBar = {};
                        convBar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        convBar.Transition.pResource = m_resolve1xTex.Get();
                        convBar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                        convBar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        convBar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &convBar);

                        ID3D12DescriptorHeap* heaps1[] = { m_resolve1xSrvHeap.Get() };
                        m_commandList->SetDescriptorHeaps(1, heaps1);
                        m_commandList->SetGraphicsRootSignature(m_blitRootSignature.Get());
                        m_commandList->SetPipelineState(
                            (rule.dxgiFmt == DXGI_FORMAT_R32G32_FLOAT) ? m_resolveConvPsoRg32f.Get() : m_resolveConvPsoRgba8.Get());
                        m_commandList->SetGraphicsRootDescriptorTable(0, m_resolve1xSrvHeap->GetGPUDescriptorHandleForHeapStart());
                        m_commandList->OMSetRenderTargets(1, &rtvH, FALSE, nullptr);
                        const D3D12_VIEWPORT vp1 = { 0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh), 0.0f, 1.0f };
                        const D3D12_RECT sc1 = { 0, 0, static_cast<LONG>(dw), static_cast<LONG>(dh) };
                        m_commandList->RSSetViewports(1, &vp1);
                        m_commandList->RSSetScissorRects(1, &sc1);
                        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        m_commandList->DrawInstanced(3, 1, 0, 0);

                        convBar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        convBar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &convBar);
                        InvalidateBoundState();
                        m_boundTargetsValid = false;
                        InvalidateDrawCaches();
                        src1xRes = m_resolve1xTex.Get();
                        converted = true;
                    }
                }
            }
            if (src1xRes && (converted || (RenderScale() == 1u && dst.fmt == rule.dxgiFmt))) {
                const D3D12_RESOURCE_DESC srcDesc1x = src1xRes->GetDesc();
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp1x = {};
                UINT rows1x = 0;
                UINT64 rowSize1x = 0, total1x = 0;
                m_device->GetCopyableFootprints(&srcDesc1x, 0, 1, 0, &fp1x, &rows1x, &rowSize1x, &total1x);
                if (total1x > 0 && (!m_resolveRbBuf || m_resolveRbBufSize < total1x)) {
                    if (m_resolveRbBuf) RetireTarget(m_resolveRbBuf);
                    m_resolveRbBuf.Reset();
                    D3D12_HEAP_PROPERTIES rhp = {};
                    rhp.Type = D3D12_HEAP_TYPE_READBACK;
                    D3D12_RESOURCE_DESC rbd = {};
                    rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    rbd.Width = total1x;
                    rbd.Height = 1;
                    rbd.DepthOrArraySize = 1;
                    rbd.MipLevels = 1;
                    rbd.SampleDesc.Count = 1;
                    rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    if (SUCCEEDED(m_device->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rbd,
                                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                    IID_PPV_ARGS(&m_resolveRbBuf))) && m_resolveRbBuf) {
                        m_resolveRbBufSize = total1x;
                    }
                }
                if (m_resolveRbBuf && m_resolveRbBufSize >= total1x) {
                    if (!converted) {
                        D3D12_RESOURCE_BARRIER rbb = {};
                        rbb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        rbb.Transition.pResource = src1xRes;
                        rbb.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                        rbb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        rbb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &rbb);
                    }
                    D3D12_TEXTURE_COPY_LOCATION bl1 = {};
                    bl1.pResource = m_resolveRbBuf.Get();
                    bl1.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    bl1.PlacedFootprint = fp1x;
                    D3D12_TEXTURE_COPY_LOCATION sl1 = {};
                    sl1.pResource = src1xRes;
                    sl1.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    m_commandList->CopyTextureRegion(&bl1, 0, 0, 0, &sl1, nullptr);

                    D3D12_RESOURCE_BARRIER rbbBack = {};
                    rbbBack.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    rbbBack.Transition.pResource = src1xRes;
                    rbbBack.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    rbbBack.Transition.StateAfter = converted ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    rbbBack.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &rbbBack);

                    if (FlushActiveCommandLists()) {
                        ++m_currentFenceValue;
                        if (SUCCEEDED(m_commandQueue->Signal(m_fence.Get(), m_currentFenceValue))) {
                            if (m_fence->GetCompletedValue() < m_currentFenceValue && m_fenceEvent) {
                                m_fence->SetEventOnCompletion(m_currentFenceValue, m_fenceEvent);
                                LARGE_INTEGER w0, w1, wf;  // MD-30 census (log-only): how often / how long this sync wait blocks
                                QueryPerformanceCounter(&w0);
                                WaitForSingleObject(m_fenceEvent, INFINITE);
                                QueryPerformanceCounter(&w1); QueryPerformanceFrequency(&wf);
                                static std::atomic<uint32_t> s_rw{0};
                                const uint32_t rwn = s_rw.fetch_add(1) + 1;
                                if (rwn <= 4 || (rwn % 200) == 0)
                                    MCLA_LOG_WARN("RESOLVE-RB-WAIT #{} rule={} base={:08X} waited_us={}", rwn, rule.name, rc.copyDestBase, (w1.QuadPart - w0.QuadPart) * 1000000 / wf.QuadPart);
                            }
                            if (m_currentFenceValue > m_fenceCompletedLB) m_fenceCompletedLB = m_currentFenceValue;
                            void* mapped = nullptr;
                            const D3D12_RANGE rr = { 0, static_cast<SIZE_T>(total1x) };
                            if (SUCCEEDED(m_resolveRbBuf->Map(0, &rr, &mapped)) && mapped) {
                                std::vector<uint8_t> linear(static_cast<size_t>(dw) * dh * rule.bpp);
                                const uint32_t rowBytes = dw * rule.bpp;
                                const auto* srcPtr = static_cast<const uint8_t*>(mapped);
                                for (uint32_t y = 0; y < dh; ++y) {
                                    std::memcpy(linear.data() + static_cast<size_t>(y) * rowBytes,
                                                srcPtr + static_cast<size_t>(y) * fp1x.Footprint.RowPitch,
                                                rowBytes);
                                }
                                const D3D12_RANGE wr = { 0, 0 };
                                m_resolveRbBuf->Unmap(0, &wr);

                                if (destEndian >= 1u && destEndian <= 3u) {
                                    ConvertTextureEndianness(destEndian, linear.data(),
                                                             static_cast<uint32_t>(linear.size()), 4u);
                                }
                                const uint32_t pitchTex = std::max(dw, (rc.copyDestPitch & 0x3FFFu));
                                const uint32_t alignedPitch = (pitchTex + 31u) & ~31u;
                                const uint32_t alignedH = (dh + 31u) & ~31u;
                                const uint32_t bpbLog2 = (rule.bpp == 8u) ? 3u : 2u;
                                std::vector<uint8_t> tiled(static_cast<size_t>(alignedPitch) * alignedH * rule.bpp, 0);
                                uint32_t maxEnd = 0;
                                for (uint32_t y = 0; y < dh; ++y) {
                                    for (uint32_t x = 0; x < dw; ++x) {
                                        const int32_t off = GetTiledOffset2D(
                                            static_cast<int32_t>(x), static_cast<int32_t>(y),
                                            alignedPitch, bpbLog2);
                                        if (off >= 0 && static_cast<size_t>(off) + rule.bpp <= tiled.size()) {
                                            std::memcpy(tiled.data() + off,
                                                        linear.data() + (static_cast<size_t>(y) * dw + x) * rule.bpp,
                                                        rule.bpp);
                                            maxEnd = std::max<uint32_t>(maxEnd, static_cast<uint32_t>(off) + rule.bpp);
                                        }
                                    }
                                }
                                if (maxEnd > 0) {
                                    const uint32_t guestVa = 0xA0000000u + rc.copyDestBase;
                                    auto& gmem = GetActiveGuestMemoryView();
                                    if (gmem.WriteBytes(guestVa, tiled.data(), maxEnd)) {
                                        static std::atomic<uint64_t> s_rbWrites{0};
                                        const uint64_t rw = s_rbWrites.fetch_add(1, std::memory_order_relaxed) + 1;
                                        if (rw <= 8 || (rw % 200) == 0) {
                                            MCLA_LOG_INFO("RESOLVE-READBACK #{} rule={} dest={:08X} va={:08X} {}x{} fmt={} endian={} bytes={}",
                                                          rw, rule.name, rc.copyDestBase, guestVa, dw, dh, destFmt, destEndian, maxEnd);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
    }
    return true;
}

bool D3D12Backend::EnsureResolveConvertPso(DXGI_FORMAT dstFmt) {
    if (!m_device) return false;
    if (dstFmt == DXGI_FORMAT_R32G32_FLOAT && m_resolveConvPsoRg32f && m_resolve1xRtvHeap && m_resolve1xSrvHeap) return true;
    if (dstFmt == DXGI_FORMAT_R8G8B8A8_UNORM && m_resolveConvPsoRgba8 && m_resolve1xRtvHeap && m_resolve1xSrvHeap) return true;
    if (!EnsureBlitPipeline() || !m_blitRootSignature) return false;

    if (!m_resolve1xRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_resolve1xRtvHeap)))) return false;
    }
    if (!m_resolve1xSrvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_resolve1xSrvHeap)))) return false;
    }

    const char* vsHlsl =
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut main(uint id : SV_VertexID) {\n"
        "    VSOut o;\n"
        "    o.uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    return o;\n"
        "}\n";
    const char* psHlsl =
        "Texture2D g_tex : register(t0);\n"
        "SamplerState g_sampler : register(s0);\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "float4 main(VSOut i) : SV_Target { return g_tex.Sample(g_sampler, i.uv); }\n";

    renderer::DxcRuntime dxc;
    std::string usedDir, error;
    if (!dxc.Load("", usedDir, error)) return false;
    std::vector<uint8_t> vsBlob, psBlob;
    if (!dxc.Compile(vsHlsl, "main", "vs_6_0", vsBlob, error) ||
        !dxc.Compile(psHlsl, "main", "ps_6_0", psBlob, error)) {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_blitRootSignature.Get();
    psoDesc.VS = { vsBlob.data(), vsBlob.size() };
    psoDesc.PS = { psBlob.data(), psBlob.size() };
    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.RTVFormats[0] = dstFmt;
    psoDesc.NumRenderTargets = 1;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    Microsoft::WRL::ComPtr<ID3D12PipelineState>& targetPso =
        (dstFmt == DXGI_FORMAT_R32G32_FLOAT) ? m_resolveConvPsoRg32f : m_resolveConvPsoRgba8;
    return SUCCEEDED(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&targetPso))) && targetPso;
}

bool D3D12Backend::EnsureOcclusionQueryResources() {
    if (m_occQueryHeap && m_occReadbackBuf && m_occReadbackMap) return true;
    if (!m_device) return false;
    if (!m_occQueryHeap) {
        D3D12_QUERY_HEAP_DESC qhd = {};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        qhd.Count = kMaxOcclusionQuerySlots;
        if (FAILED(m_device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&m_occQueryHeap))) || !m_occQueryHeap) {
            return false;
        }
    }
    if (!m_occReadbackBuf) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = static_cast<UINT64>(kMaxOcclusionQuerySlots) * sizeof(uint64_t);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&m_occReadbackBuf))) || !m_occReadbackBuf) {
            return false;
        }
        void* mapped = nullptr;
        const D3D12_RANGE rr = { 0, 0 };
        if (FAILED(m_occReadbackBuf->Map(0, &rr, &mapped)) || !mapped) {
            m_occReadbackBuf.Reset();
            return false;
        }
        m_occReadbackMap = static_cast<const uint64_t*>(mapped);
    }
    return true;
}

static bool OccCloseFixEnabled() {
    static const bool s_on = [] {
        const char* e = std::getenv("MCLA_NO_OCC_CLOSE_FIX");
        return !(e && e[0] == '1');
    }();
    return s_on;
}

void D3D12Backend::CloseActiveOcclusionQueriesBeforeClose() {
    if (!OccCloseFixEnabled() || !m_commandList || !m_mainListOpen || m_activeOccSlots.empty()) return;
    const uint32_t count = static_cast<uint32_t>(m_activeOccSlots.size());
    for (const auto& [va, slot] : m_activeOccSlots) {
        m_commandList->EndQuery(m_occQueryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slot);
        m_commandList->ResolveQueryData(m_occQueryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                        slot, 1, m_occReadbackBuf.Get(),
                                        static_cast<UINT64>(slot) * sizeof(uint64_t));
        m_autoClosedOccSlots[va] = { slot, UINT64_MAX };
    }
    m_activeOccSlots.clear();
    static std::atomic<uint32_t> s_occAutoClose{0};
    const uint32_t acN = s_occAutoClose.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (acN <= 8u || (acN % 50u) == 0u) {
        MCLA_LOG_INFO("OCC-AUTOCLOSE #{} auto-closed and resolved {} active query slot(s) before list close", acN, count);
    }
}

bool D3D12Backend::ExecuteQueryOp(const QueryOpCommand& qc) {
    static const bool s_noHwZpd = [] {
        const char* e = std::getenv("MCLA_NO_HW_ZPD");
        const char* e2 = std::getenv("MCLA_NO_OCCLUSION_QUERY");
        return (e && e[0] == '1') || (e2 && e2[0] == '1');
    }();
    if (s_noHwZpd) return false;
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || !m_device || !m_commandList) return false;
    if (m_mtRecorderCount > 1) {
        bool anyMtPending = false;
        for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
            if (m_mtRecorderState[r].drawIssued) { anyMtPending = true; break; }
        }
        if (anyMtPending) FlushActiveCommandLists();
    }
    if (!m_inFrame && !BeginFrameForDraw()) return false;
    if (!EnsureOcclusionQueryResources()) return false;

    if (qc.flags == static_cast<uint8_t>(QueryOpCommand::Op::Begin)) {
        if (OccCloseFixEnabled()) {
            m_autoClosedOccSlots.erase(qc.queryVa);
            auto activeIt = m_activeOccSlots.find(qc.queryVa);
            if (activeIt != m_activeOccSlots.end()) {
                const uint32_t oldSlot = activeIt->second;
                m_commandList->EndQuery(m_occQueryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, oldSlot);
                m_commandList->ResolveQueryData(m_occQueryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                                oldSlot, 1, m_occReadbackBuf.Get(),
                                                static_cast<UINT64>(oldSlot) * sizeof(uint64_t));
                m_activeOccSlots.erase(activeIt);
            }
        }
        const uint32_t slot = (m_occNextSlot++) % kMaxOcclusionQuerySlots;
        m_activeOccSlots[qc.queryVa] = slot;
        m_commandList->BeginQuery(m_occQueryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slot);
        g_occBeginDraws[qc.queryVa] = g_occDrawMono.load(std::memory_order_relaxed);
        return true;
    }
    if (qc.flags == static_cast<uint8_t>(QueryOpCommand::Op::End)) {
        if (OccCloseFixEnabled()) {
            auto autoIt = m_autoClosedOccSlots.find(qc.queryVa);
            if (autoIt != m_autoClosedOccSlots.end()) {
                const AutoClosedOccQuery ac = autoIt->second;
                m_autoClosedOccSlots.erase(autoIt);
                m_pendingOccQueries.push_back({ ac.slot, qc.queryVa, qc.reportVa, ac.fenceValue });
                return true;
            }
        }
        auto it = m_activeOccSlots.find(qc.queryVa);
        if (it == m_activeOccSlots.end()) return false;
        const uint32_t slot = it->second;
        m_activeOccSlots.erase(it);
        {   // MD-12 census (log-only): how many draws did the guest bracket between Begin and End?
            const auto bd = g_occBeginDraws.find(qc.queryVa);
            const uint64_t between = bd == g_occBeginDraws.end() ? ~0ull : g_occDrawMono.load(std::memory_order_relaxed) - bd->second;
            static std::atomic<uint32_t> s_ob{0};
            const uint32_t on = s_ob.fetch_add(1) + 1;
            if (on <= 16 || (on % 500) == 0)
                MCLA_LOG_WARN("OCC-BRACKET #{} va={:08X} slot={} drawsBetween={}", on, qc.queryVa, slot, between);
        }
        m_commandList->EndQuery(m_occQueryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slot);
        m_commandList->ResolveQueryData(m_occQueryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                        slot, 1, m_occReadbackBuf.Get(),
                                        static_cast<UINT64>(slot) * sizeof(uint64_t));
        m_pendingOccQueries.push_back({ slot, qc.queryVa, qc.reportVa, UINT64_MAX });
        return true;
    }
    return false;
}

void D3D12Backend::PollPendingOcclusionQueries() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (m_pendingOccQueries.empty() || !m_fence || !m_occReadbackMap) return;
    const uint64_t done = m_fence->GetCompletedValue();
    if (done > m_fenceCompletedLB) m_fenceCompletedLB = done;
    auto bswap32 = [](uint32_t v) -> uint32_t {
        return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
               ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
    };
    auto& gmem = GetActiveGuestMemoryView();
    size_t writeIdx = 0;
    for (size_t i = 0; i < m_pendingOccQueries.size(); ++i) {
        const PendingOccQuery& q = m_pendingOccQueries[i];
        if (q.fenceValue == UINT64_MAX || q.fenceValue > done) {
            m_pendingOccQueries[writeIdx++] = q;
            continue;
        }
        const uint64_t raw = m_occReadbackMap[q.slot];
        const uint32_t zpass = static_cast<uint32_t>(std::min<uint64_t>(raw, 0xFFFFFFFFull));
        if (uint8_t* ptr = gmem.GetHostPtrMutable(q.reportVa, 64u)) {
            LONG* p16 = reinterpret_cast<LONG*>(ptr + 16);
            const LONG expectedBe = static_cast<LONG>(bswap32(0xFFFFFEEDu));
            const LONG replacementBe = static_cast<LONG>(bswap32(zpass));
            {   // MD-12 census (log-only): xe_gpu_depth_sample_counts (.research/xenia/src/xenia/gpu/xenos.h:1525-1540): Total_A=+0, ZPass_A=+16, ZPass_B=+20
                static std::atomic<uint32_t> s_zr{0};
                const uint32_t zn = s_zr.fetch_add(1) + 1;
                if (zn <= 8 || (zn % 2000) == 0) {
                    uint32_t cur16 = 0, tot0 = 0; std::memcpy(&cur16, ptr + 16, 4); std::memcpy(&tot0, ptr, 4);
                    MCLA_LOG_WARN("ZPD-REPORT #{} va={:08X} zpass={} cur+16={:08X} sentinel={} total+0={:08X}", zn, q.reportVa, zpass, cur16,
                                  cur16 == static_cast<uint32_t>(expectedBe) ? "present" : "ABSENT-cas-fails", tot0);
                }
            }
            if (InterlockedCompareExchange(p16, replacementBe, expectedBe) == expectedBe) {
                uint32_t zero = 0;
                std::memcpy(ptr + 20, &zero, 4);
                std::memcpy(ptr + 48, &zero, 4);
                std::memcpy(ptr + 52, &zero, 4);
                std::atomic_thread_fence(std::memory_order_release);
            }
        }
    }
    m_pendingOccQueries.resize(writeIdx);
}

// Guest D3DDevice_Clear/ClearF, executed in queue order. Only targets the draws already created are cleared
// (a never-drawn RT has nothing stale to wipe; the placeholder auto-clear covers its first draw).
// A FULL-target clear marks lastClearList so the per-list placeholder auto-clear does not wipe it again;
// partial-rect clears do not (the placeholder still runs for the rest). Colour: RT0 only.
// shortcut: rects are host pixels 1:1 with guest pixels (no MSAA scale); MRT1-3 colour clears ignored.
void D3D12Backend::ExecuteGuestClear(const native::ClearCommand& cc) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || !m_device) return;
    static std::atomic<uint32_t> s_seen{0}, s_exec{0}, s_noTarget{0}, s_radar{0}, s_base168{0}, s_depth0{0};
    const uint32_t seen = s_seen.fetch_add(1) + 1;
    const uint32_t pitch = cc.surfaceInfo & 0x3FFFu;
    const uint32_t msaa = (cc.surfaceInfo >> 16) & 3u;
    const uint32_t dxgiRaw = MapXenosColorFormatToDxgi((cc.colorInfo >> 16) & 0xFu);
    GuestRtEntry* rt = nullptr;
    GuestDsEntry* ds = nullptr;
    if ((cc.flags & 1u) && pitch && dxgiRaw) {
        const uint64_t key = (1ull << 62) | (uint64_t(cc.colorInfo & 0x7FFu) << 32) | (uint64_t(pitch) << 16) |
                             (uint64_t(msaa) << 8) | uint64_t(dxgiRaw & 0xFFu);
        auto it = m_rtCache.find(key);
        if (it != m_rtCache.end() && it->second.res) rt = &it->second;
    }
    if ((cc.flags & 0x30u) && pitch) {
        const uint64_t dkey = (1ull << 62) | (uint64_t(cc.depthInfo & 0x7FFu) << 32) | (uint64_t(pitch) << 16) |
                              ((cc.depthInfo >> 16) & 1u);
        auto di = m_dsCache.find(dkey);
        if (di != m_dsCache.end() && di->second.res) ds = &di->second;
    }
    if (!rt && !ds) {
        const uint32_t q = s_noTarget.fetch_add(1) + 1;
        if (q <= 8 || (q % 1000) == 0)
            MCLA_LOG_ERROR("GUEST-CLEAR no-target #{} flags={:X} color={:08X} depth={:08X} pitch={}", q, cc.flags,
                           cc.colorInfo, cc.depthInfo, pitch);
        return;
    }
    if (!m_inFrame && !BeginFrameForDraw()) return;
    const LONG rs = static_cast<LONG>(RenderScale());
    auto rects = [&](uint32_t w, uint32_t h, D3D12_RECT* out) -> UINT {
        UINT n = 0;
        for (uint32_t i = 0; i < cc.nRects && i < 8; ++i) {
            const LONG x1 = std::clamp<LONG>(cc.rect[i][0], 0, LONG(w)), y1 = std::clamp<LONG>(cc.rect[i][1], 0, LONG(h));
            const LONG x2 = std::clamp<LONG>(cc.rect[i][2], 0, LONG(w)), y2 = std::clamp<LONG>(cc.rect[i][3], 0, LONG(h));
            if (x2 > x1 && y2 > y1) out[n++] = { x1 * rs, y1 * rs, x2 * rs, y2 * rs };  // guest -> host pixels
        }
        return n;
    };
    auto full = [&](uint32_t w, uint32_t h, UINT n, const D3D12_RECT* r) {
        if (cc.nRects == 0) return true;
        for (UINT i = 0; i < n; ++i) if (r[i].left <= 0 && r[i].top <= 0 && r[i].right >= LONG(w) * rs && r[i].bottom >= LONG(h) * rs) return true;  // r[] is host pixels
        return false;
    };
    D3D12_RECT rr[8];
    if (rt) {
        const UINT n = rects(rt->w, rt->h, rr);
        if (cc.nRects == 0 || n) {
            D3D12_RESOURCE_STATES& st = RtStateRef(*rt);
            if (st != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = rt->res.Get();
                b.Transition.StateBefore = st;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &b);
                st = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            m_commandList->ClearRenderTargetView(rt->rtv, cc.color, cc.nRects ? n : 0, cc.nRects ? rr : nullptr);
            if (full(rt->w, rt->h, n, rr)) rt->lastClearList = m_beginFrameBegins;
            rt->drawsSinceClear = 0;
            if (rt->w == 220 && rt->h == 220) s_radar.fetch_add(1);
            if ((cc.colorInfo & 0x7FFu) == 168) s_base168.fetch_add(1);
        }
    }
    if (ds) {
        const UINT n = rects(ds->w, ds->h, rr);
        if (StdZShadowFixEnabled() && cc.nRects == 0) {
            const D3D12_CLEAR_FLAGS f = ((cc.flags & 0x10u) ? D3D12_CLEAR_FLAG_DEPTH : D3D12_CLEAR_FLAGS(0)) |
                                        ((cc.flags & 0x20u) ? D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAGS(0));
            ds->hasPendingClear = true;
            ds->pendingClearZ = cc.z;
            ds->pendingClearStencil = static_cast<UINT8>(cc.stencil & 0xFFu);
            ds->pendingClearFlags = f;
            ds->lastClearList = m_beginFrameBegins;
            if ((cc.depthInfo & 0x7FFu) == 0) s_depth0.fetch_add(1);
        } else if (cc.nRects == 0 || n) {
            if (StdZShadowFixEnabled() && ds->hasPendingClear) {
                const float pz = ds->isMirrored ? MirrorDepth(ds->pendingClearZ) : ds->pendingClearZ;
                SetTrackedState(ds->res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                m_commandList->ClearDepthStencilView(ds->dsv, static_cast<D3D12_CLEAR_FLAGS>(ds->pendingClearFlags), pz, ds->pendingClearStencil, 0, nullptr);
                ds->hasPendingClear = false;
            }
            const D3D12_CLEAR_FLAGS f = ((cc.flags & 0x10u) ? D3D12_CLEAR_FLAG_DEPTH : D3D12_CLEAR_FLAGS(0)) |
                                        ((cc.flags & 0x20u) ? D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAGS(0));
            const float cz = (StdZShadowFixEnabled() && !ds->isMirrored) ? cc.z : MirrorDepth(cc.z);
            m_commandList->ClearDepthStencilView(ds->dsv, f, cz, static_cast<UINT8>(cc.stencil & 0xFFu),
                                                 cc.nRects ? n : 0, cc.nRects ? rr : nullptr);
            // placeholder auto-clear is depth+stencil together: only skip it when both were cleared
            if ((cc.flags & 0x30u) == 0x30u && full(ds->w, ds->h, n, rr)) ds->lastClearList = m_beginFrameBegins;
            if ((cc.depthInfo & 0x7FFu) == 0) s_depth0.fetch_add(1);
        }
    }
    if (rt || ds) {
        BumpRtPassEpoch();
    }
    const uint32_t e = s_exec.fetch_add(1) + 1;
    if (e <= 8 || (e % 500) == 0)
        MCLA_LOG_ERROR("GUEST-CLEAR exec={} seen={} noTarget={} radar220={} colorBase168={} depthBase0={} | flags={:X} rt={}x{} base={} ds={}x{} rects={} z={:.3f} col=({:.2f},{:.2f},{:.2f},{:.2f})",
                       e, seen, s_noTarget.load(), s_radar.load(), s_base168.load(), s_depth0.load(), cc.flags,
                       rt ? rt->w : 0, rt ? rt->h : 0, cc.colorInfo & 0x7FFu, ds ? ds->w : 0, ds ? ds->h : 0, cc.nRects,
                       cc.z, cc.color[0], cc.color[1], cc.color[2], cc.color[3]);
}

// TEMP-PROBE: queue a full-texture readback recorded in the open list; FlushTexProbes writes BMPs after the fence.
void D3D12Backend::QueueTexProbe(ID3D12Resource* res, D3D12_RESOURCE_STATES cur, D3D12_RESOURCE_STATES back, const char* name) {
    if (!res || !m_device || m_texProbes.size() >= 48) return;
    TexProbe tp = {};
    const D3D12_RESOURCE_DESC dd = res->GetDesc();
    UINT64 total = 0;
    m_device->GetCopyableFootprints(&dd, 0, 1, 0, &tp.fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tp.rb)))) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (cur != D3D12_RESOURCE_STATE_COPY_SOURCE) { b.Transition.StateBefore = cur; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &b); }
    D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
    dl.pResource = tp.rb.Get(); dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dl.PlacedFootprint = tp.fp;
    sl.pResource = res; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    m_commandList->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    if (back != D3D12_RESOURCE_STATE_COPY_SOURCE) { b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = back; (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &b); }
    tp.w = static_cast<uint32_t>(dd.Width); tp.h = dd.Height; tp.fmt = dd.Format;
    snprintf(tp.name, sizeof tp.name, "%s", name);
    m_texProbes.push_back(std::move(tp));
}

void D3D12Backend::FlushTexProbes() {
    if (m_texProbes.empty() || !m_fence) return;
    const uint64_t target = s_lastSignalledFenceValue;
    if (m_fence->GetCompletedValue() < target) {
        m_fence->SetEventOnCompletion(target, m_fenceEvent);
        if (WaitForSingleObject(m_fenceEvent, 2000) != WAIT_OBJECT_0) { m_texProbes.clear(); return; }
    }
    auto h2f = [](uint16_t h) { const uint32_t e = (h >> 10) & 31, m = h & 1023; float f = e == 0 ? m / 1024.0f / 16384.0f : (e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * std::ldexp(1.0f, int(e) - 15)); return (h & 0x8000) ? 0.0f : f; };
    for (auto& tp : m_texProbes) {
        void* mapped = nullptr;
        D3D12_RANGE rg = { 0, static_cast<SIZE_T>(tp.fp.Footprint.RowPitch) * tp.h };
        if (FAILED(tp.rb->Map(0, &rg, &mapped)) || !mapped) continue;
        const uint8_t* base = static_cast<const uint8_t*>(mapped) + tp.fp.Offset;
        const bool f16 = tp.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const bool f32 = tp.fmt == DXGI_FORMAT_R32_FLOAT || tp.fmt == DXGI_FORMAT_R32_TYPELESS;
        const uint32_t bpp = f16 ? 8u : 4u;
        double stSum[3] = {0,0,0}; float stMin[3] = {1e30f,1e30f,1e30f}, stMax[3] = {-1e30f,-1e30f,-1e30f};
        std::vector<uint8_t> out(static_cast<size_t>(tp.w) * tp.h * 3);
        uint64_t nz = 0; uint64_t rowNz[8] = {};
        for (uint32_t y = 0; y < tp.h; ++y) {
            const uint8_t* row = base + static_cast<size_t>(y) * tp.fp.Footprint.RowPitch;
            uint8_t* o = &out[static_cast<size_t>(tp.h - 1 - y) * tp.w * 3];
            for (uint32_t x = 0; x < tp.w; ++x) {
                const uint8_t* p = row + x * bpp;
                bool any = false;
                for (int c = 0; c < 3; ++c) {
                    float raw = f16 ? h2f(*reinterpret_cast<const uint16_t*>(p + c * 2)) : (f32 ? (c == 0 ? *reinterpret_cast<const float*>(p) : 0.0f) : p[c] / 255.0f);
                    stSum[c] += raw; stMin[c] = std::min(stMin[c], raw); stMax[c] = std::max(stMax[c], raw);
                    float v = f16 ? raw * 4.0f : (f32 ? std::min(1.0f, std::max(0.0f, raw)) : raw);
                    any |= v != 0.0f;
                    o[x * 3 + (2 - c)] = static_cast<uint8_t>(255.0f * std::pow(std::min(1.0f, v), f16 ? 1.0f / 2.2f : 1.0f));
                }
                if (any) { ++nz; if (tp.h >= 8) ++rowNz[y * 8 / tp.h]; }
            }
        }
        char fn[160]; snprintf(fn, sizeof fn, "%s%s.bmp", strncmp(tp.name, "stage", 5) == 0 ? "stages/" : "probe_", tp.name);
        if (FILE* f = fopen(fn, "wb")) {
            uint8_t hd[54] = {'B','M'}; const uint32_t sz = 54 + uint32_t(out.size()), off = 54, dib = 40, w = tp.w, hh = tp.h; const uint16_t pl = 1, bc = 24;
            memcpy(hd+2,&sz,4); memcpy(hd+10,&off,4); memcpy(hd+14,&dib,4); memcpy(hd+18,&w,4); memcpy(hd+22,&hh,4); memcpy(hd+26,&pl,2); memcpy(hd+28,&bc,2);
            fwrite(hd,1,54,f); fwrite(out.data(),1,out.size(),f); fclose(f);
        }
        if (strncmp(tp.name, "stage", 5) == 0) { const double np = double(tp.w) * tp.h; MCLA_LOG_WARN("TEMP-STAGE {} {}x{} dxgi={} nonBlackPct={:.2f} min=({:.4g},{:.4g},{:.4g}) max=({:.4g},{:.4g},{:.4g}) mean=({:.4g},{:.4g},{:.4g})", tp.name, tp.w, tp.h, static_cast<uint32_t>(tp.fmt), 100.0 * nz / np, stMin[0], stMin[1], stMin[2], stMax[0], stMax[1], stMax[2], stSum[0]/np, stSum[1]/np, stSum[2]/np); }
        MCLA_LOG_WARN("TEMP-PROBE {} {}x{} fmt={} nonZeroPx={} bands8={},{},{},{},{},{},{},{}", tp.name, tp.w, tp.h, static_cast<uint32_t>(tp.fmt), nz,
                      rowNz[0], rowNz[1], rowNz[2], rowNz[3], rowNz[4], rowNz[5], rowNz[6], rowNz[7]);
        D3D12_RANGE none = { 0, 0 };
        tp.rb->Unmap(0, &none);
    }
    m_texProbes.clear();
}

void D3D12Backend::ReportResolveReadback() {
    FlushTexProbes();
    if (!m_rsRbPending || !m_rsRb || !m_fence) return;
    m_rsRbPending = false;
    const uint64_t target = s_lastSignalledFenceValue;
    if (m_fence->GetCompletedValue() < target) {
        m_fence->SetEventOnCompletion(target, m_fenceEvent);
        if (WaitForSingleObject(m_fenceEvent, 2000) != WAIT_OBJECT_0) return;
    }
    void* mapped = nullptr;
    D3D12_RANGE rg = { 0, static_cast<SIZE_T>(m_rsRbSize) };
    if (FAILED(m_rsRb->Map(0, &rg, &mapped)) || !mapped) return;
    const uint8_t* base = static_cast<const uint8_t*>(mapped) + m_rsRbFp.Offset;
    const uint32_t bpp = (m_rsRbFmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u : 4u;
    uint64_t nz = 0, total = 0;
    for (uint32_t y = 0; y < m_rsRbH; y += 2) {
        const uint8_t* row = base + static_cast<size_t>(y) * m_rsRbFp.Footprint.RowPitch;
        for (uint32_t x = 0; x < m_rsRbW; x += 2) {
            const uint8_t* p = row + static_cast<size_t>(x) * bpp;
            const bool any = bpp == 8 ? ((p[0] | p[1] | p[2] | p[3] | p[4] | p[5]) != 0) : ((p[0] | p[1] | p[2]) != 0);
            nz += any ? 1u : 0u; ++total;
        }
    }
    const uint8_t* c = base + static_cast<size_t>(m_rsRbH / 2) * m_rsRbFp.Footprint.RowPitch + static_cast<size_t>(m_rsRbW / 2) * bpp;
    MCLA_LOG_WARN("TEMP-RESOLVE-RB #{} dest={:08X} {}x{} fmt={} nonZeroPx={}/{} centre={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
                  m_rsRbIdx, m_rsRbBase, m_rsRbW, m_rsRbH, static_cast<uint32_t>(m_rsRbFmt), nz, total,
                  c[0], c[1], c[2], c[3], bpp == 8 ? c[4] : 0, bpp == 8 ? c[5] : 0, bpp == 8 ? c[6] : 0, bpp == 8 ? c[7] : 0);
    D3D12_RANGE none = { 0, 0 };
    m_rsRb->Unmap(0, &none);
}

void D3D12Backend::ReportDrawReadback() {
    if (!m_readbackBuffer || !m_device) return;
    ++m_drawReadbackCompletions;

    // PresentOwnedFrame signalled m_fence after executing the list this copy is
    // in, so waiting on that value is what makes the Map below safe. This is a
    // CPU/GPU sync point and the reason the readback is bounded.
    //
    // F-728: THIS WAIT USED TO INDEX THE WRONG SLOT, AND IT IS NOW FIXED. It
    // read
    //   const uint64_t target = m_fenceValues[m_frameIndex];
    // but PresentOwnedFrame writes that slot (below, at the Signal) and then
    // IMMEDIATELY advances the index, and this function is called after
    // PresentOwnedFrame has returned. So the slot named belonged to a frame up
    // to two presents old, its value was <= the value just signalled, and
    // GetCompletedValue() had already passed it -- the wait was skipped and the
    // Map could run before the copy. kBufferCount is 3 (d3d12_backend.h:34).
    //
    // It was never merely theoretical. The F-728 harness measured the shipped
    // arithmetic on a real device: target=0 completed=1 waited=0, and the probe
    // returned the readback buffer's previous contents while the census, run
    // milliseconds later over the same Map, returned the copy's output. That is
    // the shape of build/w705.log row #1, whose probe read (0,0,0,0) beside a
    // census reporting nonBlack=921600/921600.
    //
    // The target is now the value captured at the Signal itself
    // (s_lastSignalledFenceValue). The two values are appended to the row so
    // that whether the wait was needed, and whether it was taken, is a
    // measurement on the next soak rather than a reading of this comment.
    const uint64_t fenceTarget = s_lastSignalledFenceValue;
    const uint64_t fenceCompletedAtEntry =
        m_fence ? m_fence->GetCompletedValue() : 0;
    bool fenceWaited = false;
    bool fenceWaitTimedOut = false;
    if (m_fence && fenceCompletedAtEntry < fenceTarget) {
        m_fence->SetEventOnCompletion(fenceTarget, m_fenceEvent);
        // F-XXX (2026-10-27): CHECK THE WAIT. The timeout return was ignored,
        // so a timed-out wait still fell through to Map -- the readback buffer
        // could be read before the copy completed, and a zero sample could be
        // an uncompleted copy rather than a rendered black pixel. A timed-out
        // wait is recorded on the row and the Map is skipped: an unmeasured
        // sample is honest, a fabricated zero is not.
        fenceWaited = true;
        if (WaitForSingleObject(m_fenceEvent, 2000) != WAIT_OBJECT_0 ||
            m_fence->GetCompletedValue() < fenceTarget) {
            fenceWaitTimedOut = true;
        }
    }
    const uint64_t fenceCompleted = m_fence ? m_fence->GetCompletedValue() : 0;
    if (fenceWaitTimedOut) {
        ++m_drawReadbackFenceTimeouts;
        const uint64_t n = m_drawReadbackFenceTimeouts;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_WARN("D3D12-DRAW-PIXEL #{} fence-wait-timeout target={} "
                          "completed={} -- the copy did not complete in 2 s; the "
                          "sample is NOT taken rather than mapped uncompleted",
                          n, fenceTarget, fenceCompleted);
        }
        return;
    }

    void* mapped = nullptr;
    D3D12_RANGE range = { 0, 0 };
    HRESULT hrMap = m_readbackBuffer->Map(0, &range, &mapped);
    if (FAILED(hrMap) || !mapped) {
        ++m_drawReadbackMapFailures;
        const uint64_t n = m_drawReadbackMapFailures;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_WARN("D3D12-DRAW-PIXEL #{} Map hr=0x{:08X} ppData={} -- the "
                          "copy executed but no value could be read, so no pixel "
                          "is claimed",
                          n, static_cast<uint32_t>(hrMap), mapped);
        }
        return;
    }

    const uint8_t* base = static_cast<const uint8_t*>(mapped) + m_readbackFootprint.Offset;
    const uint32_t pitch = static_cast<uint32_t>(m_readbackRowPitch);

    // Probe 0 is the viewport centre. It is inside a full-screen guest draw and
    // outside a small one, which is why the value is reported with its coordinates
    // and never as "the colour of the frame".
    const uint32_t px = m_width / 2u;
    const uint32_t py = m_height / 2u;
    const size_t probeOffset =
        static_cast<size_t>(py) * pitch + static_cast<size_t>(px) * 4u;
    const uint8_t* p = base + probeOffset;
    const uint8_t preR = p[0], preG = p[1], preB = p[2], preA = p[3];

    // ---------------------------------------------------------------------
    // F-728: THE ROW USED TO CONTRADICT ITSELF, AND IT WAS NOT THE VALUES.
    // build/w705.log line 7972 reads
    //   rgba=(0,0,0,0) at (640,360) ... nonBlack=921600/921600 (100.0000%) maxChannel=13
    // A surface cannot be wholly non-black and hold a fully black pixel at the
    // probe point, so one of the two numbers was not describing the surface the
    // other described. It was not a plumbing split: every one of the five
    // fields is computed from the single `base` above, and there is no call
    // between the probe and the loop. It was TIME. The four probe bytes were
    // four loads at the line this comment replaces, and the census -- ~3.5 MB
    // and 921,600 iterations at 1280x720 -- started after them, with nothing
    // ordering the GPU copy in between because the fence wait above named the
    // wrong slot and was skipped. So the probe read the buffer's previous
    // contents and the census read the copy's output.
    //
    // MEASURED, not inferred, by the F-728 harness: with the shipped
    // wrong-slot arithmetic it prints target=0 completed=1 waited=0 and the
    // probe reads the PREVIOUS submission's bytes on 12 of 12 runs while the
    // census is correct; with the captured Signal value it prints waited=1 and
    // both agree on 12 of 12. The census is exposed to the same race -- one of
    // those 12 runs read a half-copied surface (nonBlack=3634971/3686400) --
    // which is why fixing the wait and not just the probe was necessary.
    //
    // The probe is therefore no longer an independent read. It is tapped from
    // the SAME q the census is already holding, inside the loop, so rgba and
    // the four census fields are the same memory access and cannot disagree
    // about the surface by construction. The old up-front read is KEPT and
    // printed as probePreRgba, because the difference between the two is the
    // measurement: if a future edit breaks the wait again, that difference is
    // visible in the row instead of being a contradiction. Nothing is clamped
    // and no field is removed (charter F-609).
    // ---------------------------------------------------------------------

    // ---------------------------------------------------------------------
    // F-726: THIS CENSUS PRINTED AN ARITHMETICALLY IMPOSSIBLE ROW, AND THE
    // MEASUREMENT OF HOW IS BELOW. build/w700.log line 7830 reads
    //   meanChannel=26.0000 ... maxChannel=13
    // A mean over a surface cannot exceed that surface's maximum. The cause was
    // the DENOMINATOR, and it is visible two lines apart: `sum` accumulated
    // THREE bytes per pixel (the k = 0,1,2 loop below) while `total` was
    // incremented ONCE per pixel, so the printed mean was exactly 3x the mean
    // channel and could - did - exceed maxChannel. Confirmed at instruction
    // level, not inferred: in build/CMakeFiles/mcla.dir/src/d3d12_backend.cpp.obj
    // the loop body is three `addq` into the sum register and a single `incq`
    // into the total register, and the division is `divsd` of (double)sum by
    // (double)total. 26.0000 / 3 = 8.6667, a legal mean under a maximum of 13.
    //
    // The denominator is now CHANNEL SAMPLES (total * 3), so mean <= maxChannel
    // holds BY CONSTRUCTION: every one of the channelSamples bytes in `sum` is
    // <= maxChannel, because maxChannel was taken from the same bytes.
    // ---------------------------------------------------------------------
    //
    // BOUNDED (F-726). This loop runs on the render thread, and at 1280x720 it
    // was 921,600 iterations touching 3.5 MB of mapped memory. Below
    // kDrawCensusMaxPixels every pixel is still examined and the two fields mean
    // exactly what they meant before; above it, ROWS ARE STRIDED and the stride
    // and the examined row/pixel counts are appended to the row, so a sampled
    // census can never be read as an exhaustive one. The cap is a compile-time
    // constant, like kDrawReadbackMaxReadbacks, for the same reason: a runtime
    // knob is one more thing that can differ between two compared logs.
    static constexpr uint64_t kDrawCensusMaxPixels = 1ull << 20;  // 1,048,576
    // The count of censuses whose mean exceeded their maximum. A function-local
    // static rather than a member, so this file needs no header change and
    // cannot collide with another owner's edit to d3d12_backend.h.
    static uint64_t s_drawCensusInconsistent = 0;

    // ---------------------------------------------------------------------
    // F-729: THE CENSUS KNEW HOW MUCH OF THE SURFACE WAS BLACK AND NOTHING
    // ELSE. The four fields above are aggregates over 921,600 pixels, and
    // aggregates cannot answer "where" or "what colour": a solid triangle of
    // one colour and scattered noise of the same area and the same per-pixel
    // byte sums print IDENTICAL nonBlack, maxChannel, meanChannel and sum. That
    // is not a defect in those fields -- they are correct and they stay -- it
    // is that they are the wrong SHAPE of measurement for the question the boot
    // now asks, which is whether the guest's first rasterised fragment is a
    // shape at all.
    //
    // So four more measurements are appended, and each is derived from the same
    // `base` and the same censusStride the fields above already use, so a
    // strided census is described as strided here too and cannot be read as an
    // exhaustive one:
    //
    //   1. THE MODAL COLOUR and the bounding box of every pixel that DIFFERS
    //      from it, plus the count of those pixels. The comparison is against
    //      the surface's own most common RGBA value, not against black: at
    //      1280x720 the placeholder clear rgba=(5,8,13,255) is itself non-black
    //      and covers most of an unrasterised surface, so a "non-black bounding
    //      box" would be the whole surface by construction and would answer
    //      nothing. A box of the pixels that differ from the modal value is
    //      the first measurement that can distinguish "one solid region of the
    //      right size" from "the right number of pixels scattered".
    //   2. HOW MANY DISTINCT RGBA VALUES the surface holds and a bounded top-N
    //      with counts. This is the field that separates "a triangle of one
    //      colour" from "noise": noise is many colours, a triangle is one.
    //   3. ROW AND COLUMN COVERAGE of the differing pixels, so a solid region
    //      and a blob of equal area are told apart by their edge counts.
    //   4. THE COUNT OF PIXELS THAT ARE EXACTLY rgba=(0,0,0,255), beside the
    //      count the nonBlack percentage IMPLIES (total - nonBlack). nonBlack
    //      counts a pixel when any of r,g,b is nonzero, so total-nonBlack is
    //      "pixels with r==0 and g==0 and b==0" -- which INCLUDES rgba=(0,0,0,0).
    //      Printing both, and their difference, is what makes "the black really
    //      is black" a measurement instead of an assumption.
    //
    // BOUNDS, because this runs on the render thread and the log is already
    // megabytes per soak. The distinct-value table is an open-addressed set of
    // at most kDrawCensusDistinctCap RGBA keys; past that the row prints
    // distinctCapped=1 and says the modal value and the top-N are then exact
    // only over the first kDrawCensusDistinctCap distinct values seen -- so a
    // surface too varied for this table reports that it is, rather than
    // reporting a confident wrong modal. The top-N is kDrawCensusTopN entries.
    // The row and column coverage needs one counter per row and per column;
    // above kDrawCoverageMaxDim of those the counters are NOT allocated and the
    // coverage fields print as -1 beside coverageRefused=1, because a silent 0
    // there would read as "this surface has no changed rows".
    //
    // COST: the shape pass is a SECOND walk of the same bytes over the same
    // strided rows, so the census is bounded at 2 * kDrawCensusMaxPixels pixel
    // visits rather than kDrawCensusMaxPixels. It cannot be fused into the loop
    // above because the modal value is not known until that loop has ended --
    // comparing a pixel against a colour that the same loop has not yet decided
    // on is how a bounding box gets printed for the wrong colour. Two walks of
    // ~3.5 MB, twice per soak, in exchange for the first measurement of WHERE
    // the guest drew anything.
    static constexpr uint32_t kDrawCensusDistinctCap = 4096u;
    static constexpr uint32_t kDrawCensusTopN = 8u;
    static constexpr uint64_t kDrawCoverageMaxDim = 1ull << 20;
    // Power of two, four times the cap, so the open-addressed set is at most a
    // quarter full and linear probing stays short.
    static constexpr uint32_t kDrawCensusSlots = kDrawCensusDistinctCap * 4u;

    const uint64_t surfacePixels = static_cast<uint64_t>(m_width) * m_height;
    uint32_t censusStride = 1;
    while (censusStride < m_height && surfacePixels / censusStride > kDrawCensusMaxPixels) {
        censusStride *= 2;
    }

    // Whole-surface census, so "black" is a measurement and not an impression.
    // nonBlack counts a pixel when ANY OF ITS R,G,B BYTES IS NON-ZERO, i.e. it is
    // "not black", and alpha is deliberately NOT part of the predicate: a pixel
    // with rgb=0,0,0 and alpha=255 is black and must not be counted as non-black.
    uint64_t nonBlack = 0, maxChannel = 0, sum = 0, total = 0, censusRows = 0;
    // F-728: the reported probe bytes and the two counters that say where they
    // came from. probeTaps is 1 in every reachable path; probeFromCensus is 0
    // only when the stride skipped row py and the fallback below ran, which the
    // row prints so a reader never has to guess.
    uint8_t r = 0, g = 0, b = 0, a = 0;
    uint64_t probeTaps = 0, probeFromCensus = 0;
    uint32_t lastCensusRow = 0;
    // F-729: the colour side of the census. `slots` is the open-addressed index
    // (0 = empty, otherwise key index + 1), `keys` holds each distinct RGBA once
    // in first-seen order, `counts` the pixels per key. Insertion order is kept
    // so the top-N tie-break below is over a stable list rather than over hash
    // order, which would make the printed histogram differ between two runs of
    // the same surface.
    std::vector<uint32_t> slots(kDrawCensusSlots, 0u);
    std::vector<uint32_t> keys;
    std::vector<uint64_t> counts;
    keys.reserve(kDrawCensusDistinctCap);
    counts.reserve(kDrawCensusDistinctCap);
    uint64_t distinctOverflow = 0;
    // F-729: pixels whose four bytes are EXACTLY 0,0,0,255. Counted directly,
    // never derived from nonBlack: nonBlack's predicate is (r|g|b) != 0, so
    // total - nonBlack also counts rgba=(0,0,0,0), and the difference between
    // the two numbers is exactly the finding if there is one.
    uint64_t exactBlack = 0;
    for (uint32_t y = 0; y < m_height; y += censusStride) {
        const uint8_t* row = base + static_cast<size_t>(y) * pitch;
        lastCensusRow = y;
        // F-728: one comparison per row, hoisted out of the pixel loop, so the
        // tap costs nothing per pixel except the x compare on that one row.
        const bool probeRow = (y == py);
        for (uint32_t x = 0; x < m_width; ++x) {
            const uint8_t* q = row + static_cast<size_t>(x) * 4u;
            // F-728: the SAME q the four census lines below read. This is the
            // whole fix -- rgba is now that load, not a load taken at a
            // different instant.
            if (probeRow && x == px) {
                r = q[0]; g = q[1]; b = q[2]; a = q[3];
                ++probeTaps; ++probeFromCensus;
            }
            if (q[0] | q[1] | q[2]) ++nonBlack;
            if (q[0] > maxChannel) maxChannel = q[0];
            if (q[1] > maxChannel) maxChannel = q[1];
            if (q[2] > maxChannel) maxChannel = q[2];
            sum += static_cast<uint64_t>(q[0]) + q[1] + q[2];
            ++total;
            // F-729: the histogram tap. Same q, one packed key, one probe.
            const uint32_t key = static_cast<uint32_t>(q[0]) |
                                 (static_cast<uint32_t>(q[1]) << 8) |
                                 (static_cast<uint32_t>(q[2]) << 16) |
                                 (static_cast<uint32_t>(q[3]) << 24);
            uint32_t slot = ((key * 2654435761u) >> 12) & (kDrawCensusSlots - 1u);
            uint32_t probes = 0;
            uint32_t found = 0u;
            while (probes < kDrawCensusSlots) {
                const uint32_t occupant = slots[slot];
                if (occupant == 0u) break;
                if (keys[occupant - 1u] == key) { found = occupant; break; }
                slot = (slot + 1u) & (kDrawCensusSlots - 1u);
                ++probes;
            }
            if (found != 0u) {
                ++counts[found - 1u];
            } else if (slots[slot] == 0u && keys.size() < kDrawCensusDistinctCap) {
                keys.push_back(key);
                counts.push_back(1u);
                slots[slot] = static_cast<uint32_t>(keys.size());
            } else {
                // The table is at its cap. Recorded, not silently ignored: the
                // row prints distinctCapped=1 so no reader can take the modal
                // value and the top-N as exact over a surface that outran them.
                ++distinctOverflow;
            }
            if (q[0] == 0u && q[1] == 0u && q[2] == 0u && q[3] == 255u) ++exactBlack;
        }
        ++censusRows;
    }
    // A strided census can skip row py. Then there is no q to tap, so the probe
    // is read from that row immediately after the loop and the row says so via
    // probeFromCensus=0. It is NOT skipped silently and it is NOT invented.
    if (probeTaps == 0) {
        const uint8_t* row = base + static_cast<size_t>(py) * pitch;
        const uint8_t* q = row + static_cast<size_t>(px) * 4u;
        r = q[0]; g = q[1]; b = q[2]; a = q[3];
        ++probeTaps;
    }
    // F-729: THE MODAL VALUE, decided only now that the histogram is complete.
    // Strictly greater-than on the count, scanning in first-seen order, so ties
    // go to the value that appeared first -- deterministic, and stated here
    // because a modal value that moved between two runs of the same surface
    // would make every field below it move too.
    uint32_t modalIdx = 0;
    for (size_t i = 1; i < keys.size(); ++i) {
        if (counts[i] > counts[modalIdx]) modalIdx = static_cast<uint32_t>(i);
    }
    const uint32_t modalKey = keys.empty() ? 0u : keys[modalIdx];
    const uint64_t modalCount = keys.empty() ? 0u : counts[modalIdx];
    const uint8_t modalR = static_cast<uint8_t>(modalKey & 0xFFu);
    const uint8_t modalG = static_cast<uint8_t>((modalKey >> 8) & 0xFFu);
    const uint8_t modalB = static_cast<uint8_t>((modalKey >> 16) & 0xFFu);
    const uint8_t modalA = static_cast<uint8_t>((modalKey >> 24) & 0xFFu);

    // F-729: the bounded top-N, as one pre-rendered string so the format call
    // below gains exactly one argument instead of forty. Selected, not sorted:
    // kDrawCensusTopN passes of "take the largest not yet taken", which is
    // O(kDrawCensusTopN * distinct) and needs no <algorithm> include. Ties go
    // to the lower packed key, so the printed order is a function of the
    // histogram and not of the hash.
    std::string topRgba;
    {
        char ent[80];
        std::vector<uint8_t> taken(keys.size(), 0u);
        for (uint32_t n = 0; n < kDrawCensusTopN; ++n) {
            size_t best = keys.size();
            for (size_t i = 0; i < keys.size(); ++i) {
                if (taken[i] != 0u) continue;
                if (best == keys.size() || counts[i] > counts[best] ||
                    (counts[i] == counts[best] && keys[i] < keys[best])) {
                    best = i;
                }
            }
            if (best == keys.size()) break;
            taken[best] = 1u;
            std::snprintf(ent, sizeof(ent), "%s(%u,%u,%u,%u)x%llu",
                          topRgba.empty() ? "" : " ",
                          static_cast<unsigned>(keys[best] & 0xFFu),
                          static_cast<unsigned>((keys[best] >> 8) & 0xFFu),
                          static_cast<unsigned>((keys[best] >> 16) & 0xFFu),
                          static_cast<unsigned>((keys[best] >> 24) & 0xFFu),
                          static_cast<unsigned long long>(counts[best]));
            topRgba.append(ent);
        }
        if (topRgba.empty()) topRgba = "none";
        if (keys.size() > kDrawCensusTopN) {
            std::snprintf(ent, sizeof(ent), " (+%llu not shown)",
                          static_cast<unsigned long long>(keys.size() -
                                                           kDrawCensusTopN));
            topRgba.append(ent);
        }
    }

    // F-729: THE SHAPE PASS. A second walk of the same bytes over the same
    // strided rows, because the modal value did not exist until the walk above
    // ended. Everything here is counted against the SAME pixel set the four
    // original fields used, so the box and the histogram describe one set of
    // pixels and cannot be compared across two different samples.
    const uint32_t kNoCoord = static_cast<uint32_t>(-1);
    uint64_t changedPx = 0;
    uint32_t chgMinX = kNoCoord, chgMaxX = 0, chgMinY = kNoCoord, chgMaxY = 0;
    int64_t chgRows = 0, chgCols = 0;
    int64_t chgFirstRow = -1, chgLastRow = -1, chgFirstCol = -1, chgLastCol = -1;
    int64_t chgFirstRowCount = -1, chgLastRowCount = -1;
    int64_t chgFirstColCount = -1, chgLastColCount = -1;
    const bool coverageRefused =
        (static_cast<uint64_t>(m_width) + m_height) > kDrawCoverageMaxDim;
    std::vector<uint32_t> rowHits;
    std::vector<uint32_t> colHits;
    if (!coverageRefused) {
        rowHits.assign(m_height, 0u);
        colHits.assign(m_width, 0u);
        for (uint32_t y = 0; y < m_height; y += censusStride) {
            const uint8_t* row = base + static_cast<size_t>(y) * pitch;
            for (uint32_t x = 0; x < m_width; ++x) {
                const uint8_t* q = row + static_cast<size_t>(x) * 4u;
                const uint32_t key = static_cast<uint32_t>(q[0]) |
                                     (static_cast<uint32_t>(q[1]) << 8) |
                                     (static_cast<uint32_t>(q[2]) << 16) |
                                     (static_cast<uint32_t>(q[3]) << 24);
                if (key == modalKey) continue;
                ++changedPx;
                if (x < chgMinX) chgMinX = x;
                if (x > chgMaxX) chgMaxX = x;
                if (y < chgMinY) chgMinY = y;
                if (y > chgMaxY) chgMaxY = y;
                ++rowHits[y];
                ++colHits[x];
            }
        }
        for (uint32_t y = 0; y < m_height; ++y) {
            if (rowHits[y] == 0u) continue;
            // The "first" test is on chgFirstRow, NOT on chgRows. chgRows is a
            // COUNT and starts at zero, so testing it would make a rectangle
            // spanning rows 0..359 print 359 instead of 360: the sentinel
            // belongs to the index, not to the tally. MEASURED -- the harness
            // in %TEMP%\opencode\f729 reported 359 rows for a 640x360
            // rectangle with this bug, and 640x360 is 360 rows.
            if (chgFirstRow < 0) { chgFirstRow = static_cast<int64_t>(y);
                                   chgFirstRowCount = rowHits[y]; }
            chgLastRow = static_cast<int64_t>(y);
            chgLastRowCount = rowHits[y];
            ++chgRows;
        }
        for (uint32_t x = 0; x < m_width; ++x) {
            if (colHits[x] == 0u) continue;
            if (chgFirstCol < 0) { chgFirstCol = static_cast<int64_t>(x);
                                   chgFirstColCount = colHits[x]; }
            chgLastCol = static_cast<int64_t>(x);
            chgLastColCount = colHits[x];
            ++chgCols;
        }
    }
    // changedPx is 0 iff every examined pixel WAS the modal value, i.e. the
    // examined set is uniform -- so in that case there is no box to report and
    // the row says absent. Printing 0,0,0,0 would be inventing a corner for a
    // region that does not exist (charter F-609), and printing the surface's
    // own 0,0,1280,720 box would be answering a different question.
    char chgBox[96];
    if (changedPx == 0u) {
        std::snprintf(chgBox, sizeof(chgBox), "absent");
    } else {
        std::snprintf(chgBox, sizeof(chgBox), "x%u..%u y%u..%u",
                      static_cast<unsigned>(chgMinX), static_cast<unsigned>(chgMaxX),
                      static_cast<unsigned>(chgMinY), static_cast<unsigned>(chgMaxY));
    }

    // F-729: what the nonBlack percentage IMPLIES. total - nonBlack is the count
    // of pixels with r==0 AND g==0 AND b==0, alpha unconstrained, so it is an
    // upper bound on exactBlack rather than the same number -- unless every
    // black pixel also carries alpha 255, in which case the two are equal and
    // that equality is the measurement that "the black really is black" rests
    // on. Printed as both numbers so a reader never has to do the subtraction.
    const uint64_t blackImplied = total - (nonBlack < total ? nonBlack : total);

    // The probe's own byte offset and the range the census actually walked, so
    // the two are comparable by eye instead of by trust.
    const size_t censusFirstByte = 0;
    const size_t censusLastByte = static_cast<size_t>(lastCensusRow) * pitch +
                                  static_cast<size_t>(m_width) * 4u - 1u;
    const bool probeInsideCensusRange =
        probeOffset >= censusFirstByte && (probeOffset + 3u) <= censusLastByte;
    const uint64_t channelSamples = total * 3u;
    const double meanChannel = channelSamples
                                   ? static_cast<double>(sum) / static_cast<double>(channelSamples)
                                   : 0.0;

    // ---------------------------------------------------------------------
    // F-726: THE GUARD. The invariant above is structural, but "a reader has to
    // notice" is not a guard. One comparison, and if any future edit breaks the
    // denominator again this row prints NaN -- which fmt renders as `nan` -- and
    // counts the event, instead of printing a mean above the surface maximum
    // again. It does NOT clamp: clamping a summary into looking plausible is
    // fabricating a pixel value (charter F-609), so the honest answer, "this
    // number does not exist", is the one that gets printed.
    // ---------------------------------------------------------------------
    double printedMean = meanChannel;
    if (meanChannel > static_cast<double>(maxChannel)) {
        const uint64_t bad = ++s_drawCensusInconsistent;
        printedMean = std::numeric_limits<double>::quiet_NaN();
        MCLA_LOG_WARN("D3D12-DRAW-CENSUS-INCONSISTENT #{} sum={} channelSamples={} "
                      "maxChannel={} nonBlack={}/{} -- meanChannel is printed as nan "
                      "in the D3D12-DRAW-PIXEL row because it exceeded maxChannel, "
                      "which no mean over the same bytes can do. The denominators "
                      "and the raw sums are here so it can be recomputed",
                      bad, sum, channelSamples, maxChannel, nonBlack, total);
    }

    m_lastPixelX = px;
    m_lastPixelY = py;
    m_lastPixelR = r;
    m_lastPixelG = g;
    m_lastPixelB = b;
    m_lastPixelA = a;
    m_lastPixelNonBlack = nonBlack;
    m_lastPixelMaxChannel = maxChannel;
    m_lastPixelMeanChannel = printedMean;  // F-726: the guarded value, never a bare quotient
    m_lastPixelTotal = total;
    m_lastPixelRowPitch = pitch;
    m_lastPixelValid = true;

    const uint64_t n = m_drawReadbackCompletions;
    // F-729 APPENDED FIELDS, every field above the F-729 line in the row below
    // is unchanged and in its old position, including the four aggregates.
    // F-729's own comment lives here, not inside the argument list, so that the
    // format string and its arguments stay one contiguous, auditable block.
    MCLA_LOG_INFO(
        "D3D12-DRAW-PIXEL #{} rgba=({},{},{},{}) at ({},{}) of {}x{} rowPitch={} | "
        "nonBlack={}/{} ({:.4f}%) maxChannel={} meanChannel={:.4f} -- READ THESE "
        "VALUES, they are the first pixel values this backend has ever reported. "
        "Path: the draw targeted {}, CopyTextureRegion "
        "copied that surface into a D3D12_HEAP_TYPE_READBACK heap buffer "
        "inside the same command list, PresentOwnedFrame executed and signalled "
        "the fence, and Map on that heap -- which is mappable by definition -- "
        "returned these bytes. rgba is one probe at the viewport centre, not a "
        "summary of the frame; nonBlack/maxChannel/meanChannel are the whole "
        "surface. alpha=255 with rgb=0,0,0 is black, which is a real answer and "
        "is reported as one. F-726 APPENDED FIELDS, every field above is "
        "unchanged and in its old position: sum={} channelSamples={} censusRows={} "
        "censusStride={} -- F-726 also changed the MEAN's denominator from "
        "pixels to channelSamples (it was 3x too large, which is how w700.log "
        "line 7830 came to print a mean above its own maximum) and bounds the "
        "census, so a census with censusStride>1 covers the rows listed, not "
        "necessarily every row. F-727 APPENDED FIELDS: copyAfterDraw={} "
        "drawSeq={} -- copyAfterDraw=1 means this copy was recorded AFTER a draw "
        "call was issued on this command list, so the sample describes the draw's "
        "output. copyAfterDraw=0 is the F-727 defect: the copy was recorded before "
        "any draw call, so the sample describes the surface as it was BEFORE the "
        "draw -- which for a first frame is this backend's own placeholder clear, "
        "kPlaceholderBackBufferClear = {{0.02f,0.03f,0.05f,1.0f}}, and that "
        "quantises to rgba=(5,8,13,255). build/w701.log printed exactly that and "
        "was NOT a guest pixel. drawSeq is the F-653 draw ordinal this sample is "
        "attributed to. F-728 APPENDED FIELDS, every field above is unchanged and "
        "in its old position: probeTaps={} probeFromCensus={} "
        "probePreRgba=({},{},{},{}) probeByteOffset={} censusByteRange={}..{} "
        "probeInsideCensusRange={} fenceTarget={} fenceCompleted={} "
        "fenceWaited={} -- rgba is now the loop's OWN read of that pixel, taken "
        "from the same q as the four census fields above, so it cannot disagree "
        "with them; probeTaps=1 is that tap happening. probePreRgba is the same "
        "four bytes read immediately after Map, which is where this row used to "
        "take them from, and it is printed so that if the wait above is ever "
        "broken again the difference shows up here instead of as a row that "
        "contradicts itself. probeFromCensus=0 means the stride skipped the "
        "probe's row and the probe was read from it immediately after the loop. "
        "probeByteOffset and censusByteRange are byte offsets into the same "
        "mapped buffer, so the two can be compared directly. fenceTarget is the "
        "value PresentOwnedFrame signalled for THIS list and fenceWaited says "
        "whether the Map was ordered after the copy: fenceWaited=0 means every "
        "field in this row describes whatever the buffer held at Map time. "
        "distinctRgba={} distinctCapped={} topRgba={} -- the number of DIFFERENT "
        "rgba values the examined pixels hold and the eight most common of them "
        "with their pixel counts. This is the field that separates one region of "
        "one colour from scattered pixels: a single-colour region drives "
        "distinctRgba to 2 (the region and the background it cut into) and puts "
        "essentially every pixel count on one entry of topRgba, while scattered "
        "pixels of many colours leave distinctRgba high and spread the counts. "
        "distinctCapped=1 means the surface held more distinct values than the "
        "census table holds and that modalRgba and topRgba below are then exact "
        "only over the first values the table took, which the row does not hide: "
        "distinctCapped=0 is the only reading in which they are exact over "
        "everything examined. "
        "modalRgba=({},{},{},{}) modalCount={} -- the most common single rgba "
        "value of the examined pixels, and how many of them carry it. "
        "changedPx={} changedBox={} -- changedPx is the count of examined pixels "
        "whose rgba is NOT modalRgba, and changedBox is the smallest rectangle "
        "containing all of them. The comparison is against the surface's OWN "
        "most common colour rather than against black, because the placeholder "
        "clear is rgba=(5,8,13,255): it is not black, it covers most of an "
        "unrasterised surface, so a box of the non-black pixels is the whole "
        "surface by construction and would say nothing about where anything was "
        "drawn. changedBox=absent with changedPx=0 is not a zero-size box: it "
        "means every examined pixel WAS the modal value, so the examined set is "
        "uniform, and no corner is printed because no region exists to bound. "
        "changedRows={} changedCols={} changedFirstRow={} changedFirstRowCount={} "
        "changedLastRow={} changedLastRowCount={} changedFirstCol={} "
        "changedFirstColCount={} changedLastCol={} changedLastColCount={} "
        "coverageRefused={} -- how many distinct rows and columns of the surface "
        "hold at least one changed pixel, and how many changed pixels sit on the "
        "first and last of them. changedRows and changedCols are COUNTS and are "
        "0 when no row or column holds a changed pixel; changedFirstRow, "
        "changedLastRow, changedFirstCol and changedLastCol are INDICES and print "
        "-1 in that same case, so -1 there means there is no such row, not that "
        "row -1 was measured. A solid rectangle has a small count on its "
        "first and last row and column (its edge lengths) and a large count in "
        "the middle; pixels scattered over the same area have few changed pixels "
        "per row and column and hit rows and columns the rectangle misses. A "
        "coverageRefused=1 with every coverage field at -1 means the surface has "
        "more rows plus columns than the census will allocate counters for and "
        "those measurements were NOT taken; -1 is a refusal marker there, not a "
        "count. "
        "exactBlack={} blackImplied={} -- exactBlack counts examined pixels whose "
        "four bytes are exactly 0,0,0,255, and blackImplied is total - nonBlack, "
        "which is what the nonBlack percentage above implies. nonBlack tests "
        "(r|g|b)!=0, so blackImplied also counts rgba=(0,0,0,0) and is an upper "
        "bound on exactBlack; the two are equal if and only if every black pixel "
        "in the examined set also carries alpha 255. Every F-729 field describes "
        "the same examined rows and pixels as the fields above it, so censusStride "
        "and censusRows bound all of them the same way",
        n, r, g, b, a, px, py, m_width, m_height, pitch, nonBlack, total,
        total ? (100.0 * static_cast<double>(nonBlack) / static_cast<double>(total)) : 0.0,
        maxChannel, m_lastPixelMeanChannel,
        // F-XXX: the surface the draw targeted, named because the prose above
        // now says "the draw targeted {}" and that word has to be the TRUTH:
        // it is the guest's render target when the guest's draws land in it.
        (m_drawsIntoGuestRt && m_guestRt) ? "the guest's render target (EDRAM)"
                                          : "the swap-chain back buffer",
        sum, channelSamples, censusRows,
        static_cast<uint64_t>(censusStride),
        s_readbackCopyAfterDraw ? 1 : 0, s_readbackDrawSeq,
        probeTaps, probeFromCensus, preR, preG, preB, preA, probeOffset,
        censusFirstByte, censusLastByte, probeInsideCensusRange ? 1 : 0,
        fenceTarget, fenceCompleted, fenceWaited ? 1 : 0,
        static_cast<uint64_t>(keys.size()), distinctOverflow ? 1 : 0,
        topRgba.c_str(), static_cast<unsigned>(modalR), static_cast<unsigned>(modalG),
        static_cast<unsigned>(modalB), static_cast<unsigned>(modalA), modalCount,
        changedPx, chgBox, chgRows, chgCols, chgFirstRow, chgFirstRowCount,
        chgLastRow, chgLastRowCount, chgFirstCol, chgFirstColCount,
        chgLastCol, chgLastColCount, coverageRefused ? 1 : 0,
        exactBlack, blackImplied);

    // PIXPROBE (MCLA_PIXPROBE=1): second sample at (110,420), inside the
    // flat-grey-box quad (x70-146,y384-465) that the (640,360) centre probe
    // misses. Prints the PS hash and the staged SRV index of every shader slot
    // for the draw this copy is attributed to, so a flat pixel can be joined to
    // its draw and texture bindings. Default off: this block is skipped and the
    // D3D12-DRAW-PIXEL row above is the only row, byte-for-byte unchanged.
    if (PixProbeEnabled()) {
        // MCLA_PIXPROBE_X/Y: probe a custom pixel (default 110,420 = flat box).
        // MCLA_PIXPROBE_PASS=3d|post|both (default both): track and report the
        // last 3D scene-pass draw before post-process resolves/overwrites the RT.
        uint32_t sx = 0, sy = 0;
        PixProbeXY(sx, sy);
        const PixProbePass ppMode = PixProbePassMode();
        auto formatSlots = [](const uint32_t (&arr)[32]) {
            std::string out;
            char t[16];
            for (uint32_t s = 0; s < 32; ++s) {
                std::snprintf(t, sizeof(t), "%s%u", s ? "," : "", arr[s]);
                out.append(t);
            }
            return out;
        };
        std::string sceneRgbaStr = "none";
        if (ppMode != PixProbePass::kPost && s_pixprobe3dRbSlot < kBufferCount &&
            m_pixprobe3dRbPending[s_pixprobe3dRbSlot] && m_pixprobe3dRb[s_pixprobe3dRbSlot]) {
            void* sMap = nullptr;
            D3D12_RANGE sRange = { 0, 16 };
            if (SUCCEEDED(m_pixprobe3dRb[s_pixprobe3dRbSlot]->Map(0, &sRange, &sMap)) && sMap) {
                const uint8_t* p = static_cast<const uint8_t*>(sMap);
                const DXGI_FORMAT fmt = m_pixprobe3dRbFmt[s_pixprobe3dRbSlot];
                char buf[96];
                if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                    std::snprintf(buf, sizeof(buf), "(%u,%u,%u,%u)[base=%08X]", p[0], p[1], p[2], p[3],
                                  m_pixprobe3dRbBase[s_pixprobe3dRbSlot]);
                } else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                    std::snprintf(buf, sizeof(buf), "(%u,%u,%u,%u)[base=%08X]", p[2], p[1], p[0], p[3],
                                  m_pixprobe3dRbBase[s_pixprobe3dRbSlot]);
                } else if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    uint16_t h[4] = {};
                    std::memcpy(h, p, sizeof(h));
                    std::snprintf(buf, sizeof(buf), "(%.4f,%.4f,%.4f,%.4f)[fp16,base=%08X]",
                                  PixProbeHalfToFloat(h[0]), PixProbeHalfToFloat(h[1]),
                                  PixProbeHalfToFloat(h[2]), PixProbeHalfToFloat(h[3]),
                                  m_pixprobe3dRbBase[s_pixprobe3dRbSlot]);
                } else if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
                    uint32_t v = 0;
                    std::memcpy(&v, p, sizeof(v));
                    std::snprintf(buf, sizeof(buf), "(%u,%u,%u,%u)[10b,base=%08X]",
                                  v & 0x3FFu, (v >> 10) & 0x3FFu, (v >> 20) & 0x3FFu, (v >> 30) & 0x3u,
                                  m_pixprobe3dRbBase[s_pixprobe3dRbSlot]);
                } else {
                    uint64_t raw = 0;
                    std::memcpy(&raw, p, sizeof(raw));
                    std::snprintf(buf, sizeof(buf), "fmt=%u:0x%016llX[base=%08X]",
                                  static_cast<uint32_t>(fmt), (unsigned long long)raw,
                                  m_pixprobe3dRbBase[s_pixprobe3dRbSlot]);
                }
                sceneRgbaStr = buf;
                D3D12_RANGE wRange = { 0, 0 };
                m_pixprobe3dRb[s_pixprobe3dRbSlot]->Unmap(0, &wRange);
            }
            m_pixprobe3dRbPending[s_pixprobe3dRbSlot] = false;
        }
        auto logTexJoin = [&](const char* passTag, uint64_t dSeq, uint64_t pHash, uint64_t pProg,
                              const PixprobeTex* texArr, uint32_t texCnt) {
            for (uint32_t i = 0; i < texCnt; ++i) {
                const PixprobeTex& t = texArr[i];
                MCLA_LOG_ERROR("PS-TEX-JOIN pass={} drawSeq={} psProg={:016X} psHash={:016X} slot={} base={:08X} "
                               "fmt={} {}x{} packed={} mips={} swz={:03X} gamma={} colourMatched={}",
                               passTag, dSeq, pProg, pHash, t.slot, t.base,
                               t.fmt, t.w, t.h, t.packed, t.mips, t.swz, t.gamma,
                               m_resolvedLastColour.count(t.base) ? 1 : 0);
            }
        };
        if (sx < m_width && sy < m_height) {
            const uint8_t* sp = base + static_cast<size_t>(sy) * pitch + static_cast<size_t>(sx) * 4u;
            if (ppMode == PixProbePass::k3d) {
                if (s_pixprobe3d.valid) {
                    const std::string slots3d = formatSlots(s_pixprobe3d.stagedSlots);
                    MCLA_LOG_ERROR("D3D12-PIXPROBE #{} at ({},{}) of {}x{} rgba=({},{},{},{}) sceneRgba={} "
                                  "pass=3d psProg={:016X} psHash={:016X} drawSeq={} captureSeq={} draws3d={} "
                                  "rtBase={} {}x{} primVerts={} slots=[{}]",
                                  n, sx, sy, m_width, m_height,
                                  sp[0], sp[1], sp[2], sp[3], sceneRgbaStr.c_str(),
                                  s_pixprobe3d.psProgHash, s_pixprobe3d.psHash, s_readbackDrawSeq,
                                  s_pixprobe3d.drawSeq, s_pixprobeFrame3dCount,
                                  s_pixprobe3d.rtBase, s_pixprobe3d.rtW, s_pixprobe3d.rtH,
                                  s_pixprobe3d.primVerts, slots3d.c_str());
                    logTexJoin("3d", s_pixprobe3d.drawSeq, s_pixprobe3d.psHash, s_pixprobe3d.psProgHash,
                               s_pixprobe3d.tex, s_pixprobe3d.texCount);
                } else {
                    MCLA_LOG_WARN("D3D12-PIXPROBE #{} at ({},{}) of {}x{} rgba=({},{},{},{}) pass=3d no-3d-draw-captured",
                                  n, sx, sy, m_width, m_height, sp[0], sp[1], sp[2], sp[3]);
                }
            } else {
                const std::string slots = formatSlots(s_pixprobeStagedSlots);
                MCLA_LOG_ERROR("D3D12-PIXPROBE #{} at ({},{}) of {}x{} rgba=({},{},{},{}) pass=post "
                              "psProg={:016X} psHash={:016X} drawSeq={} captureSeq={} slots=[{}]",
                              n, sx, sy, m_width, m_height,
                              sp[0], sp[1], sp[2], sp[3],
                              s_pixprobePsProgHash, s_pixprobePsHash, s_readbackDrawSeq, s_pixprobeDrawSeq,
                              slots.c_str());
                logTexJoin("post", s_readbackDrawSeq, s_pixprobePsHash, s_pixprobePsProgHash,
                           s_pixprobeTex, s_pixprobeTexCount);
                if (ppMode == PixProbePass::kBoth && s_pixprobe3d.valid) {
                    const std::string slots3d = formatSlots(s_pixprobe3d.stagedSlots);
                    MCLA_LOG_ERROR("D3D12-PIXPROBE-3D #{} at ({},{}) of {}x{} sceneRgba={} "
                                   "psProg={:016X} psHash={:016X} captureSeq={} draws3d={} "
                                   "rtBase={} {}x{} primVerts={} indexed={} hasDsv={} slots=[{}]",
                                   n, sx, sy, m_width, m_height, sceneRgbaStr.c_str(),
                                   s_pixprobe3d.psProgHash, s_pixprobe3d.psHash,
                                   s_pixprobe3d.drawSeq, s_pixprobeFrame3dCount,
                                   s_pixprobe3d.rtBase, s_pixprobe3d.rtW, s_pixprobe3d.rtH,
                                   s_pixprobe3d.primVerts, s_pixprobe3d.indexed ? 1 : 0,
                                   s_pixprobe3d.hasDsv ? 1 : 0, slots3d.c_str());
                    logTexJoin("3d", s_pixprobe3d.drawSeq, s_pixprobe3d.psHash, s_pixprobe3d.psProgHash,
                               s_pixprobe3d.tex, s_pixprobe3d.texCount);
                }
            }
            if (ppMode != PixProbePass::kPost && s_pixprobe3dRingCount > 0) {
                std::string ringStr;
                for (uint32_t i = 0; i < s_pixprobe3dRingCount; ++i) {
                    const Pixprobe3dRingEntry& re = s_pixprobe3dRing[i];
                    char rb[128];
                    std::snprintf(rb, sizeof(rb), "%s[seq=%llu prog=%016llX bank=%016llX n=%u tex=%u base0=%08X]",
                                  i ? " " : "", (unsigned long long)re.lastDrawSeq,
                                  (unsigned long long)re.psProgHash, (unsigned long long)re.psHash,
                                  re.draws, re.texCount, re.firstTexBase);
                    ringStr.append(rb);
                }
                MCLA_LOG_ERROR("D3D12-PIXPROBE-3D-RING #{} draws3d={} distinct={} {}",
                               n, s_pixprobeFrame3dCount, s_pixprobe3dRingCount, ringStr.c_str());
            }
        } else {
            MCLA_LOG_WARN("D3D12-PIXPROBE #{} sample-out-of-range at ({},{}) of {}x{} "
                          "-- ({},{}) is outside this surface, no pixel read",
                          n, sx, sy, m_width, m_height, sx, sy);
        }
    }

    // F-XXX: KEEP THE GUEST RENDER TARGET'S OWN PIXEL COUNT. This census ran
    // over the surface THIS draw wrote, and when the draws land in the guest
    // render target that surface IS the guest's render target (the sample
    // source is chosen at .cpp :4454). The resolve's row prints this number,
    // because "the copy ran" and "what was copied was not blank" are two
    // different facts and only this one is the second.
    if (m_drawsIntoGuestRt) {
        m_guestRtChangedPx = changedPx;
        m_guestRtModalRgba = modalKey;
        m_guestRtModalCount = modalCount;
    }

    // Direct D3D12 frame capture: save live frames as top-down 32-bit BMPs for ground-truth inspection.
    // Use the frame index that was just presented (before m_frameIndex advanced).
    uint32_t& s_bmpSaved = s_bmpSavedFs;
    uint32_t bmpFrameIndex = (m_frameIndex == 0) ? (kBufferCount - 1) : (m_frameIndex - 1);
    if (bmpFrameIndex >= kBufferCount) bmpFrameIndex = 0;
    ID3D12Resource* bmpTarget = m_renderTargets[bmpFrameIndex].Get();
    // Opt-in only (MCLA_BMP_DUMP=1): writing files from the present path is a debug cost.
    static const bool s_bmpDump = [] { const char* e = std::getenv("MCLA_BMP_DUMP"); return e && e[0] == '1'; }();
    // MCLA_BMP_SKIP=N: start dumping only after N presents (to catch the world pass).
    const uint64_t s_bmpSkip = BmpSkip();
    static const bool s_noBmpBoundPresent = [] { const char* e = std::getenv("MCLA_NO_BMP_READBACK_BOUND"); return e && e[0] == '1'; }();
    static const bool s_noBmpSkipBypassFixPresent = [] { const char* e = std::getenv("MCLA_NO_BMP_SKIP_BYPASS_FIX"); return e && e[0] == '1'; }();
    bool bmpDue = false;
    if (s_noBmpSkipBypassFixPresent) {
        bmpDue = (s_bmpSaved == 0 || m_presentedFrames >= s_lastBmpFrame + BmpEvery());
        const uint64_t bmpEveryMs = BmpEveryMs();
        if (bmpEveryMs != 0) {
            const auto now = std::chrono::steady_clock::now();
            bmpDue = (s_bmpSaved == 0 ||
                      std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastBmpTime).count()
                          >= static_cast<int64_t>(bmpEveryMs));
            if (bmpDue) s_lastBmpTime = now;
        }
    } else {
        bmpDue = IsBmpDumpDue(m_presentedFrames, s_noBmpBoundPresent);
    }
    if (s_bmpDump && m_presentedFrames >= s_bmpSkip && (s_noBmpBoundPresent || s_bmpSaved < BmpMax()) && bmpTarget && m_width > 0 && m_height > 0 &&
        bmpDue) {
        s_lastBmpFrame = m_presentedFrames;
        s_lastBmpTime = std::chrono::steady_clock::now();
        const uint32_t bmpSeq = s_bmpSaved++;
        // BMP-SIDECAR: emit present index + wall clock + guest draw sequence for
        // EVERY dumped frame, so a sheet can be content/time aligned across boots
        // regardless of boot-phase frame-index jitter (rule 14: measured).
        if (std::FILE* sf = std::fopen("bmp_sidecar.csv", "a")) {
            auto nowT = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::fprintf(sf, "bmp=%u,present=%llu,ms=%llu\n",
                         bmpSeq, (unsigned long long)m_presentedFrames,
                         (unsigned long long)nowT);
            std::fclose(sf);
        }
        char bmpName[64];
        std::snprintf(bmpName, sizeof(bmpName), "d3d12_frame_%04u.bmp", bmpSeq);
        if (FILE* f = std::fopen(bmpName, "wb")) {
#pragma pack(push, 1)
            struct {
                uint16_t bfType = 0x4D42;
                uint32_t bfSize = 0;
                uint16_t bfReserved1 = 0;
                uint16_t bfReserved2 = 0;
                uint32_t bfOffBits = 54;
                uint32_t biSize = 40;
                int32_t  biWidth = 0;
                int32_t  biHeight = 0;
                uint16_t biPlanes = 1;
                uint16_t biBitCount = 32;
                uint32_t biCompression = 0;
                uint32_t biSizeImage = 0;
                int32_t  biXPelsPerMeter = 0;
                int32_t  biYPelsPerMeter = 0;
                uint32_t biClrUsed = 0;
                uint32_t biClrImportant = 0;
            } hdr;
#pragma pack(pop)
            hdr.biWidth = static_cast<int32_t>(m_width);
            hdr.biHeight = -static_cast<int32_t>(m_height); // top-down
            hdr.bfSize = 54 + m_width * m_height * 4;
            std::fwrite(&hdr, 1, sizeof(hdr), f);
            std::vector<uint8_t> rowBuf(m_width * 4);
            // MD-40: this readback is the guest RT (pre-scanout), so apply the same DC_LUT the present blit applies.
            uint8_t lutR[256], lutG[256], lutB[256];
            const bool bmpLut = m_drawsIntoGuestRt && GammaLutActive();
            for (uint32_t k = 0; k < 256u; ++k) lutR[k] = lutG[k] = lutB[k] = uint8_t(k);
            if (bmpLut) {
                uint32_t lut[256];
                (void)mcla::gpu::CpGammaLut256(lut);
                for (uint32_t k = 0; k < 256u; ++k) {
                    lutR[k] = uint8_t((((lut[k] >> 20) & 1023u) * 255u + 511u) / 1023u);
                    lutG[k] = uint8_t((((lut[k] >> 10) & 1023u) * 255u + 511u) / 1023u);
                    lutB[k] = uint8_t(((lut[k] & 1023u) * 255u + 511u) / 1023u);
                }
            }
            for (uint32_t row = 0; row < m_height; ++row) {
                const uint8_t* srcRow = base + static_cast<size_t>(row) * pitch;
                for (uint32_t col = 0; col < m_width; ++col) {
                    // Swap R and B for BI_RGB Windows BMP
                    rowBuf[col * 4 + 0] = lutB[srcRow[col * 4 + 2]]; // B
                    rowBuf[col * 4 + 1] = lutG[srcRow[col * 4 + 1]]; // G
                    rowBuf[col * 4 + 2] = lutR[srcRow[col * 4 + 0]]; // R
                    rowBuf[col * 4 + 3] = srcRow[col * 4 + 3]; // A
                }
                std::fwrite(rowBuf.data(), 1, m_width * 4, f);
            }
            std::fclose(f);
        }
    }

    m_readbackBuffer->Unmap(0, nullptr);
}

// ===========================================================================
// F-761: THE LATE-FRAME READBACK. A pixel sample of a frame that carries NO
// draw on it, which is the frame the window is showing for the overwhelming
// majority of a run in which the guest draws twice and then stops.
//
// WHY THE DRAW-PATH SAMPLE CANNOT ANSWER THE QUESTION. RecordDrawReadback is
// called from one place, immediately after a draw call is recorded on the open
// list, so a run's pixel census is a census of the frames the guest drew. In
// build/w716.log the guest issued two draws in 150 seconds, at 17:08:05.586 and
// 17:08:05.604, and the run's last present was 17:10:20.932 at presented=4400.
// So both existing samples describe a frame that had been off the screen for
// 135.3 seconds, and every pixel fact read out of that log -- the modal colour,
// the changed box, the distinct-value count -- is a fact about t = 17:08:05 and
// not about what anyone was looking at.
//
// THE BOUND IS NOT THE REASON. kDrawReadbackMaxReadbacks is 4
// (d3d12_backend.h:976) and the run made 2 draw-path readbacks, so the bound was
// never reached and is not why the census has two rows. The reason is that the
// census fires only after a draw and the run contained two draws.
//
// WHAT IS DELIBERATELY DUPLICATED, AND WHY. The census below re-implements the
// histogram, modal value, changed-box and exact-black measurements rather than
// sharing the draw path's. The alternative was refactoring ReportDrawReadback's
// census into a common helper, which would have touched a marker whose format is
// fixed; a refactor that cannot be shown to be byte-identical on 244 tracked
// markers is a worse trade than 60 duplicated lines. The two are kept honest
// against each other by printing the same field names in the same order, so a
// reader can put one row and the other side by side.
//
// drawSeq is NOT printed and neither is any other marker's name: naming another
// marker in this row's prose would make this row a hit for a census counting
// lines, which is how a marker with two real rows came to measure four.
// ===========================================================================

// What the ledger said about the buffer at the moment the copy was recorded, so
// the row describes the buffer it sampled rather than whatever the buffer is by
// the time the row is formatted. File-scope statics for the same reason
// s_readbackCopyAfterDraw is: they are written and read inside one present,
// under m_mutex, and they keep the header free of a second copy of the ledger.
static uint32_t s_lateFrameBuffer = 0;
static uint64_t s_lateFrameEverWritten = 0;
static uint64_t s_lateFramePresents = 0;
static uint64_t s_lateFrameClears = 0;
static uint64_t s_lateFrameDraws = 0;
static uint64_t s_lateFrameBlits = 0;
static uint64_t s_lateFrameDrawsOnList = 0;

bool D3D12Backend::RecordLateFrameReadback() {
    if (m_readbackDisabled) return false;
    static const bool s_lateDiag = [] { const char* e = std::getenv("MCLA_LOG_LEVEL"); return e && e[0] != '6'; }();
    if (!s_lateDiag) return false;
    if (m_lateFrameReadbackAttempts >= D3D12Backend::kLateFrameReadbackMaxReadbacks) {
        if (!m_lateFrameExhaustedLogged) {
            m_lateFrameExhaustedLogged = true;
            MCLA_LOG_INFO("LATE-FRAME-PIXEL exhausted after {} readbacks "
                          "(kLateFrameReadbackMaxReadbacks={}) -- recording OFF for "
                          "the rest of the process, for the reason the draw-path "
                          "bound gives: each readback is a GPU-to-CPU fence wait",
                          m_lateFrameReadbackAttempts,
                          D3D12Backend::kLateFrameReadbackMaxReadbacks);
        }
        return false;
    }
    if (!m_commandList || !m_inFrame) return false;
    if (m_frameIndex >= kBufferCount || !m_renderTargets[m_frameIndex]) return false;
    if (!EnsureDrawReadback()) return false;

    ++m_lateFrameReadbackAttempts;

    // The ledger snapshot and the index are taken BEFORE anything is recorded,
    // so the row describes this copy and not a later one.
    s_lateFrameBuffer = m_frameIndex;
    s_lateFrameDrawsOnList = s_drawIssuedOnList ? 1u : 0u;
    const BufferLedger& led = m_ledger[m_frameIndex];
    s_lateFrameEverWritten = led.everWritten;
    s_lateFramePresents = led.presents;
    s_lateFrameClears = led.clears;
    s_lateFrameDraws = led.draws;
    s_lateFrameBlits = led.blits;

    // This path records NO barrier of its own before the copy: the caller is the
    // refusal branch of ClearAndPresent, which transitions nothing, so the
    // buffer's tracked state on entry is PRESENT -- which is what the previous
    // present left it in. Asserting PRESENT here is the F-725 lesson applied in
    // the other direction: the state named must be the state that is there.
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_readbackBuffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = m_readbackFootprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = m_renderTargets[m_frameIndex].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Straight back to PRESENT, because PresentOwnedFrame presents this buffer
    // immediately afterwards and D3D12 requires the tracked state to match.
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &b);
    return true;
}

void D3D12Backend::ReportLateFrameReadback() {
    if (!m_readbackBuffer || !m_device) return;
    ++m_lateFrameReadbackCompletions;

    // The same discipline the draw path uses, for the same reason: the copy
    // executed when the list was closed and executed, so the Map below must be
    // ordered after the Signal that followed it. s_lastSignalledFenceValue is
    // written by PresentOwnedFrame at the Signal itself and read here after that
    // function has returned, so it names THIS present's list and not a slot
    // indexed by a frame index that has already moved on.
    const uint64_t fenceTarget = s_lastSignalledFenceValue;
    const uint64_t fenceCompletedAtEntry =
        m_fence ? m_fence->GetCompletedValue() : 0;
    bool fenceWaited = false;
    bool fenceWaitTimedOut = false;
    if (m_fence && fenceCompletedAtEntry < fenceTarget) {
        m_fence->SetEventOnCompletion(fenceTarget, m_fenceEvent);
        // Same checked wait as the draw-path readback: a timed-out wait must
        // not fall through to Map, or the sample would read an uncompleted
        // copy and could report a fabricated zero.
        fenceWaited = true;
        if (WaitForSingleObject(m_fenceEvent, 2000) != WAIT_OBJECT_0 ||
            m_fence->GetCompletedValue() < fenceTarget) {
            fenceWaitTimedOut = true;
        }
    }
    const uint64_t fenceCompleted = m_fence ? m_fence->GetCompletedValue() : 0;
    if (fenceWaitTimedOut) {
        ++m_lateFrameReadbackFenceTimeouts;
        const uint64_t n = m_lateFrameReadbackFenceTimeouts;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_WARN("LATE-FRAME-PIXEL #{} fence-wait-timeout target={} "
                          "completed={} -- the copy did not complete in 2 s; the "
                          "sample is NOT taken rather than mapped uncompleted",
                          n, fenceTarget, fenceCompleted);
        }
        return;
    }

    void* mapped = nullptr;
    D3D12_RANGE range = { 0, 0 };
    HRESULT hrMap = m_readbackBuffer->Map(0, &range, &mapped);
    if (FAILED(hrMap) || !mapped) {
        ++m_lateFrameReadbackMapFailures;
        const uint64_t n = m_lateFrameReadbackMapFailures;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_WARN("LATE-FRAME-PIXEL #{} Map hr=0x{:08X} ppData={} buffer={} "
                          "-- the copy executed but no value could be read, so this "
                          "row claims nothing about what the window is showing",
                          n, static_cast<uint32_t>(hrMap), mapped, s_lateFrameBuffer);
        }
        return;
    }

    const uint8_t* base = static_cast<const uint8_t*>(mapped) + m_readbackFootprint.Offset;
    const uint32_t pitch = static_cast<uint32_t>(m_readbackRowPitch);

    // Bounded exactly as the draw-path census is bounded, with the same constant
    // and the same reason: this runs on the render thread and a strided census
    // must never be readable as an exhaustive one, so the stride and the
    // examined counts are printed.
    static constexpr uint64_t kLateCensusMaxPixels = 1ull << 20;
    static constexpr uint32_t kLateCensusDistinctCap = 4096u;
    static constexpr uint32_t kLateCensusTopN = 8u;
    static constexpr uint32_t kLateCensusSlots = kLateCensusDistinctCap * 4u;

    const uint64_t surfacePixels = static_cast<uint64_t>(m_width) * m_height;
    uint32_t censusStride = 1;
    while (censusStride < m_height && surfacePixels / censusStride > kLateCensusMaxPixels) {
        censusStride *= 2;
    }

    uint64_t nonBlack = 0, maxChannel = 0, sum = 0, total = 0, censusRows = 0;
    uint64_t exactBlack = 0;
    uint64_t pr = 0, pg = 0, pb = 0, pa = 0;
    std::vector<uint32_t> slots(kLateCensusSlots, 0u);
    std::vector<uint32_t> keys;
    std::vector<uint64_t> counts;
    keys.reserve(kLateCensusDistinctCap);
    counts.reserve(kLateCensusDistinctCap);
    uint64_t distinctOverflow = 0;
    for (uint32_t y = 0; y < m_height; y += censusStride) {
        const uint8_t* row = base + static_cast<size_t>(y) * pitch;
        for (uint32_t x = 0; x < m_width; ++x) {
            const uint8_t* q = row + static_cast<size_t>(x) * 4u;
            if (q[0] | q[1] | q[2]) ++nonBlack;
            if (q[0] > maxChannel) maxChannel = q[0];
            if (q[1] > maxChannel) maxChannel = q[1];
            if (q[2] > maxChannel) maxChannel = q[2];
            sum += static_cast<uint64_t>(q[0]) + q[1] + q[2];
            ++total;
            const uint32_t key = static_cast<uint32_t>(q[0]) |
                                 (static_cast<uint32_t>(q[1]) << 8) |
                                 (static_cast<uint32_t>(q[2]) << 16) |
                                 (static_cast<uint32_t>(q[3]) << 24);
            uint32_t slot = ((key * 2654435761u) >> 12) & (kLateCensusSlots - 1u);
            uint32_t probes = 0;
            uint32_t found = 0u;
            while (probes < kLateCensusSlots) {
                const uint32_t occupant = slots[slot];
                if (occupant == 0u) break;
                if (keys[occupant - 1u] == key) { found = occupant; break; }
                slot = (slot + 1u) & (kLateCensusSlots - 1u);
                ++probes;
            }
            if (found != 0u) {
                ++counts[found - 1u];
            } else if (slots[slot] == 0u && keys.size() < kLateCensusDistinctCap) {
                keys.push_back(key);
                counts.push_back(1u);
                slots[slot] = static_cast<uint32_t>(keys.size());
            } else {
                ++distinctOverflow;
            }
            if (q[0] == 0u && q[1] == 0u && q[2] == 0u && q[3] == 255u) ++exactBlack;
        }
        ++censusRows;
    }

    // The probe is the viewport centre, taken from the SAME mapped bytes the
    // census just walked, for the reason the draw-path row gives: a probe read at
    // a different instant than the census is how a row comes to contradict
    // itself. Here it is read after the walk rather than inside it, and the walk
    // does not modify anything, so the two cannot disagree.
    {
        const uint8_t* row = base + static_cast<size_t>(m_height / 2u) * pitch;
        const uint8_t* q = row + static_cast<size_t>(m_width / 2u) * 4u;
        pr = q[0]; pg = q[1]; pb = q[2]; pa = q[3];
    }

    // The modal value, decided only now the histogram is complete. Strictly
    // greater-than in first-seen order, so ties go to the value seen first and
    // two runs of the same surface print the same row.
    uint32_t modalKey = 0;
    uint64_t modalCount = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (counts[i] > modalCount) { modalCount = counts[i]; modalKey = keys[i]; }
    }
    const unsigned modalR = modalKey & 0xFFu;
    const unsigned modalG = (modalKey >> 8) & 0xFFu;
    const unsigned modalB = (modalKey >> 16) & 0xFFu;
    const unsigned modalA = (modalKey >> 24) & 0xFFu;

    // Second walk, because the modal value is not known until the first has
    // ended. Comparing a pixel against a colour the same loop has not yet chosen
    // is how a bounding box gets printed for the wrong colour.
    uint64_t changedPx = 0;
    int64_t chgMinX = -1, chgMaxX = -1, chgMinY = -1, chgMaxY = -1;
    for (uint32_t y = 0; y < m_height; y += censusStride) {
        const uint8_t* row = base + static_cast<size_t>(y) * pitch;
        for (uint32_t x = 0; x < m_width; ++x) {
            const uint8_t* q = row + static_cast<size_t>(x) * 4u;
            const uint32_t key = static_cast<uint32_t>(q[0]) |
                                 (static_cast<uint32_t>(q[1]) << 8) |
                                 (static_cast<uint32_t>(q[2]) << 16) |
                                 (static_cast<uint32_t>(q[3]) << 24);
            if (key == modalKey) continue;
            ++changedPx;
            const int64_t lx = static_cast<int64_t>(x);
            const int64_t ly = static_cast<int64_t>(y);
            if (chgMinX < 0 || lx < chgMinX) chgMinX = lx;
            if (chgMaxX < 0 || lx > chgMaxX) chgMaxX = lx;
            if (chgMinY < 0 || ly < chgMinY) chgMinY = ly;
            if (chgMaxY < 0 || ly > chgMaxY) chgMaxY = ly;
        }
    }

    // The top-N histogram, in descending count with first-seen order as the
    // tie-break, so the printed histogram is stable between two runs of one
    // surface.
    std::string topRgba;
    {
        std::vector<uint32_t> order(keys.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<uint32_t>(i);
        std::stable_sort(order.begin(), order.end(),
                         [&](uint32_t l, uint32_t r2) { return counts[l] > counts[r2]; });
        uint32_t shown = 0;
        for (uint32_t idx : order) {
            if (shown >= kLateCensusTopN) break;
            if (shown) topRgba.append(" ");
            char ent[80];
            const uint32_t k = keys[idx];
            std::snprintf(ent, sizeof(ent), "(%u,%u,%u,%u)x%llu",
                          static_cast<unsigned>(k & 0xFFu),
                          static_cast<unsigned>((k >> 8) & 0xFFu),
                          static_cast<unsigned>((k >> 16) & 0xFFu),
                          static_cast<unsigned>((k >> 24) & 0xFFu),
                          static_cast<unsigned long long>(counts[idx]));
            topRgba.append(ent);
            ++shown;
        }
        if (topRgba.empty()) topRgba = "none";
    }

    std::string chgBox = "absent";
    if (changedPx != 0) {
        char box[96];
        std::snprintf(box, sizeof(box), "x%lld..%lld y%lld..%lld",
                      static_cast<long long>(chgMinX), static_cast<long long>(chgMaxX),
                      static_cast<long long>(chgMinY), static_cast<long long>(chgMaxY));
        chgBox = box;
    }

    const uint64_t n = m_lateFrameReadbackAttempts;
    MCLA_LOG_INFO("LATE-FRAME-PIXEL #{} buffer={} of {} buffers everWritten={} "
                  "writes={} clears={} draws={} blits={} presentedWithNoDraw={} "
                  "drawsOnList={} fenceTarget={} fenceCompleted={} fenceWaited={} "
                  "rgba=({},{},{},{}) at ({},{}) of {}x{} rowPitch={} nonBlack={}/{} "
                  "maxChannel={} sum={} channelSamples={} censusRows={} "
                  "censusStride={} distinctRgba={} distinctCapped={} topRgba={} "
                  "modalRgba=({},{},{},{}) modalCount={} changedPx={} changedBox={} "
                  "exactBlack={} -- WHICH BUFFER THE WINDOW WAS SHOWING, sampled "
                  "from a present that recorded NO draw on its command list, so it "
                  "is the frame that was on screen and not a frame the guest drew. "
                  "drawsOnList=0 is the whole point of this row and it is what "
                  "makes it a late-frame sample: a row that can only be produced by "
                  "a draw cannot say this. everWritten=0 would mean the swap chain "
                  "rotated onto a resource nothing has ever written, and the "
                  "present of it is refused by name rather than shown. distinctRgba "
                  "and topRgba are what say whether the window is showing "
                  "uninitialised memory: uninitialised is many colours with the "
                  "counts spread out, a rotated stale frame is one or two colours "
                  "with one count carrying almost all of them. changedBox is the "
                  "smallest rectangle holding every examined pixel that differs "
                  "from the surface's OWN modal value, not from black, so a frame "
                  "whose background is itself non-black still bounds its content. "
                  "Nothing here is clamped and no value is substituted for a pixel "
                  "that was not read",
                  n, s_lateFrameBuffer, kBufferCount, s_lateFrameEverWritten,
                  s_lateFrameClears + s_lateFrameDraws + s_lateFrameBlits,
                  s_lateFrameClears, s_lateFrameDraws, s_lateFrameBlits,
                  s_lateFramePresents, s_lateFrameDrawsOnList,
                  fenceTarget, fenceCompleted, fenceWaited ? 1 : 0,
                  static_cast<unsigned>(pr), static_cast<unsigned>(pg),
                  static_cast<unsigned>(pb), static_cast<unsigned>(pa),
                  m_width / 2u, m_height / 2u, m_width, m_height, pitch,
                  nonBlack, total, maxChannel, sum, total * 3ull,
                  censusRows, censusStride,
                  static_cast<uint64_t>(keys.size()),
                  distinctOverflow ? 1 : 0, topRgba.c_str(),
                  modalR, modalG, modalB, modalA, modalCount,
                  changedPx, chgBox.c_str(), exactBlack);

    m_readbackBuffer->Unmap(0, nullptr);
}

void D3D12Backend::WaitForGpu() {
    if (!m_device || !m_commandQueue || !m_fence || !m_fenceEvent) return;

    uint64_t fenceVal = ++m_currentFenceValue;
    if (SUCCEEDED(m_commandQueue->Signal(m_fence.Get(), fenceVal))) {
        if (m_fence->GetCompletedValue() < fenceVal) {
            m_fence->SetEventOnCompletion(fenceVal, m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }
}

bool D3D12Backend::StreamingUpload(ID3D12Resource* dest, const void* data, uint32_t size) {
    if (!m_device || !m_copyQueue || !m_copyCommandList || !dest || !data || size == 0) return false;

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = (size + 255) & ~255u; // align to 256 bytes
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    HRESULT hr = m_device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&uploadBuffer));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: StreamingUpload CreateCommittedResource failed hr=0x{:08X}",
                       static_cast<uint32_t>(hr));
        return false;
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0};
    hr = uploadBuffer->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: StreamingUpload Map failed hr=0x{:08X}",
                       static_cast<uint32_t>(hr));
        return false;
    }
    memcpy(mapped, data, size);
    uploadBuffer->Unmap(0, nullptr);

    hr = m_copyCommandList->Reset(m_copyAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: StreamingUpload CopyReset failed hr=0x{:08X}",
                       static_cast<uint32_t>(hr));
        return false;
    }

    m_copyCommandList->CopyBufferRegion(dest, 0, uploadBuffer.Get(), 0, size);

    hr = m_copyCommandList->Close();
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: StreamingUpload Close failed hr=0x{:08X}",
                       static_cast<uint32_t>(hr));
        return false;
    }

    ID3D12CommandList* lists[] = { m_copyCommandList.Get() };
    m_copyQueue->ExecuteCommandLists(1, lists);

    return true;
}

uint64_t D3D12Backend::SignalCopyFence() {
    if (!m_copyQueue || !m_copyFence) return 0;
    ++m_copyFenceValue;
    m_copyQueue->Signal(m_copyFence.Get(), m_copyFenceValue);
    return m_copyFenceValue;
}

void D3D12Backend::WaitForCopyFence(uint64_t fenceValue) {
    if (!m_copyFence || !m_copyFenceEvent) return;
    if (m_copyFence->GetCompletedValue() < fenceValue) {
        m_copyFence->SetEventOnCompletion(fenceValue, m_copyFenceEvent);
        WaitForSingleObject(m_copyFenceEvent, INFINITE);
    }
}

void D3D12Backend::SetGraphicsSettings(const GraphicsSettings& s) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    m_graphicsSettings = s;
    MCLA_LOG_INFO("D3D12Backend: Graphics settings updated: internal={}x{}, scale={:.2f}, aspect={}, vsync={}",
                  s.internalWidth, s.internalHeight, s.renderScale, s.aspectCorrection, s.vsync);
}

float D3D12Backend::GuestAspectRatio() const {
    // §2.3: a live menu change wins; otherwise the graphics setting is used.
    // A zero/non-finite value falls back to 16:9 so an unset or corrupted
    // setting cannot produce a degenerate letterbox. Default is 16:9, so an
    // unconfigured build is unchanged.
    const float live = s_wave3LiveAspect.load(std::memory_order_relaxed);
    const float a = (live > 0.0f && std::isfinite(live))
                        ? live : m_graphicsSettings.guestAspectRatio;
    return (a > 0.0f && std::isfinite(a)) ? a : (16.0f / 9.0f);
}

D3D12_VIEWPORT D3D12Backend::GetAspectCorrectViewport(float targetAspect) const {
    if (m_swapW == 0 || m_swapH == 0) {
        return { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    }

    if (targetAspect <= 0.0f) {
        targetAspect = 16.0f / 9.0f;
    }

    const float w = static_cast<float>(m_swapW);
    const float h = static_cast<float>(m_swapH);
    const float windowAspect = w / h;

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, w, h, 0.0f, 1.0f };

    constexpr float kAspectEpsilon = 0.001f;
    if (windowAspect > targetAspect + kAspectEpsilon) {
        // Window is wider than target aspect -> Pillarbox (black bars on left/right)
        const float targetW = h * targetAspect;
        vp.Width = targetW;
        vp.Height = h;
        vp.TopLeftX = (w - targetW) * 0.5f;
        vp.TopLeftY = 0.0f;
    } else if (windowAspect < targetAspect - kAspectEpsilon) {
        // Window is taller than target aspect -> Letterbox (black bars on top/bottom)
        const float targetH = w / targetAspect;
        vp.Width = w;
        vp.Height = targetH;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = (h - targetH) * 0.5f;
    } else {
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = w;
        vp.Height = h;
    }

    return vp;
}

bool D3D12Backend::Resize(uint32_t width, uint32_t height) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || !m_swapChain) return false;

    if (width == 0 || height == 0) {
        m_isMinimized = true;
        MCLA_LOG_INFO("D3D12Backend: Window minimized (width={}, height={}), skipping ResizeBuffers", width, height);
        return true;
    }

    m_isMinimized = false;
    MCLA_LOG_INFO("D3D12Backend: Resizing swap chain to {}x{}", width, height);

    WaitForGpu();

    for (uint32_t i = 0; i < kBufferCount; ++i) {
        m_renderTargets[i].Reset();
        m_fenceValues[i] = m_fenceValues[m_frameIndex];
    }

    // Blit texture is sized to the backbuffer; recreate on next PresentBgra.
    m_fbTexture.Reset();
    m_fbUpload.Reset();
    m_fbFootprint = {};
    m_fbTexW = 0;
    m_fbTexH = 0;
    m_fbTexState = D3D12_RESOURCE_STATE_COPY_DEST;

    HRESULT hr = m_swapChain->ResizeBuffers(kBufferCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                            m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: ResizeBuffers failed with hr=0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }

    m_width = width;
    m_height = height;

    // The extent CHANGED, so the rescale must be republished for every shader
    // translated from here on. Placed after ResizeBuffers has succeeded, so the
    // published extent is always one the swapchain really has. Resize is reached
    // from the window-resize path (src/app.cpp:328 OnResize -> RESIZE command ->
    // src/render_thread.cpp:4183), and build/w707.log:386 shows it firing once
    // during initialization, before any draw.
    // Guest-RT draws keep the guest surface's extent; only the window changed.
    if (!m_guestRtValid)
        PublishClipDisableExtent("resize", m_width, m_height, m_cachePipelineDraws);

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Viewport/scissor + constant-bank addresses from the old size/list are
    // stale after ResizeBuffers; force re-set/re-upload on the next draw.
    InvalidateDrawCaches();
    BumpRtPassEpoch();

    // F-761: ResizeBuffers has just handed back NEW back-buffer resources. Their
    // contents are undefined, so every "was this buffer ever written" answer from
    // before this call describes resources that no longer exist. MEASURED in
    // build/w716.log: this fires at 17:07:53.702, the same millisecond as the
    // first present of the run, which is precisely why the first three presents
    // after it are the ones that make all three indices written.
    ResetBufferLedger("ResizeBuffers");

    return CreateRenderTargets();
}

// Wait until the GPU is done with everything recorded against the current back
// buffer. Callers that write into the upload arena region for this frame must
// do so only after this returns, otherwise the CPU can overwrite bytes the GPU
// is still reading from the previous frame that used the same region.
bool D3D12Backend::WaitForCurrentFrameGpu() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    return WaitForCurrentFrameGpuUnlocked();
}

bool D3D12Backend::WaitForCurrentFrameGpuUnlocked() {
    if (!m_initialized) return false;
    // R8: the fence's completed value is monotonic, so once it has been observed >= the value this
    // frame slot needs, later calls skip the GetCompletedValue COM call. MCLA_NO_FENCE_LB=1 disables.
    static const bool s_fenceLb = [] { const char* e = std::getenv("MCLA_NO_FENCE_LB"); return !(e && e[0] == '1'); }();
    if (s_fenceLb && m_fenceValues[m_frameIndex] <= m_fenceCompletedLB) {
        g_fenceWaitCalls.fetch_add(1, std::memory_order_relaxed);
        if (!m_pendingOccQueries.empty()) PollPendingOcclusionQueries();
        return true;
    }
    uint64_t completedVal = m_fence->GetCompletedValue();
    if (completedVal > m_fenceCompletedLB) m_fenceCompletedLB = completedVal;
    g_fenceWaitCalls.fetch_add(1, std::memory_order_relaxed);
    if (completedVal < m_fenceValues[m_frameIndex]) {
        const auto t0 = std::chrono::steady_clock::now();
        m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
        g_fenceWaitNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
        g_fenceWaitBlocked.fetch_add(1, std::memory_order_relaxed);
        if (m_fenceValues[m_frameIndex] > m_fenceCompletedLB) m_fenceCompletedLB = m_fenceValues[m_frameIndex];  // the wait just proved it
    }
    if (!m_pendingOccQueries.empty()) PollPendingOcclusionQueries();
    return true;
}

// 2026-10-02: the gate's own name table. A function-local static table rather
// than a switch in the recorder so the token a soak greps for is written down
// exactly once per gate and the recorder cannot be handed a name that does not
// match its counter index.
static const char* const kBeginFrameGateName[] = {
    "not-initialised",  // BeginFrameGate::kNotInitialized
    "already-in-frame", // BeginFrameGate::kAlreadyInFrame
    "fence-gate",       // BeginFrameGate::kFenceGate
    "allocator-reset",  // BeginFrameGate::kAllocatorReset
    "commandlist-reset",// BeginFrameGate::kCommandListReset
};
static_assert(sizeof(kBeginFrameGateName) / sizeof(kBeginFrameGateName[0]) ==
                  static_cast<size_t>(D3D12Backend::BeginFrameGate::kCount),
              "every BeginFrameGate needs a name token, or a refusal would be "
              "counted under a gate the log cannot name");

void D3D12Backend::RecordBeginFrameRefusal(BeginFrameGate gate, uint32_t a,
                                           uint32_t b) {
    const size_t idx = static_cast<size_t>(gate);
    // Counted UNCONDITIONALLY, before the throttle (F-649: the throttle gates
    // the TEXT, never the count -- a suppressed row that is not counted is how a
    // soak reads "nothing happened" out of something that happened 616,824
    // times). Per-gate n, so each exit throttles on its OWN count and a flood
    // of one gate cannot silence another.
    const uint64_t n = ++m_beginFrameRefusalByGate[idx];
    ++m_beginFrameRefusals;
    if (!ShouldLogRefusal(n)) {
        return;
    }
    MCLA_LOG_WARN(
        "D3D12-BEGINFRAME-REFUSE #{} gate={} a={} b={} returned=false | "
        "begins={} refusals={} frameIndex={} inFrame={} initialized={} "
        "presented={} drawn={} abandoned={} | a/b are the refusing gate's own "
        "inputs, copied verbatim at the call site and NOT interpreted: "
        "not-initialised passes (0,0); already-in-frame passes "
        "(m_frameIndex, m_presentedFrames); fence-gate passes "
        "(m_frameIndex,0); allocator-reset and commandlist-reset pass "
        "(m_frameIndex, HRESULT). begins=0 with refusals>0 means the frame "
        "context NEVER opened, which is a different fault from a frame refused "
        "once in a while. already-in-frame is the latch fault: a frame was begun "
        "and never released (see D3D12-FRAME-ABANDONED).",
        n, kBeginFrameGateName[idx], a, b, m_beginFrameBegins,
        m_beginFrameRefusals, m_frameIndex, m_inFrame ? 1u : 0u,
        m_initialized ? 1u : 0u, m_presentedFrames, m_drawOwnedFrames,
        m_framesAbandoned);
}

// 2026-10-02: the release-on-abandon half of the BeginFrame fix. See the
// declaration in d3d12_backend.h for why `m_inFrame` needed one.
//
// WHAT IT DOES AND DOES NOT DO. It closes the command list and clears
// `m_inFrame` and `m_frameWriter`, and nothing else: no render target is
// created, no back buffer is bound, no command is recorded, no HRESULT is
// forced. The next BeginFrame() Resets the command allocator and the command
// list, which discards whatever this frame recorded but never executed.
//
// THE Close() IS NOT OPTIONAL, AND THE HARNESS IS WHY. The first version of
// this function cleared `m_inFrame` and nothing else, on the reasoning that
// "the next BeginFrame's Reset discards the unexecuted commands". MEASURED on
// this machine against real D3D12 objects, that is FALSE:
// ID3D12CommandAllocator::Reset fails with hr=0x80070057 while any command
// list is still RECORDING with that allocator, and the debug layer names it --
// "The command allocator cannot be reset because a command list is currently
// being recorded with the allocator." So clearing the latch alone converts a
// refusal on `already-in-frame` into a refusal on `allocator-reset`: still
// refused, still silent about why, and now with a misleading gate name.
// BeginFrame() opened the list, so abandoning the frame must close it. That is
// the whole D3D12 rule, and it is why the fix is two statements and not one.
//
// The Close() HRESULT is deliberately NOT checked as a pass/fail: closing a list
// that is already closed returns 0x80004005 (measured) and that is not an error
// here, it means the frame never opened. `m_inFrame` is the real guard -- the
// function returns before touching the list when no frame is in flight, which
// is also what makes it safe to call unconditionally at every
// BeginFrame-then-call site without threading a success flag out of five
// different functions.
void D3D12Backend::AbandonFrame(const char* owner) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_inFrame) {
        return;
    }
    if (m_commandList && m_mainListOpen) {
        CloseActiveOcclusionQueriesBeforeClose();
        // Best-effort. A failure here is recorded and bounded, not fatal: the
        // latch is still released below, and a list that would not close is a
        // condition the NEXT BeginFrame's Reset will report by name.
        const HRESULT hrClose = m_commandList->Close();
        m_mainListOpen = false;
        if (FAILED(hrClose)) {
            const uint64_t cn = ++m_frameAbandonCloseFailures;
            if (ShouldLogRefusal(cn)) {
                MCLA_LOG_WARN("D3D12-FRAME-ABANDON #{} owner={} list->Close() "
                              "failed hr=0x{:08X}; the latch is still being "
                              "released, so the next BeginFrame may refuse on "
                              "its own gate and will name it",
                              cn, owner, static_cast<uint32_t>(hrClose));
            }
        }
    }
    if (m_mtRecorderCount > 1) {
        for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
            if (m_mtCommandLists[r] && m_mtRecorderState[r].listOpen) {
                (void)m_mtCommandLists[r]->Close();
                m_mtRecorderState[r].listOpen = false;
            }
            m_mtRecorderState[r].ResetFrameAccumulators();
            m_mtRecorderState[r].ResetListBindState();
        }
    }
    m_activePassTargets = GuestPassTargets{};
    m_inFrame = false;
    m_frameWriter = FrameOwner::None;
    m_boundTargetsValid = false;  // list closed; see the cache's declaration
    InvalidateDrawCaches();
    BumpRtPassEpoch();
    for (uint32_t h = 0; h < kSamplerHeapCount; ++h) {
        for (auto& sb : m_samplerHeaps[h].blocks) {
            if (sb.fenceValue == UINT64_MAX) sb.fenceValue = m_currentFenceValue;
            if (sb.lastBeginFrame == m_beginFrameBegins) sb.lastBeginFrame = 0;
        }
    }
    m_samplerBlocks = m_samplerHeaps[0].blocks;
    if (m_drawsThisFrame > m_maxDrawsThisFrame)
        m_maxDrawsThisFrame = m_drawsThisFrame;
    m_drawsThisFrame = 0;
    // F-900: REVERT THE GUEST-RT STATE LIE. (see header for the full rationale)
    {
        static uint32_t s_abandonDiag = 0;
        const uint32_t ad = ++s_abandonDiag;
        if (ad <= 6) {
            MCLA_LOG_INFO("ABANDON-STATE #{} owner={} guestRtState={} restoring={}",
                          ad, owner,
                          static_cast<unsigned>(m_guestRtState),
                          static_cast<unsigned>(m_guestRtStateAtFrameStart));
        }
    }
    m_guestRtState = m_guestRtStateAtFrameStart;
    for (auto& kv : m_rtCache) kv.second.state = kv.second.stateAtFrameStart;
    for (auto& kv : m_resolved) kv.second.state = kv.second.stateAtFrameStart;
    for (auto& kv : m_resMips) kv.second.tex.state = kv.second.tex.stateAtFrameStart;
    for (auto& kv : m_resolvedAlt) {
        for (ResolvedTex& r : kv.second) r.state = r.stateAtFrameStart;
    }
    if (m_activeRt) m_guestRtState = m_activeRt->state;
    const uint64_t n = ++m_framesAbandoned;
    if (ShouldLogRefusal(n)) {
        MCLA_LOG_WARN(
            "D3D12-FRAME-ABANDONED #{} owner={} beganFrame={} frameIndex={} "
            "presented={} drawn={} -- the frame was begun and then DROPPED "
            "without a present, so its command list was closed and m_inFrame "
            "released here instead of leaving the next BeginFrame to refuse. "
            "Both halves are required: releasing the latch alone leaves the "
            "list recording and the next ID3D12CommandAllocator::Reset fails "
            "with 0x80070057.",
            n, owner, m_beginFrameBegins, m_frameIndex, m_presentedFrames,
            m_drawOwnedFrames);
    }
}

bool D3D12Backend::BeginFrameForDraw() {
   std::lock_guard<std::recursive_mutex> lock(m_mutex);
   if (m_batchDrawFrames && m_inFrame) {
       static std::atomic<uint64_t> s_batchReuseLog{0};  // MD-32: m_batchDrawFrameReuses resets per frame
       const uint64_t n = ++m_batchDrawFrameReuses;
       if (ShouldLogRefusal(++s_batchReuseLog)) {
           MCLA_LOG_WARN(
               "D3D12-DRAW-BATCH-REUSE #{} frameIndex={} presented={} drawn={} "
               "-- continued the open batch command list; the guest swap "
               "closes and presents it",
               n, m_frameIndex, m_presentedFrames, m_drawOwnedFrames);
       }
       return true;
   }
   return BeginFrame();
}

bool D3D12Backend::BeginFrame() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    // 2026-10-02: EVERY exit below names itself. This function used to be
    // five `return false`s with no log line on any of them, and it is the one
    // boolean that gates the whole draw path: render_thread.cpp:3598 stores its
    // result as `beganFrame`, and DRAW-CP-ISSUED then reports beginFrame=0 --
    // a row that named the outcome and withheld the cause. MEASURED on
    // build/w698.log (55,892 lines), which carries exactly two DRAW-CP-ISSUED
    // rows, both beginFrame=0, and zero of every marker downstream of this
    // gate, so the log could not say which of these five fired.
    //
    // The two conditions on the first line are SEPARATE refusals, not one: a
    // backend that was never initialised and a frame that is already open are
    // different faults with different fixes, and merging them into one token
    // would have made the row ambiguous in exactly the case it exists for.
    if (!m_initialized) {
        RecordBeginFrameRefusal(BeginFrameGate::kNotInitialized, 0u, 0u);
        return false;
    }
    if (m_inFrame) {
        RecordBeginFrameRefusal(BeginFrameGate::kAlreadyInFrame, m_frameIndex,
                                static_cast<uint32_t>(m_presentedFrames));
        return false;
    }

    if (!WaitForCurrentFrameGpuUnlocked()) {
        // Unreachable from the check above TODAY: WaitForCurrentFrameGpu's only
        // false path is its own `!m_initialized` (:3349), BeginFrame already
        // tested the same flag while holding the same recursive mutex, and
        // m_initialized is written only under that mutex (:525 true, :3875
        // false). It is named rather than deleted so that if that invariant is
        // ever broken the log says so instead of going quiet.
        RecordBeginFrameRefusal(BeginFrameGate::kFenceGate, m_frameIndex, 0u);
        return false;
    }

    HRESULT hr = m_commandAllocators[m_frameIndex]->Reset();
    if (FAILED(hr)) {
        if (m_commandList && m_mainListOpen) {
            (void)m_commandList->Close();
            m_mainListOpen = false;
            hr = m_commandAllocators[m_frameIndex]->Reset();
        }
        if (FAILED(hr)) {
            RecordBeginFrameRefusal(BeginFrameGate::kAllocatorReset, m_frameIndex,
                                    static_cast<uint32_t>(hr));
            return false;
        }
    }

    hr = m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);
    if (FAILED(hr)) {
        static const bool s_noCmdlistRecover = [] {
            const char* e = std::getenv("MCLA_NO_CMDLIST_RECOVER");
            return e && e[0] == '1';
        }();
        bool recovered = false;
        if (!s_noCmdlistRecover && m_device) {
            const HRESULT reason = m_device->GetDeviceRemovedReason();
            if (reason == S_OK) {
                const HRESULT origHr = hr;
                m_commandList.Reset();
                HRESULT recHr = m_device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    m_commandAllocators[m_frameIndex].Get(), nullptr,
                    IID_PPV_ARGS(&m_commandList));
                if (SUCCEEDED(recHr) && m_commandList) {
                    recovered = true;
                    m_mainListOpen = true;
                }
                static std::atomic<uint32_t> s_recoverCount{0};
                const uint32_t rn = s_recoverCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (rn <= 16 || (rn % 500) == 0) {
                    MCLA_LOG_WARN("D3D12-CMDLIST-RECOVER #{} frameIndex={} origHr=0x{:08X} recoverHr=0x{:08X} recovered={}",
                                  rn, m_frameIndex, static_cast<uint32_t>(origHr),
                                  static_cast<uint32_t>(recHr), recovered ? 1 : 0);
                }
            }
        }
        if (!recovered) {
            m_mainListOpen = false;
            RecordBeginFrameRefusal(BeginFrameGate::kCommandListReset, m_frameIndex,
                                    static_cast<uint32_t>(hr));
            return false;
        }
    }
    m_mainListOpen = true;
    m_activePassTargets = GuestPassTargets{};
    // New frame: invalidate SRV table caches so stale ring handles are never reused.
    ++m_srvTableEpoch;
    // Reset per-recorder allocators/lists and partitioned regions for this frame slot.
    if (m_mtRecorderCount > 1) {
        const uint32_t frameSlot = m_frameIndex;
        const uint32_t globalRingCap = SrvRingTablesPerFrame();
        const size_t regionBase = static_cast<size_t>(frameSlot) * m_uploadRegionSize;
        const D3D12_GPU_DESCRIPTOR_HANDLE neutralSamplerGpu =
            m_samplerHeap ? m_samplerHeap->GetGPUDescriptorHandleForHeapStart()
                          : D3D12_GPU_DESCRIPTOR_HANDLE{};
        for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
            if (m_mtCommandLists[r] && m_mtRecorderState[r].listOpen) {
                (void)m_mtCommandLists[r]->Close();
                m_mtRecorderState[r].listOpen = false;
            }
            if (m_mtAllocators[frameSlot][r]) {
                (void)m_mtAllocators[frameSlot][r]->Reset();
            }
            bool listOpened = false;
            if (m_mtCommandLists[r] && m_mtAllocators[frameSlot][r]) {
                const HRESULT mhr = m_mtCommandLists[r]->Reset(m_mtAllocators[frameSlot][r].Get(), nullptr);
                listOpened = SUCCEEDED(mhr);
                if (FAILED(mhr)) {
                    MCLA_LOG_ERROR("D3D12Backend: MT command list Reset failed at BeginFrame rec={} hr=0x{:08X}",
                                   r, static_cast<uint32_t>(mhr));
                }
            }
            m_mtUploadOffset[r] = 0;
            m_mtSrvRingOffset[r] = 0;
            const uint32_t tableBaseSlot = kSrvHeapBaseCount +
                (frameSlot * globalRingCap + m_mtSrvRingBase[r]) * kShaderTextureSlotCount;
            D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<size_t>(tableBaseSlot) * m_srvDescriptorSize;
            m_mtSrvTableGpu[r] = h;
            DrawRecorder& st = m_mtRecorderState[r];
            st = DrawRecorder{};
            st.index = r;
            st.list = m_mtCommandLists[r].Get();
            st.listOpen = listOpened;
            st.uploadRegionBase = regionBase + m_mainUploadRegionSize + static_cast<size_t>(r) * m_mtUploadSubRegionSize;
            st.uploadSubRegionSize = m_mtUploadSubRegionSize;
            st.srvRingBase = m_mtSrvRingBase[r];
            st.srvRingTables = m_mtSrvRingTablesPerRecorder;
            st.srvTableGpu = h;
            st.samplerTableGpu = neutralSamplerGpu;
        }
    }
    // F-XXX: a fresh list has nothing bound. See the cache's declaration.
    m_boundTargetsValid = false;
    InvalidateDrawCaches();

    RefreshNoLastColour();
    if (PixProbeEnabled()) {
        s_pixprobe3d = {};
        s_pixprobe3dRingCount = 0;
        s_pixprobeFrame3dCount = 0;
        s_pixprobe3dRbCapturedThisFrame = false;
        for (bool& p : m_pixprobe3dRbPending) p = false;
    }
    if (g_f9State.load(std::memory_order_relaxed) == 1) F9ArmFrame();
    m_inFrame = true;
    m_batchDrawFrameHasDraw = false;
    m_uploadOffset = 0; ++m_uploadEpoch;
    s_frameSrvRingCursor.store(0, std::memory_order_relaxed);
    m_srvRingOffset = 0;
    m_srvRingPassesFrame = 0;
    m_srvRingClampPassesFrame = 0;
    m_srvRingFrameHadClamp = false;
    m_currentSrvTableGpu = m_srvHeap ? m_srvHeap->GetGPUDescriptorHandleForHeapStart()
                                     : D3D12_GPU_DESCRIPTOR_HANDLE{};
    // F-900: snapshot the TRACKED guest-RT state before this frame records
    // anything. If this frame is later abandoned, AbandonFrame restores it --
    // the discarded recording never executed, so every state change it made to
    // the flag was a lie about the resource's real state.
    m_guestRtStateAtFrameStart = m_guestRtState;
    SyncActiveRtState();
    for (auto& kv : m_rtCache) kv.second.stateAtFrameStart = kv.second.state;
    for (auto& kv : m_resolved) kv.second.stateAtFrameStart = kv.second.state;
    for (auto& kv : m_resMips) kv.second.tex.stateAtFrameStart = kv.second.tex.state;
    for (auto& kv : m_resolvedAlt) {
        for (ResolvedTex& r : kv.second) r.stateAtFrameStart = r.state;
    }
    ++m_beginFrameBegins;
    BumpRtPassEpoch();
    return true;
}

bool D3D12Backend::EnsureFbTexture(uint32_t width, uint32_t height) {
    if (m_fbTexture && m_fbUpload && m_fbTexW == width && m_fbTexH == height) {
        return true;
    }

    RetireTarget(std::move(m_fbTexture));
    RetireTarget(std::move(m_fbUpload));
    m_fbFootprint = {};
    m_fbTexW = 0;
    m_fbTexH = 0;
    m_fbTexState = D3D12_RESOURCE_STATE_COPY_DEST;

    D3D12_HEAP_PROPERTIES defProps = {};
    defProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommittedResource(
        &defProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&m_fbTexture));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: EnsureFbTexture CreateCommittedResource failed hr=0x{:08X}",
                       static_cast<uint32_t>(hr));
        return false;
    }

    UINT64 totalBytes = 0;
    m_device->GetCopyableFootprints(&texDesc, 0, 1, 0, &m_fbFootprint, nullptr, nullptr, &totalBytes);

    D3D12_HEAP_PROPERTIES upProps = {};
    upProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = totalBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = m_device->CreateCommittedResource(&upProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                           nullptr, IID_PPV_ARGS(&m_fbUpload));
    if (FAILED(hr)) {
        MCLA_LOG_ERROR("D3D12Backend: EnsureFbTexture staging failed hr=0x{:08X}",
                       static_cast<uint32_t>(hr));
        m_fbTexture.Reset();
        m_fbFootprint = {};
        return false;
    }

    m_fbTexW = width;
    m_fbTexH = height;
    m_fbTexState = D3D12_RESOURCE_STATE_COPY_DEST;
    MCLA_LOG_INFO("D3D12Backend: blit framebuffer texture ready ({}x{}, R8G8B8A8, {} bytes)",
                  width, height, totalBytes);
    return true;
}

// ===========================================================================
// F-650: the back-buffer arbiter -- exactly one writer per presented frame.
//
// WHICH SURFACE THE CONSOLE RENDERS INTO, and which one this host must
// present. Measured from the guest's own frame-end path, not assumed. The
// guest drives the Xenos display engine every frame; the last
// PRESENT-SURF-AUDIT in build/w662.log (frame=292920, w662.log:797561) reads:
//
//   presents=292920 vdSwapCalls=146460 routeRatio=live
//   vdObj=C6CDA6D0 vdInfo=004E0D30
//   | resolved=CB17BF80 score=60 | phys=004E0D30 score=1 | kva=C04E0D30 score=0
//   => resolved-partially-varied
//
// `routeRatio=live` is the load-bearing part: the guest really is issuing a
// display-engine swap (146,460 of them), and that swap resolves to exactly ONE
// scanout surface, guest VA 0xCB17BF80. Of the three candidates it is the only
// one with anything in it -- score 60, "partially varied" -- while the swap's
// own source (phys=004E0D30) and its KVA (C04E0D30) are both flat, score 1 and
// 0. So on the console there is a single scanout image and this is it.
//
// WHY THAT FORCES ARRANGEMENT (a) -- draw into the surface that is presented.
// 0xCB17BF80 is GUEST memory. The host's draws are not: they go through
// DrawDynamicMeshWithPipeline into m_renderTargets[m_frameIndex], the D3D12
// swapchain back buffer. Nothing in this backend writes guest memory, and
// nothing establishes that 0xCB17BF80's bytes were produced by our draws -- they
// are whatever the guest's own Xenos emulation left there. The two surfaces
// cannot be reconciled by pointing them at each other.
//
// Arrangement (b) -- offscreen target, blit the guest framebuffer, composite --
// is therefore not merely more work, it is the current defect in a more
// expensive form. If the guest framebuffer is presented on top, the frame is
// 100% blit and 0% draw, so the real geometry stays invisible exactly as it is
// now and the draws become decoration -- which is the premise (b) requires them
// to be. That premise is also contradicted by the guest's own behaviour:
// DRAW-CP-DRAWN #20600 prim=8 vertices=3 indexed=0 is the guest issuing real
// geometry, and those pixels exist ONLY in the host back buffer.
//
// So: the render target and the presented image are the SAME OBJECT. The draw
// path owns it. The blit and the clear are refused, counted and named, instead
// of being left to win the frame on a race.
// ===========================================================================

const char* D3D12Backend::FrameOwnerName(FrameOwner owner) {
    switch (owner) {
        case FrameOwner::Draw:  return "draw";
        case FrameOwner::Blit:  return "blit";
        case FrameOwner::Clear: return "clear";
        case FrameOwner::None:  break;
    }
    return "none";
}

// ===========================================================================
// F-761: THE PER-BUFFER WRITE LEDGER, AND THE REFUSAL IT MAKES POSSIBLE.
//
// WHAT QUESTION THIS EXISTS TO ANSWER, AND WHY THE ARBITER ABOVE CANNOT. The
// arbiter answers "who may write the buffer that is current right now". It
// cannot answer "has buffer N ever been written", because
// PresentOwnedFrame advances m_frameIndex to
// m_swapChain->GetCurrentBackBufferIndex() on EVERY present, so the previous
// frame's ownership is discarded before anyone can ask about it. kBufferCount is
// 3 (d3d12_backend.h:34) and the chain rotates through all three, which makes
// "the buffer the window is showing" a different buffer from "the buffer the
// last draw wrote" for two presents out of three.
//
// THE MEASUREMENT THAT PROVES THE ROTATION IS REAL, from build/w716.log by
// timestamp. 17:07:53.702 is a ResizeBuffers (D3D12Backend: Resizing swap
// chain to 1280x720), so all three back buffers are brand-new resources at that
// instant and none of them holds anything. The first three presents after it
// carry owner=clear, which reaches ClearRenderTargetView on
// m_renderTargets[m_frameIndex] and then presents that same index -- so buffers
// 0, 1 and 2 are each written by present #1, #2 and #3 respectively. Every
// subsequent present rotates to the next index. That is why the draw-path
// sample rows at 17:08:05.582 and .604 describe a DIFFERENT buffer from the one
// the window was showing at 17:10:20.932 (presented=4400).
//
// WHY THE REFUSAL IS HERE AND NOT IN THE ARBITER. A clear, a blit and a draw
// all WRITE; the refusal is about PRESENTING a resource whose contents were
// never defined, which is a different question and belongs to the present. It
// is deliberately a refusal and not a fix: charter F-609 forbids inventing a
// pixel, and an undefined resource has no honest value to substitute -- not
// black (a black frame is a measurement), not a held copy of the last good
// frame (that is a cover-up), and not the placeholder constant (that is the
// synthetic image F-758 deleted).
// ===========================================================================

void D3D12Backend::ResetBufferLedger(const char* why) {
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        m_ledger[i] = BufferLedger();
    }
    MCLA_LOG_INFO("BUFFER-LEDGER RESET via={} buffers={} -- every 'ever written' "
                  "answer is now 0, because this handed back NEW back-buffer "
                  "resources whose contents are not the old contents. A chain that "
                  "rotates through buffers nothing has written presents "
                  "uninitialised resource memory, which is what this ledger makes "
                  "visible instead of leaving to be guessed at",
                  why, kBufferCount);
}

void D3D12Backend::NoteBufferWrite(const char* how) {
    if (m_frameIndex >= kBufferCount) return;
    BufferLedger& led = m_ledger[m_frameIndex];
    led.everWritten = 1;
    // F-763: THE CONTENT VERSION MOVES HERE AND NOWHERE ELSE. One bump per
    // write, at the single chokepoint every write to this resource passes
    // through, so contentSeq names a STATE of the resource rather than a moment
    // on a clock. The four callers are measured: "draw-path-clear" (the draw
    // path's own placeholder clear), "draw" (the draw call itself), "blit"
    // (CopyResource from the composited framebuffer) and "clear"
    // (ClearAndPresent's ClearRenderTargetView). Every other use of
    // m_renderTargets[m_frameIndex] in this file copies OUT of it for a
    // readback, reads its descriptor, transitions it, or resets it, and none of
    // those changes a byte, so none of them may bump this.
    ++led.contentSeq;
    if (std::strcmp(how, "draw") == 0) {
        ++led.draws;
    } else if (std::strcmp(how, "blit") == 0) {
        ++led.blits;
    } else {
        ++led.clears;
    }
}

bool D3D12Backend::RefuseUnwrittenPresent(const char* ownerName) {
    if (m_frameIndex >= kBufferCount) return false;
    const BufferLedger& led = m_ledger[m_frameIndex];
    if (led.everWritten) return false;
    const uint64_t n = ++m_unwrittenPresentRefused;
    if (ShouldLogRefusal(n)) {
        MCLA_LOG_ERROR("FRAME-UNWRITTEN-PRESENT-REFUSED #{} writer={} buffer={} of "
                       "{} buffers presents={} clears={} draws={} blits={} "
                       "censusSamples={} -- REFUSED, and this is the honest answer "
                       "rather than a fix: this buffer has never been written by "
                       "anything, so its contents are undefined and presenting it "
                       "would show uninitialised resource memory. Nothing is "
                       "substituted for it. A black frame would be a measurement "
                       "this backend has not taken, a held copy of the last good "
                       "frame would be a cover-up, and the placeholder constant is "
                       "the synthetic image that was deleted. The window keeps "
                       "whatever it was already showing",
                       n, ownerName, m_frameIndex, kBufferCount, led.presents,
                       led.clears, led.draws, led.blits, m_lateFrameReadbackAttempts);
    }
    return true;
}

// F-763: THE EMPTY BEAT IS HELD, NOT FLIPPED.
//
// THE DEFECT THIS REMOVES, MEASURED. build/w718.log, 150 s soak, 50,403 lines,
// 4,400 flips. PresentOwnedFrame advances m_frameIndex to
// m_swapChain->GetCurrentBackBufferIndex() on EVERY flip, kBufferCount is 3, and
// the guest issued 2 draws (DRAW-CP-ISSUED #1 at 19:21:29.255 and #2 at
// 19:21:29.301) then stopped. Every beat after that reached this file's
// FrameOwner::None branch -- 4,041 clearRefused by the last FRAME-OWNER row at
// 19:23:44.659 -- and each one flipped a DIFFERENT buffer into a window whose
// last real content was the guest's.
//
// THE ROTATION IS THE FLASH, AND THE PIXELS PROVE IT. The four
// LATE-FRAME-PIXEL rows (19:21:29.759 .. .859) sampled three consecutive beats
// and read three different surfaces out of one window:
//   #1 buffer=1 rgba=(15,25,56,255) distinctRgba=1  changedPx=0
//   #2 buffer=2 rgba=(5,8,13,255)  distinctRgba=2  changedPx=230400
//            changedBox=x0..639 y0..359
//   #3 buffer=0 rgba=(0,0,0,0)    distinctRgba=1  changedPx=0
// (15,25,56) is the startup clear's own colour -- render_thread.cpp:134 asks for
// (0.06f, 0.10f, 0.22f, 1.0f), which is 15.3, 25.5, 56.1 in 8-bit -- and it is
// the "dark blue" half of the flash the user reported. (0,0,0,0) is the "black"
// half. Buffer 2 carries the guest's draw and is the only one of the three that
// is guest content. So the window cycled navy -> guest -> zeros -> navy at the
// 30 Hz heartbeat cadence, and nothing in the log said so, because every one of
// those flips was a SUCCESS and reported as one.
//
// WHAT "NEW CONTENT" MEANS HERE, AND IT IS NOT A GUESS ABOUT THE GUEST. It is
// BufferLedger::contentIsNew(): this resource's content version differs from the
// version this same index last handed the compositor. Both halves are maintained
// at the only two places that can know them -- NoteBufferWrite bumps contentSeq,
// PresentOwnedFrame records contentShown -- and both are per-index, so the
// question asked is "would flipping THIS buffer change the window", which is the
// question a flip actually answers. Nothing here consults the guest, the clock,
// or a frame counter.
//
// WHY HOLDING IS THE HONEST DISPLAY AND NOT A COVER-UP. The window keeps
// displaying the guest's own last frame, byte for byte, because nothing has
// overwritten it and nothing has been substituted into it. That is the same
// thing a real console does when a title stops submitting frames. The
// alternatives were measured, not argued: a held SYNTHETIC frame is what F-758
// deleted and F-609 forbids; presenting an unwritten resource is what
// RefuseUnwrittenPresent above refuses; and the only remaining option -- keep
// flipping -- is the defect.
//
// WHAT THIS MUST NOT BECOME. F-760 is the defect immediately upstream of this
// one: it froze displayed= at 360 for 136 s because the clear was refused and
// the FRAME with it was abandoned, so a frame the guest HAD drawn never reached
// the screen. The two differ in exactly one measurable way and the next soak
// separates them. F-760 suppressed flips whose buffer held content the window
// had not seen; this suppresses only flips whose buffer holds the bytes already
// on screen. So a guest that draws presents on the draw's own flip -- contentSeq
// moved, contentIsNew() is true, the flip is not gated -- and only the beats in
// between are held. If a future soak shows a draw with no flip after it, this
// has reproduced F-760 and is wrong.
//
// THE MARKER IS RATE-LIMITED, because a beat is ~30 Hz and an unbounded
// per-beat line is how this tree has been burned before (see the 616,824-row
// note at soak_census.py:65-66). ShouldLogRefusal is the house pattern: the
// first 8, then every 200th, which is 29 lines across w718's 4,400 beats. Every
// line carries shownTotal and heldTotal so a reader sees presented= standing
// still while this counter climbs, which is the whole of the change in two
// numbers.
bool D3D12Backend::RefuseUnchangedPresent(const char* ownerName) {
    if (m_frameIndex >= kBufferCount) return false;
    const BufferLedger& led = m_ledger[m_frameIndex];
    if (led.contentIsNew()) return false;
    const uint64_t n = ++m_heldBeatsNotShown;
    if (ShouldLogRefusal(n)) {
        MCLA_LOG_WARN("BEAT-HELD-NO-NEW-CONTENT #{} shownTotal={} heldTotal={} "
                      "buffer={} of {} contentSeq={} contentShown={} writes={} "
                      "clears={} draws={} blits={} everWritten={} writer={} -- "
                      "this beat showed nothing new, so no flip was issued and "
                      "m_frameIndex was NOT advanced. contentSeq equals the "
                      "contentSeq this same index already showed, so its bytes "
                      "are identical; flipping it would only rotate the window "
                      "onto a buffer holding a DIFFERENT earlier frame, and that "
                      "alternation is the flash the user reported. The window "
                      "keeps the last frame a real writer produced. Nothing is "
                      "synthesised, nothing is clamped, no rotation is "
                      "re-enabled, and shownTotal is meant to be standing still",
                      n, m_presentedFrames, n, m_frameIndex, kBufferCount,
                      led.contentSeq, led.contentShown, led.writes(), led.clears,
                      led.draws, led.blits, led.everWritten, ownerName);
    }
    return true;
}

bool D3D12Backend::MayWriteBackBuffer(FrameOwner owner) {
    // T43.24 (2026-10-16, audit finding #1): the draw-path latch was
    // PERMANENT â€” m_drawPathTookBackBuffer was set on the first
    // PresentOwnedFrame(Draw) and never cleared, killing the guest's
    // blit-present path (PresentBgra) and ClearAndPresent FOREVER after
    // 2 draws. The "W6 blit proof" that justified it was for two synthetic
    // paths that are now deleted (the file's own comment at :5642-5645
    // says "keeps the W6 PresentBgra pipe proof alive" â€” that proof
    // served its purpose and is retired).
    //
    // FIX: reset the latch when a GUEST PRESENT command arrives â€” a new
    // guest frame is by definition newer than the last draw, and the
    // guest's PresentKick path (PRESENT cmd â†’ PresentBgra) is the game's
    // own mechanism for showing a frame. The draw path re-claims the
    // buffer on its next PresentOwnedFrame(Draw) as before.
    //
    // The pre-draw window (gradient + blit before the first draw) is
    // preserved: the latch is only set after a draw has ACTUALLY presented,
    // and it is cleared on the next guest PRESENT, not on a timer.
    if (owner == FrameOwner::Blit) {
        // A guest PRESENT is arriving: release the draw-path latch so
        // the blit can write. The draw path re-claims on its next present.
        if (m_drawPathTookBackBuffer) {
            m_drawPathTookBackBuffer = false;
            MCLA_LOG_INFO("FRAME-OWNER latch released for guest PRESENT "
                          "(draw had claimed the buffer; blit may write now)");
        }
    }
    // F-879 (2026-10-23, corrected by agent audit): RELEASE the draw-path latch
    // for Clear, same as Blit. The guest's Clear for the next frame is a
    // frame-boundary signal. The previous fix (owner==Clear && m_frameWriter==Draw)
    // FAILED because PresentOwnedFrame:6226 resets m_frameWriter to None after
    // every present, so by the time the next frame's Clear runs, m_frameWriter
    // is None, not Draw. The latch (m_drawPathTookBackBuffer) is what blocks
    // Clear -- release it exactly like the Blit path does above.
    if (owner == FrameOwner::Clear) {
        // Subagent 1 fix: do NOT release m_drawPathTookBackBuffer for Clear once draws
        // have started; allowing Clear to touch the back buffer destroys the drawn frame.
        // Disable protection explicitly with MCLA_PROTECT_DRAW_BUFFER=0.
        static int s_protectDrawBuffer = -1;
        if (s_protectDrawBuffer < 0) {
            const char* e = std::getenv("MCLA_PROTECT_DRAW_BUFFER");
            s_protectDrawBuffer = (e && e[0] == '0') ? 0 : 1;
        }
        if (s_protectDrawBuffer == 0 && m_drawPathTookBackBuffer) {
            m_drawPathTookBackBuffer = false;
            MCLA_LOG_INFO("FRAME-OWNER latch released for guest CLEAR "
                          "(draw had claimed the buffer; clear may write now)");
        }
    }
    const bool allowed = (owner == FrameOwner::Draw) || !m_drawPathTookBackBuffer;
    if (allowed) {
        if (m_frameWriter != FrameOwner::None && m_frameWriter != owner) {
            const uint64_t n = ++m_ownerConflicts;
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_ERROR("FRAME-OWNER CONFLICT #{}: {} wrote the back buffer "
                               "after {} in the same present window",
                               n, FrameOwnerName(owner), FrameOwnerName(m_frameWriter));
            }
        }
        m_frameWriter = owner;
        return true;
    }
    // A non-owner. Counted against the writer that lost, never into a shared
    // bucket, so "the blit never ran" and "the blit ran and was stopped here"
    // stay two different facts.
    uint64_t& counter = (owner == FrameOwner::Blit) ? m_blitRefused : m_clearRefused;
    const uint64_t n = ++counter;
    if (ShouldLogRefusal(n)) {
        MCLA_LOG_WARN("FRAME-OWNER REFUSED #{}: writer={} may not touch the back "
                      "buffer; owner=draw presented={} drawn={} blitRefused={} "
                      "clearRefused={} conflicts={}",
                      n, FrameOwnerName(owner), m_presentedFrames, m_drawOwnedFrames,
                      m_blitRefused, m_clearRefused, m_ownerConflicts);
    }
    return false;
}

// F-650: the single sanctioned Present(). Before this, three functions each
// closed, executed, presented, signalled and advanced m_frameIndex in their own
// copy of the same eleven lines, so "which writer produced the frame" was not
// recorded anywhere and could only be inferred from PRESENT-BLIT vs DRAW-CP
// counts. The FRAME-OWNER row below is that fact, once per presented frame.
void D3D12Backend::SetBatchDrawFrames(bool on) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    m_batchDrawFrames = on;
    MCLA_LOG_WARN("D3D12-DRAW-BATCH mode={} | on means the guest's draw burst "
                  "accumulates in ONE command list and the guest's own swap "
                  "(render_thread.cpp PRESENT, !IsHeartbeatPresent) closes and "
                  "presents it once. off reproduces one-present-per-draw.",
                  on ? 1 : 0);
}

// F-899: THE FRAME END THE HOST NEVER HAD.
//
// Called from render_thread.cpp's `case RenderCommand::PRESENT` when the guest
// really swaps. PresentOwnedFrame(FrameOwner::Draw) is what closes that list,
// resolves the guest render target into the scan-out surface and presents --
// i.e. the same tail one draw used to own, now owned by the frame instead of by
// whichever draw happened to be first.
void D3D12Backend::SrvRingPeakAndClamps(uint32_t& peak, uint32_t& clampPasses) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    peak = m_srvRingPassesFrame;
    clampPasses = m_srvRingClampPassesFrame;
}

bool D3D12Backend::FlushDrawFrame() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || !m_inFrame) return false;
    // TEMP F-900 FLUSH-PROBE (env-gated, remove after execution is settled):
    // stamp the guest RT RED immediately before the readback copy. If the
    // readback then shows RED, the open batch list executes and the copy
    // works, so all-black samples mean the guest draws really output black.
    // If it still shows BLACK/ZERO, the executed list is empty (or the copy
    // reads the wrong surface) and no draw-content theory can explain it.
    {
        static int s_probeRed = -1;
        if (s_probeRed < 0) {
            const char* e = std::getenv("MCLA_PROBE_FLUSH_RED");
            s_probeRed = (e && e[0] == '1') ? 1 : 0;
        }
        if (s_probeRed == 1 && m_drawsIntoGuestRt && m_guestRt) {
            const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
            m_commandList->ClearRenderTargetView(m_guestRtv, red, 0, nullptr);
            MCLA_LOG_WARN("FLUSH-PROBE-RED stamped guest RT red before readback");
        }
    }
    static uint64_t s_lastReadbackPresent = 0;
    const bool readbackDue =
        m_drawReadbackAttempts == 0 ||
        (m_presentedFrames >= s_lastReadbackPresent + BmpEvery());
    const bool rbRecorded =
        m_batchDrawFrameHasDraw && readbackDue && RecordDrawReadback();
    if (rbRecorded) {
        s_lastReadbackPresent = m_presentedFrames;
    }
    m_batchDrawFrameHasDraw = false;
    const bool presented = PresentOwnedFrame(FrameOwner::Draw);
    ReportResolveReadback();
    if (rbRecorded) {
        // Safe only after PresentOwnedFrame executed and signalled the list the
        // copy was recorded into (ReportDrawReadback waits on that fence).
        ReportDrawReadback();
    }
    return presented;
}

bool D3D12Backend::PresentOwnedFrame(FrameOwner owner) {
    if (m_isMinimized) {
        AbandonFrame("minimized");
        return false;
    }

    // F-XXX: THE RESOLVE, BEFORE THE LIST IS CLOSED. The guest's render
    // target is copied into the scan-out surface HERE, on the same command
    // list as the draws, so the copy sees the draws' output and the window
    // shows the guest's render target rather than whatever the back buffer
    // happened to hold. This is the console's EDRAM -> scanout copy (the
    // guest's own copy registers are read and named by the resolve's row).
    //
    // IT RUNS ONLY FOR THE DRAW PATH. The clear path owns the back buffer
    // itself (it clears it), and the blit path overwrites it wholesale, so
    // neither may have the guest's render target copied over it.
    bool f9Resolved = false;
    if (owner == FrameOwner::Draw && m_drawsIntoGuestRt && !m_hadBackBufferDraw) {
        // TEMP-PRESENT-ACT (pause black): 2 = log, 1 = also present the busiest RT when the selected one got < 8 draws.
        static const int s_pba = [] { const char* e = std::getenv("MCLA_TEMP_PRESENT_BY_ACTIVITY"); return e ? std::atoi(e) : 0; }();
        if (s_pba) {
            GuestRtEntry* best = nullptr; uint64_t bestKey = 0, bestD = 0;
            for (auto& kv : m_rtCache) {
                const uint64_t d = kv.second.draws - kv.second.drawsAtPresent;
                if (kv.second.res && d > bestD) { bestD = d; best = &kv.second; bestKey = kv.first; }
            }
            const uint64_t pd = m_presentRt ? m_presentRt->draws - m_presentRt->drawsAtPresent : 0;
            static uint32_t s_n = 0; const uint32_t n = ++s_n;
            if (n <= 40 || (n % 100) == 0)
                MCLA_LOG_ERROR("TEMP-PRESENT-ACT #{} presentRt={}x{} base={} ms={} fmt={} d={} | busiest base={} pitch={} ms={} fmt={} {}x{} d={}",
                               n, m_presentRt ? m_presentRt->w : 0, m_presentRt ? m_presentRt->h : 0, m_presentRt ? m_presentRt->base : 0,
                               m_presentRt ? m_presentRt->msaa : 0, m_presentRt ? static_cast<uint32_t>(m_presentRt->fmt) : 0, pd,
                               uint32_t((bestKey >> 32) & 0x7FF), uint32_t((bestKey >> 16) & 0x3FFF), uint32_t((bestKey >> 8) & 3),
                               best ? static_cast<uint32_t>(best->fmt) : 0, best ? best->w : 0, best ? best->h : 0, bestD);
            if (s_pba == 1 && best && pd < 8 && bestD >= 64) m_presentRt = best;
            for (auto& kv : m_rtCache) kv.second.drawsAtPresent = kv.second.draws;
        }
        if (m_presentRt && m_presentRt->res) {
            // RT-CACHE: present what the guest's last full-frame Resolve copied from.
            ActivateGuestRt(*m_presentRt);
            if (m_guestRtState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                D3D12_RESOURCE_BARRIER pb = {};
                pb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                pb.Transition.pResource = m_guestRt.Get();
                pb.Transition.StateBefore = m_guestRtState;
                pb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                pb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &pb);
                m_guestRtState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            m_guestRtDrawn = true;
        }
        f9Resolved = ResolveGuestRenderTarget();
    }
    // Close main list and any active MT recorder lists, then execute [main, mt...].
    std::vector<ID3D12CommandList*> execLists;
    execLists.reserve(1 + kMaxMtRecorders);
    if (f9Resolved && m_mainListOpen && g_f9State.load(std::memory_order_relaxed) == 2) F9CopyBackBuffer();  // back buffer is PRESENT here
    CloseActiveOcclusionQueriesBeforeClose();
    HRESULT hr = m_mainListOpen ? m_commandList->Close() : E_FAIL;
    m_mainListOpen = false;
    if (FAILED(hr)) {
        ++m_presentCloseFailures;
        const uint64_t cn = m_presentCloseFailures;
        if (ShouldLogRefusal(cn)) {
            MCLA_LOG_ERROR("PRESENT-CLOSE-FAILED #{} owner={} hr=0x{:08X} -- the "
                           "command list did NOT execute, no Signal was issued, and "
                           "the frame latch is released; any readback after this "
                           "row maps STALE bytes under a stale fenceTarget",
                           cn, static_cast<int>(owner), static_cast<uint32_t>(hr));
        }
        if (m_mtRecorderCount > 1) {
            for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
                if (m_mtCommandLists[r] && m_mtRecorderState[r].listOpen) {
                    (void)m_mtCommandLists[r]->Close();
                    m_mtRecorderState[r].listOpen = false;
                }
                m_mtRecorderState[r].ResetFrameAccumulators();
                m_mtRecorderState[r].ResetListBindState();
            }
        }
        m_activePassTargets = GuestPassTargets{};
        m_inFrame = false;
        m_frameWriter = FrameOwner::None;
        m_boundTargetsValid = false;
        InvalidateDrawCaches();
        if (m_drawsThisFrame > m_maxDrawsThisFrame)
            m_maxDrawsThisFrame = m_drawsThisFrame;
        m_drawsThisFrame = 0;
        return false;
    }
    execLists.push_back(m_commandList.Get());
    if (m_mtRecorderCount > 1) {
        for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
            DrawRecorder& st = m_mtRecorderState[r];
            if (!m_mtCommandLists[r] || !st.listOpen) continue;
            hr = m_mtCommandLists[r]->Close();
            st.listOpen = false;
            if (SUCCEEDED(hr)) {
                if (st.drawIssued) {
                    execLists.push_back(m_mtCommandLists[r].Get());
                }
            } else {
                MCLA_LOG_ERROR("MT-CLOSE-FAILED rec={} hr=0x{:08X}", r, static_cast<uint32_t>(hr));
            }
        }
        FoldMtRecorderStats();
    }
    m_activePassTargets = GuestPassTargets{};

    TEMP_RMCHK("pre-execute");
    m_commandQueue->ExecuteCommandLists(static_cast<UINT>(execLists.size()), execLists.data());
    TEMP_RMCHK("post-execute");

    const UINT syncInterval = m_graphicsSettings.vsync ? 1 : 0;
    hr = m_swapChain->Present(syncInterval, (syncInterval == 0 && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    m_lastPresentHr = hr;
    if (TempPerfStagesEnabled() || PerfFramesOnly()) PerfOnPresent();
    // F-XXX: A FAILED Present WAS INVISIBLE AT THE DEFAULT LOG LEVEL. The only
    // row that ever printed m_lastPresentHr is FRAME-OWNER, which is INFO, and
    // src/app.cpp:55 defaults the level to Error -- so a device removal at
    // Present would leave no line at all in a soak. (Measured on w10/w11:
    // zero [info] rows, so no FRAME-OWNER row exists in either log.) This row
    // carries GetDeviceRemovedReason(), which is the one value that separates
    // "this frame could not be shown" from "every later D3D12 call is
    // undefined". DXGI_STATUS_OCCLUDED (0x087A0001) SUCCEEDS and does not
    // reach here.
    if (FAILED(hr)) {
        static uint64_t s_presentFailedRows = 0;
        const uint64_t pn = ++s_presentFailedRows;
        if (ShouldLogRefusal(pn)) {
            const HRESULT removed =
                m_device ? m_device->GetDeviceRemovedReason() : E_FAIL;
            MCLA_LOG_ERROR(
                "PRESENT-FAILED #{} hr=0x{:08X} syncInterval={} "
                "deviceRemoved=0x{:08X} frameIndex={} presented={} drawn={} "
                "-- a failed Present is not just a dropped frame: if "
                "deviceRemoved is non-zero, every later command-list call is "
                "undefined",
                pn, static_cast<uint32_t>(hr), syncInterval,
                static_cast<uint32_t>(removed), m_frameIndex,
                m_presentedFrames, m_drawOwnedFrames);
        }
    }

    m_currentFenceValue++;
    // F-728: record the value on the SAME Signal, before m_frameIndex moves.
    // The readback's wait has to name the list it is waiting for, and this is
    // the only line that knows which value that is. Signalling is unchanged and
    // unconditional; only the result is now inspected.
    const HRESULT signalHr = m_commandQueue->Signal(m_fence.Get(), m_currentFenceValue);
    const uint64_t f9Fence = m_currentFenceValue;
    if (SUCCEEDED(signalHr)) {
        s_lastSignalledFenceValue = m_currentFenceValue;
    } else {
        // F-900: a FAILED Signal leaves s_lastSignalledFenceValue stale, so the
        // next readback's fenceTarget is an older value and the wait is skipped
        // (fenceCompleted >= staleTarget) -- Map then returns whatever the
        // previous completed copy left in the readback buffer, printed as a
        // fresh sample. Previously this branch was an empty if with no else and
        // no log, i.e. the same silent class as the Close() failure above.
        ++m_signalFailures;
        if (ShouldLogRefusal(m_signalFailures)) {
            MCLA_LOG_ERROR("PRESENT-SIGNAL-FAILED #{} value={} hr=0x{:08X} -- "
                           "Signal on m_fence failed; s_lastSignalledFenceValue "
                           "stays at {}, so the next readback waits on an OLDER "
                           "list (its target is stale and the wait will be "
                           "skipped)",
                           m_signalFailures, m_currentFenceValue,
                           static_cast<uint32_t>(signalHr),
                           s_lastSignalledFenceValue);
        }
    }
    m_fenceValues[m_frameIndex] = m_currentFenceValue;
    if (SUCCEEDED(signalHr)) {
        for (auto& r : m_retiredTargets)
            if (r.first == UINT64_MAX) r.first = m_currentFenceValue;
        for (auto& pr : m_retiredPlacedRanges)
            if (pr.fenceValue == UINT64_MAX) pr.fenceValue = m_currentFenceValue;
        for (auto& chunk : m_texUploadChunks) {
            if (chunk.inCurrentFrame) {
                chunk.fenceValue = m_currentFenceValue;
                chunk.inCurrentFrame = false;
            }
        }
        for (auto& oq : m_pendingOccQueries) {
            if (oq.fenceValue == UINT64_MAX) oq.fenceValue = m_currentFenceValue;
        }
        for (auto& [va, ac] : m_autoClosedOccSlots) {
            if (ac.fenceValue == UINT64_MAX) ac.fenceValue = m_currentFenceValue;
        }
        for (uint32_t h = 0; h < kSamplerHeapCount; ++h) {
            for (auto& sb : m_samplerHeaps[h].blocks) {
                if (sb.fenceValue == UINT64_MAX) sb.fenceValue = m_currentFenceValue;
            }
        }
        m_samplerBlocks = m_samplerHeaps[0].blocks;
    }
    PruneRetiredTargets();
    // F-761: counted against the index that was just presented, BEFORE the
    // advance below, so "how many times was this buffer shown" and "how many
    // presents have there been" are two numbers a reader can divide.
    if (m_frameIndex < kBufferCount) {
        ++m_ledger[m_frameIndex].presents;
        // F-763: and the CONTENT VERSION this flip showed. This is the only
        // line that can say "the window is now displaying this resource's
        // contentSeq", and it is placed before the m_frameIndex advance below
        // for the same reason the presents++ above is: after the advance it
        // names the wrong buffer. Every flip goes through here, so the
        // bookkeeping cannot be forgotten on one path and remembered on three.
        // The draw path's own flip lands here too, which is what makes
        // "the guest drew again" a content change rather than a repeat.
        m_ledger[m_frameIndex].contentShown = m_ledger[m_frameIndex].contentSeq;
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_inFrame = false;
    m_boundTargetsValid = false;  // list closed; see the cache's declaration
    if (SUCCEEDED(signalHr) && g_f9State.load(std::memory_order_relaxed) == 2) F9Finalize(f9Fence);
    InvalidateDrawCaches();
    BumpRtPassEpoch();
    if (m_drawsThisFrame > m_maxDrawsThisFrame)
        m_maxDrawsThisFrame = m_drawsThisFrame;
    m_drawsThisFrame = 0;
    m_uploadOffset = 0; ++m_uploadEpoch;  // reset arena for the next frame (batch mode reuses the same region)

    if (owner == FrameOwner::Draw) {
        m_drawPathTookBackBuffer = true;
        ++m_drawOwnedFrames;
    }
    m_frameWriter = FrameOwner::None;

    // The one line a soak reads to answer "who painted this frame". Bounded by
    // the house pattern: a per-present marker is exactly the kind that becomes
    // 66% of a log if it is not.
    {   // MEM-CACHES: once per second (time-gated, uncapped by count) -- AUDIT_MEMORY section 6
        static const bool s_memDiag = [] { const char* e = std::getenv("MCLA_LOG_LEVEL"); return e && e[0] != '6'; }();
        static int64_t s_memLastNs = 0;
        const int64_t memNow = s_memDiag ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
        if (s_memDiag && memNow - s_memLastNs >= 1000000000LL) {
            s_memLastNs = memNow;
            PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof pmc;
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
            uint64_t vramMb = 0;
            Microsoft::WRL::ComPtr<IDXGIAdapter3> a3;
            if (m_factory && m_device && SUCCEEDED(m_factory->EnumAdapterByLuid(m_device->GetAdapterLuid(), IID_PPV_ARGS(&a3)))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
                if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi))) vramMb = vmi.CurrentUsage >> 20;
            }
            const auto ts = m_textureCache.Stats();
            static size_t s_upPeakAll = 0; s_upPeakAll = std::max(s_upPeakAll, s_uploadPeakBytes);
            static uint64_t s_texPeakAll = 0; s_texPeakAll = std::max<uint64_t>(s_texPeakAll, ts.residentBytes);
            MCLA_LOG_WARN("MEM-CACHES rss_mb={} commit_mb={} vram_mb={} rt={} ds={} resolved={} resStates={} texPool={} texCacheEntries={} texCacheMB={} texCachePeakMB={} psoCache={} untileMB={} vsCache={} psCache={} uploadPeakMB={} uploadPeakAllMB={} uploadRegionMB={} presented={}",
                          pmc.WorkingSetSize >> 20, pmc.PrivateUsage >> 20, vramMb, m_rtCache.size(), m_dsCache.size(), m_resolved.size(),
                          m_resourceStates.size(), m_texturePoolSlots.size(), ts.entries, ts.residentBytes >> 20, s_texPeakAll >> 20,
                          m_pipelineCache.Size(), mcla::native::g_memUntileBytes.load() >> 20, mcla::native::g_memVsCache.load(),
                          mcla::native::g_memPsCache.load(), s_uploadPeakBytes >> 20, s_upPeakAll >> 20, m_uploadRegionSize >> 20, m_presentedFrames);
            s_uploadPeakBytes = 0;
        }
    }
    const uint64_t p = ++m_presentedFrames;
    if (ShouldLogRefusal(p)) {
        // F-872 (F-864's S8): the Present HRESULT was bookkept in
        // m_lastPresentHr but never printed, so "the frame reached the
        // window" was unmeasurable from the log. 0x00000000 (or 0x087A0001
        // OCCLUSION_MODE_1 for a minimized window) on every row is the
        // positive control that the presents themselves are healthy.
        MCLA_LOG_INFO("FRAME-OWNER #{} owner={} presented={} drawn={} blitRefused={} "
                      "clearRefused={} conflicts={} presentHr={:08X}",
                      p, FrameOwnerName(owner), p, m_drawOwnedFrames, m_blitRefused,
                      m_clearRefused, m_ownerConflicts,
                      static_cast<uint32_t>(hr));
    }
    return SUCCEEDED(hr);
}

bool D3D12Backend::PresentBgra(uint32_t width, uint32_t height, const uint8_t* pixels) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || m_isMinimized || !m_device || !pixels || width == 0 || height == 0) return false;

    // F-650: this function is the SECOND writer to the back buffer, and the
    // heavier of the two -- it ends in CopyResource(back, m_fbTexture), which
    // overwrites every pixel the draw path produced. The refusal is taken HERE,
    // before the staging Map and before the scale loop, because a blit that
    // copies and then declines has already destroyed the frame it declined to
    // own. Returning false is the whole contract: the caller (render_thread.cpp
    // :971) turns a false into PRESENT-BLIT fail and then falls through to its
    // clear-only fallback, which the arbiter will also refuse, so a frame the
    // draw path owns is not repainted by anything on the way out.
    if (!MayWriteBackBuffer(FrameOwner::Blit)) return false;

    const uint32_t dstW = m_width;
    const uint32_t dstH = m_height;
    if (dstW == 0 || dstH == 0) return false;

    const size_t dstBytes = static_cast<size_t>(dstW) * dstH * 4u;
    if (m_fbHost.size() < dstBytes) {
        m_fbHost.resize(dstBytes);
    }

    if (width == dstW && height == dstH) {
        std::memcpy(m_fbHost.data(), pixels, dstBytes);
    } else {
        // Nearest-neighbor scale into the backbuffer-sized host buffer so a
        // downsampled guest read still covers the full viewport.
        for (uint32_t y = 0; y < dstH; ++y) {
            const uint32_t sy = static_cast<uint32_t>(
                (static_cast<uint64_t>(y) * height) / dstH);
            const uint8_t* srcRow = pixels + static_cast<size_t>(sy) * width * 4u;
            uint8_t* dstRow = m_fbHost.data() + static_cast<size_t>(y) * dstW * 4u;
            for (uint32_t x = 0; x < dstW; ++x) {
                const uint32_t sx = static_cast<uint32_t>(
                    (static_cast<uint64_t>(x) * width) / dstW);
                std::memcpy(dstRow + static_cast<size_t>(x) * 4u,
                            srcRow + static_cast<size_t>(sx) * 4u, 4u);
            }
        }
    }

    if (m_fence->GetCompletedValue() < m_currentFenceValue) {
        if (FAILED(m_fence->SetEventOnCompletion(m_currentFenceValue, m_fenceEvent))) return false;
        if (WaitForSingleObject(m_fenceEvent, INFINITE) != WAIT_OBJECT_0) return false;
    }
    if (!EnsureFbTexture(dstW, dstH)) return false;

    // Fill the upload staging with the properly pitched rows.
    {
        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        HRESULT hr = m_fbUpload->Map(0, &readRange, &mapped);
        if (FAILED(hr) || !mapped) {
            MCLA_LOG_WARN("D3D12Backend: PresentBgra staging Map failed hr=0x{:08X}",
                          static_cast<uint32_t>(hr));
            return false;
        }
        const UINT64 rowPitch = m_fbFootprint.Footprint.RowPitch;
        auto* dst = static_cast<uint8_t*>(mapped);
        for (uint32_t y = 0; y < dstH; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * rowPitch,
                        m_fbHost.data() + static_cast<size_t>(y) * dstW * 4u,
                        static_cast<size_t>(dstW) * 4u);
        }
        m_fbUpload->Unmap(0, nullptr);
    }

    if (!m_inFrame) {
        if (!BeginFrame()) return false;
    }

    ID3D12Resource* back = m_renderTargets[m_frameIndex].Get();
    if (!back) {
        // 2026-10-02: this used to clear `m_inFrame` inline. It already
        // released the latch, so it was never a wedge -- but it released HALF
        // the state AbandonFrame clears (not m_frameWriter), and it was the one
        // release in this file that produced no count, so "how many frames were
        // dropped" had two different answers depending on which door dropped
        // them. Routed through the same helper; the refusal is unchanged.
        AbandonFrame("blit:no-back-buffer");
        return false;
    }

    // staging -> DEFAULT texture
    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_fbTexture.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = m_fbUpload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = m_fbFootprint;

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    UINT barrierCount = 0;
    if (m_fbTexState != D3D12_RESOURCE_STATE_COPY_DEST) {
        auto& b = barriers[barrierCount++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_fbTexture.Get();
        b.Transition.StateBefore = m_fbTexState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    {
        auto& b = barriers[barrierCount++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = back;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    if (barrierCount) {
        (++g_dcBarriers, m_commandList)->ResourceBarrier(barrierCount, barriers);
    }
    m_fbTexState = D3D12_RESOURCE_STATE_COPY_DEST;

    m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // texture: COPY_DEST -> COPY_SOURCE; backbuffer: COPY_DEST -> PRESENT
    barrierCount = 0;
    {
        auto& b = barriers[barrierCount++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_fbTexture.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_fbTexState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    (++g_dcBarriers, m_commandList)->ResourceBarrier(barrierCount, barriers);
    m_commandList->CopyResource(back, m_fbTexture.Get());
    // F-761: the blit is a full-surface write to this buffer, counted as one.
    NoteBufferWrite("blit");
    barrierCount = 0;
    {
        auto& b = barriers[barrierCount++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = back;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    (++g_dcBarriers, m_commandList)->ResourceBarrier(barrierCount, barriers);

    // F-650: the one sanctioned Present(). The Close/Execute/Present/Signal/
    // frameIndex block this used to own verbatim is now shared, so the blit
    // cannot present a frame the draw path also presented.
    m_stats.uploadsBytes += dstBytes;
    return PresentOwnedFrame(FrameOwner::Blit);
}

bool D3D12Backend::ClearAndPresent(float r, float g, float b, float a) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || m_isMinimized) return false;

    // F-650: the third writer. ClearRenderTargetView is not a neutral no-op --
    // it is a full-surface overwrite, so a clear-only present arriving after a
    // drawn frame erases exactly the pixels the draw path just produced. This is
    // the same last-writer-wins defect as the blit, one call earlier in the
    // fallback chain, so it goes through the same arbiter and is refused the
    // same way. Before any draw has presented it is still allowed: it is what
    // keeps the window valid on the heartbeat presents that carry no geometry.
    // F-760: THE CLEAR IS REFUSED, BUT THE FRAME IS NOT. This line used to be
  // `if (!MayWriteBackBuffer(FrameOwner::Clear)) return false;` and that
  // single `return false` is what froze the display.
  //
  // WHAT THE LATCH WAS FOR, AND WHY IT IS STILL ARMED. m_drawPathTookBackBuffer
  // is set at :5157 on the first draw-owned present and is NEVER cleared. Its
  // own comment at :5092-5096 said what it protects: "the W6 PresentBgra
  // pipe proof (render_thread.cpp:152) and the pre-guest-content gradient
  // (render_thread.cpp:1039) ... before any guest geometry arrives". BOTH
  // of those are SYNTHETIC and both are now DELETED (F-758). The latch's
  // stated justification no longer exists, but the flag survives and still
  // disarms this path.
  //
  // MEASURED IN build/w715.log, 150 s soak. presented= is FROZEN at 360 for
  // the whole run, and it reconciles with no slack:
  //   1 startup clear + 355 heartbeat PRESENT + 2 blits + 2 draw presents
  //   = 360
  // Present command #358 is the first refusal, 472 ms after the second draw:
  //   FRAME-OWNER REFUSED #1: writer=clear may not touch the back buffer;
  //   owner=draw presented=360 drawn=2 blitRefused=0 clearRefused=1
  // and FRAME-OWNER REFUSED then runs to #4000. So the guest's triangle WAS
  // on screen for 19 ms (54.795 -> 54.814) and every frame after that was
  // never presented at all. The window the user was looking at was 136 s
  // stale, showing one all-black frame.
  //
  // WHY NOT SIMPLY ALLOW THE CLEAR. F-650's protection is sound and is KEPT:
  // ClearRenderTargetView is a full-surface overwrite, so clearing here would
  // erase the guest's pixels. The bug was never the refusal -- it was
  // refusing the CLEAR and then abandoning the FRAME, which throws away the
  // very content the refusal was protecting.
  //
  // SO: skip the clear, present the back buffer unmodified. Nothing is
  // synthesised - whatever is in the buffer is what is shown, and
  // FrameOwner::None says on the FRAME-OWNER row that no writer touched it.
  //
  // F-761: AND ONLY IF THAT BUFFER HAS BEEN WRITTEN. "Whatever is in the buffer"
  // is only honest for a buffer something put something in. The chain rotates
  // (PresentOwnedFrame advances m_frameIndex to
  // m_swapChain->GetCurrentBackBufferIndex() on every present, and kBufferCount
  // is 3), so this branch will eventually be asked to present an index that no
  // clear, no blit and no draw ever touched, and presenting that shows
  // uninitialised resource memory. Measured, not assumed, in build/w716.log:
  // FRAME-OWNER #200 at 17:08:00.354 reads owner=clear drawn=0 clearRefused=0
  // blitRefused=0, and clearRefused only starts incrementing once the draw path
  // owns the back buffer, i.e. after the first DRAW present. So every one of
  // presents #1 through #200 was an owner=clear present, and each of those
  // recorded a ClearRenderTargetView on its own m_frameIndex before presenting
  // that same index. With kBufferCount=3 that is >= 66 writes per index before
  // the guest's first draw at 17:08:05.586, so all three indices were written.
  // That is a fact about THAT run and not a guarantee -- a run that reached its
  // first present without a clear would rotate onto an unwritten buffer on
  // present #1. So the ledger decides, per buffer, and the refusal names the
  // buffer rather than substituting a colour for it.
  if (!MayWriteBackBuffer(FrameOwner::Clear)) {
    if (!m_inFrame) {
      if (!BeginFrame()) return false;
    }
    if (RefuseUnwrittenPresent("none")) {
      // Nothing was recorded into this frame, so it is closed unreferenced
      // rather than executed. The window keeps whatever it was already showing,
      // which is the honest state: this backend has no guest frame to put there.
      AbandonFrame("clear-refused-unwritten-back-buffer");
      return false;
    }
    // F-763: AND NOW THE OTHER HALF OF "NOTHING TO SHOW". The buffer above was
    // written by SOMETHING, at some point in the past -- which is a different
    // question from whether it holds anything the window has not already been
    // showing. This is the narrowest possible place for the hold: it is inside
    // the branch whose ONLY act is to flip a buffer that no writer touched this
    // beat, so the draw path (PresentOwnedFrame(FrameOwner::Draw), reached from
    // DrawDynamicMeshWithPipeline), the blit (FrameOwner::Blit) and the
    // allowed clear (FrameOwner::Clear) are all upstream of here and none of
    // them can be reached by this gate.
    //
    // ORDER MATTERS AND IS THE POINT. This test sits BEFORE
    // RecordLateFrameReadback, so a beat that changes nothing spends no
    // GPU->CPU fence wait to re-measure bytes it already measured. That also
    // means LATE-FRAME-PIXEL now samples only beats that CHANGED something --
    // which is the honest population for a row whose stated subject is "which
    // buffer the window was showing", since an unchanged window has one answer
    // and the ledger below carries which buffer it was.
    if (RefuseUnchangedPresent("none")) {
      // Same shape as the refusal above and for the same measured reason: this
      // branch has recorded nothing into the list, so AbandonFrame closes it
      // unreferenced and releases m_inFrame. AbandonFrame here is not a
      // workaround and not a second gate -- it is the release, and the gate is
      // RefuseUnchangedPresent.
      AbandonFrame("clear-refused-no-new-content");
      return false;
    }
    // F-761: the sample of the frame the window is ACTUALLY showing. Recorded
    // before the present, reported after it, so the fence discipline below is
    // the one the draw path uses. It is the only pixel census in this file that
    // can fire on a frame with no draw on it.
    const bool rbRecorded = RecordLateFrameReadback();
    // No ClearRenderTargetView is recorded: the back buffer keeps the
    // guest's pixels, and the frame is presented as it stands. The
    // transition is already PRESENT because a preceding
    // PresentOwnedFrame left it there, and BeginFrame() re-armed the
    // frame if one was not open. If the readback above recorded a copy it
    // also recorded the PRESENT transition the copy needs, so the state
    // named here is the state that is there either way (the F-725 lesson).
    const bool presented = PresentOwnedFrame(FrameOwner::None);
    if (rbRecorded) {
      ReportLateFrameReadback();
    }
    return presented;
  }

    if (!m_inFrame) {
        if (!BeginFrame()) return false;
    }

    // Transition render target buffer from PRESENT to RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvHandle.ptr += static_cast<size_t>(m_frameIndex) * m_rtvDescriptorSize;

    const float clearColor[4] = { r, g, b, a };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    // F-761: this clear is a write to the buffer that is about to be presented.
    NoteBufferWrite("clear");

    // Transition render target back to PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    (++g_dcBarriers, m_commandList)->ResourceBarrier(1, &barrier);

    // F-650: one sanctioned Present(). Note this copy of the block was also the
    // only one of the three that never set m_lastPresentHr, so the last present
    // status the backend reported was whichever of the other two ran last --
    // another fact the triplication was hiding.
    return PresentOwnedFrame(FrameOwner::Clear);
}

// F-610: PresentDirect is DELETED, not fixed. It had zero callers
// (grep over src/: the only two hits were this definition and the
// d3d12_backend.h:53 declaration), and it is unsound as written: it
// signalled a fence and advanced m_frameIndex WITHOUT executing a command
// list, so the fence it waited on implied GPU work that never happened and
// the next frame's back buffer index was chosen by a present with no
// submission behind it. Every live present path in this backend
// (DrawDynamicMeshWithPipeline :1487-1504, ClearAndPresent :1912-1932,
// PresentBgra) closes + executes + presents atomically under m_mutex;
// there is no caller that has already drawn to the back buffer and wants
// only the flip, because those callers ARE the drawing. Wiring it would
// require a real EndFrame split (see :1953) to know which draws to flush,
// which is a backend restructure, not a fix to a dead function. Its
// removal trigger was zero call sites; it is gone, and the honest
// equivalent of "present what is already there" is the existing
// ClearAndPresent, which submits its own clear rather than an empty list.

void D3D12Backend::EndFrame() {
    if (m_commandList && m_mainListOpen) {
        CloseActiveOcclusionQueriesBeforeClose();
        (void)m_commandList->Close();
        m_mainListOpen = false;
    }
    if (m_mtRecorderCount > 1) {
        for (uint32_t r = 0; r < m_mtRecorderCount; ++r) {
            if (m_mtCommandLists[r] && m_mtRecorderState[r].listOpen) {
                (void)m_mtCommandLists[r]->Close();
                m_mtRecorderState[r].listOpen = false;
            }
            m_mtRecorderState[r].ResetFrameAccumulators();
            m_mtRecorderState[r].ResetListBindState();
        }
    }
    m_activePassTargets = GuestPassTargets{};
    m_inFrame = false;
    BumpRtPassEpoch();
    // F-639: the texture cache's notion of "in flight" is a frame boundary, so
    // this is where a resident texture stops being pinned. Nothing is FREED here
    // -- eviction is the TextureCache's job and it skips in-flight entries
    // (resource_cache.h:128-130) -- this only tells it which serial the GPU has
    // passed, so an entry bound this frame is not the next eviction candidate
    // while the copy is still in the queue.
    ++m_frameSerial;
    m_textureCache.NotifySubmitted(m_frameSerial);
}

void D3D12Backend::Shutdown() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return;

    WaitForGpu();

    // Stop pipeline cache worker thread
    m_pipelineCache.StopWorker();

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    if (m_copyFenceEvent) {
        CloseHandle(m_copyFenceEvent);
        m_copyFenceEvent = nullptr;
    }

    m_fence.Reset();
    m_copyFence.Reset();
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        m_renderTargets[i].Reset();
        m_commandAllocators[i].Reset();
    }
    m_rtvHeap.Reset();
    m_commandList.Reset();
    m_mainListOpen = false;
    m_activePassTargets = GuestPassTargets{};
    m_commandQueue.Reset();
    m_copyCommandList.Reset();
    m_copyAllocator.Reset();
    m_copyQueue.Reset();
    m_swapChain.Reset();

    // Phase 3 draw-path resources: clear the persistent upload mapping and
    // release the cached resources so a Shutdown() -> Initialize() recovery
    // path (device-lost) cannot touch a dangling m_uploadMap.
    if (m_uploadHeap) {
        m_uploadHeap->Unmap(0, nullptr);
        m_uploadHeap.Reset();
    }
    m_uploadMap = nullptr;
    m_uploadOffset = 0; ++m_uploadEpoch;
    m_staticIndexBuffer.Reset();
    m_decodedTexture.Reset();
    m_decodedTextureHeapIndex = UINT32_MAX;
    m_decodedTextureHeapOffset = 0;
    m_decodedTextureHeapSize = 0;
    m_decodedTextureUpload.Reset();
    m_fbTexture.Reset();
    m_fbUpload.Reset();
    m_fbFootprint = {};
    m_fbTexW = 0;
    m_fbTexH = 0;
    m_fbTexState = D3D12_RESOURCE_STATE_COPY_DEST;
    m_fbHost.clear();
    m_fbHost.shrink_to_fit();
    m_srvHeap.Reset();
    m_srvStagingHeap.Reset();
    m_srvStagingFree.clear();
    m_srvStagingNext = 1;
    ++m_srvTableEpoch;
    m_mainSrvTableCache.Clear();
    for (auto& c : m_mtSrvTableCaches) c.Clear();
    s_frameSrvRingCursor.store(0, std::memory_order_relaxed);
    m_srvRingOffset = 0;
    m_currentSrvTableGpu = {};
    m_decodedTextureSrvGpu = {};
    m_testPipeline.Reset();
    m_rootSignature.Reset();
    m_resourceCache.Clear();

    // F-639: resident textures and the samplers interned from them go with the
    // device. The pool is cleared rather than left holding ComPtrs into a
    // released device, which is what a Shutdown()->Initialize() recovery path
    // would otherwise trip over.
    for (ResidentTexture& rt : m_texturePoolSlots) {
        rt.texture.Reset();
        rt.upload.Reset();
        rt.stagingSrvs.clear();
        rt.heapIndex = UINT32_MAX;
        rt.heapOffset = 0;
        rt.heapSize = 0;
    }
    m_texturePoolSlots.clear();
    m_texturePoolByHandle.clear();
    m_texturePoolFree.clear();
    m_textureCache.Clear();
    m_retiredTargets.clear();
    m_retiredPlacedRanges.clear();
    m_textureHeaps.clear();
    for (auto& chunk : m_texUploadChunks) {
        if (chunk.buffer && chunk.mapped) {
            chunk.buffer->Unmap(0, nullptr);
            chunk.mapped = nullptr;
        }
    }
    m_texUploadChunks.clear();
    BumpRtPassEpoch();
    m_guestSamplerIndex.clear();
    m_guestSamplerBlockIndex.clear();
    for (auto& sh : m_samplerHeaps) {
        sh.heap.Reset();
        sh.blockIndex.clear();
        sh.blocks.fill({});
        sh.heapOffset = 0;
    }
    m_activeSamplerHeapIdx = 0;
    m_samplerHeapSwitched = false;
    m_samplerBlocks.fill({});
    m_samplerHeap.Reset();
    m_guestRtvHeap.Reset();
    for (auto& h : m_guestNullRtvs) h = {};
    for (auto& f : m_guestNullRtvFmts) f = DXGI_FORMAT_UNKNOWN;
    m_samplerTableGpu = {};
    m_stage2FallbackSrvs.clear();
    m_mainFallbackSrvs.clear();
    ClearFrameTextures();
    m_resolved.clear();
    m_resMips.clear();
    m_resMipOwner.clear();
    m_resolvedAlt.clear();
    m_resolvedLastColour.clear();
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        m_pixprobe3dRb[i].Reset();
        m_pixprobe3dRbPending[i] = false;
    }
    for (uint32_t r = 0; r < kMaxMtRecorders; ++r) {
        m_mtCommandLists[r].Reset();
        for (uint32_t f = 0; f < kBufferCount; ++f) {
            m_mtAllocators[f][r].Reset();
        }
        m_mtSrvTableGpu[r] = {};
        m_mtRecorderState[r] = DrawRecorder{};
    }
    m_resourceStates.clear();
    m_persistentGeoCache.clear();
    m_persistentGeoBytes = 0;
    m_pendingOccQueries.clear();
    m_activeOccSlots.clear();
    m_autoClosedOccSlots.clear();
    if (m_occReadbackBuf && m_occReadbackMap) {
        m_occReadbackBuf->Unmap(0, nullptr);
        m_occReadbackMap = nullptr;
    }
    m_occReadbackBuf.Reset();
    m_occQueryHeap.Reset();
    m_resolveRbBuf.Reset();
    m_resolveRbBufSize = 0;
    m_resolve1xTex.Reset();
    m_resolve1xRtvHeap.Reset();
    m_resolve1xSrvHeap.Reset();
    m_resolveConvPsoRgba8.Reset();
    m_resolveConvPsoRg32f.Reset();
    m_importScratch.clear();
    m_importScratch.shrink_to_fit();
    m_importMipScratch.clear();
    m_importMipScratch.shrink_to_fit();
    m_importMipLevelTmp.clear();
    m_importMipLevelTmp.shrink_to_fit();
    m_samplerHeapOffset = 0;
    m_trackedStateDisagreements = 0;

    m_device.Reset();
    m_factory.Reset();

    m_initialized = false;
    MCLA_LOG_INFO("D3D12Backend: Shutdown complete");
}

// Phase 4: Sampler state management (static samplers for root signature)
uint32_t D3D12Backend::GetOrCreateStaticSampler(const D3D12_SAMPLER_DESC& desc) {
    SamplerCacheKey key{ desc };
    auto it = m_samplerCache.find(key);
    if (it != m_samplerCache.end()) {
        for (uint32_t i = 0; i < m_staticSamplerArray.size(); ++i) {
            if (memcmp(&m_staticSamplerArray[i].Filter, &desc.Filter, sizeof(D3D12_SAMPLER_DESC)) == 0) return i;
        }
    }
    D3D12_STATIC_SAMPLER_DESC staticDesc = {};
    staticDesc.Filter = desc.Filter;
    staticDesc.AddressU = desc.AddressU;
    staticDesc.AddressV = desc.AddressV;
    staticDesc.AddressW = desc.AddressW;
    staticDesc.MipLODBias = desc.MipLODBias;
    staticDesc.MaxAnisotropy = desc.MaxAnisotropy;
    staticDesc.ComparisonFunc = desc.ComparisonFunc;
    staticDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticDesc.MinLOD = desc.MinLOD;
    staticDesc.MaxLOD = desc.MaxLOD;
    staticDesc.ShaderRegister = 0;
    staticDesc.RegisterSpace = 0;
    staticDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    uint32_t index = static_cast<uint32_t>(m_staticSamplerArray.size());
    m_staticSamplerArray.push_back(staticDesc);
    m_samplerCache.emplace(SamplerCacheKey{ desc }, desc);
    return index;
}

bool D3D12Backend::CreateRenderTargetView(ID3D12Resource* texture, DXGI_FORMAT format,
                                          D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle) {
    if (!texture || !m_device) return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = format;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    rtvDesc.Texture2D.PlaneSlice = 0;
    m_device->CreateRenderTargetView(texture, &rtvDesc, rtvHandle);
    return true;
}

bool D3D12Backend::CreateDepthStencilView(ID3D12Resource* texture, DXGI_FORMAT format,
                                          D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle) {
    if (!texture || !m_device) return false;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = format;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    dsvDesc.Texture2D.MipSlice = 0;
    m_device->CreateDepthStencilView(texture, &dsvDesc, dsvHandle);
    return true;
}

bool D3D12Backend::ResolveRenderTarget(ID3D12Resource* srcRt, ID3D12Resource* dstTexture,
                                       DXGI_FORMAT format) {
    if (!srcRt || !dstTexture || !m_commandList) {
        const uint64_t n = ++g_resolveRefusals;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_ERROR("D3D12Backend: resolve refused #{}: {} -- a resolve with an absent "
                           "target means the colour ended up nowhere and the next pass samples "
                           "uninitialised memory",
                           n, srcRt == nullptr ? "no source" : "no destination");
        }
        return false;
    }
    // F-640: the prior states are the TRACKED ones, not the asserted
    // RENDER_TARGET/COMMON the old code hardcoded. That assertion held only for
    // the first resolve of a given resource; on the second pass D3D12 was told
    // the source was in RENDER_TARGET when it was in RESOLVE_SOURCE from the
    // previous call's tail transition, which is a barrier-state mismatch and a
    // debug-layer error. TransitionResource with the right subresource is the
    // single place that knows the tracked state, so the resolve goes through it.
    D3D12_RESOURCE_BARRIER in[2] = {};
    in[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    in[0].Transition.pResource = srcRt;
    in[0].Transition.StateBefore = TrackedState(srcRt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    in[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    in[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    in[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    in[1].Transition.pResource = dstTexture;
    in[1].Transition.StateBefore =
        TrackedState(dstTexture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    in[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    in[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    (++g_dcBarriers, m_commandList)->ResourceBarrier(2, in);

    m_commandList->ResolveSubresource(dstTexture, 0, srcRt, 0, format);

    // The tail states are recorded on the tracker so the NEXT resolve of the same
    // resources starts from the truth. Note the source goes back to
    // RENDER_TARGET because the source of an EDRAM resolve is a colour RT that
    // the next pass keeps rendering into; the destination ends as a shader
    // resource, which is what the following sampling pass needs.
    in[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    in[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    in[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    in[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    (++g_dcBarriers, m_commandList)->ResourceBarrier(2, in);
    SetTrackedState(srcRt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetTrackedState(dstTexture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

// F-640: the EDRAM-resolve entry point. Everything BuildResolveDesc decides is
// honoured or refused -- see the header for the per-field list. The point of
// routing through BuildResolveDesc (rather than trusting a caller-supplied DXGI
// format) is that the resolve's legality is a property of the GUEST format: the
// format table's isResolvable column, cross-checked against Xenia's ColorFormat
// set at init, is the authority, and a host format alone cannot answer it.
bool D3D12Backend::ResolveGuestSurface(uint32_t xenosFormat, uint32_t width, uint32_t height,
                                      uint32_t sampleSelect, bool endianSwap, bool swapRedBlue,
                                      bool clearSource, uint32_t extentX, uint32_t extentY,
                                      uint32_t copyDestPitch, uint32_t copyDestHeight,
                                      uint32_t guestBaseAddress, ID3D12Resource* srcRt,
                                      ID3D12Resource* dstTexture) {
    const native::TextureResolveDesc rd = native::BuildResolveDesc(
        xenosFormat, width, height, sampleSelect, endianSwap, swapRedBlue, clearSource,
        extentX, extentY, copyDestPitch, copyDestHeight, guestBaseAddress);

    auto refuse = [&](const char* why) {
        const uint64_t n = ++g_resolveRefusals;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_ERROR("D3D12Backend: EDRAM resolve refused #{}: {} (xenosFmt={} {} "
                           "{}x{} sampleSelect={} endianSwap={} swapRB={} clearSrc={} "
                           "extent={}x{} copyDest={}x{})",
                           n, why, rd.xenosFormat, rd.formatName, rd.width, rd.height,
                           rd.sampleSelect, rd.endianSwap ? 1 : 0, rd.swapRedBlue ? 1 : 0,
                           rd.clearSource ? 1 : 0, rd.extent[0], rd.extent[1],
                           rd.copyDestPitch, rd.copyDestHeight);
        }
        return false;
    };

    if (!rd.valid) {
        return refuse(rd.refusal ? rd.refusal : "BuildResolveDesc-refused");
    }
    if (rd.isDepthResolve) {
        return refuse("depth-resolve-needs-own-path");
    }
    if (rd.hostDxgiFormat == 0) {
        return refuse("no-host-resolve-format");
    }
    if (rd.endianSwap || rd.swapRedBlue) {
        // ResolveSubresource performs no channel swizzle and no byte swap, so
        // honouring this would mean shipping an un-swizzled image.
        return refuse("resolve-needs-swizzle-D3D12-ResolveSubresource-has-none");
    }
    if (rd.clearSource) {
        // D3D12 has no copy-and-clear on a resource that is simultaneously the
        // resolve source; the caller would have to issue a separate clear, which
        // is a different command stream, not a flag on this one.
        return refuse("resolve-clearSource-unsupported");
    }
    if (!srcRt || !dstTexture) {
        return refuse(srcRt == nullptr ? "absent-resolve-source" : "absent-resolve-target");
    }
    // The destination's real format must be the one the resolve plan names, or
    // ResolveSubresource would write bytes the destination cannot represent.
    {
        const DXGI_FORMAT dstFormat = dstTexture->GetDesc().Format;
        if (dstFormat != static_cast<DXGI_FORMAT>(rd.hostDxgiFormat)) {
            const uint64_t n = ++g_resolveRefusals;
            if (ShouldLogRefusal(n)) {
                MCLA_LOG_ERROR("D3D12Backend: EDRAM resolve refused #{}: destination format {} "
                               "!= plan hostDxgiFormat {} (xenosFmt={} {})",
                               n, static_cast<uint32_t>(dstFormat), rd.hostDxgiFormat,
                               rd.xenosFormat, rd.formatName);
            }
            return false;
        }
        const D3D12_RESOURCE_DESC sd = srcRt->GetDesc();
        if (sd.SampleDesc.Count <= 1) {
            return refuse("resolve-source-is-not-MSAA");
        }
    }

    // MSAA -> single-sample is exactly what ResolveSubresource does, so the guest
    // sampleSelect (Sample0/Avg01/AvgAll/...) is NOT applied here. D3D12 always
    // box-resolves. For AvgAll and the single-sample selects the result is the
    // average, which differs from the guest's answer for a 2x/4x MSAA surface --
    // that is a real, counted, named divergence rather than a silent one, and
    // Sample0 is additionally a genuine semantic loss (the guest picked one
    // sample, D3D12 averaged all of them).
    if (rd.sampleSelect != static_cast<uint32_t>(native::GuestCopySampleSelect::AvgAll)) {
        const uint64_t n = ++g_resolveRefusals;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_WARN("D3D12Backend: EDRAM resolve #{} is an APPROXIMATION: guest "
                          "sampleSelect={} (xenosFmt={} {}), D3D12 ResolveSubresource always "
                          "box-averages all samples",
                          n, rd.sampleSelect, rd.xenosFormat, rd.formatName);
        }
    }

    return ResolveRenderTarget(srcRt, dstTexture,
                               static_cast<DXGI_FORMAT>(rd.hostDxgiFormat));
}

const D3D12_BLEND_DESC* D3D12Backend::GetOrCreateBlendState(const D3D12_BLEND_DESC& desc) {
    BlendStateCacheKey key{ desc };
    auto it = m_blendStateCache.find(key);
    if (it != m_blendStateCache.end()) return &it->second;
    auto [it2, inserted] = m_blendStateCache.emplace(BlendStateCacheKey{ desc }, desc);
    return &it2->second;
}

const D3D12_DEPTH_STENCIL_DESC* D3D12Backend::GetOrCreateDepthStencilState(const D3D12_DEPTH_STENCIL_DESC& desc) {
    DepthStencilStateCacheKey key{ desc };
    auto it = m_depthStencilStateCache.find(key);
    if (it != m_depthStencilStateCache.end()) return &it->second;
    auto [it2, inserted] = m_depthStencilStateCache.emplace(DepthStencilStateCacheKey{ desc }, desc);
    return &it2->second;
}

const D3D12_RASTERIZER_DESC* D3D12Backend::GetOrCreateRasterizerState(const D3D12_RASTERIZER_DESC& desc) {
    RasterizerStateCacheKey key{ desc };
    auto it = m_rasterizerStateCache.find(key);
    if (it != m_rasterizerStateCache.end()) return &it->second;
    auto [it2, inserted] = m_rasterizerStateCache.emplace(RasterizerStateCacheKey{ desc }, desc);
    return &it2->second;
}

// F-641: genuinely per-subresource tracking. The previous version kept ONE state
// and ONE subresource index per resource, which cannot describe a mip chain: it
// could not say "mip 0 is PIXEL_SHADER_RESOURCE while mip 1 is COPY_DEST", and
// it emitted the barrier on the remembered subresource rather than the requested
// one, so the mipgen loop transitioned a resource it was not transitioning. The
// `stateBefore` argument is now ADVISORY: the emitted barrier always names the
// tracked state, because that is the only one D3D12 can verify. A caller whose
// `stateBefore` disagrees with the tracker has a real bug, so that disagreement
// is counted rather than silently obeyed.
D3D12_RESOURCE_STATES D3D12Backend::TrackedState(ID3D12Resource* resource,
                                                UINT subresource) const {
    if (!resource) return D3D12_RESOURCE_STATE_COMMON;
    auto it = m_resourceStates.find(resource);
    if (it == m_resourceStates.end()) return D3D12_RESOURCE_STATE_COMMON;
    const ResourceStateEntry& e = it->second;
    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        return e.hasWhole ? e.whole : D3D12_RESOURCE_STATE_COMMON;
    }
    if (subresource < e.perMip.size()) return e.perMip[subresource];
    return e.hasWhole ? e.whole : D3D12_RESOURCE_STATE_COMMON;
}

void D3D12Backend::SetTrackedState(ID3D12Resource* resource, UINT subresource,
                                   D3D12_RESOURCE_STATES state) {
    if (!resource) return;
    ResourceStateEntry& e = m_resourceStates[resource];
    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        e.whole = state;
        e.hasWhole = true;
        for (auto& s : e.perMip) s = state;
        return;
    }
    if (e.perMip.size() <= subresource) e.perMip.resize(static_cast<size_t>(subresource) + 1,
                                                         e.hasWhole ? e.whole
                                                                    : D3D12_RESOURCE_STATE_COMMON);
    e.perMip[subresource] = state;
}

bool D3D12Backend::TransitionResource(ID3D12Resource* resource,
                                      D3D12_RESOURCE_STATES stateBefore,
                                      D3D12_RESOURCE_STATES stateAfter,
                                      UINT subresource,
                                      ID3D12GraphicsCommandList* cl) {
    ID3D12GraphicsCommandList* cmd = cl ? cl : m_commandList.Get();
    if (!resource || !cmd) return false;
    if (stateBefore == stateAfter) return false;

    const D3D12_RESOURCE_STATES tracked = TrackedState(resource, subresource);
    if (subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && tracked != stateBefore) {
        ++m_trackedStateDisagreements;
    }
    if (tracked == stateAfter) return false;  // already there: no barrier

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = tracked;
    barrier.Transition.StateAfter = stateAfter;
    barrier.Transition.Subresource = subresource;
    (++g_dcBarriers, cmd)->ResourceBarrier(1, &barrier);
    SetTrackedState(resource, subresource, stateAfter);
    return true;
}

bool D3D12Backend::BeginRenderPass(const D3D12Backend::RenderPassDesc& desc) {
    if (!m_commandList || desc.rtvs.empty()) return false;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
    for (const auto& rtv : desc.rtvs) rtvHandles.push_back(rtv.cpuHandle);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
    bool hasDsv = desc.dsv.has_value();
    if (hasDsv) dsvHandle = desc.dsv->cpuHandle;
    // F-XXX: guarded bind (same reason as the blit site above).
    unsigned long passOmsetCode = 0;
    if (!TryOMSetRenderTargets(m_commandList.Get(),
                               static_cast<UINT>(rtvHandles.size()),
                               rtvHandles.data(), FALSE,
                               hasDsv ? &dsvHandle : nullptr, &passOmsetCode)) {
        static std::atomic<uint32_t> s_passOmsetFaults{0};
        const uint32_t fn =
            s_passOmsetFaults.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fn <= 8 || (fn % 200) == 0) {
            MCLA_LOG_ERROR(
                "PASS-OMSET-FAULT #{} code=0x{:08X} rtvs={} hasDsv={}",
                fn, static_cast<unsigned>(passOmsetCode), rtvHandles.size(),
                hasDsv ? 1 : 0);
        }
        return false;
    }
    m_boundTargetsValid = false;  // targets changed; see cache decl
    InvalidateDrawCaches();
    for (const auto& rtv : desc.rtvs) {
        if (rtv.clear) {
            m_commandList->ClearRenderTargetView(rtv.cpuHandle, rtv.clearColor, 0, nullptr);
        }
    }
    if (hasDsv && (desc.dsv->clearDepth || desc.dsv->clearStencil)) {
        D3D12_CLEAR_FLAGS flags = static_cast<D3D12_CLEAR_FLAGS>(0);
        if (desc.dsv->clearDepth) flags |= D3D12_CLEAR_FLAG_DEPTH;
        if (desc.dsv->clearStencil) flags |= D3D12_CLEAR_FLAG_STENCIL;
        m_commandList->ClearDepthStencilView(dsvHandle, flags,
                                             desc.dsv->depthClear, desc.dsv->stencilClear,
                                             0, nullptr);
    }
    if (desc.viewport.Width > 0 && desc.viewport.Height > 0) {
        m_commandList->RSSetViewports(1, &desc.viewport);
    }
    if (desc.scissor.right > desc.scissor.left && desc.scissor.bottom > desc.scissor.top) {
        m_commandList->RSSetScissorRects(1, &desc.scissor);
    }
    m_activeRenderPass = desc;
    return true;
}

void D3D12Backend::EndRenderPass() {
    if (!m_activeRenderPass.has_value() || !m_commandList) return;
    m_activeRenderPass.reset();
}

} // namespace mcla::native

// =============================================================================
// Phase 5: Captured DrawPacket Native Draw Implementation
// =============================================================================

namespace mcla::native {

// F-645: THIN FORWARDER. The 100-line hand-rolled builder that used to live
// here is deleted; the authoritative one is renderer::BuildGrcFvfInputLayout
// (pipeline_cache.h:307-320), which is what the render thread's HashInputLayout
// and the pipeline worker's CreateGraphicsPipelineState layout hash go through.
//
// Why the old body had to go rather than be kept as a fallback:
//   * it used LANE-INDEX semantics (lane 5 -> TEXCOORD0, lane 13 -> TANGENT0)
//     where grc_fvf_decode.h uses the architected lane->semantic map (4/5 =
//     Colour0/1, 6-13 = TexCoord0-7, 14/15 = Tangent, 16/17 = Binormal), so a VS
//     expecting COLOR0 could be fed lane 5 as TEXCOORD0;
//   * it accepted all 16 type codes, including the ones grc_fvf_decode refuses
//     (2/4/11-15 and any bound lane >= 16) as "unverified";
//   * it DROPPED lanes 16-17 (loop bounded at 16) even though they decode;
//   * it set D3D12_APPEND_ALIGNED_ELEMENT for every element instead of ordering
//     by byteOffset, so the layout's offsets were implicit while the render
//     thread's hash was over the decoded declaration -- two descriptions of one
//     vertex stream that could and did disagree, which is the invariant
//     PipelineKey::inputLayoutHash exists to prevent (pipeline_cache.h:24-28).
//
// The member stays because src/render_thread.cpp:498 calls it and that file is
// owned by another agent; the declaration in d3d12_backend.h is unchanged, so
// that call site keeps compiling with no edit on their side. When the render
// thread's owner updates it, this can be deleted and the free function called
// directly (pipeline_cache.h:316 already provides the same overload).
std::vector<D3D12_INPUT_ELEMENT_DESC>
D3D12Backend::BuildInputLayoutFromGrcFvf(const native::GrcFvfDesc& grcFvf)
{
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    // outDeclHash is passed as nullptr on purpose: this function's callers want
    // the LAYOUT, and the render thread computes its own declaration hash from
    // the same GrcFvfDesc (render_thread.cpp:500) -- asking for it here would
    // publish a second hash of the same declaration and invite the two to drift.
    if (!renderer::BuildGrcFvfInputLayout(grcFvf, layout, nullptr)) {
        // BuildGrcFvfInputLayout refuses rather than degrading: an invalid
        // declaration, an unknown channel, or a declared fvfSize that disagrees
        // with the computed stride. That is the right answer here, because
        // kRefuseEmptyInputLayout is what stops a real PSO being built against a
        // layout that does not describe the stream.
        const uint64_t n = ++g_layoutRefusals;
        if (ShouldLogRefusal(n)) {
            MCLA_LOG_WARN("BuildInputLayoutFromGrcFvf refused #{}: grc_fvf_decode says this "
                          "declaration must not be laid out (mask=0x{:08X} size={} types={:016X})",
                          n, grcFvf.fvfMask, grcFvf.fvfSize, grcFvf.types);
        }
        layout.clear();
        return layout;
    }
    // 2026-10-04: EVERY ELEMENT, EVERY FIELD, appended at the END of this row.
    //
    // WHY THIS IS THE ONLY INPUT-LAYOUT DUMP IN THIS FILE. A
    // D3D12_INPUT_ELEMENT_DESC is never converted into an ID3D12InputLayout
    // anywhere in src/ -- CreateInputLayout has zero occurrences across the tree
    // -- because the descriptors are handed straight to
    // CreateGraphicsPipelineState as psoDesc.InputLayout
    // (renderer/pipeline_cache.cpp:1910-1911) and are then unreachable: a
    // D3D12 pipeline state reports none of its own fields. The live draw path
    // therefore builds its layout in render_thread.cpp, from the guest vertex
    // shader's own element table, and does not carry it into this layer at all
    // (DynamicMeshDesc has no layout member, d3d12_backend.h:68-104).
    //
    // MEASURED, so the row is not mistaken for a live one: this function's
    // literal occurs 0 times in build/w709.log, while the live layout is
    // reported by render_thread's own row (w709.log:7497, `DRAW-CP-LAYOUT #1
    // draw=1 elements=2`). So a zero of the row appended here is a tautology
    // about THIS function, never a verdict on the layout the draw used. The
    // dump is kept because this is the only site in this file that materialises
    // descriptors at all, and it is the site that would carry the moment the
    // declaration reaches the live path.
    //
    // FIELD ORDER IS WRITTEN OUT, never brace-initialised: the struct is
    // (SemanticName, SemanticIndex, Format, InputSlot, AlignedByteOffset,
    // InputSlotClass, InstanceDataStepRate). Assigning positionally into a
    // braced list is how an offset silently becomes a format.
    {
        std::string dump;
        dump.reserve(layout.size() * 128u + 32u);
        char word[64];
        dump += "n=";
        std::snprintf(word, sizeof(word), "%u", static_cast<unsigned>(layout.size()));
        dump += word;
        for (size_t i = 0; i < layout.size(); ++i) {
            // Field-by-field, into named locals first. The names are the struct's
            // own, so a reader can check the assignment without the struct.
            const char* const semanticName = layout[i].SemanticName;
            const UINT semanticIndex = layout[i].SemanticIndex;
            const DXGI_FORMAT format = layout[i].Format;
            const UINT inputSlot = layout[i].InputSlot;
            const UINT alignedByteOffset = layout[i].AlignedByteOffset;
            const D3D12_INPUT_CLASSIFICATION inputSlotClass = layout[i].InputSlotClass;
            const UINT instanceDataStepRate = layout[i].InstanceDataStepRate;
            dump += " [";
            std::snprintf(word, sizeof(word), "%zu:{", i);
            dump += word;
            dump += (semanticName != nullptr) ? semanticName : "(null)";
            dump += ",si=";
            std::snprintf(word, sizeof(word), "%u", semanticIndex);
            dump += word;
            dump += ",fmt=";
            std::snprintf(word, sizeof(word), "0x%X",
                          static_cast<unsigned>(format));
            dump += word;
            dump += ",slot=";
            std::snprintf(word, sizeof(word), "%u", inputSlot);
            dump += word;
            dump += ",off=";
            std::snprintf(word, sizeof(word), "%u", alignedByteOffset);
            dump += word;
            dump += ",class=";
            std::snprintf(word, sizeof(word), "%u",
                          static_cast<unsigned>(inputSlotClass));
            dump += word;
            dump += ",step=";
            std::snprintf(word, sizeof(word), "%u", instanceDataStepRate);
            dump += word;
            dump += "}]";
        }
        MCLA_LOG_INFO(
            "BuildInputLayoutFromGrcFvf: {} elements from mask=0x{:08X} size={} (via "
            "renderer::BuildGrcFvfInputLayout, declHash not requested) "
            "layout=[{}] -- every element, field by field, in the struct's own "
            "order SemanticName/SemanticIndex/Format/InputSlot/AlignedByteOffset/"
            "InputSlotClass/InstanceDataStepRate; n is the count that would be "
            "passed as psoDesc.NumElements. No input-layout object is created "
            "anywhere in this tree, so those descriptors are consumed inline by "
            "CreateGraphicsPipelineState and unreadable afterwards. This function's "
            "literal is absent from build/w709.log, so its absence here is not a "
            "statement about the layout the live draw used",
            layout.size(), grcFvf.fvfMask, grcFvf.fvfSize, dump);
        return layout;
    }
}

renderer::PipelineState D3D12Backend::BuildPipelineStateFromPacket(const native::DrawPacket& packet)
{
    renderer::PipelineState state = {};

    // Target formats - default to R8G8B8A8_UNORM
    state.targetFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    state.depthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    // Blend state hash - extract from paClipCntl / other registers
    state.blendState = packet.paClipCntl ^ 0x12345678;

    // Raster state hash - extract cull mode, fill mode from paClipCntl / paSuScModeCntl
    state.rasterState = packet.paClipCntl ^ packet.paSuScModeCntl;

    // Depth-stencil state hash
    state.depthStencilState = packet.paClVteCntl ^ 0xABCDEF00;

    // Primitive topology
    switch (packet.primType) {
        case 0: state.topology = 0; break; // Triangle
        case 1: state.topology = 0; break; // Triangle strip
        case 2: state.topology = 1; break; // Line
        case 3: state.topology = 1; break; // Line strip
        case 4: state.topology = 2; break; // Point
        default: state.topology = 0; break;
    }

    state.sampleCount = 1;

    return state;
}

bool D3D12Backend::UploadPacketGeometry(const native::DrawPacket& packet,
                                        D3D12_GPU_VIRTUAL_ADDRESS& vbGpu,
                                        D3D12_GPU_VIRTUAL_ADDRESS& ibGpu,
                                        uint32_t& vertexStride,
                                        uint32_t& vertexCount,
                                        uint32_t& indexCount,
                                        DXGI_FORMAT& indexFormat)
{
    // Get active guest memory view
    auto& memView = mcla::native::GetActiveGuestMemoryView();

    // Upload vertex buffer
    if (packet.vertexStreamCount == 0) {
        MCLA_LOG_WARN("UploadPacketGeometry: no vertex streams in packet");
        return false;
    }

    // For now, use first vertex stream
    const auto& stream = packet.vertexStreams[0];
    if (!stream.guestAddress || stream.stride == 0) {
        MCLA_LOG_WARN("UploadPacketGeometry: invalid vertex stream 0 (addr=0x%08X stride=%u)",
                      stream.guestAddress, stream.stride);
        return false;
    }

    // Read vertex data from guest memory
    if (!memView.IsValidRange(stream.guestAddress, stream.stride * packet.indexCount)) {
        MCLA_LOG_WARN("UploadPacketGeometry: vertex stream out of bounds (addr=0x%08X stride=%u count=%u)",
                      stream.guestAddress, stream.stride, packet.indexCount);
        return false;
    }

    // For now, use the test vertex stride (28 bytes) since our test PSO expects that
    vertexStride = 28;
    vertexCount = packet.indexCount;

    std::vector<uint8_t> vertexData(vertexStride * vertexCount);
    if (!memView.ReadBytes(stream.guestAddress, vertexData.data(), vertexData.size())) {
        MCLA_LOG_WARN("UploadPacketGeometry: failed to read vertex data from guest");
        return false;
    }

    // Upload vertex buffer
    vbGpu = 0;
    uint8_t* vbDst = MapUpload(vertexData.size(), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, vbGpu);
    if (!vbDst) return false;
    std::memcpy(vbDst, vertexData.data(), vertexData.size());

    // Upload index buffer
    if (packet.indexType == 1) {
        indexFormat = DXGI_FORMAT_R32_UINT;
    } else {
        indexFormat = DXGI_FORMAT_R16_UINT;
    }
    indexCount = packet.indexCount;

    // Check if we have a cached index buffer for this packet's index data
    ResourceKey ibKey = { ResourceKind::Buffer, packet.indexBufferAddress, packet.indexBufferSize, 0, 0 };
    ResourceEntry ibEntry;
    if (m_resourceCache.Find(ibKey, ibEntry)) {
        ibGpu = ibEntry.handle;
        MCLA_LOG_INFO("UploadPacketGeometry: using cached index buffer 0x%llx", ibGpu);
    } else {
        // Upload index buffer data
        if (!memView.IsValidRange(packet.indexBufferAddress, packet.indexBufferSize)) {
            MCLA_LOG_WARN("UploadPacketGeometry: index buffer out of bounds (addr=0x%08X size=%u)",
                          packet.indexBufferAddress, packet.indexBufferSize);
            return false;
        }

        std::vector<uint8_t> indexData(packet.indexBufferSize);
        if (!memView.ReadBytes(packet.indexBufferAddress, indexData.data(), indexData.size())) {
            MCLA_LOG_WARN("UploadPacketGeometry: failed to read index data from guest");
            return false;
        }

        ibGpu = 0;
        uint8_t* ibDst = MapUpload(indexData.size(), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, ibGpu);
        if (!ibDst) return false;
        std::memcpy(ibDst, indexData.data(), indexData.size());

        // Cache the index buffer for future frames
        ResourceEntry entry = { ibGpu, 0, false };
        m_resourceCache.Insert(ibKey, entry);
        MCLA_LOG_INFO("UploadPacketGeometry: cached new index buffer 0x%llx", ibGpu);
    }

    return true;
}

// F-610: DrawCapturedPacket is DELETED, not fixed. Its call count was zero
// (grep over src/ found only this definition and the d3d12_backend.h
// declaration), and it could not have worked if it had been called:
//
//  1. It built DynamicMeshDesc with vertexBytes=nullptr / vertexBytesSize=0
//     on the "already uploaded" assumption, but DrawDynamicMeshWithPipeline
//     guards exactly that at :1366 (!vertexBytes || vertexBytesSize == 0 ||
//     vertexCount == 0 -> return false). Every call returned false silently,
//     with no log and no stat, so nothing would have noticed.
//  2. It hardcoded vertexStride=28 while vertexCount came from the real
//     packet, so even past the guard the stride and the count disagreed with
//     the packet's own layout.
//  3. It called GetOrCompile with empty VS/PS HLSL (old :2464-2466), which
//     the pipeline cache worker drops on sight
//     (src/renderer/pipeline_cache.cpp:244-249, "missing device or empty
//     HLSL"), so the only PSO it could ever get was the test PSO it then
//     fell back to anyway.
//
// Fixing it properly means giving DrawDynamicMesh a real pre-uploaded-vertex
// path (a cachedVertexGpu on DynamicMeshDesc alongside cachedIndexGpu) and a
// real shader source, i.e. new backend surface for a feature with no caller
// and no guest capture behind it. That is speculative, not a fix, so the
// function is gone and BuildPipelineStateFromPacket / UploadPacketGeometry
// (its only two helpers) go with it rather than sit here as unreferenced
// code with no honest caller. The live equivalent is the DRAW_CAPTURED
// command path in src/render_thread.cpp, which does route real guest
// geometry through the staging-upload path via DrawDynamicMesh and is where
// a revived capture belongs. DELETION TRIGGER: zero call sites.


// Phase 3 MT: recorder-aware overloads.
uint32_t D3D12Backend::BindGuestTextures(const GuestTextureEntry* entries, uint32_t count) {
    return BindGuestTextures(entries, count, nullptr);
}
uint32_t D3D12Backend::BindGuestTextures(const GuestTextureEntry* entries, uint32_t count, DrawRecorder& rec) {
    return BindGuestTextures(entries, count, &rec);
}

bool D3D12Backend::DrawDynamicMeshWithPipeline(const DynamicMeshDesc& desc, ID3D12PipelineState* pipeline) {
    return DrawDynamicMeshWithPipeline(desc, pipeline, nullptr);
}

bool D3D12Backend::DrawDynamicMeshWithPipeline(const DynamicMeshDesc& desc, ID3D12PipelineState* pipeline, DrawRecorder& rec) {
    return DrawDynamicMeshWithPipeline(desc, pipeline, &rec);
}

// ---------------------------------------------------------------------------
// F9 OWNER PIXEL PROBE -- see g_f9State. Output: owner_reports/f9_<stamp>.{png,txt} + _ps_<hash>.hlsl.
// ---------------------------------------------------------------------------
void D3D12Backend::F9RequestCapture(float u01, float v01) {
    int idle = 0;
    g_f9ReqU.store(u01); g_f9ReqV.store(v01);
    if (g_f9State.compare_exchange_strong(idle, 1))
        MCLA_LOG_ERROR("F9-PROBE requested at window uv=({:.4f},{:.4f}); recording the next frame", u01, v01);
}
bool D3D12Backend::F9Active() {
    const int s = g_f9State.load(std::memory_order_relaxed);
    return s == 1 || s == 2;
}
void D3D12Backend::F9NotePsHlsl(uint64_t psHash, const std::string& hlsl) {
    std::lock_guard<std::mutex> lk(g_f9HlslMu);
    if (g_f9Hlsl.size() < 4096u) g_f9Hlsl.emplace(psHash, hlsl);
}

void D3D12Backend::F9ArmFrame() {
    g_f9Draws.clear();
    g_f9RbUsed = 0;
    m_f9FrameRb.Reset();
    if (!m_f9PixRb && m_device) {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = uint64_t(kF9RbSlots) * kF9RbSlotBytes;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&m_f9PixRb))))
            MCLA_LOG_ERROR("F9-PROBE pixel readback buffer creation failed; before/after values will be absent");
    }
    if (!m_f9PixRb) g_f9RbUsed = kF9RbSlots;  // no buffer: record draws without pixel values
    // Window -> back buffer -> the guest picture inside it (same viewport the scan-out blit uses).
    const float bw = static_cast<float>(m_swapW), bh = static_cast<float>(m_swapH);
    g_f9BbX = g_f9ReqU.load() * bw; g_f9BbY = g_f9ReqV.load() * bh;
    const D3D12_VIEWPORT vp = (m_graphicsSettings.aspectCorrection && m_guestRtW && m_guestRtH)
        ? GetAspectCorrectViewport(static_cast<float>(m_guestRtW) / static_cast<float>(m_guestRtH))
        : D3D12_VIEWPORT{0.0f, 0.0f, bw, bh, 0.0f, 1.0f};
    g_f9U = vp.Width > 0 ? (g_f9BbX - vp.TopLeftX) / vp.Width : -1.0f;
    g_f9V = vp.Height > 0 ? (g_f9BbY - vp.TopLeftY) / vp.Height : -1.0f;
    g_f9State.store(2);
}

void D3D12Backend::F9RecordPixel(ID3D12GraphicsCommandList* cl, ID3D12Resource* rt, uint32_t x, uint32_t y, uint32_t slot) {
    if (!cl || !rt || !m_f9PixRb || slot >= kF9RbSlots) return;
    D3D12_RESOURCE_DESC rd = rt->GetDesc();
    rd.Width = 1; rd.Height = 1; rd.MipLevels = 1; rd.DepthOrArraySize = 1;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_f9PixRb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    m_device->GetCopyableFootprints(&rd, 0, 1, uint64_t(slot) * kF9RbSlotBytes, &dst.PlacedFootprint, nullptr, nullptr, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;  // the draw binds it as RTV
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    const D3D12_BOX box = {x, y, 0, x + 1, y + 1, 1};
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
}

void D3D12Backend::F9CopyBackBuffer() {
    ID3D12Resource* back = (m_frameIndex < kBufferCount) ? m_renderTargets[m_frameIndex].Get() : nullptr;
    if (!back || !m_device) return;
    const D3D12_RESOURCE_DESC rd = back->GetDesc();
    uint64_t total = 0;
    m_device->GetCopyableFootprints(&rd, 0, 1, 0, &m_f9FrameFp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&m_f9FrameRb)))) { m_f9FrameRb.Reset(); return; }
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = back;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;  // ResolveGuestRenderTarget leaves it PRESENT
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_commandList->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_f9FrameRb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = m_f9FrameFp;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = back; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    m_commandList->ResourceBarrier(1, &b);
}

static std::string F9PixStr(DXGI_FORMAT f, const uint8_t* p) {
    char buf[96];
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        std::snprintf(buf, sizeof buf, "rgba8(%u,%u,%u,%u)", p[0], p[1], p[2], p[3]); break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        std::snprintf(buf, sizeof buf, "rgba8(%u,%u,%u,%u)", p[2], p[1], p[0], p[3]); break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        uint16_t h[4]; std::memcpy(h, p, sizeof h);
        std::snprintf(buf, sizeof buf, "f16(%.4f,%.4f,%.4f,%.4f)", PixProbeHalfToFloat(h[0]), PixProbeHalfToFloat(h[1]),
                      PixProbeHalfToFloat(h[2]), PixProbeHalfToFloat(h[3]));
        break;
    }
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        uint32_t v; std::memcpy(&v, p, 4);
        std::snprintf(buf, sizeof buf, "10b(%u,%u,%u,%u)", v & 0x3FFu, (v >> 10) & 0x3FFu, (v >> 20) & 0x3FFu, v >> 30);
        break;
    }
    case DXGI_FORMAT_R32_FLOAT: { float v; std::memcpy(&v, p, 4); std::snprintf(buf, sizeof buf, "f32(%.6f)", v); break; }
    default: {
        uint64_t v; std::memcpy(&v, p, 8);
        std::snprintf(buf, sizeof buf, "raw(dxgi=%u,%016llX)", static_cast<uint32_t>(f), static_cast<unsigned long long>(v));
    }
    }
    return buf;
}

// Minimal PNG (stored deflate; the tree has no compression library): 8-bit RGB.
static bool F9WritePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb) {
    uint32_t crcTab[256];
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTab[n] = c;
    }
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a1 = 1, a2 = 0;
    for (uint8_t v : raw) { a1 = (a1 + v) % 65521u; a2 = (a2 + a1) % 65521u; }
    for (size_t off = 0;; off += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        if (last) break;
    }
    const uint32_t ad = (a2 << 16) | a1;
    for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(ad >> s));
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; std::fwrite(b, 1, 4, f); };
    auto chunk = [&](const char* type, const uint8_t* d, size_t n) {
        be32(static_cast<uint32_t>(n));
        uint32_t c = 0xFFFFFFFFu;
        for (int i = 0; i < 4; ++i) c = crcTab[(c ^ uint8_t(type[i])) & 0xFF] ^ (c >> 8);
        for (size_t i = 0; i < n; ++i) c = crcTab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
        std::fwrite(type, 1, 4, f);
        if (n) std::fwrite(d, 1, n, f);
        be32(c ^ 0xFFFFFFFFu);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::fwrite(sig, 1, 8, f);
    const uint8_t ihdr[13] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                              uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), z.size());
    chunk("IEND", nullptr, 0);
    return std::fclose(f) == 0;
}

void D3D12Backend::F9Finalize(uint64_t fenceValue) {
    g_f9State.store(3);  // stop recording; the next frame's draws are not part of this capture
    if (m_fence && m_fence->GetCompletedValue() < fenceValue) {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (ev && SUCCEEDED(m_fence->SetEventOnCompletion(fenceValue, ev))) WaitForSingleObject(ev, 10000);
        if (ev) CloseHandle(ev);
    }
    char stamp[64];
    {
        const std::time_t t = std::time(nullptr);
        std::tm tmv{};
        localtime_s(&tmv, &t);
        std::strftime(stamp, sizeof stamp, "f9_%Y%m%d_%H%M%S", &tmv);
    }
    std::error_code ec;
    std::filesystem::create_directories("owner_reports", ec);
    const std::string stem = std::string("owner_reports/") + stamp;

    // Full frame with a magenta crosshair at the cursor (centre 5x5 left untouched).
    std::string pngNote = "not written: this frame had no guest-RT scan-out resolve (movie/clear/back-buffer frame)";
    if (m_f9FrameRb) {
        void* map = nullptr;
        const D3D12_RANGE r = {0, size_t(m_f9FrameFp.Footprint.RowPitch) * m_f9FrameFp.Footprint.Height};
        if (SUCCEEDED(m_f9FrameRb->Map(0, &r, &map)) && map) {
            const uint32_t w = m_f9FrameFp.Footprint.Width, h = m_f9FrameFp.Footprint.Height;
            const bool bgra = m_f9FrameFp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                              m_f9FrameFp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            std::vector<uint8_t> rgb(size_t(w) * h * 3);
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* s = static_cast<const uint8_t*>(map) + size_t(y) * m_f9FrameFp.Footprint.RowPitch;
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t* d = &rgb[(size_t(y) * w + x) * 3];
                    d[0] = s[x * 4 + (bgra ? 2 : 0)]; d[1] = s[x * 4 + 1]; d[2] = s[x * 4 + (bgra ? 0 : 2)];
                }
            }
            const int cx = int(g_f9BbX), cy = int(g_f9BbY);
            for (int k = -12; k <= 12; ++k) {
                if (k >= -2 && k <= 2) continue;
                const int pts[2][2] = {{cx + k, cy}, {cx, cy + k}};
                for (const auto& pt : pts) {
                    if (pt[0] < 0 || pt[1] < 0 || pt[0] >= int(w) || pt[1] >= int(h)) continue;
                    uint8_t* d = &rgb[(size_t(pt[1]) * w + pt[0]) * 3];
                    d[0] = 255; d[1] = 0; d[2] = 255;
                }
            }
            const D3D12_RANGE wr = {0, 0};
            m_f9FrameRb->Unmap(0, &wr);
            pngNote = F9WritePng(stem + ".png", w, h, rgb) ? stem + ".png" : std::string("write FAILED");
        }
        m_f9FrameRb.Reset();
    }

    // Per-draw RT0 texel before/after.
    const uint8_t* pix = nullptr;
    void* pmap = nullptr;
    if (m_f9PixRb && g_f9RbUsed && SUCCEEDED(m_f9PixRb->Map(0, nullptr, &pmap))) pix = static_cast<const uint8_t*>(pmap);
    uint32_t changed = 0;
    std::vector<bool> chg(g_f9Draws.size(), false);
    std::vector<std::string> before(g_f9Draws.size()), after(g_f9Draws.size());
    for (size_t i = 0; i < g_f9Draws.size(); ++i) {
        const F9Draw& d = g_f9Draws[i];
        if (!pix || d.rbSlot == UINT32_MAX) {
            before[i] = after[i] = d.samples > 1 ? "n/a(msaa)" : "n/a(readback cap)";
            continue;
        }
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = 1; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.Format = d.fmt; rd.SampleDesc.Count = 1;
        UINT64 rowBytes = 4;
        m_device->GetCopyableFootprints(&rd, 0, 1, 0, nullptr, nullptr, &rowBytes, nullptr);
        const uint8_t* b0 = pix + size_t(d.rbSlot) * kF9RbSlotBytes;
        const uint8_t* b1 = b0 + kF9RbSlotBytes;
        chg[i] = std::memcmp(b0, b1, std::min<size_t>(size_t(rowBytes), kF9RbSlotBytes)) != 0;
        changed += chg[i] ? 1u : 0u;
        before[i] = F9PixStr(d.fmt, b0); after[i] = F9PixStr(d.fmt, b1);
    }
    if (pmap) { const D3D12_RANGE wr = {0, 0}; m_f9PixRb->Unmap(0, &wr); }

    // Top 3 = the last three distinct PS among draws that changed the pixel (newest first), else any listed draw.
    std::vector<uint64_t> top;
    for (int pass = 0; pass < 2 && top.size() < 3; ++pass)
        for (size_t i = g_f9Draws.size(); i-- > 0 && top.size() < 3;) {
            const uint64_t ph = g_f9Draws[i].psHash;
            if ((pass == 0 && !chg[i]) || !ph || std::find(top.begin(), top.end(), ph) != top.end()) continue;
            top.push_back(ph);
        }

    if (std::FILE* f = std::fopen((stem + ".txt").c_str(), "wb")) {
        // Xenos blend factor / op / compare enums (xenos.h; layout cited at renderer/pipeline_cache.cpp decodeGuestRtBlend).
        static const char* kFac[17] = {"ZERO", "ONE", "?2", "?3", "SRC_COLOR", "INV_SRC_COLOR", "SRC_ALPHA", "INV_SRC_ALPHA",
                                       "DST_COLOR", "INV_DST_COLOR", "DST_ALPHA", "INV_DST_ALPHA", "CONST_COLOR",
                                       "INV_CONST_COLOR", "CONST_ALPHA", "INV_CONST_ALPHA", "SRC_ALPHA_SAT"};
        static const char* kOp[8] = {"ADD", "SUB", "MIN", "MAX", "REVSUB", "?5", "?6", "?7"};
        static const char* kCmp[8] = {"NEVER", "LESS", "EQUAL", "LEQUAL", "GREATER", "NOTEQUAL", "GEQUAL", "ALWAYS"};
        auto fac = [&](uint32_t v) { return (v & 31u) < 17u ? kFac[v & 31u] : "?"; };
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        SYSTEMTIME st{};
        if (GetFileAttributesExA(exe, GetFileExInfoStandard, &fad)) FileTimeToSystemTime(&fad.ftLastWriteTime, &st);
        std::fprintf(f, "F9 PIXEL PROBE %s\n", stamp);
        std::fprintf(f, "build: exe=%s exeMtimeUTC=%04u-%02u-%02u %02u:%02u:%02u compiled=%s %s\n", exe, st.wYear, st.wMonth,
                     st.wDay, st.wHour, st.wMinute, st.wSecond, __DATE__, __TIME__);
        std::fprintf(f, "cursor: windowUV=(%.4f,%.4f) backbuffer=(%d,%d) of %ux%u pictureUV=(%.4f,%.4f) guestRt=%ux%u renderScale=%u present=%llu\n",
                     g_f9ReqU.load(), g_f9ReqV.load(), int(g_f9BbX), int(g_f9BbY), m_swapW, m_swapH, g_f9U, g_f9V,
                     m_guestRtW, m_guestRtH, RenderScale(), static_cast<unsigned long long>(m_presentedFrames));
        std::fprintf(f, "png: %s\n", pngNote.c_str());
        std::fprintf(f, "env:");
        if (char* env = GetEnvironmentStringsA()) {
            for (const char* e = env; *e; e += std::strlen(e) + 1)
                if (std::strncmp(e, "MCLA_", 5) == 0) std::fprintf(f, " %s", e);
            FreeEnvironmentStringsA(env);
        }
        std::fprintf(f, "\n\ndraws whose viewport+scissor contain the pixel: %zu; changed its RT0 texel: %u; "
                        "readback slots used %u/%u\n", g_f9Draws.size(), changed, g_f9RbUsed, kF9RbSlots);
        std::fprintf(f, "Order = submission order, newest LAST. px = the texel probed in THAT draw's RT (offscreen RTs: the "
                        "same fraction of the picture). changed=0: depth/stencil/alpha rejected it, it did not cover the "
                        "texel, or it wrote the same value. Blend constant factor: never applied (PSOs with constant "
                        "factors are refused, PC-BLEND-CONSTANT). Clears and resolves are not listed. VS/PS = translated "
                        "program hash (MCLA_DUMP_VS_HASH / MCLA_DUMP_PS_HASH); psBank = PS constant-bank hash (PIXPROBE psHash). "
                        "sign letters per component: U unsigned, S signed, B unsigned-biased, G gamma.\n\n");
        for (size_t i = 0; i < g_f9Draws.size(); ++i) {
            const F9Draw& d = g_f9Draws[i];
            const uint32_t bc = d.blend, dc = d.depth;
            std::fprintf(f, "#%zu seq=%llu changed=%d prim=0x%02X %s=%u VS=%016llX PS=%016llX psBank=%016llX\n", i,
                         static_cast<unsigned long long>(d.seq), chg[i] ? 1 : 0, d.prim, d.indexed ? "indices" : "verts", d.verts,
                         static_cast<unsigned long long>(d.vsHash), static_cast<unsigned long long>(d.psHash),
                         static_cast<unsigned long long>(d.psBank));
            std::fprintf(f, "    rt: %s base=%08X dxgi=%u %ux%u msaa=%u px=(%u,%u) vp=(%.0f,%.0f %.0fx%.0f) scissor=(%ld,%ld)-(%ld,%ld)\n",
                         d.guestRt ? "guest" : "backbuffer", d.rtBase, static_cast<uint32_t>(d.fmt), d.rtW, d.rtH, d.samples,
                         d.hx, d.hy, d.vp.TopLeftX, d.vp.TopLeftY, d.vp.Width, d.vp.Height,
                         d.sc.left, d.sc.top, d.sc.right, d.sc.bottom);
            std::fprintf(f, "    depth: RB_DEPTHCONTROL=%08X dsv=%d stencil=%u z=%u zwrite=%u zfunc=%s stencilRef=%u\n",
                         dc, d.hasDsv ? 1 : 0, dc & 1u, (dc >> 1) & 1u, (dc >> 2) & 1u, kCmp[(dc >> 4) & 7u], d.stencilRef);
            std::fprintf(f, "    blend: RB_BLENDCONTROL0=%08X enable=%d color=%s %s %s alpha=%s %s %s writeMask(RT0)=%X%s%s%s%s\n",
                         bc, bc != 0x00010001u ? 1 : 0, fac(bc), kOp[(bc >> 5) & 7u], fac(bc >> 8), fac(bc >> 16),
                         kOp[(bc >> 21) & 7u], fac(bc >> 24), d.mask & 0xFu, (d.mask & 1u) ? " R" : "",
                         (d.mask & 2u) ? " G" : "", (d.mask & 4u) ? " B" : "", (d.mask & 8u) ? " A" : "");
            std::fprintf(f, "    pixel: before=%s after=%s\n", before[i].c_str(), after[i].c_str());
            std::fprintf(f, "    textures:%s\n", d.tex.empty() ? " none" : d.tex.c_str());
        }
        std::fprintf(f, "\ntop PS (HLSL dumped):");
        for (uint64_t ph : top) std::fprintf(f, " %016llX", static_cast<unsigned long long>(ph));
        std::fprintf(f, "\n");
        std::fclose(f);
    }
    uint32_t hlslWritten = 0;
    {
        std::lock_guard<std::mutex> lk(g_f9HlslMu);
        for (uint64_t ph : top) {
            auto it = g_f9Hlsl.find(ph);
            if (it == g_f9Hlsl.end()) continue;
            const std::string fn = fmt::format("{}_ps_{:016X}.hlsl", stem, ph);
            if (std::FILE* hf = std::fopen(fn.c_str(), "wb")) {
                std::fwrite(it->second.data(), 1, it->second.size(), hf);
                std::fclose(hf);
                ++hlslWritten;
            }
        }
        g_f9Hlsl.clear();
    }
    MCLA_LOG_ERROR("F9-PROBE wrote {}.txt draws={} changed={} png={} hlsl={} cwd={}", stem, g_f9Draws.size(), changed,
                   pngNote, hlslWritten, std::filesystem::current_path(ec).string());
    g_f9Draws.clear();
    g_f9State.store(0);
}

} // namespace mcla::native

