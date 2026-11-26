// Wave 3 — developer options and crash guards (2026-11-19).
//
// Guest facts verified against generated/ppc_xenon/*.cpp (not inferred):
//   - sub_823794E0: r31 = ctx.r3 (ppc_recomp.58.cpp:2553-2554), shader group
//     stored at +256 (:2578), pending byte cleared at +267 (:2600-2601).
//   - sub_823793E8: group at +252 (:2432), pending byte at +266 (:2519).
//   - sub_82193AF8 (SHGRP-NULL) is ALREADY implemented at
//     src/gpu_device.cpp:29715-29738 and is NOT duplicated here.
//   - sub_8230D988 call site: ppc_recomp.51.cpp:18021-18025, lr=0x823126A8.
//   - sub_822E5B00 call site: spec §3.3 item 4, lr=0x821315E8.
//
// ZERO rex:: symbols. All guest access is via GuestMemoryHeap.
// Every feature is OFF unless its MCLA_* env gate is '1'; the default bodies
// are exact __imp__ passthroughs and the injected midhook helpers are no-ops.

#include "hooks/hooks.h"
#include "generated/ppc_xenon/ppc_recomp_shared.h"
#include "cpu/ppc_context.h"
#include "kernel/memory.h"
#include "common/cvar.h"
#include "logging.h"
#include "boot_host.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

// Generated weak aliases.
PPC_FUNC_IMPL(__imp__sub_821C06C8);
PPC_FUNC_IMPL(__imp__sub_823794E0);
PPC_FUNC_IMPL(__imp__sub_823793E8);
PPC_FUNC_IMPL(__imp__sub_8259AA28);
PPC_FUNC_IMPL(__imp__sub_8230D988);
PPC_FUNC_IMPL(__imp__sub_8230C860);
PPC_FUNC_IMPL(__imp__sub_822E5B00);

// Local strong C++ declaration for the new gate-byte wrapper (hooks.h is not
// owned by this worker); it overrides the generated weak alias like the others.
PPC_FUNC(sub_8230C860);

namespace {

bool EnvOn(const char *name) {
  const char *e = std::getenv(name);
  return e != nullptr && e[0] == '1';
}

bool EnvIs(const char *name, const char *value) {
  const char *e = std::getenv(name);
  return e != nullptr && std::strcmp(e, value) == 0;
}

uint32_t EnsureDevTrueValue() {
  static uint32_t va = 0;
  static std::once_flag once;
  std::call_once(once, []() {
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    const uint32_t p = mem.Alloc(16u, 16u);
    if (p != 0u) {
      const uint8_t value[16] = {'1', 0};
      mem.WriteBytes(p, value, sizeof(value));
      va = p;
    }
  });
  return va;
}

bool DevSwitchEnabled(const char *name) {
  // CVar lookups by name are safe when the CVar is not (yet) registered:
  // CVarSystem::GetBool() returns false for an unknown name.
  if (std::strcmp(name, "noraceshadows") == 0)
    return !MCLA_CVAR_GET_BOOL(mcla_race_shadows) || EnvOn("MCLA_DEV_NORACESHADOWS");
  if (std::strcmp(name, "fastVehShadows") == 0)
    return MCLA_CVAR_GET_BOOL(mcla_fast_car_shadows) || EnvOn("MCLA_DEV_FASTVEHSHADOWS");
  if (std::strcmp(name, "noimpostors") == 0)
    return !MCLA_CVAR_GET_BOOL(mcla_foliage_impostors) || EnvOn("MCLA_DEV_NOIMPPOSTORS");
  if (std::strcmp(name, "nofsblur") == 0)
    return !MCLA_CVAR_GET_BOOL(mcla_fullscreen_blur) || EnvOn("MCLA_DEV_NOFSBLUR");
  // Raw guest wrappers sub_827AD318 and sub_827ACBF0 pass these exact names to
  // sub_821C06C8. These are negative switches: forcing the value string to "1"
  // disables the corresponding guest effect.
  if (std::strcmp(name, "noao") == 0)
    return EnvOn("MCLA_DEV_NOAO");        // 1 disables ambient occlusion
  if (std::strcmp(name, "noshadows") == 0)
    return EnvOn("MCLA_DEV_NOSHADOWS");   // 1 disables all guest shadows
  return false;
}

std::atomic<uint32_t> g_shaderRepairs{0};
std::atomic<uint32_t> g_uiLightsSkips{0};

}  // namespace

// ===========================================================================
// Midhook helpers — extern "C" to match tools/emit_dropped_ops.py decls.
// ===========================================================================

extern "C" bool mcla_ui_lights_guard(PPCRegister &node) {
  if (!EnvOn("MCLA_UI_LIGHTS_GUARD")) return false;
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint32_t kind = 0;
  if (!mem.ReadU32BE(node.u32 + 8u, &kind) || kind != 5u) return false;
  const uint32_t n = g_uiLightsSkips.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 4) MCLA_LOG_WARN("UI-LIGHTS-GUARD: skip #{} node={:08X} kind=5", n, node.u32);
  return true;
}

extern "C" bool mcla_map_cursor_guard(PPCRegister &r31, PPCRegister &r30,
                                      PPCRegister &r26) {
  if (!EnvOn("MCLA_MAP_CURSOR_GUARD") || r31.u32 != 0u) return false;
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint8_t pos[16] = {};
  if (!mem.ReadBytes(r30.u32, pos, sizeof(pos))) return false;
  if (!mem.WriteBytes(r26.u32, pos, sizeof(pos))) return false;
  return true;  // caller jumps to loc_826253F4 (stack cleanup)
}

extern "C" void mcla_traffic_chassis_bound_8232D048(PPCRegister &root,
                                                    PPCRegister &child) {
  if (!EnvOn("MCLA_TRAFFIC_BOUND_GUARD")) return;
  // ctx.r9 == root + 0x80; the bare phBound is at ctx.r9 - 0x80 (spec §3.4.1).
  const uint32_t bound = root.u32 - 0x80u;
  uint32_t type = 0;
  if (mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(bound + 4u, &type) &&
      type != 12u) {
    child.u64 = bound;
  }
}

extern "C" void mcla_traffic_chassis_bound_8232D900(PPCRegister &root,
                                                    PPCRegister &child) {
  if (!EnvOn("MCLA_TRAFFIC_BOUND_GUARD")) return;
  uint32_t type = 0;
  if (mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(root.u32 + 4u, &type) &&
      type != 12u) {
    child.u64 = root.u32;
  }
}

extern "C" void mcla_traffic_chassis_bound_8232E274(PPCRegister &root,
                                                    PPCRegister &child) {
  if (!EnvOn("MCLA_TRAFFIC_BOUND_GUARD")) return;
  uint32_t type = 0;
  if (mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(root.u32 + 4u, &type) &&
      type != 12u) {
    child.u64 = root.u32;
  }
}

// ===========================================================================
// C++-linkage PPC_FUNC overrides. Default = pure passthrough.
// ===========================================================================

PPC_FUNC(sub_821C06C8) {
  __imp__sub_821C06C8(ctx, base);
  // The old MCLA_DEV_OPTIONS_FIX gate is kept only as MCLA_NO_DEV_SWITCH_HOOK=1 (=hook off entirely).
  if (EnvOn("MCLA_NO_DEV_SWITCH_HOOK") || ctx.r3.u32 == 0u) return;

  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  // Switch layout (ppc_recomp.15.cpp:5081-5090): +0 = char* name, +4 = value (0 = off). The old code read the
  // name bytes AT r3 (the pointer itself), so no switch ever matched.
  uint32_t namePtr = 0;
  char name[33] = {};
  if (!mem.ReadU32BE(ctx.r3.u32, &namePtr) || !mem.ReadBytes(namePtr, name, sizeof(name) - 1u)) return;
  name[sizeof(name) - 1u] = '\0';
  const bool on = DevSwitchEnabled(name);
  if (std::strcmp(name, "noraceshadows") == 0 || std::strcmp(name, "fastVehShadows") == 0 ||
      std::strcmp(name, "noimpostors") == 0 || std::strcmp(name, "nofsblur") == 0 ||
      std::strcmp(name, "noao") == 0 || std::strcmp(name, "noshadows") == 0)
    MCLA_LOG_WARN("DEV-SWITCH: '{}' at {:08X} forced_on={}", name, ctx.r3.u32, on ? 1 : 0);
  if (!on) return;

  const uint32_t value = EnsureDevTrueValue();
  if (value == 0u || !mem.WriteU32BE(ctx.r3.u32 + 4u, value)) {
    MCLA_LOG_WARN("DEV-OPTIONS: failed to write switch value for '{}'", name);
  }
}

PPC_FUNC(sub_823794E0) {
  const uint32_t cache = ctx.r3.u32;
  __imp__sub_823794E0(ctx, base);
  if (!EnvOn("MCLA_CAR_SHADER_GUARD") || cache == 0u) return;
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint32_t group = 0;
  if (mem.ReadU32BE(cache + 256u, &group) && group == 0u) {
    mem.WriteU8(cache + 267u, 1u);  // keep pending set: shader group is null
    const uint32_t n = g_shaderRepairs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16) MCLA_LOG_WARN("[CarShaderBind] repair +267 cache={:08X} n={}", cache, n);
  }
}

PPC_FUNC(sub_823793E8) {
  const uint32_t cache = ctx.r3.u32;
  __imp__sub_823793E8(ctx, base);
  if (!EnvOn("MCLA_CAR_SHADER_GUARD") || cache == 0u) return;
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint32_t group = 0;
  if (mem.ReadU32BE(cache + 252u, &group) && group == 0u) {
    mem.WriteU8(cache + 266u, 1u);
    const uint32_t n = g_shaderRepairs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16) MCLA_LOG_WARN("[CarShaderBind] repair +266 cache={:08X} n={}", cache, n);
  }
}

PPC_FUNC(sub_8259AA28) {
  if (EnvOn("MCLA_TRAFFIC_BOUND_GUARD")) {
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    uint32_t type = 0;
    if (ctx.r3.u32 == 0u || !mem.ReadU32BE(ctx.r3.u32 + 4u, &type) || type != 12u) {
      return;  // bare phBound: never walk the composite child table
    }
  }
  __imp__sub_8259AA28(ctx, base);
}

PPC_FUNC(sub_8230D988) {
  // P1 draw-site probe (MCLA_TREE_DRAWN=1) + crossfade fix (default ON).
  // Measured from generated/ppc_xenon/ppc_recomp.51.cpp:
  //   :5175-5190  ctx.r3=mgr(r31), lp=[mgr+16], cnt=u16[lp+12]; guest takes the
  //               loc_8230DAEC early return when lp==0 or cnt==0.
  //   :2413-2424  sub_8230C860: r31=ctx.r3 = the REAL 224-byte entry;
  //               b218=u8[r31+218]. Old "ctx.r4=entry" claim RETRACTED.
  // diffIdx/normIdx=[mgr+24]/[mgr+28] are p1-analyze's decision-rule fields;
  // they are NOT read by the generated sub_8230D988 body, so they are logged
  // raw (checked reads) and NOT given any further semantic name (rule 0).
  static const bool s_treeDrawn = EnvOn("MCLA_TREE_DRAWN");
  // RETRACTED: this old fix (and the probe's b218/verdict below) read the WRONG
  // pointer -- ctx.r4 is the parent arg, not the 224-byte entry. Kept only as a
  // dormant log path. Requires MCLA_TREE_FIX=1 explicitly; default OFF. Use the
  // MCLA_TREE_GATE[_FIX] wrapper below, which reads the real ctx.r3+218 byte.
  static const bool s_treeFix = EnvOn("MCLA_TREE_FIX");
  if (s_treeDrawn || s_treeFix) {
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    const uint32_t mgr = ctx.r3.u32;
    const uint32_t entry = ctx.r4.u32;
    uint32_t lp = 0u, diffIdx = 0u, normIdx = 0u;
    uint16_t cnt = 0u;
    uint8_t b218 = 0u;
    bool ok = mem.ReadU32BE(mgr + 16u, &lp) &&
              mem.ReadU32BE(mgr + 24u, &diffIdx) &&
              mem.ReadU32BE(mgr + 28u, &normIdx) &&
              mem.ReadBytes(entry + 218u, &b218, 1u);
    if (lp != 0u) ok = mem.ReadU16BE(lp + 12u, &cnt) && ok;
    const bool early = (lp == 0u || cnt == 0u);
    const bool foliage = (diffIdx != 0u && normIdx != 0u);
    const bool crossfadeGate = (!early && foliage && static_cast<uint32_t>(b218) != 0u);
    const char *verdict = early ? "not-drawn" :
                          (diffIdx == 0u) ? "unbound-sampler" :
                          (static_cast<uint32_t>(b218) != 0u) ? "crossfade-gate" :
                          "drawn";
    if (s_treeDrawn)
      MCLA_LOG_ERROR("TREE-DRAWN mgr={:08X} entry={:08X} lp={:08X} cnt={} diffIdx={} normIdx={} "
                    "b218={:02X} early={} ok={} verdict={}",
                    mgr, entry, lp, cnt, diffIdx, normIdx,
                    static_cast<uint32_t>(b218), early ? 1 : 0, ok ? 1 : 0, verdict);
    if (s_treeFix && crossfadeGate && ok) {
      const uint8_t old = b218;
      if (mem.WriteU8(entry + 218u, 0u)) {
        static std::atomic<uint32_t> s_fixed{0};
        const uint32_t k = s_fixed.fetch_add(1) + 1;
        if (k <= 8 || (k % 1000) == 0)
          MCLA_LOG_ERROR("TREE-FIX entry={:08X} b218_old={:02X} n={}",
                         entry, static_cast<uint32_t>(old), k);
      }
    }
  }
  if (EnvOn("MCLA_IMPOSTOR_GUARD") && ctx.lr == 0x823126A8u) {
    // REFUSED (partial): spec §3.3 gives the entry predicates (+168/+196) but
    // NOT the manager's array base/count offsets. Guessing them would violate
    // rule 0. With the gate on we emit one measurement line and only skip when
    // MCLA_IMPOSTOR_FORCE_SKIP=1 is explicitly set by the operator.
    static std::atomic<uint32_t> seen{0};
    const uint32_t n = seen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4) MCLA_LOG_WARN("IMPOSTOR-GUARD: mgr={:08X} n={} (layout unmeasured)", ctx.r3.u32, n);
    if (EnvOn("MCLA_IMPOSTOR_FORCE_SKIP")) return;
  }
  __imp__sub_8230D988(ctx, base);
}

// P1 gate-byte probe/fix at the REAL 224-byte entry pointer. Generated proof:
// ppc_recomp.51.cpp:5287-5297 computes ctx.r3 = [table+8] + index*224 before
// `bl 0x8230c860`; :2413-2424 makes that r31 and reads lbz 218(r31). Our earlier
// TREE-DRAWN/TREE-FIX used ctx.r4 (the parent arg) -- WRONG, now retracted.
// MCLA_TREE_GATE=1: log-only. MCLA_TREE_GATE_FIX=1: opt-in zeroing (default OFF).
PPC_FUNC(sub_8230C860) {
  static const bool s_gate = EnvOn("MCLA_TREE_GATE");
  static const bool s_gateFix = EnvOn("MCLA_TREE_GATE_FIX");
  if (s_gate || s_gateFix) {
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    const uint32_t entry = ctx.r3.u32;
    uint8_t b218 = 0u;
    const bool ok = mem.ReadBytes(entry + 218u, &b218, 1u);
    if (s_gate) {
      const char *verdict = !ok ? "read-failed" :
                            (static_cast<uint32_t>(b218) != 0u) ? "crossfade-gate" :
                            "submit-arm";
      MCLA_LOG_ERROR("TREE-GATE entry={:08X} b218={:02X} ok={} verdict={}",
                     entry, static_cast<uint32_t>(b218), ok ? 1 : 0, verdict);
    }
    if (s_gateFix && ok && static_cast<uint32_t>(b218) != 0u) {
      if (mem.WriteU8(entry + 218u, 0u)) {
        static std::atomic<uint32_t> s_gateFixed{0};
        const uint32_t k = s_gateFixed.fetch_add(1) + 1;
        if (k <= 8 || (k % 1000) == 0)
          MCLA_LOG_ERROR("TREE-GATE-FIX entry={:08X} b218_old={:02X} n={}",
                         entry, static_cast<uint32_t>(b218), k);
      }
    }
  }
  __imp__sub_8230C860(ctx, base);
}

PPC_FUNC(sub_822E5B00) {
  if (EnvOn("MCLA_SKIP_INTRO_MASK") && ctx.lr == 0x821315E8u &&
      (EnvIs("MCLA_INTRO", "skip") || EnvOn("MCLA_SKIP_INTRO"))) {
    ctx.r4.u32 = 0xFEFFFFFFu;  // mask off the intro render pass
  }
  __imp__sub_822E5B00(ctx, base);
}

// ===========================================================================
// Problem 2 — legal-glyph draw probe (p2-edit). LOG-ONLY; changes no state.
//
// The VS/PS hashes in the brief (VS 0x1A5B2B313B294EAE, PS 0xBC145DD402736047)
// are HOST-SIDE values: HashShaderBytecode at render_thread.cpp:4915 and
// psOut.programHash at :7461. No PPC register or guest memory in this TU holds
// them, so this CANNOT be a midhook here; it must be called from the host draw
// path where pipeVsHash/pipePsHash are live (render_thread.cpp, p5-edit's file).
// Until that one-line call exists this helper is inert. Default OFF =
// byte-identical runtime behaviour.
//
// REFUSED: MCLA_HIDE_LEGALS ("force alpha to 0 in the translated shader path").
// Alpha/blend is produced by renderer/shader_translator.cpp and d3d12_backend.cpp
// (other owners), unreachable from a guest PPC hook. Forcing it here would be a
// guess (rule 0), so no hide code is added.
// ===========================================================================
extern "C" void mcla_legals_probe(uint64_t vsHash, uint64_t psHash,
                                  uint64_t drawCount) {
  static const bool on = [] {
    const char *e = std::getenv("MCLA_LEGALS_PROBE");
    return e != nullptr && e[0] == '1';
  }();
  if (!on || vsHash != 0x1A5B2B313B294EAEull) return;
  static std::atomic<uint64_t> matches{0};
  const uint64_t n = matches.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || (n % 1000u) == 0u) {
    MCLA_LOG_ERROR("LEGALS-PROBE: match #{} vsHash={:016X} psHash={:016X} drawCount={}",
                  n, vsHash, psHash, drawCount);
  }
}


// ===========================================================================
// PROP-CENSUS (2026-11-23) -- near city-prop instance draw decisions.
// LOG-ONLY unless an MCLA_TEMP_PROP_* knob is set to 1. Default = passthrough.
//
// Measured chain (generated/ppc_xenon/ppc_recomp.51.cpp + IDA):
//   sub_8230E598 (group): F16x4 sphere at grp-8, inline 8-plane viewport test
//     on [0x82839704]+864..976, distance test vs type+132/+136, then
//     sub_8230E880 (sub-groups, u16 grp+16) or sub_8230D2C0 (instances).
//   sub_8230D2C0(r3=grp, r4=grpFullyInside, r5=shadowPass): type=[0x8288D0BC],
//     instances = 32B records at [type+208], idx u16 grp+10..grp+12. Per
//     instance (r25==0 only): bl 0x8217C328 (r3=viewport, f1..f3=center,
//     f4=radius, r8=&depth; 0 => skip), depth-2r > maxdist => skip; then
//     (r17==0) bl 0x82175238(r3=sys, r4=[0x827E5640], r5=&sphere, r6=2);
//     rlwinm 0,5,5 (0x04000000) or 0,3,3 (0x10000000) set => skip; else
//     LOD pick (type+140/+144) and sub_8230B9B0 draw (model [type+168]+64/+68).
//   sub_82175238 with r6=2: (r6&1)==0 so its frustum half is skipped; only
//     the occluder loop runs (count at [sys]+57792*r4+52224): 0x10000000 =
//     occluded, 0x20000000 = tested-visible, 0 = no occluders.
// Knobs (only inside the prop instance loop, this thread):
//   MCLA_TEMP_PROP_NO_VPCULL=1   sub_8217C328 result 0 -> 1
//   MCLA_TEMP_PROP_NO_OCCLUDE=1  clear 0x10000000|0x04000000 from sub_82175238
// ===========================================================================
PPC_FUNC_IMPL(__imp__sub_8230D2C0);
PPC_FUNC_IMPL(__imp__sub_8230E598);
PPC_FUNC_IMPL(__imp__sub_8217C328);
PPC_FUNC_IMPL(__imp__sub_82175238);

namespace {
thread_local int t_propLoop = 0;  // >0 while inside sub_8230D2C0
thread_local bool t_grpReached = false;

struct PropCensus {
  bool census, noVp, noOcc;
  // depth buckets: <25, <60, <150, >=150, unknown
  std::atomic<uint32_t> grpSeen[5], grpReached[5];
  std::atomic<uint32_t> vpPass[5], vpFail[5], vpFailPlane[8];
  std::atomic<uint32_t> occVis[5], occHit[5], occNone[5];
  std::atomic<uint32_t> loops{0}, logged{0};
};

PropCensus &PC() {
  static PropCensus s{EnvOn("MCLA_PROP_CENSUS"), EnvOn("MCLA_TEMP_PROP_NO_VPCULL"),
                      EnvOn("MCLA_TEMP_PROP_NO_OCCLUDE")};
  return s;
}

float HalfToF32(uint16_t h) {
  const uint32_t s = (h & 0x8000u) << 16;
  uint32_t e = (h >> 10) & 0x1Fu, m = h & 0x3FFu, u;
  if (e == 0x1Fu) {
    u = s | 0x7F800000u | (m << 13);
  } else if (e != 0) {
    u = s | ((e + 112u) << 23) | (m << 13);
  } else if (m == 0) {
    u = s;
  } else {
    e = 113u;
    while ((m & 0x400u) == 0) { m <<= 1; --e; }
    u = s | (e << 23) | ((m & 0x3FFu) << 13);
  }
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Mirrors sub_8217C328 (ppc_recomp.8.cpp): set A rows +864(x) +880(y) +896(z)
// +912(c), set B rows +928..+976. Returns the first plane k with dist < -r
// (8 = none, -1 = read failed); depth = distA[0] + r (what the guest stores via r8).
int VpTest(uint32_t vp, float x, float y, float z, float r, float *depth) {
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  int fail = 8;
  *depth = -1e38f;
  for (int k = 0; k < 8; ++k) {
    const uint32_t b = vp + (k < 4 ? 864u : 928u) + 4u * static_cast<uint32_t>(k & 3);
    float cx = 0, cy = 0, cz = 0, cc = 0;
    if (!mem.ReadF32BE(b, &cx) || !mem.ReadF32BE(b + 16u, &cy) ||
        !mem.ReadF32BE(b + 32u, &cz) || !mem.ReadF32BE(b + 48u, &cc))
      return -1;
    const float d = x * cx + y * cy + z * cz + cc;
    if (k == 0) *depth = d + r;
    if (fail == 8 && d < -r) fail = k;
  }
  return fail;
}

void PlaneDist(uint32_t vp, float x, float y, float z, float *d) {
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  for (int k = 0; k < 8; ++k) {
    const uint32_t b = vp + (k < 4 ? 864u : 928u) + 4u * static_cast<uint32_t>(k & 3);
    float cx = 0, cy = 0, cz = 0, cc = 0;
    (void)mem.ReadF32BE(b, &cx);
    (void)mem.ReadF32BE(b + 16u, &cy);
    (void)mem.ReadF32BE(b + 32u, &cz);
    (void)mem.ReadF32BE(b + 48u, &cc);
    d[k] = x * cx + y * cy + z * cz + cc;
  }
}

// PLANES: the 8 viewport planes (nx,ny,nz,c) + the FLOW-LOD camera view
// (gpu_device.cpp FlowLodRow: [0x8287E064] + camIdx*64 + 240..248); every 20000th call, 7 rows max.
void DumpPlanes(uint32_t vp) {
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
  if (n % 20000u != 0u || n > 120000u) return;
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  float p[8][4] = {};
  for (int k = 0; k < 8; ++k) {
    const uint32_t b = vp + (k < 4 ? 864u : 928u) + 4u * static_cast<uint32_t>(k & 3);
    for (int j = 0; j < 4; ++j) (void)mem.ReadF32BE(b + 16u * static_cast<uint32_t>(j), &p[k][j]);
  }
  uint32_t camIdx = 0, camTab = 0;
  float cv[3] = {};
  (void)mem.ReadU32BE(0x827E5640u, &camIdx);
  (void)mem.ReadU32BE(0x8287E064u, &camTab);
  for (int j = 0; j < 3; ++j)
    (void)mem.ReadF32BE(camTab + (camIdx << 6) + 240u + 4u * static_cast<uint32_t>(j), &cv[j]);
  std::string s;
  for (int k = 0; k < 8; ++k)
    s += fmt::format(" P{}=({:.4f},{:.4f},{:.4f},{:.2f})", k, p[k][0], p[k][1], p[k][2], p[k][3]);
  MCLA_LOG_ERROR("PROP-CENSUS planes#{} vp={:08X} camIdx={} camView=({:.2f},{:.2f},{:.2f}){}", n,
                 vp, camIdx, cv[0], cv[1], cv[2], s);
}

int Bucket(float depth) {
  if (!(depth > -1e30f)) return 4;
  return depth < 25.f ? 0 : depth < 60.f ? 1 : depth < 150.f ? 2 : 3;
}

uint32_t Viewport() {
  uint32_t vp = 0;
  (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(0x82839704u, &vp);
  return vp;
}

void PropSummary(PropCensus &c) {
  auto row = [](std::atomic<uint32_t> *a) {
    return fmt::format("{}/{}/{}/{}/{}", a[0].load(), a[1].load(), a[2].load(), a[3].load(),
                       a[4].load());
  };
  MCLA_LOG_ERROR("PROP-CENSUS sum loops={} buckets(<25/<60/<150/>=150/unk) grpSeen={} "
                 "grpReached={} vpPass={} vpFail={} occVis={} occHit={} occNone={} "
                 "failPlane={}/{}/{}/{}/{}/{}/{}/{} knobs noVp={} noOcc={}",
                 c.loops.load(), row(c.grpSeen), row(c.grpReached), row(c.vpPass),
                 row(c.vpFail), row(c.occVis), row(c.occHit), row(c.occNone),
                 c.vpFailPlane[0].load(), c.vpFailPlane[1].load(), c.vpFailPlane[2].load(),
                 c.vpFailPlane[3].load(), c.vpFailPlane[4].load(), c.vpFailPlane[5].load(),
                 c.vpFailPlane[6].load(), c.vpFailPlane[7].load(), c.noVp ? 1 : 0,
                 c.noOcc ? 1 : 0);
}
}  // namespace

// ===========================================================================
// MCLA_LOD_SCALE=<f> (owner option; default 1.0 = passthrough, valid (0,16]).
// sub_8230F3A0 computes the per-type screen-size distances of the draw context
// [0x8288D0BC]: +132 near/LOD (= 2r*k/tan(fov*k'), or -10000 sentinel), +136
// far, +140 (= +132), then consumes them via sub_8230E598 (group test + nested
// sub_8230D2C0 instance max-dist/LOD), sub_8230AFC8 (fade constants) and
// sub_8230CF50. Scale positive values on entry to the outermost consumer and
// restore the guest's exact words on exit. The sector pager radius
// (0x827E0E34, MCLA_TEMP_SECTOR_RADIUS) is NOT touched: the stream budget.
// ===========================================================================
namespace {
float LodScale() {
  static const float s = [] {
    const char *e = std::getenv("MCLA_LOD_SCALE");
    float v = e ? std::strtof(e, nullptr) : 1.0f;
    if (!(v > 0.0f && v <= 16.0f)) v = 1.0f;
    if (v != 1.0f)
      MCLA_LOG_INFO("LOD-SCALE: active scale={} (ctx [0x8288D0BC]+132/+136/+140 around "
                    "sub_8230E598/sub_8230AFC8/sub_8230CF50)", v);
    return v;
  }();
  return s;
}

thread_local int t_lodDepth = 0;

struct LodScope {
  uint32_t ctxp = 0;
  uint32_t saved[3] = {};
  bool active = false, counted = false;
  LodScope() {
    const float s = LodScale();
    if (s == 1.0f) return;
    counted = true;
    if (t_lodDepth++ > 0) return;
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    if (!mem.ReadU32BE(0x8288D0BCu, &ctxp) || ctxp == 0) return;
    for (uint32_t k = 0; k < 3; ++k)
      if (!mem.ReadU32BE(ctxp + 132u + 4u * k, &saved[k])) return;
    active = true;
    float out[3] = {};
    for (uint32_t k = 0; k < 3; ++k) {
      float f;
      std::memcpy(&f, &saved[k], 4);
      out[k] = (f > 0.0f && f < 1e30f) ? f * s : f;
      uint32_t b;
      std::memcpy(&b, &out[k], 4);
      (void)mem.WriteU32BE(ctxp + 132u + 4u * k, b);
    }
    static std::atomic<uint32_t> s_log{0};
    if (s_log.fetch_add(1, std::memory_order_relaxed) < 3) {
      float in[3];
      std::memcpy(in, saved, sizeof(in));
      MCLA_LOG_INFO("LOD-SCALE: ctx={:08X} 132 {}->{} 136 {}->{} 140 {}->{}", ctxp, in[0], out[0],
                    in[1], out[1], in[2], out[2]);
    }
  }
  ~LodScope() {
    if (active) {
      auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
      for (uint32_t k = 0; k < 3; ++k) (void)mem.WriteU32BE(ctxp + 132u + 4u * k, saved[k]);
    }
    if (counted) --t_lodDepth;
  }
};
}  // namespace

PPC_FUNC_IMPL(__imp__sub_8230AFC8);
PPC_FUNC_IMPL(__imp__sub_8230CF50);
PPC_FUNC(sub_8230AFC8) {
  LodScope lod;
  __imp__sub_8230AFC8(ctx, base);
}
PPC_FUNC(sub_8230CF50) {
  LodScope lod;
  __imp__sub_8230CF50(ctx, base);
}

PPC_FUNC(sub_8230E598) {
  LodScope lod;
  PropCensus &c = PC();
  if (!c.census) {
    __imp__sub_8230E598(ctx, base);
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const uint32_t grp = ctx.r3.u32;
  uint16_t h[4] = {}, sub = 0;
  float depth = -1e38f;
  int fail = -1;
  if (mem.ReadU16BE(grp - 8u, &h[0]) && mem.ReadU16BE(grp - 6u, &h[1]) &&
      mem.ReadU16BE(grp - 4u, &h[2]) && mem.ReadU16BE(grp - 2u, &h[3]))
    fail = VpTest(Viewport(), HalfToF32(h[0]), HalfToF32(h[1]), HalfToF32(h[2]),
                  HalfToF32(h[3]), &depth);
  (void)mem.ReadU16BE(grp + 16u, &sub);
  // Alt decode: the 18-byte group record starts with its F16x4 sphere at grp+0 (Xenia's
  // vupkd3d128 FLOAT16_4 reads guest bytes 8..15 of the vector loaded at grp-8).
  uint16_t g[4] = {};
  float depth0 = -1e38f;
  int fail0 = -1;
  if (mem.ReadU16BE(grp, &g[0]) && mem.ReadU16BE(grp + 2u, &g[1]) &&
      mem.ReadU16BE(grp + 4u, &g[2]) && mem.ReadU16BE(grp + 6u, &g[3]))
    fail0 = VpTest(Viewport(), HalfToF32(g[0]), HalfToF32(g[1]), HalfToF32(g[2]),
                   HalfToF32(g[3]), &depth0);
  const int b = Bucket(depth);
  t_grpReached = false;
  __imp__sub_8230E598(ctx, base);
  c.grpSeen[b].fetch_add(1, std::memory_order_relaxed);
  if (t_grpReached) c.grpReached[b].fetch_add(1, std::memory_order_relaxed);
  // GRP-OUTCOME: 0 vpOut, 1 vpOut-but-reached (decode mismatch), 2 vpIn non-leaf,
  // 3 vpIn leaf reached, 4 vpIn leaf rejected (distance / sub_8230B190 gate).
  static std::atomic<uint32_t> s_cls[5], s_n{0}, s_rej{0}, s_raw{0};
  const int cls = fail != 8 ? (t_grpReached && sub == 0 ? 1 : 0)
                            : sub != 0 ? 2 : t_grpReached ? 3 : 4;
  s_cls[cls].fetch_add(1, std::memory_order_relaxed);
  // ALT: [guest reached?][grp+0 sphere vp-inside?]
  static std::atomic<uint32_t> s_alt[2][2];
  s_alt[t_grpReached ? 1 : 0][fail0 == 8 ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
  static std::atomic<uint32_t> s_altLog{0};
  if (fail0 == 8 && sub == 0 && !t_grpReached && s_altLog.fetch_add(1) < 30)
    MCLA_LOG_ERROR("PROP-CENSUS grpalt-miss grp={:08X} c0=({:.2f},{:.2f},{:.2f}) r0={:.2f} "
                   "d0-r={:.2f} guestHalf=({},{},{},{})",
                   grp, HalfToF32(g[0]), HalfToF32(g[1]), HalfToF32(g[2]), HalfToF32(g[3]),
                   depth0 - 2.f * HalfToF32(g[3]), HalfToF32(h[0]), HalfToF32(h[1]),
                   HalfToF32(h[2]), HalfToF32(h[3]));
  const float r = HalfToF32(h[3]);
  if (s_raw.fetch_add(1, std::memory_order_relaxed) < 12 ||
      ((cls == 4 || cls == 1) && s_rej.fetch_add(1, std::memory_order_relaxed) < 60)) {
    uint32_t type = 0, lodCnt = 0;
    uint8_t b8 = 0, b9 = 0;
    float t132 = 0, t136 = 0;
    (void)mem.ReadU32BE(0x8288D0BCu, &type);
    (void)mem.ReadF32BE(type + 132u, &t132);
    (void)mem.ReadU8(grp + 8u, &b8);
    (void)mem.ReadU8(grp + 9u, &b9);
    (void)mem.ReadF32BE(type + 136u, &t136);
    (void)mem.ReadU32BE(type + 144u, &lodCnt);
    MCLA_LOG_ERROR("PROP-CENSUS grp cls={} grp={:08X} h={:04X} {:04X} {:04X} {:04X} "
                   "c=({:.2f},{:.2f},{:.2f}) r={:.2f} fail={} d0-r={:.2f} sub={} b8={} b9={} "
                   "t132={} t136={} lodCnt={} lim(b8/16*(lodCnt>1?t136:t132))={:.2f}",
                   cls, grp, h[0], h[1], h[2], h[3], HalfToF32(h[0]), HalfToF32(h[1]),
                   HalfToF32(h[2]), r, fail, depth - 2.f * r, sub, b8, b9, t132, t136, lodCnt,
                   b8 * 0.0625f * (lodCnt > 1 ? t136 : t132));
  }
  if ((s_n.fetch_add(1, std::memory_order_relaxed) + 1) % 100000u == 0u)
    MCLA_LOG_ERROR("PROP-CENSUS grpcls vpOut={} vpOutReached={} vpInNonLeaf={} vpInLeafReached={} "
                   "vpInLeafRejected={} alt(reached,in0) nn={} ny={} yn={} yy={}",
                   s_cls[0].load(), s_cls[1].load(), s_cls[2].load(), s_cls[3].load(),
                   s_cls[4].load(), s_alt[0][0].load(), s_alt[0][1].load(), s_alt[1][0].load(),
                   s_alt[1][1].load());
}

PPC_FUNC(sub_8230D2C0) {
  PropCensus &c = PC();
  if (!c.census && !c.noVp && !c.noOcc) {
    __imp__sub_8230D2C0(ctx, base);
    return;
  }
  t_grpReached = true;
  const uint32_t n = c.loops.fetch_add(1, std::memory_order_relaxed) + 1;
  if (c.census && n <= 24) {
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    uint32_t type = 0, lodCnt = 0;
    uint16_t lo = 0, hi = 0;
    float d132 = 0, d136 = 0, d140 = 0;
    const bool ok = mem.ReadU32BE(0x8288D0BCu, &type) &&
                    mem.ReadU16BE(ctx.r3.u32 + 10u, &lo) &&
                    mem.ReadU16BE(ctx.r3.u32 + 12u, &hi) &&
                    mem.ReadF32BE(type + 132u, &d132) && mem.ReadF32BE(type + 136u, &d136) &&
                    mem.ReadF32BE(type + 140u, &d140) && mem.ReadU32BE(type + 144u, &lodCnt);
    MCLA_LOG_ERROR("PROP-CENSUS loop#{} grp={:08X} type={:08X} idx={}..{} grpInside={} "
                   "shadow={} t132={} t136={} t140={} lodCnt={} ok={}",
                   n, ctx.r3.u32, type, lo, hi, ctx.r4.u32, ctx.r5.u32 & 0xFFu, d132, d136,
                   d140, lodCnt, ok ? 1 : 0);
    uint32_t arr = 0, w[8] = {};
    (void)mem.ReadU32BE(type + 208u, &arr);
    for (int k = 0; k < 8; ++k) (void)mem.ReadU32BE(arr + 32u * lo + 4u * k, &w[k]);
    MCLA_LOG_ERROR("PROP-CENSUS rec arr={:08X} rec@{:08X} = {:08X} {:08X} {:08X} {:08X} "
                   "{:08X} {:08X} {:08X} {:08X}",
                   arr, arr + 32u * lo, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
  }
  ++t_propLoop;
  __imp__sub_8230D2C0(ctx, base);
  --t_propLoop;
  if (c.census && (n % 2000u) == 0u) PropSummary(c);
}

PPC_FUNC(sub_8217C328) {
  if (t_propLoop <= 0) {
    __imp__sub_8217C328(ctx, base);
    return;
  }
  PropCensus &c = PC();
  const uint32_t vp = ctx.r3.u32;
  const float x = static_cast<float>(ctx.f1.f64), y = static_cast<float>(ctx.f2.f64),
              z = static_cast<float>(ctx.f3.f64), r = static_cast<float>(ctx.f4.f64);
  // Caller frame (sub_8230D2C0): r1+304..367 = instance matrix rows from sub_8230C0A0
  // (row3 = record+0..15), r1+144..159 = dequantised record+16 bytes, r1+104 = record+12.
  static std::atomic<uint32_t> s_inDump{0};
  if (c.census && s_inDump.fetch_add(1, std::memory_order_relaxed) < 24) {
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    float m[20] = {};
    uint32_t w104 = 0;
    for (int k = 0; k < 16; ++k) (void)mem.ReadF32BE(ctx.r1.u32 + 304u + 4u * k, &m[k]);
    for (int k = 0; k < 4; ++k) (void)mem.ReadF32BE(ctx.r1.u32 + 144u + 4u * k, &m[16 + k]);
    (void)mem.ReadU32BE(ctx.r1.u32 + 104u, &w104);
    MCLA_LOG_ERROR("PROP-CENSUS inst c=({},{},{}) r={} M0=({},{},{},{}) M1=({},{},{},{}) "
                   "M2=({},{},{},{}) T=({},{},{},{}) q=({},{},{},{}) w104={:08X}",
                   x, y, z, r, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10],
                   m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19], w104);
  }
  __imp__sub_8217C328(ctx, base);
  if (c.census) {
    float depth = 0;
    const int fail = VpTest(vp, x, y, z, r, &depth);
    const int b = Bucket(depth);
    const bool culled = ctx.r3.u32 == 0;
    (culled ? c.vpFail : c.vpPass)[b].fetch_add(1, std::memory_order_relaxed);
    if (culled && fail >= 0 && fail < 8)
      c.vpFailPlane[fail].fetch_add(1, std::memory_order_relaxed);
    if (culled && b <= 1 && c.logged.fetch_add(1, std::memory_order_relaxed) < 40)
      MCLA_LOG_ERROR("PROP-CENSUS vpcull-near c=({:.2f},{:.2f},{:.2f}) r={:.2f} depth={:.2f} "
                     "plane={} ret={}",
                     x, y, z, r, depth, fail, ctx.r3.u32);
    // VPFAIL-DUMP: frame-tagged rows of failing instances with all 8 signed plane distances.
    static std::atomic<uint32_t> s_fd{0};
    if (culled && s_fd.fetch_add(1, std::memory_order_relaxed) < 400) {
      float d[8] = {};
      PlaneDist(vp, x, y, z, d);
      MCLA_LOG_ERROR("PROP-CENSUS vpfail c=({:.2f},{:.2f},{:.2f}) r={:.2f} plane={} "
                     "d=({:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f})",
                     x, y, z, r, fail, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    }
    DumpPlanes(vp);
  }
  if (c.noVp && ctx.r3.u32 == 0) ctx.r3.u64 = 1;
}

PPC_FUNC(sub_82175238) {
  if (t_propLoop <= 0) {
    __imp__sub_82175238(ctx, base);
    return;
  }
  PropCensus &c = PC();
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const uint32_t sys = ctx.r3.u32, idx = ctx.r4.u32, sp = ctx.r5.u32, mode = ctx.r6.u32;
  float s[4] = {};
  for (int k = 0; k < 4; ++k) (void)mem.ReadF32BE(sp + 4u * static_cast<uint32_t>(k), &s[k]);
  __imp__sub_82175238(ctx, base);
  const uint32_t res = ctx.r3.u32;
  if (c.census) {
    float depth = 0;
    (void)VpTest(Viewport(), s[0], s[1], s[2], s[3], &depth);
    const int b = Bucket(depth);
    if (res & 0x10000000u) c.occHit[b].fetch_add(1, std::memory_order_relaxed);
    else if (res & 0x20000000u) c.occVis[b].fetch_add(1, std::memory_order_relaxed);
    else c.occNone[b].fetch_add(1, std::memory_order_relaxed);
    if ((res & 0x14000000u) != 0u && b <= 1 &&
        c.logged.fetch_add(1, std::memory_order_relaxed) < 40) {
      uint32_t sysBase = 0, occCnt = 0;
      const bool ok = mem.ReadU32BE(sys, &sysBase) &&
                      mem.ReadU32BE(sysBase + 57792u * idx + 52224u, &occCnt);
      MCLA_LOG_ERROR("PROP-CENSUS occl-near c=({:.2f},{:.2f},{:.2f}) r={:.2f} depth={:.2f} "
                     "res={:08X} mode={} vpIdx={} occCnt={} ok={}",
                     s[0], s[1], s[2], s[3], depth, res, mode, idx, occCnt, ok ? 1 : 0);
    }
  }
  if (c.noOcc && (mode & 1u) == 0u) ctx.r3.u64 = res & ~0x14000000u;
}

// ===========================================================================
// LOCK-VIEW (2026-11-23): sub_82421CA0 is the guest D3D resource Lock (called by
// VB lock sub_82422320). Its tail (0x82421EE8..0x82421F04) returns
// `(pa & 0x1FFFFFFF) + 0xC0000000` (+0x1000 for the E-page quirk), i.e. a C-view
// pointer. On the console that view aliases the A-view pool bytes; this tree has no
// view aliasing (src/kernel/heap_arena.h:246-258), so CPU reads/writes through the
// returned pointer hit unowned flat memory (all zeros). Measured: prop instance array
// [type+208] = 0xC5A80F20 reads all-zero, 0xA5A80F20 holds the real records
// (x=935.2,y=33.5,z=-1374.8), so every prop instance collapsed onto the model origin
// and failed the viewport test. Fix: hand the guest the pool VA of the same byte.
// MCLA_NO_LOCKVIEW=1 restores the raw C-view return.
// ===========================================================================
PPC_FUNC_IMPL(__imp__sub_82421CA0);
PPC_FUNC(sub_82421CA0) {
  __imp__sub_82421CA0(ctx, base);
  static const bool off = EnvOn("MCLA_NO_LOCKVIEW");
  const uint32_t v = ctx.r3.u32;
  if (off || v < 0xC0000000u) return;
  uint32_t pool = 0;
  if (mcla::boot::TranslatePhysViewToPoolVa(v, &pool) == mcla::boot::PhysViewResult::kMapped) {
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n % 5000u) == 0u)
      MCLA_LOG_INFO("LOCK-VIEW #{} view={:08X} -> pool={:08X} lr={:08X}", n, v, pool,
                    static_cast<uint32_t>(ctx.lr));
    ctx.r3.u64 = pool;
  }
}

// ===========================================================================
// CVIEW-PRODUCERS (2026-11-23, LOG-ONLY, MCLA_PROP_CENSUS=1): the other guest sites that
// build `(pa & 0x1FFFFFFF) + ((pa>>20)+0x200 & 0x1000) + 0xC0000000` (raw scan of
// build/cache/mcla_pe.bin for `addis rD,rS,0xC000` preceded by the clrlwi-3 idiom).
// Counts calls only, so a fix is aimed at producers that actually run.
// ===========================================================================
namespace {
void CviewCount(const char *name, std::atomic<uint32_t> &n, const PPCContext &ctx, uint32_t r3in) {
  if (!PC().census) return;
  const uint32_t k = n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (k <= 3 || (k % 2000u) == 0u)
    MCLA_LOG_ERROR("CVIEW-PRODUCER {} #{} r3in={:08X} r3out={:08X} lr={:08X}", name, k, r3in,
                   ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
}
}  // namespace
#define MCLA_CVIEW_PROBE(fn)                                \
  PPC_FUNC_IMPL(__imp__##fn);                               \
  PPC_FUNC(fn) {                                            \
    static std::atomic<uint32_t> s_n{0};                    \
    const uint32_t r3in = ctx.r3.u32;                       \
    __imp__##fn(ctx, base);                                 \
    CviewCount(#fn, s_n, ctx, r3in);                        \
  }
MCLA_CVIEW_PROBE(sub_82411530)
MCLA_CVIEW_PROBE(sub_8241A630)
MCLA_CVIEW_PROBE(sub_8241A6E0)
MCLA_CVIEW_PROBE(sub_8241A9C0)
MCLA_CVIEW_PROBE(sub_8241DAE0)
MCLA_CVIEW_PROBE(sub_8241E088)
// CVIEW-MIRROR (2026-11-23): sub_82424320 writes PM4 headers 0xC0003B00/0xC0002B00, then
// (0x824243B0..0x824243F0) memcpy(cmd, view, [rec+876]) where view = C-view of
// [rec+872] + [r5+32] -- a CPU READ through the C view. Measured (build/pV9.log
// CVIEW-READ): view=DC1E6B00 size=1332, first 64 view bytes all zero, pool BC1E6B00 has
// 36 non-zero, so the guest copied 1332 zero bytes into the command buffer. This tree has
// no view aliasing (see LOCK-VIEW), so for the duration of the call the pool bytes are
// mirrored into the view range and the original view bytes restored after.
// MCLA_NO_CVIEW_MIRROR=1 restores the raw behaviour.
PPC_FUNC_IMPL(__imp__sub_82424320);
PPC_FUNC(sub_82424320) {
  static const bool off = EnvOn("MCLA_NO_CVIEW_MIRROR");
  if (off) {
    __imp__sub_82424320(ctx, base);
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const uint32_t r31 = ctx.r5.u32, r28 = ((ctx.r10.u32 + 112u) << 3) & 0xFFFFFFF8u;
  uint32_t recOff = 0, pa = 0, size = 0, add = 0, pool = 0;
  bool ok = mem.ReadU32BE(r28 + r31, &recOff) && mem.ReadU32BE(recOff + r31 + 872u, &pa) &&
            mem.ReadU32BE(recOff + r31 + 876u, &size) && mem.ReadU32BE(r31 + 32u, &add);
  pa += add;
  const uint32_t view = (pa & 0x1FFFFFFFu) + (((pa >> 20) + 0x200u) & 0x1000u) + 0xC0000000u;
  ok = ok && size != 0u && size <= (4u << 20) &&
       mcla::boot::TranslatePhysViewToPoolVa(view, &pool) == mcla::boot::PhysViewResult::kMapped;
  std::vector<uint8_t> saved, src;
  bool mirrored = false;
  if (ok) {
    saved.resize(size);
    src.resize(size);
    mirrored = mem.ReadBytes(view, saved.data(), size) && mem.ReadBytes(pool, src.data(), size) &&
               mem.WriteBytes(view, src.data(), size);
  }
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8 || (n % 1000u) == 0u) {
    uint32_t nzOld = 0, nzNew = 0;
    for (uint32_t k = 0; k < saved.size(); ++k) nzOld += saved[k] != 0;
    for (uint32_t k = 0; k < src.size(); ++k) nzNew += src[k] != 0;
    MCLA_LOG_INFO("CVIEW-MIRROR sub_82424320 #{} view={:08X} pool={:08X} size={} mirrored={} "
                  "nonzero view={} pool={}",
                  n, view, pool, size, mirrored ? 1 : 0, nzOld, nzNew);
  }
  __imp__sub_82424320(ctx, base);
  if (mirrored) (void)mem.WriteBytes(view, saved.data(), size);
}
MCLA_CVIEW_PROBE(sub_824281E8)
#undef MCLA_CVIEW_PROBE
