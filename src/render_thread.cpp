#include "diag_threads.h"
#include <filesystem>
#include <set>
#include <tuple>
#include "render_thread.h"
#include "logging.h"
#include "generated/ppc_xenon/ppc_recomp_shared.h"
#include "kernel/memory.h"
#include "kernel/xdm.h"  // T43.18: g_mainParkHandle/Lr/Ns for the HB tail-light
#include "renderer/shader_translator.h"
#include "renderer/pipeline_cache.h"
#include "renderer/xenos_shader_ir.h"
#include "renderer/texture_decode.h"  // F-899: DecodeGuestTextureDescriptor, PlanTextureImport, UntileTexture2D/3D, ConvertTextureEndianness
#include "d3d12_backend.h"
#include "gpu_cp.h"  // F-647: CpReadGuestRegisters + the Xenos register indices
#include "boot_host.h"  // F-640: TranslatePhysViewToPoolVa - the ONE view->pool rule
#include "kernel/heap_arena.h"  // kConsolePhysicalBytes - the console's 512 MiB window
#include "render_queue.h"
#include "gpu_native/gnh_p2.h"  // GNH-P2
#include <array>   // F-647: the fixed 4,096-byte constant-bank buffer
#include <atomic>
#include <cstdio>  // F-900: snprintf for the VERTEX-V0 diagnostic
#include <cstdlib> // F-900: getenv for MCLA_SKIP_PRIM8 discriminator
#include <intrin.h> // _byteswap_ulong for tight VB endian-swap loops
#include <emmintrin.h> // SSE2 vertex endian swaps
#include <cstring> // F-652: std::memcpy for the kFloat register reinterpretation
#include <cmath> // floorf for per-draw UV normalize
#include <algorithm> // std::max
#include <chrono>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>
#include <vector>
// Thread-safe atomic statistics counter for single- or multi-worker Stage 1/3 paths.
template <class T> struct StatCtr {
    std::atomic<T> v{0};
    constexpr StatCtr() = default;
    constexpr StatCtr(T x) : v(x) {}
    T fetch_add(T d, std::memory_order = std::memory_order_relaxed) { return v.fetch_add(d, std::memory_order_relaxed); }
    T load(std::memory_order = std::memory_order_relaxed) const { return v.load(std::memory_order_relaxed); }
    void store(T x, std::memory_order = std::memory_order_relaxed) { v.store(x, std::memory_order_relaxed); }
    T fetch_or(T x, std::memory_order = std::memory_order_relaxed) { return v.fetch_or(x, std::memory_order_relaxed); }
    T exchange(T x, std::memory_order = std::memory_order_relaxed) { return v.exchange(x, std::memory_order_relaxed); }
    StatCtr& operator++() { fetch_add(1); return *this; }
    T operator++(int) { return fetch_add(1); }
    operator T() const { return load(); }
};


namespace mcla::native {

// F-610: heartbeat cadence. This is the ONLY software clock left in the
// render path — the per-PRESENT Sleep pacer that used to sit at
// kTargetFrameSeconds is gone (see the PRESENT case). The real frame deadline
// is the swapchain's Present(1,0) in the backend
// (src/d3d12_backend.cpp:1496, :1922), which blocks on vsync; pacing a
// second time in software only added up to 33 ms of latency to every guest
// present. The heartbeat is a LIVENESS watchdog (it proves the consumer is
// still turning when the guest goes quiet), not a pacer.
static constexpr uint32_t kHeartbeatIntervalMs = 33;
static constexpr uint32_t kTexSetKeyMax = kCpMaxTextureDescriptors * 36u;  // TEXSET-MEMO key bytes
// F-610: suppress a heartbeat when a guest NATIVE-PRESENT was consumed within
// this window. A heartbeat present is a clear-only frame, so issuing one
// while the guest is presenting real frames both halves the guest's effective
// framerate and burns a vsync slot on a clear. The window is 15 beats: long
// enough that the heartbeat cannot flicker between two guest frames, short
// enough that a guest going quiet is covered by the next beat after the
// window. REMOVAL TRIGGER: delete the heartbeat entirely once the guest
// PresentKick path is proven in a soak (a run whose PRESENT count tracks the
// guest's own frame counter, not our timer).
static constexpr uint32_t kHeartbeatGuestSuppressMs = 500;
// Queue depth warning threshold
static constexpr uint32_t kQueueDepthWarning = 30;
// Queue depth drop threshold: if queue is this deep, skip non-critical commands
static constexpr uint32_t kQueueDepthDrop = 60;
// F-610: the consumer threshold and the producer's are the same number on
// purpose. CommandQueue::kShedDepth (render_queue.h) is 60 to match this, so
// the producer stops offering work exactly when the consumer starts refusing
// it. A producer that shed later would just fill a queue the consumer drops.
static_assert(kQueueDepthDrop == CommandQueue::kShedDepth,
              "producer shed depth must equal the consumer drop depth");

// F-610: steady_clock nanoseconds of the last CONSUMED guest (non-heartbeat)
// PRESENT. Written by the PRESENT case, read by the heartbeat thread to
// implement the suppression policy. 0 = none seen yet, which the heartbeat
// reads as "not fresh" so the first beats still fire — a guest that never
// presents must not produce a dead window.
static std::atomic<int64_t> g_lastGuestPresentNs{0};

// F-902 STALL-WATCHDOG (temporary state, kept cheap): the DRAW_CAPTURED
// handler stamps the captured draw's ordinal + NowNs() every draw; the
// heartbeat thread (already awake every 33 ms) reports when the consumer has
// not advanced for > 10 s while work is pending. Distinguishes STUCK
// (seq frozen, depth high) from IDLE (seq frozen, depth 0) and SLOW
// (seq advances). Seq 0 = no draw consumed yet.
static std::atomic<uint32_t> g_consumerDrawSeq{0};
static std::atomic<int64_t> g_consumerDrawNs{0};
// F-902b: phase stamp. Each handler phase stores a static string here (no
// log volume); the watchdog prints it, naming the exact phase the stuck
// draw died in. Static strings only -- never a pointer into a draw.
static std::atomic<const char*> g_consumerStage{"-"};

// F-902b helper: stamp + optional log (log only when verbose!=0 to keep the
// hot path quiet; the watchdog is what reports).
// TEMP-PERF: every stage stamp also charges the time since the previous stamp on the
// calling thread to lock-free atomic stage buckets and logs totals every 1000 draw-top stamps.
std::string TempDcTimersRow();  // d3d12_backend.cpp
std::string TempEnvMemoRow();   // below: envelope-memo A/B arm + hits/misses for TEMP-PERF-STAGES
// Swarm p2-edit: legal-glyph draw counter (defined in hooks/wave3_guards.cpp).
extern "C" void mcla_legals_probe(uint64_t vsHash, uint64_t psHash, uint64_t drawCount);
namespace {
constexpr size_t kPerfStageCount = 29;
constexpr const char* kPerfStageNames[kPerfStageCount] = {
    "-", "idle-wait", "other-cmd", "draw-top", "tex-loop",
    "vb-fill", "constbank", "layout", "translate-vs", "derive-pso",
    "drawcall", "stage1-end", "stage2", "stage3",
    "tex-cache", "tex-hash", "tex-untile", "vb-check", "cb-hash", "cb-post", "tex-done",
    "pr-harvest", "pr-flush", "pr-read", "pr-census", "pr-blit", "pr-tail",
    "a-barrier",  // MCLA_RENDER_2STAGE prep thread waiting for the record thread to run a non-draw
    "other"
};
inline int PerfStageIndex(const char* s) {
    if (!s) return 0;
    for (size_t i = 0; i < kPerfStageCount - 1u; ++i) {
        if (s == kPerfStageNames[i] || std::strcmp(s, kPerfStageNames[i]) == 0) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(kPerfStageCount - 1u);
}
// [bank]: 0 = render (record) thread, 1 = MCLA_RENDER_2STAGE prep thread (printed as prepA[...]).
std::atomic<int64_t> s_perfStageNs[2][kPerfStageCount]{};
std::atomic<uint32_t> s_perfStageCalls[2][kPerfStageCount]{};
thread_local int tl_perfBank = 0;
std::atomic<int64_t> s_perfFrameNs[kPerfStageCount]{};  // since the last present (PERF-SLOW-FRAME)
std::atomic<uint32_t> s_perfDraws{0};
std::atomic<uint32_t> s_perfFrameDraws{0};
std::atomic<int64_t> s_perfWinStart{0};
thread_local int tl_perfLastIdx = -1;
thread_local int64_t tl_perfLastNs = 0;
// PERF-STAGES xlate: shader-translation cache misses (TranslateShaderPinned), any thread (MT stage 1).
std::atomic<uint32_t> s_perfXlMiss[2]{};
std::atomic<int64_t> s_perfXlNs[2]{}, s_perfXlMaxNs[2]{};
std::atomic<int64_t> s_perfOcNs[16]{};
std::atomic<uint32_t> s_perfOcN[16]{};
}  // namespace

static int64_t PerfNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void PerfCharge(int idx, int64_t ns) {
    s_perfStageNs[tl_perfBank][idx].fetch_add(ns, std::memory_order_relaxed);
    s_perfFrameNs[idx].fetch_add(ns, std::memory_order_relaxed);
}

static void TempPerfStagePause() {
    if (tl_perfLastIdx >= 0 && tl_perfLastNs != 0) {
        PerfCharge(tl_perfLastIdx, PerfNowNs() - tl_perfLastNs);
    }
    tl_perfLastIdx = -1;
    tl_perfLastNs = 0;
}

static void PerfNoteXlate(int ps, int64_t ns) {
    s_perfXlMiss[ps].fetch_add(1, std::memory_order_relaxed);
    s_perfXlNs[ps].fetch_add(ns, std::memory_order_relaxed);
    int64_t c = s_perfXlMaxNs[ps].load(std::memory_order_relaxed);
    while (ns > c && !s_perfXlMaxNs[ps].compare_exchange_weak(c, ns, std::memory_order_relaxed)) {}
}

static void TempPerfStage(const char* s) {
    const int64_t now = PerfNowNs();
    if (tl_perfLastIdx >= 0 && tl_perfLastNs != 0) {
        PerfCharge(tl_perfLastIdx, now - tl_perfLastNs);
    }
    const int idx = PerfStageIndex(s);
    if (idx != tl_perfLastIdx) s_perfStageCalls[tl_perfBank][idx].fetch_add(1, std::memory_order_relaxed);
    tl_perfLastIdx = idx;
    tl_perfLastNs = now;
    if (idx == 3) {  // "draw-top"
        int64_t expectedZero = 0;
        (void)s_perfWinStart.compare_exchange_strong(expectedZero, now, std::memory_order_relaxed);
        const uint32_t d = s_perfDraws.fetch_add(1, std::memory_order_relaxed) + 1u;
        s_perfFrameDraws.fetch_add(1, std::memory_order_relaxed);
        const int64_t winStart = s_perfWinStart.load(std::memory_order_relaxed);
        if (now - winStart >= 2000000000LL) {  // one row per ~2 s of draws
            s_perfWinStart.store(now, std::memory_order_relaxed);
            s_perfDraws.store(0, std::memory_order_relaxed);
            // stage=totalMs/calls/avgUs; calls = entries into the stage.
            std::string row;
            for (int bank = 0; bank < 2; ++bank) {
                std::string part;
                for (size_t i = 0; i < kPerfStageCount; ++i) {
                    const int64_t ns = s_perfStageNs[bank][i].exchange(0, std::memory_order_relaxed);
                    const uint32_t n = s_perfStageCalls[bank][i].exchange(0, std::memory_order_relaxed);
                    if (ns > 0 || n > 0) {
                        part += std::string(kPerfStageNames[i]) + "=" + std::to_string(ns / 1000000) + "ms/" +
                                std::to_string(n) + "/" + std::to_string(n ? ns / 1000 / n : 0) + "us ";
                    }
                }
                row += (bank == 0) ? part : (part.empty() ? std::string() : "prepA[ " + part + "] ");
            }
            for (int ps = 0; ps < 2; ++ps) {
                const uint32_t n = s_perfXlMiss[ps].exchange(0, std::memory_order_relaxed);
                const int64_t ns = s_perfXlNs[ps].exchange(0, std::memory_order_relaxed);
                const int64_t mx = s_perfXlMaxNs[ps].exchange(0, std::memory_order_relaxed);
                row += std::string(ps ? "psXlMiss=" : "vsXlMiss=") + std::to_string(n) + "/" +
                       std::to_string(ns / 1000000) + "ms/max" + std::to_string(mx / 1000) + "us ";
            }
            row += "ocT[";
            for (int t = 0; t < 16; ++t) {
                const uint32_t n = s_perfOcN[t].exchange(0, std::memory_order_relaxed);
                const int64_t ns = s_perfOcNs[t].exchange(0, std::memory_order_relaxed);
                if (n) row += std::to_string(t) + "=" + std::to_string(ns / 1000000) + "ms/" + std::to_string(n) + " ";
            }
            row += "] ";
            row += mcla::renderer::PerfPsoRow();
            const QueuePerfCounters qp = g_commandQueue.PerfExchange();
            row += "queue[depth=" + std::to_string(qp.depth) + " fullWait=" + std::to_string(qp.waits) +
                   " waitMs=" + std::to_string(qp.waitNs / 1000000) + " maxWaitMs=" +
                   std::to_string(qp.maxWaitNs / 1000000) + " timeouts=" + std::to_string(qp.timeouts) +
                   " stalled=" + std::to_string(qp.stalled ? 1 : 0) + "] ";
            row += TempDcTimersRow();
            row += TempEnvMemoRow();
            MCLA_LOG_ERROR("PERF-STAGES win={}ms draws={} {}", (now - winStart) / 1000000, d, row);
        }
    }
}

// PERF-FRAMES: present-to-present histogram, called by D3D12Backend right after Present
// (MCLA_PERF_STAGES=1 / MCLA_TEMP_PERF_STAGES=1 only). Frames >100 ms get a PERF-SLOW-FRAME
// row naming the render-thread stage that owned most of that frame.
void PerfOnPresent() {
    static int64_t s_last = 0, s_winStart = 0;
    static std::vector<float> s_ms;
    const int64_t now = PerfNowNs();
    // Close the open stage interval so this frame's buckets are complete (tl_* is only set on the render thread).
    if (tl_perfLastIdx >= 0 && tl_perfLastNs != 0) {
        PerfCharge(tl_perfLastIdx, now - tl_perfLastNs);
        tl_perfLastNs = now;
    }
    int64_t fr[kPerfStageCount];
    for (size_t i = 0; i < kPerfStageCount; ++i) fr[i] = s_perfFrameNs[i].exchange(0, std::memory_order_relaxed);
    const uint32_t frDraws = s_perfFrameDraws.exchange(0, std::memory_order_relaxed);
    if (s_last == 0) { s_last = s_winStart = now; return; }
    const float ms = static_cast<float>(now - s_last) * 1e-6f;
    s_last = now;
    s_ms.push_back(ms);
    if (ms > 100.0f) {
        size_t top = 0;
        for (size_t i = 1; i < kPerfStageCount; ++i) if (fr[i] > fr[top]) top = i;
        MCLA_LOG_ERROR("PERF-SLOW-FRAME ms={:.1f} draws={} dominant={} {}ms (idle-wait={}ms derive-pso={}ms translate-vs={}ms tex-loop={}ms drawcall={}ms) {}",
                       ms, frDraws, kPerfStageNames[top], fr[top] / 1000000, fr[1] / 1000000, fr[9] / 1000000,
                       fr[8] / 1000000, fr[4] / 1000000, fr[10] / 1000000, mcla::renderer::PerfPsoRow());
    }
    if (now - s_winStart < 2000000000LL) return;
    std::sort(s_ms.begin(), s_ms.end());
    const size_t n = s_ms.size();
    auto pct = [&](double p) { return s_ms[std::min(n - 1, static_cast<size_t>(p * static_cast<double>(n - 1) + 0.5))]; };
    const size_t gt33 = static_cast<size_t>(s_ms.end() - std::upper_bound(s_ms.begin(), s_ms.end(), 33.0f));
    const size_t gt100 = static_cast<size_t>(s_ms.end() - std::upper_bound(s_ms.begin(), s_ms.end(), 100.0f));
    MCLA_LOG_ERROR("PERF-FRAMES win={}ms frames={} fps={:.1f} p50={:.1f}ms p90={:.1f}ms p99={:.1f}ms max={:.1f}ms gt33={} gt100={}",
                   (now - s_winStart) / 1000000, n, n * 1e9 / static_cast<double>(now - s_winStart),
                   pct(0.50), pct(0.90), pct(0.99), s_ms.back(), gt33, gt100);
    s_ms.clear();
    s_winStart = now;
}
// The per-stage timer costs a clock read + map lookup per stamp (~10 per draw); opt-in with MCLA_TEMP_PERF_STAGES=1.
static const bool g_tempPerfStages = [] {
    const char* e = std::getenv("MCLA_TEMP_PERF_STAGES");
    const char* p = std::getenv("MCLA_PERF_STAGES");
    return (e && e[0] == '1') || (p && p[0] == '1');
}();
// PERF A/B gates (opt-in until a boot proves them): HLSL hash cached with the translation,
// translation-cache hits referenced instead of copied.
static const bool g_psoHlslHash = [] { const char* e = std::getenv("MCLA_PSO_HLSLHASH"); return !(e && e[0] == '0');  /* default ON since tB 2026-11-13 (0 errors, +23% draws/s) */ }();
static const bool g_psoRefCache = [] { const char* e = std::getenv("MCLA_PSO_REFCACHE"); return !(e && e[0] == '0');  /* default ON since tB 2026-11-13 (0 errors, +23% draws/s) */ }();
// P5 (problem #5, fullscreen 2K FPS CPU-bound ~14us/draw): EnvelopeKeyHash walks the
// whole envelope on every draw, but those bytes are immutable inside a VpEnt of the
// parsed-VS cache and the same VS/PS pair repeats for thousands of draws. Store the
// hash WITH the exact bytes it was computed from (in the VpEnt), key the reuse on
// (program shared_ptr identity, cache generation), and recheck both at consume time.
// This is provably safe: a cleared/rebuilt cache can recycle a heap address, but the
// hash dies with the entry and a mismatched (ptr,gen) is a MISS (recompute), never a
// stale hit. Default ON; MCLA_NO_ENV_MEMO=1 restores the per-draw hash for A/B.
static const bool s_envMemoOn = [] { const char* e = std::getenv("MCLA_NO_ENV_MEMO"); return !(e && e[0] == '1'); }();
static thread_local bool t_isRenderThread = false;
static std::atomic<uint32_t> g_stage1DrawCount{0};
static std::atomic<uint64_t> g_wwBurstSeq{1};
// MCLA_RENDER_2STAGE: the prep thread stores a guest PRESENT's enqNs here as soon as it sees it, so the
// write-watch harvester starts the covering harvest while the record thread is still recording the backlog.
static std::atomic<int64_t> g_wwKickNs{0};
static std::mutex g_wwKickMu;               // wakes the harvester at once instead of its 1 ms poll
static std::condition_variable g_wwKickCv;
static void KickPresentHarvest(int64_t enqNs) {
    // max, not store: in 2-stage the prep thread may already have kicked a NEWER present than the one the
    // record thread is presenting, and lowering the kick would stall that newer wait.
    int64_t cur = g_wwKickNs.load(std::memory_order_relaxed);
    while (enqNs > cur && !g_wwKickNs.compare_exchange_weak(cur, enqNs, std::memory_order_acq_rel)) {}
    { std::lock_guard<std::mutex> lk(g_wwKickMu); }
    g_wwKickCv.notify_one();
}
#define STALL_STAGE(s) \
    do { if (t_isRenderThread) { if (g_tempPerfStages) TempPerfStage(s); g_consumerStage.store((s), std::memory_order_relaxed); } } while (0)

static int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct RenderThread::DrawStage1Meta {
    uint32_t primSlot = 15u;
    uint32_t vertexCount = 0u;
    bool indexed = false;
    uint32_t tcCount = 0u;
    uint32_t tcMaxIndex = 0u;
    uint32_t pipeVsCodeBytes = 0u;
    uint32_t pipePsCodeBytes = 0u;
    uint32_t pipeVsElements = 0u;
    uint32_t pipeVsInstructions = 0u;
    uint32_t pipeVsInterpolants = 0u;
    uint64_t pipeVsHash = 0ull;
    uint64_t pipePsHash = 0ull;
    const char* pipeVsSrc = "absent";
    const char* pipePsSrc = "absent";
    const char* pipeTrStage = "none";
    size_t pipeVsHlslSize = 0u;
    size_t pipePsHlslSize = 0u;
    uint64_t inputDeclHash = 0ull;
    uint32_t pipeColorFormatCode = 0xFFFFFFFFu;
    uint32_t pipeColorFormatDxgi = 0u;
    uint32_t pipeRsPresent = 0u;
    std::string pipeTranslateError;

    void Reset() {
        primSlot = 15u;
        vertexCount = 0u;
        indexed = false;
        tcCount = 0u;
        tcMaxIndex = 0u;
        pipeVsCodeBytes = 0u;
        pipePsCodeBytes = 0u;
        pipeVsElements = 0u;
        pipeVsInstructions = 0u;
        pipeVsInterpolants = 0u;
        pipeVsHash = 0ull;
        pipePsHash = 0ull;
        pipeVsSrc = "absent";
        pipePsSrc = "absent";
        pipeTrStage = "none";
        pipeVsHlslSize = 0u;
        pipePsHlslSize = 0u;
        inputDeclHash = 0ull;
        pipeColorFormatCode = 0xFFFFFFFFu;
        pipeColorFormatDxgi = 0u;
        pipeRsPresent = 0u;
        pipeTranslateError.clear();
    }
};

struct RenderThread::BurstScratch {
    std::vector<RenderCommand> cmds;
    std::vector<D3D12Backend::PreparedDraw> prep;
    std::vector<RenderThread::DrawStage1Meta> meta;
    std::vector<uint64_t> rtKey;
    std::vector<uint64_t> dsKey;
    std::vector<uint32_t> guestH;
    std::vector<size_t> mtRun;
    std::vector<uint8_t> mtOk;
    struct SubBatch {
        size_t begin = 0;
        size_t end = 0;
        bool useMt = false;
    };
    std::vector<SubBatch> subBatches;

    std::vector<std::thread> workers;
    std::mutex poolMutex;
    std::condition_variable cvWork;
    std::condition_variable cvDone;
    std::function<void(uint32_t workerIdx)> taskFn;
    uint32_t workerCount = 0;
    uint64_t epoch = 0;
    uint32_t activePending = 0;
    bool shutdown = false;

    void EnsureWorkers(uint32_t desired) {
        if (desired <= 1u || !workers.empty()) return;
        workerCount = desired;
        for (uint32_t wi = 1u; wi < desired; ++wi) {
            workers.emplace_back([this, wi]() {
                std::string tname = "render-w" + std::to_string(wi);
                mcla::diag::NameThisThread(tname.c_str());
                uint64_t seenEpoch = 0;
                for (;;) {
                    std::function<void(uint32_t)> fn;
                    {
                        std::unique_lock<std::mutex> lk(poolMutex);
                        cvWork.wait(lk, [&]() { return shutdown || epoch != seenEpoch; });
                        if (shutdown) return;
                        seenEpoch = epoch;
                        fn = taskFn;
                    }
                    if (fn) fn(wi);
                    {
                        std::lock_guard<std::mutex> lk(poolMutex);
                        if (activePending > 0u && --activePending == 0u) {
                            cvDone.notify_one();
                        }
                    }
                }
            });
        }
    }

    template <typename Fn>
    void DispatchWorkers(uint32_t participants, Fn&& fn) {
        if (participants <= 1u || workers.empty()) {
            fn(0u);
            return;
        }
        const uint32_t useThreads = std::min(participants, workerCount);
        {
            std::lock_guard<std::mutex> lk(poolMutex);
            taskFn = [&](uint32_t wi) {
                if (wi < useThreads) fn(wi);
            };
            activePending = static_cast<uint32_t>(workers.size());
            ++epoch;
        }
        cvWork.notify_all();
        fn(0u);
        {
            std::unique_lock<std::mutex> lk(poolMutex);
            cvDone.wait(lk, [&]() { return activePending == 0u; });
            taskFn = nullptr;
        }
    }

    ~BurstScratch() {
        if (!workers.empty()) {
            {
                std::lock_guard<std::mutex> lk(poolMutex);
                shutdown = true;
                ++epoch;
            }
            cvWork.notify_all();
            for (auto& w : workers) {
                if (w.joinable()) w.join();
            }
            workers.clear();
        }
    }
};

namespace {
struct R1GeoTrackEnt {
    uint32_t basePage = 0;
    uint32_t pageCount = 0;
    std::vector<uint32_t> epochs;
    uint64_t contentHash = 0;
    uint32_t sizeBytes = 0;
    uint32_t stride = 0;
    uint32_t maxIndex = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpuVa = 0;
    uint32_t seenMatches = 0;
    uint32_t cleanHits = 0;
    uint32_t dirtyMisses = 0;
    bool transient = false;
};
std::shared_mutex s_r1GeoMutex;
std::unordered_map<uint64_t, R1GeoTrackEnt> s_r1GeoTracker;

uint64_t MakeR1GeoKey(uint64_t a, uint64_t b, uint64_t c, uint64_t tag) {
    uint64_t h = 14695981039346656037ull ^ tag;
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
        h ^= (h >> 29);
    };
    mix(a); mix(b); mix(c);
    return h | 1ull;
}

bool HarvestWriteWatchForRenderThread(bool force = false, int64_t presentEnqNs = 0) {
    static const bool s_noWriteWatch = [] {
        const char* e = std::getenv("MCLA_NO_WRITE_WATCH");
        return e && e[0] == '1';
    }();
    if (s_noWriteWatch || !mcla::kernel::g_writeWatchActive.load(std::memory_order_acquire)) {
        return false;
    }
    // Measured (bisect_no_mt / bisect_no_ww): GetWriteWatch(WRITE_WATCH_FLAG_RESET)
    // across the 4 GiB guest window costs ~2.4 ms per call (kernel PTE walk + TLB
    // shootdown). Calling it per-draw (1,000x/1k draws = 2,477 ms) or per-burst
    // (65x/1k draws = 157 ms) destroys FPS. Harvest once per PRESENT (force=true)
    // and throttle inter-frame harvests to at most once every 8 ms.
    // PERF: the 8 ms inter-frame harvest (~2.4 ms each, ~570 ms per 2 s on the render thread) runs on its own
    // thread at the same cadence; epochs are atomics and the harvest takes its own mutex. Forced (PRESENT)
    // harvests stay inline. MCLA_NO_WW_THREAD=1 = old inline throttle.
    static const bool s_wwThread = [] {
        const char* e = std::getenv("MCLA_NO_WW_THREAD");
        return !(e && e[0] == '1');
    }();
    // PRESENT-WW-ASYNC: a forced (PRESENT) harvest only has to cover guest writes made before the
    // present was enqueued (PresentCommand::enqNs). The harvester thread wakes 1 ms after a present is
    // pushed (s_wwWantNs) and publishes the START time of each completed harvest; the render thread
    // waits only while no completed harvest started after enqNs (usually zero: queue latency > 3 ms).
    // MCLA_NO_PRESENT_WW_ASYNC=1 = old inline forced harvest (~2.9 ms on the render thread per present).
    static const bool s_presentAsync = [] {
        const char* e = std::getenv("MCLA_NO_PRESENT_WW_ASYNC");
        return !(e && e[0] == '1');
    }();
    struct Harvester {
        std::atomic<bool> stop{false};
        std::atomic<int64_t> doneStartNs{0}; // start time of the newest completed harvest
        std::mutex mu;
        std::condition_variable cv;
        std::thread t{[this] {
            int64_t lastStart = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                auto wanted = [] { return g_wwKickNs.load(std::memory_order_acquire); };  // newest present enqNs to cover
                {
                    std::unique_lock<std::mutex> lk(g_wwKickMu);
                    g_wwKickCv.wait_for(lk, std::chrono::milliseconds(1), [&] { return wanted() > lastStart || stop.load(); });
                }
                const int64_t now = NowNs();
                const int64_t want = wanted();
                if (now - lastStart < 8000000LL && want <= lastStart) continue;
                if (!mcla::kernel::g_writeWatchActive.load(std::memory_order_acquire)) continue;
                lastStart = now;
                mcla::kernel::HarvestDirtyGuestPages();
                g_wwBurstSeq.fetch_add(1, std::memory_order_release);
                { std::lock_guard<std::mutex> lk(mu); doneStartNs.store(now, std::memory_order_release); }
                cv.notify_all();
            }
        }};
        ~Harvester() { stop.store(true); if (t.joinable()) t.join(); }
    };
    if (s_wwThread && (!force || (s_presentAsync && presentEnqNs > 0))) {
        static Harvester h;
        if (!force) return true;
        KickPresentHarvest(presentEnqNs);
        std::unique_lock<std::mutex> lk(h.mu);
        if (h.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return h.doneStartNs.load(std::memory_order_acquire) >= presentEnqNs; })) {
            return true;
        }
        // shortcut: 50 ms without a covering harvest (harvester starved) -> fall through to inline.
    }
    static int64_t s_lastHarvestNs = 0;
    const int64_t nowNs = NowNs();
    if (!force && s_lastHarvestNs != 0 && (nowNs - s_lastHarvestNs) < 8000000LL) {
        return true;
    }
    s_lastHarvestNs = nowNs;
    mcla::kernel::HarvestDirtyGuestPages();
    g_wwBurstSeq.fetch_add(1, std::memory_order_release);
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS ResolveOrPromoteR1Geo(
    D3D12Backend& backend, uint64_t key, uint64_t contentHash,
    const void* data, uint32_t sizeBytes, uint32_t stride,
    D3D12_RESOURCE_STATES targetState,
    uint32_t basePage, uint32_t pageCount, const std::vector<uint32_t>& epochSnap,
    uint32_t maxIndex, bool wasCleanHit) {
    if (key == 0 || (!wasCleanHit && sizeBytes == 0)) return 0;
    if (wasCleanHit) {
        std::unique_lock<std::shared_mutex> lk(s_r1GeoMutex);
        auto it = s_r1GeoTracker.find(key);
        if (it != s_r1GeoTracker.end() && !it->second.transient && it->second.gpuVa != 0 &&
            it->second.sizeBytes == sizeBytes &&
            (contentHash == 0ull || it->second.contentHash == contentHash) &&
            it->second.pageCount > 0u &&
            it->second.epochs.size() >= it->second.pageCount &&
            (epochSnap.empty() || it->second.epochs == epochSnap) &&
            mcla::kernel::GuestPageRangeMatchesEpochs(
                it->second.basePage, it->second.pageCount, it->second.epochs.data())) {
            ++it->second.cleanHits;
            return it->second.gpuVa;
        }
        return 0;
    }
    if (!data || pageCount == 0 || epochSnap.empty()) return 0;
    {
        std::unique_lock<std::shared_mutex> lk(s_r1GeoMutex);
        auto it = s_r1GeoTracker.find(key);
        if (it == s_r1GeoTracker.end()) {
            // Defer GPU promotion until a second draw confirms identical contentHash,
            // so single-frame dynamic ring buffers never waste CreateCommittedResource calls.
            if (s_r1GeoTracker.size() < 32768u) {
                R1GeoTrackEnt& ent = s_r1GeoTracker[key];
                ent.basePage = basePage;
                ent.pageCount = pageCount;
                ent.epochs = epochSnap;
                ent.contentHash = contentHash;
                ent.sizeBytes = sizeBytes;
                ent.stride = stride;
                ent.maxIndex = maxIndex;
                ent.seenMatches = 1u;
            }
            return 0;
        }
        if (it->second.transient) {
            if (it->second.contentHash == contentHash && it->second.sizeBytes == sizeBytes) {
                it->second.basePage = basePage;
                it->second.pageCount = pageCount;
                it->second.epochs = epochSnap;
                it->second.stride = stride;
                it->second.maxIndex = maxIndex;
                ++it->second.seenMatches;
                if (it->second.seenMatches < 4u) return 0;
                it->second.transient = false;
                it->second.dirtyMisses = 0u;
                if (it->second.gpuVa != 0) {
                    return it->second.gpuVa;
                }
            } else {
                it->second.contentHash = contentHash;
                it->second.sizeBytes = sizeBytes;
                it->second.stride = stride;
                it->second.basePage = basePage;
                it->second.pageCount = pageCount;
                it->second.epochs = epochSnap;
                it->second.maxIndex = maxIndex;
                it->second.seenMatches = 1u;
                return 0;
            }
        } else if (it->second.contentHash == contentHash && it->second.sizeBytes == sizeBytes) {
            it->second.basePage = basePage;
            it->second.pageCount = pageCount;
            it->second.epochs = epochSnap;
            it->second.stride = stride;
            it->second.maxIndex = maxIndex;
            it->second.dirtyMisses = 0u;
            ++it->second.seenMatches;
            if (it->second.gpuVa != 0) {
                return it->second.gpuVa;
            }
            // Fall through to promote on confirmed repeat match (seenMatches >= 2).
        } else {
            ++it->second.dirtyMisses;
            it->second.contentHash = contentHash;
            it->second.sizeBytes = sizeBytes;
            it->second.stride = stride;
            it->second.basePage = basePage;
            it->second.pageCount = pageCount;
            it->second.epochs = epochSnap;
            it->second.maxIndex = maxIndex;
            it->second.seenMatches = 1u;
            if (it->second.dirtyMisses >= 4u && it->second.cleanHits == 0u) {
                it->second.transient = true;
                it->second.gpuVa = 0;
                it->second.seenMatches = 0u;
                lk.unlock();
                backend.ReleasePersistentGeo(key);
                return 0;
            }
            return 0;
        }
    }
    D3D12_GPU_VIRTUAL_ADDRESS gpuVa = 0;
    std::vector<uint64_t> evictedKeys;
    if (!backend.UploadPersistentGeo(key, contentHash, data, sizeBytes, targetState, gpuVa, &evictedKeys) ||
        gpuVa == 0) {
        return 0;
    }
    std::unique_lock<std::shared_mutex> lk(s_r1GeoMutex);
    for (uint64_t ek : evictedKeys) {
        s_r1GeoTracker.erase(ek);
    }
    R1GeoTrackEnt& ent = s_r1GeoTracker[key];
    ent.basePage = basePage;
    ent.pageCount = pageCount;
    ent.epochs = epochSnap;
    ent.contentHash = contentHash;
    ent.sizeBytes = sizeBytes;
    ent.stride = stride;
    ent.maxIndex = maxIndex;
    ent.transient = false;
    ent.dirtyMisses = 0u;
    ent.gpuVa = gpuVa;
    return gpuVa;
}

struct ClearShaderCacheEnt {
    std::vector<uint32_t> vsW;
    std::vector<uint32_t> psW;
    uint32_t psPresent = 0;
    uint64_t vsHash = 0;
    uint64_t psHash = 0;
    mcla::renderer::ClearShaderClassification cls{};
    std::vector<mcla::renderer::VertexInputRef> vsRefs;
};
std::shared_mutex s_clearPlanMutex;
std::unordered_map<uint64_t, std::shared_ptr<const ClearShaderCacheEnt>> s_clearPlanCache;

inline float ReadBeFloatConst(const uint8_t* bank, uint32_t reg, uint32_t comp) {
    if (!bank || reg >= 256u || comp >= 4u) return 0.0f;
    const uint8_t* p = bank + static_cast<size_t>(reg) * 16u + static_cast<size_t>(comp) * 4u;
    const uint32_t u = (static_cast<uint32_t>(p[0]) << 24) |
                       (static_cast<uint32_t>(p[1]) << 16) |
                       (static_cast<uint32_t>(p[2]) << 8) |
                       static_cast<uint32_t>(p[3]);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline bool FindClearAttrOffsetBytes(const std::vector<mcla::renderer::VertexInputRef>& vsRefs,
                                     const mcla::native::DrawCapturedCommand& dc,
                                     uint8_t usage, uint8_t usageIndex,
                                     uint32_t& outOffsetBytes, uint32_t& outDxgi) {
    if (vsRefs.empty()) {
        if (usage == 0u && usageIndex == 0u && dc.vbElementCount > 0u) {
            outOffsetBytes = dc.vbElements[0].offsetBytes;
            outDxgi = dc.vbElements[0].fmt.dxgi;
            return outDxgi == 2u || outDxgi == 6u || outDxgi == 16u || outDxgi == 41u;
        }
        return false;
    }
    for (const auto& r : vsRefs) {
        if (r.usage == usage && r.usageIndex == usageIndex) {
            const uint32_t wantVf = r.vertexFormat & 0x3Fu;
            for (uint32_t ae = 0u; ae < dc.vbElementCount && ae < mcla::native::kCpVertexElementMax; ++ae) {
                const auto& el = dc.vbElements[ae];
                if ((el.fmt.xenos & 0x3Fu) == wantVf) {
                    outOffsetBytes = (r.offset >= 0) ? (static_cast<uint32_t>(r.offset) * 4u) : el.offsetBytes;
                    outDxgi = el.fmt.dxgi;
                    return outDxgi == 2u || outDxgi == 6u || outDxgi == 16u || outDxgi == 41u;
                }
            }
        }
    }
    return false;
}

inline uint32_t DxgiFloatCompCount(uint32_t dxgi) {
    switch (dxgi) {
    case 2u:  return 4u;  // R32G32B32A32_FLOAT
    case 6u:  return 3u;  // R32G32B32_FLOAT
    case 16u: return 2u;  // R32G32_FLOAT
    case 41u: return 1u;  // R32_FLOAT
    default:  return 0u;
    }
}

inline bool EvalClearChannel(const mcla::renderer::ClearChannelExpr& expr,
                             const uint8_t* vsBank, const uint8_t* psBank,
                             const uint8_t* vbBytes, uint32_t vbBytesSize,
                             uint32_t vbStride, uint32_t vertCount,
                             const std::vector<mcla::renderer::VertexInputRef>& vsRefs,
                             const mcla::native::DrawCapturedCommand& dc,
                             float& outVal) {
    using mcla::renderer::ClearChannelSource;
    float v = 0.0f;
    switch (expr.source) {
    case ClearChannelSource::Literal:
        v = expr.literal;
        break;
    case ClearChannelSource::PsConstant:
        v = ReadBeFloatConst(psBank, expr.constReg, expr.constComp) * expr.scale + expr.bias;
        break;
    case ClearChannelSource::VsConstant:
        v = ReadBeFloatConst(vsBank, expr.constReg, expr.constComp) * expr.scale + expr.bias;
        break;
    case ClearChannelSource::VertexAttr: {
        uint32_t offB = 0, dxgi = 0;
        if (!FindClearAttrOffsetBytes(vsRefs, dc, expr.attrUsage, expr.attrUsageIndex, offB, dxgi)) {
            return false;
        }
        const uint32_t comps = DxgiFloatCompCount(dxgi);
        if (expr.attrComp >= comps || offB + static_cast<uint32_t>(expr.attrComp + 1u) * 4u > vbStride) {
            return false;
        }
        float first = 0.0f;
        for (uint32_t vi = 0u; vi < vertCount; ++vi) {
            const size_t byteOff = static_cast<size_t>(vi) * vbStride + offB + static_cast<size_t>(expr.attrComp) * 4u;
            if (byteOff + 4u > vbBytesSize) return false;
            float cur = 0.0f;
            std::memcpy(&cur, vbBytes + byteOff, 4u);
            if (!std::isfinite(cur)) return false;
            if (vi == 0u) first = cur;
            else if (std::fabs(cur - first) > 1e-5f) return false;
        }
        v = first * expr.scale + expr.bias;
        break;
    }
    default:
        return false;
    }
    if (!std::isfinite(v)) return false;
    if (expr.saturate) v = std::clamp(v, 0.0f, 1.0f);
    outVal = v;
    return true;
}
}  // namespace

RenderThread g_renderThread;
CommandQueue g_commandQueue;

// ---------------------------------------------------------------------------
// MCLA_RENDER_2STAGE: two-stage render pipeline (prep thread A -> record thread B).
//
// A ("render-prep") takes g_commandQueue in order with peek_next() (in place, no 10 KB copy, nothing popped)
// and, per command, runs AdmitCommand (shed policy) and then, for DRAW_CAPTURED, the write-watch harvest +
// PrepareDrawStage1: layout, texture-set resolve/decode/untile/memo, constant-bank build + hash, shader
// translation, PSO key derivation + PipelineCache lookup/compile, viewport/scissor/state derivation. The
// result lands in a slot of an SPSC ring (PreparedDraw + DrawStage1Meta + the queue pointer).
// B (the existing render thread, owner of the device and every command list) consumes slots in order:
// draws -> RecordPreparedDraw (R1 geometry promote, texture bind/upload, constants, PSO+state, draw,
// accounting), non-draws -> processCommand, then pop_done() so the queue frees slots oldest-first.
//
// ORDER: there is one stream. Every non-draw (PRESENT, RESOLVE_GUEST, CLEAR_GUEST, QUERY_OP, RESIZE,
// INIT_D3D12, markers) is published into the ring like a draw, so B records all earlier draws before it.
// A non-draw whose effects stage 1 can observe (RESOLVE_GUEST, QUERY_OP, RESIZE, INIT_D3D12, unknown) makes
// A BARRIER (wait until B has finished it) before preparing the next draw; the rest only touch B-owned
// state and flow through without a barrier (list in PrepThreadMain). Guest PRESENT: A itself waits for the
// forced write-watch harvest covering it, so later draws see the same epochs as in the serial path.
//
// Shared state touched by stage 1, and the decision per item:
//  - texture data: stage 1 only reads guest memory into prep-owned buffers / the untile cache (its own
//    shared_mutex); every GPU upload, the backend texture cache, descriptor heaps, SRV tables and the
//    resolved-texture tables (m_resolved*, GetResolvedTexture) are touched only by B  -> stays on B.
//  - write-watch: the per-draw harvest runs on A (stage 1 consumes the epochs); the forced PRESENT
//    harvest runs on B while A is parked in the PRESENT barrier -> never concurrent.
//  - render-scale resolve bases (XuvRsMask), occlusion-query set, width/height, presented-frame count:
//    written only by non-draws, which A barriers on -> snapshot-by-ordering, no lock needed.
//  - R1 geometry tracker: shared_mutex already (A reads, B promotes); a draw whose buffer B has not yet
//    promoted just takes the copy path, as in MCLA_MT_PREP.
//  - PipelineCache: internally locked (map/queue/inflight mutexes); A compiles/looks up, B only binds.
//  - shader-translation / PSO / vertex-layout memo caches: stage-1-local statics or thread_local -> owned
//    by A alone (one thread, so they stay warm, unlike MT_PREP's cold per-worker caches).
//  - PreparedDraw desc pointers: FinalizeDescPointers points every desc buffer at the slot's own storage,
//    so a slot is self-contained until B resets it.
//  - live CP register reads in stage 1 (SQ_PROGRAM_CNTL, RT info, native constants by frameId) were already
//    racing the guest producer in the serial path; A reads them earlier, i.e. closer to capture.
//  - perf stages: per-thread banks (tl_perfBank), A prints as prepA[...] in PERF-STAGES.
// Off-switch / opt-in: MCLA_RENDER_2STAGE (see threadMain). Needs the queue ring (MCLA_NO_QUEUE_RING unset).
// ---------------------------------------------------------------------------
struct RenderThread::TwoStageRing {
    static constexpr uint64_t kSlots = 128;
    enum Kind : uint8_t { kStop, kSkip, kDraw, kCmd };
    struct Slot {
        RenderCommand* cmd = nullptr;
        Kind kind = kStop;
        D3D12Backend::PreparedDraw prep;
        DrawStage1Meta meta;
    };
    std::unique_ptr<Slot[]> slots{new Slot[kSlots]};
    struct alignas(64) Seq {
        std::atomic<uint64_t> v{0};
        std::atomic<bool> sleeping{false};
    };
    Seq head;  // slots published by A
    Seq tail;  // slots finished by B
    // Spin briefly, then sleep on the address; Publish only notifies when the waiter declared it sleeps.
    static void WaitAtLeast(Seq& s, uint64_t want) {
        for (int i = 0; i < 512; ++i) {
            if (s.v.load(std::memory_order_acquire) >= want) return;
            _mm_pause();
        }
        for (;;) {
            s.sleeping.store(true, std::memory_order_seq_cst);
            const uint64_t cur = s.v.load(std::memory_order_seq_cst);
            if (cur >= want) break;
            s.v.wait(cur, std::memory_order_acquire);
        }
        s.sleeping.store(false, std::memory_order_relaxed);
    }
    static void Publish(Seq& s, uint64_t val) {
        s.v.store(val, std::memory_order_seq_cst);
        if (s.sleeping.load(std::memory_order_seq_cst)) s.v.notify_one();
    }
};

RenderThread::RenderThread() : running_(false), shouldStop_(false) {}

RenderThread::~RenderThread() {
    stop();
}

void RenderThread::SetWindowParams(void* hwnd, uint32_t width, uint32_t height) {
    pendingHwnd_ = hwnd;
    pendingWidth_ = width;
    pendingHeight_ = height;
}

void RenderThread::start() {
    if (running_) return;

    running_ = true;
    shouldStop_ = false;
    MCLA_LOG_INFO("RenderThread: started");
    thread_ = std::thread(&RenderThread::threadMain, this);
}

void RenderThread::stop() {
    if (!running_) return;

    shouldStop_ = true;
    g_commandQueue.shutdown();

    if (heartbeat_.joinable()) {
        heartbeat_.join();
    }
    if (thread_.joinable()) {
        thread_.join();
    }

    if (d3d12Initialized_) {
        backend_.GetPipelineCache().StopWorker();
        backend_.Shutdown();
        d3d12Initialized_ = false;
    }

    running_ = false;
    MCLA_LOG_INFO("RenderThread: stopped");
}

// Queue-depth monitoring, QUEUE-SHED logging and the consumer-side shed of sheddable commands.
// false = drop this command. Runs on whichever thread takes commands off g_commandQueue.
bool RenderThread::AdmitCommand(const RenderCommand& cmd) {
    // Queue depth monitoring
    const size_t qDepth = g_commandQueue.size();

    // F-610: the producer no longer blocks, so everything it discarded has
    // to be visible here or a shed frame is indistinguishable from a guest
    // that stopped drawing. Logged on change, then bounded (first 3, then
    // every 200th change) so a long stall cannot flood the soak.
    {
        const ShedCounters shed = g_commandQueue.Shed();
        const uint64_t shedTotal = shed.depth();
        static uint64_t lastShedTotal = 0;
        if (shedTotal != lastShedTotal) {
            lastShedTotal = shedTotal;
            static std::atomic<uint64_t> shedLines{0};
            const uint64_t n = shedLines.fetch_add(1) + 1;
            if (n <= 3 || (n % 200) == 0)
                MCLA_LOG_WARN("QUEUE-SHED total={} refused={} evictSheddable={} "
                              "evictDurable={} consumerDrop={} depth={}",
                              shedTotal, shed.refusedSheddable,
                              shed.evictedSheddable, shed.evictedDurable,
                              shed.consumerDropped, qDepth);
        }
    }

    if (qDepth >= kQueueDepthDrop) {
        // Queue dangerously deep — skip non-critical commands. F-610: the
        // list is now IsSheddable() from render_queue.h, the same six types
        // as before PLUS the two it was missing: DRAW_MARKER and the
        // heartbeat PRESENT. Those two are why a marker/heartbeat flood
        // could keep a stalled queue full while the consumer was already
        // refusing everything else. INIT, RESIZE, DRAW_CAPTURED and guest
        // PRESENT are never dropped.
        if (IsSheddable(cmd)) {
            static std::atomic<uint32_t> dropCount{0};
            const uint32_t n = dropCount.fetch_add(1) + 1;
            if (n <= 5 || (n % 100) == 0)
                MCLA_LOG_WARN("RenderThread: DROPPED cmd type={} queueDepth={} (total dropped={})",
                              static_cast<int>(cmd.type), qDepth, n);
            // F-610: fold consumer-side drops into the same QUEUE-SHED
            // accounting as the producer's, so one number answers "how much
            // work did the pipeline throw away" no matter which side
            // decided. Read-modify-write races between the two writers can
            // lose a count, which is acceptable for a lossy tally.
            g_commandQueue.NoteConsumerDrop();
            return false;
        }
    } else if (qDepth >= kQueueDepthWarning) {
        static std::atomic<uint32_t> warnCount{0};
        const uint32_t n = warnCount.fetch_add(1) + 1;
        if (n <= 5 || (n % 100) == 0)
            MCLA_LOG_WARN("RenderThread: queue depth {} (warning threshold={})",
                          qDepth, kQueueDepthWarning);
    }
    return true;
}

void RenderThread::threadMain() {
    t_isRenderThread = true;
    mcla::diag::NameThisThread("render"); mcla::diag::StartDiagThreads();
    MCLA_LOG_INFO("RenderThread: entering command loop");

    // TEMP-SAMPLER (MCLA_TEMP_SAMPLER=1): 1 kHz RIP sampler of this thread; top RVAs every 10 s. Resolve with tools/host_rva_resolve.py.
    std::atomic<bool> samplerStop{false};
    std::thread samplerThread;
    if (const char* e = std::getenv("MCLA_TEMP_SAMPLER"); e && e[0] == '1') {
        HANDLE h = thread_.native_handle();
        samplerThread = std::thread([h, &samplerStop] {
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            std::unordered_map<uint64_t, uint32_t> hist; std::mutex modMu; std::unordered_map<uint32_t, std::string> modNames;
            uint32_t total = 0;
            auto last = std::chrono::steady_clock::now();
            while (!samplerStop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::microseconds(900));
                if (SuspendThread(h) == static_cast<DWORD>(-1)) continue;
                CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &ctx)) {
                    HMODULE hm = nullptr; uint64_t key = static_cast<uint64_t>(ctx.Rip) - base;
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(ctx.Rip), &hm) && reinterpret_cast<uintptr_t>(hm) != base) {
                        wchar_t nm[MAX_PATH] = {}; GetModuleFileNameW(hm, nm, MAX_PATH);
                        std::wstring w(nm); const size_t sl = w.find_last_of(L"\\");
                        uint32_t hsh = 0; for (wchar_t c : w.substr(sl == std::wstring::npos ? 0 : sl + 1)) hsh = hsh * 131u + static_cast<uint32_t>(c);
                        { std::lock_guard<std::mutex> lk(modMu); modNames[hsh] = std::string(w.begin() + (sl == std::wstring::npos ? 0 : sl + 1), w.end()); }
                        key = (0xF0000000ull << 32) | (static_cast<uint64_t>(hsh) << 24 >> 0 & 0x0FFFFFFFFF000000ull) | ((ctx.Rip - reinterpret_cast<uintptr_t>(hm)) & 0xFFFFFF);
                    }
                    if ((key >> 60) == 0xF) {  // outside mcla.exe: also bucket the nearest return address into the exe found on the stack
                        uint64_t stk[400]; SIZE_T got = 0;
                        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(ctx.Rsp), stk, sizeof stk, &got);
                        const uint64_t imgEnd = base + 0x3B15000ull;
                        for (size_t k = 0; k < got / 8; ++k) {
                            if (stk[k] > base && stk[k] < imgEnd) { ++hist[(0xEull << 60) | (stk[k] - base)]; break; }
                        }
                    }
                    if ((key >> 60) == 0) {  // exe sample: also bucket (rip, return address at [rsp]) so memcpy/memset callers can be named offline
                        uint64_t stk6[6] = {}; uint64_t top = 0; SIZE_T got2 = 0;
                        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(ctx.Rsp), stk6, sizeof stk6, &got2);
                        const uintptr_t imgEnd2 = base + 0x3B15000ull;
                        for (size_t k = 0; k < got2 / 8; ++k) if (stk6[k] > base && stk6[k] < imgEnd2) { top = stk6[k]; break; }
                        if (top != 0) ++hist[(0xDull << 60) | ((key & 0x0FFFFFFFull) << 28) | ((top - base) & 0x0FFFFFFFull)];
                    }
                    ++hist[key]; ++total; }
                ResumeThread(h);
                const auto now = std::chrono::steady_clock::now();
                if (now - last > std::chrono::seconds(10)) {
                    last = now;
                    std::vector<std::pair<uint32_t, uint64_t>> v;
                    for (auto& kv : hist) v.emplace_back(kv.second, kv.first);
                    std::sort(v.begin(), v.end(), std::greater<>());
                    MCLA_LOG_ERROR("TEMP-SAMPLER total={} distinct={}", total, hist.size());
                    for (size_t i = 0; i < v.size() && i < 1600; ++i)
                        { if ((v[i].second >> 60) == 0xF) { const uint32_t hsh = static_cast<uint32_t>((v[i].second >> 24) & 0xFFFFFFFFull); const auto it = modNames.find(hsh);
                            MCLA_LOG_ERROR("TEMP-SAMPLER-MOD {} {:X} {}", it != modNames.end() ? it->second : std::string("?"), v[i].second & 0xFFFFFF, v[i].first); }
                          else if ((v[i].second >> 60) == 0xD) MCLA_LOG_ERROR("TEMP-SAMPLER-PAIR {:X} {:X} {}", (v[i].second >> 28) & 0x0FFFFFFFull, v[i].second & 0x0FFFFFFFull, v[i].first);
                          else if ((v[i].second >> 60) == 0xE) MCLA_LOG_ERROR("TEMP-SAMPLER-CALLER {:X} {}", v[i].second & 0x0FFFFFFFFFFFFFFFull, v[i].first);
                          else MCLA_LOG_ERROR("TEMP-SAMPLER-RVA {:X} {}", v[i].second, v[i].first); }
                    hist.clear(); total = 0;
                }
            }
        });
    }

    if (pendingHwnd_ && !d3d12Initialized_) {
        if (backend_.Initialize(static_cast<HWND>(pendingHwnd_), pendingWidth_, pendingHeight_)) {
            d3d12Initialized_ = true;
            MCLA_LOG_INFO("RenderThread: D3D12 initialized on thread start ({}x{})",
                          pendingWidth_, pendingHeight_);
            // F-899: the guest draws its frame as a BURST and swaps once.
            // Batching must be enabled HERE (thread start), not in the
            // INIT_D3D12 command case, because D3D12 is initialized on thread
            // start and INIT_D3D12 never runs (measured: g920.log has 0
            // "INIT_D3D12" rows and 0 "D3D12-DRAW-BATCH" rows despite the call
            // sitting at render_thread.cpp:1286).
            backend_.SetBatchDrawFrames(true);
            // Start the pipeline cache async compilation worker
            backend_.GetPipelineCache().StartWorker(
                backend_.GetDevice(), backend_.GetRootSignature());
            MCLA_LOG_INFO("RenderThread: PipelineCache worker started");
            if (!burstScratch_) {
                burstScratch_ = std::make_unique<BurstScratch>();
            }
            // F-610: SYNTHETIC startup content, one shot, before any guest frame.
            // REMOVAL TRIGGER: delete this block and the heartbeat below once a
            // soak shows "PRESENT-BLIT #1 ... va=<non-zero>" — i.e. the guest's
            // own NATIVE-PRESENT produced a real blit. Until then a synthetic
            // frame is the only thing separating "window is alive" from "window
            // is black because nothing works", which is the ambiguity this
            // exists to remove. It fires once per Initialize(), so it can never
            // paint over a guest frame — the guest-content latch in the PRESENT
            // case below is what guarantees that.
            if (backend_.BeginFrame()) {
                backend_.ClearAndPresent(0.0f, 0.0f, 0.0f, 1.0f);
                backend_.AbandonFrame("startup-clear");
                MCLA_LOG_INFO("RenderThread: initial clear+present (pure black)");
            }
            // F-758: THE W6 ONE-SHOT GRADIENT IS DELETED too. It is the same
            // synthetic image as the fallback that stood at :1733, painted once at
            // startup, and F-609 forbids it for the same reason. It survived the
            // first deletion precisely because it is a DIFFERENT site: its own
            // comment said "Same removal trigger as the clear above", which was
            // never a trigger anyone evaluated. Two synthetics existed and one
            // deletion would have left the other painting.
            //
            // MEASURED in build/w713.log: `PRESENT-BLIT-SYNTH ok 1280x720
            // (gradient)`, 1 row, and it SUCCEEDS -- so it is the one frame the
            // user saw a gradient on before the fallback took over.
            //
            // WHAT REPLACES IT: nothing. The startup clear immediately above is
            // retained -- it presents a real colour the code chose deliberately and
            // says so in its own log line -- and after this the frame loop reports
            // PRESENT-SURFACE-UNREADABLE rather than painting over the guest.
            // REMOVAL TRIGGER: none.
            // F-610: heartbeat. A heartbeat PRESENT is a clear-only frame, so
            // firing one while the guest is already presenting real frames both
            // halves the guest's effective framerate and spends a vsync slot on
            // a clear. POLICY (stated, not guessed): SKIP the beat when
            //   (a) a guest (non-heartbeat) PRESENT was consumed within the last
            //       kHeartbeatGuestSuppressMs, or
            //   (b) the queue is at or past kQueueDepthWarning (the consumer is
            //       already behind; a liveness ping is not worth depth).
            // Skipping loses nothing: the beat is idempotent, and the first beat
            // after the window closes restores liveness within <=500 ms. The old
            // unconditional 30 Hz version raced a guest-presenting soak and made
            // the soak's PRESENT count measure our timer instead of the guest.
            // REMOVAL TRIGGER: kHeartbeatIntervalMs (see the constant).
            heartbeat_ = std::thread([this]() {  mcla::diag::NameThisThread("render-heartbeat");
                const auto kInterval = std::chrono::milliseconds(kHeartbeatIntervalMs);
                const int64_t kSuppressNs =
                    static_cast<int64_t>(kHeartbeatGuestSuppressMs) * 1000000;
                uint32_t ticks = 0, skipped = 0;
                while (!shouldStop_) {
                    std::this_thread::sleep_for(kInterval);
                    if (shouldStop_ || !d3d12Initialized_) continue;

                    ticks++;
                    const int64_t now = NowNs();
                    const int64_t lastGuest =
                        g_lastGuestPresentNs.load(std::memory_order_relaxed);
                    // GNH-P2: native draws trickle in at guest pace (PM4's come in
                    // CP bursts that keep depth >= kQueueDepthWarning), so a beat
                    // would ClearAndPresent over the open batch. Native mode only.
                    const int64_t lastDraw =
                        g_consumerDrawNs.load(std::memory_order_relaxed);
                    const bool guestFresh =
                        (lastGuest != 0 && (now - lastGuest) < kSuppressNs) ||
                        (mcla::gnh::GetMode() == mcla::gnh::Mode::kNative &&
                         lastDraw != 0 && (now - lastDraw) < kSuppressNs);
                    const size_t depth = g_commandQueue.size();
                    if (guestFresh || depth >= kQueueDepthWarning) {
                        skipped++;
                        if (skipped <= 3 || (skipped % 200) == 0)
                            MCLA_LOG_INFO("RenderThread: HB skip #{} of {} ticks "
                                          "guestFresh={} depth={}",
                                          skipped, ticks,
                                          static_cast<int>(guestFresh), depth);
                        // F-902 STALL-WATCHDOG report (inside the already-gated
                        // skip branch, so no new log volume): if the consumer
                        // has not stamped a draw in > 10 s while commands are
                        // pending, it is STUCK (not idle, not slow).
                        {
                            static uint64_t s_lastWatchSeq = 0;
                            static int64_t s_lastWatchNs = 0;
                            static uint32_t s_stallRows = 0;
                            const uint32_t cseq =
                                g_consumerDrawSeq.load(std::memory_order_relaxed);
                            const int64_t clast =
                                g_consumerDrawNs.load(std::memory_order_relaxed);
                            const int64_t ageMs =
                                (clast != 0) ? (NowNs() - clast) / 1000000 : -1;
                            if (cseq != 0 && (cseq != s_lastWatchSeq || clast != s_lastWatchNs)) {
                                s_lastWatchSeq = cseq;
                                s_lastWatchNs = clast;
                                s_stallRows = 0;
                            } else if (depth > 0 && ageMs > 10000) {
                                if (s_stallRows < 6 || (s_stallRows % 50) == 0) {
                                    MCLA_LOG_WARN("STALL-WATCH consumerSeq={} "
                                                  "ageMs={} queueDepth={} "
                                                  "stage={} -- consumer has not "
                                                  "advanced while work is pending",
                                                  cseq,
                                                  static_cast<long long>(ageMs),
                                                  depth,
                                                  g_consumerStage.load(
                                                      std::memory_order_relaxed));
                                }
                                ++s_stallRows;
                            }
                        }
                        continue;
                    }
                    // push() is non-blocking and refuses sheddable commands past
                    // CommandQueue::kShedDepth, so a stalled backend can neither
                    // block this thread nor grow the backlog from here.
                    RenderCommand cmd;
                    cmd.type = RenderCommand::PRESENT;
                    auto& p = cmd.data.emplace<PresentCommand>();
                    p.frameNumber = 0;
                    p.obj = 0;
                    p.swapInfo = 0;
                    p.surfaceVA = 0;
                    g_commandQueue.push(std::move(cmd));
                }
            });
        } else {
            MCLA_LOG_ERROR("RenderThread: D3D12 initialization failed on thread start");
        }
    }

    // MCLA_RENDER_2STAGE=1: prep thread + record thread (TwoStageRing). Needs the queue's ring storage.
    static const bool s_twoStage = [] {
        // Default ON (2026-11-24: lean producer + 2STAGE s0x9 29.2 vs serial 26.2 fps, 14/14 runs clean). MCLA_NO_RENDER_2STAGE=1 = serial.
        const char* off = std::getenv("MCLA_NO_RENDER_2STAGE");
        const char* e = std::getenv("MCLA_RENDER_2STAGE");
        const bool want = !(off && off[0] == '1') && !(e && e[0] == '0');
        return want && CommandQueue::PeekSupported();
    }();
    if (s_twoStage && !shouldStop_) RunTwoStage();
    while (!s_twoStage && !shouldStop_) {
        STALL_STAGE("idle-wait");
        static const bool s_fpsOn = [] { const char* e = std::getenv("MCLA_TEMP_FPS"); return e && e[0] == '1'; }();
        static int64_t s_fpsT0 = 0, s_fpsIdle = 0, s_fpsPop0 = 0; static uint64_t s_fpsF0 = 0, s_fpsD0 = 0;
        if (s_fpsOn) s_fpsPop0 = NowNs();
        RenderCommand* const cmdp = g_commandQueue.pop_ref();   // in place: no 10 KB copy out of the queue
        if (!cmdp) {
            break;
        }
        STALL_STAGE("stage2");  // TEMP: split pop_ref wait from the loop prefix
        struct PopDone { bool on = true; void release() { if (on) { on = false; g_commandQueue.pop_done(); } } ~PopDone() { release(); } } popDone;
        RenderCommand& cmd = *cmdp;
        if (s_fpsOn) {  // TEMP-FPS (opt-in): error-level so it prints at the default log level; idle = time blocked in pop()
            const int64_t now = NowNs(); s_fpsIdle += now - s_fpsPop0;
            if (s_fpsT0 == 0) s_fpsT0 = now;
            if (now - s_fpsT0 >= 2000000000LL) {
                const double dt = static_cast<double>(now - s_fpsT0) * 1e-9;
                const uint64_t pf = backend_.PresentedFrames(), dr = backend_.Stats().drawsIssued;
                MCLA_LOG_ERROR("TEMP-FPS fps={:.1f} draws/s={:.0f} renderIdle={:.0f}% qDepth={}", static_cast<double>(pf - s_fpsF0) / dt,
                               static_cast<double>(dr - s_fpsD0) / dt, 100.0 * static_cast<double>(s_fpsIdle) / static_cast<double>(now - s_fpsT0), g_commandQueue.size());
                s_fpsT0 = now; s_fpsIdle = 0; s_fpsF0 = pf; s_fpsD0 = dr;
            }
        }

        if (!AdmitCommand(cmd)) continue;

        // Measured (w2_perf_auto4_nomt vs w2_perf_auto4_default): single-thread
        // in-place pop_ref() + L1-hot s_serialPrep ran 3.2x faster per 1k draws
        // (19.0 ms vs 61.0 ms wall, 23.5 vs 22.5 steady FPS, 3.1x draw throughput)
        // by avoiding 10 KB RenderCommand burst copies, mutex contention, and
        // mid-frame command-list flushes. Keep MT burst prep opt-in via MCLA_MT_PREP=1.
        static const bool s_mtPrepOptIn = [] {
            const char* e = std::getenv("MCLA_MT_PREP");
            const char* off = std::getenv("MCLA_NO_MT_PREP");
            return e && e[0] == '1' && !(off && off[0] == '1');
        }();
        if (s_mtPrepOptIn && d3d12Initialized_ && cmd.type == RenderCommand::DRAW_CAPTURED &&
            backend_.MtRecorderCount() > 1u) {
            if (!burstScratch_) {
                burstScratch_ = std::make_unique<BurstScratch>();
            }
            burstScratch_->cmds.clear();
            burstScratch_->cmds.push_back(std::move(cmd));
            popDone.release();   // front must be the NEXT command before the burst drains
            g_commandQueue.pop_draw_burst(burstScratch_->cmds, 256);
            ExecuteDrawBurst(burstScratch_->cmds);
            continue;
        }

        if (cmd.type != RenderCommand::DRAW_CAPTURED) {
            STALL_STAGE("other-cmd");
            // CMDCOUNT (gated, log-only): census of non-draw commands to find what
            // dominates the 7.3ms/1k-draw "other-cmd" stage.
            static const bool s_cmdCount = [] { const char* e = std::getenv("MCLA_CMDCOUNT"); return e && e[0] == '1'; }();
            if (s_cmdCount) {
                static std::atomic<uint64_t> cc[16] = {};
                const uint64_t n = cc[cmd.type & 15u].fetch_add(1) + 1;
                if (n == 1 || (n % 1000) == 0)
                    MCLA_LOG_WARN("CMDCOUNT type={} n={}", unsigned(cmd.type), n);
            }
        }
        if (g_tempPerfStages && cmd.type != RenderCommand::DRAW_CAPTURED) {
            // PERF-STAGES ocT: other-cmd wall time per command type (which non-draw costs ~100 us).
            const unsigned ty = unsigned(cmd.type) & 15u;
            const int64_t t0 = PerfNowNs();
            processCommand(cmd);
            s_perfOcNs[ty].fetch_add(PerfNowNs() - t0, std::memory_order_relaxed);
            s_perfOcN[ty].fetch_add(1, std::memory_order_relaxed);
        } else {
            processCommand(cmd);
        }
    }

    samplerStop = true;
    if (samplerThread.joinable()) samplerThread.join();
    burstScratch_.reset();

    if (d3d12Initialized_) {
        backend_.WaitForGpu();
    }

    MCLA_LOG_INFO("RenderThread: exiting command loop");
}

// ---------------------------------------------------------------------------
// Shader container reader: reads a Xenos .fxc container from guest memory.
// Returns the raw container bytes. Returns empty on failure.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> ReadShaderContainer(mcla::kernel::GuestMemoryHeap& mem,
                                                 uint32_t guestAddr) {
    if (guestAddr == 0) return {};

    uint32_t header[9] = {};
    if (!mem.IsValid(guestAddr, 36) ||
        !mem.ReadBytes(guestAddr, header, 36)) {
        return {};
    }

    // Validate container header: flags field must have (flags & 0xFFFFFF00) == 0x102A1100
    const uint32_t flags = mcla::renderer::AssembleBE32(
        reinterpret_cast<const uint8_t*>(header));
    if ((flags & 0xFFFFFF00) != 0x102A1100) {
        return {};
    }

    const uint32_t vsize = mcla::renderer::AssembleBE32(
        reinterpret_cast<const uint8_t*>(&header[1]));
    const uint32_t psize = mcla::renderer::AssembleBE32(
        reinterpret_cast<const uint8_t*>(header + 2));
    if (vsize + psize < vsize || vsize + psize < psize) {
        return {};
    }

    const uint32_t totalSize = 36 + vsize + psize;  // header(36) + virtual + physical
    std::vector<uint8_t> container(totalSize);
    // Copy the already-read header
    std::memcpy(container.data(), header, 36);
    // Read the rest
    if (!mem.ReadBytes(guestAddr + 36, container.data() + 36, totalSize - 36)) {
        return {};
    }

    return container;
}

// ---------------------------------------------------------------------------
// PSO lookup stats
// ---------------------------------------------------------------------------
static std::atomic<uint32_t> s_psoCacheHits{0};
static std::atomic<uint32_t> s_psoCacheMisses{0};
static StatCtr<uint32_t> s_psoTranslateFailures{0};

// ---------------------------------------------------------------------------
// F-611: Xenos index-stream transcode.
//
// A PM4 index buffer is a big-endian guest byte stream that the hardware
// reorders per VGT_DMA_SIZE.swap_mode (registers.h:301-309, xenos::Endian at
// xenos.h:193-198), and D3D12 accepts only R16_UINT / R32_UINT. So the bytes
// come out of the checked ReadBytes helper and are assembled BY HAND, element
// by element, in the order the swap mode dictates. There is deliberately no
// reinterpret_cast over guest memory and no assumption about host endianness:
// the only byte order in this function is the one spelled out in b0..b3, and it
// is big-endian because the guest is.
//
// An unhandled swap mode is a refusal, never a guess - a coin flip on byte
// order is a silently wrong draw, which is the failure mode this whole file is
// being rebuilt to avoid.
//
// Buffers are reused across draws: the render thread is the only writer and the
// draw path must not allocate per frame.
// ---------------------------------------------------------------------------
// Hot-path helpers (render-thread perf): 8-lane FNV-style content hash (one dependent multiply chain per
// lane instead of one for the whole texture) and SSE2 vertex byte swaps. Hash VALUES differ from the old
// single-chain FNV; they are only ever compared with other values from this function, so nothing else cares.
static inline uint64_t HashBlock8(uint64_t seed, const uint8_t* d, size_t n) {
    constexpr uint64_t P = 0x100000001b3ull;
    uint64_t h[8];
    for (uint32_t k = 0; k < 8; ++k) h[k] = seed + 0x9E3779B97F4A7C15ull * (k + 1);
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        uint64_t w[8];
        std::memcpy(w, d + i, 64);
        for (uint32_t k = 0; k < 8; ++k) { h[k] = (h[k] ^ w[k]) * P; h[k] ^= h[k] >> 29; }
    }
    uint64_t r = seed ^ static_cast<uint64_t>(n);
    for (uint32_t k = 0; k < 8; ++k) { r = (r ^ h[k]) * P; r ^= r >> 29; }
    for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, d + i, 8); r = (r ^ w) * P; r ^= r >> 29; }
    for (; i < n; ++i) r = (r ^ d[i]) * P;
    return r;
}
// Same hash over LIVE guest memory (no staging copy); a faulting page reports failure instead of crashing.
// Pedestrians/drivers/player car draw RGB-only (mask 7, blend off) and leave scene alpha untouched; the host scene RT is
// never colour-cleared in gameplay, so that alpha is 0 and post pass 5F7073CB... darkens those pixels to black. Measured
// 2026-11-17 (reg_a1/oa1b: alpha forced to 1 -> pedestrians correctly coloured, minimap/glass unchanged). These draws
// write alpha = 1. MITIGATION: on hardware the EDRAM alpha under them holds earlier writes we do not model.
// MCLA_NO_OPAQUE_ALPHA1=1 restores the old behaviour.
static bool OpaqueAlpha1On() { static const bool on = [] { const char* e = std::getenv("MCLA_NO_OPAQUE_ALPHA1"); return !(e && e[0] == '1'); }(); return on; }
// CHARBLEND (log-only): dedup census of colour-writing draws' blend states, to name the
// in-engine cutscene character draws instead of guessing their mask/blend configuration.
static const bool s_charBlend = [] { const char* e = std::getenv("MCLA_CHARBLEND"); return e && e[0] == '1'; }();
// CHARPROBE (log-only, gated): name the skinned 3D draws in the in-engine cutscene so the
// torn-face draw can be identified by its VS/PS hashes instead of guessed.
static const bool s_charProbe = [] { const char* e = std::getenv("MCLA_CHARPROBE"); return e && e[0] == '1'; }();
static bool CharProbeSkinned(const std::string& hlsl) {
    int n = 0;
    n += hlsl.find(".xxxx, c") != std::string::npos ? 1 : 0;
    n += hlsl.find(".yyyy, c") != std::string::npos ? 1 : 0;
    n += hlsl.find(".zzzz, c") != std::string::npos ? 1 : 0;
    n += hlsl.find(".wwww, c") != std::string::npos ? 1 : 0;
    return n >= 3;
}
static bool HashGuestRangeSeh(uint64_t seed, const uint8_t* p, size_t n, uint64_t* out) {
    __try { *out = HashBlock8(seed, p, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// In-place endian swaps over whole dwords (mode 1 = swap bytes in each halfword, 2 = swap halfwords, 3 = both = bswap32).
static void VbSwapDwords(uint8_t* p, uint32_t dwords, int mode) {
    uint32_t i = 0;
    for (; i + 4 <= dwords; i += 4) {
        __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + size_t(i) * 4u));
        if (mode & 1) x = _mm_or_si128(_mm_slli_epi16(x, 8), _mm_srli_epi16(x, 8));
        if (mode & 2) x = _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + size_t(i) * 4u), x);
    }
    for (; i < dwords; ++i) {
        uint32_t w; std::memcpy(&w, p + size_t(i) * 4u, 4);
        if (mode & 1) w = ((w << 8) & 0xFF00FF00u) | ((w >> 8) & 0x00FF00FFu);
        if (mode & 2) w = (w >> 16) | (w << 16);
        std::memcpy(p + size_t(i) * 4u, &w, 4);
    }
}

struct IndexTranscode {
    std::vector<uint16_t> narrow;
    std::vector<uint32_t> wide;
    bool isWide = false;
    uint32_t count = 0;
    uint32_t maxIndex = 0;
};

// Byte stride of one element in the RAW guest stream, and whether the value
// needs 32 bits. False for a swap mode with no D3D12 element equivalent.
// Exposed separately because the caller must size-check num_indices*stride
// against ibSize BEFORE reading anything.
bool XenosIndexElement(uint32_t swap, uint32_t& stride, bool& wide) {
    switch (swap) {
    case 0: stride = 2; wide = false; return true; // kNone   - 16-bit BE
    case 1: stride = 2; wide = false; return true; // k8in16  - halfword swapped
    case 2: stride = 4; wide = true;  return true; // k8in32  - word swapped
    case 3: stride = 4; wide = false; return true; // k16in32 - 16-bit in the
                                                   //           high halfword
    default: return false;
    }
}

bool TranscodeXenosIndices(const uint8_t* raw, uint32_t numIndices, uint32_t swap,
                           IndexTranscode& out) {
    uint32_t stride = 0;
    bool wide = false;
    if (!XenosIndexElement(swap, stride, wide)) return false;
    // Byte offsets of the value's bytes inside one element, most significant
    // first. This table IS the endianness model.
    uint32_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    switch (swap) {
    // The GPU reads the index words little-endian and then applies the swap mode; the PPC wrote them
    // big-endian. So kNone = LE value, k8in16 / k8in32 = the BE value. These were inverted, which turned
    // index 1 into 256 (RenderDoc world.rdc: IB 0,256,512,... on a 288-vertex VB) -> shattered 3D.
    case 0: b0 = 1; b1 = 0; break;
    case 1: b0 = 0; b1 = 1; break;
    case 2: b0 = 0; b1 = 1; b2 = 2; b3 = 3; break;
    default: b0 = 2; b1 = 3; break; // swap == 3
    }
    out.isWide = wide;
    out.count = numIndices;
    out.maxIndex = 0;
    uint32_t mx = 0;
    if (wide) {
        if (out.wide.size() < numIndices) out.wide.resize(numIndices);
        uint32_t* o = out.wide.data();
        if (swap == 2) {   // BE value of the dword
            for (uint32_t i = 0; i < numIndices; ++i) { uint32_t v; std::memcpy(&v, raw + size_t(i) * 4u, 4); v = _byteswap_ulong(v); o[i] = v; mx = v > mx ? v : mx; }
        } else {
            for (uint32_t i = 0; i < numIndices; ++i) {
                const uint8_t* e = raw + static_cast<size_t>(i) * stride;
                const uint32_t v = (static_cast<uint32_t>(e[b0]) << 24) | (static_cast<uint32_t>(e[b1]) << 16) |
                                   (static_cast<uint32_t>(e[b2]) << 8) | static_cast<uint32_t>(e[b3]);
                o[i] = v; mx = v > mx ? v : mx;
            }
        }
    } else {
        if (out.narrow.size() < numIndices) out.narrow.resize(numIndices);
        uint16_t* o = out.narrow.data();
        if (swap == 0) {        // LE halfword
            for (uint32_t i = 0; i < numIndices; ++i) { uint16_t v; std::memcpy(&v, raw + size_t(i) * 2u, 2); o[i] = v; mx = v > mx ? v : mx; }
        } else if (swap == 1) { // BE halfword
            for (uint32_t i = 0; i < numIndices; ++i) { uint16_t v; std::memcpy(&v, raw + size_t(i) * 2u, 2); v = _byteswap_ushort(v); o[i] = v; mx = v > mx ? v : mx; }
        } else {
            for (uint32_t i = 0; i < numIndices; ++i) {
                const uint8_t* e = raw + static_cast<size_t>(i) * stride;
                const uint16_t v = static_cast<uint16_t>((static_cast<uint32_t>(e[b0]) << 8) | static_cast<uint32_t>(e[b1]));
                o[i] = v; mx = v > mx ? v : mx;
            }
        }
    }
    out.maxIndex = mx;
    return true;
}

// F-723: the XENOS ColorRenderTargetFormat -> DXGI_FORMAT table.
//
// THE VALUE SPACE IS SPARSE, AND THAT IS THE WHOLE POINT OF THIS FUNCTION.
// `color_format` is 4 bits wide (registers.h:745, xenos.h:409
// kColorRenderTargetFormatBits) but the architected enum assigns only 0-7, 10,
// 12, 14, 15 (.research/xenia/src/xenia/gpu/xenos.h:299-319). Codes 8, 9, 11
// and 13 are UNASSIGNED - they are not "reserved for later", they are values no
// hardware produces. So a default arm here would not be a convenience, it would
// be a fabrication: it would hand the guest a render target format the guest
// never asked for, and the PSO built from it would render into a format that
// disagrees with the RTV it is used with (pipeline_cache.cpp:1726-1729 names
// exactly that failure). There is therefore NO default arm, and the caller
// REFUSES on DXGI_FORMAT_UNKNOWN.
//
// The mapping is the reference's, not a guess. Xenia resolves a colour format to
// a host format in two steps: GetColorDrawDXGIFormat
// (d3d12_render_target_cache.cc:1719-1737) handles the formats whose DRAW
// representation differs from their RESOURCE representation, and defers
// everything else to GetColorResourceDXGIFormat (:1680-1716). Both are cited
// per code below. That two-step shape is reproduced here because collapsing it
// would pick the wrong format for six of the thirteen assigned codes.
//
// ONE DEVIATION, STATED: k_8_8_8_8_GAMMA (code 1) is conditional in the
// reference -- GetColorResourceDXGIFormat returns R16G16B16A16_UNORM or
// R8G8B8A8_UNORM depending on the runtime cvar
// `gamma_render_target_as_unorm16` (d3d12_render_target_cache.cc:1687-1689;
// the field defaults to false at d3d12_render_target_cache.h:715, and Xenia
// only ever RAISES it when the device supports R16G16B16A16_UNORM blending,
// :486). This host has no such cvar, so it takes the reference's DEFAULT arm,
// R8G8B8A8_UNORM. That is the same value the reference itself produces with
// its default configuration; it is a stated host-capability choice, not a
// guest-fact guess. The alternative - refusing every gamma target - would
// refuse the single most common render target on the hardware.
//
// RETURNS: a DXGI_FORMAT as a uint32_t, or DXGI_FORMAT_UNKNOWN (0) for every
// code with no measured mapping. 0 is the right sentinel here because the
// consumer chain treats 0 as absent by construction: pipeline_cache.cpp:841-842
// copies colorFormats[] into targetFormats[] verbatim and :1731-1733 counts
// `RTVFormats[i] != DXGI_FORMAT_UNKNOWN`, so a 0 is counted as "no render
// target" rather than silently becoming slot 0 of the PSO.
uint32_t MapXenosColorFormatToDxgi(uint32_t colorFormat) {
    switch (colorFormat) {
        // --- GetColorDrawDXGIFormat: format differs between draw and resource ---
        // d3d12_render_target_cache.cc:1722-1723
        case 4:  // k_16_16
            return DXGI_FORMAT_R16G16_SNORM;
        // :1724-1725
        case 5:  // k_16_16_16_16
            return DXGI_FORMAT_R16G16B16A16_SNORM;
        // :1726-1727
        case 6:  // k_16_16_FLOAT
            return DXGI_FORMAT_R16G16_FLOAT;
        // :1728-1729
        case 7:  // k_16_16_16_16_FLOAT
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        // :1730-1731
        case 14:  // k_32_FLOAT
            return DXGI_FORMAT_R32_FLOAT;
        // :1732-1733
        case 15:  // k_32_32_FLOAT
            return DXGI_FORMAT_R32G32_FLOAT;
        // --- GetColorResourceDXGIFormat: draw == resource ---
        // :1685-1686
        case 0:  // k_8_8_8_8
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        // :1687-1689 -- conditional in the reference on the gamma cvar; this
        // host takes the cvar's DEFAULT arm. See the note above.
        case 1:  // k_8_8_8_8_GAMMA
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        // :1690-1692 -- both codes share the arm
        case 2:   // k_2_10_10_10
        case 10:  // k_2_10_10_10_AS_10_10_10_10
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        // :1693-1695 -- both codes share the arm
        case 3:   // k_2_10_10_10_FLOAT
        case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            // 8, 9, 11, 13 (unassigned in xenos.h:299-319) and 16+ (not
            // representable in the 4-bit field). The reference asserts here
            // (d3d12_render_target_cache.cc:1713-1715); this host refuses
            // instead of crashing, because an assert in a draw loop would take
            // the process down on a guest value rather than refusing the draw.
            return DXGI_FORMAT_UNKNOWN;
    }
}

// F-611: DRAW_CAPTURED gate census. One counter per named refusal, so a soak
// reports WHICH piece is missing rather than only that something was. All of
// them are logged bounded (n <= 8 || n % 200 == 0).
StatCtr<uint32_t> s_cpRefuseSrc{0};
StatCtr<uint32_t> s_cpRefuseIbEmpty{0};
StatCtr<uint32_t> s_cpRefuseIbWrap{0};
StatCtr<uint32_t> s_cpRefuseNumIdx{0};
StatCtr<uint32_t> s_cpRefuseSwap{0};
StatCtr<uint32_t> s_cpRefuseIbShort{0};
StatCtr<uint32_t> s_cpRefuseIbUnreadable{0};
StatCtr<uint32_t> s_cpRefuseVbMissing{0};
// F-648: the vertex-format and primitive-type refusals. Separate counters, not
// folds into s_cpRefuseVbMissing, because each answers a different question and
// a soak has to be able to tell "the format did not decode" from "the format and
// the stride disagree" from "the guest drew rectangles".
// F-720: s_cpRefuseVbElemOutOfRange is the vbElementCount bound check, kept as
// its OWN counter rather than folded into vb-fmt-unmapped because it answers a
// different question: "the captured element table and the array holding it
// disagree" is not "a format failed to decode", and a soak that cannot tell them
// apart cannot tell a decode bug from a producer bug. It is deliberately NOT a
// field on DRAW-CP-CENSUS: that row's format is consumed, and every refusal name
// is already countable from DRAW-CP-REFUSE's `what=`, which is where this one is
// named. So no marker's format changes anywhere in this change.
StatCtr<uint32_t> s_cpRefuseVbElemOutOfRange{0};  // vbElementCount > the array
StatCtr<uint32_t> s_cpRefuseVbFmtUnmapped{0};  // format never mapped to a size
StatCtr<uint32_t> s_cpRefuseVbFmtStride{0};    // stride < format: impossible
StatCtr<uint32_t> s_cpRefuseVbFmtShort{0};     // numIdx * formatBytes > vbSize
// F-648: the pre-existing `vb-short` check (num_indices * vbStride > vbSize)
// keeps its original s_cpRefuseVbMissing bump - that tracker is shared with
// vb-missing and vb-unreadable and its meaning is not mine to change - and
// ALSO bumps this one, purely so vb-short is separately countable in
// DRAW-CP-CENSUS. Additive: no existing counter changes meaning.
StatCtr<uint32_t> s_cpRefuseVbShortRefused{0};
// F-751: the vertex-stream byte-order refusals. Three counters, not one, on the
// same reasoning as the four above (F-720): each answers a DIFFERENT question,
// and a soak that cannot tell them apart cannot tell a producer bug from a
// consumer bug. Deliberately NOT fields on DRAW-CP-CENSUS - that row's format is
// consumed - so each is named only through DRAW-CP-REFUSE's `what=`, which is
// where the existing element-bound refusal is named too. No marker's format
// changes in this change.
//   absent : the command carries NO vertex element, so there is no fetch
//            constant on it and therefore no selector. Not "assume no swap".
//   unknown: an element carries a selector outside the four architected values.
//            The guest field is 2 bits (xenos.h:1122), so this cannot be a
//            guest fact - it can only be a producer bug, and it is NOT rounded,
//            masked or clamped into 0..3.
//   split  : two elements of the SAME command disagree. There is exactly ONE
//            copy of the stream (one vbBase/vbSize pair) and therefore only one
//            transform that can be applied to it, so a disagreement has no
//            single correct answer to pick.
StatCtr<uint32_t> s_cpRefuseVbEndianAbsent{0};   // no element -> no selector
StatCtr<uint32_t> s_cpRefuseVbEndianUnknown{0};  // selector outside 0..3
StatCtr<uint32_t> s_cpRefuseVbEndianSplit{0};    // elements disagree
StatCtr<uint32_t> s_cpRefusePrimRect{0};       // prim_type 0x08 kRectangleList
StatCtr<uint32_t> s_cpRefusePrimOther{0};      // any other unmappable prim_type
// Indexed-quad floor expansion: the %4 remainder that is counted but NOT
// refused (the floor quads still draw). Separate from prim-rect-list because
// it answers a different question: "the guest sent a partial quad at the end
// of the stream" vs "unknown topology".
StatCtr<uint32_t> s_cpRefusePrimQuadRemainder{0}; // prim-quad-remainder (non-breaking)
// Indexed-quad OOB guard: maxIndex >= vertexCount. Separate from ib-short
// because the buffer LENGTH was sufficient; the VALUES are out of range.
StatCtr<uint32_t> s_cpRefuseIbMaxIndex{0}; // ib-maxindex
StatCtr<uint32_t> s_cpIndexMeasured{0};
// s_cpDrawn: draws whose DRAW CALL WAS RECORDED -- the backend's own
// m_stats.drawsIssued advanced across DrawDynamicMeshWithPipeline -- and
// nothing else. 2026-10-02: this counter used to be bumped unconditionally at
// the draw site, outside the BeginFrame gate, so it counted commands that
// REACHED the site rather than draws that happened, and because
// DRAW-CP-CENSUS prints it as `drawn=` it made a skipped path read as a
// success. It is now gated on the draw call, which is also what its own
// adjacent comment at s_cpTopologyAssigned below always claimed it meant.
// The count it used to carry is not lost: it is s_cpDrawnSkipped.
StatCtr<uint32_t> s_cpDrawn{0};
// s_cpDrawnSkipped: commands that reached the draw site and recorded NO draw
// call. The complement of s_cpDrawn, on its own counter, so that "the draw was
// refused" and "the draw was never attempted" remain two different numbers and
// a reader can divide one by the other to get the real pass rate. Backs the
// new DRAW-CP-DRAWN-SKIPPED row; nothing existing is renamed or removed.
StatCtr<uint32_t> s_cpDrawnSkipped{0};
// F-648: draws that reached the draw call, i.e. the denominator of the
// topology/format gate. Without it a soak sees refusals but cannot tell whether
// the draws that DID issue were correctly shaped or merely few.
StatCtr<uint32_t> s_cpTopologyAssigned{0};

static StatCtr<uint32_t> s_1sConsumed[16] = {};
static StatCtr<uint32_t> s_1sDrawn[16] = {};
static StatCtr<uint32_t> s_1sSkipped[16] = {};
static StatCtr<uint32_t> s_1sFlushOk{0};
static StatCtr<uint32_t> s_1sFlushEmpty{0};
static StatCtr<uint32_t> s_1sClearOk{0};
static StatCtr<uint32_t> s_1sClearRefused{0};
static StatCtr<uint32_t> s_1sLastOtherPrim{0};
static std::atomic<int64_t> s_1sLastLogNs{0};
static StatCtr<uint32_t> s_1sSecIdx{0};

static void MaybeLogRender1s(int64_t now = NowNs()) {
    int64_t prev = s_1sLastLogNs.load(std::memory_order_relaxed);
    if (prev == 0) {
        s_1sLastLogNs.compare_exchange_strong(prev, now);
        return;
    }
    if (now - prev < 1000000000LL) return;
    if (!s_1sLastLogNs.compare_exchange_strong(prev, now)) return;
    const uint32_t sec = s_1sSecIdx.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t c4 = s_1sConsumed[4].exchange(0, std::memory_order_relaxed);
    uint32_t c6 = s_1sConsumed[6].exchange(0, std::memory_order_relaxed);
    uint32_t c8 = s_1sConsumed[8].exchange(0, std::memory_order_relaxed);
    uint32_t d4 = s_1sDrawn[4].exchange(0, std::memory_order_relaxed);
    uint32_t d6 = s_1sDrawn[6].exchange(0, std::memory_order_relaxed);
    uint32_t d8 = s_1sDrawn[8].exchange(0, std::memory_order_relaxed);
    uint32_t k4 = s_1sSkipped[4].exchange(0, std::memory_order_relaxed);
    uint32_t k6 = s_1sSkipped[6].exchange(0, std::memory_order_relaxed);
    uint32_t k8 = s_1sSkipped[8].exchange(0, std::memory_order_relaxed);
    uint32_t cOth = 0, dOth = 0, kOth = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8) continue;
        cOth += s_1sConsumed[i].exchange(0, std::memory_order_relaxed);
        dOth += s_1sDrawn[i].exchange(0, std::memory_order_relaxed);
        kOth += s_1sSkipped[i].exchange(0, std::memory_order_relaxed);
    }
    uint32_t flOk = s_1sFlushOk.exchange(0, std::memory_order_relaxed);
    uint32_t flEm = s_1sFlushEmpty.exchange(0, std::memory_order_relaxed);
    uint32_t clOk = s_1sClearOk.exchange(0, std::memory_order_relaxed);
    uint32_t clRf = s_1sClearRefused.exchange(0, std::memory_order_relaxed);
    uint32_t othPrim = s_1sLastOtherPrim.load(std::memory_order_relaxed);
    MCLA_LOG_DEBUG("TEMP-RENDER-1S s={} cons[p4={} p6={} p8={} oth={}(p={})] "
                   "drawn[p4={} p6={} p8={} oth={}] skip[p4={} p6={} p8={} oth={}] "
                   "present[FlushDrawFrame={} FlushEmpty={} ClearAndPresent={} ClearRefused={}] "
                   "fencewait[ms={} blocked={} calls={}]",
                   sec, c4, c6, c8, cOth, othPrim, d4, d6, d8, dOth, k4, k6, k8, kOth,
                   flOk, flEm, clOk, clRf,
                   g_fenceWaitNs.exchange(0, std::memory_order_relaxed) / 1000000u,
                   g_fenceWaitBlocked.exchange(0, std::memory_order_relaxed),
                   g_fenceWaitCalls.exchange(0, std::memory_order_relaxed));
}



// ---------------------------------------------------------------------------
// F-652: pipeline-state DERIVATION, counted on the live path.
// ---------------------------------------------------------------------------
// DerivePipelineState (renderer/pipeline_cache.h:541) had NO caller anywhere in
// src/ before this: the whole guest-register -> blend/depth/cull/topology mapping
// was unreachable code that read as alive. It is called below, on the live
// DRAW_CAPTURED path, and its outcome is counted THREE ways, because the whole
// point of the exercise is that a soak must be able to tell these apart:
//
//   s_deriveAttempted  - DerivePipelineState was entered. THE gate: this is what
//                        distinguishes "no pipeline was ever requested" from
//                        "every pipeline was refused". Both read as zero today.
//   s_deriveOk         - it returned a state with refuseMask == kRefuseNone.
//   s_deriveRefused    - it returned refuseMask != 0.
//
// And the refusal is NEVER an unnamed code. s_deriveRefusedByBit is indexed by
// the bit in renderer::PipelineRefusal (pipeline_cache.h:157-194), so a soak
// reads a per-reason TOTAL and never has to parse a log line; the NAME for each
// bit comes from FormatPipelineRefusal (pipeline_cache.h:202), the one owner of
// the name table (pipeline_cache.cpp:196-252). One named reason at a time is
// NOT enough here: a single mask can carry four, and a histogram that collapsed
// them would lose exactly the attribution this is for.
//
// This is not hypothetical defensiveness. pipeline_cache.cpp:226-234 records a
// guard that sat BEFORE its loop, tested an always-zero variable, and made every
// refusal in the file render as "unnamed-0x%08X"; two agents were misled by
// hex-only reasons. So the histogram is filled per bit from the enum, and the
// bounded row prints the formatted names alongside it.
StatCtr<uint32_t> s_deriveAttempted{0};
StatCtr<uint32_t> s_deriveOk{0};
StatCtr<uint32_t> s_deriveRefused{0};
StatCtr<uint32_t> s_deriveRefusedByBit[32] = {};
// The run-wide OR of every refusal mask seen, so the PC-DERIVE row can name the
// reasons the WHOLE run produced rather than only the last draw's. Paired with
// the histogram above: the histogram is exact per reason, this is what lets a
// row render names at all.
StatCtr<uint32_t> s_deriveMaskOr{0};
// Refusals that are NOT a pipeline-state derivation failure: the draw was
// refused AFTER a successful derivation, for a reason about the program rather
// than the state. Kept apart from s_deriveRefused so "the state was wrong" and
// "the state was right and there is still no shader" cannot be conflated.
StatCtr<uint32_t> s_deriveNoVsProgram{0};   // no vertex program for this draw
StatCtr<uint32_t> s_deriveNoPsProgram{0};   // no pixel program for this draw
StatCtr<uint32_t> s_deriveNoTranslatedPso{0};  // state derived, no PSO to bind
// The refusal COUNTERS for those three, so DRAW-CP-CENSUS names each one
// without borrowing a vertex-buffer counter whose meaning it has nothing to do
// with (s_cpRefuseVbMissing counts missing geometry, not missing shaders).
StatCtr<uint32_t> s_cpRefuseNoVsProgram{0};
StatCtr<uint32_t> s_cpRefuseNoPsProgram{0};
StatCtr<uint32_t> s_cpRefuseNoPso{0};
// A DerivePipelineState refusal on a prim_type that IS a geometry type - so not
// the rectangle list and not the "unmappable topology" family. Distinct from
// s_cpRefusePrimOther (which means "D3D12 has no topology for this prim_type"):
// that column would otherwise absorb a rasteriser refusal and make the two
// questions indistinguishable.
StatCtr<uint32_t> s_cpRefuseStateRefused{0};
// F-723: the colour-format refusals. Three SEPARATE counters, because "the
// guest never presented RB_COLOR_INFO" and "the guest presented a code with no
// DXGI equivalent" are different facts and a soak must be able to tell them
// apart -- collapsing them would make a future fix for the second look like a
// success on the first.
//   noColorInfo     - RB_COLOR_INFO 0x2001 was NOT in the snapshot's
//                     presentMask: the guest never wrote it before this draw.
//                     A REFUSAL, not a zero format (see render_queue.h's
//                     kCpSnapColorInfo note on why code 0 is legal).
//   colorFmtUnmapped- RB_COLOR_INFO WAS presented and its 4-bit color_format is
//                     a code with no DXGI_FORMAT (8/9/11/13 unassigned).
//                     NOT defaulted; see MapXenosColorFormatToDxgi.
StatCtr<uint32_t> s_cpRefuseNoColorInfo{0};
StatCtr<uint32_t> s_cpRefuseColorFmtUnmapped{0};
// F-652: the program census, counted CONSUMER-side from the command's own
// fields so it reports what the draw really carried. Distinct from the
// producer's identical census in CP-DRAW-CENSUS on purpose: the producer row
// counts what was enqueued, this row counts what survived the queue's shed and
// eviction policies, and a gap between them is a queue defect neither row can
// see alone.
StatCtr<uint32_t> s_cpProgVsAddr{0};     // draw carried a vertex program ADDRESS
StatCtr<uint32_t> s_cpProgPsAddr{0};     // draw carried a pixel program ADDRESS
StatCtr<uint32_t> s_cpProgVsAbsent{0};   // no vertex program: a refusal
StatCtr<uint32_t> s_cpProgPsAbsent{0};   // no pixel program: a refusal
// Raster-state COVERAGE of what was consumed: one total per register that
// matters to the derivation, plus the all-three case that sets haveRegisters.
// These are TOTALS, never "the value": a missing register must be visible as a
// missing register.
StatCtr<uint32_t> s_cpRsCovPaClipCntl{0};
StatCtr<uint32_t> s_cpRsCovPaSuScModeCntl{0};
StatCtr<uint32_t> s_cpRsCovPaClVteCntl{0};
StatCtr<uint32_t> s_cpRsAll3{0};
// F-652: the rectangle-list expansion decision. `expanded` is 0 on every build
// that refuses; it exists as a counter so the refusal and a future expansion
// are the same column and a soak can see the gate flip rather than infer it from
// a marker's disappearance. `refused` keeps its own reason named on the row.
StatCtr<uint32_t> s_rectListExpanded{0};
StatCtr<uint32_t> s_rectListRefused{0};

// ---------------------------------------------------------------------------
// step 4: the vertex INPUT LAYOUT, counted on the live DRAW_CAPTURED path.
// ---------------------------------------------------------------------------
// Four totals, so a soak can tell four DIFFERENT failures apart instead of
// reading one "no layout" that could mean any of them:
//   s_layoutBuilt        - a real D3D12_INPUT_ELEMENT_DESC list came out of the
//                          authoritative builder. This is the only one of the
//                          four that means pixels are possible.
//   s_layoutNoDecl       - the draw carried no grcFvf declaration at all. THIS
//                          is the measured state of every draw in w682.log.
//   s_layoutDeclRefused  - a declaration WAS carried and the builder refused it
//                          (unknown channel, or fvfSize disagreeing with the
//                          computed stride, pipeline_cache.cpp:1247-1251).
//   s_layoutLogged       - rows PRINTED. Bounded like every other row here, so
//                          it is not a census; the three above are.
//
// `s_layoutDeclRefused` is currently unreachable, and it is kept anyway for the
// same reason s_rectListExpanded is kept: a gate and its own "it worked" total
// have to exist together or a soak can only ever infer the flip.
StatCtr<uint32_t> s_layoutBuilt{0};
StatCtr<uint32_t> s_layoutNoDecl{0};
StatCtr<uint32_t> s_layoutDeclRefused{0};
StatCtr<uint32_t> s_layoutLogged{0};

// ---------------------------------------------------------------------------
// F-716: the program -> pipeline chain's own census, one counter per gate, so a
// soak can read how far a captured draw got instead of inferring it from the
// absence of a pixel. Uncapped, process-lifetime; the ROW is bounded.
//
//   s_pipeVsCode / s_pipePsCode   -- the stage's OWN bytes were recovered
//   s_pipeEnvelopeBuilt           -- the host-side envelope wrapped them
//   s_pipeParsed                  -- ParseShaderProgram accepted the envelope
//   s_pipeTranslatedVs/Ps         -- TranslateShaderPinned produced HLSL
//   s_pipeLayoutFromVertexElements-- a semantic came from the program's OWN
//                                    element table, not from a fallback
//   s_pipeLayoutNoVertexElements  -- the element table was empty, so every
//                                    semantic would be the POSITION fallback
//   s_pipePsoRequested / s_pipePsoNull -- GetOrCompile asked / refused
//   s_pipePsoCarriesGeomStage     -- a returned PSO is known to have the
//                                    expansion stage bound (cache hit on a
//                                    rectangle key; see pipeline_cache.h)
//   s_pipeLogged                  -- rows PRINTED
StatCtr<uint32_t> s_pipeVsCode{0};
StatCtr<uint32_t> s_pipePsCode{0};
StatCtr<uint32_t> s_pipeEnvelopeBuilt{0};
StatCtr<uint32_t> s_pipeParsed{0};
StatCtr<uint32_t> s_pipeTranslatedVs{0};
StatCtr<uint32_t> s_pipeTranslatedPs{0};
StatCtr<uint32_t> s_pipeLayoutFromVertexElements{0};
StatCtr<uint32_t> s_pipeLayoutNoVertexElements{0};
StatCtr<uint32_t> s_pipePsoRequested{0};
StatCtr<uint32_t> s_pipePsoNull{0};
StatCtr<uint32_t> s_pipePsoCarriesGeomStage{0};
StatCtr<uint32_t> s_pipeLogged{0};
// 2026-10-02: rows PRINTED by the new `DRAW-CP-ISSUED` row, which is the
// only place a draw's own outcome is reported (BeginFrame, whether the draw
// call was made, what it returned, and the guest's RB_COLOR_INFO colour_format
// the PSO's RTVFormats[0] was built from). Its own counter, NOT s_pipeLogged:
// DRAW-CP-PIPE is a tracked marker and sharing a budget with a new row would
// let either one suppress the other.
StatCtr<uint32_t> s_cpIssuedLogged{0};

// ---------------------------------------------------------------------------
// F-647: the Xenos constant bank, produced here and ONLY here.
// ---------------------------------------------------------------------------
// d3d12_backend.cpp:2645-2646 already states the division of labour: "The
// producer (render_thread) owns the guest read, because that layer has the
// validity check; this layer only consumes host bytes." This is that read.
// Before it, D3D12Backend::DynamicMeshDesc::guestConstBytes stayed nullptr for
// every draw, so the backend took its `else` branch and memset a ZERO bank
// (d3d12_backend.cpp:2656). Measured in build/w661.log: 8 rows, all
// `CONST-BANK #N src=ZEROS regs=0 guestBytes=0 zeroFilled=0 truncated=0 ok=0`.
// A zero bank is not a neutral default -- it is a specific wrong answer a
// translated shader consumes silently.
//
// WHERE THE ADDRESS COMES FROM, and why it is the register file: the brief said
// "sqVsConst / sqPsConst are captured into the draw packet", and that is true of
// native::DrawPacket (src/renderer/trace_types.h:96,98), but that packet is
// built by DrawPacketAccumulator::OnDrawBuild in src/capture_hooks.cpp, which
// is a file this stream does not own, and it feeds the frame-trace writer, not
// this command loop. Neither draw command carries the field either:
// DrawIndexedCommand and DrawCapturedCommand (src/render_queue.h:12-35,
// :108-137) have no constant-bank member, and render_queue.h is likewise not
// this stream's to edit. The one producer that DOES hold these values and is
// reachable from here is the CP register file: SQ_VS_CONST is register 0x2307
// and SQ_PS_CONST is 0x2308 (.research/xenia/src/xenia/gpu/register_table.inc:
// 578-579), both poked by the same TYPE-0 traffic that writes 0x2204..0x2206.
// So the address is read by index through F-647's export.
//
// MEASURED, and this is a real limit not an assumption: `r2307` and `r2308`
// have ZERO occurrences in build/w660.log and build/w661.log. That is NOT proof
// the guest never writes them -- CP-DRAW-REGS is a 64-entry ring printed for the
// first 3 draws only and it skips zero values, so absence there is absence of
// evidence, not evidence of absence. What it does establish is that this read
// will most likely report `absent` for a while, and that must be a COUNTED
// refusal, not a silent zero-fill. The marker below is how a soak tells the
// two apart.
//
// BUFFER STRATEGY -- a fixed-size function-local `static` array, NOT a vector
// and NOT a ring, and the reason is that the size is a compile-time constant:
// kShaderConstantBankBytes is 256 * 16 = 4096 (renderer/pipeline_cache.h:73),
// which is exactly the 1,024-word Xenos float4 bank the task describes. A
// std::array of that size is allocated ONCE, on first use, and never again: the
// render path performs no per-frame allocation at all, which is the constraint
// the previous stream was given explicitly and the one that `static std::vector
// vb;` / `static std::vector ibRaw;` above already honour by growing only when
// a bigger capture arrives. A ring would be wrong here for a second reason: the
// bytes are consumed by MapUpload inside the backend call on this very stack
// frame, so a ring would have to be sized to the in-flight depth for no benefit
// over one buffer. Thread-safety: processCommand runs on the single render
// thread (RenderThread::threadMain), so one static buffer with no lock is
// correct -- same assumption the existing `static std::vector vb` relies on.
//
// BYTE ORDER, end to end: SQ_VS_CONST is a 32-bit GUEST VA. It is read with
// mem.IsValid (checked, no reinterpret_cast) and then mem.ReadBytes, which is
// the checked bulk read (kernel/memory.cpp: IsValid + __try/__except memcpy).
// ReadBytes performs NO byte swapping -- it is a verbatim copy of the guest's
// bytes out of the guest window -- so what lands in the buffer is still
// BIG-ENDIAN Xenos data, which is exactly what renderer::PackXenosConstantBank
// documents that it expects (pipeline_cache.h:550-554, and the BE assembly at
// pipeline_cache.cpp:989-993). This producer therefore does NO byte assembly
// and adds NO second endian site: the swap happens once, inside the packer, at
// the point that owns it. ReadBytes is chosen over a ReadU32BE loop precisely
// because a loop would make this file look like a second endian site.
//
// ALL-OR-NOTHING: either the whole 4,096-byte bank is built and both descriptor
// fields are set, or the build is abandoned, the reason is counted, and BOTH
// fields are left at their zero-initialised value (nullptr / 0) so the backend
// takes its existing no-bank path and reports `src=ZEROS`. A half-filled bank is
// worse than a zero one because it is partly right, so the packer is called by
// the backend with a size this function chose; the size is either the full
// kShaderConstantBankBytes or zero, never something in between.
//
// SOURCE, CORRECTED 2026-10-27. The bank is BUILT from the Xenos ALU
// register-file array (registers 0x4000..0x47FF), not read from guest memory:
// SQ_VS_CONST/SQ_PS_CONST are (base,size) pairs, and the previous producer's
// guest-memory read at the raw register value (w842: vsVa=000FF000) handed
// shaders arbitrary bytes as constants. See FillGuestConstantBank below and
// gpu_cp.h:240-302 for the console model the builder implements.

// MEM-CACHES (d3d12_backend.cpp Present): host cache sizes the backend cannot see.
std::atomic<size_t> g_memUntileBytes{0}, g_memVsCache{0}, g_memPsCache{0};

// Per-thread constant bank staging buffers (eliminates shared static races across Stage 1 workers).
static thread_local std::array<uint8_t, mcla::renderer::kShaderConstantBankBytes>
    s_constBank{};
static thread_local std::array<uint8_t, mcla::renderer::kShaderConstantBankBytes>
    s_constBankPs{};  // F-932: the pixel stage's bank
static thread_local uint32_t s_nativeBankId = 0;

// F-647 census: attempts and each named outcome, counted separately so a soak
// can read "the producer never ran" and "the producer ran and the guest address
// was not written" as two different facts.
StatCtr<uint32_t> s_constBankAttempts{0};
StatCtr<uint32_t> s_constBankFilled{0};
StatCtr<uint32_t> s_constBankRefNoRegister{0};  // 0x2307 not present in the CP file
StatCtr<uint32_t> s_constBankRefNoAddr{0};      // present, value == 0
StatCtr<uint32_t> s_constBankRefWrap{0};        // va + 4096 leaves the 4 GiB window
StatCtr<uint32_t> s_constBankRefUnmapped{0};    // IsValid() false
StatCtr<uint32_t> s_constBankRefReadFailed{0};  // ReadBytes() failed
// F-647: the descriptor can carry ONE bank (DynamicMeshDesc has a single
// guestConstBytes/guestConstBytesSize pair, d3d12_backend.h:88-89, and the root
// signature binds ONE b0 CBV, d3d12_backend.cpp:2676). So the pixel bank is
// read and REPORTED but not handed over, and this counter is how a soak proves
// that is a deliberate single-slot decision rather than the pixel bank being
// invisible. It is not a claim that VS is the correct choice -- it is the only
// one the descriptor can express.
StatCtr<uint32_t> s_constBankPsPresent{0};
StatCtr<uint32_t> s_constBankPsDroppedNoSlot{0};

// ---------------------------------------------------------------------------
// F-716 STEP 2, THE INTERPOLATOR REGISTER STATE -- the ONE piece of SQ_PROGRAM_CNTL
// the pixel interpolant count reads.
//
// THE CAPTURE END, CITED. src/gpu_cp.cpp:1170 `CpRegPoke(target, v)` stores every
// guest TYPE-0/REGISTER write into g_xenosReg[] (gpu_cp.cpp:1043-1061), and
// src/gpu_cp.cpp:658-661 `CpRegPeek` reads it back; the file's own marker prints
// the write at gpu_cp.cpp:1179-1182. SQ_PROGRAM_CNTL is register 0x2180
// (.research/xenia/src/xenia/gpu/register_table.inc:523), and MEASURED on the live
// boot in build/w704.log:
//
//   CP-REG-T0 #17 op=IB base=2180 n=2 written=2 val=1000000E writes=26
//
// so the guest DOES write it and it IS captured. Decoded against
// .research/xenia/src/xenia/gpu/registers.h:127-150 (each *_num_reg is stored
// MINUS 1): vs_num_reg = 0xE = 14 -> 15 GPRs, ps_num_reg = (val>>8)&0x3F = 0 -> 1
// GPR, vs_export_count = (val>>20)&0xF = 0 -> 1 interpolator output.
//
// 2026-10-05 CORRECTION. That line read "= 1 -> 2 interpolator outputs".
// Both numbers were wrong, and hand arithmetic catches it: the guest wrote
// 0x1000000E (CP-REG-T0 #17 base=2180 writes=26, and CP-DRAW-REGS #1 [24/57]
// r2180=1000000E), so (val>>20)&0xF is 0, not 1 -- the only set bit above
// bit 19 is bit 28. And registers.h:127-150 documents the field as
// "Interpolator output count minus 1", so 0 means ONE output.
//
// NOT A BEHAVIOURAL DEFECT: vs_export_count appears in this tree only in
// COMMENTS -- render_thread.cpp, gpu_mmio.h and render_queue.h, zero code
// reads it. Corrected because a false decode sitting next to a real one is
// how the next reader repeats it.
//
// A CORRECTION WORTH RECORDING, because the obvious index is the wrong one. The
// name SQ_PGM_RESOURCES_PS = 0x2301 in src/gpu_mmio.h (the enum moved to
// :79 on 2026-10-05 when a census comment was added above it) is NOT
// SQ_PROGRAM_CNTL:
// .research/xenia/src/xenia/gpu/register_table.inc:572 puts 0x2301 at PA_SC_AA_CONFIG
// and :571 puts 0x2300 at PA_SC_LINE_CNTL, while SQ_PROGRAM_CNTL is 0x2180. The
// w704 row `CP-REG-T0 #519000 op=IB base=2301 n=1 written=1 val=0000C003` is real
// and captured, but it is a PA_SC_AA_CONFIG write and its value must not be read as
// ps_num_reg. src/gpu_mmio.h is not this file's to correct; the note belongs here so
// the next reader does not repeat it.
//
// THE READ END, CITED. CpReadGuestRegisters (declared src/gpu_cp.h:107, defined
// src/gpu_cp.cpp:5920-5947) is the file's sanctioned read-only, presence-reporting
// export of that same array; this file already uses it for the constant banks
// (:830-841). `present == 0` means the guest never wrote the register and the value
// MUST be treated as meaningless, so this returns 0 -- which is the INITIALISER and
// NOT "the guest said zero" (see ShaderProgram::psNumReg). Returning 0 there is
// safe because GetDynamicAddressableRegisterCount ignores ps_num_reg entirely for a
// program that does not use register dynamic addressing
// (.research/xenia/src/xenia/gpu/shader.h:938-945).
//
// NOT A NEW MARKER. The plumbing is observable on the existing SHADER-UNWRITTEN-REG-
// READ row's appended `seedRule=` clause, which carries psNumReg and the two masks,
// so a new marker here would be a second place to census the same fact.
static uint32_t ReadPsNumRegField() {
    using mcla::gpu::CpReadGuestRegisters;
    constexpr uint32_t kRegSqProgramCntl = 0x2180u;
    constexpr uint32_t kIdx[1] = {kRegSqProgramCntl};
    uint32_t val[1] = {0};
    uint32_t pres[1] = {0};
    (void)CpReadGuestRegisters(kIdx, 1u, val, pres);
    // ps_num_reg occupies bits 8-13 (registers.h:136) and is stored MINUS 1.
    return pres[0] ? ((val[0] >> 8) & 0x3Fu) : 0u;
}

// Fills `out` (a DynamicMeshDesc) with the Xenos constant bank, or leaves it
// untouched. Returns true when a bank was handed over.
//
// THE CONSOLE MODEL (gpu_cp.h:240-302). SQ_VS_CONST (0x2307) / SQ_PS_CONST
// (0x2308) are (base,size) PAIRS, not addresses: base = bits 0..8, size = bits
// 12..20 (vec4 count MINUS ONE). A shader's `cN` reads the ALU register-file
// array at 0x4000 + 4*(base+N), zeros outside the stage's window. The producer
// is therefore CpBuildShaderConstantBank -- the builder that implements exactly
// that model -- and NOT a guest-memory read. The previous producer read 4096
// bytes of guest memory at the RAW register value (w842: vsVa=000FF000), which
// is not a constant bank at all; shaders consumed arbitrary guest bytes as
// constants. The builder writes BIG-ENDIAN float4, which is what the backend's
// single endian site (PackXenosConstantBank, pipeline_cache.cpp:1341-1348)
// expects and swaps.
static bool FillGuestConstantBankTo(uint8_t* vsDst, uint8_t* psDst, uint32_t kBankBytes,
                                    D3D12Backend::DynamicMeshDesc& out) {
    using mcla::gpu::CpReadGuestRegisters;
    using mcla::gpu::CpBuildShaderConstantBank;
    using mcla::gpu::CpShaderConstantInfo;
    using mcla::gpu::CpShaderStage;
    using mcla::gpu::kCpRegSqVsConst;
    using mcla::gpu::kCpRegSqPsConst;

    // Both banks' addresses in ONE export call, so the marker and the CP-REGS-
    // EXPORT row agree on the same snapshot. VS is the one we can hand over.
    static constexpr uint32_t kIdx[2] = {kCpRegSqVsConst, kCpRegSqPsConst};
    uint32_t val[2] = {0, 0};
    uint32_t pres[2] = {0, 0};
    (void)CpReadGuestRegisters(kIdx, 2u, val, pres);

    const uint32_t n =
        s_constBankAttempts.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (pres[1]) {
        s_constBankPsPresent.fetch_add(1, std::memory_order_relaxed);
    }

    const char* reason = nullptr;
    uint32_t vsReg = val[0];

    if (!pres[0]) {
        reason = "vs-reg-absent";
        s_constBankRefNoRegister.fetch_add(1, std::memory_order_relaxed);
    }

    if (reason) {
        if (n <= 8u || (n % 200u) == 0u) {
            MCLA_LOG_WARN("CONST-BANK-PRODUCE #{} filled=0 reason={} vsRegPresent={} "
                          "vsReg={:08X} psRegPresent={} psReg={:08X} bankBytes={} "
                          "| totals attempts={} filled={} noReg={} noAddr={} "
                          "wrap={} unmapped={} readFailed={} psPresent={}",
                          n, reason, pres[0], vsReg, pres[1], val[1], kBankBytes,
                          s_constBankAttempts.load(std::memory_order_relaxed),
                          s_constBankFilled.load(std::memory_order_relaxed),
                          s_constBankRefNoRegister.load(std::memory_order_relaxed),
                          s_constBankRefNoAddr.load(std::memory_order_relaxed),
                          s_constBankRefWrap.load(std::memory_order_relaxed),
                          s_constBankRefUnmapped.load(std::memory_order_relaxed),
                          s_constBankRefReadFailed.load(std::memory_order_relaxed),
                          s_constBankPsPresent.load(std::memory_order_relaxed));
        }
        return false;
    }

    CpShaderConstantInfo info{};
    if (!CpBuildShaderConstantBank(CpShaderStage::kVertex, vsDst,
                                   kBankBytes, &info)) {
        s_constBankRefReadFailed.fetch_add(1, std::memory_order_relaxed);
        if (n <= 8u || (n % 200u) == 0u) {
            MCLA_LOG_WARN("CONST-BANK-PRODUCE #{} filled=0 reason=vs-build-failed "
                          "vsRegPresent={} vsReg={:08X} psRegPresent={} "
                          "psReg={:08X} bankBytes={} | totals attempts={} "
                          "filled={} noReg={} noAddr={} wrap={} unmapped={} "
                          "readFailed={} psPresent={}",
                          n, pres[0], vsReg, pres[1], val[1], kBankBytes,
                          s_constBankAttempts.load(std::memory_order_relaxed),
                          s_constBankFilled.load(std::memory_order_relaxed),
                          s_constBankRefNoRegister.load(std::memory_order_relaxed),
                          s_constBankRefNoAddr.load(std::memory_order_relaxed),
                          s_constBankRefWrap.load(std::memory_order_relaxed),
                          s_constBankRefUnmapped.load(std::memory_order_relaxed),
                          s_constBankRefReadFailed.load(std::memory_order_relaxed),
                          s_constBankPsPresent.load(std::memory_order_relaxed));
        }
        return false;
    }

    // The only success path. Both fields are set together or neither is, so the
    // backend can never see a pointer without its size.
    out.guestConstBytes = vsDst;
    out.guestConstBytesSize = kBankBytes;
    // F-932: build and deliver the pixel stage's bank with the same builder.
    CpShaderConstantInfo psInfo{};
    if (CpBuildShaderConstantBank(CpShaderStage::kPixel, psDst,
                                  kBankBytes, &psInfo)) {
        out.guestPsConstBytes = psDst;
        out.guestPsConstBytesSize = kBankBytes;
    } else if (pres[1] && val[1] != 0u) {
        s_constBankPsDroppedNoSlot.fetch_add(1, std::memory_order_relaxed);
    }
    s_constBankFilled.fetch_add(1, std::memory_order_relaxed);
    if (n <= 8u || (n % 200u) == 0u) {
        MCLA_LOG_WARN("CONST-BANK-PRODUCE #{} filled=1 reason=- vsRegPresent={} "
                      "vsReg={:08X} psRegPresent={} psReg={:08X} bankBytes={} "
                      "vsBase={} vsSize={} entriesFilled={} aluDwordsPresent={} "
                      "(big-endian from the ALU register file, packed by "
                      "PackXenosConstantBank) "
                      "| totals attempts={} filled={} noReg={} noAddr={} "
                      "wrap={} unmapped={} readFailed={} psDroppedNoSlot={}",
                      n, pres[0], vsReg, pres[1], val[1], kBankBytes,
                      info.vsBase, info.vsSize, info.entriesFilled,
                      info.aluDwordsPresent,
                      s_constBankAttempts.load(std::memory_order_relaxed),
                      s_constBankFilled.load(std::memory_order_relaxed),
                      s_constBankRefNoRegister.load(std::memory_order_relaxed),
                      s_constBankRefNoAddr.load(std::memory_order_relaxed),
                      s_constBankRefWrap.load(std::memory_order_relaxed),
                      s_constBankRefUnmapped.load(std::memory_order_relaxed),
                      s_constBankRefReadFailed.load(std::memory_order_relaxed),
                      s_constBankPsDroppedNoSlot.load(std::memory_order_relaxed));
    }
    return true;
}

bool FillGuestConstantBank(D3D12Backend::DynamicMeshDesc& out) {
    s_nativeBankId = 0;   // the CP path overwrites the shared buffers
    return FillGuestConstantBankTo(s_constBank.data(), s_constBankPs.data(),
                                   mcla::renderer::kShaderConstantBankBytes, out);
}

// ---------------------------------------------------------------------------
// F-716: program IDENTITY, and the envelope this project's translator needs to
// read it. Both steps were missing, and the second one is a documented DEVIATION
// from the reference, so it is spelled out here rather than discovered later.
// ---------------------------------------------------------------------------
//
// STEP 1, IDENTITY. The identity of a program is its own BYTES, and the bytes
// are already on the command: DrawCapturedCommand carries one CpShaderProgram
// per stage (render_queue.h:411-412) and that record carries the body itself
// (render_queue.h:322, `body[kCpShaderBodyDwords]`) plus the dword count the
// PM4_IM_LOAD packet declared (`dwords`, render_queue.h:307). So identity needs
// no container, no address and no lookup: `body[0..captured)` IS the program.
//
// STEP 2, THE ENVELOPE, AND WHY IT IS NOT A PORT. The reference never has one.
// Measured, in four places:
//   * command_processor.cc:1574-1576 -- the embedded form hands LoadShader the
//     RAW PACKET POINTER plus size_dwords: `LoadShader(shader_type,
//     uint32_t(reader->read_ptr()), reinterpret_cast<uint32_t*>(
//     reader->read_ptr()), size_dwords)`;
//   * command_processor.cc:1542-1544 -- the pointer form is the same shape, and
//     the guest address is a program ADDRESS, not a container address;
//   * d3d12/pipeline_cache.cc:846-847 -- the shader's identity is a hash of the
//     raw dwords alone, `XXH3_64bits(host_address, dword_count * sizeof(uint32_t))`,
//     with no header, no flags and no metadata-size term;
//   * shader.cc:27-37 -- `Shader` stores the program as a plain
//     `std::vector<uint32_t> ucode_data_` of that many dwords. There is no
//     container object anywhere in the reference's shader path.
// A container is therefore THIS PROJECT'S TRANSLATOR'S PREMISE, not the
// hardware's: ParseShaderProgram rejects anything whose first dword does not
// carry the 0x102A11 magic (xenos_shader_ir.cpp:37), and the entry point this
// stream is required to use, TranslateShaderPinned, goes through it
// (shader_translator.cpp:1269 -> :1351).
//
// So the envelope below is a HOST-SIDE ADAPTER whose only purpose is to hand
// the existing entry point the guest's own bytes. Its fields split by provenance
// and the split is not negotiable:
//   MEASURED, guest data, byte-for-byte:
//     * the code bytes. Copied, never regenerated.
//     * shaderSize = the captured dword count * 4. That count is
//       `size_dwords = start_size & 0xFFFF` (command_processor.cc:1570), the
//       same field render_queue.h:307 names.
//     * flags bit 0 (isVertex), from the stage tag the guest's own packet
//       carried (xenos::ShaderType, command_processor.cc:1567-1568).
//   HOST-SIDE LAYOUT, chosen so ParseShaderProgram's own arithmetic resolves the
// code and NOTHING ELSE resolves:
//     * virtualSize / physicalSize / `so` / physicalOffset, all below.
//   ZERO, and this is the part that COSTS:
//     * the metadata tables. fieldC (the svPos register), field10 and
//       interpolatorInfo are 0, so out.constants is empty (xenos_shader_ir.cpp
//       :94-98, cto == 0 skips the table) and out.interpolators is empty
//       (:180-192, :206-216). The vertex-element table is ABSENT BY GUARD, not
//       by omission: the guard at :167-168 is
//       `so + 36 + (veStart + veCount + 32) * 4 <= virtualSize`, and the sizes
//       below make 72 > 60, so it fails and no element is invented.
//
// THE COST, STATED PLAINLY, because it is what stops the draw downstream and a
// reader must not have to rediscover it: with no vertex-element table the
// program's `vertexElements` is empty, and ReferencedVertexInputs resolves EVERY
// fetch to usage 0 = POSITION by its own documented fallback
// (shader_translator.cpp:68-78 -- it matches against vertexElements, and when
// that list is empty it returns element [0], which does not exist, so the
// initialised 0 stands). That is a FALLBACK, not a measurement, and the layout
// gate below refuses on exactly that condition rather than binding a semantic
// the guest never declared. On the reference this information does not come
// from the program at all: the vertex elements are the vertex-FETCH hardware's,
// and the interpolant set is read from the register file
// (d3d12/pipeline_cache.cc:882 and :915, `interpolator_mask` from
// SQ_INTERPOLATOR_CNTL). Neither register is captured by this project, which is
// the named blocker.

// Envelope geometry. Internal linkage (const), and each value is either a
// constant of the format being adapted or arithmetic on measured fields.
constexpr uint32_t kEnvMagic        = 0x102A1100u;  // ParseShaderProgram's (flags & 0xFFFFFF00)
constexpr uint32_t kEnvShaderHdrOff = 36u;           // the `so` we hand it
constexpr uint32_t kEnvShaderHdrLen = 24u;           // xenos_shader_ir.cpp kShaderHeaderSize
// virtualSize only has to cover the container header plus the shader header, and
// it MUST stay below the vertex-element guard's threshold (72 bytes) so the
// element table is skipped rather than invented.
constexpr uint32_t kEnvVirtualSize  = kEnvShaderHdrOff + kEnvShaderHdrLen;  // 60

// One stage's captured program dwords serialized in the byte order
// ParseShaderProgram reads (big-endian, ReadBE32 at xenos_shader_ir.cpp:9-11),
// plus the source form that originally supplied them. Returns 0 and sets
// `sourceOut` to a refusal reason when this stage has no usable bytes.
static size_t ResolveStageCode(const CpShaderProgram& p,
                              std::vector<uint8_t>& codeOut,
                              const char*& sourceOut) {
    sourceOut = "absent";
    if (p.present == 0u) {
        sourceOut = "no-im-load";
        return 0;
    }
    // `captured` may EXCEED the array (render_queue.h:314-315), so the cap is a
    // precondition, not a clamp: reading past body[255] is an out-of-bounds read.
    if (p.captured == 0u || p.captured > kCpShaderBodyDwords) {
        sourceOut = "captured-out-of-range";
        return 0;
    }
    const size_t useDwords = p.captured;
    // CpExecImLoad reads both PM4_IM_LOAD forms with ReadU32BE, then
    // CpSnapshotDrawState carries those host-order dwords with the queued draw.
    // guestAddr is the packet's physical address and is not necessarily the
    // readable kernel-VA alias that the producer used.
    codeOut.resize(useDwords * 4u);
    for (size_t i = 0; i < useDwords; ++i) {
        const uint32_t v = p.body[i];
        codeOut[i * 4 + 0] = static_cast<uint8_t>(v >> 24);
        codeOut[i * 4 + 1] = static_cast<uint8_t>(v >> 16);
        codeOut[i * 4 + 2] = static_cast<uint8_t>(v >> 8);
        codeOut[i * 4 + 3] = static_cast<uint8_t>(v);
    }
    sourceOut = p.embedded != 0u ? "embedded" : "guest-memory";
    return codeOut.size();
}

// R6: ResolveStageCode's return value and `sourceOut` without materialising the byte vector.
static size_t StageCodeBytes(const CpShaderProgram& p, const char*& sourceOut) {
    sourceOut = "absent";
    if (p.present == 0u) { sourceOut = "no-im-load"; return 0; }
    if (p.captured == 0u || p.captured > kCpShaderBodyDwords) { sourceOut = "captured-out-of-range"; return 0; }
    sourceOut = p.embedded != 0u ? "embedded" : "guest-memory";
    return static_cast<size_t>(p.captured) * 4u;
}

// Word-wise FNV-1a over an envelope, for in-memory translation-cache keys (translate-vs).
// shortcut: MCLA_KEY_BYTEWISE=1 restores the byte chain for A/B; delete the gate once SC-2 is measured.
static uint64_t EnvelopeKeyHashBytes(const uint8_t* p, size_t len, uint64_t h) {
    static const bool s_bytewise = [] { const char* e = std::getenv("MCLA_KEY_BYTEWISE"); return e && e[0] == '1'; }();
    size_t i = 0;
    if (!s_bytewise) {
        for (; i + 8 <= len; i += 8) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            h = (h ^ w) * 0x100000001b3ULL;
            h ^= h >> 29;
        }
    }
    for (; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}
static uint64_t EnvelopeKeyHash(const std::vector<uint8_t>& b, uint64_t h) {
    return EnvelopeKeyHashBytes(b.data(), b.size(), h);
}

// P5 envelope-key memo observability. Counted at each consume site; first 8 of each
// kind then every 1000th so a long soak stays bounded. A miss is itself informative:
// it means the (program, generation) changed and the hash was really recomputed.
static std::atomic<uint64_t> s_envMemoHits{0}, s_envMemoMisses{0};
static inline void NoteEnvMemo(bool hit) {
    const uint64_t n = hit ? s_envMemoHits.fetch_add(1, std::memory_order_relaxed) + 1
                           : s_envMemoMisses.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n % 1000) == 0) {
        MCLA_LOG_ERROR("EnvelopeMemo {} #{} hits={} misses={}",
                      hit ? "hit" : "miss", n,
                      s_envMemoHits.load(std::memory_order_relaxed),
                      s_envMemoMisses.load(std::memory_order_relaxed));
    }
}

// Swarm p5 round 2: log-only A/B support. The memo's own cadence log names hits/misses
// but never the arm, so a TEMP-PERF-STAGES row cannot be attributed to on/off on its own.
// Printed inside the existing once-per-1000-draws logging block: zero draw-path cost,
// zero change to memo behaviour (default ON, MCLA_NO_ENV_MEMO=1 off).
std::string TempEnvMemoRow() {
    return " envMemo=" + std::string(s_envMemoOn ? "on" : "off") +
           " emHits=" + std::to_string(s_envMemoHits.load(std::memory_order_relaxed)) +
           " emMiss=" + std::to_string(s_envMemoMisses.load(std::memory_order_relaxed));
}

// Wraps guest code bytes in the minimum container ParseShaderProgram accepts.
// Returns false if the code is empty. `isVertex` comes from the guest's own
// stage tag; nothing else here is a guest fact.
static bool BuildProgramEnvelope(const uint8_t* code, size_t codeBytes,
                                 bool isVertex, std::vector<uint8_t>& envOut) {
    if (code == nullptr || codeBytes == 0u) return false;
    envOut.assign(kEnvVirtualSize + codeBytes, 0u);
    uint8_t* const d = envOut.data();
    const auto put = [d](size_t off, uint32_t v) {
        d[off + 0] = static_cast<uint8_t>(v >> 24);
        d[off + 1] = static_cast<uint8_t>(v >> 16);
        d[off + 2] = static_cast<uint8_t>(v >> 8);
        d[off + 3] = static_cast<uint8_t>(v);
    };
    put(0,  kEnvMagic | (isVertex ? 1u : 0u));  // bit 0 == isVertex (xenos_shader_ir.cpp:44)
    put(4,  kEnvVirtualSize);                   // >= so + 24 (guard at :47)
    put(8,  static_cast<uint32_t>(codeBytes));   // physicalSize; :41 wants vsize+psize <= size
    // +12/+16/+20 are not read by the parser; 0x10 (cto) must be 0 so the
    // constant table is skipped rather than read (:94-98).
    put(24, kEnvShaderHdrOff);                   // `so`
    put(28, 0);                                  // f1C: must be 0 (:40)
    put(32, 0);                                  // f20: must be 0 (:40)
    put(kEnvShaderHdrOff + 0,  0);               // physicalOffset -> code sits at virtualSize
    put(kEnvShaderHdrOff + 4,  static_cast<uint32_t>(codeBytes));  // shaderSize, MEASURED
    put(kEnvShaderHdrOff + 8,  0);               // unused by the parser
    put(kEnvShaderHdrOff + 12, 0);               // fieldC: the svPos register -- NOT MEASURED
    put(kEnvShaderHdrOff + 16, 0);               // field10 -- NOT MEASURED
    put(kEnvShaderHdrOff + 20, 0);               // interpolatorInfo: 0 interpolants
    std::memcpy(d + kEnvVirtualSize, code, codeBytes);
    return true;
}

// F-720: how many BYTES one vfetch instruction actually consumes of the vertex,
// as the reference computes it.
//
// WHY THIS IS NOT `CpXenosFormatBytes`. That function IS exported
// (render_queue.h:785) and it is the right table -- but its signature is
// `uint32_t(uint32_t fmt)`: ONE argument, NO components. It is therefore
// GetVertexFormatNeededWords(fmt, 0b1111) * 4, the size for ALL FOUR components,
// which is NOT what the fetch reads when the destination swizzle selects a
// subset. The reference does not use the all-components size: it builds
// `used_result_components` from the destination swizzle FIRST
// (.research/xenia/src/xenia/gpu/shader_interpreter.cc:1019-1026) and only then
// asks for the words (:1027-1028). So the components argument this arithmetic
// needs does not exist in the form available, and the raw-word formula below is
// used instead, transcribed from xenos.h:648-683 -- as instructed, because
// recomputing the popcount is the only way to honour the swizzle.
//
// WHY NOT `el.fmt.bytes` EITHER, even though it is on the struct already:
// it is the same all-components number, carried once at the producer
// (gpu_cp.cpp:3898, `CpXenosFormatBytes(e.formatXenos)`). Using it would make
// the accounting over-count any swizzle that drops a component, which would
// understate bytesUnaccounted -- i.e. the check would stop being able to fail.
//
// MEASURED (build/w686.log, CP-FETCHVB-DESC #1 / #3, the two live streams):
//   element 0  vf=57 dstSwizzle=0xA88 -> used=0b0111 -> mask 0b0111 -> 3 -> 12 B
//   element 1  vf=38 dstSwizzle=0x688 -> used=0b1111 -> mask 0b1111 -> 4 -> 16 B
//   draw 2 e0  vf=37 dstSwizzle=0xB48 -> used=0b0011 -> mask 0b0011 -> 2 ->  8 B
// and the same two vfetch words appear verbatim in the guest microcode itself,
// CP-SHADER-BODY #3 body[9..11] = 30081000 00393A88 00000007 and body[12..14]
// = 00080000 40263688 00000300, which decode to the same five fields.
// so draw 1 accounts 12 + 16 = 28 = its stride and draw 2 accounts 8 = its
// stride, both exactly. For THESE three elements all-components and
// swizzle-narrowed happen to AGREE (each swizzle selects every component its
// format carries), so CpXenosFormatBytes would print the same numbers today;
// over all 16 ids x 4096 swizzles the two disagree on 53.59% of the pairs
// (e.g. vf=6 swizzle=0x924: 0 B here vs 4 B there), which is why the
// distinction is a comment and not a bug report.
//
// GetVertexFormatNeededWords returns a 4-BIT MASK of needed dwords, NOT a
// COUNT (xenos.h:665-667 return `0b0010` for two words, and the reference
// iterates it as a mask at shader_interpreter.cc:1036-1039). The byte count is
// therefore popcount(mask) * 4. Passing the mask through as a count would give
// 28 and 60 bytes here, i.e. 7x and 15x the truth.
//
// Every id outside the 16 of xenos.h:602-619 (including kUndefined == 0) hits
// the reference's `default: assert_unhandled_case` arm, so this returns 0 and
// the CALLER refuses on it rather than inventing a size.
static uint32_t CpVfetchBytesForSwizzle(uint32_t xenosFormat, uint32_t dstSwizzle) {
    // used_result_components, shader_interpreter.cc:1020-1026 verbatim: four
    // 3-bit selectors, each naming a component when its value is <= 3.
    uint32_t used = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint32_t sel = (dstSwizzle >> (3u * i)) & 0x7u;
        if (sel <= 3u) {
            used |= (1u << sel);
        }
    }
    // xenos.h:651-653 returns 0 for used_components == 0 BEFORE the switch, so
    // this guard is not cosmetic: without it k_8_8_8_8 / k_2_10_10_10 would
    // report one dword for a swizzle that selects no component at all. Found by
    // the differential test over all 16 ids x 4096 swizzles (512 mismatches,
    // exactly 2 formats x the 256 swizzles whose four selectors are all > 3).
    if (used == 0u) {
        return 0u;
    }
    uint32_t mask = 0u;  // the 4-bit dword mask of xenos.h:648-683
    switch (xenosFormat) {
        case 6: case 7:                                   // k_8_8_8_8, k_2_10_10_10
            mask = 0b0001; break;
        case 16: case 17:                                 // k_10_11_11, k_11_11_10
            mask = (used & 0b0111) ? 0b0001 : 0b0000; break;
        case 25: case 31:                                 // k_16_16, k_16_16_FLOAT
            mask = (used & 0b0011) ? 0b0001 : 0b0000; break;
        case 26: case 32:                                 // k_16_16_16_16(_FLOAT)
            mask = ((used & 0b0011) ? 0b0001 : 0b0000) |
                   ((used & 0b1100) ? 0b0010 : 0b0000); break;
        case 33: case 36:                                 // k_32, k_32_FLOAT
            mask = used & 0b0001; break;
        case 34: case 37:                                 // k_32_32(_FLOAT)
            mask = used & 0b0011; break;
        case 35: case 38:                                 // k_32_32_32_32(_FLOAT)
            mask = used; break;
        case 57:                                          // k_32_32_32_FLOAT
            mask = used & 0b0111; break;
        default:
            return 0u;  // kUndefined and every unnamed id: a refusal, not a size
    }
    // popcount of a 4-bit mask, written out rather than <bit>'s so this file
    // gains no include.
    return ((mask & 1u) + ((mask >> 1) & 1u) + ((mask >> 2) & 1u) +
            ((mask >> 3) & 1u)) * 4u;
}

void RenderThread::LogPipeRow(const DrawCapturedCommand& dc,
                              const D3D12Backend::PreparedDraw& prep,
                              const DrawStage1Meta& meta,
                              const char* pipeStop) {
    const uint32_t pn =
        s_pipeLogged.fetch_add(1, std::memory_order_relaxed) + 1u;
    const uint32_t n = static_cast<uint32_t>(prep.drawSeq);
    auto& guestMem = mcla::kernel::GuestMemoryHeap::Instance();
    if (pn > 8u && (pn % 20000u) != 0u) return;  // MD-32: was 200
    MCLA_LOG_WARN(
        "DRAW-CP-PIPE #{} draw={} stop={} | vsSrc={} vsB={} psSrc={} "
        "psB={} | envelope={} parsed={} vsElements={} vsInstrs={} "
        "vsInterp={} | vsHlsl={} psHlsl={} vsHash={:016X} "
        "psHash={:016X} | layout={} declHash={:016X} | "
        "psoReq={} psoNull={} psoGeomStage={} || "
        "vsCode={} psCode={} envBuilt={} parsed={} vsTr={} psTr={} "
        "layFromVE={} layNoVE={} pso={} geom={}",
        pn, n, pipeStop, meta.pipeVsSrc, meta.pipeVsCodeBytes, meta.pipePsSrc,
        meta.pipePsCodeBytes,
        s_pipeEnvelopeBuilt.load(std::memory_order_relaxed),
        s_pipeParsed.load(std::memory_order_relaxed),
        meta.pipeVsElements, meta.pipeVsInstructions, meta.pipeVsInterpolants,
        meta.pipeVsHlslSize, meta.pipePsHlslSize, meta.pipeVsHash, meta.pipePsHash,
        prep.inputLayout.size(), meta.inputDeclHash,
        s_pipePsoRequested.load(std::memory_order_relaxed),
        s_pipePsoNull.load(std::memory_order_relaxed),
        prep.hasGeometryStage ? 1u : 0u,
        s_pipeVsCode.load(std::memory_order_relaxed),
        s_pipePsCode.load(std::memory_order_relaxed),
        s_pipeEnvelopeBuilt.load(std::memory_order_relaxed),
        s_pipeParsed.load(std::memory_order_relaxed),
        s_pipeTranslatedVs.load(std::memory_order_relaxed),
        s_pipeTranslatedPs.load(std::memory_order_relaxed),
        s_pipeLayoutFromVertexElements.load(std::memory_order_relaxed),
        s_pipeLayoutNoVertexElements.load(std::memory_order_relaxed),
        s_pipePsoRequested.load(std::memory_order_relaxed),
        s_pipePsoCarriesGeomStage.load(std::memory_order_relaxed));
    if (!meta.pipeTranslateError.empty()) {
        MCLA_LOG_WARN(
            "DRAW-CP-PIPE-ERR #{} draw={} stop={} stage={} err={}",
            pn, n, pipeStop, meta.pipeTrStage, meta.pipeTranslateError);
    }
}

void RenderThread::processCommand(const RenderCommand& cmd) {
    switch (cmd.type) {
        case RenderCommand::INIT_D3D12: {
            const auto& init = std::get<InitD3D12Command>(cmd.data);
            if (!d3d12Initialized_) {
                if (backend_.Initialize(static_cast<HWND>(init.hwnd), init.width, init.height)) {
                    d3d12Initialized_ = true;
                    // F-899: the guest draws its frame as a BURST and swaps once.
                    // Batching is what makes the burst land in one command list
                    // instead of being refused 552,600 times by BeginFrame's
                    // latch (measured, build/g919.log).
                    backend_.SetBatchDrawFrames(true);
                    MCLA_LOG_INFO("RenderThread: D3D12 initialized ({}x{})", init.width, init.height);
                } else {
                    MCLA_LOG_ERROR("RenderThread: D3D12 initialization failed");
                }
            }
            break;
        }
        case RenderCommand::SET_PIPELINE_STATE: {
            const auto& p = std::get<SetPipelineStateCommand>(cmd.data);
            MCLA_LOG_DEBUG("RenderThread: SetPipelineState hash={:016X}", p.psoHash);
            break;
        }
        case RenderCommand::SET_RENDER_STATE: {
            const auto& rs = std::get<SetRenderStateCommand>(cmd.data);
            static StatCtr<uint32_t> rsCount{0};
            const uint32_t n = rsCount.fetch_add(1) + 1;
            if (n <= 20 || (n % 1000) == 0)
                MCLA_LOG_INFO("RenderThread: SET_RENDER_STATE #{} dev={:08X} value={:08X}",
                              n, rs.deviceAddr, rs.value);
            break;
        }
        case RenderCommand::PRESENT: {
            if (!d3d12Initialized_) break;
            const auto& p = std::get<PresentCommand>(cmd.data);
            static StatCtr<uint32_t> presentCount{0};
            const uint32_t n = presentCount.fetch_add(1) + 1;
            // B4 (T41.4b): the 33 ms heartbeat enqueues exactly
            // {frameNumber=0, obj=0, swapInfo=0}, so it can be told apart from a
            // guest PresentKick without guessing. Counting them as PRESENT made
            // the marker measure our own timer, not the guest (w102: 60 PRESENT
            // lines, all heartbeat). The heartbeat line deliberately avoids the
            // substring "PRESENT" because soak_census counts matching LINES
            // (F-054 discipline): a guest-driven PRESENT count is now real.
            // F-610: that test is now the single IsHeartbeatPresent() in
            // render_queue.h, shared with the queue's shed policy so the two
            // cannot drift and disagree about which PRESENT is which.
            const bool heartbeat = IsHeartbeatPresent(cmd);
            if (!heartbeat) {
                STALL_STAGE("pr-harvest");
                mcla::native::D3D12Backend::ClearFrameTextures();
                (void)HarvestWriteWatchForRenderThread(true, p.enqNs);
            }
            // F-899: THE GUEST'S SWAP IS WHERE THE FRAME ENDS.
            //
            // MEASURED, build/g919.log: CP-DRAW-CENSUS enqueued=553,926 draw
            // commands, CP: XE_SWAP-EMIT #3400, and the host had presented 500
            // times in the WHOLE run with `drawn=1` -- because BeginFrame() was
            // called once per draw and its already-in-frame latch refused
            // 552,600 of them. The frame never ended, so nothing the guest drew
            // could reach the window.
            //
            // This is the boundary the backend never had: the accumulated list
            // (every draw since the last swap) is closed, resolved into the
            // scan-out surface and presented exactly once, HERE, where the
            // guest itself swaps. A heartbeat present is our own 33 ms timer,
            // not a guest frame, so it does not end the guest's frame.
            // F-610: one-way latch, declared ahead of present/blit so the success
            // path can set it. Once any real guest content has been drawn or blitted,
            // no synthetic fallback may ever paint again.
            static std::atomic<bool> s_guestContentSeen{false};

            if (!heartbeat) {
                STALL_STAGE("pr-flush");
                if (backend_.FlushDrawFrame())
                    s_1sFlushOk.fetch_add(1, std::memory_order_relaxed);
                else
                    s_1sFlushEmpty.fetch_add(1, std::memory_order_relaxed);
                backend_.PollPendingOcclusionQueries();
                // Subagent 1 fix: latch guest content seen so subsequent heartbeat
                // ticks do not paint fallback clears over the drawn frame (ga058).
                s_guestContentSeen.store(true, std::memory_order_relaxed);
            } else {
                backend_.PollPendingOcclusionQueries();
            }
            MaybeLogRender1s();
            if (n <= 10 || (n % 120) == 0) {
                // T43.18 (2026-10-13): mainwait=* is the TAIL-LIGHT for the
                // main thread's latest park (published uncapped by the
                // main-park probes; xdm.h). The probe rows cap at 200 early
                // in the run, so without this the TAIL of a soak is dark.
                // mainwait_age_ms = heartbeat-now minus stamp: large age =
                // stale stamp (thread moved on or died), small age = parked
                // there now. 0 handle = no main park observed yet.
                const uint32_t mwH =
                    g_mainParkHandle.load(std::memory_order_relaxed);
                const uint32_t mwLr =
                    g_mainParkLr.load(std::memory_order_relaxed);
                const uint64_t mwNs =
                    g_mainParkNs.load(std::memory_order_relaxed);
                const uint64_t mwAge =
                    (mwH != 0) ? static_cast<uint64_t>(NowNs() / 1000000) >= mwNs
                                     ? static_cast<uint64_t>(NowNs() / 1000000) - mwNs
                                     : 0u
                               : 0u;
                MCLA_LOG_INFO(
                    "RenderThread: {} #{} frame={} obj={:08X} fb={:08X} "
                    "surf={:08X} mainwait={:08X} mainwaitlr={:08X} "
                    "mainwait_age_ms={}",
                    heartbeat ? "HB-FLICKER" : "PRESENT", n, p.frameNumber,
                    p.obj, p.swapInfo, p.surfaceVA, mwH, mwLr, mwAge);
            }

            // F-610: stamp a guest present for the heartbeat's suppression
            // window. Without this the 30 Hz beat competes with a guest that
            // is already presenting and both are counted as the same signal.
            if (!heartbeat) {
                g_lastGuestPresentNs.store(NowNs(), std::memory_order_relaxed);
            }

            // F-610: the software per-PRESENT pacer (Sleep to kTargetFrameSeconds
            // here) is REMOVED. It was the third of three throttles stacked on
            // one frame: the 33 ms heartbeat thread, this Sleep, and
            // Present(1,0) in the backend (src/d3d12_backend.cpp:1496, :1922).
            // Present(1,0) blocks on the real vsync deadline, so the Sleep
            // could only add up to 33 ms of pure latency per guest frame on top
            // of a wait that already enforces the frame period. The heartbeat
            // cadence (kHeartbeatIntervalMs) is now the only software clock, and
            // the swapchain is the only frame deadline.

            // W5: sample average color for logs. W6: full-viewport blit of
            // the guest framebuffer when swapInfo is a high guest VA.
            // W7: if swapInfo is a swap-table slot (C71D81xx family or
            // samples as near-zero), use the resolved surface VA instead.
            float cr = 0.0f, cg = 0.0f, cb = 0.0f;
            bool blitOk = false;
            // F-762: set when the whole-surface census refused this VA. It is
            // separate from blitOk on purpose: blitOk=false means "no guest
            // pixels reached the buffer", and the block below answers that by
            // showing the measured clear colour. surfaceRefused means "the
            // bytes we read are not a surface", and the honest answer to THAT
            // is to touch nothing at all, so the two must not share a branch.
            bool surfaceRefused = false;
            bool coherent = false;

            // F-610: s_guestContentSeen is declared above ahead of FlushDrawFrame.

            // =================================================================
            // F-XXX (2026-10-21, fix_present2): WHICH SURFACE DOES THE GUEST'S
            // PRESENT NAME? What was here, and why it was structurally wrong.
            //
            // WHAT WAS HERE. `blitVA = p.swapInfo` -- the guest's PresentKick r4
            // -- behind a swap-table family test (0xC71D8180..0xC71D81E0) that
            // the guest never satisfies, so EVERY guest present read a 4-byte
            // flip-timestamp SLOT as a 1280x720 pixel buffer (3,686,400 bytes
            // out of the guest's heap). The census then refused every one of
            // them (GUEST-SURFACE-UNCOHERENT-REFUSED: 8 rows in build/w852.log,
            // 9 in build/w854.log) -- and THAT REFUSAL WAS RIGHT. The ADDRESS
            // SELECTION was wrong, not the census.
            //
            // THE GUEST'S OWN FACTS (measured; none of them inferred):
            //  * PresentKick's r4 is a swap-table SLOT, not a framebuffer. The
            //    HOST writes the flip timestamp into it:
            //    src/gpu_device.cpp:6680-6683 writes
            //    tableBase + (((queued+k)&7)+16)*4 with the timebase, and the
            //    guest's own bytes reserve the same two slots per kick.
            //    MEASURED: SWAP-FILL #3 table=B7AF0040; PRESENT-KICK-CENSUS
            //    fb=B7AF0080, B7AF0084, ... B7AF009C -- i.e. slot 16+n at
            //    0xB7AF0040 + (16+n)*4. The old family test looked for
            //    0xC71D8180..0xC71D81E0, which is not this table at all.
            //  * The console's own answer to "which surface is displayed" is the
            //    RB copy destination: RB_COPY_DEST_BASE (Xenos register 0x2319)
            //    is where the copy unit puts the rendered frame -- xenia
            //    xenos.h:911-915: "Xenos copies EDRAM contents to a tiled 2D or
            //    3D texture (resolves) by drawing primitives with the EDRAM mode
            //    EdramMode::kCopy" -- and the guest's VdSwap publishes that same
            //    buffer to the display engine (xenia
            //    xboxkrnl_video.cc:352-360: VdSwap's 8th argument is
            //    "lpdword_t frontbuffer_ptr, ptr to frontbuffer address", and
            //    the kernel turns it into the physical address it writes into
            //    the PM4_XE_SWAP packet at :424-429).
            //  * The RUNNING value is in the guest's own Xenos register file:
            //    mcla::gpu::CpGuestRenderTargetInfo() (src/gpu_cp.cpp:6713).
            //    MEASURED (the GUEST-RT-RESOLVE rows of build/w852.log and
            //    build/w854.log): RB_COPY_DEST_BASE=17E98000 and 185C8000 (two
            //    front buffers, alternating), RB_COPY_DEST_PITCH=02D00500 =>
            //    pitch 1280, height 720 (registers.h:887-897, 14 bits each),
            //    RB_MODECONTROL=6 (EdramMode::kCopy) or 4 (kColorDepth).
            //
            // SO THE BLIT SOURCE IS THE GUEST'S FRONT BUFFER, and its pool byte
            // comes from the ONE translation this tree has for a console
            // physical address: mcla::boot::TranslatePhysViewToPoolVa
            // (src/boot_host.h:222 -> src/boot_host.cpp:312-328), whose rule is
            // poolVa = kPhysLowRegionVa + pa (0xA0000000 + pa) for every pa in
            // the console's physical window.
            //
            // WHAT THIS DOES NOT CLAIM: it does not make the front buffer hold
            // a frame. This host does not model the RB copy unit, so the bytes
            // at RB_COPY_DEST_BASE are whatever the guest's allocator left
            // there. The census below therefore refuses them (a surface with a
            // single colour over its whole area is not evidence of a drawn
            // region -- see the SURFACE-EMPTY refusal), and a refusal touches
            // no memory.
            //
            // THE HEARTBEAT IS NOT A REFUSAL. IsHeartbeatPresent(cmd) is the
            // tree's single test for "this PRESENT is the host's own 33 ms
            // heartbeat" (F-610), so the heartbeat keeps its clear-only
            // behaviour and is NOT counted as a missing surface.
            // =================================================================
            uint32_t blitVA = 0;
            uint32_t blitPhys = 0;
            const char* blitSrc = "heartbeat-no-guest-present";
            if (!heartbeat) {
                blitSrc = "guest-stated-no-rb-copy-dest";
                const mcla::gpu::CpGuestRenderTarget rt =
                    mcla::gpu::CpGuestRenderTargetInfo();
                blitPhys = rt.copyDestBase;
                if (blitPhys != 0) {
                    if (blitPhys >= mcla::kernel::kConsolePhysicalBytes) {
                        // Outside the console's 512 MiB physical window: no byte
                        // anywhere in this tree can be this address, and the `|`
                        // below would corrupt it, so it is refused by name.
                        blitSrc = "guest-rb-copy-dest-outside-console-window";
                    } else {
                        uint32_t poolVa = 0;
                        // Register this site with the F-640 census ONCE, so the
                        // summary row's `adopted_sites` counts it and a
                        // `dropped=0` beside it is read as a measurement rather
                        // than as "nothing was ever routed here" (F-057(6)).
                        static const bool kRegistered = [] {
                            mcla::boot::RegisterPhysAliasWriter(
                                "render_thread.cpp:present-front-buffer");
                            return true;
                        }();
                        (void)kRegistered;
                        const mcla::boot::PhysViewResult r =
                            mcla::boot::TranslatePhysViewToPoolVa(
                                blitPhys | 0xC0000000u, &poolVa);
                        if (r == mcla::boot::PhysViewResult::kMapped) {
                            blitVA = poolVa;
                            blitSrc = "guest-rb-copy-dest";
                        } else {
                            blitSrc =
                                (r == mcla::boot::PhysViewResult::kNotAView)
                                    ? "guest-rb-copy-dest-not-a-view"
                                    : "guest-rb-copy-dest-no-pool-byte";
                        }
                    }
                }
                if (blitVA == 0) {
                    // No surface could be named. Nothing is blitted and nothing
                    // is substituted: the back buffer keeps the bytes it already
                    // holds and the window keeps what it was already showing.
                    // The row says WHICH of the named reasons applied, so a
                    // zero here is a counted refusal and not a silence.
                    surfaceRefused = true;
                    static StatCtr<uint64_t> noSurface{0};
                    const uint64_t sn = noSurface.fetch_add(1) + 1;
                    if (sn <= 8 || (sn % 120) == 0)
                        MCLA_LOG_WARN(
                            "PRESENT-SURFACE-NONE #{} src={} phys={:08X} "
                            "resolvedSurf={:08X} fb={:08X} -- NO SURFACE WAS "
                            "NAMED, so NOTHING WAS BLITTED AND NOTHING WAS "
                            "SUBSTITUTED: the back buffer keeps the bytes it "
                            "already holds and the window keeps what it was "
                            "already showing. The console names the displayed "
                            "surface in RB_COPY_DEST_BASE (Xenos register 0x2319, "
                            "read here from the guest's own register file) and "
                            "the guest's VdSwap publishes that same buffer to "
                            "the display engine. noSurface={}",
                            sn, blitSrc, blitPhys, p.surfaceVA, p.swapInfo, sn);
                }
            }
            // Note: the heartbeat leaves blitVA=0 -> clear-only, as before.

            if (blitVA != 0) {
                auto& guestMem = mcla::kernel::GuestMemoryHeap::Instance();
                // Prefer full 1280x720 linear 32bpp; fall back to 640x360 if
                // the larger ReadBytes is not valid (page still uncommitted).
                static std::vector<uint8_t> fbBuf;
                constexpr uint32_t kFullW = 1280, kFullH = 720;
                constexpr uint32_t kHalfW = 640, kHalfH = 360;
                constexpr uint32_t kFullBytes = kFullW * kFullH * 4;
                constexpr uint32_t kHalfBytes = kHalfW * kHalfH * 4;
                if (fbBuf.size() < kFullBytes) fbBuf.resize(kFullBytes);

                uint32_t fbW = 0, fbH = 0;
                STALL_STAGE("pr-read");
                if (guestMem.ReadBytes(blitVA, fbBuf.data(), kFullBytes)) {
                    fbW = kFullW;
                    fbH = kFullH;
                } else if (guestMem.ReadBytes(blitVA, fbBuf.data(), kHalfBytes)) {
                    fbW = kHalfW;
                    fbH = kHalfH;
                }

                if (fbW != 0) {
                    // =================================================================
                    // F-762: THE GATE. Refusing, by name, a surface whose bytes do
                    // not describe anything the guest drew.
                    //
                    // WHAT WAS HERE AND WHY IT GATED NOTHING. Directly below this
                    // block sat a 16x16 grid sampler (sampleCount = 16*16 = 256
                    // points, sx = x * (fbW / 16)). It read 256 of 921600 pixels
                    // = 0.0278% of the surface, averaged them into cr/cg/cb, and
                    // from that average derived a boolean named nearBlack. Two
                    // things are wrong with that and they are separate defects:
                    //
                    //   (a) IT GATED NOTHING. nearBlack's only consumer was a log
                    //       line. The upload call sat outside any test of it, so
                    //       3,686,400 bytes reached the back buffer on the
                    //       strength of a number that was only ever printed. The
                    //       real gate on this path is a frame-OWNERSHIP test
                    //       further down (does the draw path own the buffer), which
                    //       is a different question and answered "yes, go ahead".
                    //   (b) IT COULD NOT HAVE GATED ANYTHING. A mean of 256 points
                    //       estimates the surface's MODAL value, and the modal
                    //       value of never-written memory is zero, which is also
                    //       the modal value of a legitimately black frame. No
                    //       threshold on a modal estimate separates those two. The
                    //       measured disagreement is in the block comment on
                    //       GuestSurfaceCensus in d3d12_backend.h.
                    //
                    // WHAT REPLACES IT. A whole-surface census (every pixel, no
                    // stride, no sampling) and one placement-invariant statistic
                    // derived from it, because the log records which colours a
                    // surface holds but not which pixel holds which colour, so a
                    // geometric test would be answering a question nobody
                    // measured. rareFraction asks how much of the non-background
                    // mass sits in colours that occur at most 8 times in the
                    // whole surface. A drawn region is one colour repeated
                    // thousands of times and scores 0.000000; allocator debris
                    // scores 0.388073. Both numbers are read off rows in
                    // build/w717.log; the derivation and the measured error rate
                    // are on the struct.
                    //
                    // WHAT A REFUSAL DOES. Nothing. Not a substitute colour, not
                    // a held copy of the last good frame, not a clamp, not the
                    // placeholder constant -- charter F-609 forbids inventing a
                    // pixel and the placeholder is the synthetic image F-758
                    // deleted. surfaceRefused carries that decision past the
                    // fallback block below so the fallback is skipped entirely:
                    // the back buffer keeps the bytes it already holds and the
                    // window keeps what it was already showing, which is the only
                    // honest state when there is no guest content to put there.
                    // =================================================================
                    STALL_STAGE("pr-census");
                    GuestSurfaceCensus census{};
                    CensusGuestSurface(fbBuf.data(), fbW, fbH, &census);
                    coherent = GuestSurfaceIsCoherent(census);
                    // F-XXX (2026-10-21, fix_present2): AN EMPTY SURFACE IS NOT
                    // A SURFACE. GuestSurfaceIsCoherent ACCEPTS a uniform
                    // surface -- "one single value -> ACCEPT: a uniform
                    // surface. Black is a measurement, and there is no scattered
                    // content in it to be wrong about"
                    // (src/d3d12_backend.h:1684-1686) -- and that was safe
                    // while the surface being walked was the guest's
                    // flip-timestamp SLOT (3.7 MB of heap debris, refused for a
                    // different reason). It is NOT safe now that the source is
                    // the console's own front buffer: this host does not model
                    // the RB copy unit, so the bytes at RB_COPY_DEST_BASE are
                    // UNWRITTEN, the walk sees ONE colour over the whole
                    // 1280x720 area, the uniform branch would ACCEPT it, and
                    // the upload would then paint a BLANK surface over the frame
                    // the draw path had already resolved into the back buffer
                    // -- and nothing downstream would stop it, because
                    // PresentBgra RELEASES the draw-path latch for a guest
                    // PRESENT (src/d3d12_backend.cpp:6110-6118). A blank blit is
                    // worse than no blit, so the uniform branch REFUSES HERE,
                    // by name.
                    //
                    // FALSIFIER: a GUEST-SURFACE-COHERENT row with distinct=1
                    // on a surface the guest really drew would refute this. On
                    // the surfaces this log contains the measured distinct
                    // values are 2 (the drawn one) and 319 (the refused debris),
                    // so no MEASURED case changes; the only case that changes is
                    // the one the census never saw, because the old source was
                    // not a surface at all.
                    const bool emptySurface = (census.distinct <= 1);
                    if (coherent && emptySurface) {
                        coherent = false;
                    }
                    if (coherent) {
                        // Logged on the accepting side too, and deliberately: the
                        // threshold is calibrated on the two drawn surfaces this
                        // log contains and on nothing else, so every acceptance has
                        // to carry the numbers it was accepted on. A threshold
                        // moved on evidence needs the evidence in the log.
                        static StatCtr<uint64_t> coherentCount{0};
                        const uint64_t cn = coherentCount.fetch_add(1) + 1;
                        if (cn <= 8 || (cn % 120) == 0)
                            MCLA_LOG_INFO("GUEST-SURFACE-COHERENT #{} va={:08X} src={} "
                                          "srcPhys={:08X} wh={}x{} "
                                          "total={} distinct={} modalCount={} "
                                          "contaminated={} rareCount={} "
                                          "rareFraction={:.6f} rareFractionMax={:.6f} "
                                          "-- ACCEPTED for upload: every pixel was "
                                          "walked (stride 1, no sampling) and this "
                                          "fraction of the non-background mass sits "
                                          "in colours occurring at most {} times in "
                                          "the whole surface. contaminated=0 is the "
                                          "uniform branch, where there is no "
                                          "scattered mass to be wrong about",
                                          cn, blitVA, blitSrc, blitPhys, fbW, fbH,
                                          census.total,
                                          census.distinct, census.modalCount,
                                          census.contaminated, census.rareCount,
                                          census.rareFraction,
                                          kGuestCensusRareFractionMax,
                                          kGuestCensusRareCount);
                    } else {
                        surfaceRefused = true;
                        // F-XXX (2026-10-21, fix_present2): WHY, IN ONE WORD.
                        // The old prose asserted "scattered bright debris"
                        // unconditionally, which is FALSE for a surface that
                        // holds a single colour: that is an EMPTY surface, not
                        // debris, and the two have different causes and need
                        // different words. The distinction is MEASURED, not
                        // asserted: distinct == 1 means one rgba value over the
                        // whole 1280x720 area, so the bytes hold no region at
                        // all. That is the guest's front-buffer case on this
                        // host, because the RB copy unit is not modelled and
                        // nothing is ever written there.
                        const char* why =
                            emptySurface ? "SURFACE-EMPTY" : "SURFACE-DEBRIS";
                        static StatCtr<uint64_t> uncoherentCount{0};
                        const uint64_t rn = uncoherentCount.fetch_add(1) + 1;
                        if (rn <= 8 || (rn % 120) == 0)
                            MCLA_LOG_ERROR(
                                "GUEST-SURFACE-UNCOHERENT-REFUSED #{} va={:08X} "
                                "src={} srcPhys={:08X} why={} wh={}x{} "
                                "total={} distinct={} distinctOverflow={} modalCount={} "
                                "contaminated={} rareCount={} rareFraction={:.6f} "
                                "rareFractionMax={:.6f} rareCountMax={} "
                                "distinctNonModal={} largestNonModal={} "
                                "pxPerNonModalColour={} -- REFUSED, and this is the "
                                "honest answer rather than a fix: the why= field "
                                "names which of the two MEASURED shapes this "
                                "surface has. SURFACE-EMPTY means one colour over "
                                "the whole surface (distinct={}), so those bytes "
                                "hold no region at all -- nothing was drawn there. "
                                "SURFACE-DEBRIS means {} of {} non-background "
                                "pixels sit in colours occurring at most {} times "
                                "in the whole surface, i.e. scattered bright debris "
                                "rather than a region the guest drew. In both "
                                "cases {} of the surface was walked with "
                                "no stride. NOTHING WAS BLITTED AND NOTHING WAS "
                                "SUBSTITUTED: the back buffer keeps the bytes it "
                                "already holds and the window keeps what it was "
                                "already showing. Not a substitute colour, not a "
                                "held copy of the last good frame, not a clamp, and "
                                "not the placeholder constant. A previous sampler "
                                "read {} of {} pixels on this same path and its "
                                "mean could not have separated this surface from a "
                                "black one, because a mean estimates the modal "
                                "value and both surfaces are mostly modal zero. "
                                "distinctOverflow or a zero-pixel surface refuses "
                                "for the same reason: a full histogram cannot prove "
                                "uniformity and an empty walk measured nothing",
                                rn, blitVA, blitSrc, blitPhys, why, fbW, fbH,
                                census.total, census.distinct,
                                census.distinctOverflow, census.modalCount,
                                census.contaminated, census.rareCount,
                                census.rareFraction, kGuestCensusRareFractionMax,
                                kGuestCensusRareCount, census.distinctNonModal,
                                census.largestNonModal,
                                census.distinctNonModal
                                    ? census.contaminated / census.distinctNonModal
                                    : 0,
                                census.distinct, census.contaminated, census.total,
                                kGuestCensusRareCount, census.total,
                                static_cast<uint64_t>(16u * 16u), census.total);
                    }
                }

                if (fbW != 0) {
                  if (coherent) {
                    uint64_t sumR = 0, sumG = 0, sumB = 0;
                    const uint32_t sampleCount = 16 * 16;
                    for (uint32_t y = 0; y < 16; ++y) {
                        for (uint32_t x = 0; x < 16; ++x) {
                            const uint32_t sx = x * (fbW / 16);
                            const uint32_t sy = y * (fbH / 16);
                            const uint32_t off = (sy * fbW + sx) * 4;
                            // LE 32bpp in guest memory: treat as RGBA bytes.
                            sumR += fbBuf[off + 0];
                            sumG += fbBuf[off + 1];
                            sumB += fbBuf[off + 2];
                        }
                    }
                    cr = static_cast<float>(sumR / sampleCount) / 255.0f;
                    cg = static_cast<float>(sumG / sampleCount) / 255.0f;
                    cb = static_cast<float>(sumB / sampleCount) / 255.0f;
                    // F-610: the near-black clamp that used to live here
                    // (0.02 -> dark blue) is REMOVED. It rewrote the measured
                    // colour, so a genuinely black guest frame and a frame that
                    // never rendered were reported as the same dark blue, in
                    // the log AND in the fallback clear. cr/cg/cb now carry the
                    // sampled value unmodified; near-black is flagged as a
                    // separate signal instead of being repainted.
                    const bool nearBlack = (cr + cg + cb) < 0.02f;
                    if (nearBlack) {
                        static StatCtr<uint32_t> blackCount{0};
                        const uint64_t bn = blackCount.fetch_add(1) + 1;
                        if (bn <= 8 || (bn % 240) == 0)
                            MCLA_LOG_INFO("PRESENT-FB NEAR-BLAME #{} va={:08X} "
                                          "rgb=({:.3f},{:.3f},{:.3f}) — sampled as "
                                          "black, NOT rewritten",
                                          bn, blitVA, cr, cg, cb);
                    }
                    static StatCtr<uint32_t> presentFbCount{0};
                    const uint32_t pfn = presentFbCount.fetch_add(1) + 1;
                    if (pfn <= 16 || (pfn % 60) == 0)
                        MCLA_LOG_INFO("PRESENT-FB sample rgb=({:.2f},{:.2f},{:.2f}) va={:08X} "
                                          "src={} phys={:08X} (fb={:08X}) wh={}x{}",
                                      cr, cg, cb, blitVA, blitSrc, blitPhys,
                                      p.swapInfo, fbW, fbH);

                    STALL_STAGE("pr-blit");
                    if (!backend_.PresentBgra(fbW, fbH, fbBuf.data())) {
                        static StatCtr<uint32_t> blitFail{0};
                        const uint32_t fn = blitFail.fetch_add(1) + 1;
                        if (fn <= 5 || (fn % 120) == 0)
                            MCLA_LOG_WARN("PRESENT-BLIT fail #{} va={:08X} src={} "
                                          "phys={:08X} wh={}x{}",
                                          fn, blitVA, blitSrc, blitPhys, fbW, fbH);
                    } else {
                        STALL_STAGE("pr-tail");
                        blitOk = true;
                        // F-610: latch the gradient fallback off for good. From
                        // here on the fallback path can only clear, never paint
                        // synthetic pixels over a guest frame.
                        s_guestContentSeen.store(true, std::memory_order_relaxed);
                        static StatCtr<uint32_t> blitCount{0};
                        const uint32_t bn = blitCount.fetch_add(1) + 1;
                        if (bn <= 10 || (bn % 60) == 0)
                            MCLA_LOG_INFO("PRESENT-BLIT #{} {}x{} va={:08X} src={} "
                                          "phys={:08X} (fb={:08X})",
                                          bn, fbW, fbH, blitVA, blitSrc, blitPhys,
                                          p.swapInfo);
                    }
                  }
                } else if (n <= 8 || (n % 240) == 0) {
                    // F-762: this arm is reached ONLY when fbW == 0, i.e. the
                    // guest read itself failed. It used to hang off the same
                    // condition as the upload, and with the census gate in
                    // between that would have made a REFUSED surface report
                    // "read failed" -- a false statement about a read that
                    // succeeded and returned 3,686,400 bytes. The two are now
                    // nested so the arm keeps meaning exactly what it says.
                    MCLA_LOG_WARN("PRESENT-FB read failed va={:08X} src={} "
                                  "phys={:08X} (fb={:08X})",
                                  blitVA, blitSrc, blitPhys, p.swapInfo);
                }
            }

            // F-762: surfaceRefused short-circuits this whole block. Everything
            // below answers "no guest pixels arrived", and its answer is to show
            // the measured clear colour, which is a SUBSTITUTION for a surface
            // that was read successfully and found not to be one. For a refusal
            // there is no measured colour to show and nothing to substitute, so
            // the frame is left alone: no upload, no clear, no synthetic image,
            // and the back buffer keeps the bytes it already holds. That is the
            // same contract the ledger-driven refusal downstream already uses,
            // for the same reason.
            if (!blitOk && !surfaceRefused) {
                // F-610: the gradient fallback is GATED on a one-way latch. It
                // used to run on every present with no blitVA (heartbeat) or a
                // failed read, which meant a guest frame that was genuinely
                // black, or a guest frame whose surface read failed, got a
                // synthetic image painted over it — the one case where a
                // diagnostic must not be allowed to speak. Now: once ANY real
                // guest framebuffer has blitted successfully (s_guestContentSeen,
                // set at the PRESENT-BLIT success below and never cleared), the
                // gradient is never painted again; the fallback degrades to a
                // plain clear of the MEASURED colour, so black stays black and a
                // read failure stays a read failure (already logged as
                // PRESENT-FB read failed). SYNTHETIC: REMOVAL TRIGGER: delete
                // this whole block once every present carries a readable
                // surface VA, i.e. no soak ever hits PRESENT-FB read failed and
                // blitVA is never 0 outside the heartbeat.
                if (s_guestContentSeen.load(std::memory_order_relaxed)) {
                    // F-725: THE REDUNDANT FRAME IS GONE, and the measurement is
                    // structural rather than statistical.
                    //
                    // This used to be:
                    //     if (backend_.BeginFrame()) {
                    //         backend_.ClearAndPresent(cr, cg, cb, 1.0f);
                    //         backend_.AbandonFrame("present-fallback-clear");
                    //     }
                    //
                    // ClearAndPresent's FIRST act is MayWriteBackBuffer
                    // (d3d12_backend.cpp:3918), which is where a clear that loses
                    // to the draw path is refused and counted. Its BeginFrame()
                    // is at :3921, i.e. AFTER the refusal test, and it is
                    // guarded by `if (!m_inFrame)`. So once a draw has presented:
                    //     outer BeginFrame()   -> succeeds, sets m_inFrame
                    //     ClearAndPresent()    -> refused at :3918, returns false,
                    //                            having reached neither its own
                    //                            BeginFrame nor any command
                    //     outer AbandonFrame()  -> closes the list, releases the
                    //                            latch, counts an abandon
                    // The frame that was opened had no recorded command in it and
                    // was never executed. It is a pure open/close pair, once per
                    // heartbeat present, forever.
                    //
                    // MEASURED on build/w699.log. FRAME-OWNER REFUSED runs
                    // #1..#4800 at 30.058/s (05:28:28.696 -> 05:31:08.364,
                    // 159.668 s) and D3D12-FRAME-ABANDONED #200..#5000 at
                    // 30.055/s (05:28:34.449 -> 05:31:14.160, 159.711 s). Those
                    // two rates are the same to three decimal places and the two
                    // counters are the same magnitude, so one refused clear
                    // produced exactly one abandon, every time. The `beganFrame`
                    // field proves the accounting closes: 5381-581 = 4800
                    // BeginFrame successes across exactly 5000-200 = 4800
                    // abandons.
                    //
                    // The rate is not a spin and does not have to be treated as
                    // one: it is exactly the heartbeat present cadence
                    // (kHeartbeatIntervalMs = 33 ms -> 30.3 Hz), one iteration
                    // per tick, each completing and returning to the timer. So
                    // this is BENIGN -- it neither livelocks nor starves -- and
                    // it is also 30 BeginFrame + command-allocator-Reset +
                    // command-list-Reset + Close round trips per second that
                    // record nothing. That is the waste being removed.
                    //
                    // WHAT IS NOT CHANGED. MayWriteBackBuffer still refuses the
                    // clear and still counts it, and FRAME-OWNER REFUSED still
                    // prints on exactly the same occasions with exactly the same
                    // numbers. Nothing about the precedence between the draw and
                    // the clear is touched: `writer=clear may not touch the back
                    // buffer; owner=draw` remains the outcome, and a clear can
                    // still never destroy a drawn frame. ClearAndPresent opens
                    // and closes its own frame -- it calls BeginFrame at :3921
                    // when none is open and PresentOwnedFrame releases
                    // m_inFrame -- so it never needed a caller-side pair, and
                    // calling it bare is correct whether the clear is allowed or
                    // refused.
                    if (backend_.ClearAndPresent(cr, cg, cb, 1.0f))
                        s_1sClearOk.fetch_add(1, std::memory_order_relaxed);
                    else
                        s_1sClearRefused.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                // F-758: THE SYNTHETIC GRADIENT IS DELETED, not gated.
                //
                // WHAT IT WAS. F-567/F-569 painted a host-generated 1280x720 gradient
                // ("pre-guest-content only. Proves the display pipeline end-to-end while
                // the real rendering path is built") into the swapchain on the !blitOk
                // branch. It is a synthetic image painted over the guest's window, which
                // charter F-609 forbids in the boot path -- and it was the ONLY thing the
                // user could see.
                //
                // MEASURED in build/w713.log, 150 s soak:
                //   PRESENT-BLIT-SYNTH ok 1280x720 (gradient)   1 row
                //   PRESENT-BLIT #                             2 rows
                //   PRESENT-GRADIENT #1..#4, #240              5 rows = 240 paints
                //   FRAME-OWNER #1..#6 owner=blit drawn=0     drawn=0 on EVERY row
                // The guest's draw DID rasterise into the back buffer (DRAW-CP-DRAWN 4, and
                // the census saw it: distinctRgba=2, changedPx=230400,
                // changedBox=x0..639, changedCols=640) and was still never shown. The
                // gradient is what the window displayed.
                //
                // F-610 LATCHED THE WRONG BRANCH. Its s_guestContentSeen guard sits at
                // render_thread.cpp:1671, INSIDE this same `if (!blitOk)` and above this
                // code, and it breaks. So it only suppresses the gradient once a real
                // guest blit has SUCCEEDED -- and on this boot that essentially never
                // happens, so the latch never arms and the gradient runs 240 times. One
                // fallback was gated and the other left running.
                //
                // WHAT REPLACES IT. Nothing synthetic. The fallback presents the MEASURED
                // colour (cr/cg/cb, sampled from guest memory at :1596-1607 and
                // deliberately unclamped since F-610), which is exactly what the sibling
                // branch above already does. Nothing is drawn over the guest's window, and
                // "no surface was presented" is visible in the log instead of being
                // papered over with a picture the guest never drew.
                //
                // REMOVAL TRIGGER: none. This is a synthetic image standing in for no
                // guest state at all; restoring it would reintroduce the F-609
                // violation.
                static StatCtr<uint32_t> noSurface{0};
                const uint32_t ns = noSurface.fetch_add(1) + 1;
                if (backend_.BeginFrame()) {
                    if (backend_.ClearAndPresent(cr, cg, cb, 1.0f))
                        s_1sClearOk.fetch_add(1, std::memory_order_relaxed);
                    else
                        s_1sClearRefused.fetch_add(1, std::memory_order_relaxed);
                    // Reached whenever the clear itself is refused, so this is a
                    // refusal-shaped path and the frame latch must be released on it
                    // explicitly -- the same reasoning the deleted gradient block gave.
                    backend_.AbandonFrame("present-no-surface");
                }
                if (ns <= 4 || (ns % 240) == 0) {
                    MCLA_LOG_WARN("PRESENT-SURFACE-UNREADABLE #{} -- no guest surface was "
                                   "presented this frame; the window shows the measured "
                                   "clear cr={:.4f} cg={:.4f} cb={:.4f}, NOT guest pixels. "
                                   "The synthetic gradient that used to stand here is "
                                   "deleted (F-758); nothing is drawn over the guest's "
                                   "window any more.",
                                   ns, cr, cg, cb);
                }
            }
            break;
        }
        case RenderCommand::DRAW_CAPTURED: {
            if (!d3d12Initialized_) break;
            static thread_local D3D12Backend::PreparedDraw s_serialPrep;
            static thread_local DrawStage1Meta s_serialMeta;
            (void)HarvestWriteWatchForRenderThread();
            PrepareDrawStage1(cmd, s_serialPrep, s_serialMeta);
            RecordPreparedDraw(cmd, s_serialPrep, s_serialMeta);
            break;
        }
        case RenderCommand::DRAW_MARKER: {
            if (!d3d12Initialized_) break;
            const auto& mk = std::get<DrawMarkerCommand>(cmd.data);
            static StatCtr<uint32_t> markerCount{0};
            const uint32_t n = markerCount.fetch_add(1) + 1;
            if (n <= 10 || (n % 500) == 0)
                MCLA_LOG_INFO("RenderThread: DRAW-MARKER #{} seq={} prim={} numIdx={}",
                              n, mk.seq, mk.prim, mk.numIdx);

            // Synthetic quad on a 40x23 grid, ~2% cells, hue by seq. Test
            // layout (POS float3 + COL float4, stride 28) so the test PSO
            // accepts it. No clear after (T41.8 defect class); no guest
            // memory touched - SYNTHETIC pipe proof, see DrawMarkerCommand.
            struct MarkerVertex { float pos[3]; float color[4]; };
            const float gx = static_cast<float>(mk.seq % 40u) / 40.0f * 2.0f - 1.0f;
            const float gy = static_cast<float>((mk.seq / 40u) % 23u) / 23.0f * 2.0f - 1.0f;
            const float s = 0.018f;
            const float h = static_cast<float>(mk.seq % 7u) / 7.0f;
            const MarkerVertex verts[4] = {
                { { gx - s, gy - s, 0.0f }, { h, 1.0f - h, 0.35f, 1.0f } },
                { { gx + s, gy - s, 0.0f }, { 1.0f - h, h, 0.35f, 1.0f } },
                { { gx + s, gy + s, 0.0f }, { 0.35f, h, 1.0f - h, 1.0f } },
                { { gx - s, gy + s, 0.0f }, { h, 0.35f, 1.0f - h, 1.0f } },
            };
            static const uint16_t quadIdx[6] = { 0, 1, 2, 0, 2, 3 };

            D3D12Backend::DynamicMeshDesc desc = {};
            desc.vertexBytes = verts;
            desc.vertexBytesSize = sizeof(verts);
            desc.vertexStride = sizeof(MarkerVertex);
            desc.vertexCount = 4;
            desc.indexed = true;
            desc.indexBytes = quadIdx;
            desc.indexBytesSize = sizeof(quadIdx);
            desc.indexFormat = DXGI_FORMAT_R16_UINT;
            desc.indexCount = 6;

            // F-725: this BeginFrame() and AbandonFrame() pair is REMOVED, and the
            // measurement for removing them is that neither call has an effect
            // this path can use.
            backend_.DrawDynamicMesh(desc);
            break;
        }
        case RenderCommand::RESOLVE_GUEST: {
            if (!d3d12Initialized_) break;
            (void)backend_.ExecuteGuestResolve(std::get<ResolveCommand>(cmd.data));
            break;
        }
        case RenderCommand::CLEAR_GUEST: {
            if (!d3d12Initialized_) break;
            backend_.ExecuteGuestClear(std::get<ClearCommand>(cmd.data));
            break;
        }
        case RenderCommand::QUERY_OP: {
            static const bool s_noOccQuery = [] {
                const char* e = std::getenv("MCLA_NO_OCCLUSION_QUERY");
                const char* e2 = std::getenv("MCLA_NO_HW_ZPD");
                return (e && e[0] == '1') || (e2 && e2[0] == '1');
            }();
            if (!d3d12Initialized_ || s_noOccQuery) break;
            (void)backend_.ExecuteQueryOp(std::get<QueryOpCommand>(cmd.data));
            break;
        }
        case RenderCommand::RESIZE: {
            if (!d3d12Initialized_) break;
            const auto& r = std::get<ResizeCommand>(cmd.data);
            MCLA_LOG_INFO("RenderThread: RESIZE {}x{}", r.width, r.height);
            backend_.Resize(r.width, r.height);
            break;
        }
        case RenderCommand::DRAW_INDEXED:
        case RenderCommand::SET_VERTEX_BUFFERS:
        case RenderCommand::SET_INDEX_BUFFER:
        case RenderCommand::NOOP:
            break;
    }
}

// Record half of a DRAW_CAPTURED: everything after PrepareDrawStage1 (R1 geometry promote + the D3D12 draw
// + accounting). Shared by the serial path and the MCLA_RENDER_2STAGE record thread.
void RenderThread::RecordPreparedDraw(const RenderCommand& cmd, D3D12Backend::PreparedDraw& prep,
                                      DrawStage1Meta& meta) {
    if (prep.valid) {
        if (prep.foldedClearRect) {
            STALL_STAGE("drawcall");
            if (backend_.ExecuteFoldedClearRect(prep)) {
                s_1sDrawn[meta.primSlot].fetch_add(1, std::memory_order_relaxed);
                s_cpDrawn.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            const uint32_t sc = (prep.desc.streamCount > 0 && prep.desc.streamCount <= 4u)
                                    ? prep.desc.streamCount
                                    : 1u;
            for (uint32_t s = 0; s < sc; ++s) {
                if (prep.vbCleanHit[s] || prep.vbCheckPersistent[s]) {
                    const void* vp = (s == 0) ? (prep.vb.empty() ? nullptr : prep.vb.data())
                                              : (prep.vbStreamsExtra[s - 1].empty() ? nullptr : prep.vbStreamsExtra[s - 1].data());
                    const uint32_t vsz = prep.vbCleanHit[s]
                        ? prep.desc.residentVbBytes[s]
                        : ((s == 0) ? static_cast<uint32_t>(prep.vb.size())
                                    : static_cast<uint32_t>(prep.vbStreamsExtra[s - 1].size()));
                    const D3D12_GPU_VIRTUAL_ADDRESS gva = ResolveOrPromoteR1Geo(
                        backend_, prep.vbPersistentKey[s], prep.vbContentHash[s],
                        vp, vsz, prep.streamStrides[s],
                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        prep.vbBasePage[s], prep.vbPageCount[s], prep.vbEpochSnap[s],
                        0u, prep.vbCleanHit[s]);
                    if (gva != 0) {
                        prep.desc.residentVbGpu[s] = gva;
                        prep.desc.residentVbBytes[s] = prep.vbCleanHit[s] ? prep.desc.residentVbBytes[s] : vsz;
                        prep.desc.residentVbStride[s] = prep.streamStrides[s];
                    } else if (prep.vbCleanHit[s]) {
                        prep.desc.residentVbGpu[s] = 0;
                    }
                }
            }
            if (prep.desc.indexed && (prep.ibCleanHit || prep.ibCheckPersistent)) {
                const uint32_t isz = prep.ibCleanHit ? prep.desc.residentIbBytes
                                                             : static_cast<uint32_t>(prep.ib.size());
                const D3D12_GPU_VIRTUAL_ADDRESS igva = ResolveOrPromoteR1Geo(
                    backend_, prep.ibPersistentKey, prep.ibContentHash,
                    prep.ib.empty() ? nullptr : prep.ib.data(),
                    isz, 0u, D3D12_RESOURCE_STATE_INDEX_BUFFER,
                    prep.ibBasePage, prep.ibPageCount, prep.ibEpochSnap,
                    meta.tcMaxIndex, prep.ibCleanHit);
                if (igva != 0) {
                    prep.desc.residentIbGpu = igva;
                    prep.desc.residentIbBytes = isz;
                } else if (prep.ibCleanHit) {
                    prep.desc.residentIbGpu = 0;
                }
            }
            STALL_STAGE("drawcall");
            const uint64_t cpDrawCallsBefore = backend_.Stats().drawsIssued;
            const bool beganFrame = backend_.BeginFrameForDraw();
            const bool drew = beganFrame
                ? backend_.DrawDynamicMeshWithPipeline(prep.desc, prep.pso)
                : false;
            const bool cpIssuedDrawCall =
                beganFrame && (backend_.Stats().drawsIssued != cpDrawCallsBefore);
            if (!drew && backend_.HasActiveOcclusionQueries()) {
                static std::atomic<uint32_t> s_od{0};
                const uint32_t n = ++s_od;
                if (n <= 16 || (n % 500) == 0)
                    MCLA_LOG_WARN("OCC-DRAW-DROP #{} why=backend-refused beganFrame={} refuseLine={}",
                                  n, beganFrame ? 1 : 0, backend_.DrawRefuseLine());
            }
            FinalizeDrawAccounting(cmd, prep, meta, beganFrame, drew, cpIssuedDrawCall);
        }
    } else if (backend_.HasActiveOcclusionQueries()) {
        static std::atomic<uint32_t> s_os{0};
        const uint32_t n = ++s_os;
        if (n <= 16 || (n % 500) == 0)
            MCLA_LOG_WARN("OCC-DRAW-DROP #{} why=stage1-invalid (PrepareDrawStage1 left prep.valid=false)", n);
    }
    prep.ResetForReuse();
    meta.Reset();
}

void RenderThread::PrepareDrawStage1(const RenderCommand& cmd,
                                     D3D12Backend::PreparedDraw& prep,
                                     DrawStage1Meta& meta) {
    const uint64_t preSeq = prep.drawSeq;
    prep.ResetForReuse();
    if (preSeq != 0u) prep.drawSeq = preSeq;
    prep.commandOrder = cmd.commandOrder;
    prep.desc.commandOrder = cmd.commandOrder;
    meta.Reset();
    struct Stage1TimerGuard {
        ~Stage1TimerGuard() {
            if (t_isRenderThread && g_tempPerfStages) TempPerfStagePause();
        }
    } stage1TimerGuard;
    do {
            const auto& dc = std::get<DrawCapturedCommand>(cmd.data);
            mcla::renderer::PipelineState derived{};
            bool applyClipDisableRescaleForDraw = false;
            const uint32_t n = prep.drawSeq
                                   ? static_cast<uint32_t>(prep.drawSeq)
                                   : (g_stage1DrawCount.fetch_add(1, std::memory_order_relaxed) + 1u);
            prep.drawSeq = n;
            const int64_t drawTopNs = NowNs();
            if (t_isRenderThread) {
                g_consumerDrawSeq.store(dc.cpSeq, std::memory_order_relaxed);
                g_consumerDrawNs.store(drawTopNs, std::memory_order_relaxed);
                MaybeLogRender1s(drawTopNs);
            }
            uint32_t& primSlot = meta.primSlot;
            primSlot = (dc.primitiveTopology < 16u) ? dc.primitiveTopology : 15u;
            if (primSlot != 4u && primSlot != 6u && primSlot != 8u)
                s_1sLastOtherPrim.store(dc.primitiveTopology, std::memory_order_relaxed);
            s_1sConsumed[primSlot].fetch_add(1, std::memory_order_relaxed);
            STALL_STAGE("draw-top");
            if (n <= 10 || (n % 500) == 0)
                MCLA_LOG_INFO("RenderThread: DRAW_CAPTURED #{} op=0x{:02X} src={} "
                              "prim={} ib={:08X}+{}B fmt={}B swap={} numIdx={} "
                              "idxOff={:08X} vb={:08X}+{}B stride={} "
                              "vbFmt=vf{}/dxgi{}/{}B descs={}/{} "
                              "psoHash={:016X}",
                              n, dc.opcode, dc.sourceSelect, dc.primitiveTopology,
                              dc.ibBase, dc.ibSize, dc.ibFmt, dc.ibSwap,
                              dc.numIndices, dc.idxOffset, dc.vbBase, dc.vbSize,
                              dc.vbStride, dc.vbFormat.xenos, dc.vbFormat.dxgi,
                              dc.vbFormat.bytes, dc.vbFullCount, dc.vbDescCount,
                              dc.psoHash);

            auto& guestMem = mcla::kernel::GuestMemoryHeap::Instance();

            // --- F-652: program + raster-state census, before every gate ----
            // Placed with the rest of the census block, above every refusal
            // gate, so the row reports the class of the draw that is ABOUT to be
            // judged and not only of the ones that got far enough to be refused
            // downstream. Bounding the log (this row prints 4 then every 200th)
            // must not bound the census, so every number here is a process-
            // lifetime total that keeps incrementing whether or not this line
            // ever prints again.
            {
                const auto& rs = dc.rasterState;
                const bool vsAddr = (dc.vsProgram.present != 0u &&
                                     dc.vsProgram.embedded == 0u);
                const bool psAddr = (dc.psProgram.present != 0u &&
                                     dc.psProgram.embedded == 0u);
                (vsAddr ? s_cpProgVsAddr : s_cpProgVsAbsent)
                    .fetch_add(1, std::memory_order_relaxed);
                (psAddr ? s_cpProgPsAddr : s_cpProgPsAbsent)
                    .fetch_add(1, std::memory_order_relaxed);
                const bool c0 =
                    (rs.presentMask & (1u << CpSnapshotReg::kCpSnapPaClipCntl)) != 0u;
                const bool c1 = (rs.presentMask &
                                 (1u << CpSnapshotReg::kCpSnapPaSuScModeCntl)) != 0u;
                const bool c2 =
                    (rs.presentMask & (1u << CpSnapshotReg::kCpSnapPaClVteCntl)) != 0u;
                if (c0) s_cpRsCovPaClipCntl.fetch_add(1, std::memory_order_relaxed);
                if (c1) s_cpRsCovPaSuScModeCntl.fetch_add(1, std::memory_order_relaxed);
                if (c2) s_cpRsCovPaClVteCntl.fetch_add(1, std::memory_order_relaxed);
                if (c0 && c1 && c2) s_cpRsAll3.fetch_add(1, std::memory_order_relaxed);
            }

            // F-611: one bounded, fully identified refusal line per reason.
            // Never a synthetic fallback - if a field is missing the draw stops
            // here and says which field, so the next step is obvious and a soak
            // can count the reason (F-609, native-only charter).
            auto refuse = [&](const char *what, StatCtr<uint32_t> &c,
                              const char *detail) {
                s_1sSkipped[primSlot].fetch_add(1, std::memory_order_relaxed);
                const uint32_t k = c.fetch_add(1, std::memory_order_relaxed) + 1;
                if (k <= 8 || (k % 200) == 0) {
                    MCLA_LOG_ERROR("TEMP-DRAW-REFUSE #{} what={} draw={} prim={} ib={:08X}+{}B "
                                   "numIdx={} vb={:08X}+{}B stride={} vsProg={:08X}/p{}/{}d {}",
                                   k, what, n, dc.primitiveTopology, dc.ibBase, dc.ibSize, dc.numIndices,
                                   dc.vbBase, dc.vbSize, dc.vbStride, dc.vsProgram.guestAddr, dc.vsProgram.present, dc.vsProgram.dwords, detail);
                    MCLA_LOG_WARN("DRAW-CP-REFUSE #{} what={} draw={} ib={:08X}+{}B "
                                  "numIdx={} vb={:08X}+{}B stride={} {}",
                                  k, what, n, dc.ibBase, dc.ibSize, dc.numIndices,
                                  dc.vbBase, dc.vbSize, dc.vbStride, detail);
                }
            };

            // F-900 DISCRIMINATOR (env-gated, default DRAWS): prim=8
            // rectangle-list draws (draw #1 class: embedded VS, fullscreen?
            // background). The F-917 mitigation SKIPPED these by default to
            // hide a black quarter-screen box. With the CPU expansion below
            // they draw, so the default is now to draw. Set
            // MCLA_SKIP_PRIM8=1 explicitly to skip them (opt-out).
            {
                static const int s_skipPrim8 = [] {
                    const char* e = std::getenv("MCLA_SKIP_PRIM8");
                    // Default draws. =1 skips (old F-917 behaviour on demand);
                    // unset or =0 draws. =0 must not crash: it just draws, the
                    // same as the default.
                    return (e && e[0] == '1') ? 1 : 0;
                }();
                if (s_skipPrim8 && dc.primitiveTopology == 8u) {
                    static StatCtr<uint32_t> s_cpRefuseSkipPrim8{0};
                    refuse("skip-prim8-debug", s_cpRefuseSkipPrim8,
                           "- MCLA_SKIP_PRIM8=1: prim=8 draw skipped to test "
                           "whether the rect background covers the quads.");
                    break;
                }
            }

            // F-648: the AUTHORITATIVE total row for this gate. Every counter
            // the DRAW_CAPTURED path keeps is printed here, including the ones
            // whose per-reason DRAW-CP-REFUSE line is suppressed.
            //
            // Why this row had to be added rather than reusing refuse(): refuse()
            // prints rows 1..8 and then every 200th, so a reason whose count is
            // 137 leaves 129 unprinted firings and a reason whose count is 0
            // leaves none - the log alone cannot tell "never fired" from "fired
            // an awkward number of times". Bounding a log must not bound a
            // census. The producer solved exactly this with CP-DRAW-CENSUS
            // (src/gpu_cp.cpp, "Bounding the log must not bound the census, so
            // the totals live here and are printed every 200th draw"); this is
            // the consumer half, which never had one.
            //
            // It is placed before every gate so the row reports the class of the
            // draw that is about to be judged, not only of the ones that got
            // far enough to reach a later gate.
            static StatCtr<uint32_t> s_cpCensus{0};
            const uint32_t cc = s_cpCensus.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (cc <= 4u || (cc % 200u) == 0u) {
                // F-652: the PC-DERIVE row is the one that answers "was a
                // pipeline ever REQUESTED, and what happened to the ones that
                // were". It is a separate line, not extra columns on this one,
                // for the same reason F-648 put topologyAssigned next to drawn:
                // a question about a different stage of the chain must not be
                // answered by a number about another one. Every refusal bit has
                // its OWN total (s_deriveRefusedByBit, 32 slots, one per
                // renderer::PipelineRefusal bit), so this row is a histogram of
                // named reasons and never a lumped "refused=N".
                uint32_t ds = s_deriveAttempted.load(std::memory_order_relaxed);
                uint32_t dok = s_deriveOk.load(std::memory_order_relaxed);
                uint32_t drf = s_deriveRefused.load(std::memory_order_relaxed);
                // The aggregate refuseMask over the whole run, OR-ed at the
                // refusal site (lock-free fetch_or), so the row can be formatted
                // through the ONE owner of the name table and a soak reads real
                // names even if it kept only the last 200th copy of this line.
                const uint32_t maskSoFar =
                    s_deriveMaskOr.load(std::memory_order_relaxed);
                char reasons[512] = {};
                mcla::renderer::FormatPipelineRefusal(maskSoFar, reasons,
                                                      sizeof(reasons));
                MCLA_LOG_WARN(
                    "PC-DERIVE #{} attempted={} ok={} refused={} "
                    "refusedMaskSoFar=0x{:08X} reasons={} "
                    "|| byReason(stateNotDerived={} emptyHlsl={} noDevice={} "
                    "noRootSig={} emptyInputLayout={} layoutDeclMismatch={} "
                    "unsupportedPolyMode={} unsupportedTopology={} "
                    "srvOutsideT0T3={} samplerNotS0={} "
                    "unmeasuredDepthRequired={} queueSaturated={} "
                    "no2205={} no2204={} no2206={} viewportExtents={} "
                    "noPolyOffsetFront={} noPolyOffsetBack={} noDepthInfo={}) "
                    "|| noVsProgram={} noPsProgram={} noTranslatedPso={} "
                    "rectList(expanded={} refused={})",
                    cc, ds, dok, drf, maskSoFar, reasons,
                    s_deriveRefusedByBit[0].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[1].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[2].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[3].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[4].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[5].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[6].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[7].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[8].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[9].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[10].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[11].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[12].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[13].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[14].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[15].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[16].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[17].load(std::memory_order_relaxed),
                    s_deriveRefusedByBit[18].load(std::memory_order_relaxed),
                    s_deriveNoVsProgram.load(std::memory_order_relaxed),
                    s_deriveNoPsProgram.load(std::memory_order_relaxed),
                    s_deriveNoTranslatedPso.load(std::memory_order_relaxed),
                    s_rectListExpanded.load(std::memory_order_relaxed),
                    s_rectListRefused.load(std::memory_order_relaxed));

                // PrimTypeRefusalCount/Total are the backend's OWN per-prim_type
                // census (d3d12_backend.h:125-126), surfaced here so one line
                // answers "did the rectangle list get refused, and how often".
                MCLA_LOG_WARN(
                    "DRAW-CP-CENSUS #{} consumed={} drawn={} topologyAssigned={} "
                    "|| src-unsupported={} ib-empty={} ib-wrap={} numidx-zero={} "
                    "ib-swap-unhandled={} ib-short={} ib-unreadable={} "
                    "vb-missing={} vb-fmt-unmapped={} vb-fmt-stride={} "
                    "vb-fmt-short={} vb-short={} || prim-rect-list={} "
                    "prim-unmappable={} state-refused={} no-vs-program={} "
                    "no-ps-program={} no-translated-pso={} backendPrimRect={} "
                    "backendPrimTotal={} | vsProgAddr={} psProgAddr={} "
                    "vsProgAbsent={} psProgAbsent={} rsCov2204={} rsCov2205={} "
                    "rsCov2206={} rsAll3={} noColorInfo={} colorFmtUnmapped={}",
                    cc, n - 1u,
                    s_cpDrawn.load(std::memory_order_relaxed),
                    s_cpTopologyAssigned.load(std::memory_order_relaxed),
                    s_cpRefuseSrc.load(std::memory_order_relaxed),
                    s_cpRefuseIbEmpty.load(std::memory_order_relaxed),
                    s_cpRefuseIbWrap.load(std::memory_order_relaxed),
                    s_cpRefuseNumIdx.load(std::memory_order_relaxed),
                    s_cpRefuseSwap.load(std::memory_order_relaxed),
                    s_cpRefuseIbShort.load(std::memory_order_relaxed),
                    s_cpRefuseIbUnreadable.load(std::memory_order_relaxed),
                    s_cpRefuseVbMissing.load(std::memory_order_relaxed),
                    s_cpRefuseVbFmtUnmapped.load(std::memory_order_relaxed),
                    s_cpRefuseVbFmtStride.load(std::memory_order_relaxed),
                    s_cpRefuseVbFmtShort.load(std::memory_order_relaxed),
                    s_cpRefuseVbShortRefused.load(std::memory_order_relaxed),
                    s_cpRefusePrimRect.load(std::memory_order_relaxed),
                    s_cpRefusePrimOther.load(std::memory_order_relaxed),
                    s_cpRefuseStateRefused.load(std::memory_order_relaxed),
                    s_cpRefuseNoVsProgram.load(std::memory_order_relaxed),
                    s_cpRefuseNoPsProgram.load(std::memory_order_relaxed),
                    s_cpRefuseNoPso.load(std::memory_order_relaxed),
                    D3D12Backend::PrimTypeRefusalCount(0x08u),
                    D3D12Backend::PrimTypeRefusalTotal(),
                    // F-652: the program census, counted from what the draw
                    // ACTUALLY carried on the command - not re-derived from a
                    // register file that may have moved on - and the raster-state
                    // COVERAGE of what was consumed, per register and for the
                    // all-three case that gates `haveRegisters`.
                    s_cpProgVsAddr.load(std::memory_order_relaxed),
                    s_cpProgPsAddr.load(std::memory_order_relaxed),
                    s_cpProgVsAbsent.load(std::memory_order_relaxed),
                    s_cpProgPsAbsent.load(std::memory_order_relaxed),
                    s_cpRsCovPaClipCntl.load(std::memory_order_relaxed),
                    s_cpRsCovPaSuScModeCntl.load(std::memory_order_relaxed),
                    s_cpRsCovPaClVteCntl.load(std::memory_order_relaxed),
                    s_cpRsAll3.load(std::memory_order_relaxed),
                    // F-723: appended at the very END of the row. Two separate
                    // totals because "RB_COLOR_INFO was never presented" and
                    // "its color_format has no DXGI equivalent" are different
                    // faults with different fixes, and one lumped number would
                    // make either fix look like it worked for the other.
                    s_cpRefuseNoColorInfo.load(std::memory_order_relaxed),
                    s_cpRefuseColorFmtUnmapped.load(std::memory_order_relaxed));
            }

            // --- index gate, in the order the data is needed ---------------
            // F-633: source_select=2 (kAutoIndex) is a REAL, NON-INDEXED draw,
            // not a malformed one. Upstream sets index_buffer_info.guest_base=0
            // and .length=0 (command_processor.cc:1357-1361), leaves
            // is_indexed=false (:1308) and calls IssueDraw(prim_type,
            // num_indices, /*index_buffer=*/nullptr, ...) (:1380-1384): the
            // indices are implicit 0..num_indices-1. The old gate here refused
            // every such draw with src-not-dma, which is the same misreading one
            // hop downstream of the CP bridge and would have kept
            // DRAW-CP-REFUSE at 100% after that bridge started enqueueing.
            // So the index gate below is conditioned on `indexed`, and
            // num_indices becomes the VERTEX count for the non-indexed class -
            // which is exactly what IssueDraw receives upstream.
            const bool& indexed = meta.indexed;
            meta.indexed = (dc.sourceSelect == 0u);  // SourceSelect::kDMA
            if (!indexed && dc.sourceSelect != 2u) {
                refuse("src-unsupported", s_cpRefuseSrc,
                       "- source_select is kImmediate (1) or the invalid class "
                         "(3). Upstream issues no draw for either: it logs and "
                         "sets draw_succeeded=0 (command_processor.cc:1348-1356), "
                         "or takes the default arm (:1362-1366). NOT treated as "
                         "kAutoIndex, because upstream does not either.");
                break;
            }
            if (indexed && dc.ibSize == 0u) {
                refuse("ib-empty", s_cpRefuseIbEmpty,
                       "- kDMA draw with VGT_DMA_SIZE.num_words == 0: no index "
                         "elements. A kAutoIndex draw has ibSize == 0 by "
                         "construction and is NOT this case.");
                break;
            }
            if (indexed &&
                static_cast<uint64_t>(dc.ibBase) + static_cast<uint64_t>(dc.ibSize) >
                    0x100000000ull) {
                refuse("ib-wrap", s_cpRefuseIbWrap,
                       "- index range leaves the 4 GiB guest window; NOT read.");
                break;
            }
            // num_indices == 0 is a draw of nothing for BOTH classes: indexed, it
            // is an empty index stream; non-indexed, an empty vertex stream.
            if (dc.numIndices == 0u) {
                refuse("numidx-zero", s_cpRefuseNumIdx,
                       "- VGT_DRAW_INITIATOR.num_indices == 0: a draw of nothing.");
                break;
            }
            // MD-38b: PA_SU_SC_MODE_CNTL cull_front (bit 0) AND cull_back (bit 1) both set rasterizes
            // nothing; Xenia drops the draw (.research/xenia/src/xenia/gpu/draw_util.cc:59).
            {
                static const bool s_noCullBoth = [] { const char* e = std::getenv("MCLA_NO_CULLBOTH_SKIP"); return e && e[0] == '1'; }();
                if (!s_noCullBoth && (dc.rasterState.presentMask & (1u << CpSnapshotReg::kCpSnapPaSuScModeCntl)) &&
                    (dc.rasterState.regs[CpSnapshotReg::kCpSnapPaSuScModeCntl] & 3u) == 3u) {
                    static std::atomic<uint32_t> s_cullBoth{0};
                    const uint32_t cn = s_cullBoth.fetch_add(1, std::memory_order_relaxed) + 1u;
                    if (cn <= 8u || (cn % 20000u) == 0u)
                        MCLA_LOG_WARN("PC-CULL-BOTH-SKIP #{} draw={} prim={} numIdx={} suScMode={:08X}", cn, n,
                                      dc.primitiveTopology, dc.numIndices, dc.rasterState.regs[CpSnapshotReg::kCpSnapPaSuScModeCntl]);
                    s_1sSkipped[primSlot].fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            // F-633: everything from here to the transcode is about the INDEX
            // buffer, so it is kDMA-only. A non-indexed draw has no index bytes
            // to bound, swap-transcode or read, and fabricating an implicit
            // 0..n-1 index buffer here would be exactly the substitution
            // F-609 forbids - the backend already has a non-indexed path
            // (DrawInstanced, d3d12_backend.cpp:2701) and it is used below.
            static const bool s_noWwGlobal = [] {
                const char* e = std::getenv("MCLA_NO_WRITE_WATCH");
                return e && e[0] == '1';
            }();
            static const bool s_noPersistentGeo = [] {
                const char* e = std::getenv("MCLA_NO_PERSISTENT_GEO");
                return e && e[0] == '1';
            }();
            static const bool s_noMultistream = [] {
                const char* e = std::getenv("MCLA_NO_MULTISTREAM");
                return e && e[0] == '1';
            }();
            static const bool s_noClearFastpath = [] {
                const char* e = std::getenv("MCLA_NO_CLEAR_FASTPATH");
                return e && e[0] == '1';
            }();
            static const bool s_noMrt = [] {
                const char* e = std::getenv("MCLA_NO_MRT");
                return e && e[0] == '1';
            }();
            static const bool s_noYFlip = [] {
                const char* e = std::getenv("MCLA_NO_YFLIP");
                return e && e[0] == '1';
            }();
            const bool wwActive = !s_noWwGlobal && mcla::kernel::g_writeWatchActive.load(std::memory_order_acquire);

            static thread_local IndexTranscode tc;
            tc.count = 0;
            tc.maxIndex = 0;
            tc.isWide = false;
            if (indexed) {
                uint32_t ibStrideBytes = 0;
                bool wideIndex = false;
                if (!XenosIndexElement(dc.ibSwap, ibStrideBytes, wideIndex)) {
                    refuse("ib-swap-unhandled", s_cpRefuseSwap,
                           "- VGT_DMA_SIZE.swap_mode has no D3D12 index element; "
                             "refusing rather than guessing a byte order.");
                    break;
                }
                // The draw claims more indices than the buffer holds. Upstream
                // would read past the end; here it is a refusal.
                const uint64_t want = static_cast<uint64_t>(dc.numIndices) *
                                      static_cast<uint64_t>(ibStrideBytes);
                if (want > static_cast<uint64_t>(dc.ibSize)) {
                    refuse("ib-short", s_cpRefuseIbShort,
                           wideIndex
                               ? "- num_indices * 4B element stride exceeds the "
                                 "resolved index buffer; the draw would read past its "
                                 "own end."
                               : "- num_indices * element stride exceeds the resolved "
                                 "index buffer; the draw would read past its own end.");
                    break;
                }
                if (!guestMem.IsValid(dc.ibBase, dc.ibSize)) {
                    refuse("ib-unreadable", s_cpRefuseIbUnreadable,
                           "- resolved index buffer is outside the guest window.");
                    break;
                }

                const uint32_t ibNeed = static_cast<uint32_t>(std::min<uint64_t>(dc.ibSize, uint64_t(dc.numIndices) * ibStrideBytes));
                const bool ibCanPersist = wwActive && !s_noPersistentGeo &&
                                          dc.primitiveTopology != 6u && dc.primitiveTopology != 13u &&
                                          ibNeed >= 128u;
                const uint64_t ibKey = ibCanPersist
                    ? MakeR1GeoKey(dc.ibBase, ibNeed,
                                   (uint64_t(dc.ibSwap) << 32) | (uint64_t(dc.idxOffset) << 16) | dc.numIndices,
                                   0x49420001ull)
                    : 0ull;
                if (ibCanPersist) {
                    std::shared_lock<std::shared_mutex> rlk(s_r1GeoMutex);
                    auto it = s_r1GeoTracker.find(ibKey);
                    if (it != s_r1GeoTracker.end() && !it->second.transient &&
                        it->second.gpuVa != 0 && it->second.pageCount > 0 &&
                        it->second.sizeBytes == ibNeed &&
                        mcla::kernel::GuestPageRangeMatchesEpochs(
                            it->second.basePage, it->second.pageCount, it->second.epochs.data())) {
                        prep.ibCleanHit = true;
                        prep.ibPersistentKey = ibKey;
                        prep.ibContentHash = it->second.contentHash;
                        prep.desc.residentIbBytes = it->second.sizeBytes;
                        tc.isWide = wideIndex;
                        tc.count = dc.numIndices;
                        tc.maxIndex = it->second.maxIndex;
                    }
                }
                if (!prep.ibCleanHit) {
                    if (ibCanPersist) {
                        mcla::kernel::SnapshotGuestPageEpochs(
                            dc.ibBase, ibNeed, prep.ibEpochSnap, &prep.ibBasePage, &prep.ibPageCount);
                    }
                    static thread_local std::vector<uint8_t> ibRaw;
                    if (ibRaw.size() < ibNeed) ibRaw.resize(ibNeed);
                    if (!guestMem.ReadBytes(dc.ibBase, ibRaw.data(), ibNeed)) {
                        refuse("ib-read-failed", s_cpRefuseIbUnreadable,
                               "- ReadBytes on the validated index buffer failed.");
                        break;
                    }
                    if (!TranscodeXenosIndices(ibRaw.data(), dc.numIndices, dc.ibSwap, tc)) {
                        refuse("ib-transcode-failed", s_cpRefuseSwap,
                               "- index transcode refused the swap mode after the gate "
                                 "accepted it (unreachable; kept so a future swap mode "
                                 "cannot slip through silently).");
                        break;
                    }
                    if (ibCanPersist && prep.ibPageCount > 0) {
                        prep.ibCheckPersistent = true;
                        prep.ibPersistentKey = ibKey;
                    }
                }
                // maxIndex is the real vertex count this draw needs, measured
                // from guest bytes. It is what sizes the missing vertex fetch, so
                // it is reported on its own line and is deliberately NOT gated on
                // the vertex decision below.
                {
                    const uint32_t m =
                        s_cpIndexMeasured.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (m <= 8 || (m % 200) == 0)
                        MCLA_LOG_INFO("DRAW-CP-INDEX #{} va={:08X} elem={}B swap={} "
                                      "count={} maxIndex={} ({} bit) - vertex bytes "
                                      "needed >= {} * stride",
                                      m, dc.ibBase, ibStrideBytes, dc.ibSwap, tc.count,
                                      tc.maxIndex, tc.isWide ? 32 : 16, tc.maxIndex);
                }
            }

            // --- vertex gate: the part that is genuinely missing ------------
            if (dc.vbBase == 0u || dc.vbSize == 0u || dc.vbStride == 0u) {
                refuse("vb-missing", s_cpRefuseVbMissing,
                       indexed
                           ? "- index data is REAL and measured (see "
                             "DRAW-CP-INDEX), but no vertex buffer descriptor "
                             "exists: on the Xenos the VB addresses are computed "
                             "by executing the fetch-shader program, and "
                             "src/gpu_cp.cpp's CpExecImLoad reads the PM4_IM_LOAD "
                             "payloads and discards them, so there is no program "
                             "and therefore no address. NOT substituted with a "
                             "synthetic shape."
                           : "- a NON-INDEXED draw needs no index buffer, so this "
                             "is the whole geometry: the fetch/VB descriptor the CP "
                             "bridge decoded is absent or incomplete (vbBase/vbSize/"
                             "vbStride), so there is no vertex stream to draw. NOT "
                             "substituted with a synthetic shape.");
                break;
            }

            // --- both halves real -> issue the draw -------------------------
// --- both halves real -> issue the draw -------------------------
            // F-747: THIS COPY IS A RAW BYTE COPY AND THE VERTEX STREAM IS
            // BIG-ENDIAN, so every float the backend reads out of it is
            // byte-reversed. F-751 IS WHAT CLOSED THAT: the selector now crosses
            // the queue boundary (CpVertexElement::endianness, render_queue.h),
            // and the transform is applied to `vb` immediately below - in the
            // same place the raw copy is made, and before `desc.vertexBytes`
            // ever points at it.
            //
            // MEASURED. build/w708.log, draw #1: the guest's own fetch constant
            // for this stream carries endianness = 2, xenos::Endian::k8in32
            // (xenos.h:196, GpuSwap :1061-1064 = a full 32-bit reversal), and
            // that guest memory image is big-endian - `CP-FETCHVB #1 reg=4800
            // val=17C8A2BF` is the architected address 0x17C8A2BC OR two endian
            // bits, read big-endian-correct by src/gpu_cp.cpp:1211-1217; a
            // host-order read of the same four bytes would print BFA2C817.
            // MEASURED, TWICE OVER: all 8 CP-FETCHVB-DECODE rows and all 6
            // CP-FETCHVB-DESC rows in that log carry endian=2, so this is what
            // EVERY fetch constant in this boot says, not one draw's oddity.
            //
            // THE TRANSFORM IS THE REFERENCE'S, NOT AN INVENTED ONE. The
            // reference does not read the stream through a byte-order-aware
            // accessor: it takes a RAW host-order dword load
            // (.research/xenia/src/xenia/gpu/shader_interpreter.cc:1031-1032)
            // and then applies `xenos::GpuSwap(dword, fetch_constant.endian)` to
            // EVERY dword it reads (:1048-1049). draw_util.cc:839-849 is the
            // same shape through the float overload (xenos.h:1071-1079). The
            // four selectors are transcribed one for one from xenos.h:1052-1069
            // into mcla::gpu::CpGpuSwap32 (render_queue.h) and are NOT guessed:
            // the harness in %TEMP%\opencode\bytemark compiles the reference's
            // own definition next to the shipped one and finds them identical on
            // all four selectors over 262,144 values.
            //
            // (PROVENANCE: .research/xenia has no .git, is gitignored at
            // .gitignore:14, and has 0 files in HEAD - an untracked snapshot with
            // NO recorded provenance. Its xenos.h was read at SHA-256
            // EF00180E1B69A6233AD07161BDD42A9A623CB2EE88C732CE7FE69C611DFD6389.)
            //
            // WHAT IT BUYS, ON THE REAL BYTES. Vertex 0 lane 0 is four guest
            // bytes BF 00 00 00. Read host-order that is 0x000000BF = 2.68e-43, a
            // denormal no guest writes as a position; read as the guest wrote it
            // it is 0xBF000000 = -0.5. The probe in the backend prints that row
            // verbatim, so the next soak answers it directly.
            // F-899 (2026-10-27): guest texture descriptors, carried from the CP
            // (dc.textureDescriptorBytes, 24 big-endian bytes per descriptor, up
            // to kCpMaxTextureDescriptors). For each descriptor we decode it,
            // plan the import, read the tiled payload from guest memory, untile
            // it, convert endianness, and hand the linear bytes to the backend
            // via GuestTextureEntry::mipPayloads[0]. This mirrors the vertex
            // buffer path (ReadBytes + swap) plus the untile step that the
            // texture_decode module already implements.
            //
            // A failure at any step leaves mipPayloads[0] null, which makes
            // BindGuestTextures refuse honestly ("no untiled payload from the
            // producer") and write a null SRV -- the same state the heap
            // already held, but now the capture census (CP-TEXDESC-CAPTURE) and
            // the bind-refusal census both move, so the two halves of the gap
            // are independently observable.
            std::vector<uint8_t>& vb = prep.vb;
            std::vector<mcla::native::D3D12Backend::GuestTextureEntry>&
                guestTextures = prep.guestTextures;
            // Per-draw payload buffers, one per texture slot. BindGuestTextures
            // copies the bytes into the upload heap during the draw call, so a
            // buffer scoped to prep is sufficient (same lifetime as vb).
            std::vector<std::vector<uint8_t>>& texPayloads = prep.texOwnedPayloads;
            static thread_local std::vector<uint32_t> texW;
            static thread_local std::vector<uint32_t> texH;
            guestTextures.clear();
            texPayloads.clear();
            // G0: per-fetch-constant sampler table (s<N> = fetch constant N); see backend BindGuestSamplerTable.
            mcla::native::HostSamplerDesc guestSamplerTab[mcla::renderer::kShaderSamplerCount]{};  // MD-10: 32 (VS fetch constants 16..31)
            uint32_t guestSamplerMask = 0;
            static const bool s_guestSamplers = []() {
                const char* e = std::getenv("MCLA_TEMP_GUEST_SAMPLER");
                return !(e && e[0] == '0');
            }();
            const uint32_t texCount = std::min<uint32_t>(
                dc.textureCount, static_cast<uint32_t>(mcla::native::kCpMaxTextureDescriptors));
            texW.clear();
            texH.clear();
            guestTextures.reserve(texCount);
            texPayloads.reserve(texCount);
            texW.reserve(texCount);
            texH.reserve(texCount);


            // guestMem is already declared at the top of the DRAW_CAPTURED handler
            // (line ~2199); reuse it rather than re-declaring.

            // TEXSET-MEMO: when every texture of a draw took the per-texture thread-local fast path
            // (useCached), the whole result is a pure function of (fetch slots, 24-byte descriptors,
            // pool/mip VAs) under the same freshness witness that path uses (write-watch burst seq, or
            // presented frame). Replay it for an identical set; payloads are re-locked from weak_ptrs
            // exactly like the per-texture path, so an evicted/replaced payload is a miss.
            // MCLA_NO_TEXSET_MEMO=1 opts out.
            struct TexSetMemo {
                uint32_t n = 0;
                uint8_t key[kTexSetKeyMax];
                uint32_t keyLen = 0;
                bool ww = false;
                uint64_t seq = 0;
                int64_t pres = -1;
                std::vector<mcla::native::D3D12Backend::GuestTextureEntry> entries;
                std::vector<uint32_t> w, h;
                std::vector<std::weak_ptr<const std::vector<uint8_t>>> lins;
                mcla::native::HostSamplerDesc smp[mcla::renderer::kShaderSamplerCount]{};
                uint32_t smpMask = 0;
            };
            static thread_local TexSetMemo s_texSetMemo[16];
            static const bool s_texSetMemoOn = [] {
                auto on = [](const char* n) { const char* e = std::getenv(n); return !(e && e[0] == '1'); };
                return on("MCLA_NO_TEXSET_MEMO") && on("MCLA_TEMP_NO_UNTILE_WINDOW") && on("MCLA_NO_TEXPLAN_MEMO");
            }();
            static const bool s_tsNoWw = [] { const char* e = std::getenv("MCLA_NO_WRITE_WATCH"); return e && e[0] == '1'; }();
            uint8_t tsKey[kTexSetKeyMax];
            uint32_t tsKeyLen = 0;
            TexSetMemo* tsSlot = nullptr;
            const bool tsWw = !s_tsNoWw && mcla::kernel::g_writeWatchActive.load(std::memory_order_acquire);
            const uint64_t tsSeq0 = g_wwBurstSeq.load(std::memory_order_acquire);
            const int64_t tsPres0 = static_cast<int64_t>(backend_.PresentedFrames());
            uint32_t texSetCached = 0;
            std::vector<std::weak_ptr<const std::vector<uint8_t>>>* tsLins = nullptr;
            static thread_local std::vector<std::weak_ptr<const std::vector<uint8_t>>> s_tsLinsScratch;
            bool texSetHit = false;
            if (s_texSetMemoOn && texCount > 0u) {
                for (uint32_t ti = 0; ti < texCount; ++ti) {
                    uint8_t* k = tsKey + ti * 36u;
                    std::memcpy(k, dc.textureDescriptorBytes[ti], 24);
                    k[24] = dc.textureFetchSlot[ti]; k[25] = k[26] = k[27] = 0;
                    std::memcpy(k + 28, &dc.texturePoolVa[ti], 4);
                    std::memcpy(k + 32, &dc.texturePoolVaMip[ti], 4);
                }
                tsKeyLen = texCount * 36u;
                uint64_t kh = 0xcbf29ce484222325ull ^ texCount;
                for (uint32_t i = 0; i + 8u <= tsKeyLen; i += 8u) {
                    uint64_t w; std::memcpy(&w, tsKey + i, 8);
                    kh = (kh ^ w) * 0x100000001b3ull; kh ^= kh >> 29;
                }
                if (tsKeyLen & 7u) { uint32_t w; std::memcpy(&w, tsKey + tsKeyLen - 4u, 4); kh = (kh ^ w) * 0x100000001b3ull; kh ^= kh >> 29; }
                tsSlot = &s_texSetMemo[kh & 15u];
                if (tsSlot->n == texCount && tsSlot->keyLen == tsKeyLen && tsSlot->ww == tsWw &&
                    (tsWw ? tsSlot->seq == tsSeq0 : tsSlot->pres == tsPres0) &&
                    std::memcmp(tsSlot->key, tsKey, tsKeyLen) == 0) {
                    const size_t hold0 = prep.texHold.size();
                    texSetHit = true;
                    for (const auto& wl : tsSlot->lins) {
                        auto sl = wl.lock();
                        if (!sl) { texSetHit = false; break; }
                        prep.texHold.push_back(std::move(sl));
                    }
                    if (texSetHit) {
                        guestTextures.assign(tsSlot->entries.begin(), tsSlot->entries.end());
                        texW.assign(tsSlot->w.begin(), tsSlot->w.end());
                        texH.assign(tsSlot->h.begin(), tsSlot->h.end());
                        texPayloads.resize(texCount);
                        std::memcpy(guestSamplerTab, tsSlot->smp, sizeof(guestSamplerTab));
                        guestSamplerMask = tsSlot->smpMask;
                    } else {
                        prep.texHold.resize(hold0);
                    }
                }
                if (!texSetHit) { s_tsLinsScratch.clear(); tsLins = &s_tsLinsScratch; }
            }

            for (uint32_t ti = 0; ti < (texSetHit ? 0u : texCount); ++ti) {
                STALL_STAGE("tex-loop");
                mcla::native::D3D12Backend::GuestTextureEntry e{};
                e.descriptorIndex = dc.textureFetchSlot[ti];  // bind refuses >=16
                for (uint32_t b = 0; b < 24u; ++b) {
                    e.descriptorBytes[b] = dc.textureDescriptorBytes[ti][b];
                }
                e.mipPayloads[0] = nullptr;
                e.mipBytes[0] = 0;
                e.mipCount = 0;  // plan.mipCount

                // TEXDESC-MEMO: content-keyed decode memo (perf: tex-loop is the
                // #2 controllable CPU stage). DecodeGuestTextureDescriptor
                // (texture_decode.cpp:1350-1488) reads only be24[0..23] plus the
                // pure GetTextureFormatInfo lookup table and writes only `out` --
                // it is a PURE function of the 24 bytes, so a byte-identical hit
                // is exactly the decode we would otherwise redo. Key = the 24
                // bytes + descriptorIndex + generation (g_wwBurstSeq, the
                // guest-memory-change epoch, bumped ~once/present at :399); the
                // 24 bytes are re-compared with memcmp as a hard witness, so a
                // stale/rewritten descriptor can never be used. Default ON;
                // MCLA_NO_TEXDESC_MEMO=1 opts out (A/B, cf. MCLA_NO_ENV_MEMO).
                struct TexDescMemoKey {
                    uint8_t b[24];
                    uint32_t idx;
                    uint64_t gen;
                };
                struct TexDescMemoEnt {
                    TexDescMemoKey key{};
                    bool occupied = false;
                    bool ok = false;
                    mcla::native::GuestTextureDesc desc{};
                };
                static thread_local TexDescMemoEnt s_texdescMemo[16];
                // Measured 2026-11-20 (abtd_on vs abtd_off): ~34% hit rate and
                // tex-loop got WORSE (2652 vs 2270 us/1k). Default OFF; opt in
                // only for a future experiment.
                static const bool s_noTexdescMemo = [] {
                    const char* ev = std::getenv("MCLA_TEXDESC_MEMO");
                    return !(ev && ev[0] == '1');
                }();
                static StatCtr<uint32_t> s_tdHit{0}, s_tdMiss{0};
                const uint64_t tdGen = g_wwBurstSeq.load(std::memory_order_acquire);
                TexDescMemoEnt* tdSlot = nullptr;
                if (!s_noTexdescMemo) {
                    uint64_t kh = 0xcbf29ce484222325ull;
                    for (uint32_t i = 0; i < 24u; ++i)
                        kh = (kh ^ e.descriptorBytes[i]) * 0x100000001b3ull;
                    kh = (kh ^ e.descriptorIndex) * 0x100000001b3ull;
                    kh = (kh ^ tdGen) * 0x100000001b3ull;
                    tdSlot = &s_texdescMemo[kh & 15u];
                }

                // Decode the 24-byte descriptor (or reuse the memoized decode).
                mcla::native::GuestTextureDesc gtd{};
                bool decodeOk = false;
                bool tdHit = false;
                if (tdSlot && tdSlot->occupied &&
                    tdSlot->key.idx == e.descriptorIndex &&
                    tdSlot->key.gen == tdGen &&
                    std::memcmp(tdSlot->key.b, e.descriptorBytes, 24) == 0) {
                    gtd = tdSlot->desc;          // memcmp witness: identical bytes => identical decode
                    decodeOk = tdSlot->ok;
                    tdHit = true;
                }
                if (!tdHit) {
                    decodeOk = mcla::native::DecodeGuestTextureDescriptor(e.descriptorBytes, gtd) &&
                               gtd.valid;
                    if (tdSlot) {
                        std::memcpy(tdSlot->key.b, e.descriptorBytes, 24);
                        tdSlot->key.idx = e.descriptorIndex;
                        tdSlot->key.gen = tdGen;
                        tdSlot->desc = gtd;
                        tdSlot->ok = decodeOk;
                        tdSlot->occupied = true;
                    }
                }
                if (!s_noTexdescMemo) {
                    const uint32_t cn = tdHit
                        ? s_tdHit.fetch_add(1, std::memory_order_relaxed) + 1u
                        : s_tdMiss.fetch_add(1, std::memory_order_relaxed) + 1u;
                    if (cn <= 8u || (cn % 1000u) == 0u) {
                        MCLA_LOG_ERROR("TEXDESC-MEMO {} n={} hit={} miss={} ti={} idx={}",
                                       tdHit ? "HIT" : "MISS", cn,
                                       s_tdHit.load(std::memory_order_relaxed),
                                       s_tdMiss.load(std::memory_order_relaxed),
                                       ti, e.descriptorIndex);
                    }
                }
                if (!decodeOk) {
                    guestTextures.push_back(e);
                    texPayloads.push_back({});
                    texW.push_back(0);
                    texH.push_back(0);
                    continue;
                }

                if (s_guestSamplers && e.descriptorIndex < mcla::renderer::kShaderSamplerCount) {
                    const char* samplerRefusal = nullptr;
                    mcla::native::HostSamplerDesc hs = mcla::native::DecodeGuestSamplerState(gtd, &samplerRefusal);
                    if (!samplerRefusal) {
                        guestSamplerTab[e.descriptorIndex] = hs;
                        guestSamplerMask |= 1u << e.descriptorIndex;
                    }
                }

                // Bind by the shader's register dimension: t0=1D, t1=2D, t2=3D, t3=cube.
                // Bind at the guest fetch constant's own slot: the translated PS
                // declares tex<N> at register(tN), N = the tfetch constIndex.
                // (e.descriptorIndex already holds dc.textureFetchSlot[ti] from loop start.)
                // Stage 2 (ResolveGuestTexturesToStaging on RenderThread) checks HasFrameTexture and m_resolved.

                // TEMP DIAGNOSTIC (F-900): the shader picks its t-register from
                // the FETCH instruction's dimension (t1D=t0, t2D=t1, t3D=t2,
                // tCube=t3 -- shader_translator.cpp:1296-1299,1309-1311), while
                // this loop binds at heap slot kTexture2DSlot+descriptorIndex
                // (= 1 + descriptorIndex). Measured: the dumped PS samples tCube
                // (t3) yet descriptorIndex=ti=0 lands at slot 1 (t1) -> the
                // shader reads a NULL SRV -> black. Log the descriptor's own
                // dimension so the correct descriptorIndex can be derived.
                //
                // F-914: unthrottled this row printed once per texture slot per
                // draw -- 170,136 rows in ga062 (~9% of the log, the #2 marker).
                // House cadence (d3d12_backend.cpp:263-269: first 8, then every
                // 200th) keeps the marker firing (a zero would be vacuous, rule
                // 19) without the flood. The `#{}` argument is now the row
                // counter; the old format passed `ti`, i.e. `#0` on all 170,136
                // rows. `drawSeq` still carries `n`, exactly as before.
                static StatCtr<uint32_t> s_texSlotDecide{0};
                const uint32_t tsdN =
                    s_texSlotDecide.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (tsdN <= 8u || (tsdN % 20000u) == 0u) {  // MD-32: was 200
                    MCLA_LOG_INFO("TEX-SLOT-DECIDE #{} drawSeq={} ti={} dim={} fmt={} {}x{} "
                                  "curIdx={} curSlot={} poolVa={:08X} needForCube=2",
                                  tsdN, n, ti, static_cast<uint32_t>(gtd.dimension),
                                  gtd.xenosFormat, gtd.width, gtd.height,
                                  e.descriptorIndex, 1u + e.descriptorIndex,
                                  dc.texturePoolVa[ti]);
                }

                // Block-compressed (DXT) textures on Xenos store their 16-bit
                // endpoints and lookup tables in guest endianness (typically
                // k8in16 = 1), which Xenia's load shaders swap via
                // XeEndianSwap32(..., load_info.endian_32) before unpacking
                // (.research/xenia/src/xenia/gpu/shaders/texture_load_dxt3_rgba8.cs.xesl:37,
                // texture_load_dxt5_rgba8.cs.xesl:37). Do NOT zero gtd.endianness
                // here: ConvertTextureEndianness below performs the k8in16 byte-pair
                // swap on the untiled linear buffer before DecodeBc1/2/3Block reads it.
                const bool is3D = (gtd.dimension == mcla::native::GuestTextureDimension::ThreeD);
                const bool isDS = (gtd.dimension == mcla::native::GuestTextureDimension::TwoDOrStacked &&
                                   gtd.stacked && gtd.depth > 1);
                static const bool s_noTexPlanMemo = [] {
                    const char* e = std::getenv("MCLA_NO_TEXPLAN_MEMO");
                    return e && e[0] == '1';
                }();
                const mcla::native::TextureImportPlan plan = [&] {
                    if (s_noTexPlanMemo) {
                        return mcla::native::PlanTextureImport(
                            gtd.xenosFormat, gtd.width, gtd.height, gtd.depthOrArraySize,
                            gtd.mipMaxLevel, gtd.endianness, gtd.tiled, gtd.packedMips,
                            is3D, isDS);
                    }
                    struct PlanMemoKey {
                        uint32_t xenosFormat = 0;
                        uint32_t w = 0, h = 0, depth = 0, mipMaxLevel = 0;
                        uint8_t endian = 0, tiled = 0, packedMips = 0, is3D = 0, isDS = 0;
                        bool valid = false;
                        bool operator==(const PlanMemoKey& o) const {
                            return valid && o.valid &&
                                   xenosFormat == o.xenosFormat && w == o.w && h == o.h &&
                                   depth == o.depth && mipMaxLevel == o.mipMaxLevel &&
                                   endian == o.endian && tiled == o.tiled &&
                                   packedMips == o.packedMips && is3D == o.is3D && isDS == o.isDS;
                        }
                    };
                    struct PlanMemoBucket {
                        PlanMemoKey keys[2];
                        mcla::native::TextureImportPlan plans[2];
                        uint8_t rr = 0;
                    };
                    static thread_local PlanMemoBucket s_planMemo[256];
                    const PlanMemoKey pk{
                        gtd.xenosFormat,
                        gtd.width, gtd.height, gtd.depthOrArraySize, gtd.mipMaxLevel,
                        static_cast<uint8_t>(gtd.endianness),
                        static_cast<uint8_t>(gtd.tiled ? 1 : 0),
                        static_cast<uint8_t>(gtd.packedMips ? 1 : 0),
                        static_cast<uint8_t>(is3D ? 1 : 0),
                        static_cast<uint8_t>(isDS ? 1 : 0),
                        true};
                    uint64_t ph = static_cast<uint64_t>(pk.xenosFormat) * 0x9E3779B97F4A7C15ULL;
                    ph ^= static_cast<uint64_t>(pk.w) * 0xBF58476D1CE4E5B9ULL;
                    ph ^= static_cast<uint64_t>(pk.h) * 0x94D049BB133111EBULL;
                    ph ^= static_cast<uint64_t>(pk.depth) * 0xD6E8FEB86659FD93ULL;
                    ph ^= (static_cast<uint64_t>(pk.mipMaxLevel) << 16) ^
                          (static_cast<uint64_t>(pk.endian) << 8) ^
                          (static_cast<uint64_t>(pk.tiled) << 3) ^
                          (static_cast<uint64_t>(pk.packedMips) << 2) ^
                          (static_cast<uint64_t>(pk.is3D) << 1) ^
                          static_cast<uint64_t>(pk.isDS);
                    ph ^= ph >> 33;
                    PlanMemoBucket& b = s_planMemo[ph & 255u];
                    if (b.keys[0] == pk) return b.plans[0];
                    if (b.keys[1] == pk) return b.plans[1];
                    mcla::native::TextureImportPlan computed = mcla::native::PlanTextureImport(
                        gtd.xenosFormat, gtd.width, gtd.height, gtd.depthOrArraySize,
                        gtd.mipMaxLevel, gtd.endianness, gtd.tiled, gtd.packedMips,
                        is3D, isDS);
                    const uint8_t way = b.rr & 1u;
                    b.rr ^= 1u;
                    b.keys[way] = pk;
                    b.plans[way] = computed;
                    return computed;
                }();
                if (!plan.ok) {
                    guestTextures.push_back(e);
                    texPayloads.push_back({});
                    texW.push_back(0);
                    texH.push_back(0);
                    continue;
                }

                // Compute the tiled surface size (bytes of tiled guest memory).
                //
                // 32-BLOCK STORAGE ALIGNMENT (measured): Xenia aligns BOTH the
                // block pitch and the row count up to 32 blocks before computing
                // storage (texture_util.cc:324-327 row pitch, :339-342 row
                // count), and GetTiledOffset2D internally asserts pitch&31==0
                // (texture_address.h:196). A 320x256 DXT (80 blocks wide) needs
                // 96*64*8 = 49,152 B of storage but the unaligned math below
                // would pass 80*64*8 = 40,960 B, silently skipping 512 of 5,120
                // blocks (white band on the right). 256x256 is already aligned
                // (64 blocks), so this did not fire on the measured texture --
                // it is a correctness fix for every other width.
                const uint32_t bpb = plan.blockBytes ? plan.blockBytes
                                                     : mcla::native::GetTextureBytesPerBlock(gtd.xenosFormat);
                const uint32_t blocksX = (gtd.width + plan.blockWidth - 1) / plan.blockWidth;
                const uint32_t blocksY = (gtd.height + plan.blockHeight - 1) / plan.blockHeight;
                uint32_t inputPitch = (gtd.pitchTexels > 0)
                    ? ((gtd.pitchTexels + plan.blockWidth - 1) / plan.blockWidth)
                    : blocksX;
                if (inputPitch < blocksX) inputPitch = blocksX;
                uint32_t inputHeight = (gtd.height > 0)
                    ? ((gtd.height + plan.blockHeight - 1) / plan.blockHeight)
                    : blocksY;
                // Xenia's storage extent: both dimensions 32-block aligned.
                auto align32 = [](uint32_t v) { return (v + 31u) & ~31u; };
                const uint32_t storedPitch = align32(inputPitch);
                const uint32_t storedRows = align32(inputHeight);
                inputPitch = storedPitch;      // the untile guard sees the real grid
                inputHeight = storedRows;
                uint32_t tiledSize = storedPitch * storedRows * bpb;
                if (tiledSize == 0) tiledSize = plan.linearByteSize;
                // Cap against garbage descriptors, but never below the 32-block-aligned grid of the real size:
                // a 32x32 DXT5 (8x8 blocks) is stored as 32x32 blocks = 16 KiB, and a 4 KiB cap dropped its lower
                // half (half loading disc, texture 13AE8000; docs/AUDIT_LOADING_DISC.md section 7).
                const uint32_t alignedReal = storedPitch * align32(blocksY) * bpb;  // guest pitch (linear rows can be wider than the image)
                const uint32_t tiledCap = std::max(plan.linearByteSize * 4, alignedReal);
                if (tiledSize > tiledCap) tiledSize = tiledCap;
                // F-900 STALL-GUARD (2026-10-28): garbage fetch descriptors
                // name GB surfaces (measured: consumer stops ~60k draws with a
                // full queue and no crash/log -- consistent with UntileTexture2D
                // looping billions of blocks or multi-GB vector zero-fills).
                // The flat guest window validates almost any range, so IsValid
                // below cannot catch it. Refuse sizes the host cannot upload:
                // backend refuses payloads > 64 MiB anyway, so anything bigger
                // can never bind. Counted; first 4 + every 200th print dims.
                {
                    static StatCtr<uint32_t> s_tsizeRef{0};
                    const uint64_t lin = plan.linearByteSize;
                    // Extents first: plan math is uint32 and can WRAP garbage
                    // dims to a small linearByteSize (which would pass the byte
                    // cap below while info.width/height stay huge and the untile
                    // loops spin for billions of blocks). Xenos max is 8192;
                    // 16384 leaves headroom without letting garbage through.
                    const bool dimsWild =
                        gtd.width > 16384u || gtd.height > 16384u ||
                        gtd.pitchTexels > 16384u;
                    if (dimsWild || lin > 67108864ull || tiledSize > 268435456u) {
                        const uint32_t n = s_tsizeRef.fetch_add(1) + 1;
                        if (n <= 4 || (n % 200) == 0) {
                            MCLA_LOG_ERROR("RENDER-TEX-SIZE-REFUSE #{} ti={} "
                                           "fmt={} {}x{} pitch={} tiled={} "
                                           "linear={} -- NULL payload (bind refuses)",
                                           n, ti, gtd.xenosFormat, gtd.width,
                                           gtd.height, gtd.pitchTexels, tiledSize,
                                           lin);
                        }
                        guestTextures.push_back(e);
                        texPayloads.push_back({});
                        texW.push_back(0);
                        texH.push_back(0);
                        continue;
                    }
                }

                // The pool VA was translated at enqueue time (CpBridgeDraw) and
                // carried in dc.texturePoolVa[ti]. Use it directly -- the consumer
                // does not re-translate (the helpers are not declared in headers).
                // Decode already tracks basePage != 0 as gtd.hasBase: phys=0 in gpu_cp.cpp
                // maps to RAM page 0, which must never be read as mip 0 when !hasBase.
                uint32_t poolVa = gtd.hasBase ? dc.texturePoolVa[ti] : 0u;
                const uint32_t origPoolVa = dc.texturePoolVa[ti];
                static const bool s_mipMinFix = [] {
                    const char* e = std::getenv("MCLA_NO_MIP_MINLEVEL_FIX");
                    return !(e && e[0] == '1');
                }();
                uint32_t mipVa = (gtd.mipAddress != 0u) ? dc.texturePoolVaMip[ti] : 0u;
                if (s_mipMinFix && gtd.mipMinLevel > 0u && mipVa == 0u && origPoolVa != 0u) {
                    mipVa = origPoolVa;
                }
                if (gtd.mipMinLevel > 0u) {
                    poolVa = 0u;  // Xenia texture_util.cc:95-97: mips live at mip_address
                }

                // Read guest memory.
                // PERF: untile cache. Measured 9.1 s of 12.2 s per 1000 draws in this
                // loop (TEMP-PERF-STAGES tex-loop). Key = descriptor+poolVa+size; an
                // entry checked within the last 16 ms is trusted (one frame of
                // staleness, shortcut: upgrade to a guest write-watch), older ones are
                // re-validated by hashing the tiled bytes so a guest rewrite misses.
                struct UntileEnt {
                    uint64_t hash = 0;
                    int64_t checkedNs = 0;
                    bool wwValid = false;
                    uint32_t basePage = 0;
                    uint32_t pageCount = 0;
                    std::vector<uint32_t> pageEpochs;
                    uint32_t mipBasePage = 0;
                    uint32_t mipPageCount = 0;
                    std::vector<uint32_t> mipEpochs;
                    std::shared_ptr<std::vector<uint8_t>> lin;
                };
                struct UntileKey { uint8_t b[32]; bool operator==(const UntileKey& o) const { return std::memcmp(b, o.b, 32) == 0; } };
                struct UntileKeyHash { size_t operator()(const UntileKey& k) const { uint64_t w[4]; std::memcpy(w, k.b, 32); uint64_t h = 0xcbf29ce484222325ull; for (uint64_t x : w) { h = (h ^ x) * 0x100000001b3ull; h ^= h >> 29; } return static_cast<size_t>(h); } };
                static std::unordered_map<UntileKey, UntileEnt, UntileKeyHash> s_untileCache;  // was a std::string key: 2 allocs + 2 string hashes per texture
                static std::shared_mutex s_untileMutex;
                static size_t s_untileCacheBytes = 0;
                auto& s_untileHold = prep.texHold;  // keeps this draw's payloads alive across an eviction (cleared in ResetForReuse)
                static StatCtr<uint32_t> s_uHit{0}, s_uMiss{0};
                UntileKey ukey;
                std::memcpy(ukey.b, e.descriptorBytes, 24);
                std::memcpy(ukey.b + 24, &poolVa, 4);
                std::memcpy(ukey.b + 28, &tiledSize, 4);
                const int64_t presFrame = static_cast<int64_t>(backend_.PresentedFrames());
                static const bool s_noWin = [] { const char* e = std::getenv("MCLA_TEMP_NO_UNTILE_WINDOW"); return e && e[0] == '1'; }();
                static const bool s_noWwTex = [] { const char* e = std::getenv("MCLA_NO_WRITE_WATCH"); return e && e[0] == '1'; }();
                const bool wwTexActive = !s_noWwTex && mcla::kernel::g_writeWatchActive.load(std::memory_order_acquire);
                const uint64_t curBurstSeq = g_wwBurstSeq.load(std::memory_order_acquire);
                struct ThreadUntileSlot {
                    int64_t presFrame = -1;
                    uint64_t wwBurstSeq = 0;
                    bool wwActive = false;
                    uint32_t mipVa = 0;
                    uint8_t ukeyBytes[32] = {};
                    uint64_t hash = 0;
                    size_t bytesSnap = 0;
                    uint32_t chainLevels = 1;
                    mcla::native::TextureMipChain chain{};
                    std::weak_ptr<const std::vector<uint8_t>> weakLin;
                };
                static thread_local ThreadUntileSlot s_tlUntileMemo[128];
                const size_t uhash = UntileKeyHash{}(ukey);
                ThreadUntileSlot& tlSlot = s_tlUntileMemo[uhash & 127u];

                mcla::native::TextureMipChain chain;
                uint32_t chainLevels = 1;
                const auto setChainLevels = [&](auto& g, const uint8_t* base, size_t bytes, uint32_t levels) {
                    uint32_t n = 1;
                    while (n < levels && uint64_t(chain.levels[n].linearOffset) + chain.levels[n].linearBytes <= bytes) ++n;
                    g.mipPayloads[0] = base;
                    g.mipBytes[0] = plan.linearByteSize;
                    for (uint32_t L = 1; L < n; ++L) {
                        g.mipPayloads[L] = base + chain.levels[L].linearOffset;
                        g.mipBytes[L] = chain.levels[L].linearBytes;
                    }
                    g.mipCount = n;
                };
                auto useCached = [&](const std::shared_ptr<const std::vector<uint8_t>>& lin,
                                     uint64_t cachedHash, size_t bytesSnap) {
                    const uint32_t h = s_uHit.fetch_add(1, std::memory_order_relaxed) + 1u;
                    if ((h % 5000u) == 0) MCLA_LOG_WARN("TEMP-PERF-UNTILE-CACHE hit={} miss={} bytes={}", h, s_uMiss.load(std::memory_order_relaxed), bytesSnap);
                    s_untileHold.push_back(lin);
                    ++texSetCached;
                    if (tsLins) tsLins->push_back(lin);
                    texPayloads.push_back({});
                    texW.push_back(gtd.width);
                    texH.push_back(gtd.height);
                    e.contentId = (cachedHash * 31u + uhash) | 1u;
                    setChainLevels(e, lin->data(), lin->size(), chainLevels);
                    guestTextures.push_back(e);
                };
                const bool tlFresh = wwTexActive
                    ? (tlSlot.wwActive && tlSlot.wwBurstSeq == curBurstSeq)
                    : (tlSlot.presFrame == presFrame);
                if (!s_noWin && !s_noTexPlanMemo && tlFresh &&
                    tlSlot.mipVa == mipVa &&
                    std::memcmp(tlSlot.ukeyBytes, ukey.b, 32) == 0) {
                    if (auto hitLin = tlSlot.weakLin.lock()) {
                        chain = tlSlot.chain;
                        chainLevels = tlSlot.chainLevels;
                        useCached(hitLin, tlSlot.hash, tlSlot.bytesSnap);
                        continue;
                    }
                }
                // P2a: mip chain decision (both direct and CpuExpand formats now expand
                // mips 1..N-1 in d3d12_backend.cpp unless MCLA_NO_CPUEXPAND_MIPS=1).
                // Packed mips and mipMinLevel > 0 build from mip_address via BuildGuestMipChain.
                static thread_local std::vector<uint8_t> mipTiled;
                mipTiled.clear();
                uint64_t mipTiledBytes = 0;
                if (plan.mipCount > 1 &&
                    mipVa != 0 && mipVa != poolVa && mcla::native::BuildGuestMipChain(gtd, chain) &&
                    chain.levelCount == plan.mipCount && chain.levels[0].linearBytes == plan.linearByteSize) {
                    mipTiledBytes = chain.totalBytes;
                    if (mipTiledBytes > 0 && mipTiledBytes <= 67108864ull &&
                        guestMem.IsValid(mipVa, static_cast<uint32_t>(mipTiledBytes)))
                        chainLevels = chain.levelCount;
                }
                if (poolVa == 0u && chainLevels <= 1u) {
                    if (s_mipMinFix && origPoolVa != 0u && guestMem.IsValid(origPoolVa, plan.linearByteSize)) {
                        poolVa = origPoolVa;
                        static StatCtr<uint32_t> s_mipFb{0};
                        const uint32_t fbN = s_mipFb.fetch_add(1, std::memory_order_relaxed) + 1u;
                        if (fbN <= 8u || (fbN % 200u) == 0u) {
                            MCLA_LOG_INFO("MIP-MINLEVEL-FIX #{} fallback poolVa restored from origPoolVa=0x{:08X}", fbN, origPoolVa);
                        }
                    } else {
                        guestTextures.push_back(e);
                        texPayloads.push_back({});
                        texW.push_back(0);
                        texH.push_back(0);
                        continue;
                    }
                }
                const uint32_t chainLinBytes = chainLevels > 1 ? chain.linearTotal : plan.linearByteSize;
                auto recordTlSlot = [&](const std::shared_ptr<const std::vector<uint8_t>>& lin,
                                        uint64_t hVal, size_t bSnap) {
                    if (s_noWin || s_noTexPlanMemo || !lin) return;
                    tlSlot.presFrame = presFrame;
                    tlSlot.wwBurstSeq = curBurstSeq;
                    tlSlot.wwActive = wwTexActive;
                    tlSlot.mipVa = mipVa;
                    std::memcpy(tlSlot.ukeyBytes, ukey.b, 32);
                    tlSlot.hash = hVal;
                    tlSlot.bytesSnap = bSnap;
                    tlSlot.chainLevels = chainLevels;
                    tlSlot.chain = chain;
                    tlSlot.weakLin = lin;
                };
                STALL_STAGE("tex-cache");
                {
                    std::shared_ptr<const std::vector<uint8_t>> hitLin;
                    uint64_t hitHash = 0;
                    size_t bytesSnap = 0;
                    {
                        std::shared_lock<std::shared_mutex> rlk(s_untileMutex);
                        auto fit = s_untileCache.find(ukey);
                        if (!s_noWin && fit != s_untileCache.end() && fit->second.lin) {
                            bool epochsClean = false;
                            if (wwTexActive && fit->second.wwValid) {
                                const bool baseClean = (poolVa == 0u) ||
                                    (fit->second.pageCount > 0 &&
                                     mcla::kernel::GuestPageRangeMatchesEpochs(
                                         fit->second.basePage, fit->second.pageCount, fit->second.pageEpochs.data()));
                                const bool mipClean = (chainLevels <= 1u) ||
                                    (fit->second.mipPageCount > 0 &&
                                     mcla::kernel::GuestPageRangeMatchesEpochs(
                                         fit->second.mipBasePage, fit->second.mipPageCount,
                                         fit->second.mipEpochs.data()));
                                epochsClean = baseClean && mipClean;
                            } else {
                                epochsClean = (fit->second.checkedNs == presFrame);
                            }
                            if (epochsClean) {
                                hitLin = fit->second.lin;
                                hitHash = fit->second.hash;
                                bytesSnap = s_untileCacheBytes;
                            }
                        }
                    }
                    if (hitLin) {
                        recordTlSlot(hitLin, hitHash, bytesSnap);
                        useCached(hitLin, hitHash, bytesSnap);
                        continue;
                    }
                }
                // R1 Harvest-Before-Read: snapshot page epochs strictly BEFORE hashing or reading guest bytes.
                STALL_STAGE("tex-hash");
                std::vector<uint32_t> snapPageEpochs, snapMipEpochs;
                uint32_t snapBasePage = 0, snapPageCount = 0;
                uint32_t snapMipBasePage = 0, snapMipPageCount = 0;
                if (wwTexActive) {
                    if (poolVa != 0u) {
                        mcla::kernel::SnapshotGuestPageEpochs(poolVa, tiledSize, snapPageEpochs, &snapBasePage, &snapPageCount);
                    }
                    if (chainLevels > 1) {
                        mcla::kernel::SnapshotGuestPageEpochs(mipVa, static_cast<uint32_t>(mipTiledBytes),
                                                              snapMipEpochs, &snapMipBasePage, &snapMipPageCount);
                    }
                }
                static thread_local std::vector<uint8_t> tiledBuf;
                // Hash straight from live guest memory (no staging copy); the copy happens below on a cache MISS only.
                uint64_t uh = 0xcbf29ce484222325ull;
                if (poolVa != 0u &&
                    (!guestMem.IsValid(poolVa, tiledSize) ||
                     !HashGuestRangeSeh(uh, guestMem.Base() + poolVa, tiledSize, &uh))) {
                    guestTextures.push_back(e);
                    texPayloads.push_back({});
                    texW.push_back(0);
                    texH.push_back(0);
                    continue;
                }
                if (chainLevels > 1 &&   // the mip tail is part of the identity, so a rewritten mip misses
                    !HashGuestRangeSeh(uh, guestMem.Base() + mipVa, static_cast<size_t>(mipTiledBytes), &uh)) {
                    if (poolVa == 0u) {
                        guestTextures.push_back(e);
                        texPayloads.push_back({});
                        texW.push_back(0);
                        texH.push_back(0);
                        continue;
                    }
                    chainLevels = 1;
                }
                {
                    std::shared_ptr<const std::vector<uint8_t>> hitLin;
                    uint64_t hitHash = 0;
                    size_t bytesSnap = 0;
                    bool needStamp = false;
                    {
                        std::shared_lock<std::shared_mutex> rlk(s_untileMutex);
                        auto fit = s_untileCache.find(ukey);
                        if (fit != s_untileCache.end() && fit->second.hash == uh && fit->second.lin) {
                            hitLin = fit->second.lin;
                            hitHash = fit->second.hash;
                            bytesSnap = s_untileCacheBytes;
                            needStamp = (fit->second.checkedNs != presFrame) || wwTexActive;
                        }
                    }
                    if (hitLin) {
                        if (needStamp) {
                            std::unique_lock<std::shared_mutex> wlk(s_untileMutex);
                            auto fit = s_untileCache.find(ukey);
                            if (fit != s_untileCache.end() && fit->second.hash == uh) {
                                fit->second.checkedNs = presFrame;
                                if (wwTexActive) {
                                    fit->second.wwValid = (poolVa == 0u || snapPageCount > 0) &&
                                                          (chainLevels <= 1u || snapMipPageCount > 0);
                                    fit->second.basePage = snapBasePage;
                                    fit->second.pageCount = snapPageCount;
                                    fit->second.pageEpochs = std::move(snapPageEpochs);
                                    fit->second.mipBasePage = snapMipBasePage;
                                    fit->second.mipPageCount = snapMipPageCount;
                                    fit->second.mipEpochs = std::move(snapMipEpochs);
                                }
                            }
                        }
                        recordTlSlot(hitLin, hitHash, bytesSnap);
                        useCached(hitLin, hitHash, bytesSnap);
                        continue;
                    }
                }
                s_uMiss.fetch_add(1, std::memory_order_relaxed);
                STALL_STAGE("tex-untile");
                {   // MISS: stage a private copy and re-hash THAT copy, so the cached hash always describes the bytes untiled below
                    if (poolVa != 0u) {
                        if (tiledBuf.size() < tiledSize) tiledBuf.resize(tiledSize);
                        if (!guestMem.ReadBytes(poolVa, tiledBuf.data(), tiledSize)) {
                            guestTextures.push_back(e);
                            texPayloads.push_back({});
                            texW.push_back(0);
                            texH.push_back(0);
                            continue;
                        }
                    }
                    if (chainLevels > 1) {   // a failed tail read falls back to mip 0 (or refuses if poolVa == 0)
                        mipTiled.resize(static_cast<size_t>(mipTiledBytes));
                        if (!guestMem.ReadBytes(mipVa, mipTiled.data(), static_cast<uint32_t>(mipTiledBytes))) {
                            if (poolVa == 0u) {
                                guestTextures.push_back(e);
                                texPayloads.push_back({});
                                texW.push_back(0);
                                texH.push_back(0);
                                continue;
                            }
                            chainLevels = 1;
                        }
                    }
                    uh = 0xcbf29ce484222325ull;
                    if (poolVa != 0u) uh = HashBlock8(uh, tiledBuf.data(), tiledSize);
                    if (chainLevels > 1) uh = HashBlock8(uh, mipTiled.data(), mipTiled.size());
                }

                // Untile + convert endianness.
                std::vector<uint8_t> linearBuf(chainLinBytes, 0);   // mip 0 first, then the chain levels
                if (poolVa != 0u) {
                    if (gtd.tiled) {
                        mcla::native::TextureUntileInfo info{};
                        info.format = mcla::native::GetTextureFormatInfo(gtd.xenosFormat);
                        info.offsetX = 0;
                        info.offsetY = 0;
                        info.width = plan.blocksX;
                        info.height = plan.blocksY;
                        info.inputPitch = inputPitch;
                        info.inputHeight = inputHeight;
                        info.outputPitch = plan.blocksX;
                        info.srcBytes = tiledSize;
                        info.dstBytes = plan.linearByteSize;
                        mcla::native::TextureUntileRefusal refusal = mcla::native::TextureUntileRefusal::None;
                        if (is3D) {
                            mcla::native::UntileTexture3D(tiledBuf.data(), linearBuf.data(), info,
                                                         gtd.depthOrArraySize, &refusal);
                        } else if (plan.packedOffsetX || plan.packedOffsetY) {
                            // Base level inside the packed-mip tail: untile the region up to the base's
                            // far corner, then cut the base out (UntileTexture2D applies offsetX/Y to the
                            // destination too, so the offset cannot be passed directly).
                            mcla::native::TextureUntileInfo pi = info;
                            pi.width = plan.blocksX + plan.packedOffsetX;
                            pi.height = plan.blocksY + plan.packedOffsetY;
                            pi.outputPitch = pi.width;
                            static thread_local std::vector<uint8_t> packedRegion;
                            packedRegion.assign(size_t(pi.width) * pi.height * bpb, 0u);
                            pi.dstBytes = static_cast<uint32_t>(packedRegion.size());
                            mcla::native::UntileTexture2D(tiledBuf.data(), packedRegion.data(), pi, &refusal);
                            if (refusal == mcla::native::TextureUntileRefusal::None)
                                for (uint32_t y = 0; y < plan.blocksY; ++y)
                                    std::memcpy(linearBuf.data() + size_t(y) * plan.blocksX * bpb,
                                                packedRegion.data() + (size_t(y + plan.packedOffsetY) * pi.width + plan.packedOffsetX) * bpb,
                                                size_t(plan.blocksX) * bpb);
                        } else {
                            mcla::native::UntileTexture2D(tiledBuf.data(), linearBuf.data(), info, &refusal);
                        }
                        if (refusal != mcla::native::TextureUntileRefusal::None) {
                            // Untile failed -- leave payload null (BindGuestTextures refuses).
                            // F-902: keep texW/texH ALIGNED with texPayloads (push zeros
                            // like every other refusal path). Omitting them misaligns
                            // texW[i]/texH[i] from texPayloads[i] for all later slots,
                            // and the TEXEL-FLIP probe reads texPayloads[0] at
                            // texW/H[0]-derived offsets -- a short/empty payload with
                            // foreign dims AVs the render thread (ga020: parked).
                            guestTextures.push_back(e);
                            texPayloads.push_back({});
                            texW.push_back(0);
                            texH.push_back(0);
                            continue;
                        }
                    } else {
                        // Linear textures keep the guest row pitch (32-block aligned, Xenia texture_util.cc:324-329);
                        // copy row by row so a width that is not a multiple of 32 blocks does not shear the image.
                        const uint32_t dstRow = blocksX * bpb, srcRow = storedPitch * bpb;
                        const size_t srcBase = size_t(plan.packedOffsetY) * srcRow + size_t(plan.packedOffsetX) * bpb;  // packed-mip base
                        if ((srcRow > dstRow || srcBase) && plan.linearByteSize == dstRow * blocksY) {
                            for (uint32_t y = 0; y < blocksY && srcBase + size_t(y) * srcRow + dstRow <= tiledSize; ++y)
                                std::memcpy(linearBuf.data() + size_t(y) * dstRow, tiledBuf.data() + srcBase + size_t(y) * srcRow, dstRow);
                        } else if (tiledSize >= plan.linearByteSize) {
                            std::memcpy(linearBuf.data(), tiledBuf.data(), plan.linearByteSize);
                        } else {
                            std::memcpy(linearBuf.data(), tiledBuf.data(), tiledSize);
                        }
                    }
                }

                // P2a: mips 1..N-1, each its own 32-block-padded tiled grid (or packed tail subrect) cropped to host extent.
                if (chainLevels > 1) {
                    static thread_local std::vector<uint8_t> mipPackedRegion;
                    for (uint32_t L = 1; L < chainLevels; ++L) {
                        const auto& m = chain.levels[L];
                        mcla::native::TextureUntileInfo li{};
                        li.format = mcla::native::GetTextureFormatInfo(gtd.xenosFormat);
                        li.inputPitch = m.paddedBlocksX;    // surface = padded grid (texture_extent.cc)
                        li.inputHeight = m.paddedBlocksY;
                        li.srcBytes = m.byteSize;
                        mcla::native::TextureUntileRefusal r = mcla::native::TextureUntileRefusal::None;
                        if (m.tileOffsetX != 0u || m.tileOffsetY != 0u) {
                            li.width = m.tileOffsetX + m.blocksX;
                            li.height = m.tileOffsetY + m.blocksY;
                            li.outputPitch = li.width;
                            mipPackedRegion.assign(size_t(li.width) * li.height * bpb, 0u);
                            li.dstBytes = static_cast<uint32_t>(mipPackedRegion.size());
                            mcla::native::UntileTexture2D(mipTiled.data() + m.guestOffset,
                                                          mipPackedRegion.data(), li, &r);
                            if (r == mcla::native::TextureUntileRefusal::None) {
                                for (uint32_t y = 0; y < m.blocksY; ++y) {
                                    std::memcpy(linearBuf.data() + m.linearOffset + size_t(y) * m.blocksX * bpb,
                                                mipPackedRegion.data() + (size_t(y + m.tileOffsetY) * li.width + m.tileOffsetX) * bpb,
                                                size_t(m.blocksX) * bpb);
                                }
                            }
                        } else {
                            li.width = m.blocksX;           // visible region, top-left of the padded grid
                            li.height = m.blocksY;
                            li.outputPitch = m.blocksX;
                            li.dstBytes = m.linearBytes;
                            mcla::native::UntileTexture2D(mipTiled.data() + m.guestOffset,
                                                          linearBuf.data() + m.linearOffset, li, &r);
                        }
                        if (r != mcla::native::TextureUntileRefusal::None) { chainLevels = 1; break; }   // mip 0 stays valid
                    }
                    if (chainLevels <= 1u && poolVa == 0u) {
                        if (s_mipMinFix && origPoolVa != 0u && guestMem.IsValid(origPoolVa, plan.linearByteSize)) {
                            poolVa = origPoolVa;
                        } else {
                            guestTextures.push_back(e);
                            texPayloads.push_back({});
                            texW.push_back(0);
                            texH.push_back(0);
                            continue;
                        }
                    }
                    if (poolVa == 0u && chainLevels > 1u) {
                        // Missing mip 0 (!hasBase or mipMinLevel > 0): synthesize subresource 0
                        // from firstValidMip so D3D12 subresources are initialized.
                        const uint32_t firstValidMip = s_mipMinFix
                            ? std::min(std::max(1u, gtd.mipMinLevel), chainLevels - 1u)
                            : 1u;
                        const auto& srcMip = chain.levels[firstValidMip];
                        const uint32_t mbx = srcMip.blocksX ? srcMip.blocksX : 1u;
                        const uint32_t mby = srcMip.blocksY ? srcMip.blocksY : 1u;
                        const uint8_t* srcData = linearBuf.data() + srcMip.linearOffset;
                        for (uint32_t y = 0; y < plan.blocksY; ++y) {
                            const uint32_t sy = std::min(y >> firstValidMip, mby - 1u);
                            for (uint32_t x = 0; x < plan.blocksX; ++x) {
                                const uint32_t sx = std::min(x >> firstValidMip, mbx - 1u);
                                std::memcpy(linearBuf.data() + (size_t(y) * plan.blocksX + x) * bpb,
                                            srcData + (size_t(sy) * mbx + sx) * bpb, bpb);
                            }
                        }
                    }
                }
                // Convert endianness per mip level (mip 0 at offset 0) so odd-sized sub-word mips do not shift later levels.
                if (chainLevels > 1u) {
                    mcla::native::ConvertTextureEndianness(gtd.endianness, linearBuf.data(),
                                                           plan.linearByteSize, plan.texelBytes);
                    for (uint32_t L = 1u; L < chainLevels; ++L) {
                        const auto& m = chain.levels[L];
                        mcla::native::ConvertTextureEndianness(gtd.endianness,
                                                               linearBuf.data() + m.linearOffset,
                                                               m.linearBytes, plan.texelBytes);
                    }
                } else {
                    mcla::native::ConvertTextureEndianness(gtd.endianness, linearBuf.data(),
                                                           chainLinBytes, plan.texelBytes);
                }

                auto newLin = std::make_shared<std::vector<uint8_t>>(linearBuf);
                std::vector<std::shared_ptr<std::vector<uint8_t>>> deadLins;
                size_t cacheBytesAfter = 0;
                std::shared_ptr<const std::vector<uint8_t>> insertedLin = newLin;
                {
                    std::unique_lock<std::shared_mutex> wlk(s_untileMutex);
                    if (s_untileCacheBytes > (384u << 20)) {  // evict oldest-checked entries down to 3/4 of the cap (was a whole-map clear: a re-untile cliff)
                        std::vector<std::pair<int64_t, const UntileKey*>> age;
                        age.reserve(s_untileCache.size());
                        for (const auto& kv : s_untileCache) age.emplace_back(kv.second.checkedNs, &kv.first);
                        std::sort(age.begin(), age.end());
                        std::vector<UntileKey> victims;
                        size_t bytes = s_untileCacheBytes;
                        for (const auto& a : age) {
                            if (bytes <= (288u << 20)) break;
                            auto entIt = s_untileCache.find(*a.second);
                            if (entIt == s_untileCache.end()) continue;
                            bytes -= entIt->second.lin ? entIt->second.lin->size() : 0;
                            deadLins.push_back(std::move(entIt->second.lin));
                            victims.push_back(*a.second);
                        }
                        for (const auto& k : victims) s_untileCache.erase(k);
                        s_untileCacheBytes = bytes;
                    }
                    auto& slotc = s_untileCache[ukey];
                    s_untileCacheBytes += newLin->size() - (slotc.lin ? slotc.lin->size() : 0);
                    if (slotc.lin) deadLins.push_back(std::move(slotc.lin));
                    slotc.hash = uh;
                    slotc.checkedNs = presFrame;
                    if (wwTexActive) {
                        slotc.wwValid = (poolVa == 0u || snapPageCount > 0) &&
                                        (chainLevels <= 1u || snapMipPageCount > 0);
                        slotc.basePage = snapBasePage;
                        slotc.pageCount = snapPageCount;
                        slotc.pageEpochs = std::move(snapPageEpochs);
                        slotc.mipBasePage = snapMipBasePage;
                        slotc.mipPageCount = snapMipPageCount;
                        slotc.mipEpochs = std::move(snapMipEpochs);
                    } else {
                        slotc.wwValid = false;
                    }
                    slotc.lin = std::move(newLin);
                    cacheBytesAfter = s_untileCacheBytes;
                }
                recordTlSlot(insertedLin, uh, cacheBytesAfter);
                g_memUntileBytes.store(cacheBytesAfter, std::memory_order_relaxed);
                e.contentId = (uh * 31u + uhash) | 1u;
                // Store the untiled, endian-converted bytes.
                texPayloads.push_back(std::move(linearBuf));
                texW.push_back(gtd.width);
                texH.push_back(gtd.height);
                setChainLevels(e, texPayloads.back().data(), texPayloads.back().size(), chainLevels);
                guestTextures.push_back(e);
            }

            if (tsLins && texSetCached == texCount && guestTextures.size() == texCount &&
                texW.size() == texCount && texH.size() == texCount &&
                tsSeq0 == g_wwBurstSeq.load(std::memory_order_acquire) &&
                tsPres0 == static_cast<int64_t>(backend_.PresentedFrames())) {
                tsSlot->n = texCount;
                tsSlot->keyLen = tsKeyLen;
                std::memcpy(tsSlot->key, tsKey, tsKeyLen);
                tsSlot->ww = tsWw;
                tsSlot->seq = tsSeq0;
                tsSlot->pres = tsPres0;
                tsSlot->entries.assign(guestTextures.begin(), guestTextures.end());
                tsSlot->w.assign(texW.begin(), texW.end());
                tsSlot->h.assign(texH.begin(), texH.end());
                tsSlot->lins.swap(*tsLins);
                std::memcpy(tsSlot->smp, guestSamplerTab, sizeof(guestSamplerTab));
                tsSlot->smpMask = guestSamplerMask;
            }
            STALL_STAGE("vb-check");
            uint32_t streamCount = (!s_noMultistream && dc.vbStreamCount > 0u && dc.vbStreamCount <= 4u)
                                       ? dc.vbStreamCount
                                       : 1u;
            mcla::native::DrawCapturedCommand::CpVertexStreamBinding streams[4]{};
            if (!s_noMultistream && dc.vbStreamCount > 0u && dc.vbStreamCount <= 4u) {
                for (uint32_t s = 0u; s < streamCount; ++s) streams[s] = dc.vbStreams[s];
            } else {
                streams[0].vbBase = dc.vbBase;
                streams[0].vbSize = dc.vbSize;
                streams[0].vbStride = dc.vbStride;
                streams[0].slotIndex = (dc.vbElementCount > 0u) ? dc.vbElements[0].slotIndex : 0u;
                streams[0].endianness = (dc.vbElementCount > 0u) ? dc.vbElements[0].endianness : 0u;
            }

            static const bool s_vbDumpEnabled = [](){
                const char* e = std::getenv("MCLA_VB_DUMP");
                return e != nullptr && e[0] == '1';
            }();
            static const bool s_stride36UvDiv = [](){
                const char* e = std::getenv("MCLA_TEMP_STRIDE36_UVDIV");
                return e != nullptr && e[0] == '1';
            }();
            bool hasColorAt24 = false;
            if (s_stride36UvDiv) {
                for (uint32_t fe = 0u; fe < dc.vbElementCount &&
                                        fe < mcla::native::kCpVertexElementMax; ++fe) {
                    if (dc.vbElements[fe].offsetBytes == 24u &&
                        dc.vbElements[fe].fmt.dxgi == 28u) {
                        hasColorAt24 = true;
                        break;
                    }
                }
            }

            // Non-indexed quad-expanding shaders (e.g. foliage billboards) fetch
            // via floor(SV_VertexID * 0.25) placed in r0.y/r0.z (srcSwizzle != 0).
            // IA mode leaves r0.x=1.0 and advances 1:1 per corner; XE_RAWVB evaluates
            // the guest fetch GPR via SV_VertexID.
            bool vsUsesNonZeroSrcFetch = false;
            for (uint32_t fe = 0u; fe < dc.vbElementCount &&
                                    fe < mcla::native::kCpVertexElementMax; ++fe) {
                const auto& el = dc.vbElements[fe];
                if (el.miniFetch == 0u &&
                    (el.srcReg != 0u || (el.srcSwizzle & 3u) != 0u)) {
                    vsUsesNonZeroSrcFetch = true;
                    break;
                }
            }

            // RAWVB check before multi-stream VB read (rawVb binds only stream 0 SRV).
            uint32_t vertexCount = dc.vbSize / dc.vbStride;
            bool rawVbDraw = false;
            if (!indexed) {
                const uint64_t vbWant =
                    (static_cast<uint64_t>(dc.idxOffset) +
                     static_cast<uint64_t>(dc.numIndices)) *
                    static_cast<uint64_t>(dc.vbStride);
                const bool isAutoQuad =
                    (dc.primitiveTopology == 13u && dc.numIndices >= 4u &&
                     (dc.numIndices % 4u) == 0u && dc.vbSize >= 4u);
                if (isAutoQuad &&
                    ((streamCount == 1u && vsUsesNonZeroSrcFetch) ||
                     vbWant > static_cast<uint64_t>(dc.vbSize))) {
                    if (streamCount > 1u) {
                        refuse("rawvb-multistream-unsupported", s_cpRefuseVbMissing,
                               "- non-indexed quad list RAWVB cannot bind >1 vertex stream.");
                        break;
                    }
                    rawVbDraw = true;
                } else if (vbWant > static_cast<uint64_t>(dc.vbSize)) {
                    refuse("vb-short", s_cpRefuseVbMissing,
                           "- non-indexed draw: (idxOffset + num_indices) * vbStride exceeds the "
                             "resolved vertex buffer, so the draw would read past "
                             "its own end. See numIdx/vb/stride on this line.");
                    s_cpRefuseVbShortRefused.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                vertexCount = dc.numIndices;
            }

            bool vbAborted = false;
            bool uvRewritten = false;
            for (uint32_t s = 0u; s < streamCount; ++s) {
                const auto& st = streams[s];
                prep.streamStrides[s] = st.vbStride;
                if (st.vbBase == 0u || st.vbSize == 0u || st.vbStride == 0u) {
                    refuse("vb-missing", s_cpRefuseVbMissing,
                           "- multi-stream vertex buffer descriptor incomplete.");
                    vbAborted = true;
                    break;
                }
                if (st.vbSize > 67108864u) {
                    static StatCtr<uint32_t> s_vbSizeRef{0};
                    const uint32_t n2 = s_vbSizeRef.fetch_add(1) + 1;
                    if (n2 <= 4 || (n2 % 200) == 0) {
                        MCLA_LOG_ERROR("RENDER-VB-SIZE-REFUSE #{} vbBase={:08X} "
                                       "vbSize={} -- NULL draw (bind refuses)",
                                       n2, st.vbBase, st.vbSize);
                    }
                    refuse("vb-size-wild", s_cpRefuseVbMissing,
                           "- vertex buffer size exceeds the 64 MiB host cap; "
                           "refused rather than copied.");
                    vbAborted = true;
                    break;
                }

                uint32_t streamEndian = mcla::native::kCpVertexEndianNone;
                bool streamEndianSeen = false;
                bool streamEndianSplit = false;
                for (uint32_t fe = 0u; fe < dc.vbElementCount &&
                                        fe < mcla::native::kCpVertexElementMax; ++fe) {
                    if (streamCount > 1u && dc.vbElements[fe].slotIndex != st.slotIndex) {
                        continue;
                    }
                    const uint32_t e = dc.vbElements[fe].endianness;
                    if (!streamEndianSeen) {
                        streamEndian = e;
                        streamEndianSeen = true;
                    } else if (e != streamEndian) {
                        streamEndianSplit = true;
                        break;
                    }
                }
                if (!streamEndianSeen) {
                    if (streamCount == 1u && dc.vbElementCount == 0u) {
                        refuse("vb-endian-absent", s_cpRefuseVbEndianAbsent,
                               "- the command carries no vertex element, so it carries "
                                 "no fetch constant and therefore no byte-order selector. "
                                 "NOT assumed to be kNone: a missing selector is not the "
                                 "same fact as a guest that stated one.");
                        vbAborted = true;
                        break;
                    }
                    streamEndian = st.endianness;
                }
                if (streamEndianSplit) {
                    refuse("vb-endian-split", s_cpRefuseVbEndianSplit,
                           "- two elements of the same vertex stream name different byte orders, "
                             "so no single transform can be applied to it.");
                    vbAborted = true;
                    break;
                }
                if (!mcla::native::CpVertexEndianIsArchitected(streamEndian)) {
                    refuse("vb-endian-unknown", s_cpRefuseVbEndianUnknown,
                           "- the carried byte-order selector is outside the four "
                             "architected values, and the guest field is 2 bits wide, "
                             "so this is a producer value, not a guest one. NOT "
                             "masked, rounded or clamped into 0..3.");
                    vbAborted = true;
                    break;
                }

                const bool stream0NeedsCpuBytes =
                    (s == 0u) &&
                    ((!indexed && dc.primitiveTopology == 8u) ||
                     (s_stride36UvDiv && hasColorAt24 && st.vbStride == 36u && !texW.empty() && !texH.empty()) ||
                     s_vbDumpEnabled);
                const bool vbCanPersist = wwActive && !s_noPersistentGeo &&
                                          !stream0NeedsCpuBytes && st.vbSize >= 256u;
                const uint64_t vbKey = vbCanPersist
                    ? MakeR1GeoKey(st.vbBase, st.vbSize,
                                   (uint64_t(st.vbStride) << 32) | (uint64_t(streamEndian) << 16) | st.slotIndex,
                                   0x56420001ull)
                    : 0ull;
                if (vbCanPersist) {
                    std::shared_lock<std::shared_mutex> rlk(s_r1GeoMutex);
                    auto it = s_r1GeoTracker.find(vbKey);
                    if (it != s_r1GeoTracker.end() && !it->second.transient &&
                        it->second.gpuVa != 0 && it->second.pageCount > 0 &&
                        it->second.sizeBytes == st.vbSize &&
                        mcla::kernel::GuestPageRangeMatchesEpochs(
                            it->second.basePage, it->second.pageCount, it->second.epochs.data())) {
                        prep.vbCleanHit[s] = true;
                        prep.vbPersistentKey[s] = vbKey;
                        prep.vbContentHash[s] = it->second.contentHash;
                        prep.desc.residentVbBytes[s] = it->second.sizeBytes;
                        prep.desc.residentVbStride[s] = st.vbStride;
                        continue;
                    }
                }

                if (vbCanPersist) {
                    mcla::kernel::SnapshotGuestPageEpochs(
                        st.vbBase, st.vbSize, prep.vbEpochSnap[s], &prep.vbBasePage[s], &prep.vbPageCount[s]);
                }

                std::vector<uint8_t>& sBuf = (s == 0u) ? vb : prep.vbStreamsExtra[s - 1u];
                if (sBuf.size() != st.vbSize) sBuf.resize(st.vbSize);
                if (s == 0u) STALL_STAGE("vb-fill");
                if (!guestMem.ReadBytes(st.vbBase, sBuf.data(), st.vbSize)) {
                    refuse("vb-unreadable", s_cpRefuseVbMissing,
                           "- vertex buffer range validated but ReadBytes failed.");
                    vbAborted = true;
                    break;
                }

                if (s == 0u && s_vbDumpEnabled) {
                    static StatCtr<uint32_t> s_vbDump{0};
                    const uint32_t dn = s_vbDump.fetch_add(1) + 1;
                    if (dn <= 900 && dc.vbSize <= 65536u && dc.vbStride > 0 &&
                        (dc.vbSize % dc.vbStride) == 0u) {
                        char name[96];
                        std::snprintf(name, sizeof(name), "vb_raw_%04d_%08X_s%02u_%u.bin",
                                      dn, dc.vbBase, dc.vbStride, dc.vbSize);
                        if (std::FILE* f = std::fopen(name, "wb")) {
                            std::fwrite(sBuf.data(), 1, dc.vbSize, f);
                            std::fclose(f);
                        }
                        if (dn <= 8 || (dn % 200) == 0) {
                            char els[280] = {0};
                            size_t w = 0;
                            for (uint32_t fe = 0u;
                                 fe < dc.vbElementCount && fe < mcla::native::kCpVertexElementMax &&
                                 w + 48 < sizeof(els); ++fe) {
                                w += static_cast<size_t>(std::snprintf(
                                    els + w, sizeof(els) - w,
                                    "e%u:off%u,st%u,xs%u,b%u,en%u,",
                                    dc.vbElements[fe].elementIndex,
                                    dc.vbElements[fe].offsetBytes,
                                    dc.vbElements[fe].strideBytes,
                                    dc.vbElements[fe].fmt.xenos,
                                    dc.vbElements[fe].fmt.bytes,
                                    dc.vbElements[fe].endianness));
                            }
                            MCLA_LOG_WARN("VB-DUMP #{} base={:08X} size={} stride={} nelem={} nverts={} [{}]",
                                          dn, dc.vbBase, dc.vbSize, dc.vbStride, dc.vbElementCount,
                                          (dc.vbStride > 0) ? (dc.vbSize / dc.vbStride) : 0u, els);
                            for (uint32_t tb = 0u;
                                 tb < dc.textureCount && tb < 4u; ++tb) {
                                mcla::native::GuestTextureDesc bt{};
                                const uint8_t* bts = dc.textureDescriptorBytes[tb];
                                if (mcla::native::DecodeGuestTextureDescriptor(
                                        bts, bt) && bt.valid) {
                                    MCLA_LOG_WARN(
                                        "TEXTURE-BIND #{} ti={} dim={} "
                                        "baseAddress={:08X} {}x{} "
                                        "xenos={} endianness={} "
                                        "tiled={} poolVa={:08X}",
                                        dn, tb,
                                        static_cast<uint32_t>(bt.dimension),
                                        bt.baseAddress, bt.width, bt.height,
                                        bt.xenosFormat,
                                        bt.endianness, bt.tiled,
                                        dc.texturePoolVa[tb]);
                                } else {
                                    MCLA_LOG_WARN(
                                        "TEXTURE-BIND #{} ti={} "
                                        "DECODE-REFUSED(first4={:02X}{:02X}"
                                        "{:02X}{:02X})",
                                        dn, tb, bts[0], bts[1], bts[2], bts[3]);
                                }
                            }
                        }
                    }
                }

                if (streamEndian != mcla::native::kCpVertexEndianNone) {
                    const uint32_t dwords = st.vbSize / 4u;
                    uint8_t *vbPtr = sBuf.data();
                    switch (streamEndian) {
                    case mcla::native::kCpVertexEndian8in16:  VbSwapDwords(vbPtr, dwords, 1); break;
                    case mcla::native::kCpVertexEndian8in32:  VbSwapDwords(vbPtr, dwords, 3); break;
                    case mcla::native::kCpVertexEndian16in32: VbSwapDwords(vbPtr, dwords, 2); break;
                    default: break;
                    }
                }

                if (s_stride36UvDiv && s == 0u && hasColorAt24 && st.vbStride == 36u && st.vbSize >= 36u && !texW.empty() && !texH.empty()) {
                    const uint32_t w = texW[0];
                    const uint32_t h = texH[0];
                    if (w > 0u && h > 0u) {
                        const uint32_t numVerts = st.vbSize / 36u;
                        for (uint32_t v = 0; v < numVerts; ++v) {
                            uint8_t* uvb = sBuf.data() + v * 36u + 28u;
                            float u = 0.f, vv = 0.f;
                            std::memcpy(&u, uvb, 4);
                            std::memcpy(&vv, uvb + 4, 4);
                            const float amax = std::max(std::abs(u), std::abs(vv));
                            if (amax > 2.0f) {
                                float nu = u / static_cast<float>(w);
                                float nv = vv / static_cast<float>(h);
                                nu -= std::floor(nu);
                                nv -= std::floor(nv);
                                u = nu;
                                vv = nv;
                                std::memcpy(uvb, &u, 4);
                                std::memcpy(uvb + 4, &vv, 4);
                                uvRewritten = true;
                            }
                        }
                    }
                }

                if (vbCanPersist && prep.vbPageCount[s] > 0 && !(s == 0u && uvRewritten)) {
                    prep.vbCheckPersistent[s] = true;
                    prep.vbPersistentKey[s] = vbKey;
                    prep.vbContentHash[s] = HashBlock8(0xcbf29ce484222325ull, sBuf.data(), st.vbSize);
                }
            }
            if (vbAborted) break;

            // F-648: the FORMAT gate, which did not exist because the format was
            // never carried (F-632 printed it and dropped it). Three checks, in
            // order, each with its own counter.
            bool vbFmtUnmapped = false;
            bool vbElemOutOfRange = false;
            for (uint32_t fe = 0u; fe < dc.vbElementCount; ++fe) {
                if (fe >= mcla::native::kCpVertexElementMax) {
                    vbElemOutOfRange = true;
                    break;
                }
                if (dc.vbElements[fe].fmt.known == 0u ||
                    dc.vbElements[fe].fmt.bytes == 0u) {
                    vbFmtUnmapped = true;
                    break;
                }
            }
            if (vbElemOutOfRange) {
                refuse("vb-elem-out-of-range", s_cpRefuseVbElemOutOfRange,
                       "- vbElementCount exceeds kCpVertexElementMax, so the "
                         "captured element table and the array holding it "
                         "disagree. NOT walked; NOT drawn.");
                break;
            }
            if (vbFmtUnmapped) {
                refuse("vb-fmt-unmapped", s_cpRefuseVbFmtUnmapped,
                       "- the vfetch's 6-bit data_format() is not one of the "
                         "named xenos::VertexFormats, so it has no byte size "
                         "and no DXGI equivalent. NOT defaulted to any format. "
                         "(element ordinal and format id are on "
                         "CP-FETCHVB-DESC / DRAW-CP-LAYOUT.)");
                break;
            }
            if (dc.vbElementCount == 0u &&
                (dc.vbFormat.known == 0u || dc.vbFormat.bytes == 0u)) {
                refuse("vb-fmt-unmapped", s_cpRefuseVbFmtUnmapped,
                       "- the vfetch's 6-bit data_format() is not one of the named "
                         "xenos::VertexFormats, so it has no byte size and no DXGI "
                         "equivalent. NOT defaulted to any format.");
                break;
            }
            if (dc.vbStride < dc.vbFormat.bytes) {
                refuse("vb-fmt-stride", s_cpRefuseVbFmtStride,
                       "- vbStride is smaller than the decoded format's own size, "
                         "so one vertex cannot hold one element. NOT drawn; the "
                         "decode is wrong, not the draw.");
                break;
            }
            if (!indexed && !rawVbDraw &&
                (static_cast<uint64_t>(dc.idxOffset) +
                 static_cast<uint64_t>(dc.numIndices)) *
                        static_cast<uint64_t>(dc.vbFormat.bytes) >
                    static_cast<uint64_t>(dc.vbSize)) {
                refuse("vb-fmt-short", s_cpRefuseVbFmtShort,
                       "- non-indexed draw: (idxOffset + num_indices) * the decoded format's size "
                         "exceeds the resolved vertex buffer. See numIdx/vb/stride on this "
                         "line and stride/fmtB on CP-DRAW-BRIDGE.");
                break;
            }

            D3D12Backend::DynamicMeshDesc& desc = prep.desc;
            desc.rawVb = rawVbDraw;
            desc.streamCount = streamCount;
            {   // guest window scissor: clips HUD text (radio ticker) and the minimap to their boxes
                namespace rqw = mcla::native;
                const auto& sr = dc.rasterState;
                const uint32_t need = (1u << rqw::CpSnapshotReg::kCpSnapWinScissorTL) | (1u << rqw::CpSnapshotReg::kCpSnapWinScissorBR);
                if ((sr.presentMask & need) == need) {
                    const uint32_t tl = sr.regs[rqw::CpSnapshotReg::kCpSnapWinScissorTL], br = sr.regs[rqw::CpSnapshotReg::kCpSnapWinScissorBR];
                    const bool haveWo = !(tl >> 31) &&
                                        (sr.presentMask & (1u << rqw::CpSnapshotReg::kCpSnapWinOffset)) != 0u;
                    const uint32_t wo = haveWo ? sr.regs[rqw::CpSnapshotReg::kCpSnapWinOffset] : 0u;
                    const int32_t ox = int32_t(wo << 17) >> 17, oy = int32_t(wo << 1) >> 17;
                    const int32_t l = int32_t(tl & 0x3FFFu) + ox, t = int32_t((tl >> 16) & 0x3FFFu) + oy;
                    const int32_t r = int32_t(br & 0x3FFFu) + ox, b = int32_t((br >> 16) & 0x3FFFu) + oy;
                    if (r > l && b > t) {
                        desc.winScissorValid = true; desc.winScissorL = l; desc.winScissorT = t; desc.winScissorR = r; desc.winScissorB = b;
                    }
                }
            }
            bool vpYFlipped = false;
            float vpXs = 0.0f, vpXo = 0.0f, vpYs = 0.0f, vpYo = 0.0f;
            {   // Guest viewport (vp120 census: 640x360 / 320x180 / 640x640 on 640/320-pitch targets).
                namespace rqs = mcla::native;
                const auto& vr = dc.rasterState;
                const uint32_t vte = vr.regs[rqs::CpSnapshotReg::kCpSnapPaClVteCntl];
                const uint32_t need = (1u << rqs::CpSnapshotReg::kCpSnapVpXScale) | (1u << rqs::CpSnapshotReg::kCpSnapVpYOffset) |
                                      (1u << rqs::CpSnapshotReg::kCpSnapPaClVteCntl);
                if ((vr.presentMask & need) == need && (vte & 0xFu) == 0xFu) {
                    std::memcpy(&vpXs, &vr.regs[rqs::CpSnapshotReg::kCpSnapVpXScale], 4);
                    std::memcpy(&vpXo, &vr.regs[rqs::CpSnapshotReg::kCpSnapVpXOffset], 4);
                    std::memcpy(&vpYs, &vr.regs[rqs::CpSnapshotReg::kCpSnapVpYScale], 4);
                    std::memcpy(&vpYo, &vr.regs[rqs::CpSnapshotReg::kCpSnapVpYOffset], 4);
                    const float w = 2.0f * std::fabs(vpXs), h = 2.0f * std::fabs(vpYs);
                    if (std::isfinite(vpXs + vpXo + vpYs + vpYo) && w >= 1.0f && h >= 1.0f && w <= 8192.0f && h <= 8192.0f) {
                        desc.guestVpValid = true;
                        desc.guestVpX = vpXo - std::fabs(vpXs); desc.guestVpY = vpYo - std::fabs(vpYs);
                        desc.guestVpW = w; desc.guestVpH = h;
                        float vpZs = 1.0f;
                        if (vr.presentMask & (1u << rqs::CpSnapshotReg::kCpSnapVpZScale)) {
                            std::memcpy(&vpZs, &vr.regs[rqs::CpSnapshotReg::kCpSnapVpZScale], 4);
                        }
                        static const bool s_noStdZFixVp = []() {
                            const char* e = std::getenv("MCLA_NO_STDZ_SHADOW_FIX");
                            return e && e[0] == '1';
                        }();
                        const bool vteHasZScale = (vte & 0x10u) != 0u;
                        desc.depthMirrored = DepthCompareMirrored() && (s_noStdZFixVp ? true : (vteHasZScale && vpZs < 0.0f));
                        vpYFlipped = !s_noYFlip && (vpYs > 0.0f) && ((vte & 0x4u) != 0u);
                        // TEMP-YFLIP-CENSUS: count positive-y-scale draws that do NOT get the flip
                        // (the VTE bit-4 gate). If a significant class exists, those draws appear
                        // vertically mirrored about the horizon (floating rooftops in the title).
                        static const bool s_yflipCensus = [] {
                            const char* e = std::getenv("MCLA_TEMP_YFLIP_CENSUS");
                            return e && e[0] == '1';
                        }();
                        if (s_yflipCensus && vpYs > 0.0f) {
                            if ((vte & 0x4u) == 0u) {
                                static std::atomic<uint32_t> s_yPosNoFlip{0};
                                const uint32_t n = s_yPosNoFlip.fetch_add(1) + 1;
                                if (n <= 8 || (n % 4096u) == 0u)
                                    MCLA_LOG_ERROR("YFLIP-CENSUS ys>0 vteY=0 n={} ys={:.3f} vte={:08X}",
                                                   n, vpYs, vte);
                            } else {
                                // Positive control: proves this census path can fire.
                                static std::atomic<uint32_t> s_yPosFlip{0};
                                const uint32_t n = s_yPosFlip.fetch_add(1) + 1;
                                if (n <= 4 || (n % 4096u) == 0u)
                                    MCLA_LOG_ERROR("YFLIP-CENSUS-CONTROL ys>0 vteY=1 n={} ys={:.3f}", n, vpYs);
                            }
                        }
                    }
                }
            }
            if (dc.rtCaptured) {  // RT-CACHE: the draw's own EDRAM registers
                desc.hasRt = true;
                desc.rtSurfaceInfo = dc.rtSurfaceInfo;
                desc.rtColorInfo = dc.rtColorInfo;
                desc.rtDepthInfo = dc.rtDepthInfo;
                desc.rtScissorBr = dc.rtScissorBr;
                if (!s_noMrt) {
                    if (dc.rtBoundMask == 0u && dc.psProgram.present == 0u) {
                        desc.rtCount = 0;
                        desc.rtActiveMask = 0;
                    } else if (dc.rtBoundMask != 0u) {
                        uint8_t rtCnt = 0;
                        for (uint32_t i = 0u; i < 4u; ++i) {
                            desc.rtColorInfoMrt[i] = dc.rtColorInfoMrt[i];
                            if (dc.rtBoundMask & (1u << i)) rtCnt = static_cast<uint8_t>(i + 1u);
                        }
                        desc.rtActiveMask = static_cast<uint8_t>(dc.rtBoundMask & 0xFu);
                        desc.rtCount = rtCnt;
                    } else {
                        desc.rtColorInfoMrt[0] = dc.rtColorInfo;
                        desc.rtActiveMask = 1;
                        desc.rtCount = 1;
                    }
                } else {
                    desc.rtColorInfoMrt[0] = dc.rtColorInfo;
                    desc.rtActiveMask = 1;
                    desc.rtCount = 1;
                }
            }
            if (dc.rasterState.presentMask & (1u << mcla::native::CpSnapshotReg::kCpSnapDepthClear)) {  // CP path only
                desc.guestDepthClearValid = true;
                desc.guestDepthClear = dc.rasterState.regs[mcla::native::CpSnapshotReg::kCpSnapDepthClear];
            }
            desc.vertexBytes = vb.empty() ? nullptr : vb.data();
            desc.vertexBytesSize = static_cast<uint32_t>(vb.size());
            if (!prep.vbCleanHit[0] && streamCount == 1u && dc.vbSize >= 16384u && !vb.empty()) {
                uint64_t h = 1469598103934665603ull;
                auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
                mix(dc.vbBase); mix(dc.vbSize); mix(dc.vbStride); mix(dc.vbElementCount);
                for (uint32_t fe = 0; fe < dc.vbElementCount && fe < mcla::native::kCpVertexElementMax; ++fe)
                    mix((uint64_t(dc.vbElements[fe].fmt.xenos) << 32) | (uint64_t(dc.vbElements[fe].offsetBytes) << 8) | dc.vbElements[fe].endianness);
                const uint8_t* vp = vb.data();
                const size_t hashBytes = std::min<size_t>(dc.vbSize, vb.size());
                h = HashBlock8(h, vp, hashBytes);
                desc.vbDedupeKey = h | 1ull;
            }
            desc.vertexStride = dc.vbStride;
            desc.vertexCount = vertexCount;
            {
                static const bool s_uvDiagEnabled = [] { const char* e = std::getenv("MCLA_LOG_LEVEL"); return e && e[0] >= '0' && e[0] <= '2'; }();
                static std::atomic<uint32_t> s_uvDiag{0};
                const uint32_t ud = s_uvDiagEnabled ? (s_uvDiag.fetch_add(1, std::memory_order_relaxed) + 1u) : 0u;
                if (s_uvDiagEnabled && !vb.empty() && vb.size() >= 36 && (ud <= 4 || (ud % 500) == 0)) {
                    // TEMP F-900 FULL-VERT0 (remove after UV/layout overlap is
                    // settled): the three-dword read below assumed TEXCOORD0 at
                    // off28/32 (stride-36 layout). Some draws show insane values
                    // there (-653.95: ga001 #2) and some sane (0.207: ga001 #3)
                    // under the SAME layout, so the question is what the whole
                    // first vertex holds and where the sane floats actually sit.
                    // Dump vert-0..2 raw (up to 84B = 3 stride-28 verts) + as
                    // floats, with vbBase so a stale/unwritten buffer can be told
                    // from a misdecoded one. Extended 2026-10-28: prim-8's v0 UV
                    // is (0,0,0,1) (ga012 #1) -- need v1/v2 to tell constant
                    // black (all verts ~0) from a gradient (varying UVs).
                    {
                        char hex[253] = {};
                        char flt[353] = {};
                        const uint32_t avail =
                            (dc.vbSize < 84u) ? dc.vbSize : 84u;
                        for (uint32_t k = 0; k < avail; ++k) {
                            std::snprintf(hex + k * 2, 3, "%02X",
                                          vb.data()[k]);
                        }
                        for (uint32_t k = 0; k + 4 <= avail && k < 84; k += 4) {
                            float fv = 0;
                            std::memcpy(&fv, vb.data() + k, 4);
                            char tmp[32] = {};
                            std::snprintf(tmp, sizeof(tmp), "%.3g,",
                                          static_cast<double>(fv));
                            std::strncat(flt, tmp,
                                         sizeof(flt) - std::strlen(flt) - 1);
                        }
                        MCLA_LOG_INFO("VERTEX-V0 #{} vbBase={:08X} stride={} "
                                      "bytes={} floats=[{}]",
                                      ud, dc.vbBase, dc.vbStride, hex, flt);
                    }
                    uint32_t w6 = 0, w7 = 0, w8 = 0;
                    float f6 = 0, f7 = 0, f8 = 0;
                    std::memcpy(&w6, vb.data() + 24, 4);
                    std::memcpy(&w7, vb.data() + 28, 4);
                    std::memcpy(&w8, vb.data() + 32, 4);
                    std::memcpy(&f6, &w6, 4);
                    std::memcpy(&f7, &w7, 4);
                    std::memcpy(&f8, &w8, 4);
                    MCLA_LOG_INFO("VERTEX-UV #{} stride={} off24={:08X}({:.4f}) "
                                  "off28={:08X}({:.4f}) off32={:08X}({:.4f})",
                                  ud, dc.vbStride, w6, f6, w7, f7, w8, f8);
                    // F-900: CPU-side texel probe. Samples ti=0's linear payload
                    // at vert-0's own UV -- the exact texel the GPU SampleLevel
                    // would read (modulo filtering). Settles content-vs-pipeline
                    // without touching the GPU: non-zero here + black on screen
                    // = pipeline fault; zero here = UVs point at empty atlas.
                    if (!texPayloads.empty() && !texPayloads[0].empty() &&
                        texW.size() > 0 && texH.size() > 0 && texW[0] > 0 &&
                        f7 >= 0.0f && f7 < 1.0f && f8 >= 0.0f && f8 < 1.0f) {
                        uint32_t tw = texW[0], th = texH[0];
                        uint32_t tx = static_cast<uint32_t>(f7 * tw);
                        uint32_t ty = static_cast<uint32_t>(f8 * th);
                        if (tx >= tw) tx = tw - 1;
                        if (ty >= th) ty = th - 1;
                        const uint8_t* px = texPayloads[0].data() +
                                            (static_cast<size_t>(ty) * tw + tx) * 4u;
                        // Guard against short payloads (shouldn't happen:
                        // linearByteSize == w*h*4 for RGBA8).
                        if ((static_cast<size_t>(ty) * tw + tx + 1u) * 4u <=
                            texPayloads[0].size()) {
                            MCLA_LOG_INFO("TEXEL-AT-UV #{} u={:.4f} v={:.4f} "
                                          "tex={}x{} xy={}x{} rgba=({},{},{},{})",
                                          ud, f7, f8, tw, th, tx, ty,
                                          px[0], px[1], px[2], px[3]);
                            // TEMP F-900 Y-FLIP probe (remove after overlap is
                            // settled): sample the VERTICALLY MIRRORED texel too.
                            // If the guest's v=0 is the image BOTTOM, content in
                            // rows 0-63 is really at v 0.75-1.0 and every direct
                            // sample misses while the mirror hits.
                            // F-902: BOUND the mirror read like the direct one
                            // above. An untile refusal once left texW/texH
                            // misaligned with texPayloads (fixed at the push
                            // site), so tw/th can describe a LARGER texture
                            // than payload[0] holds; sampling (fy*tw+tx) then
                            // reads OOB and AVs the render thread (ga020:
                            // parked at TEXEL-FLIP after TEXEL-AT-UV #68500).
                            uint32_t fy = (th - 1) - ty;
                            if ((static_cast<size_t>(fy) * tw + tx + 1u) * 4u <=
                                texPayloads[0].size()) {
                                const uint8_t* fx = texPayloads[0].data() +
                                                    (static_cast<size_t>(fy) * tw + tx) * 4u;
                                MCLA_LOG_INFO("TEXEL-FLIP #{} u={:.4f} v={:.4f} "
                                              "xy={}x{} rgba=({},{},{},{})",
                                              ud, f7, f8, tx, fy,
                                              fx[0], fx[1], fx[2], fx[3]);
                            }
                        }
                    }
                }
            }
            // F-897 (2026-10-25): THE VERTEX DATA DUMP -- what the GPU is
            // actually asked to rasterise, in the bytes the backend uploads.
            // WHY: after F-896 the 2D UI's draws stopped being empty (573,717
            // triangle-class draws, prim0=0, emptyIdx=0, build/g914.log) and
            // the framebuffer is STILL one uniform colour, rgba=(5,8,13,255).
            // The remaining candidates are (a) the vertex bytes are not what the
            // guest wrote, (b) the layout/PSO makes the shader emit a constant,
            // (c) the viewport/scissor. This row separates (a) from the rest by
            // printing the first eight dwords of the TRANSFORMED buffer (the
            // byte-order selector has already been applied at :2531-2582) and
            // the same words read as floats. Sane screen-space positions in the
            // first dwords would rule (a) out; garbage would name it.
            // Log-only: nothing is written, nothing is skipped, the upload that
            // follows is untouched.
            {
              static StatCtr<uint32_t> s_vdump{0};
              const uint32_t vn =
                  s_vdump.fetch_add(1, std::memory_order_relaxed) + 1u;
              if (vn <= 12u || (vn % 500u) == 0u) {
                uint32_t dw[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                float fl[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                const size_t avail = std::min<size_t>(vb.size(), 32u);
                if (avail > 0u) {
                  std::memcpy(dw, vb.data(), avail);
                }
                std::memcpy(fl, dw, sizeof(fl));
                MCLA_LOG_WARN(
                    "DRAW-CP-VERTEX #{} vbBase={:08X} vbSize={} stride={} fmtB={} "
                    "descs={}/{} numIdx={} vtxCount={} | dword[0..7]=[{:08X} "
                    "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] "
                    "float[0..7]=[{:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} "
                    "{:.4f} {:.4f}]",
                    vn, dc.vbBase, dc.vbSize, dc.vbStride, dc.vbFormat.bytes,
                    dc.vbDescCount, dc.vbFullCount, dc.numIndices, vertexCount,
                    dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7], fl[0],
                    fl[1], fl[2], fl[3], fl[4], fl[5], fl[6], fl[7]);
              }
            }
            // F-633: the index half exists only for kDMA. For kAutoIndex these
            // stay at their zero-initialised values and the backend takes its
            // non-indexed branch, DrawInstanced(desc.vertexCount, 1, 0, 0)
            // (d3d12_backend.cpp:2701). No implicit 0..n-1 index buffer is
            // manufactured: upstream passes index_buffer=nullptr for this class.
            desc.indexed = indexed;
            // VGT_INDX_OFFSET is added to every index (indexed kDMA) and acts as
            // StartVertex for non-indexed draws (kAutoIndex / D3DDevice_DrawVertices).
            desc.baseVertexLocation = static_cast<int32_t>(dc.idxOffset);
            if (indexed) {
                if (!prep.ibCleanHit &&
                    !(dc.primitiveTopology == 13u || (dc.primitiveTopology == 6u && tc.count >= 3u))) {
                    const uint32_t ibSz = tc.count * (tc.isWide ? 4u : 2u);
                    prep.ib.resize(ibSz);
                    if (ibSz > 0u) {
                        std::memcpy(prep.ib.data(),
                                    tc.isWide ? static_cast<const void*>(tc.wide.data())
                                              : static_cast<const void*>(tc.narrow.data()),
                                    ibSz);
                    }
                    desc.indexBytes = prep.ib.data();
                } else {
                    desc.indexBytes = nullptr;
                }
                desc.indexBytesSize = tc.count * (tc.isWide ? 4u : 2u);
                desc.indexFormat = tc.isWide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
                // F-611: num_indices from VGT_DRAW_INITIATOR, NOT ibSize/elemSize.
                // The old handler derived it from the buffer length, which
                // over-drew every real index buffer by its unused tail.
                desc.indexCount = tc.count;
            }
            bool constBanksBuilt = false;
            auto ensureConstBanks = [&]() {
                if (constBanksBuilt) return;
                constBanksBuilt = true;
                STALL_STAGE("constbank");
                static const bool s_noNbSkip = [] { const char* e = std::getenv("MCLA_TEMP_NO_NATIVEBANK_SKIP"); return e && e[0] == '1'; }();
                if (s_noNbSkip) s_nativeBankId = 0;
                const bool nativeBank = ((dc.frameId & 0x80000000u) && dc.frameId == s_nativeBankId) ||
                    (s_nativeBankId = 0, mcla::gnh::FillNativeConstants(dc.frameId, s_constBank.data(),
                                                   s_constBankPs.data(),
                                                   mcla::renderer::kShaderConstantBankBytes) && (s_nativeBankId = dc.frameId, true));
                if (nativeBank) {
                    std::memcpy(prep.vsBank.data(), s_constBank.data(), mcla::renderer::kShaderConstantBankBytes);
                    std::memcpy(prep.psBank.data(), s_constBankPs.data(), mcla::renderer::kShaderConstantBankBytes);
                    prep.vsConstBytesSize = mcla::renderer::kShaderConstantBankBytes;
                    prep.psConstBytesSize = mcla::renderer::kShaderConstantBankBytes;
                    desc.guestConstBytes = prep.vsBank.data();
                    desc.guestConstBytesSize = prep.vsConstBytesSize;
                    desc.guestPsConstBytes = prep.psBank.data();
                    desc.guestPsConstBytesSize = prep.psConstBytesSize;
                    {   // TEMP-NATIVE-VS-CP-CONST: native shadow bank vs CP register-file bank (PS c254/c255)
                        static StatCtr<uint32_t> s_nc{0};
                        const uint32_t q = s_nc.fetch_add(1) + 1;
                        if (q <= 12 || (q % 20000) == 0) {
                            std::vector<uint8_t> cpPs(mcla::renderer::kShaderConstantBankBytes);
                            mcla::gpu::CpShaderConstantInfo inf{};
                            const bool ok = mcla::gpu::CpBuildShaderConstantBank(mcla::gpu::CpShaderStage::kPixel, cpPs.data(),
                                                                                 mcla::renderer::kShaderConstantBankBytes, &inf);
                            auto rf = [](const uint8_t* p, uint32_t o) { uint32_t u = (uint32_t(p[o])<<24)|(uint32_t(p[o+1])<<16)|(uint32_t(p[o+2])<<8)|p[o+3]; float f; std::memcpy(&f,&u,4); return f; };
                            auto cnt = [](const uint8_t* p) { uint32_t c = 0; for (uint32_t k = 0; k < 256; ++k) { uint32_t a=0; for (int j=0;j<16;++j) a|=p[k*16+j]; c += a!=0; } return c; };
                            const uint8_t* np = prep.psBank.data();
                            MCLA_LOG_WARN("TEMP-NATIVE-VS-CP-CONST #{} cpBuilt={} nativeNZ={} cpNZ={} native c254.raw={:02X}{:02X}{:02X}{:02X} c255.raw={:02X}{:02X}{:02X}{:02X} cp c254=({:.3f},{:.3f},{:.3f},{:.3f}) c255=({:.3f},{:.3f},{:.3f},{:.3f}) cp c26.x={:.3f} native c26.x(raw be)={:.3f}",
                                q, ok?1:0, cnt(np), cnt(cpPs.data()), np[4064],np[4065],np[4066],np[4067], np[4080],np[4081],np[4082],np[4083],
                                rf(cpPs.data(),4064),rf(cpPs.data(),4068),rf(cpPs.data(),4072),rf(cpPs.data(),4076),
                                rf(cpPs.data(),4080),rf(cpPs.data(),4084),rf(cpPs.data(),4088),rf(cpPs.data(),4092),
                                rf(cpPs.data(),416), rf(np,416));
                        }
                    }
                } else {
                    static StatCtr<uint32_t> s_tmpFb{0};  // TEMP-NATIVE-CONST-FALLBACK
                    const uint32_t fq = s_tmpFb.fetch_add(1) + 1;
                    if ((dc.frameId & 0x80000000u) && (fq <= 8 || (fq % 5000) == 0))
                        MCLA_LOG_WARN("TEMP-NATIVE-CONST-FALLBACK #{} frameId={:08X} (ring slot recycled -> stale CP bank)", fq, dc.frameId);
                    s_nativeBankId = 0;
                    if (FillGuestConstantBankTo(prep.vsBank.data(), prep.psBank.data(),
                                                mcla::renderer::kShaderConstantBankBytes, desc)) {
                        prep.vsConstBytesSize = desc.guestConstBytesSize;
                        prep.psConstBytesSize = desc.guestPsConstBytesSize;
                    }
                }
                // PERF: the memo keys on the buffer pointer. prep.vsBank/psBank are
                // per-draw slots, so the memo never hits. On the native path the
                // bytes came from the STABLE thread-local source arrays; hashing
                // those yields the identical value (bytes are memcpy'd verbatim)
                // and lets the 4-entry memo actually hit. Fallback keeps prep.
                STALL_STAGE("cb-hash");
                // PERF: a repeated native frameId (producer bank dedup) means s_constBank/Ps were not
                // refilled, so their hashes are still exact; skip the 2x4 KB memo compare.
                static thread_local uint32_t s_nativeHashId = 0;
                static thread_local uint64_t s_nativeVsHash = 0, s_nativePsHash = 0;
                static const bool s_noBankDedup = [] { const char* e = std::getenv("MCLA_NO_BANK_DEDUP"); return e && e[0] == '1'; }();
                if (nativeBank && !s_noBankDedup && s_nativeHashId == dc.frameId) {
                    prep.vsConstHash = s_nativeVsHash;
                    prep.psConstHash = s_nativePsHash;
                } else {
                if (prep.vsConstBytesSize > 0u) {
                    prep.vsConstHash = nativeBank
                        ? D3D12Backend::HashConstantBankMemo(s_constBank.data(), prep.vsConstBytesSize)
                        : D3D12Backend::HashConstantBankMemo(prep.vsBank.data(), prep.vsConstBytesSize);
                }
                if (prep.psConstBytesSize > 0u) {
                    prep.psConstHash = nativeBank
                        ? D3D12Backend::HashConstantBankMemo(s_constBankPs.data(), prep.psConstBytesSize)
                        : D3D12Backend::HashConstantBankMemo(prep.psBank.data(), prep.psConstBytesSize);
                }
                s_nativeHashId = nativeBank ? dc.frameId : 0u;
                s_nativeVsHash = prep.vsConstHash;
                s_nativePsHash = prep.psConstHash;
                }
                desc.vsConstPreHash = prep.vsConstHash;
                desc.psConstPreHash = prep.psConstHash;
                STALL_STAGE("cb-post");
            };

            // Wave 2 Deliverable 3: Zero-Emulation Clear-Rect Folding (prim == 8, MCLA_NO_CLEAR_FASTPATH=1 opt-out).
            if (!s_noClearFastpath && !indexed && dc.primitiveTopology == 8u && streamCount == 1u &&
                dc.rtCaptured != 0u && dc.numIndices >= 3u && (dc.numIndices % 3u) == 0u &&
                (dc.numIndices / 3u) <= 8u && dc.vbStride > 0u && (dc.vbStride % 4u) == 0u &&
                dc.vbElementCount > 0u && dc.vbElementCount <= mcla::native::kCpVertexElementMax &&
                static_cast<uint64_t>(dc.numIndices) * dc.vbStride <= vb.size() &&
                dc.vsProgram.present != 0u && dc.vsProgram.captured > 0u &&
                dc.vsProgram.captured <= kCpShaderBodyDwords &&
                (dc.psProgram.present == 0u ||
                 (dc.psProgram.captured > 0u && dc.psProgram.captured <= kCpShaderBodyDwords))) {
                bool floatOnly = true;
                for (uint32_t fe = 0u; fe < dc.vbElementCount; ++fe) {
                    const uint32_t dx = dc.vbElements[fe].fmt.dxgi;
                    if (dx != 2u && dx != 6u && dx != 16u && dx != 41u && dx != 34u && dx != 10u) {
                        floatOnly = false;
                        break;
                    }
                }
                const auto& rs = dc.rasterState;
                const bool haveClip = (rs.presentMask & (1u << CpSnapshotReg::kCpSnapPaClipCntl)) != 0u;
                const bool haveSu = (rs.presentMask & (1u << CpSnapshotReg::kCpSnapPaSuScModeCntl)) != 0u;
                const bool haveVte = (rs.presentMask & (1u << CpSnapshotReg::kCpSnapPaClVteCntl)) != 0u;
                const bool haveCc = (rs.presentMask & (1u << CpSnapshotReg::kCpSnapColorControl)) != 0u;
                const uint32_t ccReg = haveCc ? rs.regs[CpSnapshotReg::kCpSnapColorControl] : 0u;
                const bool alphaTestOn = haveCc && ((ccReg >> 3) & 1u) != 0u && (ccReg & 7u) != 7u;
                static const bool s_noAlphaToMaskClr = [] { const char* e = std::getenv("MCLA_NO_ALPHA_TO_MASK"); return e && e[0] == '1'; }();
                const bool alphaToMaskOn = !s_noAlphaToMaskClr && haveCc && ((ccReg >> 4) & 1u) != 0u;

                const bool haveColorMask = (rs.presentMask & (1u << CpSnapshotReg::kCpSnapColorMask)) != 0u;
                const uint32_t colorMask = haveColorMask ? rs.regs[CpSnapshotReg::kCpSnapColorMask] : 0xFu;
                const bool haveBlend0 = (rs.presentMask & (1u << CpSnapshotReg::kCpSnapBlendControl0)) != 0u;
                const uint32_t blend0 = rs.regs[CpSnapshotReg::kCpSnapBlendControl0];
                const bool blendIdentity = !haveBlend0 || (blend0 == 0x00010001u);

                const bool haveDc = (rs.presentMask & (1u << CpSnapshotReg::kCpSnapDepthControl)) != 0u;
                const uint32_t dcv = rs.regs[CpSnapshotReg::kCpSnapDepthControl];
                static const bool s_noStencilClr = [] { const char* e = std::getenv("MCLA_NO_STENCIL"); return e && e[0] == '1'; }();
                const bool stencilOn = !s_noStencilClr && haveDc && ((dcv & 1u) != 0u);
                const bool zEn = haveDc && ((dcv & 2u) != 0u);
                const bool zWr = haveDc && ((dcv & 4u) != 0u);
                const uint32_t zFn = (dcv >> 4) & 7u;
                const bool depthGateOk = !stencilOn && (!zEn || zFn == 7u);

                const uint32_t rtMask = s_noMrt ? 1u : (desc.rtActiveMask & 0xFu);
                const uint32_t rt0Mask = colorMask & 0xFu;
                const bool rtColorFormatValid =
                    (MapXenosColorFormatToDxgi((dc.rtColorInfo >> 16) & 0xFu) != DXGI_FORMAT_UNKNOWN);
                const bool colorClearAllowed =
                    (dc.psProgram.present != 0u) && ((rtMask & 1u) != 0u) && rtColorFormatValid &&
                    (rt0Mask == 0xFu) && ((rtMask & ~1u) == 0u || (colorMask & 0xFFF0u) == 0u) && blendIdentity;
                const bool colorMaskGateOk = (rt0Mask == 0u) || colorClearAllowed;
                const bool depthClearAllowed = zEn && zWr && (zFn == 7u);

                if (floatOnly && haveClip && haveSu && haveVte && !alphaTestOn && !alphaToMaskOn &&
                    depthGateOk && colorMaskGateOk && (colorClearAllowed || depthClearAllowed)) {
                    const uint32_t vsN = dc.vsProgram.captured;
                    const uint32_t psN = (dc.psProgram.present != 0u) ? dc.psProgram.captured : 0u;
                    uint64_t clrKey = HashBlock8(0xcbf29ce484222325ull ^ dc.psProgram.present,
                                                 reinterpret_cast<const uint8_t*>(dc.vsProgram.body), vsN * 4u);
                    if (psN > 0u) {
                        clrKey = HashBlock8(clrKey, reinterpret_cast<const uint8_t*>(dc.psProgram.body), psN * 4u);
                    }
                    std::shared_ptr<const ClearShaderCacheEnt> plan;
                    {
                        std::shared_lock<std::shared_mutex> clk(s_clearPlanMutex);
                        auto it = s_clearPlanCache.find(clrKey);
                        if (it != s_clearPlanCache.end() &&
                            it->second->psPresent == dc.psProgram.present &&
                            it->second->vsW.size() == vsN && it->second->psW.size() == psN &&
                            std::memcmp(it->second->vsW.data(), dc.vsProgram.body, vsN * 4u) == 0 &&
                            (psN == 0u || std::memcmp(it->second->psW.data(), dc.psProgram.body, psN * 4u) == 0)) {
                            plan = it->second;
                        }
                    }
                    if (!plan) {
                        static thread_local std::vector<uint8_t> cVsCode, cPsCode, cVsEnv, cPsEnv;
                        const char* dummySrc = nullptr;
                        const size_t vsBytes = ResolveStageCode(dc.vsProgram, cVsCode, dummySrc);
                        const size_t psBytes = (dc.psProgram.present != 0u)
                                                   ? ResolveStageCode(dc.psProgram, cPsCode, dummySrc)
                                                   : 0u;
                        auto ent = std::make_shared<ClearShaderCacheEnt>();
                        ent->psPresent = dc.psProgram.present;
                        ent->vsW.assign(dc.vsProgram.body, dc.vsProgram.body + vsN);
                        if (psN > 0u) ent->psW.assign(dc.psProgram.body, dc.psProgram.body + psN);
                        mcla::renderer::ShaderProgram vsP, psP;
                        if (vsBytes > 0u && BuildProgramEnvelope(cVsCode.data(), cVsCode.size(), true, cVsEnv) &&
                            mcla::renderer::ParseShaderProgram(cVsEnv.data(), cVsEnv.size(), vsP)) {
                            bool psOk = (dc.psProgram.present == 0u);
                            if (!psOk && psBytes > 0u &&
                                BuildProgramEnvelope(cPsCode.data(), cPsCode.size(), false, cPsEnv) &&
                                mcla::renderer::ParseShaderProgram(cPsEnv.data(), cPsEnv.size(), psP)) {
                                psOk = true;
                            }
                            if (psOk) {
                                ent->vsHash = mcla::renderer::HashShaderBytecode(cVsEnv);
                                ent->psHash = (dc.psProgram.present != 0u)
                                                  ? mcla::renderer::HashShaderBytecode(cPsEnv)
                                                  : 0ull;
                                ent->cls = mcla::renderer::ClassifyClearRectShaders(
                                    vsP, (dc.psProgram.present != 0u) ? &psP : nullptr,
                                    cVsEnv.data(), cVsEnv.size(),
                                    (dc.psProgram.present != 0u) ? cPsEnv.data() : nullptr,
                                    (dc.psProgram.present != 0u) ? cPsEnv.size() : 0u);
                                ent->vsRefs = mcla::renderer::ReferencedVertexInputs(vsP);
                            }
                        }
                        {
                            std::unique_lock<std::shared_mutex> clk(s_clearPlanMutex);
                            if (s_clearPlanCache.size() >= 4096) s_clearPlanCache.clear();
                            s_clearPlanCache[clrKey] = ent;
                        }
                        plan = std::move(ent);
                    }

                    uint32_t posOffB = 0u, posDxgi = 0u;
                    const uint32_t posComps = (plan && plan->cls.eligible &&
                                               FindClearAttrOffsetBytes(plan->vsRefs, dc, plan->cls.vs.posUsage,
                                                                        plan->cls.vs.posUsageIndex, posOffB, posDxgi))
                                                  ? DxgiFloatCompCount(posDxgi)
                                                  : 0u;
                    if (plan && plan->cls.eligible && posComps >= 2u &&
                        posOffB + posComps * 4u <= dc.vbStride) {
                        const uint32_t rectCount = dc.numIndices / 3u;
                        const bool wantColorClear = colorClearAllowed && plan->cls.ps.writesColor0;
                        const bool wantDepthClear = depthClearAllowed &&
                            (!plan->cls.ps.psPresent || !plan->cls.ps.writesDepth ||
                             plan->cls.ps.oDepth.source != mcla::renderer::ClearChannelSource::Invalid);
                        if (wantColorClear || wantDepthClear) {
                            ensureConstBanks();
                            bool paramsOk = true;
                            float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                            if (wantColorClear) {
                                for (uint32_t ci = 0u; ci < 4u; ++ci) {
                                    if (!EvalClearChannel(plan->cls.ps.oC0[ci], prep.vsBank.data(), prep.psBank.data(),
                                                          vb.data(), static_cast<uint32_t>(vb.size()), dc.vbStride,
                                                          dc.numIndices, plan->vsRefs, dc, clearColor[ci])) {
                                        paramsOk = false;
                                        break;
                                    }
                                }
                            }
                            float psFixedDepth = 0.0f;
                            if (paramsOk && wantDepthClear && plan->cls.ps.psPresent && plan->cls.ps.writesDepth) {
                                if (!EvalClearChannel(plan->cls.ps.oDepth, prep.vsBank.data(), prep.psBank.data(),
                                                      vb.data(), static_cast<uint32_t>(vb.size()), dc.vbStride,
                                                      dc.numIndices, plan->vsRefs, dc, psFixedDepth)) {
                                    paramsOk = false;
                                }
                            }

                            auto evalAxis = [&](const mcla::renderer::ClearPosAxisPlan& ax, const float vertPos[4],
                                                float& outVal) -> bool {
                                if (!ax.valid) return false;
                                float base = ax.fromVertexPos ? ((ax.vertComp < 4u) ? vertPos[ax.vertComp] : 0.0f) : 0.0f;
                                float sc = (ax.scaleConstReg >= 0)
                                               ? ReadBeFloatConst(prep.vsBank.data(), static_cast<uint32_t>(ax.scaleConstReg), ax.scaleConstComp)
                                               : 1.0f;
                                sc *= ax.inlineScale;
                                float off = (ax.offsetConstReg >= 0)
                                                ? ReadBeFloatConst(prep.vsBank.data(), static_cast<uint32_t>(ax.offsetConstReg), ax.offsetConstComp)
                                                : 0.0f;
                                off += ax.inlineOffset;
                                outVal = base * sc + off;
                                return std::isfinite(outVal);
                            };

                            uint64_t dummyRt = 0, dummyDs = 0;
                            uint32_t guestH = 0;
                            backend_.ComputeGuestPassKeys(desc, dummyRt, dummyDs, guestH);
                            const uint32_t rawGuestW = dc.rtSurfaceInfo & 0x3FFFu;
                            const uint32_t guestW = (rawGuestW > 0u) ? rawGuestW : backend_.GetWidth();
                            if (guestH == 0u) guestH = backend_.GetHeight();

                            const uint32_t vte = rs.regs[CpSnapshotReg::kCpSnapPaClVteCntl];
                            const uint32_t clipCntl = rs.regs[CpSnapshotReg::kCpSnapPaClipCntl];
                            const bool clipDisable = (clipCntl & (1u << 16)) != 0u;
                            const bool directPxSpace = clipDisable && ((vte & 0xFu) == 0u);

                            D3D12_RECT candRects[8] = {};
                            uint32_t candCount = 0u;
                            float hostDepth = 0.0f;
                            bool depthSet = false;

                            for (uint32_t q = 0u; paramsOk && q < rectCount; ++q) {
                                float px[3] = {}, py[3] = {}, pz[3] = {}, pw[3] = {};
                                for (uint32_t vi = 0u; vi < 3u; ++vi) {
                                    const uint8_t* vp = vb.data() + static_cast<size_t>(q * 3u + vi) * dc.vbStride + posOffB;
                                    std::memcpy(&px[vi], vp + 0u, 4u);
                                    std::memcpy(&py[vi], vp + 4u, 4u);
                                    pz[vi] = (posComps >= 3u) ? ([&] { float f = 0.0f; std::memcpy(&f, vp + 8u, 4u); return f; }()) : 0.0f;
                                    pw[vi] = (posComps >= 4u) ? ([&] { float f = 1.0f; std::memcpy(&f, vp + 12u, 4u); return f; }()) : 1.0f;
                                    if (!std::isfinite(px[vi] + py[vi] + pz[vi] + pw[vi])) { paramsOk = false; break; }
                                }
                                if (!paramsOk) break;
                                if (std::fabs(pz[0] - pz[1]) > 1e-5f || std::fabs(pz[0] - pz[2]) > 1e-5f ||
                                    std::fabs(pw[0] - pw[1]) > 1e-5f || std::fabs(pw[0] - pw[2]) > 1e-5f) {
                                    paramsOk = false;
                                    break;
                                }
                                const float xMin = std::min({px[0], px[1], px[2]});
                                const float xMax = std::max({px[0], px[1], px[2]});
                                const float yMin = std::min({py[0], py[1], py[2]});
                                const float yMax = std::max({py[0], py[1], py[2]});
                                const float spanX = xMax - xMin;
                                const float spanY = yMax - yMin;
                                if (!(spanX > 1e-5f && spanY > 1e-5f)) { paramsOk = false; break; }
                                const float tolX = std::max(1e-4f, spanX * 1e-4f);
                                const float tolY = std::max(1e-4f, spanY * 1e-4f);
                                bool rightTriOk = true;
                                for (uint32_t vi = 0u; vi < 3u; ++vi) {
                                    const bool eqX = (std::fabs(px[vi] - xMin) <= tolX) || (std::fabs(px[vi] - xMax) <= tolX);
                                    const bool eqY = (std::fabs(py[vi] - yMin) <= tolY) || (std::fabs(py[vi] - yMax) <= tolY);
                                    if (!eqX || !eqY) { rightTriOk = false; break; }
                                }
                                if (!rightTriOk) { paramsOk = false; break; }

                                const float vLo[4] = {xMin, yMin, pz[0], pw[0]};
                                const float vHi[4] = {xMax, yMax, pz[0], pw[0]};
                                float oLo[4] = {}, oHi[4] = {};
                                for (uint32_t c = 0u; c < 4u; ++c) {
                                    if (!evalAxis(plan->cls.vs.oPos[c], vLo, oLo[c]) ||
                                        !evalAxis(plan->cls.vs.oPos[c], vHi, oHi[c])) {
                                        paramsOk = false;
                                        break;
                                    }
                                }
                                if (!paramsOk || std::fabs(oLo[3]) < 1e-6f || std::fabs(oLo[3] - oHi[3]) > 1e-5f) {
                                    paramsOk = false;
                                    break;
                                }
                                const float ndcX0 = oLo[0] / oLo[3], ndcX1 = oHi[0] / oHi[3];
                                const float ndcY0 = oLo[1] / oLo[3], ndcY1 = oHi[1] / oHi[3];
                                const float ndcZ = oLo[2] / oLo[3];

                                float sx0 = 0.0f, sx1 = 0.0f, sy0 = 0.0f, sy1 = 0.0f;
                                if (desc.guestVpValid) {
                                    sx0 = vpXo + ndcX0 * vpXs;
                                    sx1 = vpXo + ndcX1 * vpXs;
                                    sy0 = vpYo + ndcY0 * vpYs;
                                    sy1 = vpYo + ndcY1 * vpYs;
                                } else if (directPxSpace) {
                                    sx0 = ndcX0; sx1 = ndcX1;
                                    sy0 = ndcY0; sy1 = ndcY1;
                                } else {
                                    sx0 = (ndcX0 * 0.5f + 0.5f) * static_cast<float>(guestW);
                                    sx1 = (ndcX1 * 0.5f + 0.5f) * static_cast<float>(guestW);
                                    sy0 = (0.5f - ndcY0 * 0.5f) * static_cast<float>(guestH);
                                    sy1 = (0.5f - ndcY1 * 0.5f) * static_cast<float>(guestH);
                                }
                                const float fxMin = std::min(sx0, sx1), fxMax = std::max(sx0, sx1);
                                const float fyMin = std::min(sy0, sy1), fyMax = std::max(sy0, sy1);
                                const LONG ix0 = static_cast<LONG>(std::lround(fxMin));
                                const LONG ix1 = static_cast<LONG>(std::lround(fxMax));
                                const LONG iy0 = static_cast<LONG>(std::lround(fyMin));
                                const LONG iy1 = static_cast<LONG>(std::lround(fyMax));
                                if (std::fabs(fxMin - float(ix0)) > 1e-2f || std::fabs(fxMax - float(ix1)) > 1e-2f ||
                                    std::fabs(fyMin - float(iy0)) > 1e-2f || std::fabs(fyMax - float(iy1)) > 1e-2f) {
                                    paramsOk = false;
                                    break;
                                }
                                LONG cl = std::max<LONG>(0, ix0);
                                LONG ct = std::max<LONG>(0, iy0);
                                LONG cr = std::min<LONG>(static_cast<LONG>(guestW), ix1);
                                LONG cb = std::min<LONG>(static_cast<LONG>(guestH), iy1);
                                if (desc.guestVpValid) {
                                    cl = std::max<LONG>(cl, static_cast<LONG>(std::floor(desc.guestVpX)));
                                    ct = std::max<LONG>(ct, static_cast<LONG>(std::floor(desc.guestVpY)));
                                    cr = std::min<LONG>(cr, static_cast<LONG>(std::ceil(desc.guestVpX + desc.guestVpW)));
                                    cb = std::min<LONG>(cb, static_cast<LONG>(std::ceil(desc.guestVpY + desc.guestVpH)));
                                }
                                if (desc.winScissorValid) {
                                    cl = std::max<LONG>(cl, static_cast<LONG>(desc.winScissorL));
                                    ct = std::max<LONG>(ct, static_cast<LONG>(desc.winScissorT));
                                    cr = std::min<LONG>(cr, static_cast<LONG>(desc.winScissorR));
                                    cb = std::min<LONG>(cb, static_cast<LONG>(desc.winScissorB));
                                }
                                if (cl < cr && ct < cb && candCount < 8u) {
                                    candRects[candCount++] = {cl, ct, cr, cb};
                                }
                                if (wantDepthClear) {
                                    const float zd = (plan->cls.ps.psPresent && plan->cls.ps.writesDepth)
                                                         ? psFixedDepth
                                                         : std::clamp(ndcZ, 0.0f, 1.0f);
                                    if (!depthSet) {
                                        hostDepth = zd;
                                        depthSet = true;
                                    } else if (std::fabs(zd - hostDepth) > 1e-5f) {
                                        paramsOk = false;
                                        break;
                                    }
                                }
                            }
                            if (paramsOk && candCount > 0u) {
                                prep.foldedClearRect = true;
                                prep.clearFlags = (wantColorClear ? 1u : 0u) | (wantDepthClear ? 2u : 0u);
                                std::memcpy(prep.clearColor, clearColor, sizeof(clearColor));
                                prep.clearDepth = DepthCompareMirrored() ? (1.0f - hostDepth) : hostDepth;
                                prep.clearRectCount = candCount;
                                for (uint32_t ci = 0u; ci < candCount; ++ci) prep.clearRects[ci] = candRects[ci];
                                prep.clearVsHash = plan->vsHash;
                                prep.clearPsHash = plan->psHash;
                                meta.vertexCount = vertexCount;
                                prep.valid = true;
                                break;
                            }
                        }
                    }
                }
            }

            // Rect-list CPU expansion (prim=8, kAutoIndex): 3 guest verts per
            // rectangle -> 4 verts + 6 indices, drawn as an indexed
            // TRIANGLE_LIST through the existing dynamic-mesh path. The
            // reference expands in a geometry shader; this draw carries a
            // non-GS pipeline (see psi.primType below), so the expansion must
            // happen here or the draw would rasterise as one triangle (half
            // the quad) against a GS-less PSO (F-609). The math is the
            // parallelogram completion over the whole vertex (position and
            // every float interpolator with the same combination), and the
            // quad is (0,1,2)+(0,2,3) per rect. Restricted to the measured
            // class: non-indexed, num_indices a multiple of 3, float-only
            // vertex formats (dxgi 2/6/16/41/34/10). Anything else falls
            // through to the GS path unchanged (or its honest refusal).
            bool rectCpuExpanded = false;
            bool quadCpuExpanded = false;
            if (!indexed && dc.primitiveTopology == 8u && dc.numIndices >= 3u &&
                (dc.numIndices % 3u) == 0u && dc.vbStride > 0u &&
                dc.vbStride % 4u == 0u) {
                bool floatOnly = true;
                for (uint32_t fe = 0u; fe < dc.vbElementCount &&
                                      fe < mcla::native::kCpVertexElementMax; ++fe) {
                    const uint32_t dx = dc.vbElements[fe].fmt.dxgi;
                    if (dx != 2u && dx != 6u && dx != 16u && dx != 41u &&
                        dx != 34u && dx != 10u) {
                        floatOnly = false;
                        break;
                    }
                }
                if (dc.vbElementCount == 0u) floatOnly = false;
                const uint32_t rectCount = dc.numIndices / 3u;
                const uint64_t expVerts64 = static_cast<uint64_t>(rectCount) * 4ull;
                if (floatOnly && expVerts64 <= 65535ull &&
                    (static_cast<uint64_t>(dc.idxOffset) + static_cast<uint64_t>(dc.numIndices)) *
                            dc.vbStride <=
                        vb.size()) {
                    const uint32_t expVerts = static_cast<uint32_t>(expVerts64);
                    const uint32_t expIdx = rectCount * 6u;
                    static thread_local std::vector<uint8_t> rectVbScratch;
                    const size_t needVb = static_cast<size_t>(expVerts) * dc.vbStride;
                    if (rectVbScratch.size() < needVb) rectVbScratch.resize(needVb);
                    prep.ib.resize(static_cast<size_t>(expIdx) * 2u);
                    uint16_t* rectIbOut = reinterpret_cast<uint16_t*>(prep.ib.data());
                    const uint32_t lanes = dc.vbStride / 4u;
                    {   // TEMP-RECT-LAYOUT
                        static StatCtr<uint32_t> s_rl{0};
                        if (s_rl.fetch_add(1) < 12u) {
                            std::string es;
                            for (uint32_t fe = 0u; fe < dc.vbElementCount && fe < 8u; ++fe)
                                es += fmt::format(" [slot={} off={} dxgi={} dstReg={} end={}]", dc.vbElements[fe].slotIndex,
                                                  dc.vbElements[fe].offsetBytes, dc.vbElements[fe].fmt.dxgi, dc.vbElements[fe].dstReg,
                                                  dc.vbElements[fe].endianness);
                            std::string vs0;
                            for (uint32_t l = 0u; l < lanes && l < 12u; ++l) {
                                float f; std::memcpy(&f, vb.data() + l * 4u, 4);
                                vs0 += fmt::format(" {}", f);
                            }
                            MCLA_LOG_WARN("TEMP-RECT-LAYOUT n={} stride={} elems={}{} v0:{}", rectCount, dc.vbStride, dc.vbElementCount, es, vs0);
                        }
                    }
                    for (uint32_t q = 0u; q < rectCount; ++q) {
                        const uint8_t* v0 = vb.data() +
                            (static_cast<size_t>(dc.idxOffset) + static_cast<size_t>(q * 3u)) * dc.vbStride;
                        const uint8_t* v1 = v0 + dc.vbStride;
                        const uint8_t* v2 = v0 + 2u * dc.vbStride;
                        uint8_t* out0 = rectVbScratch.data() +
                            static_cast<size_t>(q * 4u) * dc.vbStride;
                        uint8_t* out1 = out0 + dc.vbStride;
                        uint8_t* out2 = out0 + 2u * dc.vbStride;
                        uint8_t* out3 = out0 + 3u * dc.vbStride;
                        std::memcpy(out0, v0, dc.vbStride);
                        std::memcpy(out1, v1, dc.vbStride);
                        std::memcpy(out2, v2, dc.vbStride);
                        // Xenia rule (d3d12 pipeline_cache.cc GS rect path): the vertex
                        // with the right angle (min |dot| of the two edges, measured on
                        // lanes 0,1 = position) is the pivot p; fourth = a + b - p.
                        // MEASURED ed16 composite: (-1,1),(1,1),(-1,-1) = pivot v0, so the
                        // old fixed v0+v2-v1 gave (-3,-1) -> the half-screen triangle.
                        auto lane = [&](const uint8_t* v, uint32_t l) {
                            float f = 0.0f;
                            std::memcpy(&f, v + static_cast<size_t>(l) * 4u, 4);
                            return f;
                        };
                        const uint8_t* vv[3] = {v0, v1, v2};
                        uint32_t piv = 0u;
                        float bestDot = 3.4e38f;
                        for (uint32_t pi = 0u; pi < 3u; ++pi) {
                            const uint8_t* pp = vv[pi];
                            const uint8_t* pa = vv[(pi + 1u) % 3u];
                            const uint8_t* pb = vv[(pi + 2u) % 3u];
                            const float d = std::fabs((lane(pa, 0) - lane(pp, 0)) * (lane(pb, 0) - lane(pp, 0)) +
                                                      (lane(pa, 1) - lane(pp, 1)) * (lane(pb, 1) - lane(pp, 1)));
                            if (d < bestDot) { bestDot = d; piv = pi; }
                        }
                        for (uint32_t l = 0u; l < lanes; ++l) {
                            const float f3 = lane(vv[(piv + 1u) % 3u], l) + lane(vv[(piv + 2u) % 3u], l) - lane(vv[piv], l);
                            std::memcpy(out3 + static_cast<size_t>(l) * 4u, &f3, 4);
                        }
                        const uint32_t ia = (piv + 1u) % 3u, ib = (piv + 2u) % 3u;
                        const float cr1 = (lane(v1, 0) - lane(v0, 0)) * (lane(v2, 1) - lane(v0, 1)) -
                                          (lane(v1, 1) - lane(v0, 1)) * (lane(v2, 0) - lane(v0, 0));
                        const float cr2 = (lane(vv[ib], 0) - lane(vv[ia], 0)) * (lane(out3, 1) - lane(vv[ia], 1)) -
                                          (lane(vv[ib], 1) - lane(vv[ia], 1)) * (lane(out3, 0) - lane(vv[ia], 0));
                        const bool flip2 = (cr1 < 0.0f) != (cr2 < 0.0f);
                        const uint16_t b = static_cast<uint16_t>(q * 4u);
                        rectIbOut[q * 6u + 0u] = static_cast<uint16_t>(b + 0u);
                        rectIbOut[q * 6u + 1u] = static_cast<uint16_t>(b + 1u);
                        rectIbOut[q * 6u + 2u] = static_cast<uint16_t>(b + 2u);
                        rectIbOut[q * 6u + 3u] = static_cast<uint16_t>(b + (flip2 ? ib : ia));
                        rectIbOut[q * 6u + 4u] = static_cast<uint16_t>(b + (flip2 ? ia : ib));
                        rectIbOut[q * 6u + 5u] = static_cast<uint16_t>(b + 3u);
                    }
                    vb.assign(rectVbScratch.data(), rectVbScratch.data() + needVb);
                    desc.vertexBytes = vb.data();
                    desc.vbDedupeKey = 0;
                    desc.vertexBytesSize = static_cast<uint32_t>(needVb);
                    desc.vertexCount = expVerts;
                    desc.indexed = true;
                    desc.baseVertexLocation = 0;
                    desc.indexBytes = prep.ib.data();
                    desc.indexBytesSize = expIdx * 2u;
                    desc.indexFormat = DXGI_FORMAT_R16_UINT;
                    desc.indexCount = expIdx;
                    rectCpuExpanded = true;
                    s_rectListExpanded.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Quad-list CPU expansion (prim=13, indexed): 4 indices per quad ->
            // (0,1,2),(0,2,3) indexed triangles, drawn as TRIANGLE_LIST. The
            // reference expands in a geometry shader; this draw carries a non-GS
            // pipeline here, so expand on the host (same as DrawIndexed path
            // lines 1610-1643). numIdx=32 = 8 quads in the measured logo draws.
            // FLOOR expansion: tc.count need not be a multiple of 4. Expand
            // floor(tc.count/4) quads and count the %4 remainder once as
            // prim-quad-remainder; the draw still proceeds with the floor
            // portion instead of refusing the whole draw.
            if (indexed && dc.primitiveTopology == 13u && tc.count >= 4u) {
                // ib-maxindex: indices must address the vertex buffer.
                // vertexCount is vbSize/vbStride for indexed draws. Refuse
                // the whole draw if any index is out of range.
                if (tc.maxIndex >= vertexCount) {
                    refuse("ib-maxindex", s_cpRefuseIbMaxIndex,
                           "- indexed-quad: maxIndex >= vertexCount, index would "
                             "read past the vertex buffer. NOT drawn.");
                    break;
                }
                const uint32_t quadCount = tc.count / 4u;
                const uint32_t remainder = tc.count % 4u;
                if (remainder != 0u) {
                    // ONE counted refuse row for the remainder; does NOT
                    // refuse the draw. Mirrors refuse() logging but leaves
                    // s_1sSkipped untouched so TEMP-RENDER-1S skip stays
                    // honest (the floor quads still issue).
                    const uint32_t rk =
                        s_cpRefusePrimQuadRemainder.fetch_add(1, std::memory_order_relaxed) + 1u;
                    if (rk <= 8u || (rk % 200u) == 0u) {
                        MCLA_LOG_ERROR("TEMP-DRAW-REFUSE #{} what={} draw={} prim={} ib={:08X}+{}B "
                                       "numIdx={} vb={:08X}+{}B stride={} {}",
                                       rk, "prim-quad-remainder", n, dc.primitiveTopology, dc.ibBase, dc.ibSize, tc.count,
                                       dc.vbBase, dc.vbSize, dc.vbStride,
                                       "- indexed-quad remainder: count % 4 != 0; floor(count/4)"
                                       " quads expanded, remainder indices not drawn.");
                        MCLA_LOG_WARN("DRAW-CP-REFUSE #{} what={} draw={} ib={:08X}+{}B "
                                      "numIdx={} vb={:08X}+{}B stride={} {}",
                                      rk, "prim-quad-remainder", n, dc.ibBase, dc.ibSize, tc.count,
                                      dc.vbBase, dc.vbSize, dc.vbStride,
                                      "- indexed-quad remainder: count % 4 != 0; floor(count/4)"
                                      " quads expanded, remainder indices not drawn.");
                    }
                }
                const uint32_t expIdx = quadCount * 6u;
                // Keep narrow/wide safe above 65535 verts: the wide path stays
                // wide (R32). If the narrow guest stream would need to address
                // >65535 verts, promote to the wide buffer instead of wrapping.
                const bool needWide = tc.isWide || (vertexCount > 65535u) || (tc.maxIndex > 65535u);
                if (needWide) {
                    prep.ib.resize(static_cast<size_t>(expIdx) * 4u);
                    uint32_t* wideOut = reinterpret_cast<uint32_t*>(prep.ib.data());
                    for (uint32_t q = 0u; q < quadCount; ++q) {
                        uint32_t i0, i1, i2, i3;
                        if (tc.isWide) {
                            i0 = tc.wide[q * 4u + 0u];
                            i1 = tc.wide[q * 4u + 1u];
                            i2 = tc.wide[q * 4u + 2u];
                            i3 = tc.wide[q * 4u + 3u];
                        } else {
                            i0 = tc.narrow[q * 4u + 0u];
                            i1 = tc.narrow[q * 4u + 1u];
                            i2 = tc.narrow[q * 4u + 2u];
                            i3 = tc.narrow[q * 4u + 3u];
                        }
                        wideOut[q * 6u + 0u] = i0;
                        wideOut[q * 6u + 1u] = i1;
                        wideOut[q * 6u + 2u] = i2;
                        wideOut[q * 6u + 3u] = i0;
                        wideOut[q * 6u + 4u] = i2;
                        wideOut[q * 6u + 5u] = i3;
                    }
                    desc.indexBytes = prep.ib.data();
                    desc.indexBytesSize = expIdx * 4u;
                    desc.indexFormat = DXGI_FORMAT_R32_UINT;
                } else {
                    prep.ib.resize(static_cast<size_t>(expIdx) * 2u);
                    uint16_t* narrowOut = reinterpret_cast<uint16_t*>(prep.ib.data());
                    for (uint32_t q = 0u; q < quadCount; ++q) {
                        const uint16_t i0 = tc.narrow[q * 4u + 0u];
                        const uint16_t i1 = tc.narrow[q * 4u + 1u];
                        const uint16_t i2 = tc.narrow[q * 4u + 2u];
                        const uint16_t i3 = tc.narrow[q * 4u + 3u];
                        narrowOut[q * 6u + 0u] = i0;
                        narrowOut[q * 6u + 1u] = i1;
                        narrowOut[q * 6u + 2u] = i2;
                        narrowOut[q * 6u + 3u] = i0;
                        narrowOut[q * 6u + 4u] = i2;
                        narrowOut[q * 6u + 5u] = i3;
                    }
                    desc.indexBytes = prep.ib.data();
                    desc.indexBytesSize = expIdx * 2u;
                    desc.indexFormat = DXGI_FORMAT_R16_UINT;
                }
                desc.indexCount = expIdx;
                quadCpuExpanded = true;
            }
            if (indexed && dc.primitiveTopology == 6u && tc.count >= 3u) {
                // Indexed triangle strip: the host PSO has no strip-cut value, so the guest's primitive-restart
                // index (0xFFFF / 0xFFFFFFFF; Xenia sets IBStripCutValue, d3d12/pipeline_cache.cc:1364-1372) was
                // drawn as a real vertex that fetches zeros -> triangles pulled to one point. Expand to a list,
                // restarting at the reset index, with the strip's alternating winding.
                static thread_local std::vector<uint32_t> stripIb;
                stripIb.clear();
                const uint32_t reset = tc.isWide ? 0xFFFFFFFFu : 0xFFFFu;
                uint32_t a = 0, b = 0, sN = 0;
                for (uint32_t i = 0; i < tc.count; ++i) {
                    const uint32_t v = tc.isWide ? tc.wide[i] : tc.narrow[i];
                    if (v == reset) { sN = 0; continue; }
                    if (sN >= 2) {
                        if (sN & 1u) { stripIb.push_back(b); stripIb.push_back(a); }
                        else         { stripIb.push_back(a); stripIb.push_back(b); }
                        stripIb.push_back(v);
                    }
                    a = b; b = v; ++sN;
                }
                const uint32_t stripBytes = static_cast<uint32_t>(stripIb.size() * 4u);
                prep.ib.resize(stripBytes);
                if (stripBytes > 0u) std::memcpy(prep.ib.data(), stripIb.data(), stripBytes);
                desc.indexBytes = prep.ib.empty() ? nullptr : prep.ib.data();
                desc.indexBytesSize = stripBytes;
                desc.indexFormat = DXGI_FORMAT_R32_UINT;
                desc.indexCount = static_cast<uint32_t>(stripIb.size());
                quadCpuExpanded = true;  // draw as the triangle list it now is
            }
            if (!indexed && dc.primitiveTopology == 13u && dc.numIndices >= 4u &&
                (dc.numIndices % 4u) == 0u && dc.vbStride > 0u) {
                // kAutoIndex quad list: every 4 sequential vertices form one
                // quad (0,1,2)(0,2,3), no index bytes on the wire.
                const uint32_t quadCount = dc.numIndices / 4u;
                const uint32_t expIdx = quadCount * 6u;
                // Under XE_RAWVB the VS indexes g_rawVb with SV_VertexID, which
                // D3D12 does NOT offset by BaseVertexLocation: bake dc.idxOffset
                // into prep.ib and clear baseVertexLocation instead. On the IA
                // path BaseVertexLocation shifts every vertex fetch.
                const uint32_t baseIdx = rawVbDraw ? dc.idxOffset : 0u;
                const uint64_t maxIdx = static_cast<uint64_t>(baseIdx) +
                                        static_cast<uint64_t>(dc.numIndices) - 1ull;
                if (maxIdx > 65535ull) {
                    prep.ib.resize(static_cast<size_t>(expIdx) * 4u);
                    uint32_t* autoOut = reinterpret_cast<uint32_t*>(prep.ib.data());
                    for (uint32_t q = 0u; q < quadCount; ++q) {
                        const uint32_t b = baseIdx + q * 4u;
                        autoOut[q * 6u + 0u] = b + 0u;
                        autoOut[q * 6u + 1u] = b + 1u;
                        autoOut[q * 6u + 2u] = b + 2u;
                        autoOut[q * 6u + 3u] = b + 0u;
                        autoOut[q * 6u + 4u] = b + 2u;
                        autoOut[q * 6u + 5u] = b + 3u;
                    }
                    desc.indexBytesSize = expIdx * 4u;
                    desc.indexFormat = DXGI_FORMAT_R32_UINT;
                } else {
                    prep.ib.resize(static_cast<size_t>(expIdx) * 2u);
                    uint16_t* autoOut = reinterpret_cast<uint16_t*>(prep.ib.data());
                    for (uint32_t q = 0u; q < quadCount; ++q) {
                        const uint16_t b = static_cast<uint16_t>(baseIdx + q * 4u);
                        autoOut[q * 6u + 0u] = static_cast<uint16_t>(b + 0u);
                        autoOut[q * 6u + 1u] = static_cast<uint16_t>(b + 1u);
                        autoOut[q * 6u + 2u] = static_cast<uint16_t>(b + 2u);
                        autoOut[q * 6u + 3u] = static_cast<uint16_t>(b + 0u);
                        autoOut[q * 6u + 4u] = static_cast<uint16_t>(b + 2u);
                        autoOut[q * 6u + 5u] = static_cast<uint16_t>(b + 3u);
                    }
                    desc.indexBytesSize = expIdx * 2u;
                    desc.indexFormat = DXGI_FORMAT_R16_UINT;
                }
                if (rawVbDraw) {
                    desc.baseVertexLocation = 0;
                }
                desc.indexed = true;
                desc.indexBytes = prep.ib.data();
                desc.indexCount = expIdx;
                quadCpuExpanded = true;
            }
            // F-647: hand the backend the guest's real constant bank. A refusal
            // here is NOT fatal to the draw: the descriptor keeps its
            // zero-initialised nullptr/0 and the backend reports `src=ZEROS` on
            // its own CONST-BANK row, which is the honest state. The draw still
            // issues, because refusing the whole draw over a constant bank would
            // hide the pixels the index buffer already proved are real.
            ensureConstBanks();

            // F-899 (2026-10-27): carry the guest's texture descriptors through
            // to the backend. The descriptors are snapshotted at enqueue time
            // (CpBridgeDraw, CP-TEXDESC-CAPTURE); the payload (untiled guest
            // pixels) is not yet produced here, so guestTextures entries carry
            // null mipPayloads[0] and BindGuestTextures refuses them honestly
            // ("no untiled payload from the producer") -- the same null SRV the
            // heap already held, but now the capture census (CP-TEXDESC-CAPTURE)
            // and the bind-refusal census both move, so the two halves of the gap
            // are independently observable. The untile + guest-memory read lands
            // once the memory-location report confirms where the tiled data lives.
            if (!guestTextures.empty()) {
                desc.guestTextures = guestTextures.data();
                desc.guestTextureCount = static_cast<uint32_t>(guestTextures.size());
                if (s_guestSamplers) {  // G0 (static lives in the same scope as guestTextures)
                    desc.hasGuestSamplerTable = true;
                    desc.guestSamplerMask = guestSamplerMask;
                    std::memcpy(desc.guestSamplerTable, guestSamplerTab, sizeof(guestSamplerTab));
                    for (const auto& gt : guestTextures) {
                        if (gt.mipPayloads[0] != nullptr || gt.contentId != 0) {
                            desc.activeSamplerMask |= (1u << gt.descriptorIndex);
                        }
                    }
                } else {
                    // PATCH_GFX_TFETCH_FILTER T1 (fallback when MCLA_TEMP_GUEST_SAMPLER is off).
                    // One sampler per draw (s0): take the first bound descriptor's fetch-constant sampler.
                    mcla::native::GuestTextureDesc s0Desc{};
                    if (mcla::native::DecodeGuestTextureDescriptor(guestTextures[0].descriptorBytes, s0Desc) && s0Desc.valid) {
                        desc.guestSampler = mcla::native::DecodeGuestSamplerState(s0Desc);
                        desc.hasGuestSampler = true;
                        static StatCtr<uint32_t> s_tfs{0};
                        const uint32_t tfn = s_tfs.fetch_add(1, std::memory_order_relaxed) + 1u;
                        if (tfn <= 8u || (tfn % 500u) == 0u)
                            MCLA_LOG_INFO("TFETCH-SAMPLER-WIRED #{} filter=0x{:X} addr={}/{}/{} aniso={} minLod={:.4g} maxLod={:.4g} mipBias={:.4g}",
                                          tfn, desc.guestSampler.filter, desc.guestSampler.addressU, desc.guestSampler.addressV,
                                          desc.guestSampler.addressW, desc.guestSampler.maxAnisotropy, desc.guestSampler.minLod,
                                          desc.guestSampler.maxLod, desc.guestSampler.mipLodBias);
                    }
                }
            }

            // --- step 4: the vertex INPUT LAYOUT, on the LIVE path ------------
            //
            // WHAT THIS IS. The DRAW_INDEXED case above runs a complete,
            // fully-built translator sequence (ReadShaderContainer ->
            // TranslateShader -> ParseShaderProgram ->
            // BuildInputLayoutFromGrcFvf -> ComputePipelineKey ->
            // cache.GetOrCompile, :767-892) and it has never executed once: in
            // build/w682.log (9,724,301 B) the literal "RenderThread:
            // DRAW_INDEXED" counts 0, and so do "PSO HIT", "PSO MISS" and
            // "shader container read failed". The LIVE case is DRAW_CAPTURED
            // (:1166), and on it the only input-layout builder in the tree -
            // D3D12Backend::BuildInputLayoutFromGrcFvf (d3d12_backend.h:366),
            // whose only caller in the whole tree is :804, inside that dead
            // path - was never called either, so its two markers
            // ("BuildInputLayoutFromGrcFvf refused #{}" /
            // "BuildInputLayoutFromGrcFvf: {} elements from mask=",
            // d3d12_backend.cpp:4005/4012) count 0 in the same log.
            //
            // WHAT IS ACTUALLY AVAILABLE TO BUILD ONE. Measured, not assumed:
            // the authoritative builder takes a mcla::native::GrcFvfDesc - a
            // RAGE grcFvf declaration (trace_types.h:36-63: fvfMask, fvfSize,
            // flags, dynamicOrder, channelCount, 16 x 4-bit "types"), which
            // mcla::native::DecodeGrcFvf turns into lanes and
            // renderer::BuildGrcFvfInputLayout turns into
            // D3D12_INPUT_ELEMENT_DESC with ascending byteOffset
            // (pipeline_cache.cpp:1242-1280). THE ONLY PRODUCER OF A grcFvf
            // ANYWHERE IN THIS TREE is capture_hooks.cpp:538-591, and it fills
            // a native::DrawPacket's grcFvf/hasGrcFvf, never a
            // DrawCapturedCommand. A grep for fvfMask/fvfTypes/fvfChannelCount
            // across src/ returns exactly that one producer plus the consumers,
            // so the declaration is not missing a decode on this path - it does
            // not exist here, and nothing in this file can conjure it.
            //
            // WHY IT IS NOT SYNTHESISED FROM dc.vbFormat. The command DOES
            // carry one fully-specified vertex element (render_queue.h:131-136,
            // F-648: xenos/dxgi/bytes/known; measured in w682.log as
            // "vf=57 dxgi=6 fmtB=12 stride=28 nDesc=2 nFull=1" on four draws
            // and "vf=37 dxgi=16 fmtB=8 stride=8 nDesc=1 nFull=1" on two).
            // That is a STREAM element, not a LAYOUT element: a
            // D3D12_INPUT_ELEMENT_DESC additionally needs an AlignedByteOffset
            // and a SemanticName, and BOTH ARE ABSENT HERE:
            //   * the byte offset is decoded by the producer - it is on the
            //     bridge line as "vbOff=" and it is d0.offsetBytes (gpu_cp.cpp
            //     :1001, printed at :3661, measured 0 on all six) - but
            //     DrawCapturedCommand has no field for it (render_queue.h:
            //     232-288 carries vbBase/vbSize/vbStride/vbFormat/vbDescCount/
            //     vbFullCount and no offset), so a literal 0 here would be a
            //     zero standing in for a fact, which is the exact defect F-648
            //     documents for dxgi == 0 and refuses rather than defaults.
            //   * the SEMANTIC does not exist anywhere on this path and cannot
            //     be derived from what is carried. It is not a property of the
            //     format: the grcFvf lane number IS the semantic
            //     (grc_fvf_decode.cpp:49-63 - lane 0 = POSITION, 6-13 =
            //     TexCoord0-7), and gpu_cp.cpp does not decode the instruction's
            //     dst_reg at all. CpVertexFetchInstr (gpu_cp.cpp:981-993)
            //     carries execBlock/execIndex/slotIndex/opcode/formatXenos/
            //     strideDwords/offsetDwords/destSwizzle/isMiniFetch/
            //     isPredicated/conditional and no destination register, and
            //     gpu_cp.cpp:1352 states the omission as deliberate ("the
            //     vfetch's own dst_reg/dest() and the per-component
            //     num_format_all/signed_rf_mode_all decodes are not folded
            //     into the descriptor - this describes the STREAM, and the
            //     attribute-level decode is the renderer's job").
            //     native::VertexFormatDesc (renderer/vertex_decode.h:36-53),
            //     the module that comment names, carries no semantic either.
            //     The one source of a semantic in this tree is
            //     ReferencedVertexInputs(vsProg) (:813, dead path) - i.e. the
            //     guest's own vertex shader - which is the step-5 chain.
            // So a layout built here would have to invent a lane number AND a
            // 4-bit grcFvf type code from a 6-bit xenos::VertexFormat, and
            // those are two different enumerations from two different APIs
            // (xenos.h:602-619 vs grc_fvf_decode.h:44-57). They agree in size
            // for the measured formats - dxgi 6 is 12 B, dxgi 2 is 16 B, dxgi 16
            // is 8 B - and that agreement is a COINCIDENCE of two encodings,
            // not a documented correspondence: nothing in this repo states it,
            // so acting on it is inference where the charter demands a
            // measurement (F-609, Golden Rule 5). It would also be a
            // plausible-but-wrong vertex, the failure mode this gate exists to
            // stop.
            //
            // SO: the layout is attempted, the absence is REFUSED BY NAME, and
            // the refusal is countable. inputLayout and inputDeclHash are
            // declared HERE, in the same scope as desc and the F-652
            // derivation below, so the step-5 consumer that replaces
            // no-translated-pso reads them with no restructuring of this case.
            // The draw's fate is UNCHANGED: this adds no refuse() call and bumps
            // no existing counter, so DRAW-CP-REFUSE stays at its measured 12
            // and DRAW-CP-DRAWN stays 0.
            std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout = prep.inputLayout;
            inputLayout.clear();
            // The SAME layout in the numbering TranslateShaderPinned pins against
            // (shader_translator.h:72-75: usage/usageIndex are the DeclUsage
            // numbering VertexUsageSemanticName and grc_fvf_decode's lane table
            // share, so a semantic computed here and a lane decoded there name the
            // same thing). It is carried beside the D3D12 array rather than
            // recovered from it, because SemanticName is a STRING in the D3D12
            // form and re-deriving the number from it would be a guess.
            static thread_local std::vector<mcla::renderer::VertexInputSemantic> pinnedLayout;
            pinnedLayout.clear();
            uint64_t inputDeclHash = 0;
            const char *layoutRefusal = "none";

            // F-716: the pipeline state this chain produces, declared HERE so the
            // F-652 derivation below (and the topology gate and the draw after it)
            // read it with no restructuring. `pipeStop` is the ONE word that says
            // how far this draw got; "none" means it may draw.
            Microsoft::WRL::ComPtr<ID3D12PipelineState> pipePso;
            bool pipeHasGeomStage = false;   // CARRIED, never queried (pipeline_cache.h)
            const char *pipeStop = "none";
            uint32_t pipeVsCodeBytes = 0, pipePsCodeBytes = 0;
            uint32_t pipeVsElements = 0;     // the VS's OWN vertex-element table size
            uint32_t pipeVsInstructions = 0, pipeVsInterpolants = 0;
            uint64_t pipeVsHash = 0, pipePsHash = 0;
            bool pipeVsPrecompiledAvail = false, pipePsPrecompiledAvail = false;
            uint64_t pipeVsPrecompiledHash = 0, pipePsPrecompiledHash = 0;
            const char *pipeVsSrc = "absent", *pipePsSrc = "absent";
            static const std::string kEmptyHlsl;
            std::shared_ptr<const mcla::renderer::TranslatedShader> pipeVsHold;
            std::shared_ptr<const mcla::renderer::TranslatedShader> pipePsHold;
            std::shared_ptr<const std::string> pipeVsModHold;
            std::shared_ptr<const std::string> pipePsModHold;
            const std::string *pipeVsHlsl = &kEmptyHlsl;
            const std::string *pipePsHlsl = &kEmptyHlsl;
            uint64_t pipeVsHlslHash = 0, pipePsHlslHash = 0;  // MCLA_PSO_HLSLHASH: 0 = hash the string at derive-pso
            std::string pipeTranslateError;
            // Built once per draw by the layout gate below; GATE B reuses them (INV_PERF2 PATCH 3).
            static thread_local std::vector<uint8_t> vsCode, psCode, vsEnv, psEnv;
            const uint8_t* vsEnvPtr = nullptr; size_t vsEnvBytes = 0;
            const uint8_t* psEnvPtr = nullptr; size_t psEnvBytes = 0;
            static thread_local std::vector<uint8_t> tl_vsCodeDump;  // TEMP-REPLAY raw microcode copy
            // P5: envelope-key memo state, fresh (invalid) every draw. s_vpCacheGen is
            // bumped whenever the parsed-VS cache is cleared, so a recycled program
            // pointer can never satisfy the memo key across a clear.
            static thread_local uint32_t s_vpCacheGen = 0;
            const void* memoProgPtr = nullptr;
            const void* envMemoCurPtr = nullptr;  // outer-scope mirror of vsHold.get() for the consume-site key check
            uint32_t memoGen = 0;
            uint64_t memoVsEnvKeyHash = 0, memoPsEnvKeyHash = 0;
            bool memoVsValid = false, memoPsValid = false;
            vsCode.clear(); psCode.clear(); vsEnv.clear(); psEnv.clear();
            auto findInputSlot = [&](uint32_t fetchSlotIdx) -> UINT {
                if (streamCount <= 1u) return 0u;
                for (uint32_t s = 0u; s < streamCount; ++s) {
                    if (streams[s].slotIndex == fetchSlotIdx) return static_cast<UINT>(s);
                }
                return 0u;
            };
            // WHICH STAGE the refusal string below came from. Set immediately
            // before each TranslateShaderPinned call, so a non-empty
            // pipeTranslateError is attributed to the stage that produced it
            // rather than to "something". "none" until the first attempt.
            const char *pipeTrStage = "none";
            // 2026-10-02: the GUEST's own render-target format, hoisted out of
            // the derivation scope (where `psi` is local, render_thread.cpp
            // :2884) so the draw site can report it. These two are the missing
            // half of the only comparison that decides whether the draw can
            // rasterize at all: the PSO's RTVFormats[0] and
            // NumRenderTargets are computed from this value
            // (renderer/pipeline_cache.cpp:1921-1945), while the render target
            // the draw path actually binds is the swapchain back buffer,
            // DXGI_FORMAT_R8G8B8A8_UNORM (src/d3d12_backend.cpp:711, :763).
            // MapXenosColorFormatToDxgi (:456-501) returns R8G8B8A8_UNORM for
            // guest codes 0 and 1 ONLY; every other assigned code -- 2,3,4,5,6,
            // 7,10,12,14,15 -- maps to a different DXGI format, and a D3D12 PSO
            // whose RTVFormats[0] disagrees with the bound RTV rasterizes
            // nothing and reports no error. Nobody could check that before,
            // because the code was consumed inside the derivation and never
            // printed.
            // 0xFFFFFFFF = RB_COLOR_INFO was never presented; 0 = k_8_8_8_8,
            // which is a legal and common render target and NOT an absence.
            uint32_t pipeColorFormatCode = 0xFFFFFFFFu;
            uint32_t pipeColorFormatDxgi = 0u;
            uint32_t pipeStencilRef = 0u;
            bool pipeStencilOn = false;
            // The register-present mask, hoisted for the same reason: `rs` is
            // local to the derivation block (:2905) and the draw site is
            // outside it. Bit 8 is CpSnapshotReg::kCpSnapColorInfo, i.e. this
            // mask is what says whether RB_COLOR_INFO was presented at all --
            // which is a DIFFERENT question from what its colour_format was
            // (code 0 is present and legal, so "non-zero" is not a presence
            // test; see the note at the colour block).
            uint32_t pipeRsPresent = 0u;
            {
                STALL_STAGE("layout");
                // THE FILL SITE, first source: a grcFvf DECLARATION carried on the
                // command. It is still not carried -- DrawCapturedCommand has no
                // such member (render_queue.h:232-288) and capture_hooks.cpp:538-
                // 591 only fills a native::DrawPacket -- so this arm is dead and
                // is kept as the PREFERRED source, not deleted: when the
                // declaration lands, this block starts producing elements with no
                // edit to the builder, the marker or the counters. The guard is
                // written against the DECLARATION rather than omitted so the shape
                // of that fix stays visible in one place.
                mcla::native::GrcFvfDesc liveDecl{};
                if (liveDecl.fvfMask != 0u && liveDecl.fvfSize != 0u) {
                    inputLayout = backend_.BuildInputLayoutFromGrcFvf(liveDecl);
                    if (inputLayout.empty()) {
                        layoutRefusal = "decl-refused";
                        s_layoutDeclRefused.fetch_add(1,
                                                      std::memory_order_relaxed);
                    } else {
                        inputDeclHash = mcla::native::HashGrcFvfDeclaration(
                            mcla::native::DecodeGrcFvf(liveDecl));
                        s_layoutBuilt.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    layoutRefusal = "no-decl-carried";
                    s_layoutNoDecl.fetch_add(1, std::memory_order_relaxed);

                    // F-716, SECOND SOURCE: the guest's OWN vertex shader. This is
                    // the step-5 chain the note above names, and it is the ONLY
                    // source of a semantic anywhere in this tree. It is attempted
                    // for real now, and it is REFUSED by the measurement rather
                    // than by an assumption:
                    //
                    //   1. recover each stage's own code bytes (ResolveStageCode),
                    //      which is identity -- the bytes are on the command;
                    //   2. wrap them in the host-side envelope and PARSE it, so the
                    //      program's decoded form is measured, not assumed;
                    //   3. read `vertexElements` OUT OF THE PARSE. That vector is
                    //      the only place a semantic can come from
                    //      (ReferencedVertexInputs matches against it,
                    //      shader_translator.cpp:68-74). If it is EMPTY then every
                    //      semantic that function would hand back is its POSITION
                    //      fallback, and binding that is exactly the plausible-wrong
                    //      vertex this gate exists to stop, so the draw refuses.
                    //
                    // The step that CANNOT be done here, and is the blocker: the
                    // reference does not get vertex elements from the program
                    // either -- they are the vertex-FETCH hardware's -- and its
                    // interpolant set comes from the register file
                    // (d3d12/pipeline_cache.cc:882, :915). An embedded program
                    // therefore carries no element table at all, and this project
                    // captures neither the vfetch dst_reg (gpu_cp.cpp:1352 states
                    // the omission as deliberate) nor SQ_INTERPOLATOR_CNTL.
                    // R6: on a parsed-VS cache hit the code bytes are never needed (the cache holds the
                    // envelopes), so only sizes are resolved up front. MCLA_NO_LAYOUT_FASTHIT=1 restores the old copies.
                    static const bool s_vpFast = [] { const char* e = std::getenv("MCLA_NO_LAYOUT_FASTHIT"); return !(e && e[0] == '1'); }();
                    if (s_vpFast) {
                        pipeVsCodeBytes = static_cast<uint32_t>(StageCodeBytes(dc.vsProgram, pipeVsSrc));
                        pipePsCodeBytes = static_cast<uint32_t>(StageCodeBytes(dc.psProgram, pipePsSrc));
                    } else {
                    pipeVsCodeBytes = static_cast<uint32_t>(
                        ResolveStageCode(dc.vsProgram, vsCode, pipeVsSrc));
                    pipePsCodeBytes = static_cast<uint32_t>(
                        ResolveStageCode(dc.psProgram, psCode, pipePsSrc));
                    }
                    if (pipeVsCodeBytes != 0u) {
                        s_pipeVsCode.fetch_add(1, std::memory_order_relaxed);
                        // TEMP-REPLAY: capture the raw microcode here, where ResolveStageCode
                        // has just filled it (the later dump point sees a cleared buffer).
                        static const bool s_vsDump = [] {
                            const char* e = std::getenv("MCLA_TEMP_DUMP_VSALL");
                            return e && e[0] == '1';
                        }();
                        if (s_vsDump) {
                            // With s_vpFast the envelope path never fills vsCode;
                            // force the raw microcode read into the dump buffer.
                            const char* dumpSrc = "dump";
                            (void)ResolveStageCode(dc.vsProgram, tl_vsCodeDump, dumpSrc);
                        }
                    }
                    if (pipePsCodeBytes != 0u)
                        s_pipePsCode.fetch_add(1, std::memory_order_relaxed);
                    // Parsed-VS cache: the layout derivation only reads the parsed program, and the same VS/PS pair repeats
                    // for thousands of draws. Keyed on the exact code bytes (content-compared, so never stale).
                    struct VpEnt { std::vector<uint8_t> vs, ps; uint32_t psPresent = 0; std::shared_ptr<const mcla::renderer::ShaderProgram> prog;
                                   std::vector<uint32_t> vsW, psW; std::vector<uint8_t> vsEnvC, psEnvC;  // R6: snapshot dwords + envelopes
                                   std::vector<mcla::renderer::VertexInputRef> vsRefs;
                                   uint64_t vsRefsDeclHash = 0;
                                   // P5: hash of the exact bytes above. progPtr+gen are the memo key; the
                                   // hash lives with the bytes, so no cleared entry can supply a stale hash.
                                   const void* progPtr = nullptr;
                                   uint32_t gen = 0;
                                   uint64_t vsEnvKeyHash = 0, psEnvKeyHash = 0;
                                   bool envKeyValid = false; };
                    static thread_local std::unordered_map<uint64_t, VpEnt> s_vpCache;
                    uint64_t vpKey = 0;
                    std::shared_ptr<const mcla::renderer::ShaderProgram> vsHold;
                    const std::vector<mcla::renderer::VertexInputRef>* vsRefsPtr = nullptr;
                    uint64_t vsRefsDeclHash = 0;
                    const size_t vsN = pipeVsCodeBytes / 4u, psN = pipePsCodeBytes / 4u;  // R6 (fast mode): dwords, no byte copy
                    if (pipeVsCodeBytes != 0u) {
                        if (s_vpFast) {
                            vpKey = HashBlock8(HashBlock8(0xcbf29ce484222325ull ^ dc.psProgram.present,
                                reinterpret_cast<const uint8_t*>(dc.vsProgram.body), vsN * 4u),
                                reinterpret_cast<const uint8_t*>(dc.psProgram.body), psN * 4u);
                            const auto vit = s_vpCache.find(vpKey);
                            if (vit != s_vpCache.end() && vit->second.psPresent == dc.psProgram.present &&
                                vit->second.vsW.size() == vsN && vit->second.psW.size() == psN &&
                                std::memcmp(vit->second.vsW.data(), dc.vsProgram.body, vsN * 4u) == 0 &&
                                std::memcmp(vit->second.psW.data(), dc.psProgram.body, psN * 4u) == 0) {
                                vsHold = vit->second.prog;
                                vsEnvPtr = vit->second.vsEnvC.data();
                                vsEnvBytes = vit->second.vsEnvC.size();
                                psEnvPtr = vit->second.psEnvC.data();
                                psEnvBytes = vit->second.psEnvC.size();
                                vsRefsPtr = &vit->second.vsRefs;
                                vsRefsDeclHash = vit->second.vsRefsDeclHash;
                                if (s_envMemoOn && vit->second.envKeyValid) {
                                    envMemoCurPtr = vsHold.get();
                                    memoProgPtr = vit->second.progPtr;
                                    memoGen = vit->second.gen;
                                    memoVsEnvKeyHash = vit->second.vsEnvKeyHash;
                                    memoPsEnvKeyHash = vit->second.psEnvKeyHash;
                                    memoVsValid = true;
                                    memoPsValid = (psEnvBytes != 0u);
                                }
                            } else {
                                (void)ResolveStageCode(dc.vsProgram, vsCode, pipeVsSrc);
                                (void)ResolveStageCode(dc.psProgram, psCode, pipePsSrc);
                            }
                        } else {
                        vpKey = HashBlock8(HashBlock8(0xcbf29ce484222325ull ^ dc.psProgram.present, vsCode.data(), vsCode.size()), psCode.data(), psCode.size());
                        const auto vit = s_vpCache.find(vpKey);
                        if (vit != s_vpCache.end() && vit->second.psPresent == dc.psProgram.present && vit->second.vs == vsCode && vit->second.ps == psCode) {
                            vsHold = vit->second.prog;
                            vsRefsPtr = &vit->second.vsRefs;
                            vsRefsDeclHash = vit->second.vsRefsDeclHash;
                        }
                        }
                    }
                    if (vsHold || (pipeVsCodeBytes != 0u &&
                        BuildProgramEnvelope(vsCode.data(), vsCode.size(), true, vsEnv))) {
                        s_pipeEnvelopeBuilt.fetch_add(1, std::memory_order_relaxed);
                        // depth-only draw (no PS program): nothing to wrap, the layout is the VS's alone
                        if (vsHold || dc.psProgram.present == 0u ||
                            BuildProgramEnvelope(psCode.data(), psCode.size(), false, psEnv)) {
                            mcla::renderer::ShaderProgram vsProgOwn;
                            const bool vpOk = vsHold ? true
                                : mcla::renderer::ParseShaderProgram(vsEnv.data(), vsEnv.size(), vsProgOwn);
                            if (vpOk && !vsHold) {
                                if (s_vpCache.size() > 4096) { s_vpCache.clear(); ++s_vpCacheGen; }
                                VpEnt& ent = s_vpCache[vpKey];
                                ent.psPresent = dc.psProgram.present;
                                if (!s_vpFast) { ent.vs = vsCode; ent.ps = psCode; }
                                if (s_vpFast) {
                                    ent.vsW.assign(dc.vsProgram.body, dc.vsProgram.body + vsN);
                                    ent.psW.assign(dc.psProgram.body, dc.psProgram.body + psN);
                                    ent.vsEnvC = vsEnv; ent.psEnvC = psEnv;
                                }
                                ent.prog = std::make_shared<const mcla::renderer::ShaderProgram>(std::move(vsProgOwn));
                                vsHold = ent.prog;
                                ent.vsRefs = mcla::renderer::ReferencedVertexInputs(*ent.prog);
                                {
                                    static thread_local std::vector<uint8_t> tl_vfCodes;
                                    tl_vfCodes.clear();
                                    tl_vfCodes.reserve(ent.vsRefs.size() * 4u);
                                    for (const auto& r2 : ent.vsRefs) {
                                        const uint32_t be = r2.vertexFormat;
                                        tl_vfCodes.push_back(static_cast<uint8_t>(be >> 24));
                                        tl_vfCodes.push_back(static_cast<uint8_t>(be >> 16));
                                        tl_vfCodes.push_back(static_cast<uint8_t>(be >> 8));
                                        tl_vfCodes.push_back(static_cast<uint8_t>(be));
                                    }
                                    ent.vsRefsDeclHash = mcla::renderer::HashShaderBytecode(tl_vfCodes);
                                }
                                vsRefsPtr = &ent.vsRefs;
                                vsRefsDeclHash = ent.vsRefsDeclHash;
                                vsEnvPtr = vsEnv.data();
                                vsEnvBytes = vsEnv.size();
                                psEnvPtr = psEnv.data();
                                psEnvBytes = psEnv.size();
                                if (s_envMemoOn) {
                                    ent.progPtr = ent.prog.get();
                                    envMemoCurPtr = ent.prog.get();
                                    ent.gen = s_vpCacheGen;
                                    ent.vsEnvKeyHash = EnvelopeKeyHash(vsEnv, 0xcbf29ce484222325ULL);
                                    if (!psEnv.empty())
                                        ent.psEnvKeyHash = EnvelopeKeyHash(psEnv, 0xcbf29ce484222325ULL);
                                    ent.envKeyValid = true;
                                    memoProgPtr = ent.progPtr;
                                    memoGen = ent.gen;
                                    memoVsEnvKeyHash = ent.vsEnvKeyHash;
                                    memoPsEnvKeyHash = ent.psEnvKeyHash;
                                    memoVsValid = true;
                                    memoPsValid = !psEnv.empty();
                                }
                            }
                            if (vpOk) {
                                const mcla::renderer::ShaderProgram& vsProg = *vsHold;
                                s_pipeParsed.fetch_add(1, std::memory_order_relaxed);
                                // THE MEASUREMENT THAT DECIDES THE DRAW.
                                pipeVsElements = static_cast<uint32_t>(vsProg.vertexElements.size());
                                pipeVsInstructions = static_cast<uint32_t>(vsProg.instructions.size());
                                pipeVsInterpolants = static_cast<uint32_t>(vsProg.interpolators.size());
                                if (pipeVsElements == 0u) {
                                    // F-897: the embedded VS carries no
                                    // vertexElements table (measured empty), but
                                    // the CP may have captured fetch descriptors
                                    // (dc.vbElementCount > 0). Those descriptors
                                    // ARE the guest's own vertex declaration, so
                                    // build the D3D12 input layout from them
                                    // instead of refusing.
                                    if (dc.vbElementCount > 0u &&
                                        dc.vbElementCount <=
                                            mcla::native::kCpVertexElementMax) {
                                      static const uint32_t kUsageByElement[] = {
                                          0u,  // POSITION (0)
                                          3u,  // NORMAL (3)
                                          6u,  // TANGENT (6)
                                          5u,  // TEXCOORD (5, index 0)
                                          5u,  // TEXCOORD (5, index 1)
                                          10u, // COLOR (10)
                                          1u,  // BLENDWEIGHT (1)
                                          2u,  // BLENDINDICES (2)
                                      };
                                      for (uint32_t ae = 0u; ae < dc.vbElementCount;
                                           ++ae) {
                                        const auto& el = dc.vbElements[ae];
                                        if (el.fmt.known == 0u || el.fmt.dxgi == 0u)
                                          continue;
                                        // F-900: PER-USAGE ORDINAL as the semantic
                                        // index. The old code hardcoded
                                        // SemanticIndex = 0 for every element, so
                                        // element 0 AND elements 3..7 all resolved to
                                        // POSITION0 (kUsageByElement maps 0,3,4,5,6,7
                                        // -> 0): two POSITION/0 entries in one layout
                                        // = "input layout does not match shader" at
                                        // PSO creation. Never fired in a recorded
                                        // soak only because the arm has never seen
                                        // >3 elements (max measured 3; dormant since
                                        // g932). The per-usage ordinal is
                                        // byte-identical to the old output for every
                                        // <=3-element layout (usageSeen yields
                                        // 0,0,0 for three DISTINCT usages), so no
                                        // measured draw changes; it only makes the
                                        // (name,index) pair unique BY CONSTRUCTION
                                        // for any vbElementCount <= 8. The table's
                                        // own values (NORMAL/BLENDWEIGHT/... -> 0u)
                                        // are deliberately NOT changed here: that
                                        // mapping is unmeasured and the ordinal alone
                                        // closes the duplicate.
                                        uint32_t usageSeen[14] = {};
                                        for (uint32_t ae2 = 0u; ae2 < ae; ++ae2) {
                                          const auto& el2 = dc.vbElements[ae2];
                                          if (el2.fmt.known == 0u || el2.fmt.dxgi == 0u)
                                            continue;
                                          const uint32_t u2 =
                                              ae2 < 8u ? kUsageByElement[ae2] : 0u;
                                          if (u2 < 14u) ++usageSeen[u2];
                                        }
                                        uint32_t usage =
                                            ae < 8u ? kUsageByElement[ae] : 0u;
                                        if (usage >= 14u) usage = 0u;
                                        const uint32_t semanticIndex = usageSeen[usage];
                                        D3D12_INPUT_ELEMENT_DESC e = {};
                                        e.SemanticName =
                                            mcla::renderer::VertexUsageSemanticName(
                                                usage);
                                        e.SemanticIndex = semanticIndex;
                                        e.Format =
                                            static_cast<DXGI_FORMAT>(el.fmt.dxgi);
                                        e.InputSlot = findInputSlot(el.slotIndex);
                                        e.InputSlotClass =
                                            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                                        e.AlignedByteOffset = el.offsetBytes;
                                        inputLayout.push_back(e);
                                        mcla::renderer::VertexInputSemantic sem;
                                        sem.usage = usage;
                                        sem.usageIndex = semanticIndex;
                                        pinnedLayout.push_back(sem);
                                      }
                                    }
                                    if (inputLayout.empty()) {
                                      s_pipeLayoutNoVertexElements.fetch_add(
                                          1, std::memory_order_relaxed);
                                      layoutRefusal = "no-vertex-elements";
                                    } else {
                                      layoutRefusal = "none";
                                    }
                                } else {
                                    // A REAL element table. Build the layout from
                                    // the guest's own declaration and PROVE it is
                                    // complete against the draw before using it.
                                    //
                                    // RESET THE REFUSAL NAME HERE. This is the fix for
                                    // a LIVE LANDMINE, and the gate below proves it
                                    // was armed.
                                    //
                                    // `no-decl-carried` (:2130) describes the ABSENCE
                                    // OF THE FIRST SOURCE -- the grcFvf DECLARATION
                                    // -- and it is set UNCONDITIONALLY. Nothing in the
                                    // tree ever put `layoutRefusal` back to "none".
                                    // The completeness proof below is gated on
                                    // `strcmp(layoutRefusal, "none") == 0`, so on the
                                    // live path it could not run on ANY draw, ever.
                                    // Measured on build/w690.log (105,187,535 B,
                                    // sha256 1d5ac2ab...348641, 677,188 lines):
                                    //   DRAW-CP-LAYOUT #2 draw=2 elements=1
                                    //     refusal=no-decl-carried declHash=0000...0000
                                    //   DRAW-CP-PIPE  #2 draw=2 layFromVE=0 declHash=0000
                                    // A ONE-ELEMENT LAYOUT WAS BUILT and the proof did
                                    // not run, so `inputDeclHash` stayed 0 and
                                    // ComputePipelineKeyForLayout folded a ZERO
                                    // DECLARATION HASH into the pipeline key for a
                                    // layout nothing had checked. Draw 2's layout was
                                    // UNPROVEN, and a PSO built from it would have
                                    // meant nothing.
                                    //
                                    // WHAT IS NOT LOST. The absence of the declaration
                                    // stays countable exactly as before, and this
                                    // assignment does not touch either of its two homes:
                                    //   s_layoutNoDecl, bumped at :2131, and the
                                    //   `noDecl=` field of every DRAW-CP-LAYOUT row.
                                    // Both are process-lifetime totals independent of
                                    // this string. What changes is only the MEANING:
                                    // `layoutRefusal` goes back to "no layout refusal
                                    // on this draw", which is what lets the proof run
                                    // and refuse in its OWN name.
                                    //
                                    // NOTHING HERE IS WEAKENED. Every refusal this arm
                                    // sets -- no-vertex-elements, no-format-for-element,
                                    // no-carried-element-for-fmt,
                                    // layout-element-count-mismatch, layout-past-stride
                                    // -- OVERWRITES the reset, and GATE A reports
                                    // whatever this string ends up holding. The proof
                                    // that was dead now runs, and if it refuses, that
                                    // refusal is the CORRECT answer and is left in
                                    // place: a refusal that names its blocker beats a
                                    // pipeline that lies (charter F-609).
                                    layoutRefusal = "none";
                                    // F-897 FALLBACK NOT REACHABLE HERE: a copy of
                                    // the descriptor-built layout arm used to sit at
                                    // this spot guarded on
                                    // vsProg.vertexElements.empty(), but this is
                                    // the else arm of `pipeVsElements == 0` (the
                                    // assignment reads that same vector's size), so
                                    // the guard was always false and the copy never
                                    // executed (dead-code audit, 2026-10-27; the
                                    // live arm is inside that if). Removed so it is
                                    // not re-added -- it carried the same
                                    // SemanticIndex=0 duplicate-POSITION0 defect
                                    // the live arm was just fixed for (F-900).
                                    //
                                    // FIELD PROVENANCE, one line per field, because
                                    // a wrong layout is a fake and this is where
                                    // that risk lives:
                                    //   SemanticName       <- vsProg.vertexElements
                                    //                         .usage, via
                                    //                         VertexUsageSemanticName
                                    //   SemanticIndex      <- .usageIndex
                                    //   AlignedByteOffset  <- VertexInputRef::offset,
                                    //                         i.e. the OFFSET THE
                                    //                         GUEST'S OWN FETCH
                                    //                         INSTRUCTION STATES
                                    //   InputSlotClass     <- PER_VERTEX_DATA: a
                                    //                         vertex buffer, stated
                                    //                         not inferred
                                    //   InputSlot          <- 0: this stream is
                                    //                         the one bound at slot 0
                                    //                         (dc.vbBase is the
                                    //                         slot-0 address the
                                    //                         producer resolved)
                                    //   Format             <- dc.vbElements[].fmt.dxgi,
                                    //                         PER ELEMENT, and
                                    //                         only after that
                                    //                         element's own 6-bit
                                    //                         format code was
                                    //                         matched against the
                                    //                         code the VS's own
                                    //                         fetch named. The
                                    //                         provenance rule and
                                    //                         the two refusals are
                                    //                         at the lookup below.
                                    //                         dc.vbFormat (element
                                    //                         0's summary) is no
                                    //                         longer read here at
                                    //                         all; it is still
                                    //                         printed on the
                                    //                         DRAW-CP-LAYOUT row.
                                    static thread_local std::vector<mcla::renderer::VertexInputRef> tl_refsFallback;
                                    const auto& refs = vsRefsPtr
                                        ? *vsRefsPtr
                                        : (tl_refsFallback = mcla::renderer::ReferencedVertexInputs(vsProg));
                                    for (size_t ri = 0; ri < refs.size(); ++ri) {
                                        const auto& r = refs[ri];
                                        // THE FORMAT FOR THIS ELEMENT, from the
                                        // CAPTURE. Not from element 0.
                                        //
                                        // WAS: "Only the element the command
                                        // actually carried has a measured FORMAT",
                                        // i.e. `(ri == 0) && (dc.vbFormat.dxgi !=
                                        // 0u)`. That grants a DXGI format to element
                                        // 0 ALONE, so with the two derived
                                        // elements of build/w690.log `ri == 1` could
                                        // never be true. Measured on that log:
                                        //   DRAW-CP-PIPE #1 draw=1
                                        //     stop=no-format-for-element
                                        //     vsElements=2
                                        //   DRAW-CP-LAYOUT #1 draw=1 elements=0
                                        //     nDesc=2 descriptorsNotCarried=1
                                        // TWO descriptors were DECODED by the
                                        // producer and the second was dropped here.
                                        // That is this gate keeping draw 1 from a
                                        // pixel.
                                        //
                                        // THE JUSTIFYING COMMENT WAS FALSE AND IS
                                        // CORRECTED, because a false sentence is
                                        // what the gate was resting on. It read: "this
                                        // project captures neither the vfetch
                                        // dst_reg (gpu_cp.cpp:1352 states the
                                        // omission as deliberate) nor
                                        // SQ_INTERPOLATOR_CNTL".
                                        //
                                        // WITHDRAWN: the dst_reg half. DrawCaptured-
                                        // Command carries `vbElements[]`
                                        // (src/render_queue.h:191-211 for the
                                        // element, :464-465 for the array and its
                                        // bound) with `elementIndex`, `dstReg`,
                                        // `dstRegAm`, `srcReg`, `srcSwizzle`,
                                        // `dstSwizzle`, `slotIndex`, `strideBytes`,
                                        // `offsetBytes`, `miniFetch`, `predicated`,
                                        // `conditional`, `predCondition`,
                                        // `indexRounded` and a PER-ELEMENT `fmt`
                                        // (xenos / dxgi / bytes / known). It is
                                        // filled at capture and printed as
                                        // CP-FETCHVB-ELEM. On the very draws this
                                        // gate refused, build/w690.log prints:
                                        //   #1 elem=0/2 dstReg=1 dstSwiz=A88 vf=57
                                        //      dxgi=6  fmtB=12 known=1 strideB=28
                                        //      offB=0  mini=0
                                        //   #1 elem=1/2 dstReg=0 dstSwiz=688 vf=38
                                        //      dxgi=2  fmtB=16 known=1 strideB=0
                                        //      offB=12 mini=1
                                        //   #2 elem=0/1 dstReg=0 dstSwiz=B48 vf=37
                                        //      dxgi=16 fmtB=8  known=1 strideB=8
                                        //      offB=0  mini=0
                                        // BOTH of draw 1's elements carry a real
                                        // DXGI mapping, and both are captured. So
                                        // this was never a capture gap: it was the
                                        // layout builder reading the wrong field.
                                        //
                                        // NOT WITHDRAWN: the SQ_INTERPOLATOR_CNTL
                                        // half. It is still uncarried, and nothing
                                        // in the format path needs it -- the comment
                                        // above the layout block (the grc_fvf
                                        // lane-code note) is the thing that does.
                                        //
                                        // HOW THE PROGRAM'S INPUT PAIRED WITH THE
                                        // CAPTURED ELEMENT, and why it invents
                                        // nothing. Two INDEPENDENT decoders read
                                        // the same guest fetch:
                                        //   refs[ri].vertexFormat -- the 6-bit
                                        //     data_format() the VS's OWN fetch
                                        //     named for this input, already
                                        //     masked at the decode
                                        //     (src/renderer/xenos_microcode.cpp:
                                        //     164, `instr.vertexFormat =
                                        //     (word1 >> 16) & 0x3F`);
                                        //   dc.vbElements[ae].fmt.xenos -- the same
                                        //     6-bit code the CP decoder read out
                                        //     of that fetch (src/gpu_cp.cpp; the
                                        //     field is carried at
                                        //     src/render_queue.h:210).
                                        // The pair is accepted ONLY when the two
                                        // name the SAME 6-bit code, and the format
                                        // used is then that element's CAPTURED
                                        // `fmt`. This file never calls a vf->DXGI
                                        // table here, never defaults, and never
                                        // lends element 0's format to another
                                        // element. When the two decoders disagree,
                                        // the refusal below is the correct answer,
                                        // not a mismatch to be papered over.
                                        //
                                        // TWO REFUSALS, because they are DIFFERENT
                                        // blockers and a refusal that misdescribes
                                        // its own cause is the defect this whole
                                        // chain exists to prevent:
                                        //   no-carried-element-for-fmt -- no element
                                        //     the CP captured on THIS draw names
                                        //     that 6-bit code. The VS and the CP
                                        //     disagree about this vertex.
                                        //   no-format-for-element       -- the code WAS
                                        //     captured but is UNMAPPED
                                        //     (`fmt.known == 0`) or has no DXGI
                                        //     mapping (`fmt.dxgi == 0`). NOT
                                        //     NEGOTIABLE: an unmapped format is a
                                        //     refusal, never a default. The NAME is
                                        //     the one w690 already printed for that
                                        //     case, so the census of this refusal
                                        //     keeps its vocabulary; the MEANING is
                                        //     now the per-element one rather than
                                        //     "only element 0 has a format".
                                        //
                                        // Both are countable by value on two existing
                                        // rows (`stop=` on DRAW-CP-PIPE, `refusal=`
                                        // on DRAW-CP-LAYOUT), so neither needs a new
                                        // counter and no existing marker changes
                                        // format.
                                        const uint32_t wantVf = r.vertexFormat & 0x3Fu;
                                        const mcla::native::CpVertexElement *matchedElem = nullptr;
                                        const uint32_t wantOffBytes = (r.offset >= 0) ? (static_cast<uint32_t>(r.offset) * 4u) : 0xFFFFFFFFu;
                                        for (uint32_t ae = 0u;
                                             ae < dc.vbElementCount &&
                                             ae < mcla::native::kCpVertexElementMax;
                                             ++ae) {
                                            const auto& el = dc.vbElements[ae];
                                            if ((el.fmt.xenos & 0x3Fu) == wantVf &&
                                                el.slotIndex == r.slotIndex &&
                                                (wantOffBytes == 0xFFFFFFFFu || el.offsetBytes == wantOffBytes)) {
                                                matchedElem = &el;
                                                break;
                                            }
                                        }
                                        if (matchedElem == nullptr) {
                                            for (uint32_t ae = 0u;
                                                 ae < dc.vbElementCount &&
                                                 ae < mcla::native::kCpVertexElementMax;
                                                 ++ae) {
                                                const auto& el = dc.vbElements[ae];
                                                if ((el.fmt.xenos & 0x3Fu) == wantVf &&
                                                    el.slotIndex == r.slotIndex) {
                                                    matchedElem = &el;
                                                    break;
                                                }
                                            }
                                        }
                                        if (matchedElem == nullptr && streamCount <= 1u) {
                                            for (uint32_t ae = 0u;
                                                 ae < dc.vbElementCount &&
                                                 ae < mcla::native::kCpVertexElementMax;
                                                 ++ae) {
                                                if ((dc.vbElements[ae].fmt.xenos & 0x3Fu) == wantVf) {
                                                    matchedElem = &dc.vbElements[ae];
                                                    break;
                                                }
                                            }
                                        }
                                        const mcla::native::CpVertexFormat *carried =
                                            matchedElem ? &matchedElem->fmt : nullptr;
                                        if (carried == nullptr) {
                                            layoutRefusal = "no-carried-element-for-fmt";
                                            inputLayout.clear();
                                            pinnedLayout.clear();
                                            break;
                                        }
                                        // BOTH `known` and `dxgi` are required, and
                                        // that is what makes this unable to
                                        // substitute a format: `known` is the
                                        // capture's own statement that the 6-bit
                                        // code is one of the named
                                        // xenos::VertexFormats (render_queue.h:
                                        // 135), and `dxgi` is the mapping itself.
                                        const bool haveFormat =
                                            (carried->known != 0u) &&
                                            (carried->dxgi != 0u);
                                        if (!haveFormat) {
                                            layoutRefusal = "no-format-for-element";
                                            inputLayout.clear();
                                            pinnedLayout.clear();
                                            break;
                                        }
                                        D3D12_INPUT_ELEMENT_DESC e = {};
                                        e.SemanticName =
                                            mcla::renderer::VertexUsageSemanticName(r.usage);
                                        e.SemanticIndex = r.usageIndex;
                                        e.Format =
                                            static_cast<DXGI_FORMAT>(carried->dxgi);
                                        e.InputSlot = findInputSlot(matchedElem ? matchedElem->slotIndex : r.slotIndex);
                                        e.InputSlotClass =
                                            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                                        // AlignedByteOffset <- VertexInputRef::offset,
                                        // the OFFSET THE GUEST'S OWN FETCH
                                        // INSTRUCTION STATES, IN BYTES.
                                        //
                                        // WAS: `static_cast<UINT>(r.offset)`, no
                                        // scale. That is a UNIT ERROR and it is
                                        // measured, not suspected:
                                        // src/renderer/xenos_microcode.cpp:171-172
                                        // decodes the field as `(word2 >> 8) &
                                        // 0x7FFFFF` sign-extended, which is DWORDS
                                        // -- the same field is documented as
                                        // "offset() :742, DWORDS" at
                                        // src/render_queue.h:175, and the
                                        // neighbouring `stride` is `word2 & 0xFF`
                                        // dwords at :170. The CP side scales it
                                        // exactly ONCE, at capture:
                                        // CP-FETCHVB-ELEM #1 in build/w690.log
                                        // prints `offB=12` for the fetch whose own
                                        // word2 holds `offsetDwords=3`
                                        // (CP-FETCHVB-DESC #1 instr=1, the same
                                        // log). Passing the dword count as a byte
                                        // offset therefore placed draw 1's second
                                        // element at byte 3, INSIDE element 0's
                                        // 12-byte payload, and it also made the
                                        // completeness check's `top` four times
                                        // too small -- a check that could not
                                        // fail. The scale is 4 because the field
                                        // is in dwords; it is the same scale
                                        // CpVertexElement::strideBytes applies
                                        // (src/render_queue.h:203, "stride() * 4;
                                        // DWORDS scaled ONCE here"). A negative
                                        // offset keeps its existing meaning (append)
                                        // and is not scaled.
                                        e.AlignedByteOffset =
                                            (r.offset >= 0)
                                                ? (static_cast<UINT>(r.offset) * 4u)
                                                : D3D12_APPEND_ALIGNED_ELEMENT;
                                        inputLayout.push_back(e);
                                        mcla::renderer::VertexInputSemantic sem;
                                        sem.usage = r.usage;
                                        sem.usageIndex = r.usageIndex;
                                        pinnedLayout.push_back(sem);
                                    }
                                    if (!inputLayout.empty() &&
                                        std::strcmp(layoutRefusal, "none") == 0) {
                                        // COMPLETENESS, measured against the draw
                                        // and not assumed. Two independent facts
                                        // have to agree: the number of elements the
                                        // program reads must be the number the
                                        // producer decoded, and the top of the
                                        // layout must reach the stride. Either
                                        // mismatch means the draw would read
                                        // attributes this PSO does not supply, or
                                        // would read past the vertex.
                                        const uint32_t wantDesc =
                                            (dc.vbDescCount > 0u) ? dc.vbDescCount
                                                                   : inputLayout.size();
                                        uint32_t topBySlot[4] = {};
                                        for (const auto& e : inputLayout) {
                                            const uint32_t sl = (e.InputSlot < 4u) ? e.InputSlot : 0u;
                                            if (e.AlignedByteOffset !=
                                                D3D12_APPEND_ALIGNED_ELEMENT) {
                                                topBySlot[sl] = (e.AlignedByteOffset >
                                                       topBySlot[sl]) ? e.AlignedByteOffset + 1u
                                                            : topBySlot[sl] + 1u;
                                            }
                                        }
                                        bool strideExceeded = false;
                                        for (uint32_t s = 0u; s < streamCount; ++s) {
                                            if (topBySlot[s] > streams[s].vbStride) {
                                                strideExceeded = true;
                                                break;
                                            }
                                        }
                                        // Measured (TEMP-LAYOUT-DIAG, nb18.log): the VS microcode fetches
                                        // 4 elements (pos@0, vf7@12, vf6@16, vf31@20; 1 full + 3 mini) but
                                        // the translated VS declares 2 (ReferencedVertexInputs: pos, usage 5).
                                        // Fetched-but-unread elements are not an error: the VB view keeps
                                        // the full stride. Only a VS reading MORE than was fetched refuses.
                                        if (inputLayout.size() > wantDesc) {
                                            {   // GNH-LAYOUT-MISMATCH: measured counts behind the refusal
                                                static StatCtr<uint32_t> s_lm{0};
                                                const uint32_t lm = s_lm.fetch_add(1) + 1;
                                                                                                if (lm <= 12 || lm % 500 == 0)
                                                    MCLA_LOG_WARN("GNH-LAYOUT-MISMATCH #{} layout={} wantDesc={} vbElem={} full={} streams={} sel={} stride={}",
                                                                  lm, inputLayout.size(), wantDesc, dc.vbElementCount,
                                                                  dc.vbFullCount, dc.vbStreamCount, dc.sourceSelect, dc.vbStride);
                                            }
                                            layoutRefusal = "layout-element-count-mismatch";
                                            inputLayout.clear();
                                            pinnedLayout.clear();
                                        } else if (strideExceeded) {
                                            layoutRefusal = "layout-past-stride";
                                            inputLayout.clear();
                                            pinnedLayout.clear();
                                        } else {
                                            s_pipeLayoutFromVertexElements.fetch_add(
                                                1, std::memory_order_relaxed);
                                            // vertexDeclHash, precomputed per parsed VS in s_vpCache when available.
                                            if (vsRefsPtr) {
                                                inputDeclHash = vsRefsDeclHash;
                                            } else {
                                                static thread_local std::vector<uint8_t> vfCodes;
                                                vfCodes.clear();
                                                vfCodes.reserve(refs.size() * 4u);
                                                for (const auto& r2 : refs) {
                                                    const uint32_t be = r2.vertexFormat;
                                                    vfCodes.push_back(static_cast<uint8_t>(be >> 24));
                                                    vfCodes.push_back(static_cast<uint8_t>(be >> 16));
                                                    vfCodes.push_back(static_cast<uint8_t>(be >> 8));
                                                    vfCodes.push_back(static_cast<uint8_t>(be));
                                                }
                                                inputDeclHash = mcla::renderer::HashShaderBytecode(vfCodes);
                                            }
                                        }
                                    }
                                }
                            } else {
                                layoutRefusal = "vs-parse-refused";
                            }
                        }
                    }
                }

                // The row. bytesUnaccounted is the load-bearing number and it is
                // ARITHMETIC ON CARRIED FIELDS. It is the per-vertex stride the
                // program's OWN vertex fetches do not account for, and it is the
                // completeness check on the stride: if the fetches do not cover
                // the whole vertex then the input layout does not describe the
                // stream and nothing downstream may treat it as if it did.
                //
                // WHAT WAS WRONG (F-720). This summed ONE number --
                // dc.vbFormat.bytes, element 0's summary -- and therefore dropped
                // the vfetch_mini the producer had already decoded and carried.
                // Measured in build/w686.log it printed bytesUnaccounted=16 of a
                // 28 B vertex on the stride-28 draws and 0 of an 8 B vertex on the
                // stride-8 draws. The 16 B was not padding and not a second copy
                // of element 0: CP-FETCHVB-DESC #1 in the same log carries TWO
                // descriptors for that stream, and the arithmetic closes exactly
                // when both are counted:
                //
                //   element 0 dword_0=30081000  vf=57(k_32_32_32_FLOAT)
                //           dstSwizzle=0xA88 -> used=0b0111 -> mask 0b0111
                //           -> 3 dwords -> 12 B   miniFetch=0
                //   element 1 dword_0=00080000  vf=38(k_32_32_32_32_FLOAT)
                //           dstSwizzle=0x688 -> used=0b1111 -> mask 0b1111
                //           -> 4 dwords -> 16 B   miniFetch=1
                //   12 + 16 = 28 = the stride. Unaccounted = 0.
                //
                //   draw 2, one element: vf=37(k_32_32_FLOAT)
                //           dstSwizzle=0xB48 -> used=0b0011 -> mask 0b0011
                //           -> 2 dwords ->  8 B = the stride. Unaccounted = 0,
                //           which is why that draw already read 0 and hid the bug.
                //
                // The destination-swizzle narrowing is the reference's own
                // (shader_interpreter.cc:1019-1028 over xenos.h:648-683); see
                // CpVfetchBytesForSwizzle for why CpXenosFormatBytes cannot
                // express it.
                //
                // THE CHECK IS NOT WEAKENED, IT IS STRENGTHENED. Before, ANY
                // second element was worth a fixed 16 B of unexplained stride,
                // so the only draw that could read 0 was a one-element one whose
                // single element happened to equal the whole stride. Now a
                // mismatch can ONLY come from the guest's own fetches failing to
                // cover its own vertex, and that case still reads non-zero. There
                // is no path by which summing more elements turns a refusal into
                // a pass: this number is a REPORT either way, and the refusal
                // that depends on it is the layout-element-count /
                // layout-past-stride check above, which this change does not
                // touch.
                //
                // Saturating, not wrapping: `accounted` is accumulated in
                // uint64 and clamped, so a stream whose fetches exceed the stride
                // reports the whole stride accounted (unaccounted = 0) rather
                // than a wrapped 0xFFFF... The wrap would be a bigger lie than
                // either.
                uint64_t accountedRaw = 0;
                uint32_t countedElems = 0;
                for (uint32_t ae = 0u;
                     ae < dc.vbElementCount && ae < mcla::native::kCpVertexElementMax;
                     ++ae) {
                    accountedRaw += CpVfetchBytesForSwizzle(
                        dc.vbElements[ae].fmt.xenos,
                        dc.vbElements[ae].dstSwizzle);
                    ++countedElems;
                }
                if (countedElems == 0u) {
                    // No element was carried, so there is nothing to sum. Element
                    // 0's summary is then the only thing on the command and it is
                    // what a reader would use; it was ALREADY refused as
                    // unmapped above if it had no size.
                    accountedRaw = dc.vbFormat.bytes;
                }
                const uint32_t accounted =
                    (accountedRaw > static_cast<uint64_t>(dc.vbStride))
                        ? dc.vbStride
                        : static_cast<uint32_t>(accountedRaw);
                const uint32_t lb =
                    s_layoutLogged.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (lb <= 8u || (lb % 20000u) == 0u) {  // MD-32: was 200 (also gates LAYOUT-ELEM)
                    MCLA_LOG_WARN(
                        "DRAW-CP-LAYOUT #{} draw={} elements={} declHash={:016X} "
                        "refusal={} | vf={} dxgi={} fmtB={} stride={} "
                        "bytesUnaccounted={} nDesc={} nFull={} "
                        "descriptorsNotCarried={} built={} noDecl={} "
                        "declRefused={}",
                        lb, n, inputLayout.size(), inputDeclHash,
                        layoutRefusal, dc.vbFormat.xenos, dc.vbFormat.dxgi,
                        dc.vbFormat.bytes, dc.vbStride,
                        dc.vbStride - accounted, dc.vbDescCount,
                        dc.vbFullCount,
                        (dc.vbDescCount > 1u) ? (dc.vbDescCount - 1u) : 0u,
                        s_layoutBuilt.load(std::memory_order_relaxed),
                        s_layoutNoDecl.load(std::memory_order_relaxed),
                        s_layoutDeclRefused.load(std::memory_order_relaxed));
                    // F-900: per-element semantics. The summary row cannot show
                    // whether POSITION/COLOR/TEXCOORD map to the shader's
                    // POSITION0/TEXCOORD0/TEXCOORD1. Log each bound element.
                    for (size_t li = 0; li < inputLayout.size(); ++li) {
                        MCLA_LOG_WARN("DRAW-CP-LAYOUT-ELEM #{} draw={} elem={}/{} "
                                      "sem={}{} offB={} dxgi={}",
                                      lb, n, li, inputLayout.size(),
                                      inputLayout[li].SemanticName ? inputLayout[li].SemanticName : "?",
                                      inputLayout[li].SemanticIndex,
                                      inputLayout[li].AlignedByteOffset,
                                      static_cast<uint32_t>(inputLayout[li].Format));
                    }
                }
            }
            // inputLayout / inputDeclHash are the seam step 5 consumes. They are
            // read here so the pair cannot be deleted as unused before then, and
            // so this file states in one place what the next change has to do: a
            // PSO cannot be built from a stream format, and the F-652
            // no-translated-pso gate still stands unchanged above them.
            //
            // F-716: NO LONGER A SEAM. The (void) reads below are gone because the
            // F-652 derivation block now CONSUMES all three -- it builds the key
            // with inputDeclHash, refuses on an empty inputLayout, pins the VS
            // against pinnedLayout, and asks the cache for a PSO. They are not
            // deleted, they are used.

            // F-716: THE CHAIN'S OWN ROW. A new marker name, because the existing
            // ones are tracked (tools/soak_census.py) and their formats must not
            // change; this one answers "how far did this draw get" in one place,
            // which is otherwise spread over five refusal reasons. Bounded like
            // every other row here (n <= 8, then every 200th) while every number
            // on it is a process-lifetime total, so bounding the log does not
            // bound the census (F-054 / F-648).
            auto pipeRow = [&]() {
                const uint32_t pn =
                    s_pipeLogged.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (pn > 8u && (pn % 20000u) != 0u) return;  // MD-32: was 200
                // Swarm p2-edit probe: count legal-glyph draws (self-cadenced inside).
                mcla_legals_probe(pipeVsHash, pipePsHash, pn);
                MCLA_LOG_WARN(
                    "DRAW-CP-PIPE #{} draw={} stop={} | vsSrc={} vsB={} psSrc={} "
                    "psB={} | envelope={} parsed={} vsElements={} vsInstrs={} "
                    "vsInterp={} | vsHlsl={} psHlsl={} vsHash={:016X} "
                    "psHash={:016X} | layout={} declHash={:016X} | "
                    "psoReq={} psoNull={} psoGeomStage={} || "
                    "vsCode={} psCode={} envBuilt={} parsed={} vsTr={} psTr={} "
                    "layFromVE={} layNoVE={} pso={} geom={}",
                    pn, n, pipeStop, pipeVsSrc, pipeVsCodeBytes, pipePsSrc,
                    pipePsCodeBytes,
                    s_pipeEnvelopeBuilt.load(std::memory_order_relaxed),
                    s_pipeParsed.load(std::memory_order_relaxed),
                    pipeVsElements, pipeVsInstructions, pipeVsInterpolants,
                    pipeVsHlsl->size(), pipePsHlsl->size(), pipeVsHash, pipePsHash,
                    inputLayout.size(), inputDeclHash,
                    s_pipePsoRequested.load(std::memory_order_relaxed),
                    s_pipePsoNull.load(std::memory_order_relaxed),
                    pipeHasGeomStage ? 1u : 0u,
                    s_pipeVsCode.load(std::memory_order_relaxed),
                    s_pipePsCode.load(std::memory_order_relaxed),
                    s_pipeEnvelopeBuilt.load(std::memory_order_relaxed),
                    s_pipeParsed.load(std::memory_order_relaxed),
                    s_pipeTranslatedVs.load(std::memory_order_relaxed),
                    s_pipeTranslatedPs.load(std::memory_order_relaxed),
                    s_pipeLayoutFromVertexElements.load(std::memory_order_relaxed),
                    s_pipeLayoutNoVertexElements.load(std::memory_order_relaxed),
                    s_pipePsoRequested.load(std::memory_order_relaxed),
                    s_pipePsoCarriesGeomStage.load(std::memory_order_relaxed));
                // THE REFUSAL STRING, WHICH HAD NO READER IN THE TREE.
                //
                // Measured before this line existed: `pipeTranslateError`
                // had THREE occurrences in the whole repository -- its
                // declaration (:2106) and the two TranslateShaderPinned calls
                // that hand it over as an out-parameter. NOTHING READ IT. The
                // translator does fill it on every refusal path
                // (src/renderer/shader_translator.cpp:1362 "container parse
                // failed", :1399 the pixel-shader/layout refusal, :1422
                // `gen.error()`, :1428 "translation produced no HLSL"), so the
                // string was produced and then dropped on the floor: a
                // `stop=ps-translate-refused` row said THAT it stopped and
                // never WHY.
                //
                // A NEW MARKER NAME, NOT A FIELD ON THIS ROW. DRAW-CP-PIPE is
                // one of the tracked markers (its §9 inventory is re-parsed
                // by tools/soak_census.py) and appending a field to it would
                // change a tracked format. `DRAW-CP-PIPE-ERR` is a name of its
                // own, measured at 0 hits across src/**/*.cpp, src/**/*.h and
                // docs/*.md before this line was written, so it cannot collide
                // with a marker, a row or a doc heading.
                //
                // Bounded by the same `pn` and printed only when the string is
                // non-empty, from inside the same pipeRow() call, so a
                // DRAW-CP-PIPE-ERR line ALWAYS follows its DRAW-CP-PIPE line and
                // the two shapes cannot disagree about which draws they
                // describe. F-054 (one shape owning a log) is not at risk: at
                // most one extra line per bounded row.
                if (!pipeTranslateError.empty()) {
                    MCLA_LOG_WARN(
                        "DRAW-CP-PIPE-ERR #{} draw={} stop={} stage={} err={}",
                        pn, n, pipeStop, pipeTrStage, pipeTranslateError);
                }
            };

            // --- F-652: pipeline-state derivation, ON THE LIVE PATH ----------
            //
            // Placed HERE - after every data-integrity gate, BEFORE the topology
            // gate - for a reason that is not cosmetic. The measured stream is
            // 100% prim=0x08 kRectangleList (build/w664.log), and if the
            // derivation sat below the MapXenosPrimType gate it would be dead
            // code again on exactly the stream it exists to explain: the
            // topology gate would break first, every time. So the derivation
            // runs first, refuses prim=0x08 by its OWN named reason
            // (unsupported-topology, pipeline_cache.cpp:898-912), and the
            // existing prim-rect-list counter is still bumped below so neither
            // marker goes dark.
            //
            // WHAT POPULATES PipelineStateInputs, field by field:
            //   paClipCntl/paSuScModeCntl/paClVteCntl  <- dc.rasterState.regs[],
            //     snapshotted by the CP at enqueue (src/gpu_cp.cpp,
            //     CpSnapshotDrawState). NOT re-read here.
            //   registersPresent  <- dc.rasterState.presentMask, mapped onto the
            //     renderer::kGuestReg* bits by POSITION below. This is the fact
            //     the whole refusal chain rests on, and it comes from the
            //     export's per-index presence bitmap (src/gpu_cp.cpp:144, read
            //     at snapshot time) - NEVER from "the value is non-zero". Those
            //     are different facts: PA_SU_SC_MODE_CNTL = 0 is a LEGAL and
            //     DIFFERENT rasteriser (pipeline_cache.h:132-136), so a
            //     value-derived presence bit would turn "never written" into a
            //     confident, wrong rasteriser.
            //   primType          <- dc.primitiveTopology, which is
            //     VGT_DRAW_INITIATOR.prim_type read at 6 bits
            //     (src/gpu_cp.cpp DrawInitiatorField(di, 0, 6), registers.h:315)
            //     - i.e. the ARCHITECTED xenos::PrimitiveType. This settles, for
            //     this path, the "UNMEASURED PREMISE" F-645 left open at
            //     pipeline_cache.cpp:857-871: the stale `ctx.r4.u32 & 7` premise
            //     belongs to capture_hooks.cpp/DRAW_INDEXED, NOT to this one. A
            //     3-bit field could not even express 0x08.
            //   polyOffset*       <- the four kFloat magnitudes, host-order.
            //   depthFormat       <- RB_DEPTH_INFO bit 16, ONLY when present.
            //   colorFormats[0]    <- F-723: RB_COLOR_INFO.color_format (4 bits
            //     at +16), mapped to a DXGI_FORMAT by
            //     MapXenosColorFormatToDxgi, ONLY when the register is PRESENT.
            //     colorFormats[1..7] and depthStencilFormat: STILL LEFT AT ZERO.
            //     RB_COLOR1/2/3_INFO (0x2003/0x2004/0x2005) have no slot in
            //     CpSnapshotReg, so slots 1..7 are not captured rather than
            //     guessed, and DerivePipelineState's numRt therefore counts only
            //     the targets this stream really measured. Viewport extents
            //     remain zero too, which DerivePipelineState refuses itself if
            //     the guest enabled a viewport transform
            //     (pipeline_cache.cpp:787), and it folds DepthEnable off when
            //     there is no depth format (:1144-1149) rather than inventing
            //     one.
            {
                s_deriveAttempted.fetch_add(1, std::memory_order_relaxed);
                mcla::renderer::PipelineStateInputs psi;
                const auto& rs = dc.rasterState;
                psi.paClipCntl = rs.regs[CpSnapshotReg::kCpSnapPaClipCntl];
                psi.paSuScModeCntl =
                    rs.regs[CpSnapshotReg::kCpSnapPaSuScModeCntl];
                psi.paClVteCntl = rs.regs[CpSnapshotReg::kCpSnapPaClVteCntl];
                psi.viewportYFlipped = vpYFlipped;
                // Expanded rect lists are drawn as triangle lists, so the key
                // must name the triangle pipeline (no geometry stage), not
                // the guest's rectangle primitive. The guest's 0x08 is kept
                // in desc.xenosPrimType below; this field is what the PSO is
                // built for.
                psi.primType = static_cast<uint8_t>(
                    rectCpuExpanded ? 0x04u : dc.primitiveTopology);
                // haveRegisters is the DERIVATION's precondition, not the
                // guest's: it says "these three values are the guest's", which is
                // only true when all three presence bits are set. Any subset and
                // the answer is kRefuseStateNotDerived, which is precisely the
                // refusal that keeps a partially-snapshotted draw from building
                // a half-default rasteriser.
                psi.haveRegisters = (rs.presentMask &
                                     (mcla::renderer::kGuestRegPaClipCntl |
                                      mcla::renderer::kGuestRegPaSuScModeCntl |
                                      mcla::renderer::kGuestRegPaClVteCntl)) ==
                                    (mcla::renderer::kGuestRegPaClipCntl |
                                     mcla::renderer::kGuestRegPaSuScModeCntl |
                                     mcla::renderer::kGuestRegPaClVteCntl);
                // POSITION, not VALUES: bit i of the producer's mask -> bit k of
                // the renderer's GuestRegisterPresent. The two enums number
                // things differently on purpose (kCpSnapPaClipCntl = 0 vs
                // kGuestRegPaClipCntl = 1<<1), so this table is the ONE place the
                // correspondence exists and it cannot be "obvious" enough to
                // drift.
                psi.registersPresent =
                    ((rs.presentMask &
                      (1u << CpSnapshotReg::kCpSnapPaClipCntl))
                         ? mcla::renderer::kGuestRegPaClipCntl
                         : mcla::renderer::kGuestRegNone) |
                    ((rs.presentMask &
                      (1u << CpSnapshotReg::kCpSnapPaSuScModeCntl))
                         ? mcla::renderer::kGuestRegPaSuScModeCntl
                         : mcla::renderer::kGuestRegNone) |
                    ((rs.presentMask &
                      (1u << CpSnapshotReg::kCpSnapPaClVteCntl))
                         ? mcla::renderer::kGuestRegPaClVteCntl
                         : mcla::renderer::kGuestRegNone) |
                    ((rs.presentMask & ((1u << CpSnapshotReg::kCpSnapPolyOffFrontScale) |
                                        (1u << CpSnapshotReg::kCpSnapPolyOffFrontOffset)))
                         ? mcla::renderer::kGuestRegPolyOffsetFront
                         : mcla::renderer::kGuestRegNone) |
                    ((rs.presentMask & ((1u << CpSnapshotReg::kCpSnapPolyOffBackScale) |
                                        (1u << CpSnapshotReg::kCpSnapPolyOffBackOffset)))
                         ? mcla::renderer::kGuestRegPolyOffsetBack
                         : mcla::renderer::kGuestRegNone) |
                    ((rs.presentMask & (1u << CpSnapshotReg::kCpSnapDepthInfo))
                         ? mcla::renderer::kGuestRegDepthInfo
                         : mcla::renderer::kGuestRegNone) |
                    // F-904: blend control + write mask. Two bits because a
                    // lone control with an unmeasured mask (or vice versa)
                    // must not read as "measured blend".
                    ((rs.presentMask & (1u << CpSnapshotReg::kCpSnapBlendControl0))
                         ? mcla::renderer::kGuestRegBlendControl
                         : mcla::renderer::kGuestRegNone) |
                    ((!s_noMrt && (rs.presentMask & (1u << CpSnapshotReg::kCpSnapBlendControl1)))
                         ? mcla::renderer::kGuestRegBlendControl1
                         : mcla::renderer::kGuestRegNone) |
                    ((!s_noMrt && (rs.presentMask & (1u << CpSnapshotReg::kCpSnapBlendControl2)))
                         ? mcla::renderer::kGuestRegBlendControl2
                         : mcla::renderer::kGuestRegNone) |
                    ((!s_noMrt && (rs.presentMask & (1u << CpSnapshotReg::kCpSnapBlendControl3)))
                         ? mcla::renderer::kGuestRegBlendControl3
                         : mcla::renderer::kGuestRegNone) |
                    ((rs.presentMask & (1u << CpSnapshotReg::kCpSnapColorMask))
                         ? mcla::renderer::kGuestRegColorMask
                         : mcla::renderer::kGuestRegNone);
                {
                    static const bool s_noAlphaToMask = [] {
                        const char* e = std::getenv("MCLA_NO_ALPHA_TO_MASK");
                        return e && e[0] == '1';
                    }();
                    psi.alphaToMaskEnable =
                        !s_noAlphaToMask &&
                        ((rs.presentMask & (1u << CpSnapshotReg::kCpSnapColorControl)) != 0u) &&
                        (((rs.regs[CpSnapshotReg::kCpSnapColorControl] >> 4) & 1u) != 0u);
                }
                // kFloat registers: the file's register values are the LOGICAL
                // dwords (a big-endian READ produced them, which is byte-for-byte
                // the reference's ReadAndSwap result), so the bits reinterpret as
                // a host float with NO byte swap. memcpy, never a union pun and
                // never a reinterpret_cast of a guest address.
                const uint32_t poFloats[4] = {
                    rs.regs[CpSnapshotReg::kCpSnapPolyOffFrontScale],
                    rs.regs[CpSnapshotReg::kCpSnapPolyOffFrontOffset],
                    rs.regs[CpSnapshotReg::kCpSnapPolyOffBackScale],
                    rs.regs[CpSnapshotReg::kCpSnapPolyOffBackOffset]};
                std::memcpy(&psi.polyOffsetFrontScale, &poFloats[0],
                            sizeof(psi.polyOffsetFrontScale));
                std::memcpy(&psi.polyOffsetFrontOffset, &poFloats[1],
                            sizeof(psi.polyOffsetFrontOffset));
                std::memcpy(&psi.polyOffsetBackScale, &poFloats[2],
                            sizeof(psi.polyOffsetBackScale));
                std::memcpy(&psi.polyOffsetBackOffset, &poFloats[3],
                            sizeof(psi.polyOffsetBackOffset));
                // depth_format is ONE bit at +16 of RB_DEPTH_INFO
                // (registers.h:844). Read it ONLY when the register is present,
                // which is what PipelineStateInputs documents.
                psi.depthFormat =
                    (psi.registersPresent & mcla::renderer::kGuestRegDepthInfo)
                        ? ((rs.regs[CpSnapshotReg::kCpSnapDepthInfo] >> 16) & 1u)
                        : 0u;
                // Viewport extents: RB_SURFACE_INFO carries surface_pitch (the
                // guest's render-target width in pixels, measured 1280) but no
                // height field (registers.h:670-680). Height defaults to the
                // back-buffer height, exactly as d3d12_backend.cpp:3653-3656
                // does (guestH = m_height when no guest height exists).
                const auto grt = gpu::CpGuestRenderTargetInfo();
                psi.viewportWidth = backend_.GetWidth();
                psi.viewportHeight = backend_.GetHeight();
                // Depth/stencil wiring: RB_DEPTHCONTROL (0x2200) captured from snapshot.
                if (rs.presentMask & (1u << mcla::native::CpSnapshotReg::kCpSnapDepthControl)) {
                    psi.registersPresent |= mcla::renderer::kGuestRegDepthControl;
                    psi.depthControl = rs.regs[mcla::native::CpSnapshotReg::kCpSnapDepthControl];
                    // Reversed Z (z122: PA_CL_VPORT_ZSCALE=-1, ZOFFSET=1): the guest compares z' = 1 - z. The host
                    // keeps storing z (depth textures stay as before) and mirrors the compare instead:
                    // z' <= s' <=> z >= s. Clears are mirrored in the backend (DepthCompareMirrored).
                    float zs = 1.0f;
                    std::memcpy(&zs, &rs.regs[mcla::native::CpSnapshotReg::kCpSnapVpZScale], 4);
                    const bool vteHasZScale = (rs.regs[mcla::native::CpSnapshotReg::kCpSnapPaClVteCntl] & 0x10u) != 0u;
                    static const bool s_noStdZFixPsi = []() {
                        const char* e = std::getenv("MCLA_NO_STDZ_SHADOW_FIX");
                        return e && e[0] == '1';
                    }();
                    const bool drawMirrored = DepthCompareMirrored() && (s_noStdZFixPsi ? true : (vteHasZScale && zs < 0.0f));
                    psi.depthMirrored = drawMirrored;
                    if (DepthCompareMirrored() && vteHasZScale && zs < 0.0f) {
                        static constexpr uint32_t kMirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};  // never,<,==,<=,>,!=,>=,always
                        const uint32_t f = (psi.depthControl >> 4) & 7u;
                        psi.depthControl = (psi.depthControl & ~0x70u) | (kMirror[f] << 4);
                    }
                }
                // Stencil: Xenia d3d12_command_processor.cc:2953-2964 picks the _BF register iff backface_enable && cull_front.
                {
                    static const bool s_noStencil = [] { const char* e = std::getenv("MCLA_NO_STENCIL"); return e && e[0] == '1'; }();
                    const uint32_t dcv = psi.depthControl;
                    uint32_t refMask = 0;
                    if (psi.registersPresent & mcla::renderer::kGuestRegDepthControl) {
                        const bool cullFront = (rs.regs[CpSnapshotReg::kCpSnapPaSuScModeCntl] & 1u) != 0u;
                        refMask = rs.regs[((dcv & (1u << 7)) && cullFront) ? CpSnapshotReg::kCpSnapStencilRefBF
                                                                          : CpSnapshotReg::kCpSnapStencilRef];
                        if (s_noStencil) psi.depthControl &= ~1u;
                    }
                    psi.stencilRefMask = refMask;
                    {   // per-boot census: proves the capture fires (rule 19) and whether the guest uses stencil at all
                        static std::atomic<uint32_t> cTot{0}, cPres{0}, cSten{0}, cBf{0}, cRef{0};
                        const uint32_t t = cTot.fetch_add(1) + 1;
                        if (psi.registersPresent & mcla::renderer::kGuestRegDepthControl) cPres.fetch_add(1);
                        if (dcv & 1u) { cSten.fetch_add(1); if (dcv & (1u << 7)) cBf.fetch_add(1); if (refMask) cRef.fetch_add(1); }
                        // MD-22 (log-only): D3D12 has one stencil ref per draw; count draws whose front/back refs differ.
                        static std::atomic<uint32_t> cBfDiff{0};
                        if ((dcv & 1u) && (dcv & (1u << 7)) && (psi.registersPresent & mcla::renderer::kGuestRegDepthControl) &&
                            (rs.regs[CpSnapshotReg::kCpSnapStencilRef] & 0xFFu) != (rs.regs[CpSnapshotReg::kCpSnapStencilRefBF] & 0xFFu)) {
                            const uint32_t n = cBfDiff.fetch_add(1) + 1;
                            if (n <= 4 || (n % 200) == 0)
                                MCLA_LOG_WARN("STENCIL-BF-REF-MISMATCH #{} front_ref={:02X} back_ref={:02X} bound_ref={:02X} (D3D12 single ref; BF ref only used when cull_front)",
                                              n, rs.regs[CpSnapshotReg::kCpSnapStencilRef] & 0xFFu, rs.regs[CpSnapshotReg::kCpSnapStencilRefBF] & 0xFFu, refMask & 0xFFu);
                        }
                        if (t == 1 || (t % 5000) == 0)
                            MCLA_LOG_WARN("STENCIL-CENSUS draws={} depthctl_present={} stencil_enable={} backface={} refmask_nonzero_on_stencil={}",
                                          t, cPres.load(), cSten.load(), cBf.load(), cRef.load());
                    }
                    pipeStencilRef = refMask & 0xFFu;
                    pipeStencilOn = !s_noStencil && (dcv & 1u) != 0u &&
                                    (psi.registersPresent & mcla::renderer::kGuestRegDepthControl) != 0u;
                }
                // Wire DXGI_FORMAT_D24_UNORM_S8_UINT so 3D pipelines can enable depth testing.
                psi.depthStencilFormat = D3D12Backend::GuestDepthDxgiFormat(dc.rtCaptured, dc.rtDepthInfo);  // MD-04: must equal the DSV format (opt-in MCLA_D32_DEPTH=1)

                // --- F-723: the render-target format, from RB_COLOR_INFO ------
                //
                // This is what unblocks the PSO. Until it existed,
                // PipelineStateInputs::colorFormats[] was left all-zero on
                // purpose (the "LEFT AT ZERO" note above), DerivePipelineState
                // copied those zeroes into targetFormats[] verbatim
                // (pipeline_cache.cpp:841-842), and the worker's own gate then
                // refused the key with kRefuseEmptyHlsl / "key names no render
                // target format" (pipeline_cache.cpp:1730-1747) - the
                // DRAW-CP-DRAWN=0 state measured in build/w691_err.log.
                //
                // THE PRESENCE BIT IS LOAD-BEARING, and this is the same
                // discipline kCpSnapDepthInfo uses two lines above. The trap is
                // concrete: color_format 0 is k_8_8_8_8 (xenos.h:300), a legal
                // and common render target, so a test written as "the value is
                // non-zero" would refuse every k_8_8_8_8 draw -- and a test that
                // ignored presence would build a PSO naming a target the guest
                // never declared. Neither fact may be inferred from the other.
                //
                // ONLY RT0 IS FILLED, and that is the measured limit of the
                // snapshot, not a choice: RB_COLOR1/2/3_INFO (0x2003/0x2004/
                // 0x2005) have no slot in CpSnapshotReg, so slots 1..3 of
                // colorFormats[] stay zero and the worker's numRt counts what was
                // really captured. A multi-RT draw is therefore reported as
                // numRt=1 rather than four guessed formats -- see
                // s_cpRefuseColorFmtUnmapped's row below for what happens when
                // even RT0 cannot be named.
                {
                    if (!s_noMrt && dc.rtCaptured != 0u) {
                        if (dc.rtBoundMask == 0u && dc.psProgram.present == 0u) {
                            // Depth-only pass with zero bound color RTs: leave colorFormats all 0.
                            pipeColorFormatCode = 0xFFFFFFFFu;
                            pipeColorFormatDxgi = 0u;
                            pipeRsPresent = rs.presentMask;
                        } else if (dc.rtBoundMask != 0u) {
                            bool unmappedRt0 = false;
                            bool setPipeFmt = false;
                            for (uint32_t i = 0u; i < 4u; ++i) {
                                if (!(dc.rtBoundMask & (1u << i))) continue;
                                const uint32_t code = (dc.rtColorInfoMrt[i] >> 16) & 0xFu;
                                const uint32_t dxgi = MapXenosColorFormatToDxgi(code);
                                if (dxgi == DXGI_FORMAT_UNKNOWN) {
                                    if (i == 0u) {
                                        unmappedRt0 = true;
                                        s_cpRefuseColorFmtUnmapped.fetch_add(1, std::memory_order_relaxed);
                                        const uint32_t rn = dc.cpSeq;
                                        if (rn <= 8u || (rn % 200u) == 0u) {
                                            MCLA_LOG_WARN(
                                                "CP-COLORFMT-UNMAPPED #{} drawSeq={} "
                                                "colorFormat={} rbColorInfo={:08X} rsPresent={:08X}",
                                                rn, rn, code, dc.rtColorInfoMrt[i], rs.presentMask);
                                        }
                                        refuse("color-fmt-unmapped", s_cpRefuseColorFmtUnmapped,
                                               "- RB_COLOR_INFO.color_format is a code with no DXGI_FORMAT.");
                                        break;
                                    }
                                    continue;
                                }
                                psi.colorFormats[i] = dxgi;
                                if (!setPipeFmt) {
                                    pipeColorFormatCode = code;
                                    pipeColorFormatDxgi = dxgi;
                                    pipeRsPresent = rs.presentMask;
                                    setPipeFmt = true;
                                }
                            }
                            if (unmappedRt0) break;
                        } else {
                            const uint32_t code = (dc.rtColorInfo >> 16) & 0xFu;
                            const uint32_t dxgi = MapXenosColorFormatToDxgi(code);
                            if (dxgi == DXGI_FORMAT_UNKNOWN) {
                                s_cpRefuseColorFmtUnmapped.fetch_add(1, std::memory_order_relaxed);
                                refuse("color-fmt-unmapped", s_cpRefuseColorFmtUnmapped,
                                       "- RB_COLOR_INFO.color_format has no DXGI_FORMAT.");
                                break;
                            }
                            psi.colorFormats[0] = dxgi;
                            pipeColorFormatCode = code;
                            pipeColorFormatDxgi = dxgi;
                            pipeRsPresent = rs.presentMask;
                        }
                    } else {
                    const bool colorInfoPresent =
                        dc.rtCaptured != 0u ||
                        (rs.presentMask &
                         (1u << CpSnapshotReg::kCpSnapColorInfo)) != 0u;
                    if (colorInfoPresent) {
                        const uint32_t code =
                            ((dc.rtCaptured ? dc.rtColorInfo
                                            : rs.regs[CpSnapshotReg::kCpSnapColorInfo]) >> 16) & 0xFu;
                        const uint32_t dxgi = MapXenosColorFormatToDxgi(code);
                        if (dxgi == DXGI_FORMAT_UNKNOWN) {
                            s_cpRefuseColorFmtUnmapped.fetch_add(
                                1, std::memory_order_relaxed);
                            const uint32_t rn = dc.cpSeq;
                            if (rn <= 8u || (rn % 200u) == 0u) {
                                MCLA_LOG_WARN(
                                    "CP-COLORFMT-UNMAPPED #{} drawSeq={} "
                                    "colorFormat={} (unassigned in "
                                    "xenos::ColorRenderTargetFormat, "
                                    "xenos.h:299-319) rbColorInfo={:08X} "
                                    "rsPresent={:08X} -- NO render target format "
                                    "exists for this code; NOT defaulted",
                                    rn, rn, code,
                                    rs.regs[CpSnapshotReg::kCpSnapColorInfo],
                                    rs.presentMask);
                            }
                            refuse("color-fmt-unmapped", s_cpRefuseColorFmtUnmapped,
                                   "- RB_COLOR_INFO.color_format is a code with "
                                   "no DXGI_FORMAT (8/9/11/13 are unassigned, "
                                   "xenos.h:299-319). NOT defaulted to any "
                                   "format; the draw has no render target to "
                                   "name, so no PSO can be built from it.");
                            break;
                        }
                        psi.colorFormats[0] = dxgi;
                        pipeColorFormatCode = code;
                        pipeColorFormatDxgi = dxgi;
                        pipeRsPresent = rs.presentMask;
                    } else {
                        s_cpRefuseNoColorInfo.fetch_add(1, std::memory_order_relaxed);
                        const uint32_t rn = dc.cpSeq;
                        if (rn <= 8u || (rn % 200u) == 0u) {
                            MCLA_LOG_WARN(
                                "CP-COLORFMT-NOINFO #{} drawSeq={} rsPresent={:08X}"
                                " -- RB_COLOR_INFO (0x2001) was NOT presented "
                                "before this draw, so no render target format is "
                                "the guest's. NOT defaulted",
                                rn, rn, rs.presentMask);
                        }
                        refuse("no-color-info", s_cpRefuseNoColorInfo,
                               "- the state derived and no RB_COLOR_INFO (0x2001) "
                               "was presented before this draw, so there is no "
                               "render target format to name. NOT defaulted to "
                               "any format.");
                        break;
                    }
                    }
                }

                // F-904: RT0..3 blend control + write mask, raw dwords.
                psi.blendControl0 = rs.regs[CpSnapshotReg::kCpSnapBlendControl0];
                psi.blendControl[0] = rs.regs[CpSnapshotReg::kCpSnapBlendControl0];
                psi.blendControl[1] = s_noMrt ? 0u : rs.regs[CpSnapshotReg::kCpSnapBlendControl1];
                psi.blendControl[2] = s_noMrt ? 0u : rs.regs[CpSnapshotReg::kCpSnapBlendControl2];
                psi.blendControl[3] = s_noMrt ? 0u : rs.regs[CpSnapshotReg::kCpSnapBlendControl3];
                psi.blendColorMask = rs.regs[CpSnapshotReg::kCpSnapColorMask];
                // Opaque RGB-only draws (write mask 7, blend off 0x00010001) write alpha = 1 (see XE_ALPHA1 below).
                const bool opaqueRgbOnly = (dc.rtBoundMask <= 1u || s_noMrt) &&
                    OpaqueAlpha1On() && (psi.blendColorMask & 0xFu) == 7u && psi.blendControl0 == 0x00010001u;
                if (opaqueRgbOnly) psi.blendColorMask |= 0x8u;
                const bool alphaFixApplies = opaqueRgbOnly;
                // CHARBLEND (log-only, gated): dedup census of blend states for draws that
                // write the colour target, so the in-engine cutscene character draws can be
                // named by their (mask, blend, rtBound, bound texture) instead of guessed.
                if (s_charBlend) {
                    const unsigned lowMask = static_cast<unsigned>(psi.blendColorMask & 0xFu);
                    if (lowMask != 0u) {
                        const uint32_t texBase = dc.textureCount ? dc.texturePoolVa[0] : 0u;
                        const uint64_t key = (uint64_t(lowMask) << 48) ^
                            (uint64_t(psi.blendControl0) << 16) ^
                            (uint64_t(dc.rtBoundMask) << 8) ^
                            (uint64_t(texBase >> 16) & 0xFFu);
                        static std::unordered_set<uint64_t> s_cbSeen;
                        if (s_cbSeen.size() < 8192 && s_cbSeen.insert(key).second) {
                            MCLA_LOG_ERROR("CHARBLEND mask={:#x} blend0={:#x} rtBound={} a1={} "
                                           "texBase={:08X} idx={} prim={}",
                                           lowMask, psi.blendControl0, dc.rtBoundMask,
                                           alphaFixApplies ? 1 : 0, texBase,
                                           dc.numIndices, dc.primitiveTopology);
                        }
                    }
                }

                // F-914 option (a): this draw samples the font atlas if any bound
                // texture's pool VA maps to guest base 0x13BCC000 (pool VA =
                // guest + 0xA0000000 per F-640), or baseAddress == 0x13BCC000.
                // Used to gate blend-on for text and 2D UI quads.
                {
                    bool samplesFontAtlas = false;
                    for (uint32_t _ti = 0; _ti < dc.textureCount; ++_ti) {
                        const uint32_t poolVa = dc.texturePoolVa[_ti];
                        if (poolVa == 0xB3BCC000u || poolVa == 0x13BCC000u ||
                            (poolVa & 0x1FFFFFFFu) == 0x13BCC000u) {
                            samplesFontAtlas = true;
                            break;
                        }
                        if (_ti < 4u) {
                            mcla::native::GuestTextureDesc td{};
                            if (mcla::native::DecodeGuestTextureDescriptor(
                                    dc.textureDescriptorBytes[_ti], td) && td.valid) {
                                if (td.baseAddress == 0x13BCC000u ||
                                    (td.baseAddress & 0x1FFFFFFFu) == 0x13BCC000u) {
                                    samplesFontAtlas = true;
                                    break;
                                }
                            }
                        }
                    }
                    psi.samplesFontAtlas = samplesFontAtlas;
                    psi.is2DQuad = (dc.vbStride == 36u || dc.primitiveTopology == 8u || samplesFontAtlas);
                }

                uint32_t refuseMask = mcla::renderer::kRefuseNone;
                mcla::renderer::DecodedXenosRenderState decodedRenderState;
                derived =
                    mcla::renderer::DerivePipelineState(
                        psi, refuseMask, &decodedRenderState);
                // Force colour-write mask 0 for shadow-caster draws.
                if ((pipeVsHash == 0x8864A97C3620B77AULL || pipeVsHash == 0xC668134EB77260DEULL))
                    derived.d3d12.blend.RenderTarget[0].RenderTargetWriteMask = 0;
                desc.stencilOn = pipeStencilOn;
                desc.stencilRef = pipeStencilRef;
                {   // MCLA_TEMP_HUDDRAW=1: guest state of draws in the bottom-left quarter (radar), rate-limited
                    static const int s_hud = [] { const char* e = std::getenv("MCLA_TEMP_HUDDRAW"); return e ? e[0] - '0' : 0; }();  // 1 = bottom-left draws, 2 = also every draw with z-test off (2D/UI)
                    if (s_hud) {
                        const float tw = float(backend_.GetWidth()), th = float(backend_.GetHeight());
                        float x0 = 0, y0 = 0, x1 = tw, y1 = th;
                        if (desc.guestVpValid) { x0 = desc.guestVpX; y0 = desc.guestVpY; x1 = x0 + desc.guestVpW; y1 = y0 + desc.guestVpH; }
                        if (desc.winScissorValid) { x0 = std::max(x0, float(desc.winScissorL)); y0 = std::max(y0, float(desc.winScissorT));
                                                    x1 = std::min(x1, float(desc.winScissorR)); y1 = std::min(y1, float(desc.winScissorB)); }
                        const bool bl = (x1 <= tw * 0.5f && y0 >= th * 0.5f && x1 > x0 && y1 > y0) ||
                                        (s_hud == 2 && !(dc.rasterState.regs[CpSnapshotReg::kCpSnapDepthControl] & 2u));
                        static std::atomic<uint32_t> s_n{0};
                        const uint32_t n = bl ? s_n.fetch_add(1) + 1 : 0;
                        if (bl && (n <= 300 || (n % (s_hud == 2 ? 400u : 100u)) == 0)) {
                            const auto& r = dc.rasterState.regs;
                            std::string tx;
                            for (uint32_t ti = 0; ti < dc.textureCount && ti < 4u; ++ti) {
                                mcla::native::GuestTextureDesc td{};
                                if (mcla::native::DecodeGuestTextureDescriptor(dc.textureDescriptorBytes[ti], td) && td.valid)
                                    tx += fmt::format(" t{}={}x{}fmt{}@{:08X}", ti, td.width, td.height, td.xenosFormat, td.baseAddress);
                            }
                            MCLA_LOG_WARN("HUDDRAW #{} rect=({:.0f},{:.0f})-({:.0f},{:.0f}) blend={:08X} cmask={:X} depthctl={:08X} stencilrefmask={:08X}/bf={:08X} stencilOn={} colorctl={:08X} stride={} topo={} vs={:016X} ps={:016X}{}",
                                          n, x0, y0, x1, y1, r[CpSnapshotReg::kCpSnapBlendControl0], r[CpSnapshotReg::kCpSnapColorMask] & 0xFu,
                                          r[CpSnapshotReg::kCpSnapDepthControl], r[CpSnapshotReg::kCpSnapStencilRef], r[CpSnapshotReg::kCpSnapStencilRefBF],
                                          pipeStencilOn ? 1 : 0, r[CpSnapshotReg::kCpSnapColorControl], dc.vbStride, dc.primitiveTopology,
                                          pipeVsHash, pipePsHash, tx);
                        }
                    }
                }
                applyClipDisableRescaleForDraw =
                    decodedRenderState.clPresent &&
                    decodedRenderState.clipDisable &&
                    decodedRenderState.vtePresent &&
                    !decodedRenderState.vportTransformApplied;
                if (refuseMask != mcla::renderer::kRefuseNone) {
                    s_deriveRefused.fetch_add(1, std::memory_order_relaxed);
                    // The run-wide OR of every refusal mask, so the PC-DERIVE row
                    // can print real NAMES for the whole run rather than only for
                    // whichever mask the last draw happened to produce. Lock-free
                    // fetch_or; a lost update here would only ever under-report,
                    // never invent a reason, and the per-bit histogram above is
                    // the exact authority anyway.
                    s_deriveMaskOr.fetch_or(refuseMask,
                                            std::memory_order_relaxed);
                    // Per-BIT totals, so the census is a histogram of named
                    // reasons and not one lumped "refused" number. A mask can
                    // legitimately carry several bits at once (an absent 0x2205
                    // gives kRefuseNoPaSuScModeCntl AND the poly-offset/depth
                    // consequences of a rasteriser nobody measured), and
                    // counting the mask once would understate the reasons that
                    // matter.
                    for (uint32_t bit = 0; bit < 32u; ++bit) {
                        if ((refuseMask & (1u << bit)) == 0u) continue;
                        s_deriveRefusedByBit[bit].fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    // The NAME, from the one owner of the name table. Never a
                    // bare hex code: pipeline_cache.cpp:226-234 records the guard
                    // that once made every refusal in that file print
                    // "unnamed-0x%08X", and two agents were misled by it.
                    char reasons[512] = {};
                    mcla::renderer::FormatPipelineRefusal(refuseMask, reasons,
                                                          sizeof(reasons));
                    if (dc.primitiveTopology == 0x08u) {
                        // Keep the rectangle list countable on ITS OWN counter
                        // too. This is not a second refusal of the same draw: it
                        // is the same draw, refused once, recorded under both the
                        // reason the derivation gives and the gate that has
                        // always named it - so neither marker can be lost by the
                        // reordering, and neither can be double-counted as two
                        // draws (there is exactly one `break` below).
                        s_rectListRefused.fetch_add(1, std::memory_order_relaxed);
                        refuse("prim-rect-list", s_cpRefusePrimRect,
                               "- xenos::PrimitiveType::kRectangleList "
                               "(xenos.h:53) has no D3D12 primitive type, so "
                               "DerivePipelineState refused the key with "
                               "reason(s)=");
                        // refuse() above cannot take the formatted string (it is
                        // std::string_view), so the names are printed on their own
                        // bounded row rather than being dropped. Bounded, but the
                        // HISTOGRAM above is the authority and is uncapped.
                        const uint32_t rn = dc.cpSeq;
                        if (rn <= 8u || (rn % 200u) == 0u) {
                            MCLA_LOG_WARN(
                                "PC-DERIVE-REFUSE #{} drawSeq={} prim=0x{:02X} "
                                "refuseMask=0x{:08X} reasons={} | "
                                "rsPresent={:08X} haveRegisters={}",
                                rn, rn, dc.primitiveTopology, refuseMask,
                                reasons, rs.presentMask,
                                psi.haveRegisters ? 1u : 0u);
                        }
                    } else {
                        refuse("state-refused", s_cpRefuseStateRefused,
                               "- DerivePipelineState refused the pipeline key; "
                               "the named reasons are on PC-DERIVE-REFUSE and the "
                               "per-bit totals on PC-DERIVE. NOT drawn on a "
                               "default rasteriser.");
                    }
                    // REFUSE. The draw stops here. It does not fall through to
                    // DrawDynamicMesh, because that resolves to the TEST PSO
                    // (src/d3d12_backend.cpp:2537-2539) and a test PSO against a
                    // rasteriser we could not derive is the plausible-but-wrong
                    // picture this project's charter (F-609) exists to prevent.
                    break;
                }
                s_deriveOk.fetch_add(1, std::memory_order_relaxed);
                // The derivation stamps its own provenance
                // (pipeline_cache.h:242, kStateDerivedGuest). Checking it here
                // is not ceremony: it is the ONLY thing that distinguishes "the
                // state was derived from the guest's registers" from the one
                // path where DerivePipelineState returns a ZERO state with
                // refuseMask already overwritten - the layout self-check
                // failure (pipeline_cache.cpp:804-816), which is the one case
                // where a non-zero mask is NOT set. A zero state there would
                // otherwise sail past `refuseMask != 0` and reach the draw call
                // with an empty rasteriser.
                if (derived.derivation == mcla::renderer::kStateDerivedNone) {
                    s_cpRefuseStateRefused.fetch_add(1, std::memory_order_relaxed);
                    s_deriveMaskOr.fetch_or(
                        static_cast<uint32_t>(mcla::renderer::kRefuseStateNotDerived),
                        std::memory_order_relaxed);
                    refuse("state-not-derived", s_cpRefuseStateRefused,
                           "- DerivePipelineState returned a state with no "
                           "derivation provenance (kStateDerivedNone). That is "
                           "the PC-REG-LAYOUT-BAD self-check path: the register "
                           "overlays do not decode the measured guest values, so "
                           "every shift in the derivation is wrong somewhere. "
                           "NOT drawn on the returned state.");
                    break;
                }

                // F-914 (a) REVERTED: blend override (src_alpha/inv_src_alpha) made
                // the halo invisible but also made ink invisible (UVs sample
                // transparent texels -> alpha=0 -> contribution 0). The halo fix
                // (making halo black-transparent) works with blend OFF: halo writes
                // black (invisible), ink writes white (visible). So blend stays off.
                (void)0;

                // The state derived. Two things can still legitimately stop the
                // draw, and both are NAMED rather than defaulted past:
                //   (1) no program. The command carries the programs this draw
                //       selects (F-652). A missing stage is a refusal, not a
                //       zero program address - address 0 is a readable guest page
                //       in this window, so it would be the cheapest plausible-
                //       wrong shader there is.
                //   (2) no PSO. Building one needs translated HLSL and an input
                //       layout from the guest's own program.
                //
                // F-716 REPLACED (2). It used to be an unconditional refusal with
                // the comment "until src/renderer/** lands it is the SEAM", and it
                // was: the block that follows ended in `break` on every path, which
                // made EVERY statement after the derivation scope -- the topology
                // gate, BeginFrame, the draw call and the DRAW-CP-DRAWN row itself
                // -- unreachable, so the compiler discarded them. That is measured,
                // not inferred: a probe literal inserted immediately before the
                // derivation block IS in render_thread.cpp.obj, and identical
                // probes immediately after it and at the DRAW-CP-DRAWN row are
                // NOT, at /O2 and at /Od alike, while `DRAW-CP-DRAWN` itself is
                // present in the source and absent from the shipped
                // build/mcla.exe. So the marker's zero was never "uninformative
                // until it ships": the code that prints it did not exist in the
                // binary. The chain below is what makes it exist.
                //
                // The chain, in order, ONE path, no second mechanism:
                //   code bytes (identity, already resolved) -> envelope -> parse
                //   (done in the layout block, because the VS's entry signature
                //   has to be pinned to the layout) -> TranslateShaderPinned ->
                //   ComputePipelineKeyForLayout -> GetOrCompile -> PSO.
                if (dc.vsProgram.present == 0u) {
                    s_deriveNoVsProgram.fetch_add(1, std::memory_order_relaxed);
                    refuse("no-vs-program", s_cpRefuseNoVsProgram,
                           "- the state derived, but no PM4_IM_LOAD has ever "
                           "delivered a vertex program, so there is no shader to "
                           "translate. NOT defaulted to any program.");
                    break;
                }
                // Depth-only draw: no PS and no colour channel enabled (or no bound color RT / active occlusion query).
                const bool noColorWrite =
                    (dc.rasterState.presentMask & (1u << CpSnapshotReg::kCpSnapColorMask)) != 0u &&
                    (dc.rasterState.regs[CpSnapshotReg::kCpSnapColorMask] & 0xFu) == 0u;
                const bool noBoundColorRt = !s_noMrt && (dc.rtCaptured != 0u && dc.rtBoundMask == 0u);
                const bool depthOnlyNoPs =
                    dc.psProgram.present == 0u &&
                    (noColorWrite || noBoundColorRt || backend_.HasActiveOcclusionQueries());
                if (dc.psProgram.present == 0u && !depthOnlyNoPs) {
                    s_deriveNoPsProgram.fetch_add(1, std::memory_order_relaxed);
                    refuse("no-ps-program", s_cpRefuseNoPsProgram,
                           "- the state derived, but no PM4_IM_LOAD has ever "
                           "delivered a pixel program, so there is no shader to "
                           "translate. NOT defaulted to any program.");
                    break;
                }
                {
                    // F-716: THE CHAIN. Everything the old unconditional refusal
                    // said about itself is preserved below, because it is a correct
                    // account of the OLD state and it is the reason the code after
                    // this block was dead code.
                    //
                    // WHAT THE OLD BLOCK WAS, verbatim in substance: "the state
                    // DERIVED (refuseMask == 0) and both stages name a program, but
                    // no translated HLSL / input layout exists for them yet, so
                    // there is no pipeline to bind." Its `break` is what made the
                    // refusal a refusal -- with no break this block fell THROUGH
                    // out of the derivation scope, past MapXenosPrimType and into
                    // DrawDynamicMesh(desc), so every draw reaching this gate was
                    // refused TWICE and counted twice (once as no-translated-pso
                    // and again as prim-rect-list). Measured in build/w682.log:
                    // 6/6 captured draws, refusals-per-draw exactly [2] for all
                    // six. So the `break` was correct AND the block was wrong: the
                    // chain it was standing in for now exists, so the refusal is
                    // no longer unconditional and the break is no longer free.
                    //
                    // F-653 already removed the backend-side half of the old
                    // fallback: DrawDynamicMesh no longer forwards to the test PSO,
                    // it refuses (d3d12_backend.cpp:2622-2634), and the 28-byte
                    // layout gate moved to :2674-2698 and fires only for a test PSO
                    // a caller passed deliberately. A refusal in this file still has
                    // to be a refusal HERE, and each gate below is one.

                    // GATE A -- the input layout. If this is empty there is nothing
                    // to pin the VS to and GetOrCompile would refuse it anyway
                    // (kRefuseEmptyInputLayout, pipeline_cache.cpp:2204-2208), so
                    // it is refused here where the REASON is known. `layoutRefusal`
                    // is the name of the specific gap, measured in the layout block
                    // above, not a guess.
                    if (inputLayout.empty()) {
                        { static StatCtr<uint32_t> s_tp{0}; const uint32_t q = ++s_tp;  // TEMP-GATEA
                          if (q <= 6 || q % 100000 == 0)
                            MCLA_LOG_WARN("TEMP-GATEA #{} draw={} psPresent={} psBytes={} vsBytes={} vsElem={} vbElemCnt={} refusal={} layFromVE={} parsed={}",
                                q, 0, dc.psProgram.present, pipePsCodeBytes, pipeVsCodeBytes, pipeVsElements, dc.vbElementCount, layoutRefusal, s_pipeParsed.load(), pipeVsInstructions); } 
                        pipeStop = layoutRefusal;
                        pipeRow();
                        s_deriveNoTranslatedPso.fetch_add(1, std::memory_order_relaxed);
                        refuse("no-translated-pso", s_cpRefuseNoPso,
                               "- the state DERIVED (refuseMask == 0) and both stages "
                               "named a program, and the program's OWN code was "
                               "recovered and parsed, but no D3D12 input layout could "
                               "be derived from it, so there is no pipeline to bind. "
                               "See DRAW-CP-PIPE for the gate that stopped it. NOT "
                               "drawn on any substitute: the break ends the draw "
                               "here, so this command reaches neither "
                               "MapXenosPrimType nor any draw call and is never read "
                               "through an input layout the guest never declared.");
                        break;
                    }

                    // GATE B -- the vertex shader, TRANSLATED, with its entry
                    // signature PINNED to the layout above. Pinned, not free: with
                    // a pin the signature can only use semantics the input
                    // assembler actually supplies (shader_translator.h:77-97),
                    // which is what removes the second naming that F-609 item 4
                    // describes. `TranslateShaderPinned` is the required entry
                    // point and the ONLY one used here; `TranslateShader` (the dead
                    // path's, :803) is not, because it would re-derive semantics
                    // from a fetch walk and could disagree with the layout.
                    {
                        // Envelopes come from the layout gate (same program bytes, same BuildProgramEnvelope
                        // arguments). Rebuild only if that gate did not produce them (INV_PERF2 PATCH 3).
                        if (vsEnvBytes == 0u || (!depthOnlyNoPs && psEnvBytes == 0u)) {
                            vsCode.clear(); psCode.clear(); vsEnv.clear(); psEnv.clear();
                            vsEnvPtr = nullptr; vsEnvBytes = 0u;
                            psEnvPtr = nullptr; psEnvBytes = 0u;
                            memoVsValid = memoPsValid = false;  // P5: bytes replaced; any memo snapshot is void
                            const char *vsSrc = "absent";
                            (void)ResolveStageCode(dc.vsProgram, vsCode, vsSrc);
                            // TEMP-REPLAY: keep a copy while the raw microcode is resident.
                            {
                                static const bool s_on = [] {
                                    const char* e = std::getenv("MCLA_TEMP_DUMP_VSALL");
                                    return e && e[0] == '1';
                                }();
                                if (s_on) tl_vsCodeDump = vsCode;
                            }
                            const char *psSrc = "absent";
                            (void)ResolveStageCode(dc.psProgram, psCode, psSrc);
                            if (BuildProgramEnvelope(vsCode.data(), vsCode.size(), true, vsEnv) &&
                                (depthOnlyNoPs ||
                                 BuildProgramEnvelope(psCode.data(), psCode.size(), false, psEnv))) {
                                vsEnvPtr = vsEnv.data();
                                vsEnvBytes = vsEnv.size();
                                psEnvPtr = psEnv.data();
                                psEnvBytes = psEnv.size();
                            } else {
                                vsEnv.clear();  // makes the refusal below fire, as before
                                vsEnvPtr = nullptr;
                                vsEnvBytes = 0u;
                            }
                        }
                        if (vsEnvBytes == 0u || (!depthOnlyNoPs && psEnvBytes == 0u)) {
                            pipeStop = "envelope-refused";
                            pipeRow();
                            refuse("no-translated-pso", s_cpRefuseNoPso,
                                   "- a stage had no usable program bytes to wrap. "
                                   "See DRAW-CP-PIPE. NOT translated from nothing.");
                            break;
                        }
                        struct CachedModHlsl {
                            std::shared_ptr<const std::string> hlsl;
                            uint64_t hlslHash = 0;
                        };
                        auto mixHlslHash64 = [](uint64_t h, uint64_t a, uint64_t b) -> uint64_t {
                            uint64_t x = h ^ (a * 0x9E3779B97F4A7C15ULL + 0xBF58476D1CE4E5B9ULL);
                            x ^= (x >> 30);
                            x *= 0xBF58476D1CE4E5B9ULL;
                            x ^= b * 0x94D049BB133111EBULL;
                            x ^= (x >> 27);
                            x *= 0x94D049BB133111EBULL;
                            x ^= (x >> 31);
                            return x ? x : 1ULL;
                        };
                        struct VsModKey {
                            uint64_t vsCacheKey = 0;
                            uint32_t modMask = 0;
                            bool operator==(const VsModKey& o) const {
                                return vsCacheKey == o.vsCacheKey && modMask == o.modMask;
                            }
                        };
                        struct VsModKeyHash {
                            size_t operator()(const VsModKey& k) const {
                                uint64_t x = k.vsCacheKey ^ (static_cast<uint64_t>(k.modMask) * 0x9E3779B97F4A7C15ULL);
                                x ^= (x >> 33);
                                x *= 0xff51afd7ed558ccdULL;
                                x ^= (x >> 33);
                                return static_cast<size_t>(x);
                            }
                        };
                        pipeTrStage = "vs";
                        STALL_STAGE("translate-vs");
                        // Shader translation cache (thread-safe shared_mutex + shared_ptr for MT Stage 1).
                        static std::shared_mutex s_vsCacheMtx;
                        static std::unordered_map<uint64_t, std::shared_ptr<const mcla::renderer::TranslatedShader>> s_vsCache;
                        static std::atomic<uint32_t> s_vsCacheGen{0};
                        static std::atomic<uint32_t> s_vsModCacheGen{0};
                        // P5: reuse the hash cached with the envelope bytes when the
                        // (program, generation) key still matches; otherwise recompute.
                        uint64_t vsCacheKey;
                        if (s_envMemoOn && memoVsValid && envMemoCurPtr &&
                            memoProgPtr == envMemoCurPtr && memoGen == s_vpCacheGen) {
                            vsCacheKey = memoVsEnvKeyHash;
                            NoteEnvMemo(true);
                        } else {
                            vsCacheKey = EnvelopeKeyHashBytes(vsEnvPtr, vsEnvBytes, 0xcbf29ce484222325ULL);
                            NoteEnvMemo(false);
                        }
                        vsCacheKey ^= inputDeclHash;
                        vsCacheKey *= 0x100000001b3ULL;
                        vsCacheKey ^= (applyClipDisableRescaleForDraw ? 1ULL : 0ULL);
                        vsCacheKey *= 0x100000001b3ULL;
                        uint32_t clipExtW = 0, clipExtH = 0;  // per-draw guest RT extent baked into the rescale
                        // MCLA_NO_CLIPEXT=1: A/B off-switch, restores the window-extent bake (pre-MD-33).
                        static const bool s_noClipExt = [] { const char* e = std::getenv("MCLA_NO_CLIPEXT"); return e && e[0] == '1'; }();
                        if (!s_noClipExt && applyClipDisableRescaleForDraw && dc.rtCaptured && (dc.rtSurfaceInfo & 0x3FFFu) != 0u) {
                            clipExtW = dc.rtSurfaceInfo & 0x3FFFu;
                            const uint32_t br = (dc.rtScissorBr >> 16) & 0x7FFFu;  // same rule as ComputeGuestPassKeys
                            clipExtH = (br > 0u && br <= 4096u) ? br : 720u;
                            vsCacheKey ^= (uint64_t(clipExtW) << 32) | clipExtH;
                            vsCacheKey *= 0x100000001b3ULL;
                        }
                        desc.clipExtW = clipExtW; desc.clipExtH = clipExtH;
                        static std::unordered_map<uint64_t, uint64_t> s_vsHlslHash;  // MCLA_PSO_HLSLHASH: vsCacheKey -> HashShaderBytecode(hlsl)
                        static std::unordered_map<VsModKey, CachedModHlsl, VsModKeyHash> s_vsModCache;
                        struct VsL1Ent {
                            bool occupied = false;
                            uint64_t key = 0;
                            uint32_t gen = 0;
                            uint64_t hlslHash = 0;
                            std::shared_ptr<const mcla::renderer::TranslatedShader> sp;
                        };
                        static thread_local VsL1Ent s_vsL1[64];
                        const uint32_t vsGenSnap = s_vsCacheGen.load(std::memory_order_acquire);
                        VsL1Ent& vsL1Slot = s_vsL1[(vsCacheKey ^ (vsCacheKey >> 16)) & 63u];
                        if (vsL1Slot.occupied && vsL1Slot.key == vsCacheKey && vsL1Slot.gen == vsGenSnap) {
                            pipeVsHold = vsL1Slot.sp;
                            if (g_psoHlslHash) pipeVsHlslHash = vsL1Slot.hlslHash;
                        } else {
                            {
                                std::shared_lock<std::shared_mutex> rlk(s_vsCacheMtx);
                                auto vsCacheIt = s_vsCache.find(vsCacheKey);
                                if (vsCacheIt != s_vsCache.end()) {
                                    pipeVsHold = vsCacheIt->second;
                                    if (g_psoHlslHash) {
                                        auto hh = s_vsHlslHash.find(vsCacheKey);
                                        if (hh != s_vsHlslHash.end()) pipeVsHlslHash = hh->second;
                                    }
                                    vsL1Slot = {true, vsCacheKey, vsGenSnap, pipeVsHlslHash, pipeVsHold};
                                }
                            }
                            if (!pipeVsHold) {
                                mcla::renderer::TranslatedShader vsMiss;
                                const int64_t xlT0 = g_tempPerfStages ? PerfNowNs() : 0;
                                const bool vsXlOk = mcla::renderer::TranslateShaderPinned(
                                        vsEnvPtr, vsEnvBytes, pinnedLayout.data(),
                                        pinnedLayout.size(), vsMiss, pipeTranslateError,
                                        applyClipDisableRescaleForDraw, nullptr, clipExtW, clipExtH);
                                if (g_tempPerfStages) PerfNoteXlate(0, PerfNowNs() - xlT0);
                                if (!vsXlOk) {
                                    pipeStop = "vs-translate-refused";
                                    pipeRow();
                                    refuse("no-translated-pso", s_cpRefuseNoPso,
                                           "- TranslateShaderPinned refused the vertex "
                                           "shader. See DRAW-CP-PIPE. NOT drawn on a "
                                           "default shader.");
                                    break;
                                }
                                if (clipExtW) {
                                    static std::atomic<uint32_t> s_clipExtLog{0};
                                    const uint32_t n = s_clipExtLog.fetch_add(1, std::memory_order_relaxed);
                                    if (n < 16 || (n % 1000) == 0)
                                        MCLA_LOG_INFO("CLIPEXT-BAKED vs={:016X} ext={}x{} scale={}", vsMiss.programHash, clipExtW, clipExtH, 2.0f / clipExtW);
                                }
                                auto sp = std::make_shared<const mcla::renderer::TranslatedShader>(std::move(vsMiss));
                                const uint64_t hlslHash = g_psoHlslHash ? mcla::renderer::HashShaderBytecode(sp->hlsl) : 0ull;
                                std::unique_lock<std::shared_mutex> wlk(s_vsCacheMtx);
                                auto vsCacheIt = s_vsCache.find(vsCacheKey);
                                if (vsCacheIt == s_vsCache.end()) {
                                    if (s_vsCache.size() >= 4096) {
                                        s_vsCache.clear(); s_vsHlslHash.clear(); s_vsModCache.clear();
                                        s_vsCacheGen.fetch_add(1u, std::memory_order_release);
                                        s_vsModCacheGen.fetch_add(1u, std::memory_order_release);
                                    }
                                    vsCacheIt = s_vsCache.emplace(vsCacheKey, sp).first;
                                    if (g_psoHlslHash) s_vsHlslHash[vsCacheKey] = hlslHash;
                                    g_memVsCache.store(s_vsCache.size(), std::memory_order_relaxed);
                                }
                                pipeVsHold = vsCacheIt->second;
                                if (g_psoHlslHash) {
                                    auto hh = s_vsHlslHash.find(vsCacheKey);
                                    if (hh != s_vsHlslHash.end()) pipeVsHlslHash = hh->second;
                                }
                                vsL1Slot = {true, vsCacheKey, s_vsCacheGen.load(std::memory_order_relaxed), pipeVsHlslHash, pipeVsHold};
                            }
                        }
                        const mcla::renderer::TranslatedShader& vsOut = *pipeVsHold;
                        s_pipeTranslatedVs.fetch_add(1, std::memory_order_relaxed);
                        pipeVsHlsl = &vsOut.hlsl;
                        pipeVsHash = vsOut.programHash;
                        // Force colour-write mask 0 for shadow-caster draws
                        if ((vsOut.programHash == 0x8864A97C3620B77AULL || vsOut.programHash == 0xC668134EB77260DEULL))
                            derived.d3d12.blend.RenderTarget[0].RenderTargetWriteMask = 0;
                        const bool vsRawVb = desc.rawVb;
                        if (vsRawVb) {
                            pipeVsHash ^= 0x7A7A7A7A7A7A7A7AULL;
                            static StatCtr<uint32_t> s_rawN{0};
                            const uint32_t rn = s_rawN.fetch_add(1, std::memory_order_relaxed) + 1u;
                            if (rn <= 20u || (rn % 5000u) == 0u)
                                MCLA_LOG_WARN("RAWVB-DRAW #{} numIdx={} vbSize={} stride={} vs={:016X}", rn,
                                              dc.numIndices, dc.vbSize, dc.vbStride, pipeVsHash);
                        }
                        const uint32_t vsModMask = (vsRawVb ? 4u : 0u) | (vpYFlipped ? 8u : 0u);
                        if (vsModMask != 0u) {
                            const VsModKey modKey{vsCacheKey, vsModMask};
                            struct VsModL1Ent {
                                bool occupied = false;
                                VsModKey key{};
                                uint32_t gen = 0;
                                CachedModHlsl val{};
                            };
                            static thread_local VsModL1Ent s_vsModL1[32];
                            const uint32_t modGenSnap = s_vsModCacheGen.load(std::memory_order_acquire);
                            VsModL1Ent& modL1Slot = s_vsModL1[(VsModKeyHash{}(modKey)) & 31u];
                            if (modL1Slot.occupied && modL1Slot.key == modKey && modL1Slot.gen == modGenSnap) {
                                pipeVsModHold = modL1Slot.val.hlsl;
                                pipeVsHlslHash = modL1Slot.val.hlslHash;
                            } else {
                                {
                                    std::shared_lock<std::shared_mutex> rlk(s_vsCacheMtx);
                                    auto modIt = s_vsModCache.find(modKey);
                                    if (modIt != s_vsModCache.end()) {
                                        pipeVsModHold = modIt->second.hlsl;
                                        pipeVsHlslHash = modIt->second.hlslHash;
                                        modL1Slot = {true, modKey, modGenSnap, modIt->second};
                                    }
                                }
                                if (!pipeVsModHold) {
                                    std::string prefix;
                                    if (vsRawVb) prefix += "#define XE_RAWVB 1\n";
                                    if (vpYFlipped) prefix += "#define XE_YFLIP 1\n";
                                    std::string built;
                                    built.reserve(prefix.size() + vsOut.hlsl.size());
                                    built.append(prefix);
                                    built.append(vsOut.hlsl);
                                    CachedModHlsl entry;
                                    entry.hlsl = std::make_shared<const std::string>(std::move(built));
                                    entry.hlslHash = (pipeVsHlslHash != 0)
                                        ? mixHlslHash64(pipeVsHlslHash, static_cast<uint64_t>(vsModMask), 0ULL)
                                        : 0ULL;
                                    std::unique_lock<std::shared_mutex> wlk(s_vsCacheMtx);
                                    auto modIt = s_vsModCache.find(modKey);
                                    if (modIt == s_vsModCache.end()) {
                                        if (s_vsModCache.size() >= 4096) {
                                            s_vsModCache.clear();
                                            s_vsModCacheGen.fetch_add(1u, std::memory_order_release);
                                        }
                                        modIt = s_vsModCache.emplace(modKey, std::move(entry)).first;
                                    }
                                    pipeVsModHold = modIt->second.hlsl;
                                    pipeVsHlslHash = modIt->second.hlslHash;
                                    modL1Slot = {true, modKey, s_vsModCacheGen.load(std::memory_order_relaxed), modIt->second};
                                }
                            }
                            pipeVsHlsl = pipeVsModHold.get();
                        }
                        pipeVsPrecompiledAvail = vsOut.precompiledAvailable;
                        pipeVsPrecompiledHash = vsOut.precompiledCacheHash;
                        // TEMP-REPLAY restore (2026-11-20): raw VS microcode + translated HLSL.
                        static const bool s_dumpVs = [] {
                            const char* e = std::getenv("MCLA_TEMP_DUMP_VSALL");
                            return e && e[0] == '1';
                        }();
                        static const uint64_t s_dumpVsHash = [] {
                            const char* e = std::getenv("MCLA_DUMP_VS_HASH");
                            return e && *e ? std::strtoull(e, nullptr, 16) : 0ULL;
                        }();
                        if ((s_dumpVs || (s_dumpVsHash != 0 && pipeVsHash == s_dumpVsHash)) &&
                            pipeVsHash != 0u) {
                            std::filesystem::create_directories("vs_hlsl");
                            char fn[128];
                            std::snprintf(fn, sizeof fn, "vs_hlsl/%016llX.vs.bin", (unsigned long long)pipeVsHash);
                            if (std::FILE* f = std::fopen(fn, "wb")) {
                                std::fwrite(tl_vsCodeDump.data(), 1, tl_vsCodeDump.size(), f); std::fclose(f);
                            }
                            std::snprintf(fn, sizeof fn, "vs_hlsl/%016llX.vs.hlsl", (unsigned long long)pipeVsHash);
                            if (std::FILE* f = std::fopen(fn, "wb")) {
                                std::fwrite(pipeVsHlsl->data(), 1, pipeVsHlsl->size(), f); std::fclose(f);
                            }
                            if (s_dumpVsHash != 0 && pipeVsHash == s_dumpVsHash)
                                MCLA_LOG_WARN("DUMP-VS-HIT hash={:016X} bytes={}", pipeVsHash,
                                              pipeVsHlsl->size());
                        }
                        struct PsAtKey {
                            uint64_t psCacheKey = 0;
                            uint32_t func = 0;
                            uint32_t arefRaw = 0;
                            uint32_t flags = 0;
                            bool operator==(const PsAtKey& o) const {
                                return psCacheKey == o.psCacheKey && func == o.func &&
                                       arefRaw == o.arefRaw && flags == o.flags;
                            }
                        };
                        struct PsAtKeyHash {
                            size_t operator()(const PsAtKey& k) const {
                                uint64_t x = k.psCacheKey ^ (static_cast<uint64_t>(k.func) * 0x9E3779B97F4A7C15ULL);
                                x ^= (x >> 30);
                                x *= 0xBF58476D1CE4E5B9ULL;
                                x ^= static_cast<uint64_t>(k.arefRaw) * 0x94D049BB133111EBULL;
                                x ^= static_cast<uint64_t>(k.flags) * 0xD6E8FEB86659FD93ULL;
                                x ^= (x >> 31);
                                return static_cast<size_t>(x);
                            }
                        };
                        static const mcla::renderer::TranslatedShader kEmptyPsShader{};
                        const mcla::renderer::TranslatedShader* psOutP = &kEmptyPsShader;
                        uint64_t psCacheKey = 0;
                        pipeTrStage = "ps";
                        static std::shared_mutex s_psCacheMtx;
                        static std::unordered_map<PsAtKey, CachedModHlsl, PsAtKeyHash> s_psAtCache;
                        static std::atomic<uint32_t> s_psCacheGen{0};
                        static std::atomic<uint32_t> s_psAtCacheGen{0};
                        if (!depthOnlyNoPs) {
                            const uint32_t psNumReg = ReadPsNumRegField();
                            static std::unordered_map<uint64_t, std::shared_ptr<const mcla::renderer::TranslatedShader>> s_psCache;
                            static std::unordered_map<uint64_t, uint64_t> s_psHlslHash;  // MCLA_PSO_HLSLHASH: psCacheKey -> HashShaderBytecode(hlsl)
                            // P5: same memo as VS; keyed on the same (program, generation).
                            if (s_envMemoOn && memoPsValid && envMemoCurPtr &&
                                memoProgPtr == envMemoCurPtr && memoGen == s_vpCacheGen) {
                                psCacheKey = memoPsEnvKeyHash;
                                NoteEnvMemo(true);
                            } else {
                                psCacheKey = EnvelopeKeyHashBytes(psEnvPtr, psEnvBytes, 0xcbf29ce484222325ULL);
                                NoteEnvMemo(false);
                            }
                            psCacheKey ^= pipeVsHash;
                            psCacheKey *= 0x100000001b3ULL;
                            psCacheKey ^= static_cast<uint64_t>(psNumReg);
                            psCacheKey *= 0x100000001b3ULL;
                            // MD-26 rt7e3 clamp mask (RT n colour format 3 or 12, MapXenosColorFormatToDxgi) and
                            // MD-14 param_gen (SQ_PROGRAM_CNTL bit 18 + SQ_CONTEXT_MISC bits 15:8): per-draw HLSL
                            // variants, so they are part of the key. MCLA_NO_RT7E3_CLAMP / MCLA_NO_PARAM_GEN
                            // (also read by the translator) leave the extras at their defaults.
                            mcla::renderer::ShaderTranslateExtras psExtras;
                            {
                                static const bool s_noRt7e3 = [] { const char* e = std::getenv("MCLA_NO_RT7E3_CLAMP"); return e && e[0] == '1'; }();
                                static const bool s_noPgen = [] { const char* e = std::getenv("MCLA_NO_PARAM_GEN"); return e && e[0] == '1'; }();
                                if (!s_noRt7e3) {
                                    for (uint32_t i = 0; i < 4u; ++i) {
                                        const uint32_t info = (i == 0u) ? (dc.rtColorInfoMrt[0] ? dc.rtColorInfoMrt[0] : dc.rtColorInfo)
                                                                         : ((dc.rtBoundMask >> i) & 1u ? dc.rtColorInfoMrt[i] : 0u);
                                        const uint32_t fmt = (info >> 16) & 0xFu;
                                        if (fmt == 3u || fmt == 12u) {
                                            psExtras.rt7e3Mask |= 1u << i;
                                            // color_exp_bias bits 20..25 signed (same decode as RESOLVE-EXP-BIAS rtBias); off: MCLA_NO_RT7E3_BIAS=1
                                            static const bool s_noBias = [] { const char* e = std::getenv("MCLA_NO_RT7E3_BIAS"); return e && e[0] == '1'; }();
                                            if (!s_noBias) psExtras.rtExpBias[i] = int32_t(info << 6) >> 26;
                                        }
                                    }
                                }
                                if (!s_noPgen && (dc.extraRegPresent & 2u)) {
                                    // SQ_PROGRAM_CNTL (0x2180) is the CP register file's global value, same read as
                                    // ReadPsNumRegField (not snapshotted per draw).
                                    constexpr uint32_t kIdx[1] = {0x2180u};
                                    uint32_t val[1] = {0}, pres[1] = {0};
                                    (void)mcla::gpu::CpReadGuestRegisters(kIdx, 1u, val, pres);
                                    const uint32_t pgReg = (dc.sqContextMisc >> 8) & 0xFFu;
                                    if (pres[0] && ((val[0] >> 18) & 1u) && pgReg < 16u) {
                                        psExtras.paramGenReg = static_cast<int32_t>(pgReg);
                                        const uint32_t pt = dc.primitiveTopology;  // xenos::PrimitiveType: 1 point, 2/3/12 line
                                        psExtras.paramGenPrim = (pt == 1u) ? 2u : (pt == 2u || pt == 3u || pt == 12u) ? 1u : 0u;
                                        static std::atomic<uint32_t> s_pgenLog{0};
                                        const uint32_t pn = s_pgenLog.fetch_add(1, std::memory_order_relaxed);
                                        if (pn < 8u || (pn % 20000u) == 0u)
                                            MCLA_LOG_INFO("PARAMGEN-USED #{} reg={} prim={} programCntl={:08X} sqMisc={:08X}",
                                                          pn + 1u, pgReg, psExtras.paramGenPrim, val[0], dc.sqContextMisc);
                                    }
                                }
                                // MD-17: fetch constants bound to a render-scaled resolve get XE_RS_<n> = scale (off: MCLA_NO_XUV_RS=1).
                                psExtras.xuvRsMask = backend_.XuvRsMask(guestTextures.data(), static_cast<uint32_t>(guestTextures.size()), psExtras.xuvRsScale);
                                psCacheKey ^= (uint64_t(psExtras.rt7e3Mask) << 40) ^ (uint64_t(psExtras.paramGenReg + 1) << 48) ^
                                              (uint64_t(psExtras.paramGenPrim) << 56);
                                for (uint32_t i = 0; i < 4u; ++i)  // zero bias leaves the key unchanged
                                    if (psExtras.rtExpBias[i]) { psCacheKey *= 0x100000001b3ULL; psCacheKey ^= (uint64_t(uint8_t(psExtras.rtExpBias[i])) << 8) | i; }
                                if (psExtras.xuvRsMask) {  // key unchanged when no slot is scaled
                                    psCacheKey *= 0x100000001b3ULL;
                                    psCacheKey ^= (uint64_t(psExtras.xuvRsMask) << 8) ^ uint64_t(psExtras.xuvRsScale);
                                }
                                psCacheKey *= 0x100000001b3ULL;
                            }
                            struct PsL1Ent {
                                bool occupied = false;
                                uint64_t key = 0;
                                uint32_t gen = 0;
                                uint64_t hlslHash = 0;
                                std::shared_ptr<const mcla::renderer::TranslatedShader> sp;
                            };
                            static thread_local PsL1Ent s_psL1[64];
                            const uint32_t psGenSnap = s_psCacheGen.load(std::memory_order_acquire);
                            PsL1Ent& psL1Slot = s_psL1[(psCacheKey ^ (psCacheKey >> 16)) & 63u];
                            if (psL1Slot.occupied && psL1Slot.key == psCacheKey && psL1Slot.gen == psGenSnap) {
                                pipePsHold = psL1Slot.sp;
                                if (g_psoHlslHash) pipePsHlslHash = psL1Slot.hlslHash;
                            } else {
                                {
                                    std::shared_lock<std::shared_mutex> rlk(s_psCacheMtx);
                                    auto psCacheIt = s_psCache.find(psCacheKey);
                                    if (psCacheIt != s_psCache.end()) {
                                        pipePsHold = psCacheIt->second;
                                        if (g_psoHlslHash) {
                                            auto hh = s_psHlslHash.find(psCacheKey);
                                            if (hh != s_psHlslHash.end()) pipePsHlslHash = hh->second;
                                        }
                                        psL1Slot = {true, psCacheKey, psGenSnap, pipePsHlslHash, pipePsHold};
                                    }
                                }
                                if (!pipePsHold) {
                                    mcla::renderer::TranslatedShader psMiss;
                                    mcla::renderer::PixelShaderPairing psPairing;
                                    psPairing.vsContainer = vsEnvPtr;
                                    psPairing.vsContainerSize = vsEnvBytes;
                                    psPairing.psNumReg = psNumReg;
                                    const int64_t xlT0 = g_tempPerfStages ? PerfNowNs() : 0;
                                    const bool psXlOk = mcla::renderer::TranslateShaderPinned(
                                            psEnvPtr, psEnvBytes, nullptr, 0, psMiss,
                                            pipeTranslateError, false, &psPairing, 0, 0, &psExtras);
                                    if (g_tempPerfStages) PerfNoteXlate(1, PerfNowNs() - xlT0);
                                    if (!psXlOk) {
                                        pipeStop = "ps-translate-refused";
                                        pipeRow();
                                        refuse("no-translated-pso", s_cpRefuseNoPso,
                                               "- TranslateShaderPinned refused the pixel "
                                               "shader. See DRAW-CP-PIPE. NOT drawn on a "
                                               "default shader.");
                                        break;
                                    }
                                    auto sp = std::make_shared<const mcla::renderer::TranslatedShader>(std::move(psMiss));
                                    const uint64_t hlslHash = g_psoHlslHash ? mcla::renderer::HashShaderBytecode(sp->hlsl) : 0ull;
                                    std::unique_lock<std::shared_mutex> wlk(s_psCacheMtx);
                                    auto psCacheIt = s_psCache.find(psCacheKey);
                                    if (psCacheIt == s_psCache.end()) {
                                        if (s_psCache.size() >= 4096) {
                                            s_psCache.clear(); s_psHlslHash.clear(); s_psAtCache.clear();
                                            s_psCacheGen.fetch_add(1u, std::memory_order_release);
                                            s_psAtCacheGen.fetch_add(1u, std::memory_order_release);
                                        }
                                        psCacheIt = s_psCache.emplace(psCacheKey, sp).first;
                                        if (g_psoHlslHash) s_psHlslHash[psCacheKey] = hlslHash;
                                        g_memPsCache.store(s_psCache.size(), std::memory_order_relaxed);
                                    }
                                    pipePsHold = psCacheIt->second;
                                    if (g_psoHlslHash) {
                                        auto hh = s_psHlslHash.find(psCacheKey);
                                        if (hh != s_psHlslHash.end()) pipePsHlslHash = hh->second;
                                    }
                                    psL1Slot = {true, psCacheKey, s_psCacheGen.load(std::memory_order_relaxed), pipePsHlslHash, pipePsHold};
                                }
                            }
                            psOutP = pipePsHold.get();
                        }  // !depthOnlyNoPs
                        const mcla::renderer::TranslatedShader& psOut = *psOutP;
                        s_pipeTranslatedPs.fetch_add(1, std::memory_order_relaxed);
                        pipePsHlsl = &psOut.hlsl;
                        {   // TONE-CENSUS (MCLA_TONE_CENSUS=1): first-seen (ps program, RB_COLOR_INFO[0..3]) tuples, state only.
                            // MCLA_DUMP_PS_HASH=<hex programHash>: write that PS's translated HLSL to ps_hlsl/ once.
                            static const bool s_toneCensus = [] { const char* e = std::getenv("MCLA_TONE_CENSUS"); return e && e[0] == '1'; }();
                            static const uint64_t s_dumpPsHash = [] { const char* e = std::getenv("MCLA_DUMP_PS_HASH"); return e && *e ? std::strtoull(e, nullptr, 16) : 0ULL; }();
                            if (s_toneCensus) {
                                uint32_t ci[4];
                                for (uint32_t i = 0; i < 4u; ++i)
                                    ci[i] = (i == 0u) ? (dc.rtColorInfoMrt[0] ? dc.rtColorInfoMrt[0] : dc.rtColorInfo)
                                                      : ((dc.rtBoundMask >> i) & 1u ? dc.rtColorInfoMrt[i] : 0u);
                                const uint64_t k = psOut.programHash ^ (uint64_t(ci[0]) * 0x9E3779B97F4A7C15ULL) ^ (uint64_t(ci[1]) << 7) ^ (uint64_t(ci[2]) << 13) ^ (uint64_t(ci[3]) << 19);
                                static std::mutex s_tcMu;
                                static std::unordered_set<uint64_t> s_tcSeen;
                                std::lock_guard<std::mutex> lk(s_tcMu);
                                if (s_tcSeen.size() < 600 && s_tcSeen.insert(k).second)
                                    MCLA_LOG_WARN("TONE-CENSUS ps={:016X} rt0=fmt{}/bias{}/base{:03X} rt1=fmt{}/bias{} rt2=fmt{}/bias{} rt3=fmt{}/bias{} mask={:X} n={}",
                                                  psOut.programHash, (ci[0] >> 16) & 0xFu, int32_t(ci[0] << 6) >> 26, ci[0] & 0xFFFu,
                                                  (ci[1] >> 16) & 0xFu, int32_t(ci[1] << 6) >> 26, (ci[2] >> 16) & 0xFu, int32_t(ci[2] << 6) >> 26,
                                                  (ci[3] >> 16) & 0xFu, int32_t(ci[3] << 6) >> 26, dc.rtBoundMask, s_tcSeen.size());
                            }
                            if (s_dumpPsHash != 0 && psOut.programHash == s_dumpPsHash) {
                                static std::atomic<bool> s_dumped{false};
                                if (!s_dumped.exchange(true)) {
                                    std::error_code ec; std::filesystem::create_directories("ps_hlsl", ec);
                                    char fn[128];
                                    std::snprintf(fn, sizeof fn, "ps_hlsl/%016llX.ps.hlsl", (unsigned long long)s_dumpPsHash);
                                    if (std::FILE* f = std::fopen(fn, "wb")) { std::fwrite(psOut.hlsl.data(), 1, psOut.hlsl.size(), f); std::fclose(f); }
                                }
                            }
                        }
                        {   // Alpha test, alpha-to-mask, and MIN/MAX blend SrcAlpha premultiply.
                            const auto& ars = dc.rasterState;
                            const bool have = (ars.presentMask >> mcla::native::CpSnapshotReg::kCpSnapColorControl) & 1u;
                            const uint32_t cc = have ? ars.regs[mcla::native::CpSnapshotReg::kCpSnapColorControl] : 0u;
                            const uint32_t func = cc & 7u;
                            const uint32_t arefRaw = ars.regs[mcla::native::CpSnapshotReg::kCpSnapAlphaRef];
                            float aref = 0.0f;
                            std::memcpy(&aref, &arefRaw, 4);
                            static const bool s_noAlphaToMask = [] {
                                const char* e = std::getenv("MCLA_NO_ALPHA_TO_MASK");
                                return e && e[0] == '1';
                            }();
                            const bool alphaTestOn = have && ((cc >> 3) & 1u) && func != 7u && std::isfinite(aref);
                            const bool alphaToMaskOn = !s_noAlphaToMask && have && (((cc >> 4) & 1u) != 0u) &&
                                                       (derived.sampleCount <= 1u);
                            const uint32_t alphaToMaskOffsets = alphaToMaskOn ? ((cc >> 24) & 0xFFu) : 0u;
                            const bool minMaxRgb = decodedRenderState.minMaxPremulRgb;
                            const bool minMaxAlpha = decodedRenderState.minMaxPremulAlpha;
                            // MCLA_ALPHA_JOIN=1 (2026-11-20): log alpha state per PS so a
                            // foliage draw can be joined to its discard behaviour. Log-only.
                            {
                                static const bool s_alphaJoin = [] {
                                    const char* e = std::getenv("MCLA_ALPHA_JOIN");
                                    return e && e[0] == '1';
                                }();
                                if (s_alphaJoin) {
                                    // Shared cadence with FOLIAGE-BIND (draw index % 100)
                                    // so the two logs can be joined by draw number.
                                    if (n <= 20u || (n % 100u) == 0u)
                                        MCLA_LOG_ERROR("ALPHA-JOIN draw={} ps={:016X} cc={:08X} func={} aref={:.6g} "
                                                       "test={} a2m={} minmax={}{} have={}",
                                                       n, psOut.programHash, cc, func, aref,
                                                       alphaTestOn ? 1u : 0u, alphaToMaskOn ? 1u : 0u,
                                                       minMaxRgb ? 1u : 0u, minMaxAlpha ? 1u : 0u,
                                                       have ? 1u : 0u);
                                }
                            }
                            if (!pipePsHlsl->empty() && (alphaTestOn || alphaToMaskOn || minMaxRgb || minMaxAlpha))
                            {
                                const uint32_t psFlags = (alphaTestOn ? 1u : 0u) |
                                                         (alphaToMaskOn ? 2u : 0u) |
                                                         (minMaxRgb ? 4u : 0u) |
                                                         (minMaxAlpha ? 8u : 0u) |
                                                         (alphaToMaskOffsets << 8);
                                const PsAtKey atKey{
                                    psCacheKey,
                                    alphaTestOn ? func : 7u,
                                    alphaTestOn ? arefRaw : 0u,
                                    psFlags};
                                std::shared_ptr<const std::string> atHold;
                                uint64_t atHash = 0;
                                struct PsAtL1Ent {
                                    bool occupied = false;
                                    PsAtKey key{};
                                    uint32_t gen = 0;
                                    CachedModHlsl val{};
                                };
                                static thread_local PsAtL1Ent s_psAtL1[64];
                                const uint32_t atGenSnap = s_psAtCacheGen.load(std::memory_order_acquire);
                                PsAtL1Ent& atL1Slot = s_psAtL1[(PsAtKeyHash{}(atKey)) & 63u];
                                if (atL1Slot.occupied && atL1Slot.key == atKey && atL1Slot.gen == atGenSnap) {
                                    atHold = atL1Slot.val.hlsl;
                                    atHash = atL1Slot.val.hlslHash;
                                } else {
                                    {
                                        std::shared_lock<std::shared_mutex> rlk(s_psCacheMtx);
                                        auto atIt = s_psAtCache.find(atKey);
                                        if (atIt != s_psAtCache.end()) {
                                            atHold = atIt->second.hlsl;
                                            atHash = atIt->second.hlslHash;
                                            atL1Slot = {true, atKey, atGenSnap, atIt->second};
                                        }
                                    }
                                    if (!atHold) {
                                        std::string hdr;
                                        if (alphaTestOn) {
                                            hdr += fmt::format("#define XE_ALPHA_FUNC {}\n#define XE_ALPHA_REF {:.9g}\n", func, aref);
                                        }
                                        if (alphaToMaskOn) {
                                            hdr += fmt::format("#define XE_ALPHA_TO_MASK 1\n#define XE_ALPHA_TO_MASK_OFFSETS {}u\n",
                                                               alphaToMaskOffsets);
                                        }
                                        if (minMaxRgb) hdr += "#define XE_MINMAX_PREMUL_RGB 1\n";
                                        if (minMaxAlpha) hdr += "#define XE_MINMAX_PREMUL_ALPHA 1\n";
                                        static const bool s_hashvizA0 = [] {
                                            const char* e = std::getenv("MCLA_HASHVIZ_A0");
                                            return e && e[0] == '1';
                                        }();
                                        std::string built;
                                        built.reserve(hdr.size() + pipePsHlsl->size() + 64);
                                        built.append(hdr);
                                        if (s_hashvizA0) built += "#define XE_HASHVIZ_A0 1\n";
                                        built.append(*pipePsHlsl);
                                        CachedModHlsl entry;
                                        entry.hlsl = std::make_shared<const std::string>(std::move(built));
                                        entry.hlslHash = (pipePsHlslHash != 0)
                                            ? mixHlslHash64(pipePsHlslHash,
                                                            (static_cast<uint64_t>(atKey.func) << 32) | atKey.flags,
                                                            static_cast<uint64_t>(atKey.arefRaw))
                                            : 0ULL;
                                        std::unique_lock<std::shared_mutex> wlk(s_psCacheMtx);
                                        auto atIt = s_psAtCache.find(atKey);
                                        if (atIt == s_psAtCache.end()) {
                                            if (s_psAtCache.size() >= 4096) {
                                                s_psAtCache.clear();
                                                s_psAtCacheGen.fetch_add(1u, std::memory_order_release);
                                            }
                                            atIt = s_psAtCache.emplace(atKey, std::move(entry)).first;
                                        }
                                        atHold = atIt->second.hlsl;
                                        atHash = atIt->second.hlslHash;
                                        atL1Slot = {true, atKey, s_psAtCacheGen.load(std::memory_order_relaxed), atIt->second};
                                    }
                                }
                                pipePsModHold = std::move(atHold);
                                pipePsHlsl = pipePsModHold.get();
                                pipePsHlslHash = atHash;
                                if (pipePsHlslHash == 0) {
                                    pipePsHlslHash = mixHlslHash64(
                                        psCacheKey ? psCacheKey : 0x9e3779b97f4a7c15ULL,
                                        (static_cast<uint64_t>(atKey.func) << 32) | atKey.flags,
                                        static_cast<uint64_t>(atKey.arefRaw));
                                }
                            }
                        }
                        pipePsHash = psOut.programHash;
                        // TEXUSE (log-only, MCLA_TEXUSE=<hex,hex,...> guest bases): names every pass that samples the shadow
                        // atlas / depth resolves (PS hash, slot, colour mask, blend, RT colour/depth info), once per
                        // (ps,slot,base), cap 512. Default list = cascade atlas rows + the two 1280x720 depth resolves (s1.log).
                        {
                            static const std::vector<uint32_t> s_tu = [] {
                                std::vector<uint32_t> v;
                                const char* e = std::getenv("MCLA_TEXUSE");
                                if (!e || !*e) return v;
                                if (e[0] == '1' && e[1] == 0) return std::vector<uint32_t>{0x1B754000u, 0x1B768000u, 0x1BA74000u, 0x1BA88000u, 0x1A144000u, 0x1B3BC000u};
                                for (const char* p = e; *p;) {
                                    char* end = nullptr;
                                    const unsigned long x = std::strtoul(p, &end, 16);
                                    if (end == p) break;
                                    v.push_back(static_cast<uint32_t>(x) & 0x1FFFFFFFu);
                                    p = (*end == ',') ? end + 1 : end;
                                }
                                return v;
                            }();
                            if (!s_tu.empty()) {
                                static std::mutex s_tuMtx;
                                static std::unordered_set<uint64_t> s_tuSeen;
                                for (uint32_t ti = 0; ti < dc.textureCount && ti < mcla::native::kCpMaxTextureDescriptors; ++ti) {
                                    const uint32_t b = dc.texturePoolVa[ti] & 0x1FFFFFFFu;
                                    if (std::find(s_tu.begin(), s_tu.end(), b) == s_tu.end()) continue;
                                    const uint64_t k = pipePsHash ^ (uint64_t(b) << 8) ^ ti;
                                    std::lock_guard<std::mutex> lk(s_tuMtx);
                                    if (s_tuSeen.size() >= 512 || !s_tuSeen.insert(k).second) continue;
                                    MCLA_LOG_ERROR("TEXUSE base={:08X} slot={} ps={:016X} vs={:016X} mask={:#x} blend0={:08X} "
                                                   "colorInfo={:08X} depthInfo={:08X} rtBound={} a1={} idx={} prim={} nTex={}",
                                                   b, ti, pipePsHash, pipeVsHash, psi.blendColorMask, psi.blendControl0,
                                                   dc.rasterState.regs[CpSnapshotReg::kCpSnapColorInfo],
                                                   dc.rasterState.regs[CpSnapshotReg::kCpSnapDepthInfo],
                                                   dc.rtBoundMask, opaqueRgbOnly ? 1 : 0, dc.numIndices,
                                                   dc.primitiveTopology, dc.textureCount);
                                }
                            }
                        }
                        if (s_charProbe && pipeVsHlsl && CharProbeSkinned(*pipeVsHlsl) &&
                            dc.numIndices >= 30u) {
                            static std::unordered_set<uint64_t> s_cpSeen;
                            const uint64_t kc = (pipeVsHash << 1) ^ pipePsHash;
                            if (s_cpSeen.size() < 512 && s_cpSeen.insert(kc).second) {
                                MCLA_LOG_WARN("CHARPROBE vs={:016X} ps={:016X} idx={} stride={} "
                                              "tex={:08X} nTex={}", pipeVsHash, pipePsHash,
                                              dc.numIndices, dc.vbStride,
                                              dc.textureCount ? dc.texturePoolVa[0] : 0u,
                                              dc.textureCount);
                            }
                        }
                        {   // MCLA_HASHVIZ=<k>: paint each draw flat by base-7 digit k of its PS hash.
                            static const int s_hv = [] { const char* e = std::getenv("MCLA_HASHVIZ"); return (e && *e) ? std::atoi(e) : -1; }();
                            if (opaqueRgbOnly && !pipePsHlsl->empty()) {
                                static std::unordered_map<uint64_t, std::shared_ptr<const std::string>> s_a1Cache;
                                const uint64_t a1Key = pipePsHlslHash ^ psCacheKey;
                                std::shared_ptr<const std::string> a1Hold;
                                {
                                    std::shared_lock<std::shared_mutex> rlk(s_psCacheMtx);
                                    auto it = s_a1Cache.find(a1Key);
                                    if (it != s_a1Cache.end()) a1Hold = it->second;
                                }
                                if (!a1Hold) {
                                    auto sp = std::make_shared<const std::string>("#define XE_ALPHA1\n" + *pipePsHlsl);
                                    std::unique_lock<std::shared_mutex> wlk(s_psCacheMtx);
                                    auto it = s_a1Cache.find(a1Key);
                                    if (it == s_a1Cache.end()) {
                                        if (s_a1Cache.size() >= 4096) s_a1Cache.clear();
                                        it = s_a1Cache.emplace(a1Key, sp).first;
                                    }
                                    a1Hold = it->second;
                                }
                                pipePsModHold = std::move(a1Hold);
                                pipePsHlsl = pipePsModHold.get();
                                pipePsHlslHash = mixHlslHash64(pipePsHlslHash ? pipePsHlslHash : psCacheKey, 0x4131ull, 1ull);
                            }
                            if (s_hv >= 0 && !pipePsHlsl->empty() && dc.numIndices >= 30) {
                                static std::unordered_map<uint64_t, std::shared_ptr<const std::string>> s_hvCache;
                                const uint64_t hvKey = pipePsHlslHash ^ psCacheKey;
                                std::shared_ptr<const std::string> hvHold;
                                {
                                    std::shared_lock<std::shared_mutex> rlk(s_psCacheMtx);
                                    auto it = s_hvCache.find(hvKey);
                                    if (it != s_hvCache.end()) hvHold = it->second;
                                }
                                if (!hvHold) {
                                    uint64_t d = pipePsHash; for (int i = 0; i < s_hv; ++i) d /= 7;
                                    static const char* kPal[7] = {"1,0,0", "0,1,0", "0,0,1", "0,1,1", "1,0,1", "1,1,0", "1,1,1"};
                                    const uint32_t c = static_cast<uint32_t>(d % 7);
                                    MCLA_LOG_ERROR("HASHVIZ ps={:016X} digit{}={} rgb={}", pipePsHash, s_hv, c, kPal[c]);
                                    auto sp = std::make_shared<const std::string>(
                                        fmt::format("#define XE_HASHVIZ_RGB float3({})\n", kPal[c]) + *pipePsHlsl);
                                    std::unique_lock<std::shared_mutex> wlk(s_psCacheMtx);
                                    auto it = s_hvCache.find(hvKey);
                                    if (it == s_hvCache.end()) {
                                        if (s_hvCache.size() >= 4096) s_hvCache.clear();
                                        it = s_hvCache.emplace(hvKey, sp).first;
                                    }
                                    hvHold = it->second;
                                }
                                pipePsModHold = std::move(hvHold);
                                pipePsHlsl = pipePsModHold.get();
                                pipePsHlslHash = mixHlslHash64(pipePsHlslHash ? pipePsHlslHash : psCacheKey, 0x4856ull, static_cast<uint64_t>(s_hv));
                            }
                        }
                        pipePsPrecompiledAvail = psOut.precompiledAvailable;
                        pipePsPrecompiledHash = psOut.precompiledCacheHash;
                        if (vsOut.hlsl.empty() || (psOut.hlsl.empty() && !depthOnlyNoPs)) {
                            pipeStop = "empty-hlsl";
                            pipeRow();
                            refuse("no-translated-pso", s_cpRefuseNoPso,
                                   "- a stage translated but produced no HLSL. See "
                                   "DRAW-CP-PIPE. NOT drawn on empty source.");
                            break;
                        }
                        // The four assignments that used to live here are
                        // GONE, hoisted to the two `fetch_add`s above. They are
                        // not deleted, they are read at the right time.
                    }

                    // GATE D -- the key and the PSO.
                    //
                    // ComputePipelineKeyForLayout, NOT ComputePipelineKey: GetOrCompile
                    // re-hashes the layout it is handed and refuses a key whose
                    // inputLayoutHash disagrees (pipeline_cache.cpp:2209-2213), so
                    // folding the layout in here is what keeps the key from claiming
                    // one declaration and handing the worker another.
                    //
                    // The sixth argument is the CARRIED geometry-stage capability
                    // (pipeline_cache.h, GetOrCompile). It is false on any cache
                    // miss, which is the point: the fallback PSO has no stage, and a
                    // rectangle draw mapped to TRIANGLELIST against it would draw one
                    // of the rectangle's two triangles. Reading the DEVICE's
                    // capability instead would be true on every draw, because the
                    // device is measured capable (0xB000/11.0, F-653), and would
                    // therefore be the F-609 fake.
                    {
                        STALL_STAGE("derive-pso");
                        // R5: direct-mapped 32-entry thread-local PSO memo. Valid only for a real map hit
                        // (never the async fallback), same exact key inputs, and same PipelineCache
                        // generation (bumped on every map_ insert/erase/clear). MCLA_NO_PSO_MEMO=1 disables.
                        static const bool s_psoMemoOn = [] { const char* e = std::getenv("MCLA_NO_PSO_MEMO"); return !(e && e[0] == '1'); }();
                        struct PsoMemo {
                            bool valid = false, carries = false, negative = false;
                            uint32_t gen = 0, failGen = 0;
                            uint64_t vsHash = 0, psHash = 0, vsHlsl = 0, psHlsl = 0, decl = 0;
                            uint32_t smpMask = 0;
                            mcla::renderer::PipelineState state{};
                            std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
                            Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
                        };
                        static thread_local PsoMemo s_psoMemo[32];
                        auto& pc = backend_.GetPipelineCache();
                        uint64_t psoSlotH = pipeVsHash ^ (pipePsHash * 0x9E3779B97F4A7C15ULL);
                        psoSlotH ^= (pipeVsHlslHash * 0xBF58476D1CE4E5B9ULL) ^ (pipePsHlslHash * 0x94D049BB133111EBULL);
                        psoSlotH ^= (inputDeclHash * 0xD6E8FEB86659FD93ULL) ^ (static_cast<uint64_t>(inputLayout.size()) * 0x517CC1B727220A95ULL);
                        PsoMemo& psoSlot = s_psoMemo[((psoSlotH ^ (psoSlotH >> 32)) & 31u)];
                        // 0 = the HLSL hash would be computed from the string; take the full path.
                        const bool memoOk = s_psoMemoOn && pipeVsHlslHash != 0 && pipePsHlslHash != 0;
                        bool memoHit = false;
                        if (memoOk && psoSlot.valid &&
                            (psoSlot.negative ? (psoSlot.failGen == pc.FailedGeneration())
                                              : (psoSlot.gen == pc.Generation())) &&
                            psoSlot.vsHash == pipeVsHash && psoSlot.psHash == pipePsHash &&
                            psoSlot.vsHlsl == pipeVsHlslHash && psoSlot.psHlsl == pipePsHlslHash &&
                            psoSlot.decl == inputDeclHash && psoSlot.state == derived &&
                            psoSlot.layout.size() == inputLayout.size()) {
                            memoHit = true;
                            for (size_t li = 0; li < inputLayout.size(); ++li) {
                                const D3D12_INPUT_ELEMENT_DESC& a = psoSlot.layout[li];
                                const D3D12_INPUT_ELEMENT_DESC& b = inputLayout[li];
                                if (a.SemanticIndex != b.SemanticIndex || a.Format != b.Format ||
                                    a.AlignedByteOffset != b.AlignedByteOffset || a.InputSlot != b.InputSlot ||
                                    a.InputSlotClass != b.InputSlotClass || a.InstanceDataStepRate != b.InstanceDataStepRate ||
                                    (a.SemanticName != b.SemanticName &&
                                     (!a.SemanticName || !b.SemanticName || std::strcmp(a.SemanticName, b.SemanticName) != 0))) {
                                    memoHit = false;
                                    break;
                                }
                            }
                        }
                        mcla::renderer::PipelineKey key;
                        if (!memoHit) {
                            key = mcla::renderer::ComputePipelineKeyForLayout(
                                pipeVsHash, pipePsHash,
                                pipeVsHlslHash ? pipeVsHlslHash : mcla::renderer::HashShaderBytecode(*pipeVsHlsl),
                                pipePsHlslHash ? pipePsHlslHash : mcla::renderer::HashShaderBytecode(*pipePsHlsl),
                                inputDeclHash, derived,
                                inputLayout);
                        }
                        s_pipePsoRequested.fetch_add(1, std::memory_order_relaxed);

                        // Precompiled PSO substitution: if both VS and PS are available in the
                        // precompiled cache, substitute the precompiled PSO and flag the draw
                        // to bind precompiled root signature and SharedConstants.
                        static bool s_enablePrecompiledSubstitution = [] {
                            const char* env = std::getenv("MCLA_USE_PRECOMPILED_PSO");
                            return (env != nullptr && env[0] == '1'); // default OFF until precompiled path renders"
                        }();
                        if (s_enablePrecompiledSubstitution &&
                            pipeVsPrecompiledAvail && pipePsPrecompiledAvail) {
                            if (pipeVsPrecompiledHash == 0x015E705A375C3C99ull &&
                                backend_.GetPrecompiledDrawPipeline()) {
                                pipePso = backend_.GetPrecompiledDrawPipeline();
                                desc.usePrecompiledPipeline = true;
                            } else if (backend_.GetPrecompiledTestPipeline()) {
                                pipePso = backend_.GetPrecompiledTestPipeline();
                                desc.usePrecompiledPipeline = true;
                            }
                            if (desc.usePrecompiledPipeline) {
                                static StatCtr<uint32_t> s_substitutions{0};
                                const uint32_t subNo = s_substitutions.fetch_add(1, std::memory_order_relaxed) + 1;
                                if (subNo <= 8 || (subNo % 200) == 0) {
                                    MCLA_LOG_INFO("PRECOMP-PSO-SUBSTITUTED #{} vsHash={:016X} psHash={:016X} -- using precompiled PSO and root signature",
                                                  subNo, pipeVsPrecompiledHash, pipePsPrecompiledHash);
                                }
                            }
                        }
                        auto capturePsoRtvMetadata = [&](const mcla::renderer::PipelineState& state) {
                            desc.psoRtvKnown = true;
                            desc.psoRtvCount = 0;
                            desc.psoRtvMask = 0;
                            for (uint32_t i = 0; i < desc.psoRtvFormats.size(); ++i) {
                                desc.psoRtvFormats[i] = state.targetFormats[i];
                                if (state.targetFormats[i] != DXGI_FORMAT_UNKNOWN) {
                                    ++desc.psoRtvCount;
                                    desc.psoRtvMask |= static_cast<uint8_t>(1u << i);
                                }
                            }
                        };
                        if (!desc.usePrecompiledPipeline) {
                            if (memoHit) {
                                if (psoSlot.negative) {
                                    pipePso.Reset();
                                    pipeHasGeomStage = false;
                                } else {
                                    pipePso = psoSlot.pso;
                                    pipeHasGeomStage = psoSlot.carries;
                                    capturePsoRtvMetadata(derived);
                                    if (psoSlot.smpMask != 0) desc.activeSamplerMask = psoSlot.smpMask;
                                    pc.NoteMemoHit();
                                }
                            } else {
                                // Generation read BEFORE the lookup: a concurrent change can only make the memo stale-safe (mismatch next draw).
                                const uint32_t gen0 = pc.Generation();
                                const uint32_t failGen0 = pc.FailedGeneration();
                                bool mapHit = false;
                                bool negHit = false;
                                pipePso = pc.GetOrCompile(
                                    key, *pipeVsHlsl, *pipePsHlsl, inputLayout,
                                    "render_thread.cpp:F-716", &pipeHasGeomStage, true, &mapHit, &negHit);
                                if (mapHit && pipePso) capturePsoRtvMetadata(key.state);
                                if (memoOk && mapHit && pipePso) {
                                    psoSlot.valid = true;
                                    psoSlot.negative = false;
                                    psoSlot.carries = pipeHasGeomStage;
                                    psoSlot.gen = gen0;
                                    psoSlot.vsHash = pipeVsHash; psoSlot.psHash = pipePsHash;
                                    psoSlot.vsHlsl = pipeVsHlslHash; psoSlot.psHlsl = pipePsHlslHash;
                                    psoSlot.decl = inputDeclHash;
                                    psoSlot.state = derived;
                                    psoSlot.layout = inputLayout;
                                    psoSlot.pso = pipePso;
                                    const uint32_t sm = pc.SamplerMaskFor(key);
                                    if (sm != 0) desc.activeSamplerMask = sm;
                                    psoSlot.smpMask = sm;
                                } else if (memoOk && negHit && !pipePso) {
                                    psoSlot.valid = true;
                                    psoSlot.negative = true;
                                    psoSlot.carries = false;
                                    psoSlot.failGen = failGen0;
                                    psoSlot.vsHash = pipeVsHash; psoSlot.psHash = pipePsHash;
                                    psoSlot.vsHlsl = pipeVsHlslHash; psoSlot.psHlsl = pipePsHlslHash;
                                    psoSlot.decl = inputDeclHash;
                                    psoSlot.state = derived;
                                    psoSlot.layout = inputLayout;
                                    psoSlot.pso.Reset();
                                } else {
                                    psoSlot.valid = false;
                                }
                            }
                        }
                        if (!pipePso) {
                            pipeStop = "pso-refused";
                            pipeRow();
                            s_pipePsoNull.fetch_add(1, std::memory_order_relaxed);
                            s_cpRefuseNoPso.fetch_add(1, std::memory_order_relaxed);
                            refuse("no-translated-pso", s_cpRefuseNoPso,
                                   "- the key was built from the guest's own "
                                   "translated shaders and input layout and "
                                   "GetOrCompile returned null, so its own refusal "
                                   "counters and names are the authority. NOT drawn "
                                   "on the fallback: that PSO has no geometry stage "
                                   "and no guest layout.");
                            break;
                        }
                        if (pipeHasGeomStage) {
                            s_pipePsoCarriesGeomStage.fetch_add(1,
                                                                std::memory_order_relaxed);
                        }
                    }
                }
            }

            // F-648: desc.topology, ASSIGNED. F-637 added the field to
            // DynamicMeshDesc (src/d3d12_backend.h:101) and F-650 added the
            // backend's UNDEFINED refusal that named this file as the cause
            // (src/d3d12_backend.cpp:2596-2619, citing render_thread.cpp:1253),
            // but nothing here ever assigned it: the `desc = {}` zero-initialises
            // it, and a D3D_PRIMITIVE_TOPOLOGY of 0 is UNDEFINED. So every
            // captured draw reached the backend as "the producer never set a
            // topology" and the guest's real prim_type was discarded - exactly
            // what this call fixes.
            //
            // MapXenosPrimType is the architected map, written out per code
            // against xenos::PrimitiveType (xenos.h:44-59) rather than as an
            // offset into an array (src/d3d12_backend.cpp:265-319). It refuses
            // every prim_type D3D12 has no equivalent for and counts each one
            // per prim_type, so calling it is what MAKES the refusal counter
            // able to see the guest's primitive type at all.
            //
            // F-716: the THIRD ARGUMENT IS THE CARRIED GEOMETRY-STAGE FACT, and
            // this call is the only reason it exists. `pipeHasGeomStage` came out
            // of GetOrCompile (pipeline_cache.h), which can only set it on a cache
            // hit whose key named the expansion stage -- so it means "the PSO this
            // draw is about to use HAS the stage bound". Passing
            // `GeometryStageSupported()` here instead would be true on every
            // draw, because the device IS capable (0xB000/11.0, F-653), and would
            // map every rectangle list to TRIANGLELIST including the ones about to
            // be drawn against the GS-less cache fallback: half a quad per
            // rectangle with nothing in the log saying so. That is the F-609 fake
            // this call is built to make impossible, and it is why the flag is
            // CARRIED per-draw rather than read from a device-wide property.
            desc.xenosPrimType = dc.primitiveTopology;
            // Expanded rect lists / quad lists map as the triangle list they now are.
            if (!D3D12Backend::MapXenosPrimType(
                (rectCpuExpanded || quadCpuExpanded) ? 0x04u : dc.primitiveTopology,
                                                 desc.topology,
                                                 pipeHasGeomStage)) {
                // F-648: 0x08 kRectangleList is the case that matters here, so it
                // gets its own counter instead of hiding in the fallthrough.
                //
                // THE DECISION IS A REFUSAL, not an expansion, and F-652 re-
                // examined all three prerequisites this comment used to list as
                // undecoded. Two are DECODED and one is still BLOCKING. Recording
                // that here rather than in a session note, because "we tried" is
                // not evidence and the next stream must not re-derive it.
                //
                // (a) PIXEL-CENTRE CONVENTION - DECODED: there is no host-side
                //     pixel-centre offset in the expansion AT ALL. The complete
                //     rectangle rule is the D3D12 geometry-shader fallback,
                //     .research/xenia/src/xenia/gpu/d3d12/pipeline_cache.cc:2550-
                //     2694, and every arithmetic term in it (:2579-2595 edge
                //     squares, :2669-2674 the fourth corner) is a plain float
                //     add of vertex positions with no 0.5 and no PA_SU_VTX_CNTL
                //     term. `grep half_pixel` over primitive_processor.cc and the
                //     rectangle-list paths in both pipeline caches returns hits
                //     only in the EDRAM-resolve code (draw_util.cc:344, :842-849,
                //     :1098-1103, the resolve.xesli shaders), which is a
                //     different feature. The half-pixel offset the old comment
                //     cited is xenos.h:934-939 describing what the GUEST's OWN
                //     vertex shader applies ("with the half-pixel offset ...
                //     pre-applied to the vertices"), not something the emulator
                //     adds. So this prerequisite is settled and it is settled
                //     AGAINST adding an offset.
                //
                // (b) VERTEX-FETCH ENDIANNESS - DECODED, CARRIED AND NOW
                //     APPLIED. The slot's `endian` is `xe_gpu_vertex_fetch_t`'s
                //     2-bit field (xenos.h:1122), read by this file's own working
                //     decode at src/gpu_cp.cpp:1705 (and :1735, the by-index
                //     reader). 2026-10-04: this cited :1056, which is inside
                //     CpRegPoke and carries no `s.endian =`; re-measured, the two
                //     lines above are the only ones. The index-side byte-order
                //     model is the same shape (render_thread.cpp
                //     XenosIndexElement / TranscodeXenosIndices, :363-398), and
                //     the vertex side now matches it: F-751 added
                //     CpVertexElement::endianness to src/render_queue.h and the
                //     DRAW_CAPTURED path now applies xenos::GpuSwap to every
                //     dword of the stream it reads.
                //
                //     THE "IT IS MOOT" SENTENCE THAT STOOD HERE WAS WRONG, AND
                //     IT WAS WRONG IN A MEASURABLE WAY. It read "It is moot for
                //     the corner rule anyway, for the reason in (c): the
                //     positions are not vertex-buffer bytes." The second clause
                //     is false: the positions ARE vertex-buffer bytes on this
                //     path. The backend's own probe reads the float lanes
                //     straight out of the stream this file hands it -
                //     `memcpy(&f, vp + l * 4u, 4)` at src/d3d12_backend.cpp:2975,
                //     over `desc.vertexBytes`, which is the very buffer
                //     `ReadBytes` filled above - and MEASURED in build/w708.log
                //     it printed v0lane0Bits=0x000000BF,0x000000BF where the
                //     architected reading is 0xBF000000 = -0.5. So (c)'s own
                //     premise is what (b) was reasoning from, and (c) is about
                //     WHICH LANE is position and whether it is PRE- or POST- the
                //     vertex shader - a different question from what byte order
                //     the lanes arrive in. Leaving (b) unresolved did not make
                //     it moot; it left a real transform unmade on the path that
                //     reaches the rasterizer.
                //
                // (c) WHICH ELEMENT IS POSITION - STILL BLOCKING, and it is not
                //     a gap in the vertex fetch. The reference computes the
                //     fourth corner from the POST-VERTEX-SHADER position:
                //     d3d12/pipeline_cache.cc:2579-2582 reads
                //     `V2D(2, input_register_position, ...)` and :2636-2637
                //     emits `O(output_register_position)` from
                //     `V2D(input_vertex_index, input_register_position)` - i.e.
                //     a GEOMETRY-SHADER input, the guest VS's own output. The
                //     longest-edge test (:2597-2622) is on those transformed
                //     values too, precisely so the expansion stays correct under
                //     an arbitrary VS transform (the same concern the triangle-
                //     fan comment at primitive_processor.h:536-543 raises).
                //     Producing them on the CPU means EXECUTING the guest vertex
                //     shader, and this is a static recompiler - there is no
                //     interpreter, and the alternative (folding the rule into the
                //     translated shader) is src/renderer/**, which another stream
                //     owns and is mid-edit.
                //     The reference agrees this is unimplemented: its own SPIR-V
                //     translator carries the TODO at
                //     spirv_shader_translator.cc:1322-1323 ("For
                //     HostVertexShaderType::kRectangeListAsTriangleStrip, start
                //     the vertex loop, and load the index there"), and
                //     dxbc_shader_translator.cc:630-634 takes the whole type to
                //     assert_unhandled_case. Only the D3D12 GS fallback above
                //     implements it.
                //
                // So it is refused by name, with the blocker named, rather than
                // half-implemented: a CPU expansion that computed v1+v2-v0 over
                // UNTRANSFORMED vertex bytes would produce parallelograms for
                // every rectangle whose VS moves a corner, which is sheared
                // quads - strictly worse than the blank below. WHAT THE GAME
                // EXPERIENCES: these draws produce NO pixels, where before they
                // produced half of every quad with nothing in the log saying so.
                // prim-rect-list= on DRAW-CP-CENSUS and rectList(expanded=0
                // refused=N) on PC-DERIVE are both non-zero, so the gate is
                // visible and countable rather than inferred.
                if (dc.primitiveTopology == 0x08u) {
                    refuse("prim-rect-list", s_cpRefusePrimRect,
                           "- xenos::PrimitiveType::kRectangleList (xenos.h:53): 3 "
                             "vertices per rectangle, expanded by the reference to a "
                             "two-triangle strip IN THE VERTEX SHADER "
                             "(primitive_processor.cc:392-398, :449-457). NOT "
                             "substituted with a triangle list, which would draw one "
                             "of the rectangle's two triangles.");
                } else {
                    refuse("prim-unmappable", s_cpRefusePrimOther,
                           "- D3D12 has no topology for this Xenos prim_type; see "
                             "MapXenosPrimType (src/d3d12_backend.cpp:265-319).");
                }
                break;
            }
            s_cpTopologyAssigned.fetch_add(1, std::memory_order_relaxed);

            // B4 (T41.4b), same fix as DRAW_INDEXED: DrawDynamicMesh already
            // closes, executes, presents and advances the frame index
            // (src/d3d12_backend.cpp:1487-1504). The ClearAndPresent() that
            // used to follow was a SECOND present of the same frame whose first
            // act is ClearRenderTargetView — it erased the draw that had just
            // rendered. One present per drawn frame, owned by the draw path.
            // 2026-10-02: WHAT THIS DRAW ACTUALLY DID, as a row of its own.
            //
            // HISTORY OF THE SENTENCE BELOW, because it described the defect
            // rather than the fix and would now be false: "`DRAW-CP-DRAWN`
            // counts commands that REACHED this site. It does not count draws,
            // and the difference is the whole measurement problem." That was
            // TRUE of the tree this file was last compiled against and is NO
            // LONGER TRUE of it. `DRAW-CP-DRAWN` is now gated on the draw call
            // actually being recorded (the backend's own m_stats.drawsIssued
            // advancing across the call), and the commands-that-reached-the-
            // site count it used to carry has moved to its own row,
            // DRAW-CP-DRAWN-SKIPPED, on its own counter. The measurement
            // problem this paragraph was written about is the one the next
            // paragraph's evidence is for, and it is kept below as the record
            // of what was wrong.
            //
            // MEASURED on build/w696.log (sha256 3D21DE0E..7F1893,
            // 55,669 lines), which carries `DRAW-CP-DRAWN #1` at 03:30:04.948 and
            // `#2` at 03:30:04.961, and also carries:
            //   - `FRAME-OWNER owner=draw` ZERO times, so
            //     PresentOwnedFrame(FrameOwner::Draw) never ran;
            //   - `D3D12Backend: draw #N issued through a cache-produced PSO` ZERO
            //     times, so the body of DrawDynamicMeshWithPipeline never reached
            //     SetPipelineState;
            //   - the BACKEND's `CONST-BANK #n` ZERO times (the two
            //     CONST-BANK rows in that log are `CONST-BANK-PRODUCE`, the
            //     producer side -- a different marker, and matching its prefix
            //     would have been a false positive);
            //   - `DRAW-CP-PIPE` ZERO times. NOTE: the parenthetical claim that
            //     used to sit here -- "pipeStop reaches `drawn` and pipeRow() is
            //     called for it at the end of this case" -- was FALSE and is
            //     retracted. `pipeStop` is set to "drawn" only inside the
            //     `else if (pipePso)` arm, which is reached only when
            //     `beganFrame` is true, and the call at the end of this case is
            //     `if (std::strcmp(pipeStop, "drawn") == 0) pipeRow();`. So a
            //     BeginFrame refusal sets pipeStop to "begin-frame-false",
            //     pipeRow() is NOT called, and DRAW-CP-PIPE=0 is the CORRECT
            //     and EXPECTED consequence of a closed gate -- it is not
            //     independent evidence of a second fault.
            // Two readings of the rest are consistent with every line above, and
            // they are different faults: BeginFrame() returning false is a
            // completely different problem from a refusal inside the draw body.
            // As of 2026-10-02 this is no longer a choice between them on the
            // evidence: the backend names all five of its own exits
            // (D3D12-BEGINFRAME-REFUSE, one row per gate) and names every
            // abandoned frame (D3D12-FRAME-ABANDONED), so the next soak says
            // which one fired instead of leaving the two to be told apart by
            // absence. This row reports the outcome plus the draw's own return
            // value, which the call below used to DISCARD -- a draw that
            // returned false and a draw that rendered were the same two lines.
            // 2026-10-02: hoisted out of the block below so that DRAW-CP-DRAWN,
            // which sits ~80 lines further down and outside that block, can gate
            // on the same fact instead of on a second, weaker guess. Declared
            // here and assigned inside; both are single-threaded with respect to
            // the backend (this is the render thread, the only consumer).
            // ---- Stage 1 hand-off (serial path): publish locals into prep/meta ----
            meta.vertexCount = vertexCount;
            meta.tcCount = tc.count;
            meta.tcMaxIndex = tc.maxIndex;
            meta.pipeVsCodeBytes = pipeVsCodeBytes;
            meta.pipePsCodeBytes = pipePsCodeBytes;
            meta.pipeVsElements = pipeVsElements;
            meta.pipeVsInstructions = pipeVsInstructions;
            meta.pipeVsInterpolants = pipeVsInterpolants;
            meta.pipeVsHash = pipeVsHash;
            meta.pipePsHash = pipePsHash;
            meta.pipeVsSrc = pipeVsSrc;
            meta.pipePsSrc = pipePsSrc;
            meta.pipeTrStage = pipeTrStage;
            meta.pipeVsHlslSize = pipeVsHlsl->size();
            meta.pipePsHlslSize = pipePsHlsl->size();
            meta.inputDeclHash = inputDeclHash;
            meta.pipeColorFormatCode = pipeColorFormatCode;
            meta.pipeColorFormatDxgi = pipeColorFormatDxgi;
            meta.pipeRsPresent = pipeRsPresent;
            meta.pipeTranslateError = pipeTranslateError;
            prep.hasGeometryStage = pipeHasGeomStage;
            desc.psoRtvFormat = pipeColorFormatDxgi;
            desc.psoPath = 2;
            desc.psProgramHash = pipePsHash;
            desc.vsProgramHash = pipeVsHash;
            desc.guestBlendControl0 = dc.rasterState.regs[CpSnapshotReg::kCpSnapBlendControl0];
            desc.guestColorMask = dc.rasterState.regs[CpSnapshotReg::kCpSnapColorMask];
            desc.guestDepthControl = dc.rasterState.regs[CpSnapshotReg::kCpSnapDepthControl];
            if (D3D12Backend::F9Active() && pipePsHlsl) D3D12Backend::F9NotePsHlsl(pipePsHash, *pipePsHlsl);
            prep.pipePso = pipePso;
            prep.pso = pipePso.Get();
            prep.FinalizeDescPointers();
            prep.valid = true;
    } while (false);
}


void RenderThread::FinalizeDrawAccounting(const RenderCommand& cmd,
                                          const D3D12Backend::PreparedDraw& prep,
                                          const DrawStage1Meta& meta,
                                          bool beganFrame,
                                          bool drew,
                                          bool cpIssuedDrawCall) {
    const auto& dc = std::get<DrawCapturedCommand>(cmd.data);
    const uint32_t n = static_cast<uint32_t>(prep.drawSeq);
    const uint32_t primSlot = meta.primSlot;
    const uint32_t vertexCount = meta.vertexCount;
    const bool indexed = meta.indexed;
    const uint64_t pipeVsHash = meta.pipeVsHash, pipePsHash = meta.pipePsHash;
    const uint32_t pipeColorFormatCode = meta.pipeColorFormatCode, pipeColorFormatDxgi = meta.pipeColorFormatDxgi, pipeRsPresent = meta.pipeRsPresent;
    const auto& desc = prep.desc;
    auto& guestMem = mcla::kernel::GuestMemoryHeap::Instance();
    (void)drew; (void)vertexCount; (void)pipeVsHash; (void)pipePsHash; (void)guestMem; (void)desc;

    // TEMP-SKYDRAW (2026-11-20). The deleted pre-cleanup draw replay was needed to
    // name the draws that render the floating fragments above the title-screen
    // horizon. This compact version projects a capped subset of each drawn draw's
    // vertices with the c8..c11 matrix (the same convention as the old
    // TEMP-DRAWINFO-CLIP) and logs draws whose geometry reaches the upper screen.
    // Gated: MCLA_TEMP_SKYDRAW=1. Unset = no guest reads, no logs.
    static const bool s_skyDraw = [] {
        const char* e = std::getenv("MCLA_TEMP_SKYDRAW");
        return e && e[0] == '1';
    }();
    if (s_skyDraw) {
        static std::atomic<uint32_t> s_skyGateCalls{0}, s_skyBail{0};
        const uint32_t g = s_skyGateCalls.fetch_add(1) + 1u;
        if (g == 1u) MCLA_LOG_ERROR("SKY-GATE active");
        if (!(drew && pipeVsHash != 0u && desc.guestConstBytes != nullptr &&
              desc.guestConstBytesSize >= 192u && dc.vbStride >= 12u && dc.vbSize >= dc.vbStride)) {
            const uint32_t b = s_skyBail.fetch_add(1) + 1u;
            if (b <= 2u) MCLA_LOG_ERROR("SKY-BAIL #{} drew={} vs={:016X} cb={} cbSize={} stride={} vbSize={}",
                                       b, drew ? 1 : 0, pipeVsHash, desc.guestConstBytes ? 1 : 0,
                                       desc.guestConstBytesSize, dc.vbStride, dc.vbSize);
        }
    }
    if (s_skyDraw && drew && pipeVsHash != 0u && desc.guestConstBytes != nullptr &&
        desc.guestConstBytesSize >= 192u && dc.vbStride >= 12u && dc.vbSize >= dc.vbStride) {
        const uint8_t* cb = static_cast<const uint8_t*>(desc.guestConstBytes);
        float cm[16];
        auto beF = [](const uint8_t* p) {
            const uint32_t u = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                               (uint32_t(p[2]) << 8) | p[3];
            float f; std::memcpy(&f, &u, 4); return f;
        };
        for (int k = 0; k < 16; ++k) cm[k] = beF(cb + 128u + size_t(k) * 4u);
        const uint32_t stride = dc.vbStride;
        const uint32_t nv = std::min<uint32_t>(dc.vbSize / stride, 2048u);
        std::vector<uint8_t> vbBuf(size_t(nv) * stride);
        if (guestMem.ReadBytes(dc.vbBase, vbBuf.data(), vbBuf.size())) {
            auto pos = [&](uint32_t v, float p[3]) -> bool {
                if (v >= nv) return false;  // IB index outside the captured VB — never dereference
                const uint8_t* q = vbBuf.data() + size_t(v) * stride;
                for (int c = 0; c < 3; ++c) p[c] = beF(q + size_t(c) * 4u);
                return true;
            };
            std::vector<uint8_t> ibBuf;
            if (dc.ibBase && dc.ibSize && dc.ibSize <= (1u << 22)) {
                ibBuf.resize(dc.ibSize);
                if (!guestMem.ReadBytes(dc.ibBase, ibBuf.data(), ibBuf.size())) ibBuf.clear();
            }
            // TEMP-REPLAY (2026-11-20): per-draw VB/IB/cb dumps for tools/xvs_replay.py.
            static const bool s_dumpAll = [] {
                const char* e = std::getenv("MCLA_TEMP_DUMP_ALLDRAWS");
                return e && e[0] == '1';
            }();
            static std::unordered_map<uint64_t, uint32_t> s_dumpedPerVs;
            // MCLA_TEMP_DUMP_CAP=N (2026-11-20): per-VS dump cap. Default 8
            // preserves prior behaviour; a high cap is needed to sample the
            // thousands of foliage draws that share a handful of VS hashes.
            static const uint32_t s_dumpCap = [] {
                const char* e = std::getenv("MCLA_TEMP_DUMP_CAP");
                const unsigned long v = e ? std::strtoul(e, nullptr, 10) : 0;
                return v ? static_cast<uint32_t>(v) : 8u;
            }();
            if (s_dumpAll && s_dumpedPerVs[pipeVsHash]++ < s_dumpCap) {
                std::filesystem::create_directories("replay2");
                char fn[160];
                const auto wr = [&](const char* ext, const void* p, size_t len) {
                    std::snprintf(fn, sizeof fn, "replay2/d%05u_%016llX.%s", n,
                                  (unsigned long long)pipeVsHash, ext);
                    if (std::FILE* f = std::fopen(fn, "wb")) {
                        std::fwrite(p, 1, len, f); std::fclose(f);
                    }
                };
                // xvs_replay reads VB as host-order ('<'); guest floats are BE, so swap words.
                std::vector<uint8_t> hostVb(vbBuf.size());
                for (size_t i = 0; i + 3u < vbBuf.size(); i += 4u) {
                    hostVb[i] = vbBuf[i + 3]; hostVb[i + 1] = vbBuf[i + 2];
                    hostVb[i + 2] = vbBuf[i + 1]; hostVb[i + 3] = vbBuf[i];
                }
                wr("vb", hostVb.data(), hostVb.size());
                wr("ib", ibBuf.data(), ibBuf.size());
                wr("cb", cb, std::min<size_t>(desc.guestConstBytesSize, 4096u));
                char txt[512];
                const int tn = std::snprintf(txt, sizeof txt,
                    "di=%u prim=%u nIdx=%u ibFmt=%u ibSwap=%u idxOff=%u stride=%u vbSize=%u frame=%08X ps=%016llX\n",
                    n, dc.primitiveTopology, dc.numIndices, dc.ibFmt, dc.ibSwap, dc.idxOffset,
                    dc.vbStride, dc.vbSize, dc.frameId, (unsigned long long)pipePsHash);
                wr("txt", txt, size_t(std::max(tn, 0)));
                // TEMP-TEXBIND: join draw -> PS hash -> guest texture base/fmt so the
                // exact texture of a screen-region defect can be identified.
                {
                    std::string tex;
                    for (uint32_t gi = 0; gi < desc.guestTextureCount && desc.guestTextures; ++gi) {
                        const D3D12Backend::GuestTextureEntry& ge = desc.guestTextures[gi];
                        const uint8_t* b = ge.descriptorBytes;
                        const uint32_t d1 = (uint32_t(b[4]) << 24) | (uint32_t(b[5]) << 16) |
                                            (uint32_t(b[6]) << 8) | b[7];
                        const uint32_t d2 = (uint32_t(b[8]) << 24) | (uint32_t(b[9]) << 16) |
                                            (uint32_t(b[10]) << 8) | b[11];
                        tex += fmt::format("{:08X}/fmt{:02X};", d1 & 0xFFFFF000u, d1 & 0x3Fu);
                        (void)d2;
                    }
                    MCLA_LOG_ERROR("TEMP-TEXBIND draw={} ps={:016X} tex={}", n,
                                   (unsigned long long)pipePsHash, tex);
                }
            }
            auto indexAt = [&](uint32_t i) -> uint32_t {
                if (ibBuf.empty()) return i;
                if (dc.ibFmt == 0u && ibBuf.size() >= size_t(i) * 2u + 2u)
                    return (uint32_t(ibBuf[size_t(i) * 2u]) << 8) | ibBuf[size_t(i) * 2u + 1u];
                if (dc.ibFmt == 1u && ibBuf.size() >= size_t(i) * 4u + 4u) {
                    const uint8_t* q = ibBuf.data() + size_t(i) * 4u;
                    return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) |
                           (uint32_t(q[2]) << 8) | q[3];
                }
                return i;
            };
            uint32_t skyTris = 0, tested = 0;
            float hiNy = -1e30f, hiNx = -1e30f, loNx = 1e30f;
            const uint32_t ni = std::min<uint32_t>(dc.numIndices, 6000u);
            for (uint32_t t = 0u; t + 2u < ni; t += 3u) {
                float p[3][4];
                bool ok = true;
                for (uint32_t k = 0u; k < 3u; ++k) {
                    float v[3] = {0, 0, 0};
                    if (!pos(indexAt(t + k), v)) { ok = false; break; }
                    for (int c = 0; c < 4; ++c)
                        p[k][c] = v[0] * cm[c] + v[1] * cm[4 + c] + v[2] * cm[8 + c] + cm[12 + c];
                    if (p[k][3] <= 0.0001f) ok = false;
                }
                if (!ok) continue;
                ++tested;
                for (uint32_t k = 0u; k < 3u; ++k) {
                    const float nx = p[k][0] / p[k][3], ny = p[k][1] / p[k][3];
                    hiNy = std::max(hiNy, ny); hiNx = std::max(hiNx, nx); loNx = std::min(loNx, nx);
                    if (ny > 0.25f) ++skyTris;
                }
            }
            if (tested) {
                // Positive control: prove this path runs and show the ny distribution,
                // even when nothing exceeds the sky threshold (rule 19).
                static std::atomic<uint32_t> s_skyTested{0};
                const uint32_t q = s_skyTested.fetch_add(1) + 1u;
                if (q <= 8u || (q % 2000u) == 0u)
                    MCLA_LOG_ERROR("SKY-CTRL #{} draw={} vs={:016X} tested={} hiNy={:.3f} nx=[{:.2f},{:.2f}]",
                                  q, n, pipeVsHash, tested, hiNy, loNx, hiNx);
            }
            if (tested && skyTris) {
                static std::atomic<uint32_t> s_skyRows{0};
                const uint32_t r = s_skyRows.fetch_add(1) + 1u;
                if (r <= 20u || (r % 500u) == 0u)
                    MCLA_LOG_ERROR("SKY-DRAW #{} draw={} vs={:016X} ps={:016X} skyVerts={}/{} "
                                  "hiNy={:.3f} nx=[{:.2f},{:.2f}] vb={:08X}/{} nIdx={} rt={:08X}",
                                  r, n, pipeVsHash, pipePsHash, skyTris, tested * 3u,
                                  hiNy, loNx, hiNx, dc.vbBase, dc.vbSize, dc.numIndices,
                                  dc.rtColorInfo);
            }
        }
    }
    const char* pipeStop = "none";
    {
        if (!beganFrame) {
            pipeStop = "begin-frame-false";
        } else if (cpIssuedDrawCall) {
            pipeStop = "drawn";
        } else {
            pipeStop = prep.pso ? "draw-refused" : "no-pso-at-draw";
            LogPipeRow(dc, prep, meta, pipeStop);
        }
                const uint32_t in = s_cpIssuedLogged.fetch_add(
                                        1, std::memory_order_relaxed) + 1u;
                if (in <= 8u || (in % 20000u) == 0u) {  // MD-32: was 200
                    MCLA_LOG_WARN(
                        "DRAW-CP-ISSUED #{} draw={} beginFrame={} drawCall={} "
                        "returned={} pso={} | colorFormatCode={} "
                        "colorFormatDxgi={} rsPresent={:08X} -- "
                        "colorFormatDxgi is what the PSO's RTVFormats[0] was "
                        "built from; compare it with D3D12-RTV-FORMAT, the "
                        "format of the render target actually bound. A "
                        "disagreement rasterizes nothing without an error.",
                        in, n, beganFrame ? 1 : 0,
                        (beganFrame && prep.pso) ? 1 : 0, drew ? 1 : 0,
                        prep.pso ? 1u : 0u, pipeColorFormatCode,
                        pipeColorFormatDxgi, pipeRsPresent);
                }
                // 2026-10-02: THE COMPARISON, IN ONE ROW. P5 could not be
                // satisfied by D3D12-RTV-FORMAT alone and the reason is
                // structural, not an oversight: a D3D12 pipeline state does not
                // report its own RTVFormats[] -- there is no reflection getter
                // for it -- so the PSO side is knowable ONLY here, where the
                // guest's colour_format was mapped, and the bound side is
                // knowable ONLY in the backend, which read it off the bound
                // resource's D3D12_RESOURCE_DESC. Two rows in two translation
                // units, never adjacent. This row is that comparison, and it is
                // NEW: it appends to nothing and renames nothing, because
                // DRAW-CP-ISSUED's format is one of the 244 tracked and
                // changing it is forbidden.
                //
                // `bound=0` means the backend has not bound a render target yet,
                // which on a closed gate is the expected reading and not a
                // format: the bind site is downstream of BeginFrame(). `agree`
                // is therefore only claimed when BOTH numbers are real, and a
                // disagreement is printed as a disagreement rather than left for
                // a reader to subtract two numbers from two different rows.
                const uint32_t boundRtvDxgi = backend_.BoundRtvFormat();
                if (in <= 8u || (in % 20000u) == 0u) {  // MD-32: was 200
                    MCLA_LOG_WARN(
                        "CP-COLORFMT-VSRTV #{} draw={} psoRtvDxgi={} boundRtvDxgi={} "
                        "agree={} boundSet={} colorFormatCode={} | psoRtvDxgi is "
                        "MapXenosColorFormatToDxgi(colorFormatCode) -- the value "
                        "renderer/pipeline_cache.cpp put in RTVFormats[0]; "
                        "boundRtvDxgi is the backend's D3D12-RTV-FORMAT value, "
                        "read off the bound back buffer's own descriptor. "
                        "boundSet=0 means no render target has been bound yet, so "
                        "no comparison is possible and agree is reported 0, NOT as "
                        "a match. psoRtvDxgi=28 is R8G8B8A8_UNORM.",
                        in, n, pipeColorFormatDxgi, boundRtvDxgi,
                        (boundRtvDxgi != 0 && boundRtvDxgi == pipeColorFormatDxgi)
                            ? 1 : 0,
                        (boundRtvDxgi != 0) ? 1u : 0u, pipeColorFormatCode);
                }
            }
            // ------------------------------------------------------------------
            // HISTORY OF THE BLOCK THAT JUST MOVED ABOVE, kept whole because two
            // of its claims were wrong and cost a previous stream a re-do of
            // F-653. It is COMMENT ONLY: the `if (backend_.BeginFrame())` that
            // wrapped it is gone, because the call it guarded now happens once,
            // above, with its result kept. Nothing below this line executes.
            //
            // THE OLD SHAPE was
            //     if (backend_.BeginFrame()) {
            //         if (pipePso) { backend_.DrawDynamicMeshWithPipeline(desc,
            //                                            pipePso.Get());
            //                        pipeStop = "drawn"; }
            //         else { pipeStop = "no-pso-at-draw"; LogPipeRow(dc, prep, meta, pipeStop); }
            //     }
            //     const uint32_t dn = s_cpDrawn.fetch_add(1) + 1;
            // i.e. `DRAW-CP-DRAWN` was incremented and logged whether or not the
            // body ran. That is the defect measured in build/w696.log: the row
            // reads as a closed draw chain and no row anywhere says a draw call
            // was made.
            //
            // THE HISTORY, because the previous version of this comment was
                // wrong and would have misled the next agent into re-doing F-653.
                // It claimed that `DrawDynamicMesh` hard-passed the test PSO, that
                // the hard pass was the `m_testPipeline.Get()` argument, and that
                // the second fallback at d3d12_backend.cpp:2801 still had to be
                // deleted. All three were already false in the tree this file was
                // last compiled against: F-653 rewrote that entry point so it
                // REFUSES instead of forwarding (d3d12_backend.cpp:2622-2634) and
                // deleted the `pipeline ? pipeline : m_testPipeline.Get()` line
                // (it survives only as history at d3d12_backend.cpp:2920-2922;
                // :2801 is now `if (desc.indexed && !desc.cachedIndexGpu)`). The
                // 28-byte stride gate this comment cited at :2554-2557 also moved
                // to :2674-2698, where it fires only for a test PSO the caller
                // passed deliberately.
                //
                // So the backend is no longer what protects this line - the gate
                // is. `no-translated-pso` above ends the draw with a `break`, and
                // that is the only thing standing between a derived rasteriser
                // with no PSO and a draw call. It was measured, not assumed: in
                // build/w682.log that gate did NOT break, fell through to the
                // primitive mapper, and on any prim_type other than 0x08 it would
                // have carried on to this call.
                //
                // F-716: WHAT IS STILL MISSING IS NOT IN THIS FILE, AND IT IS NOW
                // NAMED BY A MEASUREMENT INSTEAD OF A HOPE. The old text here said
                // "wiring this call site to DrawDynamicMeshWithPipeline(desc,
                // realPso.Get()) needs translated HLSL plus an input layout from
                // the guest's own program, which is src/renderer/** - owned
                // elsewhere". That was half right and half wrong: the HLSL needs no
                // ownership change at all (TranslateShaderPinned is already in the
                // tree and is called from the chain above), and the input layout is
                // NOT an src/renderer/** problem. It is that an EMBEDDED program
                // carries no vertex-element table -- the reference does not keep
                // one either, taking vertex elements from the vertex-FETCH hardware
                // and the interpolant set from SQ_INTERPOLATOR_CNTL
                // (d3d12/pipeline_cache.cc:882, :915) -- so with
                // `vertexElements` empty every semantic ReferencedVertexInputs can
                // return is its own POSITION fallback (shader_translator.cpp:68-78).
                // The blockers are therefore two CAPTURES, in files this stream
                // does not own: the vfetch `dst_reg` (which would give the element
                // address to match, and gpu_cp.cpp:1352 states its omission as
                // deliberate) and the vf->DXGI table for elements past the first
                // (inline in gpu_cp.cpp as F-632, not exported). Neither is in the
                // renderer, and neither is a translation problem.
                //
            // The call is the WithPipeline overload, on the PSO the chain
            // above actually obtained. The no-pipeline `DrawDynamicMesh` is
            // NEVER called from here again: it refuses by design (F-653,
            // d3d12_backend.cpp:2622-2634), and leaving it as the tail would
            // make this a counted absence rather than a drawn frame.
            //
            // END OF HISTORY. The live call is the block above this comment.
            // ------------------------------------------------------------------
            // 2026-10-02: THIS MARKER WAS A LIE AND NO LONGER IS.
            //
            // `s_cpDrawn` was incremented UNCONDITIONALLY here, outside the
            // BeginFrame gate, so it counted commands that REACHED this site
            // rather than draws that happened. MEASURED on build/w698.log: two
            // DRAW-CP-DRAWN rows and zero PC-PSO-BOUND, zero
            // D3D12-DRAW-RETURN, zero `D3D12Backend: draw #N issued through a
            // cache-produced PSO` and zero `FRAME-OWNER owner=draw` -- i.e. two
            // rows reading as a closed draw chain for two frames in which no
            // draw call was recorded. The same counter is what
            // DRAW-CP-CENSUS prints as `drawn=`, so that field was lying too,
            // with the same mechanism and the same fix.
            //
            // WHY THIS AND NOT A RENAME. Two options were available: move the
            // increment inside the gate, or rename/retarget the row. Renaming
            // is not available -- `DRAW-CP-DRAWN` is one of the 244 markers
            // tools/soak_census.py tracks, and its format literal
            // ("DRAW-CP-DRAWN #{} prim={} ...") may not be changed, and the
            // marker name IS part of that literal. So the row keeps its name and
            // its format and becomes TRUE instead: the gate is
            // `cpIssuedDrawCall`, which is the backend's own
            // m_stats.drawsIssued having advanced across the draw call, i.e. the
            // draw call was recorded into the command list. A weaker gate
            // (`beganFrame`) was rejected for the same reason the old
            // unconditional increment was: it would still report "drawn" for a
            // frame whose body refused at D3D12-DRAW-RETURN before
            // DrawInstanced.
            //
            // NOTHING BECOMES ABSENT. The count this used to carry -- commands
            // that reached this site and did NOT become a draw call -- is now
            // its own row, DRAW-CP-DRAWN-SKIPPED below, on its own counter, so
            // "the draw was refused" and "the draw was never attempted" stay
            // two different numbers and neither is lost.
            if (cpIssuedDrawCall) {
                s_1sDrawn[primSlot].fetch_add(1, std::memory_order_relaxed);
                {   // TEMP-CHARPS: the character shader (docs/INV_CHARACTER.md): tex0 * (saturate(N.L)*c39 + c26). Default off.
                }

                const uint32_t dn =
                    s_cpDrawn.fetch_add(1, std::memory_order_relaxed) + 1;
                if (dn <= 8 || (dn % 20000) == 0) {  // MD-32: was 200
                    MCLA_LOG_DEBUG("TEMP-DRAW-DRAWN #{} prim={} topo={} vertices={} "
                                   "indexed={} stride={}",
                                   dn, dc.primitiveTopology,
                                   static_cast<uint32_t>(desc.topology), vertexCount,
                                   indexed ? 1u : 0u, dc.vbStride);
                    // F-633: meta.tcCount / meta.tcMaxIndex are the static transcode
                    // buffer's previous contents on a non-indexed draw - nothing
                    // wrote them - so they are reported as the guest's own
                    // numbers instead of a stale value from an earlier kDMA draw.
                    MCLA_LOG_INFO("DRAW-CP-DRAWN #{} prim={} topo={} vertices={} "
                                  "indexed={} indices={} maxIndex={} fmt=vf{}:dxgi{}:{}B "
                                  "stride={} descs={}/{}",
                                  dn, dc.primitiveTopology,
                                  static_cast<uint32_t>(desc.topology), vertexCount,
                                  indexed ? 1u : 0u, indexed ? meta.tcCount : 0u,
                                  indexed ? meta.tcMaxIndex : 0u, dc.vbFormat.xenos,
                                  dc.vbFormat.dxgi, dc.vbFormat.bytes, dc.vbStride,
                                  dc.vbFullCount, dc.vbDescCount);
                }
            } else {
                s_1sSkipped[primSlot].fetch_add(1, std::memory_order_relaxed);
                static StatCtr<uint32_t> s_drawSkipped{0};
                const uint32_t sn = s_drawSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
                if (sn <= 8 || (sn % 200) == 0) {
                    MCLA_LOG_DEBUG("TEMP-DRAW-SKIPPED #{} pipeStop={}", sn, pipeStop);
                }
                // The new row. `pipeStop` is the reason, taken from the same
                // string DRAW-CP-PIPE prints, so the two can be read together
                // without a lookup table. begin-frame-false means the gate never
                // opened -- the backend's D3D12-BEGINFRAME-REFUSE row names
                // which of its five exits -- and draw-refused means the gate
                // opened and the draw body refused, which
                // D3D12-DRAW-RETURN names.
                const uint32_t sk =
                    s_cpDrawnSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
                if (sk <= 8 || (sk % 200) == 0) {
                    MCLA_LOG_WARN(
                        "DRAW-CP-DRAWN-SKIPPED #{} draw={} stop={} beganFrame={} "
                        "issuedDrawCall=0 pso={} drawnTotal={} | this command "
                        "reached the draw site and NO draw call was recorded, so "
                        "it is deliberately NOT counted in DRAW-CP-DRAWN and NOT "
                        "in DRAW-CP-CENSUS drawn=. stop=begin-frame-false is the "
                        "backend gate (D3D12-BEGINFRAME-REFUSE names which exit); "
                        "stop=draw-refused is inside the draw body "
                        "(D3D12-DRAW-RETURN names which gate); "
                        "stop=no-pso-at-draw means there was no PSO to draw with.",
                        sk, n, pipeStop, cpIssuedDrawCall ? 1 : 0,
                        prep.pso ? 1u : 0u,
                        s_cpDrawn.load(std::memory_order_relaxed));
                }
            }
            if (std::strcmp(pipeStop, "drawn") == 0) LogPipeRow(dc, prep, meta, pipeStop);
            // ------------------------------------------------------------------
            // F-725: THE POST-DRAW PIXEL SAMPLE. This is where a framebuffer
            // sample belongs, and it is not inside RenderCommand::PRESENT.
            //
            // MEASURED on build/w699.log: both `PRESENT-FB sample` rows are
            // timestamped 05:28:28.187 and .188, the two `DRAW-CP-DRAWN` rows
            // are .209 and .221. Every sample in that log predates every draw,
            // by 21 ms and 33 ms. It is not a sampling accident: PRESENT-FB is
            // emitted from inside `case RenderCommand::PRESENT` and only when
            // blitVA != 0, and it reads the GUEST's linear surface
            // (va=B96EF2C0) -- a buffer the host's draw never writes. So the
            // sample is in the wrong place in the pipeline (a present, not a
            // draw) AND on the wrong surface (guest memory, not the back
            // buffer), and both had to change before the measurement could mean
            // anything. PRESENT-FB is left exactly where it is: it measures the
            // guest's own framebuffer, which is a different and still-useful
            // fact, and moving it would change the format of a tracked marker.
            //
            // What this row adds is a sample of the surface the draw actually
            // wrote, timed AFTER the DRAW-CP-DRAWN row above. The values come
            // from the backend's own readback (D3D12-DRAW-PIXEL), which copies
            // the back buffer into a D3D12_HEAP_TYPE_READBACK heap buffer and
            // Maps it -- a READBACK heap resource is mappable by definition,
            // which is the one property every other heap type refuses.
            //
            // NEW ROW NAME. `DRAW-CP-PIXEL` occurs 0 times in this file's MCLA_LOG
            // literals and 0 times in build/w699.log, so nothing is appended to
            // and nothing is renamed. The gate is the backend's own
            // RenderThreadPixelsValid(), which is false until a readback has
            // completed: a caller that reaches here before the first readback
            // prints nothing rather than a default-initialised zero.
            if (backend_.RenderThreadPixelsValid()) {
                static StatCtr<uint32_t> pixelRow{0};
                const uint32_t prn = pixelRow.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (prn <= 8u || (prn % 200u) == 0u) {
                    MCLA_LOG_WARN(
                        "DRAW-CP-PIXEL #{} draw={} rgba=({},{},{},{}) at ({},{}) "
                        "rowPitch={} | nonBlack={}/{} ({:.4f}%) maxChannel={} "
                        "meanChannel={:.4f} readbacks={} -- a sample of the "
                        "surface THIS draw wrote, timestamped after DRAW-CP-DRAWN. "
                        "PRESENT-FB samples the guest's linear surface at present "
                        "time and, in build/w699.log, every one of its rows "
                        "predated every draw; this one cannot. Read the value, not "
                        "the presence of the row: rgba is a single probe at the "
                        "viewport centre, and rgb=0,0,0 with alpha=255 is a real "
                        "black frame, not a missing sample. Pair with "
                        "D3D12-DRAW-PIXEL, which is the same measurement one layer "
                        "down where the copy and the Map happen.",
                        prn, n, backend_.RenderThreadPixelR(), backend_.RenderThreadPixelG(),
                        backend_.RenderThreadPixelB(), backend_.RenderThreadPixelA(),
                        backend_.RenderThreadPixelX(), backend_.RenderThreadPixelY(),
                        backend_.RenderThreadPixelRowPitch(),
                        backend_.RenderThreadNonBlackPixels(),
                        backend_.RenderPixelCensusTotal(),
                        backend_.RenderPixelCensusTotal()
                            ? (100.0 * static_cast<double>(backend_.RenderThreadNonBlackPixels()) /
                               static_cast<double>(backend_.RenderPixelCensusTotal()))
                            : 0.0,
                        backend_.RenderThreadMaxChannel(),
                        backend_.RenderThreadMeanChannel(),
                        backend_.DrawReadbackCompletions());
                }
            }
}

void RenderThread::ExecuteDrawBurst(const std::vector<RenderCommand>& burst) {
    static const bool s_noMtPrep = [] {
        const char* e = std::getenv("MCLA_NO_MT_PREP");
        return e && e[0] == '1';
    }();
    mcla::renderer::ClipDisableRescaleConstants rescale{};
    const uint32_t cfgW = D3D12Backend::ConfiguredMtWorkerCount();
    const uint32_t recW = backend_.MtRecorderCount();
    const uint32_t maxW = std::min(cfgW, recW);
    bool allDraws = true;
    for (const RenderCommand& cmd : burst) {
        if (cmd.type != RenderCommand::DRAW_CAPTURED) {
            allDraws = false;
            break;
        }
    }
    if (s_noMtPrep || !d3d12Initialized_ || !burstScratch_ || !allDraws ||
        burst.size() < 8u || maxW <= 1u ||
        !mcla::renderer::QueryClipDisableRescale(&rescale) || !rescale.valid) {
        for (const RenderCommand& cmd : burst) {
            if (cmd.type == RenderCommand::DRAW_CAPTURED) processCommand(cmd);
            else { STALL_STAGE("other-cmd"); processCommand(cmd); }
        }
        return;
    }

    const size_t N = burst.size();
    (void)HarvestWriteWatchForRenderThread();

    BurstScratch& bs = *burstScratch_;
    bs.prep.resize(N);
    bs.meta.resize(N);
    bs.mtOk.assign(N, 0u);
    bs.mtRun.clear();

    const uint32_t baseSeq = g_stage1DrawCount.fetch_add(static_cast<uint32_t>(N), std::memory_order_relaxed);
    for (size_t i = 0u; i < N; ++i) {
        bs.prep[i].drawSeq = static_cast<uint64_t>(baseSeq) + static_cast<uint64_t>(i) + 1ull;
    }

    const uint32_t stage1W = std::min<uint32_t>(maxW, static_cast<uint32_t>(N));
    bs.EnsureWorkers(maxW);

    STALL_STAGE("stage1-end");
    {
        std::atomic<size_t> nextIdx{0};
        bs.DispatchWorkers(stage1W, [&](uint32_t /*wi*/) {
            for (;;) {
                const size_t idx = nextIdx.fetch_add(1u, std::memory_order_relaxed);
                if (idx >= N) break;
                PrepareDrawStage1(burst[idx], bs.prep[idx], bs.meta[idx]);
            }
        });
    }

    if (!backend_.BeginBurstSession()) {
        for (size_t i = 0u; i < N; ++i) {
            if (!bs.prep[i].valid || bs.prep[i].foldedClearRect) continue;
            FinalizeDrawAccounting(burst[i], bs.prep[i], bs.meta[i], false, false, false);
        }
        return;
    }

    // MCLA_HIDE_LEGALS=1 (swarm P2, 2026-11-20): measured root cause is the boot
    // legal-SWF object never unloading; its glyph draws (VS 1A5B2B313B294EAE)
    // bleed into the title. This drops ONLY those draws at the render layer so
    // the logo (a different draw) stays. Opt-in diagnostic, never default.
    static const bool s_hideLegals = [] {
        const char* e = std::getenv("MCLA_HIDE_LEGALS");
        return e && e[0] == '1';
    }();
    auto isLegalGlyphDraw = [&](size_t idx) -> bool {
        return s_hideLegals && bs.meta[idx].pipeVsHash == 0x1A5B2B313B294EAEull;
    };
    auto runSerialOne = [&](size_t idx) {
        if (isLegalGlyphDraw(idx)) {
            static std::atomic<uint32_t> s_legalDropped{0};
            const uint32_t k = s_legalDropped.fetch_add(1) + 1u;
            if (k <= 8u || (k % 1000u) == 0u)
                MCLA_LOG_ERROR("LEGALS-DROP #{} draw={} (legal glyph VS, MCLA_HIDE_LEGALS=1)", k, idx);
            FinalizeDrawAccounting(burst[idx], bs.prep[idx], bs.meta[idx], false, false, false);
            return;
        }
        STALL_STAGE("drawcall");
        const uint64_t cpDrawCallsBefore = backend_.Stats().drawsIssued;
        const bool beganFrame = backend_.BeginFrameForDraw();
        const bool drew = beganFrame
            ? backend_.DrawDynamicMeshWithPipeline(bs.prep[idx].desc, bs.prep[idx].pso)
            : false;
        const bool cpIssuedDrawCall =
            beganFrame && (backend_.Stats().drawsIssued != cpDrawCallsBefore);
        FinalizeDrawAccounting(burst[idx], bs.prep[idx], bs.meta[idx], beganFrame, drew, cpIssuedDrawCall);
    };

    auto alignUp256 = [](size_t v) -> size_t { return (v + 255ull) & ~size_t(255ull); };

    auto flushMtBatch = [&]() {
        const size_t M = bs.mtRun.size();
        if (M == 0u) return;

        // Avoid mid-frame FlushActiveCommandLists() (Close + ExecuteCommandLists +
        // Reset + InvalidateDrawCaches) on small passes; bisect_no_ww vs bisect_no_mt_ww
        // measured -4 FPS when flushing small passes mid-frame.
        static const size_t s_mtRecThreshold = []() -> size_t {
            if (const char* e = std::getenv("MCLA_MT_RECORD"); e && e[0] == '1') return 8u;
            return 64u;
        }();
        const uint32_t recCount = std::min<uint32_t>(maxW, static_cast<uint32_t>(M));
        // F9 probe needs every draw on the main list in order (its pixel copies live there).
        bool useMt = (M >= s_mtRecThreshold && recCount >= 2u) && !D3D12Backend::F9Active();
        if (useMt) {
            for (uint32_t r = 0u; r < recCount; ++r) {
                const D3D12Backend::DrawRecorder* rec = backend_.MtRecorder(r);
                if (!rec || !rec->listOpen) {
                    useMt = false;
                    break;
                }
                const size_t beg = (static_cast<size_t>(r) * M) / recCount;
                const size_t end = (static_cast<size_t>(r + 1u) * M) / recCount;
                size_t estUp = 0u;
                uint32_t estSrv = 0u;
                for (size_t k = beg; k < end; ++k) {
                    const auto& p = bs.prep[bs.mtRun[k]];
                    const auto& d = p.desc;
                    const uint32_t sc = (d.streamCount > 0u && d.streamCount <= 4u) ? d.streamCount : 1u;
                    for (uint32_t s = 0u; s < sc; ++s) {
                        if (d.residentVbGpu[s] == 0u) {
                            const uint32_t sz = (d.streamCount > 0u) ? d.streams[s].bytesSize : d.vertexBytesSize;
                            if (sz > 0u) estUp += alignUp256(sz);
                        }
                    }
                    if (d.indexed && d.residentIbGpu == 0u && d.cachedIndexGpu == 0u && d.indexBytesSize > 0u) {
                        estUp += alignUp256(d.indexBytesSize);
                    }
                    if (p.vsConstBytesSize > 0u) estUp += 4352u;
                    if (p.psConstBytesSize > 0u) estUp += 4352u;
                    if (p.bindTextures) ++estSrv;
                }
                if (rec->uploadOffset + estUp > rec->uploadSubRegionSize ||
                    rec->srvRingOffset + estSrv > rec->srvRingTables) {
                    useMt = false;
                    break;
                }
            }
        }

        if (!useMt) {
            for (size_t idx : bs.mtRun) {
                runSerialOne(idx);
            }
            bs.mtRun.clear();
            return;
        }

        STALL_STAGE("stage3");
        bs.DispatchWorkers(recCount, [&](uint32_t wi) {
            D3D12Backend::DrawRecorder* rec = backend_.MtRecorder(wi);
            const size_t beg = (static_cast<size_t>(wi) * M) / recCount;
            const size_t end = (static_cast<size_t>(wi + 1u) * M) / recCount;
            for (size_t k = beg; k < end; ++k) {
                const size_t idx = bs.mtRun[k];
                bs.mtOk[idx] = isLegalGlyphDraw(idx)
                    ? 0u
                    : (backend_.DrawPreparedStage3(bs.prep[idx], *rec) ? 1u : 0u);
            }
        });
        (void)backend_.FlushActiveCommandLists();
        for (size_t idx : bs.mtRun) {
            const bool ok = (bs.mtOk[idx] != 0u);
            FinalizeDrawAccounting(burst[idx], bs.prep[idx], bs.meta[idx], true, ok, ok);
        }
        bs.mtRun.clear();
    };

    bool passActive = false;
    uint64_t curRtKey = 0u;
    uint64_t curDsKey = 0u;
    uint32_t curGuestH = 0u;

    for (size_t i = 0u; i < N; ++i) {
        auto& p = bs.prep[i];
        if (!p.valid) continue;

        if (p.foldedClearRect) {
            flushMtBatch();
            passActive = false;
            STALL_STAGE("drawcall");
            if (backend_.ExecuteFoldedClearRect(p)) {
                s_1sDrawn[bs.meta[i].primSlot].fetch_add(1, std::memory_order_relaxed);
                s_cpDrawn.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }

        const uint32_t sc = (p.desc.streamCount > 0u && p.desc.streamCount <= 4u) ? p.desc.streamCount : 1u;
        for (uint32_t s = 0u; s < sc; ++s) {
            if (p.vbCleanHit[s] || p.vbCheckPersistent[s]) {
                const void* vp = (s == 0u) ? (p.vb.empty() ? nullptr : p.vb.data())
                                           : (p.vbStreamsExtra[s - 1u].empty() ? nullptr : p.vbStreamsExtra[s - 1u].data());
                const uint32_t vsz = p.vbCleanHit[s]
                    ? p.desc.residentVbBytes[s]
                    : ((s == 0u) ? static_cast<uint32_t>(p.vb.size())
                                 : static_cast<uint32_t>(p.vbStreamsExtra[s - 1u].size()));
                const D3D12_GPU_VIRTUAL_ADDRESS gva = ResolveOrPromoteR1Geo(
                    backend_, p.vbPersistentKey[s], p.vbContentHash[s],
                    vp, vsz, p.streamStrides[s],
                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    p.vbBasePage[s], p.vbPageCount[s], p.vbEpochSnap[s],
                    0u, p.vbCleanHit[s]);
                if (gva != 0u) {
                    p.desc.residentVbGpu[s] = gva;
                    p.desc.residentVbBytes[s] = p.vbCleanHit[s] ? p.desc.residentVbBytes[s] : vsz;
                    p.desc.residentVbStride[s] = p.streamStrides[s];
                } else if (p.vbCleanHit[s]) {
                    p.desc.residentVbGpu[s] = 0u;
                }
            }
        }
        if (p.desc.indexed && (p.ibCleanHit || p.ibCheckPersistent)) {
            const uint32_t isz = p.ibCleanHit ? p.desc.residentIbBytes
                                              : static_cast<uint32_t>(p.ib.size());
            const D3D12_GPU_VIRTUAL_ADDRESS igva = ResolveOrPromoteR1Geo(
                backend_, p.ibPersistentKey, p.ibContentHash,
                p.ib.empty() ? nullptr : p.ib.data(),
                isz, 0u, D3D12_RESOURCE_STATE_INDEX_BUFFER,
                p.ibBasePage, p.ibPageCount, p.ibEpochSnap,
                bs.meta[i].tcMaxIndex, p.ibCleanHit);
            if (igva != 0u) {
                p.desc.residentIbGpu = igva;
                p.desc.residentIbBytes = isz;
            } else if (p.ibCleanHit) {
                p.desc.residentIbGpu = 0u;
            }
        }

        uint64_t rtKey = 0u, dsKey = 0u;
        uint32_t guestH = 0u;
        backend_.ComputeGuestPassKeys(p.desc, rtKey, dsKey, guestH);

        if (!passActive || rtKey != curRtKey || dsKey != curDsKey || guestH != curGuestH) {
            flushMtBatch();
            curRtKey = rtKey;
            curDsKey = dsKey;
            curGuestH = guestH;
            passActive = backend_.BeginPreparedPass(p.desc, guestH);
        }

        if (passActive && !p.desc.usePrecompiledPipeline) {
            (void)backend_.PrepareDrawStage2(p);
        }

        if (!passActive || !p.stage2Ready || p.forceMainThread || p.desc.usePrecompiledPipeline) {
            flushMtBatch();
            passActive = false;
            runSerialOne(i);
            continue;
        }

        bs.mtRun.push_back(i);
    }

    flushMtBatch();
}


// MCLA_RENDER_2STAGE record side (B = this render thread). Design: see TwoStageRing.
void RenderThread::RunTwoStage() {
    ring2_ = std::make_unique<TwoStageRing>();
    TwoStageRing& r = *ring2_;
    MCLA_LOG_WARN("RENDER-2STAGE on: prep thread + record thread, ring={} slots (off: MCLA_NO_RENDER_2STAGE=1)",
                  TwoStageRing::kSlots);
    prepThread_ = std::thread(&RenderThread::PrepThreadMain, this);
    for (uint64_t t = 0;; ++t) {
        STALL_STAGE("idle-wait");
        TwoStageRing::WaitAtLeast(r.head, t + 1u);
        TwoStageRing::Slot& s = r.slots[t % TwoStageRing::kSlots];
        if (s.kind == TwoStageRing::kStop) break;
        if (s.kind == TwoStageRing::kDraw) {
            STALL_STAGE("stage2");
            RecordPreparedDraw(*s.cmd, s.prep, s.meta);
        } else if (s.kind == TwoStageRing::kCmd) {
            STALL_STAGE("other-cmd");
            if (g_tempPerfStages) {
                const unsigned ty = unsigned(s.cmd->type) & 15u;
                const int64_t t0 = PerfNowNs();
                processCommand(*s.cmd);
                s_perfOcNs[ty].fetch_add(PerfNowNs() - t0, std::memory_order_relaxed);
                s_perfOcN[ty].fetch_add(1, std::memory_order_relaxed);
            } else {
                processCommand(*s.cmd);
            }
        }
        s.cmd = nullptr;
        g_commandQueue.pop_done();
        TwoStageRing::Publish(r.tail, t + 1u);
    }
    if (prepThread_.joinable()) prepThread_.join();
}

// MCLA_RENDER_2STAGE prep side (A). Never records D3D12 commands.
void RenderThread::PrepThreadMain() {
    t_isRenderThread = true;  // STALL_STAGE / consumer-progress stamps from stage 1
    tl_perfBank = 1;
    mcla::diag::NameThisThread("render-prep");
    TwoStageRing& r = *ring2_;
    for (uint64_t h = 0;; ++h) {
        STALL_STAGE("idle-wait");
        if (h >= TwoStageRing::kSlots) TwoStageRing::WaitAtLeast(r.tail, h + 1u - TwoStageRing::kSlots);
        TwoStageRing::Slot& s = r.slots[h % TwoStageRing::kSlots];
        RenderCommand* const cmd = shouldStop_ ? nullptr : g_commandQueue.peek_next();
        s.cmd = cmd;
        if (!cmd) {
            s.kind = TwoStageRing::kStop;
            TwoStageRing::Publish(r.head, h + 1u);
            break;
        }
        if (!AdmitCommand(*cmd)) {
            s.kind = TwoStageRing::kSkip;
        } else if (cmd->type == RenderCommand::DRAW_CAPTURED) {
            if (d3d12Initialized_) {
                (void)HarvestWriteWatchForRenderThread();
                PrepareDrawStage1(*cmd, s.prep, s.meta);
                s.kind = TwoStageRing::kDraw;
            } else {
                s.kind = TwoStageRing::kSkip;
            }
        } else {
            s.kind = TwoStageRing::kCmd;
            if (cmd->type == RenderCommand::PRESENT && !IsHeartbeatPresent(*cmd))
                KickPresentHarvest(std::get<PresentCommand>(cmd->data).enqNs);
        }
        // Barrier-free: log-only commands, and commands whose only effects are on B-owned state (command
        // list, RT/DS caches, swap chain). CLEAR_GUEST touches only m_rtCache/m_dsCache + the list; a guest
        // PRESENT's one stage-1-visible effect is the forced write-watch harvest, which A waits for itself.
        // Still barriered: RESOLVE_GUEST (render-scale resolve bases read by stage 1's XuvRsMask), QUERY_OP
        // (the active-query set read by stage 1), RESIZE (width/height), INIT_D3D12 and anything new.
        // Decided BEFORE Publish: once published, B may pop_done() and recycle *cmd.
        bool needsBarrier = false;
        int64_t presentEnqNs = 0;
        if (s.kind == TwoStageRing::kCmd) {
            switch (cmd->type) {
                case RenderCommand::SET_RENDER_STATE: case RenderCommand::SET_PIPELINE_STATE:
                case RenderCommand::NOOP: case RenderCommand::DRAW_INDEXED: case RenderCommand::SET_VERTEX_BUFFERS:
                case RenderCommand::SET_INDEX_BUFFER: case RenderCommand::CLEAR_GUEST: case RenderCommand::DRAW_MARKER:
                    break;
                case RenderCommand::PRESENT:
                    if (d3d12Initialized_ && !IsHeartbeatPresent(*cmd))
                        presentEnqNs = std::get<PresentCommand>(cmd->data).enqNs;
                    break;
                default:
                    needsBarrier = true;
            }
        }
        TwoStageRing::Publish(r.head, h + 1u);
        if (presentEnqNs != 0) {
            STALL_STAGE("pr-harvest");
            (void)HarvestWriteWatchForRenderThread(true, presentEnqNs);
        }
        if (needsBarrier) {
            // Ordering barrier: the next draw's stage 1 must see this command's side effects.
            STALL_STAGE("a-barrier");
            TwoStageRing::WaitAtLeast(r.tail, h + 1u);
        }
    }
    TempPerfStagePause();
}

} // namespace mcla::native
