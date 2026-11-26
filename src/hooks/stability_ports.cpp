// Wave 1 stability & performance ports (2026-11-18).
//
// All guest offsets and ABI contracts in this TU were directly verified against
// generated/ppc_xenon/*.cpp and build/cache/mcla_pe.bin:
//   - sub_8221BE98 (ppc_recomp.25.cpp:16112) : camera-flight warp recovery
//   - sub_822BC658 (ppc_recomp.43.cpp:6921)  : racing AI brain reset null guard
//   - sub_821E1598 (ppc_recomp.19.cpp:20458) : AES-NI 16-round ECB script decrypt
//   - sub_821E4B20 (ppc_recomp.19.cpp:28944) : zlib 1.2.3 inflate_fast

#include "hooks/hooks.h"
#include "generated/ppc_xenon/ppc_recomp_shared.h"
#include "cpu/ppc_context.h"
#include "kernel/memory.h"
#include "logging.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <intrin.h>
#include <wmmintrin.h>

PPC_FUNC_IMPL(__imp__sub_8221BE98);
PPC_FUNC_IMPL(__imp__sub_822BC658);
PPC_FUNC_IMPL(__imp__sub_821E1598);
PPC_FUNC_IMPL(__imp__sub_821E4B20);

namespace {

bool EnvEnabled(const char *name) {
  const char *e = std::getenv(name);
  return e != nullptr && e[0] == '1';
}

// ===========================================================================
// 1. Camera-flight warp recovery (sub_8221BE98)
//
// Verified against generated/ppc_xenon/ppc_recomp.25.cpp:
//   sub_8221BE98 (:16112) : kCineScriptPtr = 0x8286D8F0 (:16534-16538),
//     kCineGpsInterlock = 293 (:16540), kWarperDone = 276 (:16542),
//     kWarperState = 288 (:16544), kWarperTransition = 296 (:16436),
//     kStateWaitTimed = 19, kStateWaitCinematic = 20, kStateIdle = 29.
//   sub_8221B8C8 (:15208) : kWarperEventId = 256 (:15231),
//     kWarperPending = 300 (:15249), calls sub_8221B790 (:15332).
// ===========================================================================
constexpr uint32_t kCineScriptPtr = 0x8286D8F0u;
constexpr uint32_t kCineGpsInterlock = 293u;

constexpr uint32_t kWarperEventId = 256u;
constexpr uint32_t kWarperDone = 276u;
constexpr uint32_t kWarperState = 288u;
constexpr uint32_t kWarperTransition = 296u;
constexpr uint32_t kWarperPending = 300u;
constexpr uint32_t kStateWaitTimed = 19u;
constexpr uint32_t kStateWaitCinematic = 20u;
constexpr uint32_t kStateIdle = 29u;

constexpr uint32_t kTransitionFlags = 16u;
constexpr uint32_t kTransitionName = 20u;
constexpr uint32_t kTransitionDuration = 76u;
constexpr uint32_t kTransitionTime = 96u;
constexpr uint32_t kTransitionActive = 0x80u;

constexpr float kNeverStartedGraceSec = 3.0f;

struct WarpWatch {
  uint32_t transition = 0;
  bool seenActive = false;
  float waited = 0.0f;
  float lastTime = 0.0f;
  float lastDt = 0.0f;
  uint32_t frames = 0;
};

std::mutex g_warpMtx;
WarpWatch g_warpWatch;
std::atomic<uint32_t> g_warpFixCount{0};

bool IsWarpWaitState(uint32_t state) {
  return state == kStateWaitTimed || state == kStateWaitCinematic;
}

std::string ReadTransitionName(const mcla::kernel::GuestMemoryHeap &mem,
                               uint32_t transition) {
  uint32_t nameVa = 0;
  if (!mem.ReadU32BE(transition + kTransitionName, &nameVa) || nameVa == 0) {
    return "<unnamed>";
  }
  char buf[65] = {};
  if (!mem.ReadBytes(nameVa, buf, 64)) {
    return "<unreadable>";
  }
  buf[64] = '\0';
  return std::string(buf);
}

void FinishMissedWarp(PPCContext &ctx, uint8_t *base,
                      mcla::kernel::GuestMemoryHeap &mem, uint32_t warper,
                      uint32_t transition, bool neverStarted,
                      const WarpWatch &snap) {
  float duration = 0.0f;
  (void)mem.ReadF32BE(transition + kTransitionDuration, &duration);
  uint32_t pending = 0;
  (void)mem.ReadU32BE(warper + kWarperPending, &pending);

  const uint32_t fixN = g_warpFixCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (fixN <= 64 || (fixN % 100) == 0) {
    const std::string name = ReadTransitionName(mem, transition);
    if (neverStarted) {
      MCLA_LOG_WARN("WARP-FIX #{} '{}' never started; waited {:.2f}s (duration {:.3f}s) -> finishing",
                    fixN, name, snap.waited, duration);
    } else {
      MCLA_LOG_WARN("WARP-FIX #{} '{}' ended between frames (last t={:.3f}s/{:.3f}s dt={:.4f}) -> finishing ({})",
                    fixN, name, snap.lastTime, duration, snap.lastDt,
                    pending ? "TRANSITION_COMPLETE not seen" : "after TRANSITION_COMPLETE");
    }
  }

  uint32_t cine = 0;
  if (mem.ReadU32BE(kCineScriptPtr, &cine) && cine != 0) {
    (void)mem.WriteU8(cine + kCineGpsInterlock, 0);
  }
  (void)mem.WriteU8(warper + kWarperDone, 1);
  (void)mem.WriteU32BE(warper + kWarperState, kStateIdle);

  if (pending == transition) {
    uint32_t eventId = 0;
    (void)mem.ReadU32BE(warper + kWarperEventId, &eventId);
    ctx.r3.u64 = warper;
    ctx.r4.u64 = eventId;
    ctx.r5.u64 = transition;
    sub_8221B8C8(ctx, base);
  } else {
    ctx.r3.u64 = warper;
    sub_8221B790(ctx, base);
  }
}

void WatchWarper(PPCContext &ctx, uint8_t *base, uint32_t warper, float dt) {
  static const bool s_noWarpFix = EnvEnabled("MCLA_NO_WARP_FIX");
  static const bool s_warpTrace = EnvEnabled("MCLA_WARP_TRACE");

  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint32_t state = 0;
  if (!mem.ReadU32BE(warper + kWarperState, &state) || !IsWarpWaitState(state)) {
    std::lock_guard<std::mutex> lk(g_warpMtx);
    if (g_warpWatch.transition != 0 && g_warpWatch.frames > 0 && s_warpTrace) {
      MCLA_LOG_INFO("WARP-TRACE '{}' finished normally after {} frames ({:.2f}s)",
                    ReadTransitionName(mem, g_warpWatch.transition),
                    g_warpWatch.frames, g_warpWatch.waited);
    }
    g_warpWatch = {};
    return;
  }

  uint32_t transition = 0;
  if (!mem.ReadU32BE(warper + kWarperTransition, &transition) || transition == 0) {
    return;
  }

  uint32_t flags = 0;
  float time = 0.0f;
  float duration = 0.0f;
  if (!mem.ReadU32BE(transition + kTransitionFlags, &flags) ||
      !mem.ReadF32BE(transition + kTransitionTime, &time) ||
      !mem.ReadF32BE(transition + kTransitionDuration, &duration)) {
    return;
  }

  const bool active = (flags & kTransitionActive) != 0;

  WarpWatch snap;
  bool shouldFinish = false;
  bool neverStarted = false;

  {
    std::lock_guard<std::mutex> lk(g_warpMtx);
    if (transition != g_warpWatch.transition) {
      g_warpWatch = {};
      g_warpWatch.transition = transition;
      if (s_warpTrace) {
        MCLA_LOG_INFO("WARP-TRACE waiting on '{}' ({:.3f}s)",
                      ReadTransitionName(mem, transition), duration);
      }
    }

    ++g_warpWatch.frames;
    g_warpWatch.waited += dt;
    if (s_warpTrace) {
      MCLA_LOG_INFO("WARP-TRACE frame={} dt={:.4f} active={} t={:.4f}/{:.4f}",
                    g_warpWatch.frames, dt, active ? 1 : 0, time, duration);
    }

    if (active) {
      g_warpWatch.seenActive = true;
      g_warpWatch.lastTime = time;
      g_warpWatch.lastDt = dt;
      return;
    }

    neverStarted = !g_warpWatch.seenActive &&
                   g_warpWatch.waited > (std::max)(duration, 0.0f) + kNeverStartedGraceSec;
    if (!g_warpWatch.seenActive && !neverStarted) {
      return;
    }

    if (s_noWarpFix) {
      if ((g_warpWatch.frames % 600) == 1) {
        MCLA_LOG_WARN("WARP-FIX disabled (MCLA_NO_WARP_FIX=1): stuck on '{}'",
                      ReadTransitionName(mem, transition));
      }
      return;
    }

    snap = g_warpWatch;
    g_warpWatch = {};
    shouldFinish = true;
  }

  if (shouldFinish) {
    FinishMissedWarp(ctx, base, mem, warper, transition, neverStarted, snap);
  }
}

// ===========================================================================
// 2. Native AES-NI 16-round ECB script decrypt (sub_821E1598 + sub_821E0FF0)
// ===========================================================================
bool HostHasAesNi() {
  static const bool s_has = []() {
    int info[4] = {};
    __cpuid(info, 1);
    return (info[2] & (1 << 25)) != 0;
  }();
  return s_has;
}

__attribute__((target("aes")))
void AesDecrypt16xBlocksAesNi(uint8_t *buf, uint32_t len, const uint8_t *rkBytes,
                              uint32_t nr) {
  __m128i rk[15];
  for (uint32_t r = 0; r <= nr; ++r) {
    rk[r] = _mm_loadu_si128(reinterpret_cast<const __m128i *>(rkBytes + r * 16u));
  }
  for (uint32_t off = 0; off < len; off += 16u) {
    __m128i state = _mm_loadu_si128(reinterpret_cast<const __m128i *>(buf + off));
    for (int iter = 0; iter < 16; ++iter) {
      state = _mm_xor_si128(state, rk[0]);
      for (uint32_t r = 1; r < nr; ++r) {
        state = _mm_aesdec_si128(state, rk[r]);
      }
      state = _mm_aesdeclast_si128(state, rk[nr]);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i *>(buf + off), state);
  }
}

std::mutex g_aesCheckMtx;
// 0 = untested, 1 = passed, -1 = failed
std::atomic<int> g_aesSelfCheckState{0};

// ===========================================================================
// 3. Native zlib 1.2.3 inflate_fast (sub_821E4B20)
//
// All struct offsets verified directly against generated/ppc_xenon/ppc_recomp.19.cpp:
//   z_stream (r3):
//     +0  next_in    +4  avail_in   +12 next_out   +16 avail_out
//     +24 msg        +28 state
//   inflate_state (r30):
//     +0  mode       +40 wsize      +44 whave      +48 wnext (write)
//     +52 window     +56 hold       +60 bits
//     +76 lencode    +80 distcode   +84 lenbits    +88 distbits
//   code entry (BE 4 bytes):
//     +0 op (u8), +1 bits (u8), +2..+3 val (BE u16)
//   Error strings in build/cache/mcla_pe.bin:
//     0x820168B0 = "invalid literal/length code"
//     0x820168CC = "invalid distance code"
//     0x82016938 = "invalid distance too far back"
// ===========================================================================
constexpr uint32_t kZlibMsgBadLitLen = 0x820168B0u;
constexpr uint32_t kZlibMsgBadDist   = 0x820168CCu;
constexpr uint32_t kZlibMsgTooFar    = 0x82016938u;
constexpr uint32_t kZlibModeType     = 11u;
constexpr uint32_t kZlibModeBad      = 27u;

inline void ReadCodeBe(const uint8_t *tableBase, uint32_t index, uint8_t &op,
                       uint8_t &bits, uint16_t &val) {
  const uint8_t *p = tableBase + static_cast<size_t>(index) * 4u;
  op = p[0];
  bits = p[1];
  val = static_cast<uint16_t>((static_cast<uint16_t>(p[2]) << 8) | p[3]);
}

bool RunNativeInflateFast(uint32_t strmVa, uint32_t start) {
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();

  uint32_t nextInVa = 0, availIn = 0, nextOutVa = 0, availOut = 0, stateVa = 0;
  if (!mem.ReadU32BE(strmVa + 0u, &nextInVa) ||
      !mem.ReadU32BE(strmVa + 4u, &availIn) ||
      !mem.ReadU32BE(strmVa + 12u, &nextOutVa) ||
      !mem.ReadU32BE(strmVa + 16u, &availOut) ||
      !mem.ReadU32BE(strmVa + 28u, &stateVa) ||
      stateVa == 0 || nextInVa == 0 || nextOutVa == 0) {
    return false;
  }

  // inflate_fast preconditions in inflate() (sub_821DEE40:17296-17303)
  if (availIn < 6u || availOut < 258u || start < availOut) {
    return false;
  }

  uint32_t wsize = 0, whave = 0, wnext = 0, windowVa = 0;
  uint32_t hold = 0, bits = 0;
  uint32_t lencodeVa = 0, distcodeVa = 0, lenbits = 0, distbits = 0;
  if (!mem.ReadU32BE(stateVa + 40u, &wsize) ||
      !mem.ReadU32BE(stateVa + 44u, &whave) ||
      !mem.ReadU32BE(stateVa + 48u, &wnext) ||
      !mem.ReadU32BE(stateVa + 52u, &windowVa) ||
      !mem.ReadU32BE(stateVa + 56u, &hold) ||
      !mem.ReadU32BE(stateVa + 60u, &bits) ||
      !mem.ReadU32BE(stateVa + 76u, &lencodeVa) ||
      !mem.ReadU32BE(stateVa + 80u, &distcodeVa) ||
      !mem.ReadU32BE(stateVa + 84u, &lenbits) ||
      !mem.ReadU32BE(stateVa + 88u, &distbits)) {
    return false;
  }

  if (lenbits > 15u || distbits > 15u || lencodeVa == 0 || distcodeVa == 0) {
    return false;
  }
  if (whave > 0 && (windowVa == 0 || wsize == 0 || wnext > wsize || whave > wsize)) {
    return false;
  }

  const uint32_t outPrefix = start - availOut;
  if (nextOutVa < outPrefix) {
    return false;
  }
  const uint32_t begVa = nextOutVa - outPrefix;

  if (!mem.IsValid(nextInVa, availIn) ||
      !mem.IsValid(begVa, start) ||
      !mem.IsValid(lencodeVa, (1u << lenbits) * 4u) ||
      !mem.IsValid(distcodeVa, (1u << distbits) * 4u)) {
    return false;
  }
  if (wsize > 0 && windowVa != 0 && !mem.IsValid(windowVa, wsize)) {
    return false;
  }

  const uint8_t *inBase = static_cast<const uint8_t *>(mem.Translate(nextInVa));
  uint8_t *begBase = static_cast<uint8_t *>(mem.Translate(begVa));
  const uint8_t *lencodeBase = static_cast<const uint8_t *>(mem.Translate(lencodeVa));
  const uint8_t *distcodeBase = static_cast<const uint8_t *>(mem.Translate(distcodeVa));
  const uint8_t *windowBase = (wsize > 0 && windowVa != 0)
                                  ? static_cast<const uint8_t *>(mem.Translate(windowVa))
                                  : nullptr;
  if (!inBase || !begBase || !lencodeBase || !distcodeBase) {
    return false;
  }

  uint32_t inIdx = 0;
  const uint32_t lastIdx = availIn - 5u;
  uint32_t outIdx = outPrefix; // relative to begBase
  const uint32_t endIdx = outPrefix + (availOut - 257u);

  const uint32_t lmask = (1u << lenbits) - 1u;
  const uint32_t dmask = (1u << distbits) - 1u;

  uint8_t op = 0;
  uint8_t codeBits = 0;
  uint16_t val = 0;

  do {
    if (bits < 15u) {
      hold += static_cast<uint32_t>(inBase[inIdx++]) << bits;
      bits += 8u;
      hold += static_cast<uint32_t>(inBase[inIdx++]) << bits;
      bits += 8u;
    }
    ReadCodeBe(lencodeBase, hold & lmask, op, codeBits, val);
  dolen:
    hold >>= codeBits;
    bits -= codeBits;
    if (op == 0u) {
      begBase[outIdx++] = static_cast<uint8_t>(val);
    } else if ((op & 16u) != 0u) {
      uint32_t len = val;
      uint32_t extra = op & 15u;
      if (extra != 0u) {
        if (bits < extra) {
          hold += static_cast<uint32_t>(inBase[inIdx++]) << bits;
          bits += 8u;
        }
        len += hold & ((1u << extra) - 1u);
        hold >>= extra;
        bits -= extra;
      }
      if (bits < 15u) {
        hold += static_cast<uint32_t>(inBase[inIdx++]) << bits;
        bits += 8u;
        hold += static_cast<uint32_t>(inBase[inIdx++]) << bits;
        bits += 8u;
      }
      ReadCodeBe(distcodeBase, hold & dmask, op, codeBits, val);
    dodist:
      hold >>= codeBits;
      bits -= codeBits;
      if ((op & 16u) != 0u) {
        uint32_t dist = val;
        extra = op & 15u;
        if (bits < extra) {
          hold += static_cast<uint32_t>(inBase[inIdx++]) << bits;
          bits += 8u;
          if (bits < extra) {
            hold += static_cast<uint32_t>(inBase[inIdx++]) << bits;
            bits += 8u;
          }
        }
        dist += hold & ((1u << extra) - 1u);
        hold >>= extra;
        bits -= extra;
        if (dist > outIdx) {
          uint32_t fromWin = dist - outIdx;
          if (fromWin > whave) {
            (void)mem.WriteU32BE(strmVa + 24u, kZlibMsgTooFar);
            (void)mem.WriteU32BE(stateVa + 0u, kZlibModeBad);
            break;
          }
          uint32_t winPos = 0;
          if (wnext == 0u) {
            winPos = wsize - fromWin;
            if (fromWin < len) {
              len -= fromWin;
              std::memcpy(begBase + outIdx, windowBase + winPos, fromWin);
              outIdx += fromWin;
              // Rest comes from current output buffer at outIdx - dist
              for (uint32_t i = 0; i < len; ++i) {
                begBase[outIdx + i] = begBase[outIdx + i - dist];
              }
              outIdx += len;
              len = 0;
            }
          } else if (wnext < fromWin) {
            winPos = wsize + wnext - fromWin;
            fromWin -= wnext;
            if (fromWin < len) {
              len -= fromWin;
              std::memcpy(begBase + outIdx, windowBase + winPos, fromWin);
              outIdx += fromWin;
              if (wnext < len) {
                fromWin = wnext;
                len -= fromWin;
                std::memcpy(begBase + outIdx, windowBase, fromWin);
                outIdx += fromWin;
                for (uint32_t i = 0; i < len; ++i) {
                  begBase[outIdx + i] = begBase[outIdx + i - dist];
                }
                outIdx += len;
                len = 0;
              } else {
                std::memcpy(begBase + outIdx, windowBase, len);
                outIdx += len;
                len = 0;
              }
            }
          } else {
            winPos = wnext - fromWin;
            if (fromWin < len) {
              len -= fromWin;
              std::memcpy(begBase + outIdx, windowBase + winPos, fromWin);
              outIdx += fromWin;
              for (uint32_t i = 0; i < len; ++i) {
                begBase[outIdx + i] = begBase[outIdx + i - dist];
              }
              outIdx += len;
              len = 0;
            }
          }
          if (len > 0u) {
            std::memcpy(begBase + outIdx, windowBase + winPos, len);
            outIdx += len;
          }
        } else {
          // Copy from output buffer (may overlap when dist < len)
          for (uint32_t i = 0; i < len; ++i) {
            begBase[outIdx + i] = begBase[outIdx + i - dist];
          }
          outIdx += len;
        }
      } else if ((op & 64u) == 0u) {
        const uint32_t subIdx = static_cast<uint32_t>(val) +
                                (hold & ((1u << op) - 1u));
        if (!mem.IsValid(distcodeVa + subIdx * 4u, 4u)) {
          return false;
        }
        ReadCodeBe(distcodeBase, subIdx, op, codeBits, val);
        goto dodist;
      } else {
        (void)mem.WriteU32BE(strmVa + 24u, kZlibMsgBadDist);
        (void)mem.WriteU32BE(stateVa + 0u, kZlibModeBad);
        break;
      }
    } else if ((op & 64u) == 0u) {
      const uint32_t subIdx = static_cast<uint32_t>(val) +
                              (hold & ((1u << op) - 1u));
      if (!mem.IsValid(lencodeVa + subIdx * 4u, 4u)) {
        return false;
      }
      ReadCodeBe(lencodeBase, subIdx, op, codeBits, val);
      goto dolen;
    } else if ((op & 32u) != 0u) {
      (void)mem.WriteU32BE(stateVa + 0u, kZlibModeType);
      break;
    } else {
      (void)mem.WriteU32BE(strmVa + 24u, kZlibMsgBadLitLen);
      (void)mem.WriteU32BE(stateVa + 0u, kZlibModeBad);
      break;
    }
  } while (inIdx < lastIdx && outIdx < endIdx);

  const uint32_t unusedBytes = bits >> 3;
  inIdx -= unusedBytes;
  bits -= unusedBytes << 3;
  hold &= (1u << bits) - 1u;

  (void)mem.WriteU32BE(strmVa + 0u, nextInVa + inIdx);
  (void)mem.WriteU32BE(strmVa + 4u, availIn - inIdx);
  (void)mem.WriteU32BE(strmVa + 12u, begVa + outIdx);
  (void)mem.WriteU32BE(strmVa + 16u, start - outIdx);
  (void)mem.WriteU32BE(stateVa + 56u, hold);
  (void)mem.WriteU32BE(stateVa + 60u, bits);
  return true;
}

} // namespace

// ===========================================================================
// Global C++-linkage hook definitions (declared in src/hooks/hooks.h)
// ===========================================================================

PPC_FUNC(sub_8221BE98) {
  const uint32_t warper = ctx.r3.u32;
  const float dt = static_cast<float>(ctx.f1.f64);
  __imp__sub_8221BE98(ctx, base);
  if (warper != 0) {
    WatchWarper(ctx, base, warper, dt);
  }
}

PPC_FUNC(sub_822BC658) {
  static const bool s_noAiGuard = EnvEnabled("MCLA_NO_AI_GUARD");
  if (!s_noAiGuard) {
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    const uint32_t self = ctx.r3.u32;
    uint32_t slot8 = 0;
    uint32_t brain = 0;
    uint32_t vt = 0;
    if (self == 0 || !mem.ReadU32BE(self + 8u, &slot8) || slot8 == 0 ||
        !mem.ReadU32BE(slot8, &brain) || brain == 0 ||
        !mem.ReadU32BE(brain, &vt) || vt == 0) {
      static std::atomic<uint32_t> s_aiNull{0};
      const uint32_t k = s_aiNull.fetch_add(1, std::memory_order_relaxed) + 1;
      if (k <= 32 || (k % 500) == 0) {
        MCLA_LOG_WARN("AI-RESET-NULL #{} r3={:08X} slot8={:08X} brain={:08X} vt={:08X} lr={:08X}",
                      k, self, slot8, brain, vt, static_cast<uint32_t>(ctx.lr));
      }
      return;
    }
  }
  __imp__sub_822BC658(ctx, base);
}

PPC_FUNC(sub_821E1598) {
  static const bool s_noNativeAes = EnvEnabled("MCLA_NO_NATIVE_AES");
  const uint32_t bufVa = ctx.r3.u32;
  const uint32_t len = ctx.r4.u32;
  const uint32_t aesCtxVa = ctx.r5.u32;
  const uint32_t fnVa = ctx.r6.u32;

  if (s_noNativeAes || fnVa != 0x821E0FF0u || (len % 16u) != 0u ||
      !HostHasAesNi()) {
    __imp__sub_821E1598(ctx, base);
    return;
  }
  if (len == 0u) {
    ctx.r3.s64 = 1;
    return;
  }

  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint32_t nr = 0;
  if (!mem.ReadU32BE(aesCtxVa + 480u, &nr) ||
      (nr != 10u && nr != 12u && nr != 14u)) {
    __imp__sub_821E1598(ctx, base);
    return;
  }
  const uint32_t rkBytesCount = (nr + 1u) * 16u;
  if (!mem.IsValid(bufVa, len) || !mem.IsValid(aesCtxVa + 240u, rkBytesCount)) {
    __imp__sub_821E1598(ctx, base);
    return;
  }

  uint8_t rkBytes[15u * 16u] = {};
  if (!mem.ReadBytes(aesCtxVa + 240u, rkBytes, rkBytesCount)) {
    __imp__sub_821E1598(ctx, base);
    return;
  }

  uint8_t *hostBuf = static_cast<uint8_t *>(mem.Translate(bufVa));
  if (!hostBuf) {
    __imp__sub_821E1598(ctx, base);
    return;
  }

  int check = g_aesSelfCheckState.load(std::memory_order_acquire);
  if (check == 0) {
    std::lock_guard<std::mutex> lk(g_aesCheckMtx);
    check = g_aesSelfCheckState.load(std::memory_order_relaxed);
    if (check == 0) {
      uint8_t testBlock[16];
      std::memcpy(testBlock, hostBuf, 16);
      AesDecrypt16xBlocksAesNi(testBlock, 16u, rkBytes, nr);

      PPCContext checkCtx = ctx;
      checkCtx.r3.u64 = bufVa;
      checkCtx.r4.u64 = 16u;
      checkCtx.r5.u64 = aesCtxVa;
      checkCtx.r6.u64 = fnVa;
      __imp__sub_821E1598(checkCtx, base);

      if ((checkCtx.r3.u32 & 0xFFu) != 0u &&
          std::memcmp(testBlock, hostBuf, 16) == 0) {
        g_aesSelfCheckState.store(1, std::memory_order_release);
        MCLA_LOG_INFO("AES-NI self-test PASSED (Nr={})", nr);
        if (len > 16u) {
          AesDecrypt16xBlocksAesNi(hostBuf + 16u, len - 16u, rkBytes, nr);
        }
        ctx.r3.s64 = 1;
        return;
      }
      g_aesSelfCheckState.store(-1, std::memory_order_release);
      MCLA_LOG_ERROR("AES-NI self-test FAILED (Nr={}) - falling back to guest", nr);
      if (len > 16u) {
        ctx.r3.u64 = bufVa + 16u;
        ctx.r4.u64 = len - 16u;
        ctx.r5.u64 = aesCtxVa;
        ctx.r6.u64 = fnVa;
        __imp__sub_821E1598(ctx, base);
      } else {
        ctx.r3.u64 = checkCtx.r3.u64;
      }
      return;
    }
  }

  if (check < 0) {
    __imp__sub_821E1598(ctx, base);
    return;
  }

  AesDecrypt16xBlocksAesNi(hostBuf, len, rkBytes, nr);
  ctx.r3.s64 = 1;
}

PPC_FUNC(sub_821E4B20) {
  static const bool s_noNativeInflate = EnvEnabled("MCLA_NO_NATIVE_INFLATE");
  if (!s_noNativeInflate && RunNativeInflateFast(ctx.r3.u32, ctx.r4.u32)) {
    return;
  }
  __imp__sub_821E4B20(ctx, base);
}
