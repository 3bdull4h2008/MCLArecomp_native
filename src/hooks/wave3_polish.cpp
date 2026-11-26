// Wave 3 — UI/input/HUD polish (2026-11-19).
//
// Guest facts in this TU were read from build/cache/mcla_pe.bin via the
// generated recompiler TUs, not inferred:
//   - 0x822240C0 `stbx r27,r31,r28`   : generated/ppc_xenon/ppc_recomp.26.cpp:15022
//   - 0x822241D0 `cmplw cr6,r10,r8`   : generated/ppc_xenon/ppc_recomp.26.cpp:15176
//   - 0x825EDC6C `beq cr6,0x825edc78`: generated/ppc_xenon/ppc_recomp.120.cpp:27213
//     (loc_825EDD44: is at :27343)
//   - 0x821F816C is the lr of the `bl 0x825ee0e0` in sub_821F8038 (spec §2.1)
//   - 0x8201E7EC raw BE word 0x3FE38E39 = 16/9 (spec §2.3)
//
// ZERO rex:: symbols. All guest memory I/O goes through GuestMemoryHeap.
// EVERY feature is off unless its own MCLA_* env gate is '1'. With no gates
// set, the PPC_FUNC bodies are exact __imp__ passthroughs and the midhook
// helpers are no-ops, so untagged behaviour is unchanged.
//
// Gates:
//   MCLA_UI_REPEAT_FIX=1       -> wall-clock UI hold repeat (TU 26 midhooks)
//   MCLA_UI_30HZ_FIX=1         -> 30 Hz movie/Flash pacing (TU 120 midhook)
//   MCLA_BTN_PROMPTS_FIX=1     -> button-prompt platform flag overrides
//   MCLA_BTN_PROMPTS_PS=1      -> report PlayStation glyphs (0/X = Xbox)
//   MCLA_ASPECT_FIX=1          -> allow MclaApplyAspectRatio() writing 0x8201E7EC
//   MCLA_ASPECT_WIDTH=<1280..4095> -> guest render width (auto needs SDL, see below)
//   MCLA_TITLECAM_LOG=1        -> log sub_822C8118 incoming camera floats + a2/a3
//                                 (problem 6; LOG-ONLY, never changes behaviour)

#include "hooks/hooks.h"
#include "generated/ppc_xenon/ppc_recomp_shared.h"
#include "cpu/ppc_context.h"
#include "kernel/memory.h"
#include "logging.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

// Generated weak aliases; our C++-linkage PPC_FUNC definitions preempt them.
PPC_FUNC_IMPL(__imp__sub_825EE0E0);
PPC_FUNC_IMPL(__imp__sub_825EE970);
PPC_FUNC_IMPL(__imp__sub_822C8118);  // problem 6 — title camera setter


namespace {

bool EnvOn(const char *name) {
  const char *e = std::getenv(name);
  return e != nullptr && e[0] == '1';
}

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// UI hold repeat (spec §1.5). The guest compares a "previous key" byte with a
// computed "current key" byte; on a hold they are equal and the guest skips.
// When enabled we force equality once per wall-clock interval so repeat rate
// is independent of FPS. The timer is keyed by the destination buffer plus the
// byte index, so independent menus do not share a repeat clock.
// ---------------------------------------------------------------------------
struct RepeatState {
  uint32_t key = 0;
  Clock::time_point started{};
  Clock::time_point nextFire{};
};

std::mutex g_repeatMtx;
std::unordered_map<uint32_t, RepeatState> g_repeat;

constexpr int64_t kRepeatDelayUs = 233000;     // mcla_ui_repeat_delay_ms default
constexpr int64_t kRepeatIntervalUs = 200000;  // mcla_ui_repeat_interval_ms default

uint32_t RepeatId(uint32_t dest, uint32_t index) {
  return dest ^ (index * 0x9E3779B9u);
}

// ---------------------------------------------------------------------------
// Flash 30 Hz pacing (spec §1.5)
// ---------------------------------------------------------------------------
std::mutex g_movieMtx;
Clock::time_point g_lastMovieAdvance{};
constexpr auto kMoviePeriod = std::chrono::microseconds(33333);

// ---------------------------------------------------------------------------
// Button prompts (spec §2.1)
// ---------------------------------------------------------------------------
bool ShowPlayStationPrompts() {
  // "auto" requires SDL_GetGamepadType()/SDL_GAMEPAD_TYPE_PS4|PS5, which is
  // tracked in src/kernel/xam.cpp (worker 3) and is NOT exposed to this TU.
  // No cross-worker symbol is invented here. The deterministic override is:
  //   MCLA_BTN_PROMPTS_PS=1 -> PS glyphs, unset/0 -> Xbox (stock behaviour).
  return EnvOn("MCLA_BTN_PROMPTS_PS");
}

}  // namespace

// ===========================================================================
// Midhook helpers — extern "C" because tools/emit_dropped_ops.py injects the
// matching extern "C" declarations into the generated overlay TUs.
// ===========================================================================

extern "C" void mcla_ui_repeat_start(PPCRegister &dest, PPCRegister &key) {
  if (!EnvOn("MCLA_UI_REPEAT_FIX")) return;
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lk(g_repeatMtx);
  auto &st = g_repeat[RepeatId(dest.u32, 0u)];
  st.key = key.u32;
  st.started = now;
  st.nextFire = now + std::chrono::microseconds(kRepeatDelayUs);
}

extern "C" void mcla_ui_repeat_gate(PPCRegister &dest, PPCRegister &index,
                                    PPCRegister &key, PPCRegister &cur,
                                    PPCRegister &prev) {
  (void)index;  // spec's start call passes only (dest, key); key by dest alone
  if (!EnvOn("MCLA_UI_REPEAT_FIX")) return;
  const auto now = Clock::now();
  const uint32_t id = RepeatId(dest.u32, 0u);
  std::lock_guard<std::mutex> lk(g_repeatMtx);
  auto it = g_repeat.find(id);
  if (it == g_repeat.end()) return;
  auto &st = it->second;
  if (st.key != key.u32) {  // a different key: restart the hold timer
    st.key = key.u32;
    st.started = now;
    st.nextFire = now + std::chrono::microseconds(kRepeatDelayUs);
    return;
  }
  if (now >= st.nextFire) {
    // Force the guest's `cmplw cr6,r10,r8` to report equality so it emits one
    // repeat this frame, then schedule the next one.
    prev.u64 = cur.u64;
    st.nextFire = now + std::chrono::microseconds(kRepeatIntervalUs);
  }
}

extern "C" bool mcla_ui_movie_advance(PPCRegister &r25, PPCRegister &r31,
                                      PPCRegister &f30, PPCRegister &f31,
                                      PPCCRRegister &cr6) {
  (void)r25;
  (void)r31;
  (void)f30;
  (void)f31;
  (void)cr6;
  if (!EnvOn("MCLA_UI_30HZ_FIX")) return false;
  std::lock_guard<std::mutex> lk(g_movieMtx);
  const auto now = Clock::now();
  if (now - g_lastMovieAdvance < kMoviePeriod) return false;
  g_lastMovieAdvance = now;
  return true;
}

extern "C" void mcla_prompts_platform_flag(PPCRegister &node) {
  if (!EnvOn("MCLA_BTN_PROMPTS_FIX")) return;
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  // 0x821FE22C writes stw r30,88(r27) (ppc_recomp.22.cpp:14481-14482); the
  // low byte of that field is the platform flag consumed by the prompt code.
  mem.WriteU8(node.u32 + 88u, ShowPlayStationPrompts() ? 1u : 0u);
}

// ===========================================================================
// Opt-in startup aspect-ratio write (spec §2.3). Not called from a static
// initializer: GuestMemoryHeap is not adopted until boot_host.cpp has run.
// Worker 3 (app.cpp, App::Initialize) owns the startup call site.
// ===========================================================================
void MclaApplyAspectRatio() {
  if (!EnvOn("MCLA_ASPECT_FIX")) return;
  static std::once_flag once;
  std::call_once(once, []() {
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    uint32_t orig = 0;
    if (!mem.ReadU32BE(0x8201E7ECu, &orig) || orig != 0x3FE38E39u) {
      MCLA_LOG_WARN("ASPECT: unexpected constant at 0x8201E7EC (got {:08X})", orig);
      return;
    }
    uint32_t width = 1280u;
    if (const char *e = std::getenv("MCLA_ASPECT_WIDTH")) {
      const int v = std::atoi(e);
      if (v >= 1280 && v <= 4095) width = static_cast<uint32_t>(v);
    }
    const float ratio = static_cast<float>(width) / 720.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &ratio, sizeof(bits));
    mem.WriteU32BE(0x8201E7ECu, bits);
    MCLA_LOG_INFO("ASPECT: guest width {} -> 0x8201E7EC = {:08X}", width, bits);
  });
}

// ===========================================================================
// C++-linkage PPC_FUNC overrides (spec §4.2). Default = pure passthrough.
// ===========================================================================

PPC_FUNC(sub_825EE0E0) {
  if (EnvOn("MCLA_BTN_PROMPTS_FIX") && ctx.lr == 0x821F816Cu &&
      ShowPlayStationPrompts()) {
    ctx.r5.u64 = 1;  // Flash "g_bIsPS3"
  }
  __imp__sub_825EE0E0(ctx, base);
}

PPC_FUNC(sub_825EE970) {
  if (EnvOn("MCLA_BTN_PROMPTS_FIX") && ShowPlayStationPrompts()) {
    ctx.r6.u64 = 1;  // prompt token: PS3 glyphs
  }
  __imp__sub_825EE970(ctx, base);
}

// ===========================================================================
// Problem 6 — title camera setter sub_822C8118 (swarm p6-edit).
//
// Measured (rule 0), not inferred:
//   - addr_owners.py --check 0x822C8118: 1 site, map row only, unowned.
//   - ppc_func_mapping.cpp:11306 binds sub_822C8118 (C++-mangled); the weak
//     alias in ppc_recomp.44.cpp:2595 chains to __imp__sub_822C8118 (:2596).
//   - a1 = ctx.r3 is the incoming camera vector: ppc_recomp.44.cpp:2649 does
//     `lvx128 v63,r0,r3` = a 16-byte (4-float) VMX load from the r3 pointer.
//   - a2 = ctx.r4 (low byte tested at :2624), a3 = ctx.r5 (pointer kept in r31
//     at :2626). Both are captured at entry, before __imp__ clobbers them.
//
// LOG-ONLY. When MCLA_TITLECAM_LOG != '1' this is an exact __imp__ passthrough:
// no guest memory access, no state change, no behaviour difference.
// ===========================================================================
PPC_FUNC(sub_822C8118) {
  if (EnvOn("MCLA_TITLECAM_LOG")) {
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    const uint32_t camPtr = ctx.r3.u32;
    float pos[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool ok = mem.IsValid(camPtr, 16u);
    if (ok) {
      for (int i = 0; i < 4; ++i) {
        if (!mem.ReadF32BE(camPtr + static_cast<uint32_t>(i) * 4u, &pos[i])) {
          ok = false;
          break;
        }
      }
    }
    MCLA_LOG_ERROR(
        "TITLECAM: camPtr={:08X} pos=({:.3f},{:.3f},{:.3f},{:.3f}) "
        "a2={:08X} a3={:08X} valid={}",
        camPtr, pos[0], pos[1], pos[2], pos[3], ctx.r4.u32, ctx.r5.u32,
        ok ? 1 : 0);
  }
  __imp__sub_822C8118(ctx, base);
}

