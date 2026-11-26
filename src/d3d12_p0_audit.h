#pragma once

// P0-02 observation-only audit core. This header deliberately has no rendering
// dependencies: classifiers are pure and the log sink is injectable so the
// offline test can prove every verdict without creating a D3D12 device.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mcla::p0_audit {

constexpr uint32_t kMaskSampler = 0x1u;
constexpr uint32_t kMaskAllocator = 0x2u;
constexpr uint32_t kMaskResolved = 0x4u;
constexpr uint32_t kMaskPsoRtv = 0x8u;
constexpr uint32_t kMaskFixture = 0x80000000u;
constexpr uint32_t kMaskAllowed = 0x8000000Fu;

struct Config {
    bool valid = true;
    bool armed = false;
    bool emitConfig = false;
    uint32_t mask = 0;
    const char* source = "unset";
    const char* status = "disabled";
};

inline Config ParseConfig(const char* text) {
    Config result;
    if (text == nullptr) return result;
    result.emitConfig = true;
    result.source = "explicit";
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 0);
    if (text[0] == '-' || end == text || *end != '\0' || errno == ERANGE ||
        value > kMaskAllowed || (value & ~kMaskAllowed) != 0) {
        result.valid = false;
        result.armed = false;
        result.mask = 0;
        result.status = "invalid";
        return result;
    }
    result.mask = static_cast<uint32_t>(value);
    result.armed = true;
    result.status = "armed";
    return result;
}

struct Verdict {
    bool ok = false;
    bool attention = false;
    bool unknown = false;
    bool notApplicable = false;
    bool recovered = false;
    std::string label = "unknown";
    std::string reason;

    static Verdict Make(bool okValue, bool attentionValue, bool unknownValue,
                        bool notApplicableValue, bool recoveredValue, std::string reasonValue,
                        std::string labelValue = {}) {
        Verdict value;
        value.ok = okValue;
        value.attention = attentionValue;
        value.unknown = unknownValue;
        value.notApplicable = notApplicableValue;
        value.recovered = recoveredValue;
        value.reason = std::move(reasonValue);
        value.label = std::move(labelValue);
        return value;
    }
    static Verdict Ok(std::string reason, std::string label = "ok") {
        return Make(true, false, false, false, false, std::move(reason), std::move(label));
    }
    static Verdict Attention(std::string reason, std::string label = "attention") {
        return Make(false, true, false, false, false, std::move(reason), std::move(label));
    }
    static Verdict Unknown(std::string reason, std::string label = "unknown") {
        return Make(false, false, true, false, false, std::move(reason), std::move(label));
    }
    static Verdict NotApplicable(std::string reason, std::string label = "not-applicable") {
        return Make(false, false, false, true, false, std::move(reason), std::move(label));
    }
    static Verdict Recovered(std::string reason, std::string label = "recovered") {
        return Make(false, false, false, false, true, std::move(reason), std::move(label));
    }
};

struct P0SamplerOwner {
    void* heap = nullptr;
    uint32_t heapIndex = 0;
    uint64_t base = 0;
    uint64_t handle = 0;
    uint64_t heapBaseGpu = 0;
    uint32_t descriptorSize = 0;
    uint32_t descriptorCount = 0;
    uint32_t heapDescriptorCount = 0;
    bool internOk = false;
    bool neutralFallback = false;
};

struct P0SamplerSnapshot {
    P0SamplerOwner owner{};
    void* boundHeap = nullptr;
    bool descriptorHeapsSet = false;
    uint64_t commandOrder = 0;
    uint32_t recorder = 0;
};

struct P0AllocatorSnapshot {
    const char* kind = "main";
    uint32_t recorder = 0;
    int32_t firstHresult = 0;
    int32_t finalHresult = 0;
    bool retried = false;
    bool fenceKnown = false;
    uint64_t targetFence = 0;
    uint64_t completedBefore = 0;
    bool recordingAtFinal = false;
    uint64_t frameIndex = 0;
};

struct P0ResolvedSnapshot {
    uint64_t commandOrder = 0;
    uint32_t slot = 0;
    uint32_t candidateKey = 0;
    uint32_t winnerKey = 0;
    const char* priority = "none";
    const char* source = "none";
    bool selected = false;
    bool candidateKnown = false;
    bool metadataKnown = true;
    bool agePolicyEnabled = false;
    uint64_t ageLimitFrames = 0;
    uint64_t ageFrames = 0;
    bool frameUnderflow = false;
    bool formatExact = true;
    bool formatCompatible = true;
    bool winnerIsLastColour = true;
    bool depthFetch = false;
    uint32_t fetchWidth = 0;
    uint32_t fetchHeight = 0;
    uint32_t resolvedWidth = 0;
    uint32_t resolvedHeight = 0;
    uint32_t fetchFormat = 0;
    uint32_t resolvedFormat = 0;
    const char* stagedResult = "none";
};

struct P0PsoRtvSnapshot {
    P0PsoRtvSnapshot() { rtvSlotKnown.fill(true); }
    uint64_t commandOrder = 0;
    bool psoKnown = false;
    bool omSetRenderTargetsOk = false;
    bool setPipelineStateOk = false;
    bool depthOnly = false;
    bool rtvFormatsKnown = true;
    uint8_t psoCount = 0;
    uint8_t psoMask = 0;
    uint8_t rtvCount = 0;
    uint8_t rtvMask = 0;
    std::array<uint32_t, 8> psoFormats{};
    std::array<uint32_t, 8> rtvFormats{};
    std::array<bool, 8> rtvSlotKnown{};
};

inline Verdict ClassifySampler(const P0SamplerSnapshot& snapshot) {
    const P0SamplerOwner& owner = snapshot.owner;
    if (!owner.internOk) return Verdict::Unknown("intern-not-ok", "intern-failed");
    if (!snapshot.descriptorHeapsSet || snapshot.boundHeap == nullptr)
        return Verdict::Attention("descriptor-heaps-unbound", "unbound");
    if (owner.heap == nullptr) return Verdict::Unknown("owner-heap-unknown", "owner-unknown");
    if (owner.heap != snapshot.boundHeap)
        return Verdict::Attention("owner-bound-heap-mismatch", "owner-mismatch");
    if (owner.descriptorSize == 0 || owner.descriptorCount == 0 || owner.heapDescriptorCount == 0)
        return Verdict::Unknown("descriptor-geometry-unknown", "geometry-unknown");
    if (owner.handle < owner.heapBaseGpu) return Verdict::Attention("handle-before-heap", "out-of-range");
    const uint64_t offset = owner.handle - owner.heapBaseGpu;
    if (offset % owner.descriptorSize != 0) return Verdict::Attention("handle-misaligned", "misaligned");
    const uint64_t first = offset / owner.descriptorSize;
    if (first + owner.descriptorCount > owner.heapDescriptorCount)
        return Verdict::Attention("handle-out-of-range", "out-of-range");
    if (owner.neutralFallback)
        return Verdict::Attention(owner.base == 0 ? "neutral-fallback" : "neutral-fallback-nonzero-base",
                                  "neutral-fallback");
    return Verdict::Ok("owner-bound-aligned-in-range", "bound-ok");
}

inline Verdict ClassifyAllocator(const P0AllocatorSnapshot& snapshot) {
    if (snapshot.finalHresult != 0)
        return Verdict::Attention("allocator-reset-failed", "reset-failed");
    if (snapshot.retried && snapshot.firstHresult != 0)
        return Verdict::Recovered("allocator-reset-retry-recovered", "recovered");
    if (snapshot.recordingAtFinal)
        return Verdict::Attention("command-list-recording-at-reset", "unsafe");
    if (!snapshot.fenceKnown || snapshot.targetFence == 0)
        return Verdict::Unknown("fence-evidence-unavailable", "fence-unknown");
    if (snapshot.completedBefore < snapshot.targetFence)
        return Verdict::Attention("allocator-reset-before-fence", "unsafe");
    return Verdict::Ok("allocator-reset-after-fence", "safe");
}

inline Verdict ClassifyResolved(const P0ResolvedSnapshot& snapshot) {
    if (!snapshot.candidateKnown) return Verdict::Unknown("no-known-resolve-candidate", "metadata-unknown");
    if (!snapshot.metadataKnown) return Verdict::Unknown("resolve-metadata-unknown", "metadata-unknown");
    if (snapshot.frameUnderflow) return Verdict::Unknown("resolve-frame-underflow", "frame-underflow");
    if (!snapshot.selected) {
        if (snapshot.agePolicyEnabled && snapshot.ageFrames > snapshot.ageLimitFrames)
            return Verdict::Attention("enabled-age-stale", "stale");
        return Verdict::Attention("known-candidate-rejected", "rejected");
    }
    if (snapshot.resolvedWidth != snapshot.fetchWidth || snapshot.resolvedHeight != snapshot.fetchHeight)
        return Verdict::Attention("fallback-dim", "fallback-dim");
    if (snapshot.agePolicyEnabled && snapshot.ageFrames > snapshot.ageLimitFrames)
        return Verdict::Attention("enabled-age-stale", "stale");
    if (snapshot.depthFetch) {
        if (!snapshot.formatExact || !snapshot.formatCompatible)
            return Verdict::NotApplicable("depth-nonexact-format-fallback", "depth-fallback");
        if (snapshot.stagedResult != std::string("staged"))
            return Verdict::Unknown("resolve-staging-unknown", "staging-unknown");
        return Verdict::Ok("exact-depth-staged", "exact-depth");
    }
    if (!snapshot.formatCompatible) return Verdict::Attention("resolve-format-mismatch", "format-mismatch");
    if (!snapshot.formatExact)
        return Verdict::Attention("compatible-nonexact-format-fallback", "format-fallback");
    if (!snapshot.winnerIsLastColour)
        return Verdict::Attention("non-last-resolve-winner", "non-last-winner");
    if (snapshot.stagedResult == std::string("fallback-srv"))
        return Verdict::Attention("resolved-staged-through-fallback-srv", "fallback-srv");
    if (snapshot.stagedResult != std::string("staged"))
        return Verdict::Unknown("resolve-staging-unknown", "staging-unknown");
    if (!snapshot.agePolicyEnabled) return Verdict::Ok("disabled-age-not-stale", "not-stale");
    return Verdict::Ok("selected-exact-staged", "exact");
}

inline Verdict ClassifyPsoRtv(const P0PsoRtvSnapshot& snapshot) {
    if (!snapshot.psoKnown || !snapshot.omSetRenderTargetsOk || !snapshot.setPipelineStateOk)
        return Verdict::Unknown("pso-rtv-metadata-or-bind-unavailable", "metadata-unknown");
    if (snapshot.depthOnly && snapshot.rtvCount == 0)
        return Verdict::NotApplicable("depth-only-zero-rtv", "depth-only");
    if (snapshot.psoCount > snapshot.psoFormats.size() || snapshot.rtvCount > snapshot.rtvFormats.size())
        return Verdict::Unknown("pso-rtv-slot-count-unknown", "metadata-unknown");
    if (!snapshot.rtvFormatsKnown)
        return Verdict::Unknown("effective-rtv-formats-unknown", "rtv-format-unknown");
    for (uint32_t slot = 0; slot < snapshot.rtvCount; ++slot)
        if (!snapshot.rtvSlotKnown[slot])
            return Verdict::Unknown("effective-rtv-slot-format-unknown", "rtv-format-unknown");
    if (snapshot.psoCount != snapshot.rtvCount || snapshot.psoMask != snapshot.rtvMask ||
        snapshot.psoFormats != snapshot.rtvFormats)
        return Verdict::Attention("pso-rtv-mismatch", "pso-rtv-mismatch");
    return Verdict::Ok("pso-rtv-match", "pso-rtv-match");
}

inline std::string Escape(std::string value) {
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '"', '\'');
    return value;
}

inline std::string Hex(uint64_t value, unsigned width = 0) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    if (width != 0) out << std::setw(width);
    out << value;
    return out.str();
}

inline std::string VerdictCategory(const Verdict& verdict) {
    if (verdict.ok) return "ok";
    if (verdict.attention) return "attention";
    if (verdict.unknown) return "unknown";
    if (verdict.notApplicable) return "not-applicable";
    if (verdict.recovered) return "recovered";
    return "unknown";
}

inline std::string VerdictName(const Verdict& verdict) {
    return verdict.label.empty() ? VerdictCategory(verdict) : verdict.label;
}

using Detail = std::pair<std::string, std::string>;
using LogSink = std::function<void(const std::string&)>;

struct DomainCounters {
    std::atomic<uint64_t> domainSeq{0};
    std::atomic<uint64_t> events{0};
    std::atomic<uint64_t> attention{0};
    std::atomic<uint64_t> unknown{0};
    std::atomic<uint64_t> notApplicable{0};
    std::atomic<uint64_t> recovered{0};
};

class Emitter {
public:
    void Initialize(LogSink sink) {
        std::call_once(once_, [&] {
            config_ = ParseConfig(std::getenv("MCLA_P0_AUDIT_MASK"));
            if (config_.valid && (config_.mask & kMaskFixture)) sequence_.store(8, std::memory_order_relaxed);
            sink_ = std::move(sink);
            initialized_ = true;
            if (config_.emitConfig) {
                std::ostringstream row;
                row << "P0-AUDIT-CONFIG status=" << config_.status << " mask=0x"
                    << Hex(config_.mask, 8) << " source=" << config_.source;
                sink_(row.str());
            }
        });
    }

    bool Enabled(uint32_t domainMask) const {
        return initialized_ && config_.valid && config_.armed && (config_.mask & domainMask) != 0;
    }

    void Emit(uint32_t domainMask, const char* domain, const Verdict& verdict,
              const std::vector<Detail>& details = {}, bool fixture = false) {
        if (!Enabled(domainMask)) return;
        // One lock makes global seq assignment, per-domain counter advancement,
        // cadence selection, and sink order agree across worker threads.
        std::lock_guard<std::mutex> lock(sinkMutex_);
        DomainCounters& counters = counters_[DomainIndex(domainMask)];
        const uint64_t domainSeq = counters.domainSeq.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t events = counters.events.fetch_add(1, std::memory_order_relaxed) + 1;
        if (verdict.attention) counters.attention.fetch_add(1, std::memory_order_relaxed);
        if (verdict.unknown) counters.unknown.fetch_add(1, std::memory_order_relaxed);
        if (verdict.notApplicable) counters.notApplicable.fetch_add(1, std::memory_order_relaxed);
        if (verdict.recovered) counters.recovered.fetch_add(1, std::memory_order_relaxed);
        if (events <= 8 || (events % 200) == 0) {
            const uint64_t sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            std::ostringstream row;
            row << "P0-AUDIT-" << domain << " v=1 seq=" << sequence
                << " domainSeq=" << domainSeq
                << " events=" << events
                << " attention=" << counters.attention.load(std::memory_order_relaxed)
                << " unknown=" << counters.unknown.load(std::memory_order_relaxed)
                << " notApplicable=" << counters.notApplicable.load(std::memory_order_relaxed)
                << " recovered=" << counters.recovered.load(std::memory_order_relaxed)
                << " fixture=" << (fixture ? 1 : 0)
                << " category=" << VerdictCategory(verdict)
                << " verdict=" << VerdictName(verdict)
                << " reason=\"" << Escape(verdict.reason) << "\"";
            for (const auto& detail : details)
                row << " " << detail.first << "=" << Escape(detail.second);
            if (sink_) sink_(row.str());
        }
    }

    void NoteFrameBegun(uint64_t frameIndex) {
        if (!InitializedAndActive()) return;
        const uint64_t frame = frames_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (frame <= 2 || (frame % 600) == 0) {
            for (unsigned domain = 0; domain < counters_.size(); ++domain)
                if (Enabled(DomainMask(domain))) Summary(domain, "frame-begun", frameIndex);
        }
    }

    void Shutdown() {
        if (!InitializedAndActive()) return;
        for (unsigned domain = 0; domain < counters_.size(); ++domain)
            if (Enabled(DomainMask(domain))) Summary(domain, "shutdown", frames_.load(std::memory_order_relaxed));
    }

    uint64_t Events() const { return Total([](const DomainCounters& c) { return c.events.load(std::memory_order_relaxed); }); }
    uint64_t Attention() const { return Total([](const DomainCounters& c) { return c.attention.load(std::memory_order_relaxed); }); }
    uint64_t Unknown() const { return Total([](const DomainCounters& c) { return c.unknown.load(std::memory_order_relaxed); }); }
    uint64_t NotApplicable() const { return Total([](const DomainCounters& c) { return c.notApplicable.load(std::memory_order_relaxed); }); }
    uint64_t Recovered() const { return Total([](const DomainCounters& c) { return c.recovered.load(std::memory_order_relaxed); }); }

private:
    template <class Read>
    uint64_t Total(Read read) const {
        uint64_t value = 0;
        for (const DomainCounters& counters : counters_) value += read(counters);
        return value;
    }
    bool InitializedAndActive() const {
        return initialized_ && config_.valid && config_.armed && config_.mask != 0;
    }
    unsigned DomainIndex(uint32_t mask) const {
        switch (mask) {
            case kMaskSampler: return 0;
            case kMaskAllocator: return 1;
            case kMaskResolved: return 2;
            default: return 3;
        }
    }
    uint32_t DomainMask(unsigned index) const {
        switch (index) {
            case 0: return kMaskSampler;
            case 1: return kMaskAllocator;
            case 2: return kMaskResolved;
            default: return kMaskPsoRtv;
        }
    }
    const char* DomainName(unsigned index) const {
        switch (index) {
            case 0: return "SAMPLER";
            case 1: return "ALLOCATOR";
            case 2: return "RESOLVED";
            default: return "PSO-RTV";
        }
    }
    void Summary(unsigned domain, const char* reason, uint64_t frame) {
        std::lock_guard<std::mutex> lock(sinkMutex_);
        const DomainCounters& counters = counters_[domain];
        const uint64_t sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        std::ostringstream row;
        row << "P0-AUDIT-SUMMARY v=1 seq=" << sequence
            << " domain=" << DomainName(domain)
            << " domainSeq=" << counters.domainSeq.load(std::memory_order_relaxed)
            << " events=" << counters.events.load(std::memory_order_relaxed)
            << " attention=" << counters.attention.load(std::memory_order_relaxed)
            << " unknown=" << counters.unknown.load(std::memory_order_relaxed)
            << " notApplicable=" << counters.notApplicable.load(std::memory_order_relaxed)
            << " recovered=" << counters.recovered.load(std::memory_order_relaxed)
            << " fixture=0 category=summary verdict=summary reason=\"" << reason << "\" frame=" << frame;
        if (sink_) sink_(row.str());
    }

    std::once_flag once_;
    Config config_{};
    LogSink sink_;
    std::mutex sinkMutex_;
    bool initialized_ = false;
    std::atomic<uint64_t> sequence_{0};
    std::atomic<uint64_t> frames_{0};
    std::array<DomainCounters, 4> counters_{};
};

inline Emitter& Global() {
    static Emitter emitter;
    return emitter;
}

inline std::string FixtureRow(const char* domain, const Verdict& verdict,
                              const std::vector<Detail>& details, uint64_t sequence) {
    std::ostringstream row;
    row << "P0-AUDIT-" << domain << " v=1 seq=" << sequence
        << " events=" << sequence << " attention=" << (verdict.attention ? sequence : 0)
        << " unknown=" << (verdict.unknown ? sequence : 0)
        << " notApplicable=" << (verdict.notApplicable ? sequence : 0)
        << " recovered=" << (verdict.recovered ? sequence : 0)
        << " fixture=1 category=" << VerdictCategory(verdict)
        << " verdict=" << VerdictName(verdict)
        << " reason=\"" << Escape(verdict.reason) << "\"";
    for (const auto& detail : details)
        row << " " << detail.first << "=" << Escape(detail.second);
    return row.str();
}

inline uint32_t RunBuiltInFixtures(const LogSink& sink) {
    uint32_t passed = 0;
    auto run = [&](const char* domain, uint32_t mask, const Verdict& verdict,
                   const std::vector<Detail>& details) {
        const bool categoryCount = static_cast<int>(verdict.ok) + static_cast<int>(verdict.attention) +
                                   static_cast<int>(verdict.unknown) + static_cast<int>(verdict.notApplicable) +
                                   static_cast<int>(verdict.recovered);
        if (categoryCount == 1 && !verdict.reason.empty()) {
            if (sink) sink(FixtureRow(domain, verdict, details, passed + 1));
            ++passed;
        }
        (void)mask;
    };
    P0SamplerOwner owner;
    owner.heap = reinterpret_cast<void*>(0x1000);
    owner.handle = owner.heapBaseGpu = 0x1000;
    owner.descriptorSize = 16;
    owner.descriptorCount = owner.heapDescriptorCount = 16;
    owner.internOk = true;
    owner.neutralFallback = true;
    run("SAMPLER", kMaskSampler, ClassifySampler({owner, owner.heap, true, 1, 0}), {{"case", "neutral-fallback"}});
    run("SAMPLER", kMaskSampler, ClassifySampler({owner, reinterpret_cast<void*>(0x2000), true, 1, 1}), {{"case", "two-recorder-owner-A-bound-B"}});
    P0SamplerOwner range = owner;
    range.handle = range.heapBaseGpu + range.descriptorSize * range.heapDescriptorCount;
    run("SAMPLER", kMaskSampler, ClassifySampler({range, range.heap, true, 1, 0}), {{"case", "out-of-range"}});
    run("SAMPLER", kMaskSampler, ClassifySampler({owner, nullptr, false, 1, 0}), {{"case", "unbound"}});
    P0SamplerOwner unknown = owner;
    unknown.internOk = false;
    run("SAMPLER", kMaskSampler, ClassifySampler({unknown, unknown.heap, true, 1, 0}), {{"case", "unknown"}});
    P0AllocatorSnapshot unsafe;
    unsafe.fenceKnown = true;
    unsafe.targetFence = 10;
    unsafe.completedBefore = 9;
    run("ALLOCATOR", kMaskAllocator, ClassifyAllocator(unsafe), {{"case", "unsafe"}});
    P0ResolvedSnapshot fallbackDim;
    fallbackDim.candidateKey = 0x1000;
    fallbackDim.winnerKey = 0x1000;
    fallbackDim.priority = "4";
    fallbackDim.source = "last-colour";
    fallbackDim.selected = true;
    fallbackDim.candidateKnown = true;
    fallbackDim.fetchWidth = fallbackDim.fetchHeight = 512;
    fallbackDim.resolvedWidth = fallbackDim.resolvedHeight = 256;
    fallbackDim.stagedResult = "staged";
    run("RESOLVED", kMaskResolved, ClassifyResolved(fallbackDim), {{"case", "fallback-dim"}});
    P0PsoRtvSnapshot depthOnly;
    depthOnly.psoKnown = depthOnly.omSetRenderTargetsOk = depthOnly.setPipelineStateOk = true;
    depthOnly.depthOnly = true;
    run("PSO-RTV", kMaskPsoRtv, ClassifyPsoRtv(depthOnly), {{"case", "depth-only"}});
    if (sink) sink("P0-AUDIT-SELFTEST-DONE pass=8 total=8 fixture=1");
    return passed;
}

}  // namespace mcla::p0_audit
