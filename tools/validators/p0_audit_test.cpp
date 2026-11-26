#include "d3d12_p0_audit.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using mcla::p0_audit::Config;
using mcla::p0_audit::ParseConfig;
using mcla::p0_audit::P0AllocatorSnapshot;
using mcla::p0_audit::P0PsoRtvSnapshot;
using mcla::p0_audit::P0ResolvedSnapshot;
using mcla::p0_audit::P0SamplerOwner;
using mcla::p0_audit::P0SamplerSnapshot;
using mcla::p0_audit::Verdict;

bool Expect(bool condition, const char* message) {
    if (!condition) std::cerr << "P0-AUDIT-TEST-FAIL " << message << '\n';
    return condition;
}

bool ExpectVerdict(const Verdict& verdict, const char* category, const char* label, const char* reason) {
    const bool categoryMatches =
        (std::string(category) == "ok" && verdict.ok) ||
        (std::string(category) == "attention" && verdict.attention) ||
        (std::string(category) == "unknown" && verdict.unknown) ||
        (std::string(category) == "not-applicable" && verdict.notApplicable) ||
        (std::string(category) == "recovered" && verdict.recovered);
    const int count = static_cast<int>(verdict.ok) + static_cast<int>(verdict.attention) +
                      static_cast<int>(verdict.unknown) + static_cast<int>(verdict.notApplicable) +
                      static_cast<int>(verdict.recovered);
    bool ok = true;
    ok &= Expect(categoryMatches && count == 1, "verdict category must be exact and singular");
    ok &= Expect(verdict.label == label, "verdict domain label mismatch");
    ok &= Expect(verdict.reason == reason, "verdict reason mismatch");
    ok &= Expect(mcla::p0_audit::VerdictCategory(verdict) == category, "formatted category mismatch");
    ok &= Expect(mcla::p0_audit::VerdictName(verdict) == label, "formatted verdict mismatch");
    return ok;
}

P0SamplerOwner SamplerOwner() {
    P0SamplerOwner owner;
    owner.heap = reinterpret_cast<void*>(0x1000);
    owner.handle = owner.heapBaseGpu = 0x1000;
    owner.descriptorSize = 16;
    owner.descriptorCount = owner.heapDescriptorCount = 16;
    owner.internOk = true;
    return owner;
}

P0ResolvedSnapshot ResolvedBase() {
    P0ResolvedSnapshot snapshot;
    snapshot.candidateKey = snapshot.winnerKey = 0x1000;
    snapshot.priority = "2";
    snapshot.source = "last-colour";
    snapshot.selected = true;
    snapshot.candidateKnown = true;
    snapshot.metadataKnown = true;
    snapshot.agePolicyEnabled = true;
    snapshot.ageLimitFrames = 10;
    snapshot.fetchWidth = snapshot.fetchHeight = 512;
    snapshot.resolvedWidth = snapshot.resolvedHeight = 512;
    snapshot.fetchFormat = 6;
    snapshot.resolvedFormat = 28;
    snapshot.stagedResult = "staged";
    return snapshot;
}

}  // namespace

int main() {
    using namespace mcla::p0_audit;
    bool ok = true;
    const Config unset = ParseConfig(nullptr);
    ok &= Expect(!unset.armed && !unset.emitConfig && unset.mask == 0, "unset must be silent");

    const Config zero = ParseConfig("0");
    ok &= Expect(zero.valid && zero.armed && zero.emitConfig && zero.mask == 0, "explicit zero must arm");

    const Config full = ParseConfig("0xF");
    ok &= Expect(full.valid && full.armed && full.mask == 0xF, "0xF must parse");

    const Config fixture = ParseConfig("0x8000000F");
    ok &= Expect(fixture.valid && fixture.armed && fixture.mask == 0x8000000F, "fixture bit must parse");

    for (const char* invalid : {"-1", "0x10", "0x100000000", "12junk", ""}) {
        const Config parsed = ParseConfig(invalid);
        ok &= Expect(parsed.emitConfig && !parsed.valid && !parsed.armed && parsed.mask == 0 &&
                     std::string(parsed.status) == "invalid", "invalid mask must disable audit");
    }

    P0SamplerOwner owner = SamplerOwner();
    ok &= ExpectVerdict(ClassifySampler({owner, owner.heap, true, 1, 0}),
                        "ok", "bound-ok", "owner-bound-aligned-in-range");
    owner.neutralFallback = true;
    ok &= ExpectVerdict(ClassifySampler({owner, owner.heap, true, 1, 0}),
                        "attention", "neutral-fallback", "neutral-fallback");
    ok &= ExpectVerdict(ClassifySampler({owner, nullptr, false, 1, 0}),
                        "attention", "unbound", "descriptor-heaps-unbound");
    ok &= ExpectVerdict(ClassifySampler({owner, reinterpret_cast<void*>(0x2000), true, 1, 1}),
                        "attention", "owner-mismatch", "owner-bound-heap-mismatch");

    P0AllocatorSnapshot allocator;
    allocator.fenceKnown = true;
    allocator.targetFence = 10;
    allocator.completedBefore = 9;
    ok &= ExpectVerdict(ClassifyAllocator(allocator), "attention", "unsafe", "allocator-reset-before-fence");
    allocator.finalHresult = -1;
    ok &= ExpectVerdict(ClassifyAllocator(allocator), "attention", "reset-failed", "allocator-reset-failed");
    allocator.finalHresult = 0;
    allocator.firstHresult = -1;
    allocator.retried = true;
    allocator.completedBefore = 10;
    ok &= ExpectVerdict(ClassifyAllocator(allocator), "recovered", "recovered", "allocator-reset-retry-recovered");
    allocator.firstHresult = 0;
    allocator.retried = false;
    ok &= ExpectVerdict(ClassifyAllocator(allocator), "ok", "safe", "allocator-reset-after-fence");

    P0ResolvedSnapshot resolved = ResolvedBase();
    ok &= ExpectVerdict(ClassifyResolved(resolved), "ok", "exact", "selected-exact-staged");
    resolved.resolvedWidth = resolved.resolvedHeight = 256;
    ok &= ExpectVerdict(ClassifyResolved(resolved), "attention", "fallback-dim", "fallback-dim");
    resolved = ResolvedBase();
    resolved.selected = false;
    resolved.ageFrames = 11;
    ok &= ExpectVerdict(ClassifyResolved(resolved), "attention", "stale", "enabled-age-stale");
    resolved = ResolvedBase();
    resolved.agePolicyEnabled = false;
    resolved.ageFrames = 99;
    ok &= ExpectVerdict(ClassifyResolved(resolved), "ok", "not-stale", "disabled-age-not-stale");
    resolved = ResolvedBase();
    resolved.formatExact = false;
    resolved.formatCompatible = true;
    ok &= ExpectVerdict(ClassifyResolved(resolved), "attention", "format-fallback",
                        "compatible-nonexact-format-fallback");
    resolved = ResolvedBase();
    resolved.winnerIsLastColour = false;
    ok &= ExpectVerdict(ClassifyResolved(resolved), "attention", "non-last-winner", "non-last-resolve-winner");
    resolved = ResolvedBase();
    resolved.frameUnderflow = true;
    ok &= ExpectVerdict(ClassifyResolved(resolved), "unknown", "frame-underflow", "resolve-frame-underflow");
    resolved = ResolvedBase();
    resolved.metadataKnown = false;
    ok &= ExpectVerdict(ClassifyResolved(resolved), "unknown", "metadata-unknown", "resolve-metadata-unknown");
    resolved = ResolvedBase();
    resolved.depthFetch = true;
    ok &= ExpectVerdict(ClassifyResolved(resolved), "ok", "exact-depth", "exact-depth-staged");
    resolved.formatExact = false;
    ok &= ExpectVerdict(ClassifyResolved(resolved), "not-applicable", "depth-fallback",
                        "depth-nonexact-format-fallback");
    resolved = ResolvedBase();
    resolved.stagedResult = "fallback-srv";
    ok &= ExpectVerdict(ClassifyResolved(resolved), "attention", "fallback-srv",
                        "resolved-staged-through-fallback-srv");

    P0PsoRtvSnapshot pso;
    pso.psoKnown = pso.omSetRenderTargetsOk = pso.setPipelineStateOk = true;
    pso.psoCount = pso.rtvCount = 1;
    pso.psoMask = pso.rtvMask = 1;
    pso.psoFormats[0] = pso.rtvFormats[0] = 28;
    ok &= ExpectVerdict(ClassifyPsoRtv(pso), "ok", "pso-rtv-match", "pso-rtv-match");
    pso.rtvFormats[0] = 10;
    ok &= ExpectVerdict(ClassifyPsoRtv(pso), "attention", "pso-rtv-mismatch", "pso-rtv-mismatch");
    pso.rtvFormats[0] = 0;
    pso.rtvSlotKnown[0] = false;
    ok &= ExpectVerdict(ClassifyPsoRtv(pso), "unknown", "rtv-format-unknown",
                        "effective-rtv-slot-format-unknown");
    pso = P0PsoRtvSnapshot{};
    pso.psoKnown = pso.omSetRenderTargetsOk = pso.setPipelineStateOk = true;
    pso.depthOnly = true;
    ok &= ExpectVerdict(ClassifyPsoRtv(pso), "not-applicable", "depth-only", "depth-only-zero-rtv");

#ifdef _WIN32
    _putenv_s("MCLA_P0_AUDIT_MASK", "0xF");
#else
    setenv("MCLA_P0_AUDIT_MASK", "0xF", 1);
#endif
    std::vector<std::string> emitted;
    Emitter emitter;
    emitter.Initialize([&](const std::string& row) { emitted.push_back(row); });
    P0AllocatorSnapshot emitterAllocator;
    emitterAllocator.fenceKnown = true;
    emitterAllocator.targetFence = 10;
    emitterAllocator.completedBefore = 9;
    P0SamplerOwner emitterOwner = SamplerOwner();
    emitter.Emit(kMaskSampler, "SAMPLER", ClassifySampler({emitterOwner, emitterOwner.heap, true, 1, 0}));
    emitter.Emit(kMaskAllocator, "ALLOCATOR", ClassifyAllocator(emitterAllocator));
    emitterOwner.neutralFallback = true;
    emitter.Emit(kMaskSampler, "SAMPLER", ClassifySampler({emitterOwner, emitterOwner.heap, true, 2, 0}));
    emitter.Emit(kMaskPsoRtv, "PSO-RTV", ClassifyPsoRtv(pso));
    emitter.NoteFrameBegun(1);
    emitter.NoteFrameBegun(2);
    ok &= Expect(emitted.size() == 13, "config plus four events plus two per-domain summary rounds");
    bool samplerCounters = false;
    bool allocatorCounters = false;
    bool domainSummaries = false;
    for (const std::string& row : emitted) {
        if (row.find("P0-AUDIT-SAMPLER") == 0 && row.find(" domainSeq=2 events=2 attention=1 ") != std::string::npos)
            samplerCounters = true;
        if (row.find("P0-AUDIT-ALLOCATOR") == 0 && row.find(" domainSeq=1 events=1 attention=1 ") != std::string::npos)
            allocatorCounters = true;
        if (row.find("P0-AUDIT-SUMMARY") == 0 && row.find(" domain=SAMPLER ") != std::string::npos)
            domainSummaries = true;
    }
    ok &= Expect(samplerCounters && allocatorCounters && domainSummaries,
                 "per-domain counters and summaries must be independent");

    std::vector<std::string> rows;
    auto sink = [&](const std::string& row) {
        rows.push_back(row);
        std::cout << row << '\n';
    };
    const uint32_t passed = RunBuiltInFixtures(sink);
    ok &= Expect(passed == 8, "eight built-in fixtures must pass");
    ok &= Expect(rows.size() == 9, "eight fixture rows plus done row");
    if (!rows.empty())
        ok &= Expect(rows.back() == "P0-AUDIT-SELFTEST-DONE pass=8 total=8 fixture=1", "fixture done marker");
    const char* fixtureLabels[] = {"neutral-fallback", "owner-mismatch", "out-of-range", "unbound",
                                   "intern-failed", "unsafe", "fallback-dim", "depth-only"};
    for (size_t i = 0; i < rows.size() && i < 8; ++i) {
        const std::string expected = std::string(" verdict=") + fixtureLabels[i] + " ";
        ok &= Expect(rows[i].find(" v=1 ") != std::string::npos &&
                     rows[i].find(" fixture=1 ") != std::string::npos &&
                     rows[i].find(" category=") != std::string::npos &&
                     rows[i].find(expected) != std::string::npos,
                     "fixture row verdict/category mismatch");
    }
    return ok ? 0 : 1;
}
