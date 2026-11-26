// TEMP-FLOW census (2026-11-11): one counter per station of docs/GAME_FLOW_MAP.md
// that had no hook yet. Count-only pass-throughs: every hook calls the original
// unchanged. A host thread prints FLOW-SUM every 2 s so a run shows WHERE the
// main/render threads are (initial load vs frame loop, world pass, submits).
#include "diag_threads.h"
#include "generated/ppc_xenon/ppc_recomp_shared.h"
#include <cpu/ppc_context.h>
#include "logging.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace {
enum Station {
  kLoadPhaseUpd,   // sub_822C52C8  phase-manager Update(mgr, phase)
  kPhaseDraw,      // sub_822C5348  phase Draw(mgr, phase)
  kGameUpdate,     // sub_822C1FA8  frame loop: game update   (>0 => loop entered)
  kRenderHand,     // sub_82305D00  frame loop: hand frame to render thread
  kGamePost,       // sub_822BF110  frame loop: post update
  kLoopExitCheck,  // sub_821776B8  frame loop condition
  kWorld,          // sub_822E6408  world / 3D pass
  kScene,          // sub_822E55D0  scene pass
  kPost,           // sub_822DD790  post/overlay pass
  kSubmit,         // sub_82304DF0  render-list submit
  kStatechart,     // sub_82554E20  .sc.xml statechart engine
  kUiFlash,        // sub_82305A00
  kPresent,        // sub_821775D8  graphics vt+0 Present (real frame path)
  kClearSwap,      // sub_8217B7B0  clear+Swap (Present and loading sub_821FC008 both call it)
  kCount
};
const char *const kNames[kCount] = {"loadPhaseUpd", "phaseDraw", "gameUpdate", "renderHand",
                                    "gamePost",     "loopExit",  "world",      "scene",
                                    "post",         "submit",    "statechart", "uiFlash",
                                    "present",      "clearSwap"};
std::atomic<uint64_t> g_n[kCount];
std::atomic<uint32_t> g_phaseUpd[16], g_phaseDraw[16];
const auto g_t0 = std::chrono::steady_clock::now();

void First(Station s, PPCContext &ctx) {
  if (g_n[s].fetch_add(1) == 0) {
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
    MCLA_LOG_WARN("FLOW-FIRST {} t={:.1f}s r3={:08X} r4={:08X} lr={:08X} tid={}", kNames[s],
                  t, ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr),
                  GetCurrentThreadId());
  }
}

const bool g_sumThread = [] {
  std::thread([] { mcla::diag::NameThisThread("flow-census");
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      std::string s, pu, pd;
      for (int i = 0; i < kCount; ++i) s += fmt::format(" {}={}", kNames[i], g_n[i].load());
      for (int i = 0; i < 16; ++i) {
        if (const uint32_t v = g_phaseUpd[i].load()) pu += fmt::format(" p{}={}", i, v);
        if (const uint32_t v = g_phaseDraw[i].load()) pd += fmt::format(" p{}={}", i, v);
      }
      MCLA_LOG_WARN("FLOW-SUM{} | upd:{} | draw:{}", s, pu, pd);
    }
  }).detach();
  return true;
}();
}  // namespace

#define FLOW_HOOK(fn, st)            \
  PPC_FUNC_IMPL(__imp__##fn);        \
  PPC_FUNC(fn) {                     \
    First(st, ctx);                  \
    __imp__##fn(ctx, base);          \
  }

PPC_FUNC_IMPL(__imp__sub_822C52C8);
PPC_FUNC(sub_822C52C8) {
  First(kLoadPhaseUpd, ctx);
  g_phaseUpd[ctx.r4.u32 & 15].fetch_add(1);
  __imp__sub_822C52C8(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_822C5348);
PPC_FUNC(sub_822C5348) {
  First(kPhaseDraw, ctx);
  g_phaseDraw[ctx.r4.u32 & 15].fetch_add(1);
  __imp__sub_822C5348(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_821776B8);
PPC_FUNC(sub_821776B8) {
  First(kLoopExitCheck, ctx);
  __imp__sub_821776B8(ctx, base);
  if ((ctx.r3.u32 & 0xFF) != 0)
    MCLA_LOG_WARN("FLOW-LOOP-EXIT sub_821776B8 returned {:02X} (frame loop ends)", ctx.r3.u32 & 0xFF);
}
FLOW_HOOK(sub_822C1FA8, kGameUpdate)
FLOW_HOOK(sub_82305D00, kRenderHand)
FLOW_HOOK(sub_822BF110, kGamePost)
FLOW_HOOK(sub_822E6408, kWorld)
FLOW_HOOK(sub_822E55D0, kScene)
FLOW_HOOK(sub_822DD790, kPost)
FLOW_HOOK(sub_82304DF0, kSubmit)
FLOW_HOOK(sub_82554E20, kStatechart)
FLOW_HOOK(sub_82305A00, kUiFlash)
FLOW_HOOK(sub_821775D8, kPresent)
FLOW_HOOK(sub_8217B7B0, kClearSwap)

// TEMP-JOB (2026-11-11): fl1 crash_dump — worker sub_825BF438 did an atomic
// increment on a1[19] == 0 (sub_821CE0D8). Producer sub_825BFD20 passes
// &local as that field via sub_821C4B00(mgr, fn, &args, ...). Dump both ends.
PPC_FUNC_IMPL(__imp__sub_821C4B00);
PPC_FUNC(sub_821C4B00) {
  if (ctx.r4.u32 == 0x825BF438u) {
    static std::atomic<uint32_t> s_n{0};
    if (s_n.fetch_add(1) < 8) {
      std::string w;
      for (uint32_t i = 0; i < 12; ++i) w += fmt::format(" {:08X}", PPC_LOAD_U32(ctx.r5.u32 + 4 * i));
      MCLA_LOG_WARN("TEMP-JOB-ENQ mgr={:08X} args={:08X} r6={:08X} lr={:08X} args[0..11]:{}",
                    ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr), w);
    }
  }
  __imp__sub_821C4B00(ctx, base);
}
static thread_local uint32_t t_jobA1 = 0;
static thread_local uint64_t t_jobEnterNs = 0;
PPC_FUNC_IMPL(__imp__sub_821CE0D8);
PPC_FUNC(sub_821CE0D8) {
  if (ctx.r3.u32 == 0) {  // the AV in fl1/jb1: atomic ++ on guest address 0
    static std::atomic<uint32_t> s_z{0};
    if (s_z.fetch_add(1) < 4) {
      const uint32_t a1 = t_jobA1;
      std::string w;
      for (uint32_t i = 15; i < 24 && a1; ++i) w += fmt::format(" [{}]={:08X}", i, PPC_LOAD_U32(a1 + 4 * i));
      const uint64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
      MCLA_LOG_WARN("TEMP-JOB-ZERO lr={:08X} tid={} jobA1={:08X} sinceEnterNs={} now:{}",
                    static_cast<uint32_t>(ctx.lr), GetCurrentThreadId(), a1,
                    now - t_jobEnterNs, w);
    }
  }
  __imp__sub_821CE0D8(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_825BF438);
PPC_FUNC(sub_825BF438) {
  const uint32_t a1 = ctx.r3.u32;
  t_jobA1 = a1;
  t_jobEnterNs = std::chrono::steady_clock::now().time_since_epoch().count();
  if (PPC_LOAD_U32(a1 + 76) == 0) {
    static std::atomic<uint32_t> s_n{0};
    if (s_n.fetch_add(1) < 8) {
      std::string w;
      for (uint32_t i = 0; i < 26; ++i) w += fmt::format(" {:08X}", PPC_LOAD_U32(a1 + 4 * i));
      MCLA_LOG_WARN("TEMP-JOB-NULL a1={:08X} lr={:08X} tid={} a1[0..25]:{}", a1,
                    static_cast<uint32_t>(ctx.lr), GetCurrentThreadId(), w);
    }
  }
  __imp__sub_825BF438(ctx, base);
}

bool MclaFlowWorldStarted() { return g_n[kWorld].load(std::memory_order_relaxed) != 0; }
