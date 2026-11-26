// Wave 2 frame pacing, ring wake-on-demand, continuous physics/input timing,
// and checkpoint sweep ports (2026-11-19).
//
// Zero rex:: APIs and zero unchecked raw guest-memory pointer casts: all guest
// memory accesses go through mcla::kernel::GuestMemoryHeap::Instance().
//
// Env opt-outs / controls (override CVars when set):
//   MCLA_NO_FPS_UNLOCK=1       -> restore stock 30 Hz timing & swap interval (-1)
//   MCLA_FPS_LIMIT=<N>         -> target FPS cap (-1 = stock 30 Hz, 0 = uncapped, 1..240)
//   MCLA_NO_RING_WAKE=1        -> disable poll_wait & pump wake-on-demand
//   MCLA_NO_PHYSICS_DT=1       -> disable continuous-time camera/chassis/bike/clock scaling
//   MCLA_NO_CHECKPOINT_SWEEP=1 -> disable checkpoint & trigger-line segment sweep
//   MCLA_NO_CHECKPOINT_FADE=1  -> restore the guest's frame-rate-dependent checkpoint smoke fade
//   MCLA_NO_BTN_LATCH=1        -> disable script pad button latch
//   MCLA_CAMERA_TRACE=1        -> log camera-tick timing, published matrix delta and vehicle speed
//   MCLA_CAMERA_FREEZE=1       -> seed the camera once at race-ready, then skip camera-system updates

#include "hooks/hooks.h"
#include "generated/ppc_xenon/ppc_recomp_shared.h"
#include "cpu/ppc_context.h"
#include "kernel/memory.h"
#include "common/cvar.h"
#include "logging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <immintrin.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Cross-TU contracts
extern "C" uint64_t mcla_guest_timebase_49875();

namespace apu {
uint64_t SteadyUnderruns();
} // namespace apu
namespace mcla::apu {
using ::apu::SteadyUnderruns;
} // namespace mcla::apu

// Original recompiled guest bodies (__imp__sub_*)
PPC_FUNC_IMPL(__imp__sub_821BDA90);
PPC_FUNC_IMPL(__imp__sub_824199B0);
PPC_FUNC_IMPL(__imp__sub_825EE188);
PPC_FUNC_IMPL(__imp__sub_822653A8);
PPC_FUNC_IMPL(__imp__sub_82256220);
PPC_FUNC_IMPL(__imp__sub_822C8AF8);
PPC_FUNC_IMPL(__imp__sub_822C7FC8);
PPC_FUNC_IMPL(__imp__sub_82614D30);
PPC_FUNC_IMPL(__imp__sub_8247A8D0);

// CVars (configurable in cache/mcla.toml; overridden by MCLA_* env vars)
MCLA_CVAR_INT(mcla_fps, 60, "MCLA",
              "Target FPS (-1 = stock 30 Hz timing, 0 = uncapped, 1..240 = FPS cap)");
MCLA_CVAR_FLOAT(mcla_dt_max, 0.125f, "MCLA",
                "Maximum frame delta clamp in seconds");
MCLA_CVAR_BOOL(mcla_float_clock, true, "MCLA",
               "Double-precision shadow accumulators for engine timer +0x14/+0x18");
MCLA_CVAR_BOOL(mcla_swap_unlock, true, "MCLA",
               "Unlock swap interval 2 -> 1 in sub_824199B0 when FPS unlock is active");
MCLA_CVAR_BOOL(mcla_pace_at_swap, true, "MCLA",
               "Hold frames to target FPS at swap boundary instead of before timer update");
MCLA_CVAR_BOOL(mcla_physics_continuous, true, "MCLA",
               "Continuous-time scaling for camera boom, chassis filter, bike gyro, race clock");
MCLA_CVAR_BOOL(mcla_poll_wait, true, "MCLA",
               "Sleep on release event + HR timer instead of Sleep(0) spin in sub_82148A10");
MCLA_CVAR_INT(mcla_poll_spin_us, 40, "MCLA",
              "Microseconds to spin before sleeping in mcla_poll_wait");
MCLA_CVAR_INT(mcla_poll_tail_us, 400, "MCLA",
              "Microseconds before deadline to wake and spin in mcla_poll_wait");
MCLA_CVAR_BOOL(mcla_pump_wake_on_demand, true, "MCLA",
               "Wake sub_82144888 pump thread on demand when main thread waits on ring");
MCLA_CVAR_INT(mcla_pump_sleep_max_ms, -1, "MCLA",
              "Optional cap (ms) for sub_82144888 sleep (-1 = use wake-on-demand / stock)");
MCLA_CVAR_INT(mcla_pump_wake_pause_s, 60, "MCLA",
              "Seconds to pause pump wake-on-demand after first audio underrun");
MCLA_CVAR_BOOL(mcla_checkpoint_sweep, true, "MCLA",
               "Swept segment continuation for checkpoints and trigger lines");
MCLA_CVAR_INT(mcla_script_pad_latch_ms, 100, "MCLA",
              "Milliseconds to latch missed button edges for polling script threads (0 = off)");

namespace {

// ---------------------------------------------------------------------------
// 0. Env / CVar helpers & checked guest memory accessors
// ---------------------------------------------------------------------------
bool EnvEnabled(const char *name) {
  const char *e = std::getenv(name);
  return e != nullptr && e[0] == '1';
}

bool TryEnvInt(const char *name, int32_t *outVal) {
  const char *e = std::getenv(name);
  if (!e || e[0] == '\0') {
    return false;
  }
  char *end = nullptr;
  const long v = std::strtol(e, &end, 10);
  if (end == e || *end != '\0') {
    return false;
  }
  *outVal = static_cast<int32_t>(v);
  return true;
}

int32_t EffectiveFpsTarget() {
  if (EnvEnabled("MCLA_NO_FPS_UNLOCK")) {
    return -1;
  }
  // MCLA_FPS_CAP=<n> (owner option): 0/unset = existing default; 1..240 = cap;
  // negative or "uncapped" = no limiter. Same pacing path as MCLA_FPS_LIMIT; the
  // guest frame delta comes from the 49.875 MHz timebase (sub_821BDA90), so
  // game speed does not depend on the rate.
  static const int32_t s_cap = [] {
    const char *e = std::getenv("MCLA_FPS_CAP");
    int32_t v = 0;
    if (e && std::strcmp(e, "uncapped") == 0) {
      v = -1;
    } else if (!TryEnvInt("MCLA_FPS_CAP", &v)) {
      v = 0;
    }
    const int32_t eff = v < 0 ? 0 : std::min(v, 240);
    if (v != 0) {
      MCLA_LOG_INFO("FPS-CAP: active MCLA_FPS_CAP={} -> limiter target {} ({})", e, eff,
                    eff == 0 ? "uncapped" : "fps");
    }
    return v == 0 ? INT32_MIN : eff;
  }();
  if (s_cap != INT32_MIN) {
    return s_cap;
  }
  int32_t envFps = 0;
  if (TryEnvInt("MCLA_FPS_LIMIT", &envFps)) {
    return std::clamp(envFps, -1, 240);
  }
  return std::clamp(MCLA_CVAR_GET_INT(mcla_fps), -1, 240);
}

bool OriginalTiming() {
  return EffectiveFpsTarget() < 0;
}

bool PhysicsContinuousEnabled() {
  return !OriginalTiming() && !EnvEnabled("MCLA_NO_PHYSICS_DT") &&
         MCLA_CVAR_GET_BOOL(mcla_physics_continuous);
}

bool RingWakeEnabled() {
  return !EnvEnabled("MCLA_NO_RING_WAKE");
}

bool PollWaitEnabled() {
  return RingWakeEnabled() && MCLA_CVAR_GET_BOOL(mcla_poll_wait);
}

bool PumpWakeEnabled() {
  return RingWakeEnabled() && MCLA_CVAR_GET_BOOL(mcla_pump_wake_on_demand);
}

bool CheckpointSweepEnabled() {
  return !EnvEnabled("MCLA_NO_CHECKPOINT_SWEEP") &&
         MCLA_CVAR_GET_BOOL(mcla_checkpoint_sweep);
}

bool CheckpointEffectFadeEnabled() {
  return !EnvEnabled("MCLA_NO_CHECKPOINT_FADE");
}

int32_t ScriptLatchMs() {
  if (EnvEnabled("MCLA_NO_BTN_LATCH")) {
    return 0;
  }
  return std::clamp(MCLA_CVAR_GET_INT(mcla_script_pad_latch_ms), 0, 500);
}

inline float ReadF32BE(const mcla::kernel::GuestMemoryHeap &mem, uint32_t addr,
                       bool *ok = nullptr) {
  float val = 0.0f;
  const bool good = mem.ReadF32BE(addr, &val);
  if (ok) {
    *ok = good;
  }
  return val;
}

inline bool WriteF32BE(mcla::kernel::GuestMemoryHeap &mem, uint32_t addr,
                       float val) {
  uint32_t bits = 0;
  std::memcpy(&bits, &val, sizeof(bits));
  return mem.WriteU32BE(addr, bits);
}

inline uint32_t ReadU32BEOr0(const mcla::kernel::GuestMemoryHeap &mem,
                             uint32_t addr) {
  uint32_t v = 0;
  (void)mem.ReadU32BE(addr, &v);
  return v;
}

inline uint64_t NowMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Guest constants & globals (verified against generated/ppc_xenon & mcla_pe.bin)
constexpr uint32_t kGuestTimerBase = 0x827D7500u;
constexpr uint32_t kGuestFrameDelta = 0x827D7508u; // scaled dt (+8)
constexpr uint32_t kGuestFrameRate = 0x827D750Cu;  // 1/dt (+12)
constexpr uint32_t kZeroFloatAddr = 0x82000ED4u;
constexpr uint32_t kOneFloatAddr = 0x82001D14u;
constexpr uint32_t kInvTimebaseFreqAddr = 0x82011110u; // 1 / 49,875,000
constexpr uint32_t kMsPerSecFloatAddr = 0x82002C64u;   // 1000.0f
constexpr uint32_t kBikeGyroLowRateScale = 0x82040E94u; // 2.0f
constexpr uint32_t kCurrentScriptThread = 0x828DA3BCu;
constexpr uint32_t kScriptThreadProgram = 8u;
constexpr uint32_t kScriptThreadPc = 16u;

// ---------------------------------------------------------------------------
// 1. Frame Pacing + FloatClock + sub_821BDA90 / sub_824199B0
// ---------------------------------------------------------------------------
std::atomic<bool> g_limiterRanSinceSwap{false};
std::atomic<bool> g_engineTimerStarted{false};

enum PaceSite { kPaceAtTimer, kPaceAtSwap };

template <PaceSite kSite>
void WaitForFrameDeadline() {
  static uint64_t s_nextUs = 0;
  static const std::thread::id s_owner = std::this_thread::get_id();
  if constexpr (kSite == kPaceAtTimer) {
    if (std::this_thread::get_id() != s_owner) {
      return;
    }
  }

  const int32_t fps = EffectiveFpsTarget();
  if (fps <= 0) {
    s_nextUs = 0;
    return;
  }
  const uint64_t periodUs = 1'000'000ull / static_cast<uint64_t>(fps);

  uint64_t now = NowMicros();
  if (s_nextUs == 0) {
    s_nextUs = now + periodUs;
    return;
  }
  if (now < s_nextUs) {
    constexpr uint64_t kSpinMicros = 1500;
    if (s_nextUs - now > kSpinMicros + 500) {
      std::this_thread::sleep_for(
          std::chrono::microseconds(s_nextUs - now - kSpinMicros));
    }
    while (NowMicros() < s_nextUs) {
      _mm_pause();
    }
  }
  now = NowMicros();
  s_nextUs += periodUs;
  if (s_nextUs < now) {
    s_nextUs = now + periodUs;
  }
}

void PaceUnlimitedSwap(bool timed) {
  static std::atomic<uint64_t> s_lastSwapUs{0};
  static std::atomic<bool> s_lastUntimed{false};
  constexpr uint64_t kPeriod30Us = 1'000'000ull / 30ull;
  const bool untimedLoop =
      !timed && s_lastUntimed.exchange(true, std::memory_order_relaxed);
  if (timed) {
    s_lastUntimed.store(false, std::memory_order_relaxed);
  }
  const bool engineStarted = g_engineTimerStarted.load(std::memory_order_relaxed);
  const uint64_t last = s_lastSwapUs.load(std::memory_order_relaxed);
  uint64_t now = NowMicros();
  if (untimedLoop && engineStarted && last != 0 && now - last < kPeriod30Us) {
    std::this_thread::sleep_for(
        std::chrono::microseconds(kPeriod30Us - (now - last)));
    now = NowMicros();
  }
  s_lastSwapUs.store(now, std::memory_order_relaxed);
}

struct FloatClock {
  double sum = 0.0;
  float published = 0.0f;
  bool valid = false;

  void Reset() { valid = false; }

  float Advance(float guestSum, float inc) {
    if (!std::isfinite(guestSum) || !std::isfinite(inc)) {
      valid = false;
      return guestSum;
    }
    if (valid && guestSum != published + inc) {
      valid = false;
    }
    if (valid) {
      sum += static_cast<double>(inc);
    } else {
      sum = static_cast<double>(guestSum);
      valid = true;
    }
    published = static_cast<float>(sum);
    return published;
  }
};

struct TimerClocks {
  FloatClock total; // +20 (+0x14)
  FloatClock game;  // +24 (+0x18)
};

std::mutex g_timerClocksMtx;
std::unordered_map<uint32_t, TimerClocks> g_timerClocks;

void AdvanceTimerAccum(mcla::kernel::GuestMemoryHeap &mem, uint32_t timerObj,
                       uint32_t offset, float inc, bool isGame) {
  const float prev = ReadF32BE(mem, timerObj + offset);
  const float rawNext = static_cast<float>(static_cast<double>(inc) +
                                           static_cast<double>(prev));
  if (!MCLA_CVAR_GET_BOOL(mcla_float_clock)) {
    (void)WriteF32BE(mem, timerObj + offset, rawNext);
    return;
  }
  float out = rawNext;
  {
    std::lock_guard<std::mutex> lk(g_timerClocksMtx);
    TimerClocks &tc = g_timerClocks[timerObj];
    FloatClock &fc = isGame ? tc.game : tc.total;
    out = fc.Advance(rawNext, inc);
  }
  (void)WriteF32BE(mem, timerObj + offset, out);
}

void ResetTimerClocks(uint32_t timerObj) {
  std::lock_guard<std::mutex> lk(g_timerClocksMtx);
  auto it = g_timerClocks.find(timerObj);
  if (it != g_timerClocks.end()) {
    it->second.total.Reset();
    it->second.game.Reset();
  }
}

// ---------------------------------------------------------------------------
// 2. Continuous-Time Physics & Gameplay Helpers
// ---------------------------------------------------------------------------
double PerFrameToContinuous(double k30, double dt) {
  return 1.0 - std::pow(1.0 - k30, dt * 30.0);
}

void RescaleCameraFactor(double *regVal) {
  if (!regVal || !PhysicsContinuousEnabled()) {
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const double dt = static_cast<double>(ReadF32BE(mem, kGuestFrameDelta));
  const double raw = *regVal;
  if (raw <= 0.0 || raw >= 1.0 || dt <= 0.0) {
    return;
  }
  const float fps = ReadF32BE(mem, kGuestFrameRate);
  const bool alreadyHalved = static_cast<int>(fps + 0.5f) < 60;
  *regVal = PerFrameToContinuous(alreadyHalved ? raw : 0.5 * raw, dt);
}

void RescaleBikeGyroFactor(double *regVal) {
  if (!regVal || !PhysicsContinuousEnabled()) {
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const double dt = static_cast<double>(ReadF32BE(mem, kGuestFrameDelta));
  const double raw = *regVal;
  if (raw <= 0.0 || raw >= 1.0 || dt <= 0.0) {
    return;
  }
  const float fps = ReadF32BE(mem, kGuestFrameRate);
  const bool alreadyDoubled = static_cast<int>(fps + 0.5f) < 60;
  const double k30 =
      alreadyDoubled ? raw : raw * static_cast<double>(ReadF32BE(mem, kBikeGyroLowRateScale));
  if (k30 <= 0.0 || k30 >= 1.0) {
    return;
  }
  *regVal = PerFrameToContinuous(k30, dt);
}

// ---------------------------------------------------------------------------
// 3. HUD countdownUpdate accumulator (for sub_825EE188 when lr == 0x822124E4)
// ---------------------------------------------------------------------------
struct HudDtFeed {
  std::mutex mutex;
  uint32_t context = 0;
  double total = 0.0;
  double consumed = 0.0;
  uint32_t sinceStep = 0;
  bool stepHookActive = false;
};
HudDtFeed g_hudDt;

void ApplyHudCountdownDt(uint32_t ctxPtr, double &f1Val) {
  const double value = f1Val;
  if (!(value >= 0.0 && value < 1.0)) {
    return;
  }
  std::lock_guard<std::mutex> lk(g_hudDt.mutex);
  if (g_hudDt.context != ctxPtr) {
    g_hudDt.context = ctxPtr;
    g_hudDt.total = 0.0;
    g_hudDt.consumed = 0.0;
    g_hudDt.sinceStep = 0;
  }
  // Only accumulate when movie stepping is externally paced slower than updates;
  // when UI movie stepping runs every frame, pass dt through unchanged.
  if (!g_hudDt.stepHookActive) {
    return;
  }
  g_hudDt.total += value;
  g_hudDt.sinceStep++;
  constexpr uint32_t kHudDtLiveWrites = 100;
  if (g_hudDt.sinceStep <= kHudDtLiveWrites) {
    const double maxSec = std::max(value, static_cast<double>(MCLA_CVAR_GET_FLOAT(mcla_dt_max)));
    const double fed = std::max(value, std::min(g_hudDt.total - g_hudDt.consumed, maxSec));
    f1Val = static_cast<double>(static_cast<float>(fed));
  }
}

// ---------------------------------------------------------------------------
// 4. Ring Wake-on-Demand & Pump Watchdog (poll_wait port)
// ---------------------------------------------------------------------------
HANDLE ReleaseEvent() {
  static HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  return ev;
}

HANDLE PumpWakeEvent() {
  static HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  return ev;
}

struct HrTimer {
  HANDLE handle = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  ~HrTimer() {
    if (handle) {
      CloseHandle(handle);
    }
  }
};

int64_t QpcNow() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

int64_t QpcFreq() {
  static const int64_t freq = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart > 0 ? v.QuadPart : 1LL;
  }();
  return freq;
}

struct WakeState {
  std::atomic<bool> pumpWaiting{false};
  std::atomic<bool> off{false};
  std::atomic<int64_t> pausedUntilNs{0};
  std::atomic<bool> shutdown{false};
  std::atomic<uint32_t> trips{0};
  std::atomic<uint64_t> waits{0};
  std::atomic<uint64_t> early{0};
  std::atomic<uint64_t> timedOut{0};
  std::atomic<int64_t> lastEarlyNs{0};
  uint64_t steadySeen = 0; // pump thread only
  bool announced = false;  // pump thread only
};
WakeState g_wake;

void WakePumpIfWaiting(uint32_t r31) {
  if (!g_wake.pumpWaiting.load(std::memory_order_acquire)) {
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint8_t ringByte = 0;
  if (!mem.ReadU8(r31 + 199u, &ringByte)) {
    return;
  }
  const uint32_t ring = (static_cast<uint32_t>(ringByte) >> 2) & 0x3FFFFFF8u;
  if (ring != 0u) {
    return;
  }
  HANDLE ev = PumpWakeEvent();
  if (ev) {
    SetEvent(ev);
  }
}

void TripWatchdog(const char *why, uint64_t added, int64_t nowNs) {
  if (g_wake.off.load(std::memory_order_relaxed)) {
    return;
  }
  const uint32_t trips = g_wake.trips.fetch_add(1, std::memory_order_relaxed) + 1;
  const int32_t pauseS = std::clamp(MCLA_CVAR_GET_INT(mcla_pump_wake_pause_s), 0, 3600);
  if (trips == 1 && pauseS > 0) {
    g_wake.pausedUntilNs.store(
        nowNs + static_cast<int64_t>(pauseS) * 1'000'000'000LL,
        std::memory_order_relaxed);
    MCLA_LOG_WARN("PUMP-WAKE: {} ({} underruns); pausing wake-on-demand for {}s "
                  "(early={} timeouts={})",
                  why, added, pauseS, g_wake.early.load(), g_wake.timedOut.load());
    return;
  }
  g_wake.off.store(true, std::memory_order_relaxed);
  MCLA_LOG_WARN("PUMP-WAKE: {} ({} underruns); disabling wake-on-demand for session "
                "(early={} timeouts={} trips={})",
                why, added, g_wake.early.load(), g_wake.timedOut.load(), trips);
}

void CheckAudioWatchdog(int64_t nowNs) {
  const uint64_t steady = mcla::apu::SteadyUnderruns();
  if (g_wake.shutdown.load(std::memory_order_acquire)) {
    g_wake.steadySeen = steady;
    return;
  }
  if (steady != g_wake.steadySeen) {
    const uint64_t added = steady - g_wake.steadySeen;
    g_wake.steadySeen = steady;
    const int64_t lastEarly = g_wake.lastEarlyNs.load(std::memory_order_relaxed);
    if (lastEarly != 0 && (nowNs - lastEarly) < 2'000'000'000LL) {
      TripWatchdog("audio underrun during early pump wake", added, nowNs);
    }
  }
}

bool PumpWait(uint32_t ms) {
  if (ms == 0 || g_wake.off.load(std::memory_order_relaxed) ||
      g_wake.shutdown.load(std::memory_order_acquire)) {
    return false;
  }
  HANDLE ev = PumpWakeEvent();
  if (!ev) {
    return false;
  }
  const int64_t nowNs = SteadyNowNs();
  if (!g_wake.announced) {
    g_wake.announced = true;
    g_wake.steadySeen = mcla::apu::SteadyUnderruns();
    MCLA_LOG_INFO("PUMP-WAKE: active (pause_on_underrun={}s)",
                  MCLA_CVAR_GET_INT(mcla_pump_wake_pause_s));
  }
  const int64_t pausedUntil = g_wake.pausedUntilNs.load(std::memory_order_relaxed);
  if (pausedUntil != 0) {
    if (nowNs < pausedUntil) {
      g_wake.steadySeen = mcla::apu::SteadyUnderruns();
      return false;
    }
    g_wake.pausedUntilNs.store(0, std::memory_order_relaxed);
    g_wake.lastEarlyNs.store(0, std::memory_order_relaxed);
    g_wake.steadySeen = mcla::apu::SteadyUnderruns();
    MCLA_LOG_INFO("PUMP-WAKE: resumed after pause (early={} timeouts={})",
                  g_wake.early.load(), g_wake.timedOut.load());
  }
  CheckAudioWatchdog(nowNs);
  if (g_wake.off.load(std::memory_order_relaxed) ||
      g_wake.pausedUntilNs.load(std::memory_order_relaxed) != 0) {
    return false;
  }
  ResetEvent(ev);
  g_wake.pumpWaiting.store(true, std::memory_order_seq_cst);
  const DWORD waitRes = WaitForSingleObject(ev, ms);
  g_wake.pumpWaiting.store(false, std::memory_order_seq_cst);
  g_wake.waits.fetch_add(1, std::memory_order_relaxed);
  if (waitRes == WAIT_OBJECT_0) {
    g_wake.early.fetch_add(1, std::memory_order_relaxed);
    const int64_t stamp = SteadyNowNs();
    g_wake.lastEarlyNs.store(stamp, std::memory_order_relaxed);
  } else {
    g_wake.timedOut.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

// ---------------------------------------------------------------------------
// 5. Checkpoint & Line-Trigger Segment Sweep (checkpoint_sweep port)
// ---------------------------------------------------------------------------
struct V3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

inline float DotV3(const V3 &a, const V3 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline V3 SubV3(const V3 &a, const V3 &b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

constexpr int64_t kSweepWindowMs = 100;
constexpr float kSweepMaxGapM = 15.0f;
constexpr float kSweepLateralM = 1.0f;
constexpr float kSweepStillM = 0.01f;

struct SweepLook {
  uint32_t thread = 0;
  uint32_t pc = 0;
  uint32_t subject = 0;
  uint32_t racer = 0;
  V3 end{};
  int64_t atMs = 0;
};

class SweepLookTable {
public:
  static constexpr int kBuckets = 512;
  static constexpr int kWays = 24;

  bool Update(uint32_t thread, uint32_t pc, uint32_t subject, uint32_t racer,
              const V3 &start, const V3 &end, int64_t nowMs, V3 &previous,
              int64_t &gapMs) {
    SweepLook *bucket = m_looks[Bucket(thread, pc, subject, racer)];
    int best = -1;
    float bestD2 = 0.0f;
    int slot = -1;
    const V3 dir = SubV3(end, start);
    const float len2 = DotV3(dir, dir);

    for (int i = 0; i < kWays; ++i) {
      SweepLook &l = bucket[i];
      const bool live = l.atMs != 0 && (nowMs - l.atMs) <= kSweepWindowMs;
      if (!live) {
        if (slot < 0 ||
            (bucket[slot].atMs != 0 &&
             (l.atMs == 0 || l.atMs < bucket[slot].atMs))) {
          slot = i;
        }
        continue;
      }
      if (l.thread != thread || l.pc != pc || l.subject != subject ||
          l.racer != racer) {
        continue;
      }
      if (l.atMs == nowMs) {
        continue;
      }
      const V3 d = SubV3(l.end, start);
      const float d2 = DotV3(d, d);
      if (!(d2 <= kSweepMaxGapM * kSweepMaxGapM)) {
        continue;
      }
      float off2 = d2;
      if (len2 > kSweepStillM * kSweepStillM) {
        const float along = DotV3(d, dir) / len2;
        const V3 lateral{d.x - dir.x * along, d.y - dir.y * along,
                         d.z - dir.z * along};
        off2 = DotV3(lateral, lateral);
      }
      if (!(off2 <= kSweepLateralM * kSweepLateralM)) {
        continue;
      }
      if (best < 0 || d2 < bestD2) {
        best = i;
        bestD2 = d2;
      }
    }

    gapMs = -1;
    if (best >= 0) {
      SweepLook &l = bucket[best];
      previous = l.end;
      gapMs = nowMs - l.atMs;
      l.end = end;
      l.atMs = nowMs;
      return DotV3(SubV3(previous, start), SubV3(end, start)) < 0.0f;
    }

    if (slot < 0) {
      slot = 0;
      for (int i = 1; i < kWays; ++i) {
        if (bucket[i].atMs < bucket[slot].atMs) {
          slot = i;
        }
      }
    }
    SweepLook &l = bucket[slot];
    l.thread = thread;
    l.pc = pc;
    l.subject = subject;
    l.racer = racer;
    l.end = end;
    l.atMs = nowMs;
    return false;
  }

private:
  static uint32_t Bucket(uint32_t thread, uint32_t pc, uint32_t subject,
                         uint32_t racer) {
    uint32_t h = thread * 0x9E3779B1u;
    h = (h ^ (h >> 15)) + pc * 0x85EBCA6Bu;
    h = (h ^ (h >> 13)) + subject * 0xC2B2AE35u;
    h = (h ^ (h >> 16)) + racer * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h % kBuckets;
  }

  SweepLook m_looks[kBuckets][kWays]{};
};

std::mutex g_sweepMtx;
SweepLookTable g_sweepLooks;
std::atomic<uint32_t> g_sweepTold[2]{};

int64_t NowSweepMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool PreviousSweepEnd(uint32_t thread, uint32_t pc, uint32_t subject,
                      uint32_t racer, const V3 &start, const V3 &end,
                      V3 &previous, int64_t &gapMs) {
  const int64_t nowMs = NowSweepMs();
  std::lock_guard<std::mutex> lk(g_sweepMtx);
  return g_sweepLooks.Update(thread, pc, subject, racer, start, end, nowMs,
                             previous, gapMs);
}

bool ReadGuestV3(const mcla::kernel::GuestMemoryHeap &mem, uint32_t addr,
                 V3 *out) {
  return mem.ReadF32BE(addr + 0u, &out->x) &&
         mem.ReadF32BE(addr + 4u, &out->y) &&
         mem.ReadF32BE(addr + 8u, &out->z);
}

bool WriteGuestV3(mcla::kernel::GuestMemoryHeap &mem, uint32_t addr,
                  const V3 &v) {
  return WriteF32BE(mem, addr + 0u, v.x) &&
         WriteF32BE(mem, addr + 4u, v.y) &&
         WriteF32BE(mem, addr + 8u, v.z);
}

// ---------------------------------------------------------------------------
// 6. Pad Probe & Script Button Latch (pad_probe port)
// ---------------------------------------------------------------------------
constexpr uint32_t kPadBase = 0x82905388u;
constexpr uint32_t kPadStride = 64u;
constexpr uint32_t kPadCount = 4u;
constexpr uint32_t kPadButtons = 16u;
constexpr uint64_t kSrcPadTag = 1ull << 56;
constexpr uint64_t kPollerWindowUs = 200'000ull;
constexpr size_t kMaxPolls = 6000;

inline uint64_t SrcPad(uint32_t pad, uint32_t button) {
  return kSrcPadTag | (static_cast<uint64_t>(pad) << 16) | button;
}

struct PadEdgeInfo {
  uint64_t seq = 0;
  uint64_t us = 0;
  bool hasEdge = false;
};

struct PollKey {
  uint32_t thread = 0;
  uint32_t program = 0;
  uint32_t pc = 0;
  uint64_t src = 0;
  bool operator==(const PollKey &o) const {
    return thread == o.thread && program == o.program && pc == o.pc && src == o.src;
  }
};

struct PollKeyHash {
  size_t operator()(const PollKey &k) const {
    uint64_t h = k.src + 0x7F4A7C15ull;
    h = (h ^ k.thread) * 0x9E3779B97F4A7C15ull;
    h = (h ^ k.program) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ k.pc) * 0x94D049BB133111EBull;
    return static_cast<size_t>(h ^ (h >> 32));
  }
};

struct PollState {
  uint64_t lastPollUs = 0;
  uint64_t seenSeq = 0;
  bool hasSeenSeq = false;
};

std::mutex g_latchMtx;
std::unordered_map<uint64_t, PadEdgeInfo> g_padEdges;
std::unordered_map<PollKey, PollState, PollKeyHash> g_padPolls;

struct ScriptSite {
  uint32_t thread = 0;
  uint32_t program = 0;
  uint32_t pc = 0;
};

ScriptSite CurrentScriptSite(const mcla::kernel::GuestMemoryHeap &mem) {
  ScriptSite s;
  s.thread = ReadU32BEOr0(mem, kCurrentScriptThread);
  if (s.thread != 0) {
    s.program = ReadU32BEOr0(mem, s.thread + kScriptThreadProgram);
    s.pc = ReadU32BEOr0(mem, s.thread + kScriptThreadPc);
  }
  return s;
}

void PrunePollsLocked(uint64_t nowUs) {
  for (auto it = g_padPolls.begin(); it != g_padPolls.end();) {
    if (nowUs - it->second.lastPollUs > 5'000'000ull) {
      it = g_padPolls.erase(it);
    } else {
      ++it;
    }
  }
}

bool PadNativePoll(uint32_t pad, uint32_t button, bool real) {
  const int32_t latchMs = ScriptLatchMs();
  if (latchMs <= 0 || pad >= kPadCount || button >= kPadButtons) {
    return false;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const ScriptSite s = CurrentScriptSite(mem);
  const uint64_t src = SrcPad(pad, button);
  const uint64_t nowUs = NowMicros();

  std::lock_guard<std::mutex> lk(g_latchMtx);
  if (g_padPolls.size() > kMaxPolls) {
    PrunePollsLocked(nowUs);
  }
  const PadEdgeInfo &e = g_padEdges[src];
  PollState &st = g_padPolls[PollKey{s.thread, s.program, s.pc, src}];
  bool latched = false;

  if (real) {
    // Guest read the real pad state itself and saw the button down: no latch
    // needed. Remember this edge so a later "up" poll cannot replay it.
    st.seenSeq = e.seq;
    st.hasSeenSeq = e.hasEdge;
  } else if (e.hasEdge && (!st.hasSeenSeq || e.seq != st.seenSeq)) {
    // CONSUME-ONCE per (script thread, program, PC, pad, button, edge seq).
    // The old code re-delivered the SAME edge on every later poll while
    // its update-counter gate still matched the current pad update. The pause menu polls a
    // button several times per frame, so one physical press was reported as
    // two or more (the owner's "counted as two presses" symptom).
    //
    // Only a site that was already polling when the edge arrived may catch it.
    // A newly active site (for example the pause menu opened by this press)
    // must not replay that opening press. ScriptPollBlocked() clears this arm
    // by setting lastPollUs to zero, so the first poll after a modal block
    // consumes any stale edge without delivering it.
    const bool armed = st.lastPollUs != 0 && st.lastPollUs <= e.us &&
                       (e.us - st.lastPollUs) <= kPollerWindowUs;
    const uint64_t ageUs = nowUs >= e.us ? (nowUs - e.us) : 0ull;
    latched = armed && ageUs <= static_cast<uint64_t>(latchMs) * 1000ull;
    st.seenSeq = e.seq;  // consume this edge regardless of eligibility
    st.hasSeenSeq = true;
  }
  st.lastPollUs = nowUs;
  return latched;
}

void ScriptPollBlocked() {
  if (ScriptLatchMs() <= 0) {
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const ScriptSite site = CurrentScriptSite(mem);
  std::lock_guard<std::mutex> lk(g_latchMtx);
  for (auto &kv : g_padPolls) {
    const PollKey &key = kv.first;
    if (key.thread == site.thread && key.program == site.program && key.pc == site.pc) {
      kv.second.lastPollUs = 0;
    }
  }
}

void UpdatePadProbeLatch(uint32_t padRec) {
  if (ScriptLatchMs() <= 0 || padRec < kPadBase) {
    return;
  }
  const uint32_t off = padRec - kPadBase;
  if ((off % kPadStride) != 0u) {
    return;
  }
  const uint32_t pad = off / kPadStride;
  if (pad >= kPadCount) {
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint8_t connected = 0;
  if (!mem.ReadU8(padRec + 44u, &connected) || connected == 0) {
    return;
  }
  uint32_t cur = 0;
  uint32_t last = 0;
  if (!mem.ReadU32BE(padRec + 8u, &cur) ||
      !mem.ReadU32BE(padRec + 12u, &last)) {
    return;
  }
  const uint32_t pressed = cur & ~last;
  const uint64_t nowUs = NowMicros();
  if (pressed != 0u) {
    std::lock_guard<std::mutex> lk(g_latchMtx);
    for (uint32_t b = 0; b < kPadButtons; ++b) {
      if ((pressed & (1u << b)) != 0u) {
        PadEdgeInfo &e = g_padEdges[SrcPad(pad, b)];
        ++e.seq;
        e.us = nowUs;
        e.hasEdge = true;
      }
    }
  }
}

} // namespace

// ===========================================================================
// C-linkage mid-asm hooks (called from build/gen_overlay/ & gpu_device/app)
// ===========================================================================

extern "C" bool mcla_poll_wait(uint32_t r31, uint32_t r30, uint32_t r29,
                               uint32_t r26, uint32_t r3) {
  if (!RingWakeEnabled() || g_wake.shutdown.load(std::memory_order_acquire)) {
    return false;
  }
  if (PumpWakeEnabled()) {
    WakePumpIfWaiting(r31);
  }
  if (!PollWaitEnabled()) {
    return false;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  static thread_local HrTimer s_timer;
  if (!s_timer.handle) {
    return false;
  }

  uint8_t ringByte = 0;
  if (!mem.ReadU8(r31 + 199u, &ringByte)) {
    return false;
  }
  const uint32_t ring = (static_cast<uint32_t>(ringByte) >> 2) & 0x3FFFFFF8u;
  const uint32_t ringWordVa = r30 + 4u + ring;
  if (!mem.IsValid(ringWordVa, 4u)) {
    return false;
  }

  const int64_t freq = QpcFreq();
  const int64_t entered = QpcNow();
  const int32_t leftUs = static_cast<int32_t>(r26 - r3);
  if (leftUs <= 0) {
    return true;
  }
  // Bound wait to 10 ms to guard against unexpected clock skew.
  const int64_t clampedUs = std::min<int64_t>(leftUs, 10'000);
  const int64_t deadline = entered + freq * clampedUs / 1'000'000;

  const int32_t spinUs = std::clamp(MCLA_CVAR_GET_INT(mcla_poll_spin_us), 0, 2000);
  const int64_t spinEnd = entered + freq * spinUs / 1'000'000;

  HANDLE release = ReleaseEvent();
  if (!release) {
    return false;
  }
  ResetEvent(release);

  auto wordChanged = [&]() -> bool {
    uint32_t cur = 0;
    return mem.ReadU32BE(ringWordVa, &cur) && cur != r29;
  };

  for (;;) {
    for (int i = 0; i < 16; ++i) {
      if (wordChanged()) {
        return true;
      }
      YieldProcessor();
    }
    const int64_t now = QpcNow();
    if (now >= spinEnd || now >= deadline) {
      break;
    }
  }

  const int32_t tailUs = std::clamp(MCLA_CVAR_GET_INT(mcla_poll_tail_us), 0, 1000);
  const int64_t tailTicks = freq * tailUs / 1'000'000;
  const int64_t now = QpcNow();
  const int64_t sleepTicks = deadline - tailTicks - now;
  if (sleepTicks > freq / 5000) {
    LARGE_INTEGER due;
    due.QuadPart = -(sleepTicks * 10'000'000LL / freq);
    if (SetWaitableTimerEx(s_timer.handle, &due, 0, nullptr, nullptr, nullptr, 0)) {
      const HANDLE handles[2] = {release, s_timer.handle};
      const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
      if (woke == WAIT_OBJECT_0) {
        CancelWaitableTimer(s_timer.handle);
        return true;
      }
    }
  }

  for (;;) {
    for (int i = 0; i < 16; ++i) {
      if (wordChanged() || g_wake.shutdown.load(std::memory_order_acquire)) {
        return true;
      }
      YieldProcessor();
    }
    if (QpcNow() >= deadline) {
      break;
    }
  }
  return true;
}

extern "C" void mcla_poll_release(uint32_t r6) {
  (void)r6;
  if (!PollWaitEnabled()) {
    return;
  }
  HANDLE ev = ReleaseEvent();
  if (ev) {
    SetEvent(ev);
  }
}

extern "C" bool mcla_poll_tick(uint32_t *ms_inout) {
  if (!ms_inout || !RingWakeEnabled()) {
    return false;
  }
  const uint32_t asked = *ms_inout;
  int32_t cap = MCLA_CVAR_GET_INT(mcla_pump_sleep_max_ms);
  int32_t envCap = 0;
  if (TryEnvInt("MCLA_PUMP_SLEEP_MAX_MS", &envCap)) {
    cap = envCap;
  }
  if (cap >= 0 && asked > static_cast<uint32_t>(cap)) {
    *ms_inout = static_cast<uint32_t>(cap);
  }
  return cap < 0 && PumpWakeEnabled() && PumpWait(asked);
}

extern "C" void MclaPumpWakeShutdownBegins() {
  g_wake.shutdown.store(true, std::memory_order_release);
  if (HANDLE pumpEv = PumpWakeEvent()) {
    SetEvent(pumpEv);
  }
  if (HANDLE relEv = ReleaseEvent()) {
    SetEvent(relEv);
  }
}

extern "C" void mcla_cam_boom_scale_outer(double *f13) {
  RescaleCameraFactor(f13);
}

extern "C" void mcla_cam_boom_scale_inner(double *f0) {
  RescaleCameraFactor(f0);
}

extern "C" void mcla_chassis_filter_scale(double *f0) {
  if (!f0 || !PhysicsContinuousEnabled()) {
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const double dt = static_cast<double>(ReadF32BE(mem, kGuestFrameDelta));
  if (dt > 0.0) {
    *f0 = PerFrameToContinuous(0.10, dt);
  }
}

extern "C" void mcla_bike_gyro_scale_lean(double *f13) {
  RescaleBikeGyroFactor(f13);
}

extern "C" void mcla_bike_gyro_scale_damp(double *f0) {
  RescaleBikeGyroFactor(f0);
}

extern "C" void mcla_trigger_line_extend_stack(uint32_t sp, uint32_t trigger,
                                              uint32_t racer) {
  if (!CheckpointSweepEnabled()) {
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const uint32_t thread = ReadU32BEOr0(mem, kCurrentScriptThread);
  if (thread == 0) {
    return;
  }
  const uint32_t pc = ReadU32BEOr0(mem, thread + kScriptThreadPc);
  const uint32_t startPtr = sp + 128u;
  const uint32_t endPtr = sp + 144u;
  V3 start{};
  V3 end{};
  if (!ReadGuestV3(mem, startPtr, &start) || !ReadGuestV3(mem, endPtr, &end)) {
    return;
  }
  V3 previous{};
  int64_t gapMs = -1;
  if (!PreviousSweepEnd(thread, pc, trigger, racer, start, end, previous, gapMs)) {
    return;
  }
  (void)WriteGuestV3(mem, startPtr, previous);
  const uint32_t told = g_sweepTold[1].fetch_add(1, std::memory_order_relaxed) + 1;
  if (told <= 4u) {
    const V3 diff = SubV3(previous, start);
    MCLA_LOG_INFO("SWEEP-TRIGGER #{}: trigger={:08X} gap={}ms ext={:.1f}m",
                  told, trigger, gapMs, std::sqrt(DotV3(diff, diff)));
  }
}

// The guest's checkpoint close path writes [effect+464] = dt and
// [effect+468] = dt*25 immediately before this update. The update multiplies
// [effect+432] by [effect+468]. At the console's 30 Hz that is (25/30) per
// update; using the same per-frame factor at higher FPS squares the decay rate.
// Keep the console's exponential curve instead: (25/30)^(dt*30). The three
// call sites and both stored values are checked before anything is changed.
PPC_FUNC(sub_8247A8D0) {
  const uint32_t site = ctx.lr;
  bool patched = false;
  uint32_t effect = 0;
  float dt = 0.0f;
  float guestFade = 0.0f;
  float fixedFade = 0.0f;

  if (CheckpointEffectFadeEnabled() &&
      (site == 0x82264EF0u || site == 0x82266868u || site == 0x82266D78u)) {
    effect = ctx.r3.u32;
    if (effect >= 0x10000u) {
      auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
      if (mem.IsValid(effect, 492u)) {
        dt = ReadF32BE(mem, kGuestFrameDelta);
        const float step = ReadF32BE(mem, effect + 464u);
        guestFade = ReadF32BE(mem, effect + 468u);
        if (dt > 0.0f && std::isfinite(dt) && step == dt &&
            guestFade == dt * 25.0f) {
          fixedFade = static_cast<float>(
              std::pow(5.0 / 6.0, static_cast<double>(dt) * 30.0));
          patched = std::isfinite(fixedFade) &&
                    WriteF32BE(mem, effect + 468u, fixedFade);
        }
      }
    }
  }

  __imp__sub_8247A8D0(ctx, base);

  if (patched) {
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    (void)WriteF32BE(mem, effect + 468u, guestFade);
    static std::atomic<uint32_t> told{0};
    const uint32_t n = told.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4u) {
      MCLA_LOG_INFO("CKPT-FADE #{}: site={:08X} dt={:.6f} guest={:.6f} fixed={:.6f}",
                    n, site, dt, guestFade, fixedFade);
    }
  }
}

// ===========================================================================
// C++-linkage PPC_FUNC overrides (declared in src/hooks/hooks.h)
// ===========================================================================

PPC_FUNC(sub_821BDA90) {
  if (OriginalTiming()) {
    __imp__sub_821BDA90(ctx, base);
    return;
  }

  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const uint32_t timerObj = ctx.r3.u32;
  if (timerObj == 0u || !mem.IsValid(timerObj, 108u)) {
    __imp__sub_821BDA90(ctx, base);
    return;
  }

  g_engineTimerStarted.store(true, std::memory_order_relaxed);
  // FPS-CAP-RATE (only with MCLA_FPS_CAP set): engine-timer frames/s and the guest
  // total clock (+0x14) against wall time; ratio 1.00 = game runs real-time.
  if (std::getenv("MCLA_FPS_CAP") != nullptr) {
    static uint64_t s_t0 = 0, s_n = 0;
    static float s_g0 = 0.0f;
    const float g = ReadF32BE(mem, timerObj + 20u);
    const uint64_t now = NowMicros();
    if (s_t0 == 0) {
      s_t0 = now;
      s_g0 = g;
    } else if (++s_n % 600u == 0u && now > s_t0) {
      const double wall = static_cast<double>(now - s_t0) / 1e6;
      MCLA_LOG_INFO("FPS-CAP-RATE frames={} wall={:.2f}s fps={:.1f} guestClock={:.2f}s ratio={:.3f}",
                    s_n, wall, s_n / wall, g - s_g0, (g - s_g0) / wall);
    }
  }
  if (!MCLA_CVAR_GET_BOOL(mcla_pace_at_swap)) {
    WaitForFrameDeadline<kPaceAtTimer>();
  }
  g_limiterRanSinceSwap.store(true, std::memory_order_relaxed);

  // loc_821BDA90: read 49.875 MHz timebase; re-read if low 32 bits == 0.
  uint64_t tbNow = 0;
  do {
    tbNow = mcla_guest_timebase_49875();
  } while ((static_cast<uint32_t>(tbNow)) == 0u);

  uint64_t prevTb = 0;
  (void)mem.ReadU64BE(timerObj + 64u, &prevTb);
  (void)mem.WriteU64BE(timerObj + 64u, tbNow);

  int64_t rawTicks = static_cast<int64_t>(tbNow - prevTb);
  const double maxSec = std::clamp(
      static_cast<double>(MCLA_CVAR_GET_FLOAT(mcla_dt_max)), 0.001, 1.0);
  const int64_t maxTicks = static_cast<int64_t>(maxSec * 49875000.0);
  if (rawTicks < 0) {
    rawTicks = 0;
  } else if (rawTicks > maxTicks) {
    rawTicks = maxTicks;
  }

  const float scale = ReadF32BE(mem, timerObj + 84u);
  const float invFreq = ReadF32BE(mem, kInvTimebaseFreqAddr);
  const float oneF = ReadF32BE(mem, kOneFloatAddr);
  const float zeroF = ReadF32BE(mem, kZeroFloatAddr);

  const float f8 = static_cast<float>(
      static_cast<double>(static_cast<float>(static_cast<double>(rawTicks))) *
      static_cast<double>(invFreq));
  (void)WriteF32BE(mem, timerObj + 92u, f8);

  const double f7 = (ctx.f1.f64 >= 0.0) ? ctx.f1.f64 : static_cast<double>(f8);
  (void)WriteF32BE(mem, timerObj + 100u, static_cast<float>(f7));

  const float f6 =
      static_cast<float>(static_cast<double>(scale) * static_cast<double>(f8));
  (void)WriteF32BE(mem, timerObj + 96u, f6);

  (void)WriteF32BE(mem, timerObj + 88u, static_cast<float>(f7));
  const float f0Dt = static_cast<float>(f7 * static_cast<double>(scale));
  (void)WriteF32BE(mem, timerObj + 104u, f0Dt);
  (void)WriteF32BE(mem, timerObj + 8u, f0Dt);

  uint8_t resetFlag = 0;
  uint8_t unscaledFlag = 0;
  (void)mem.ReadU8(timerObj + 56u, &resetFlag);
  (void)mem.ReadU8(timerObj + 72u, &unscaledFlag);
  const bool updateGameTime = (ctx.r5.u32 & 0xFFu) != 0u;

  if (resetFlag != 0u) {
    // loc_821BDBC8: reset guard
    uint32_t f60 = 0;
    (void)mem.ReadU32BE(timerObj + 60u, &f60);
    float resetDt = ReadF32BE(mem, timerObj + 32u);
    if (static_cast<int32_t>(f60) == 0 && resetDt == zeroF) {
      const float denom = ReadF32BE(mem, timerObj + 28u);
      resetDt = static_cast<float>(static_cast<double>(oneF) /
                                   static_cast<double>(denom));
    }
    (void)WriteF32BE(mem, timerObj + 8u, resetDt);
    ResetTimerClocks(timerObj);
    if (unscaledFlag == 0u) {
      AdvanceTimerAccum(mem, timerObj, 20u, resetDt, false);
      if (updateGameTime) {
        AdvanceTimerAccum(mem, timerObj, 24u, resetDt, true);
      }
    }
    (void)WriteF32BE(mem, timerObj + 88u, resetDt);
    (void)mem.WriteU8(timerObj + 56u, 0u);
  } else {
    const float prev20 = ReadF32BE(mem, timerObj + 20u);
    (void)WriteF32BE(mem, timerObj + 16u, prev20);

    uint8_t f58 = 0;
    uint32_t f60 = 0;
    (void)mem.ReadU8(timerObj + 58u, &f58);
    (void)mem.ReadU32BE(timerObj + 60u, &f60);

    if (f58 != 0u || static_cast<int32_t>(f60) != 0) {
      // loc_821BDB90: use real measured unscaled dt at +88 instead of fixed +32.
      const float stepUnscaled = ReadF32BE(mem, timerObj + 88u);
      const float scaledStep = static_cast<float>(
          static_cast<double>(stepUnscaled) * static_cast<double>(scale));
      (void)WriteF32BE(mem, timerObj + 88u, stepUnscaled);
      (void)WriteF32BE(mem, timerObj + 8u, scaledStep);
      AdvanceTimerAccum(mem, timerObj, 20u, scaledStep, false);
      if (updateGameTime) {
        AdvanceTimerAccum(mem, timerObj, 24u, scaledStep, true);
      }
    } else {
      if (unscaledFlag == 0u) {
        AdvanceTimerAccum(mem, timerObj, 20u, f0Dt, false);
        if (updateGameTime) {
          AdvanceTimerAccum(mem, timerObj, 24u, f0Dt, true);
        }
      }
      // Bypass loc_821BDB58 (30 Hz floor snap & [r3+52]=1 flag).
    }
  }

  // loc_821BDC34: frame counters + [minDt, maxDt] clamp (+40 min, +36 max)
  uint32_t c44 = 0;
  (void)mem.ReadU32BE(timerObj + 44u, &c44);
  (void)mem.WriteU32BE(timerObj + 44u, c44 + 1u);
  if (updateGameTime) {
    uint32_t c48 = 0;
    (void)mem.ReadU32BE(timerObj + 48u, &c48);
    (void)mem.WriteU32BE(timerObj + 48u, c48 + 1u);
  }

  const double minDt = static_cast<double>(ReadF32BE(mem, timerObj + 40u));
  const double maxDt = static_cast<double>(ReadF32BE(mem, timerObj + 36u));
  const double v8 = static_cast<double>(ReadF32BE(mem, timerObj + 8u));
  const double v88 = static_cast<double>(ReadF32BE(mem, timerObj + 88u));

  const double lo8 = (v8 - minDt >= 0.0) ? v8 : minDt;
  const double lo88 = (v88 - minDt >= 0.0) ? v88 : minDt;
  const float clamped8 = static_cast<float>((lo8 - maxDt >= 0.0) ? maxDt : lo8);
  const float clamped88 = static_cast<float>((lo88 - maxDt >= 0.0) ? maxDt : lo88);

  (void)WriteF32BE(mem, timerObj + 8u, clamped8);
  (void)WriteF32BE(mem, timerObj + 88u, clamped88);

  if (unscaledFlag != 0u) {
    AdvanceTimerAccum(mem, timerObj, 20u, clamped8, false);
    if (updateGameTime) {
      AdvanceTimerAccum(mem, timerObj, 24u, clamped8, true);
    }
  }

  const float rate = static_cast<float>(static_cast<double>(oneF) /
                                        static_cast<double>(clamped8));
  (void)WriteF32BE(mem, timerObj + 12u, rate);
}

PPC_FUNC(sub_824199B0) {
  if (OriginalTiming()) {
    __imp__sub_824199B0(ctx, base);
    return;
  }

  const bool timed =
      g_limiterRanSinceSwap.exchange(false, std::memory_order_relaxed);
  if (timed && MCLA_CVAR_GET_BOOL(mcla_pace_at_swap)) {
    WaitForFrameDeadline<kPaceAtSwap>();
  }
  PaceUnlimitedSwap(timed);

  const uint32_t dev = ctx.r3.u32;
  uint32_t savedInterval = 0;
  bool swappedInterval = false;
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  if (MCLA_CVAR_GET_BOOL(mcla_swap_unlock) && dev != 0u &&
      mem.ReadU32BE(dev + 13596u, &savedInterval) && savedInterval == 2u) {
    swappedInterval = mem.WriteU32BE(dev + 13596u, 1u);
  }

  __imp__sub_824199B0(ctx, base);

  if (swappedInterval) {
    (void)mem.WriteU32BE(dev + 13596u, savedInterval);
  }
}

PPC_FUNC(sub_825EE188) {
  if (ctx.lr == 0x822124E4u && PhysicsContinuousEnabled()) {
    ApplyHudCountdownDt(ctx.r3.u32, ctx.f1.f64);
  }
  __imp__sub_825EE188(ctx, base);
}

PPC_FUNC(sub_822653A8) {
  const uint32_t checkpoint = ctx.r3.u32;
  const uint32_t endPtr = ctx.r4.u32;
  const uint32_t startPtr = ctx.r5.u32;
  const uint64_t savedLr = ctx.lr;

  __imp__sub_822653A8(ctx, base);

  if (!CheckpointSweepEnabled() || endPtr < 0x10000u || startPtr < 0x10000u) {
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const uint32_t thread = ReadU32BEOr0(mem, kCurrentScriptThread);
  if (thread == 0u) {
    return;
  }
  const uint32_t pc = ReadU32BEOr0(mem, thread + kScriptThreadPc);
  V3 start{};
  V3 end{};
  if (!ReadGuestV3(mem, startPtr, &start) || !ReadGuestV3(mem, endPtr, &end)) {
    return;
  }
  V3 previous{};
  int64_t gapMs = -1;
  const bool behind =
      PreviousSweepEnd(thread, pc, checkpoint, 0u, start, end, previous, gapMs);
  if (!behind || (ctx.r3.u32 & 0xFFu) != 0u) {
    return;
  }

  const uint32_t sp = ctx.r1.u32;
  const uint32_t scratch = (sp - 0x80u) & ~15u;
  uint8_t vecBuf[16] = {};
  if (!mem.ReadBytes(startPtr, vecBuf, 16u) ||
      !mem.WriteBytes(scratch, vecBuf, 16u) ||
      !WriteGuestV3(mem, scratch, previous)) {
    return;
  }

  ctx.r1.u32 = sp - 0x100u;
  ctx.lr = savedLr;
  ctx.r3.u64 = checkpoint;
  ctx.r4.u64 = endPtr;
  ctx.r5.u64 = scratch;
  __imp__sub_822653A8(ctx, base);
  ctx.r1.u32 = sp;
  ctx.lr = savedLr;

  if ((ctx.r3.u32 & 0xFFu) != 0u) {
    ctx.r3.u64 = 1;
    const uint32_t told = g_sweepTold[0].fetch_add(1, std::memory_order_relaxed) + 1;
    if (told <= 4u) {
      const V3 diff = SubV3(previous, start);
      MCLA_LOG_INFO("SWEEP-CKPT #{}: cp={:08X} gap={}ms ext={:.1f}m",
                    told, checkpoint, gapMs, std::sqrt(DotV3(diff, diff)));
    }
  } else {
    ctx.r3.u64 = 0;
  }
}

PPC_FUNC(sub_82256220) {
  if (!PhysicsContinuousEnabled()) {
    __imp__sub_82256220(ctx, base);
    return;
  }

  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const uint32_t obj = ctx.r3.u32;
  const uint64_t savedLr = ctx.lr;

  uint8_t active = 0;
  if (obj == 0u || !mem.ReadU8(obj + 3764u, &active)) {
    __imp__sub_82256220(ctx, base);
    return;
  }
  if (active == 0u) {
    return;
  }

  // Check pause/UI state [0x828CCEA0]
  uint32_t mgr = 0;
  if (mem.ReadU32BE(0x828CCEA0u, &mgr) && mgr != 0u) {
    uint8_t b4 = 0;
    uint8_t b10 = 0;
    if (mem.ReadU8(mgr + 4u, &b4) && b4 != 0u &&
        mem.ReadU8(mgr + 10u, &b10) && b10 == 0u) {
      return;
    }
  }

  bool useUnscaled = false;
  if (ReadU32BEOr0(mem, 0x82874810u) != 0u) {
    useUnscaled = true;
  } else {
    sub_82387A18(ctx, base);
    if ((ctx.r3.u32 & 0xFFu) != 0u) {
      useUnscaled = true;
    } else {
      uint32_t curMs = 0;
      if (mem.ReadU32BE(obj + 3748u, &curMs) && static_cast<int32_t>(curMs) < 0) {
        ctx.r3.u64 = ReadU32BEOr0(mem, 0x8286D8F0u);
        sub_823BB408(ctx, base);
        if ((ctx.r3.u32 & 0xFFu) != 0u) {
          useUnscaled = true;
        }
      }
    }
  }
  ctx.lr = savedLr;

  const float dtSec =
      ReadF32BE(mem, useUnscaled ? (kGuestTimerBase + 100u) : (kGuestTimerBase + 8u));
  const float msPerSec = ReadF32BE(mem, kMsPerSecFloatAddr);
  const float frameMsF32 = static_cast<float>(static_cast<double>(dtSec) *
                                              static_cast<double>(msPerSec));

  static double s_carry = 0.0;
  double wholeMs = static_cast<double>(frameMsF32);
  const double ms = wholeMs + s_carry;
  if (!std::isfinite(ms) || std::fabs(ms) > 1.0e9) {
    s_carry = 0.0;
  } else {
    wholeMs = std::trunc(ms);
    s_carry = ms - wholeMs;
  }

  const int32_t deltaMs = (wholeMs > static_cast<double>(INT_MAX))
                              ? INT_MAX
                              : static_cast<int32_t>(wholeMs);
  uint32_t prevMs = 0;
  (void)mem.ReadU32BE(obj + 3748u, &prevMs);
  (void)mem.WriteU32BE(obj + 3768u, static_cast<uint32_t>(deltaMs));
  (void)mem.WriteU32BE(obj + 3748u, prevMs + static_cast<uint32_t>(deltaMs));
}

PPC_FUNC(sub_822C8AF8) {
  const uint32_t argsVa = ctx.r3.u32;
  __imp__sub_822C8AF8(ctx, base);

  if (ScriptLatchMs() <= 0 || argsVa == 0u) {
    return;
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint32_t retPtr = 0;
  uint32_t argVec = 0;
  uint32_t pad = 0;
  uint32_t btn = 0;
  if (!mem.ReadU32BE(argsVa + 0u, &retPtr) ||
      !mem.ReadU32BE(argsVa + 8u, &argVec) ||
      !mem.ReadU32BE(argVec + 0u, &pad) ||
      !mem.ReadU32BE(argVec + 4u, &btn)) {
    return;
  }
  const bool real = (ctx.r3.u32 & 0xFFu) != 0u;
  if (PadNativePoll(pad, btn, real) && !real) {
    (void)mem.WriteU32BE(retPtr, 1u);
    ctx.r3.u64 = 1;
  }
}

PPC_FUNC(sub_822C7FC8) {
  if (ScriptLatchMs() <= 0) {
    __imp__sub_822C7FC8(ctx, base);
    return;
  }

  const uint32_t player = ctx.r3.u32;
  const uint32_t button = ctx.r4.u32;
  const uint64_t savedLr = ctx.lr;

  // Check modal UI input block (sub_82207D30) before testing pad edge.
  sub_82207D30(ctx, base);
  if ((ctx.r3.u32 & 0xFFu) != 0u) {
    ScriptPollBlocked();
    ctx.r3.s64 = 0;
    ctx.lr = savedLr;
    return;
  }

  ctx.r3.u64 = player;
  ctx.r4.u64 = button;
  ctx.lr = savedLr;
  __imp__sub_822C7FC8(ctx, base);

  const bool real = (ctx.r3.u32 & 0xFFu) != 0u;
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint32_t p1 = 0;
  uint32_t p2 = 0;
  uint32_t p3 = 0;
  uint32_t padIdx = 0;
  if (mem.ReadU32BE(0x8288E5ACu, &p1) && p1 != 0u &&
      mem.ReadU32BE(p1, &p2) && p2 != 0u &&
      mem.ReadU32BE(p2 + player * 4u, &p3) && p3 != 0u &&
      mem.ReadU32BE(p3 + 4u, &padIdx)) {
    if (PadNativePoll(padIdx, button, real) && !real) {
      ctx.r3.u64 = 1;
    }
  }
  ctx.lr = savedLr;
}

PPC_FUNC(sub_82614D30) {
  const uint32_t padRec = ctx.r3.u32;
  __imp__sub_82614D30(ctx, base);
  if (padRec != 0u) {
    UpdatePadProbeLatch(padRec);
  }
}
