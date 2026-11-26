#pragma once

// Phase 2 split of src/patches.cpp: per-family hook entry points.
// Each hooks/*.cpp TU owns its PPC_FUNC bodies; patches.cpp keeps only the
// registry (mcla_ApplyPatches) and the kernel/input/GPU-observers needed by
// it.
//
// LINKAGE (2026-10-21, fix_hooks2). THESE 13 MUST HAVE C++ LINKAGE. The guest's
// dispatch table is generated/ppc_xenon/ppc_func_mapping.cpp -- `{ 0x821C29A0,
// sub_821C29A0 }` at :4001 and the other twelve rows -- and that reference is to
// the C++-MANGLED symbol `?sub_821C29A0@@YAXAEIAUPPCContext@@PEAE@Z`. The
// generated TU exports that name as a WEAK alias of its own body
// (`__attribute__((alias("__imp__sub_821C29A0"))) PPC_WEAK_FUNC(sub_821C29A0);`,
// generated/ppc_xenon/ppc_recomp.15.cpp:10696), and the weak alias is what the
// table binds.
//
// The `extern "C" { ... }` block that used to be here made all 13 definitions
// C-LINKED, and a C-linked definition lands on a DIFFERENT symbol that nothing
// references. The /MAP of the build that produced build/w847.log proves it:
//   build/mcla.map  0001:00065d30  sub_821C29A0
//                                alloc_census.cpp.obj          <- C linkage, ORPHAN
//   build/mcla.map  0001:007b8e70  ?sub_821C29A0@@YAXAEIAUPPCContext@@PEAE@Z
//                                ppc_recomp.15.cpp.obj          <- what the table binds
// So the 13 hooks were DEAD and their 34 census printers had NEVER fired
// (0 occurrences of any of them in build/w847.log, build/w846.log,
// build/w840.log). The block was added by the 2026-09-20 module reorg
// (5eb9932), which MOVED the bodies out of src/patches.cpp; before it, the same
// `PPC_FUNC(sub_X)` bodies had no `extern "C"` declaration in scope, so they
// were C++-linked, STRONG, and won over the generated weak alias.
//
// A C++-linked definition is a strong external and overrides the generated weak
// alias, so the table (and every recompiled TU that calls `sub_X(ctx, base)`)
// binds THIS body, which chains to `__imp__sub_X` -- the generated body --
// exactly as it did before the reorg.
//
// `PPC_FUNC(x)` is the C++-mangled declaration form the generated tree uses
// (src/ppc_context.h:41); `PPC_FUNC_IMPL(x)` is `extern "C"` and is reserved
// for the `__imp__sub_X` thunks, which the generated TUs also define with C
// linkage. DO NOT put these 13 back in an `extern "C"` block.

#include "ppc_context.h"
#include "kernel/memory.h"

// hooks/alloc_census.cpp — OOM/allocation-path/pool16/slab census
PPC_FUNC(sub_821C29A0);
PPC_FUNC(sub_82130B50);
PPC_FUNC(sub_82130010);
PPC_FUNC(sub_82130048);
PPC_FUNC(sub_8218DCE8);
PPC_FUNC(sub_8218DE38);
PPC_FUNC(sub_821C22D0);
PPC_FUNC(sub_821C1BB0);
PPC_FUNC(sub_821DE9D8);
PPC_FUNC(sub_821DE908);
PPC_FUNC(sub_821C09C8);
// hooks/config_dispatch.cpp — config-dispatch census + the TEXCREATE-SC fix
PPC_FUNC(sub_82177EB0);
PPC_FUNC(sub_8218CC70);
// hooks/stability_ports.cpp — Wave 1 warp fix, AI null guard, AES-NI, inflate_fast
PPC_FUNC(sub_8221BE98);
PPC_FUNC(sub_822BC658);
PPC_FUNC(sub_821E1598);
PPC_FUNC(sub_821E4B20);
// hooks/frame_timing_ports.cpp — Wave 2 FPS unlock, swap interval, continuous timing, sweep, pad latch
PPC_FUNC(sub_821BDA90);
PPC_FUNC(sub_824199B0);
PPC_FUNC(sub_825EE188);
PPC_FUNC(sub_822653A8);
PPC_FUNC(sub_82256220);
PPC_FUNC(sub_822C8AF8);
PPC_FUNC(sub_822C7FC8);
PPC_FUNC(sub_82614D30);
// apu/audio.cpp — Wave 1 audio mixer thread priority + MMCSS
PPC_FUNC(sub_823EC8C8);

// Wave 3 (2026-11-19). All of these are C++-LINKAGE strong overrides of the
// generated weak aliases; see the linkage note at the top of this file. Every
// body defaults to a straight __imp__ passthrough and is enabled only by its
// own MCLA_* environment gate, so the untagged boot is byte-for-byte unchanged.
//
// hooks/wave3_polish.cpp (worker 2)
PPC_FUNC(sub_825EE0E0);   // 0x825EE0E0 — Flash int setter / button prompts
PPC_FUNC(sub_825EE970);   // 0x825EE970 — button prompt token
// Problem 6 (swarm p6-edit): title-camera log-only wrapper. 1-site, unowned.
PPC_FUNC(sub_822C8118);   // 0x822C8118 — title camera setter (MCLA_TITLECAM_LOG)
// hooks/wave3_guards.cpp (worker 2)
PPC_FUNC(sub_821C06C8);   // 0x821C06C8 — developer options
PPC_FUNC(sub_823794E0);   // 0x823794E0 — car shader-group post-call repair
PPC_FUNC(sub_823793E8);   // 0x823793E8 — car shader-group post-call repair
PPC_FUNC(sub_8259AA28);   // 0x8259AA28 — phBoundComposite release guard
PPC_FUNC(sub_8230D988);   // 0x8230D988 — impostor round-robin search guard
PPC_FUNC(sub_822E5B00);   // 0x822E5B00 — skip-intro render-pass mask
// hooks/wave3_menu.cpp (worker 1) — declared here so the generated TUs and the
// midhook overlay preamble see the same C++-mangled entry points.
PPC_FUNC(sub_82220308);   // 0x82220308
PPC_FUNC(sub_82631FD0);   // 0x82631FD0
PPC_FUNC(sub_82630E40);   // 0x82630E40
PPC_FUNC(sub_826309E0);   // 0x826309E0
PPC_FUNC(sub_8260EBB8);   // 0x8260EBB8 — depth-of-field
PPC_FUNC(sub_8260E9A0);   // 0x8260E9A0 — live motion-blur companion

// hooks/wave3_polish.cpp — opt-in guest aspect-ratio constant write. Defined
// here (not called from a static initializer) so the startup call site stays
// in app.cpp, which owns App::Initialize().
void MclaApplyAspectRatio();


// Session 73: tiny-slab fill-on-alloc census (implemented in
// hooks/alloc_census.cpp, declared in patches.h historically; keep the
// declaration there — this header only carries the hook entry points).

// Census gate: keep false for ship; true enables dense logging inside
// alloc_census.cpp hooks. If true, also needs MCLA_ALLOC_DEBUG defined.
static constexpr bool kPool16CensusEnabled = false;

// Checked heap-field read: returns 0 when the guest address is not backed.
// Shared by alloc_census.cpp and config_dispatch.cpp.
inline uint32_t OomCensusReadU32(uint32_t addr, bool *ok = nullptr) {
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint32_t val = 0;
  const bool read = mem.IsValid(addr, 4) && mem.ReadU32BE(addr, &val);
  if (ok)
    *ok = read;
  return val;
}

// CP progress helper: advance guest publication counter for all tracked devices.
// Used to unblock guest fence waits when CP is stalled.
// Declared in gpu_cp.h as mcla::gpu::CpAdvanceGuestPublication.
