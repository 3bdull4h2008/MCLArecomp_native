// Phase 2 split of src/patches.cpp: this TU owns the OOM/allocation-path/pool16/slab census hooks (ex-lines 1093-2647).







// Entry points used by mcla_ApplyPatches are declared in hooks/hooks.h.







// includes mirror patches.cpp (plus kernel/heap_arena.h + <mutex> for the F-412 guard-aware census) so hook bodies resolve identically.







#include "patches.h"







#include "hooks/hooks.h"







#include "app.h"







#include "cpu/ppc_context.h"







#include "generated/ppc_xenon/ppc_recomp_shared.h"







#include "gpu_cp.h"







#include "gpu_device.h"







#include "gpu_mmio.h"







#include "guest_memory.h"







#include "logging.h"







#include "native_renderer.h"







#include "render_queue.h"







#include "render_thread.h"







#include "renderer_mode.h"







#include "fs/vfs_rpf.h"
#include "kernel/heap_arena.h"  // F-412: kUserHeapArenaVa / kPhysArenaBeginVa, so the census knows our own instance















#include "kernel/memory.h"







#include "kernel/xbox.h"







#include "kernel/kernel_objects.h"















#include <algorithm>
#include <map>
#include <set>







#include <atomic>







#include <cctype>







#include <chrono>







#include <cstdlib>
#include <cstring>







#include <filesystem>







#include <fstream>







#include <mutex>







#include <unordered_set>







#include <system_error>







#include <unordered_map>







#include <unordered_set>







#include <vector>















#include <fcntl.h>







#include <io.h>







#include <windows.h>















// Forward declarations for kernel functions via import thunks







extern "C" uint32_t KeReleaseSemaphore(void *, uint32_t, uint32_t, uint32_t);


PPC_FUNC_IMPL(__imp__sub_821C29A0);







PPC_FUNC(sub_821C29A0) {







  // SESSION 39 FIX: handle zero-size allocations (legitimate degenerate







  // requests from the game). Standard allocator semantics: malloc(0) returns







  // a valid minimum-size allocation. Session-37 used size=4 (elemsize==4







  // class), but the game's own zero-size free path writes directly to the







  // elemsize==4 slab header — bypassing DE908 — corrupting the freelist.







  // Fix: route to the size class matching the requested alignment instead,







  // avoiding the elemsize==4 slab entirely. When align=0, fall back to 16.







  uint32_t effectiveSize = ctx.r4.u32;







  if (effectiveSize == 0) {







    effectiveSize = (ctx.r5.u32 > 0) ? ctx.r5.u32 : 16;







    ctx.r4.u32 = effectiveSize;







  }















  __imp__sub_821C29A0(ctx, base);
}

extern std::atomic<uint32_t> g_carsResCat;  // gpu_device.cpp, MCLA_TEMP_CARS_BUDDY

// MCLA_TEMP_BUDDY_CALLERS=1: buddyB bytes per caller chain, cumulative and LIVE (freed bytes subtracted).
struct BuddyChainStat { uint64_t allocBytes = 0, count = 0; int64_t liveBytes = 0; };
static std::mutex s_bcMu;
static std::unordered_map<uint64_t, BuddyChainStat> s_bcByChain;
static std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> s_bcLive;  // ptr -> (chain, size)
static void BuddyChainReport() {
  static uint64_t tick = 0;
  const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  if (now < tick + 5000) return;
  tick = now;
  std::vector<std::pair<uint64_t, BuddyChainStat>> rows(s_bcByChain.begin(), s_bcByChain.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.liveBytes > b.second.liveBytes; });
  for (size_t i = 0; i < rows.size() && i < 8; ++i)
    MCLA_LOG_ERROR("BUDDY-CALLER #{} lr1={:08X} lr2={:08X} liveKiB={} allocKiB={} n={}", i, uint32_t(rows[i].first >> 32),
                   uint32_t(rows[i].first), rows[i].second.liveBytes >> 10, rows[i].second.allocBytes >> 10, rows[i].second.count);
}
static void BuddyChainNote(uint32_t ptr, uint64_t chain, uint32_t size) {
  std::lock_guard<std::mutex> lk(s_bcMu);
  auto& s = s_bcByChain[chain]; s.allocBytes += size; ++s.count; s.liveBytes += size;
  s_bcLive[ptr] = {chain, size};
  BuddyChainReport();
}
uint64_t CsAllocBytes() {  // MCLA_TEMP_CSTREAM: cumulative buddyB bytes (all chains), for before/after diffs
  std::lock_guard<std::mutex> lk(s_bcMu); uint64_t t = 0; for (auto& kv : s_bcByChain) t += kv.second.allocBytes; return t;
}
static void BuddyChainFree(uint32_t ptr) {
  std::lock_guard<std::mutex> lk(s_bcMu);
  auto it = s_bcLive.find(ptr);
  if (it == s_bcLive.end()) return;
  s_bcByChain[it->second.first].liveBytes -= it->second.second;
  s_bcLive.erase(it);
}

// sub_821E2820 = pgStreamable placement, called from the streamer Update (sub_821D3A68) for the ONE
// request it picked (min prio). On failure the request stays at the head of the list and is retried
// every Update: measured 599 failures of one 7.4 MB resource in 90 s, blocking every request behind it.
// Fix: push the failed request's priority back so the next Update picks another one; it still loads
// once the queue has room. Streamer object = 0x8286CE50; request list ptr +6216, u16 count +6220,
// entries {res, cb, float prio @8, arg} x16 bytes (sub_821D3810). MCLA_NO_STREAM_SKIP=1 disables.
static void DeferFailedStreamRequest(uint32_t res) {
  static const bool off = [] { const char* e = std::getenv("MCLA_NO_STREAM_SKIP"); return e && e[0] == '1'; }();
  // Opt-in: the corrected deref DEADLOCKS the title (rc8: fps 0, draws 0 after 13 s) because the guest waits for the deferred nearest sector.
  static const bool fix = [] { const char* e = std::getenv("MCLA_STREAM_DEFER_FIX"); return e && e[0] == '1'; }();
  if (off || !fix || !res) return;
  auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
  // dword_8286CE50 HOLDS the streamer pointer (822D79F4-822D7A04: lis/lwz then lbz 0x1844(r11)); using the
  // global's own address made this whole function a silent no-op (reads returned 0 -> early return).
  uint32_t kStreamer = 0;
  if (!gm.ReadU32BE(0x8286CE50u, &kStreamer) || !kStreamer) return;
  uint32_t list = 0, count = 0;
  if (!gm.ReadU32BE(kStreamer + 6216, &list) || !list) return;
  if (!gm.ReadU32BE(kStreamer + 6220 - 2, &count)) return;  // u16 at +6220 is the low half of the BE word at +6218
  count &= 0xFFFFu;
  for (uint32_t i = 0; i < count && i < 4096; ++i) {
    uint32_t r = 0, pbits = 0;
    if (!gm.ReadU32BE(list + i * 16, &r) || r != res) continue;
    if (!gm.ReadU32BE(list + i * 16 + 8, &pbits)) return;
    float prio; std::memcpy(&prio, &pbits, 4);
    prio += 1.0e6f;
    std::memcpy(&pbits, &prio, 4);
    (void)gm.WriteU32BE(list + i * 16 + 8, pbits);
    return;
  }
}
// STREAM-ALLOC-CENSUS (log-only, MCLA_STREAM_ALLOC=1): sub_821E58F0 = pgStreamable chunk allocator
// (two vtable allocs per chunk: type 1 then type 3, 128-byte align). Prints chunk sizes + result.
PPC_FUNC_IMPL(__imp__sub_821E58F0);
PPC_FUNC(sub_821E58F0) {
  static const bool on = [] { const char* e = std::getenv("MCLA_STREAM_ALLOC"); return e && e[0] == '1'; }();
  const uint32_t a2 = ctx.r4.u32;
  __imp__sub_821E58F0(ctx, base);
  if (!on) return;
  static std::atomic<uint32_t> nOk{0}, nBad{0};
  const bool ok = ctx.r3.u32 != 0;
  const uint32_t k = (ok ? nOk : nBad).fetch_add(1) + 1;
  if (k > 24 && (k % 100) != 0) return;
  auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
  uint16_t n1 = 0, n2 = 0; uint32_t sz1 = 0, sz3 = 0, mode = 0;
  (void)gm.ReadU16BE(a2, &n1); (void)gm.ReadU16BE(a2 + 2, &n2); (void)gm.ReadU32BE(0x8286CE80u, &mode);
  for (uint32_t i = 0; i < uint32_t(n1) + n2 && i < 64; ++i) {
    uint32_t sz = 0; (void)gm.ReadU32BE(a2 + 12u + 12u * i, &sz);
    (i < n1 ? sz1 : sz3) += (sz + 127u) & ~127u;
  }
  uint32_t tls = 0, obj = 0, vt = 0, f8 = 0, f14 = 0;
  (void)gm.ReadU32BE(ctx.r13.u32, &tls); (void)gm.ReadU32BE(tls + 28u, &obj);
  (void)gm.ReadU32BE(obj, &vt); (void)gm.ReadU32BE(vt + 8u, &f8); (void)gm.ReadU32BE(vt + 20u, &f14);
  MCLA_LOG_ERROR("STREAM-ALLOC-OBJ tls={:08X} obj={:08X} vt={:08X} alloc_fn={:08X} sel_fn={:08X}", tls, obj, vt, f8, f14);
  MCLA_LOG_ERROR("STREAM-ALLOC {} #{} res_a2={:08X} n1={} n2={} sum_t1={} sum_t3={} mode_dword={:08X}",
                 ok ? "OK" : "FAIL", k, a2, n1, n2, sz1, sz3, mode);
}
// SECTOR-STATE census (log-only, MCLA_STREAM_ALLOC=1): sub_822D9DE0 = per-sector paging step
// (state +252: 0 out,1 sched,2 loading,3 resident,4 evict,5 evicting; dist +260; id +244; res +248;
// page-in when dist < [0x827E0E34] (raw 600.0f), evict when dist >= it). Logs every state change.
PPC_FUNC_IMPL(__imp__sub_822D9DE0);
PPC_FUNC(sub_822D9DE0) {
  static const bool on = [] { const char* e = std::getenv("MCLA_STREAM_ALLOC"); return e && e[0] == '1'; }();
  const uint32_t s = ctx.r3.u32;
  auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
  // MCLA_TEMP_SECTOR_RADIUS=<float>: experiment only -- overwrite the guest page-in radius so every
  // in-radius sector fits the 299.5 MiB buddy; discriminates "near trees need streaming" (default off).
  static const float s_rad = [] { const char* e = std::getenv("MCLA_TEMP_SECTOR_RADIUS"); return e ? std::strtof(e, nullptr) : 0.0f; }();
  if (s_rad > 0.0f) { uint32_t b; std::memcpy(&b, &s_rad, 4); (void)gm.WriteU32BE(0x827E0E34u, b); }
  uint32_t st0 = 0;
  if (on) (void)gm.ReadU32BE(s + 252, &st0);
  __imp__sub_822D9DE0(ctx, base);
  if (!on) return;
  uint32_t st1 = 0, id = 0, dbits = 0, res = 0, w8 = 0, rbits = 0;
  (void)gm.ReadU32BE(s + 252, &st1);
  if (st1 == st0) return;
  (void)gm.ReadU32BE(s + 244, &id); (void)gm.ReadU32BE(s + 260, &dbits); (void)gm.ReadU32BE(s + 248, &res);
  if (res) (void)gm.ReadU32BE(res + 8, &w8);
  (void)gm.ReadU32BE(0x827E0E34u, &rbits);
  float d, r; std::memcpy(&d, &dbits, 4); std::memcpy(&r, &rbits, 4);
  static std::atomic<uint32_t> n{0};
  if (n.fetch_add(1) < 4000)
    MCLA_LOG_ERROR("SECTOR-STATE id={} obj={:08X} {}->{} dist={:.1f} radius={:.1f} res={:08X} sizeK={}", id, s, st0, st1, d, r, res,
                   (w8 & 0x7FFFFE0u) >> 10);
}
PPC_FUNC_IMPL(__imp__sub_821BC6B8);
PPC_FUNC_IMPL(__imp__sub_821E2820);
PPC_FUNC(sub_821E2820) {
  static const bool on = [] { const char* e = std::getenv("MCLA_TEMP_BUDDY_CALLERS"); return e && e[0] == '1'; }();
  const uint32_t res = ctx.r3.u32;
  // STREAM-TYPE census (log-only, MCLA_STREAM_ALLOC=1): request callback/arg (= requesting object type)
  // for this placement, read from the streamer request list BEFORE the call; bytes placed per callback.
  static const bool st = [] { const char* e = std::getenv("MCLA_STREAM_ALLOC"); return e && e[0] == '1'; }();
  uint32_t stCb = 0, stArg = 0, stArgVt = 0, stW8 = 0, stH = 0;
  if (st && res) {
    auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
    uint32_t sp = 0, list = 0, cnt = 0;
    (void)gm.ReadU32BE(res + 4, &stH); (void)gm.ReadU32BE(res + 8, &stW8);
    if (gm.ReadU32BE(0x8286CE50u, &sp) && sp && gm.ReadU32BE(sp + 6216, &list) && list && gm.ReadU32BE(sp + 6218, &cnt))
      for (uint32_t i = 0; i < (cnt & 0xFFFFu) && i < 4096; ++i) {
        uint32_t r = 0;
        if (!gm.ReadU32BE(list + i * 16, &r) || r != res) continue;
        (void)gm.ReadU32BE(list + i * 16 + 4, &stCb); (void)gm.ReadU32BE(list + i * 16 + 12, &stArg);
        if (stArg) (void)gm.ReadU32BE(stArg, &stArgVt);
        break;
      }
  }
  __imp__sub_821E2820(ctx, base);
  if (st && res) {
    struct Row { uint64_t okK = 0; uint32_t ok = 0, fail = 0; uint32_t vt = 0; };
    static std::mutex m; static std::map<uint32_t, Row> rows; static std::set<uint32_t> failRes; static uint64_t t = 0;
    std::lock_guard<std::mutex> lk(m);
    auto& r = rows[stCb]; r.vt = stArgVt;
    const uint32_t kb = (stW8 >> 10) & 0x1FFFFu;
    if (ctx.r3.u32) { ++r.ok; r.okK += kb; } else {
      ++r.fail;
      if (failRes.insert(res).second) {
        auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
        uint32_t ht = 0, hm = 0, e[7] = {};
        (void)gm.ReadU32BE(0x8283D1C4u, &ht); (void)gm.ReadU32BE(0x8283D1A8u, &hm);
        if (ht && hm) for (int i = 0; i < 7; ++i) (void)gm.ReadU32BE(ht + 28u * (stH & (hm - 1)) + 4u * i, &e[i]);
        MCLA_LOG_ERROR("STREAM-TYPE-FAIL res={:08X} h={:08X} w8={:08X} sizeK={} cb={:08X} arg={:08X} argVt={:08X} hent=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                       res, stH, stW8, kb, stCb, stArg, stArgVt, e[0], e[1], e[2], e[3], e[4], e[5], e[6]);
      }
    }
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now >= t + 5000) {
      t = now;
      for (auto& kv : rows)
        MCLA_LOG_ERROR("STREAM-TYPE cb={:08X} argVt={:08X} ok={} okKiB={} fail={}", kv.first, kv.second.vt, kv.second.ok, kv.second.okK, kv.second.fail);
    }
  }
  if (!ctx.r3.u32) DeferFailedStreamRequest(res);
  {  // STREAM-FAIL-NAME (log-only, MCLA_STREAM_ALLOC=1): name of the file whose placement failed
    static const bool nm = [] { const char* e = std::getenv("MCLA_STREAM_ALLOC"); return e && e[0] == '1'; }();
    static std::atomic<uint32_t> nn{0};
    if (nm && !ctx.r3.u32 && ((nn.fetch_add(1) + 1) <= 3)) {
      auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
      uint32_t h = 0; (void)gm.ReadU32BE(res + 4, &h);
      PPCContext c2 = ctx; c2.r3.u32 = h;
      __imp__sub_821BC6B8(c2, base);
      char buf[200] = {}; (void)gm.ReadBytes(c2.r3.u32, buf, 199);
      MCLA_LOG_ERROR("STREAM-FAIL-NAME res={:08X} h={:08X} name='{}'", res, h, buf);
    }
  }
  if (!on) return;
  static std::mutex mu;
  static std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> fails;  // res -> (fails, sizeWord)
  static uint64_t ok = 0, bad = 0, tick = 0;
  std::lock_guard<std::mutex> lk(mu);
  if (ctx.r3.u32) { ++ok; } else {
    ++bad;
    uint32_t w = 0;
    (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(res + 8, &w);
    auto& f = fails[res]; ++f.first; f.second = w;
  }
  const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  if (now >= tick + 5000) {
    tick = now;
    std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> rows(fails.begin(), fails.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    MCLA_LOG_ERROR("PLACE-821E2820 ok={} fail={} distinctFailing={}", ok, bad, fails.size());
    for (size_t i = 0; i < rows.size() && i < 5; ++i)
      MCLA_LOG_ERROR("PLACE-FAIL #{} res={:08X} fails={} size={} KiB", i, rows[i].first, rows[i].second.first,
                     (rows[i].second.second & 0x0FFFFFE0u) >> 10);
  }
}

PPC_FUNC_IMPL(__imp__sub_82130010);







static std::atomic<uint32_t> s_buddyAlloc{0};







// MEM81 BUDDY-STATE: live bytes of buddyB (guest arena A3A8C000..B660C000, 0x12B80000 B) from the wrapper
// calls themselves; once per second. shortcut: no largest-free-block (needs the guest's buddy bitmap).
static std::mutex s_b76Mu;
static std::unordered_map<uint32_t, uint32_t> s_b76Live;
static uint64_t s_b76Bytes = 0, s_b76AllocB = 0, s_b76FreeB = 0;
static std::atomic<uint64_t> s_b76LastTick{0};
static void Buddy76State() {
  const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  uint64_t last = s_b76LastTick.load();
  if (now < last + 1000 || !s_b76LastTick.compare_exchange_strong(last, now)) return;
  std::lock_guard<std::mutex> lk(s_b76Mu);
  constexpr uint64_t kCap = 0x12B80000ull;
  MCLA_LOG_WARN("BUDDY-STATE buddyB live={}KiB blocks={} free={}KiB allocB/s={}KiB freeB/s={}KiB",
                s_b76Bytes >> 10, s_b76Live.size(), (kCap > s_b76Bytes ? kCap - s_b76Bytes : 0) >> 10,
                s_b76AllocB >> 10, s_b76FreeB >> 10);
  s_b76AllocB = s_b76FreeB = 0;
}
static bool InBuddyB(uint32_t p) { return p >= 0xA3A8C000u && p < 0xB660C000u + 0x10000u; }
PPC_FUNC(sub_82130010) {







  const uint32_t n = s_buddyAlloc.fetch_add(1) + 1;







  const uint32_t size = ctx.r4.u32;
  // MCLA_TEMP_BUDDY_CALLERS=1: who allocates from buddyB, by caller chain (pre-call lr + two saved LRs
  // up the back chain, saved at prevSP-8 by the guest prologue `stw r12,-8(r1)`); top rows every 5 s.
  static const bool s_bc = [] { const char* e = std::getenv("MCLA_TEMP_BUDDY_CALLERS"); return e && e[0] == '1'; }();
  uint64_t chainKey = 0;
  if (s_bc) {
    auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
    uint32_t sp1 = 0, lr1 = 0, sp2 = 0, lr2 = 0;
    (void)gm.ReadU32BE(ctx.r1.u32, &sp1);
    if (sp1) (void)gm.ReadU32BE(sp1 - 8, &lr1);
    if (sp1) (void)gm.ReadU32BE(sp1, &sp2);
    if (sp2) (void)gm.ReadU32BE(sp2 - 8, &lr2);
    chainKey = (uint64_t(lr1) << 32) | lr2;
  }

  __imp__sub_82130010(ctx, base);
  if (s_bc && ctx.r3.u32 && InBuddyB(ctx.r3.u32)) BuddyChainNote(ctx.r3.u32, chainKey, size);
  if (ctx.r3.u32 && InBuddyB(ctx.r3.u32)) { std::lock_guard<std::mutex> lk(s_b76Mu); s_b76Live[ctx.r3.u32] = size; s_b76Bytes += size; s_b76AllocB += size; }
  {  // MCLA_TEMP_CARS_BUDDY=1: who fills buddyB, and what the guest asked for when it ran dry (prediction: vehicle dominates, then failures cluster after 100% fill)
    static const bool s_cb = [] { const char* e = std::getenv("MCLA_TEMP_CARS_BUDDY"); return e && e[0] == '1'; }();
    if (s_cb) {
      static std::atomic<uint64_t> s_catBytes[6], s_catFail[6];
      static std::unordered_map<uint32_t, uint8_t> s_catOf;   // guarded by s_b76Mu
      const uint32_t cat = g_carsResCat.load(std::memory_order_relaxed) % 6u;
      if (ctx.r3.u32 && InBuddyB(ctx.r3.u32)) { std::lock_guard<std::mutex> lk(s_b76Mu); s_catOf[ctx.r3.u32] = static_cast<uint8_t>(cat); s_catBytes[cat] += size; }
      else if (!ctx.r3.u32) {
        const uint64_t nf = ++s_catFail[cat];
        if (nf <= 8 || (nf % 64) == 0)
          MCLA_LOG_WARN("TEMP-CARS-BUDDY-FAIL size={:#x} cat={} (1=veh 2=city 3=chr 4=ui 5=oth) fails[cat]={} lr={:08X} bytesAllocByCat v={} c={} h={} u={} o={} (KiB, cumulative, never decremented)",
                        size, cat, nf, static_cast<uint32_t>(ctx.lr), s_catBytes[1] >> 10, s_catBytes[2] >> 10, s_catBytes[3] >> 10, s_catBytes[4] >> 10, s_catBytes[5] >> 10);
      }
    }
  }
  Buddy76State();







  if (n <= 60 || size >= 1024) {







    MCLA_LOG_WARN("BUDDY76-ALLOC #{} dual={:08X} size={:#x} ret={:08X} lr={:08X}",







                  n, ctx.r3.u32, size, ctx.r3.u32,







                  static_cast<uint32_t>(ctx.lr));







  }







}















PPC_FUNC_IMPL(__imp__sub_82130048);







static std::atomic<uint32_t> s_buddyFree{0};







PPC_FUNC(sub_82130048) {







  const uint32_t n = s_buddyFree.fetch_add(1) + 1;







  const uint32_t ptr = ctx.r4.u32;







  __imp__sub_82130048(ctx, base);
  if (InBuddyB(ptr)) BuddyChainFree(ptr);
  if (InBuddyB(ptr)) { std::lock_guard<std::mutex> lk(s_b76Mu); auto it = s_b76Live.find(ptr); if (it != s_b76Live.end()) { s_b76Bytes -= it->second; s_b76FreeB += it->second; s_b76Live.erase(it); } }
  Buddy76State();







  // F-412: F-391 moved the pool to kPhysArenaBeginVa (0xA0400000). The old
  // 0xA0000000..0xA4000000 band over-included 4 MiB that are NOT ours (the title's own XTL
  // region) and under-included 0xA4000000..0xFFFFFFFF -- ~75% of the pool, which w430 has live
  // (HEAP76 buddyA/B @A4401000/A5401000). uint32_t tops out at 0xFFFFFFFF, so no upper bound.
  if (n <= 60 || ptr >= mcla::kernel::kPhysArenaBeginVa) {







    MCLA_LOG_WARN("BUDDY76-FREE #{} ptr={:08X} lr={:08X}{}", n, ptr,







                  static_cast<uint32_t>(ctx.lr),







                  // F-412: same band as the gate above (pool begins at kPhysArenaBeginVa).
                  (ptr >= mcla::kernel::kPhysArenaBeginVa) ? " LOWPOOL" : "");







  }







}















// MCLA_TEMP_FREEGATE=1 (diagnostic only, F-free-gate): who ever releases resource memory?
// Counts the guest's texture/resource chunk-free sub_821E5598 (20 static callers) with the first
// caller LRs, the car eviction state machine sub_822AA5C0 (state at obj+656: 0 idle,1 evict-requested,
// 2 waiting-on-vinyls, 3, 4; frame countdowns at +664/+668) and car-slot evict sub_82372C20, plus
// the GPU-identifier fence dword. One FREEGATE row per second at error level.
static bool FreeGateOn() {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_FREEGATE"); return e && *e == '1'; }();
  return on;
}
static std::atomic<uint32_t> s_fgChunkFree{0}, s_fgEvictSm{0}, s_fgCarEvict{0}, s_fgNoStream{0};
static std::atomic<uint32_t> s_fgSt[8];
static std::atomic<uint64_t> s_fgTick{0};
static void FreeGateRow() {
  const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  uint64_t last = s_fgTick.load();
  if (now < last + 1000 || !s_fgTick.compare_exchange_strong(last, now)) return;
  uint32_t gid = 0, gidv = 0;
  gid = mcla::gpu::CpGpuIdentifierAddress();
  if (gid) (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(gid, &gidv);
  MCLA_LOG_ERROR("FREEGATE chunkFree(821E5598)={} evictSm(822AA5C0)={} st[0..4]={}/{}/{}/{}/{} carEvict(82372C20)={} "
                 "cannotStream(82720AC8)={} gpuId@{:08X}={:08X}",
                 s_fgChunkFree.load(), s_fgEvictSm.load(), s_fgSt[0].load(), s_fgSt[1].load(), s_fgSt[2].load(),
                 s_fgSt[3].load(), s_fgSt[4].load(), s_fgCarEvict.load(), s_fgNoStream.load(), gid, gidv);
}

// MCLA_TEMP_RELCYCLE=1: per-streamable lifecycle (request / state->3 placed / Release call / state->0) to see whether
// the same resources cycle place->release->re-request. Census only; 5 s summaries.
namespace {
struct RelRow { uint32_t req = 0, place = 0, rel = 0, zero = 0, lastRelLr = 0, lastRelState = 0; };
std::mutex g_rcMu;
std::unordered_map<uint32_t, RelRow> g_rc;
std::unordered_map<uint64_t, uint32_t> g_rcLr;  // (lr<<2|state) -> Release calls
uint64_t g_rcUpd = 0, g_rcUpdPrev = 0;
uint32_t g_rcEv = 0;
struct RelLive { uint32_t lr = 0, kib = 0; };
std::unordered_map<uint32_t, RelLive> g_rcLive;  // res -> placed (lr, KiB) while state==3
void RelEv(const char* what, uint32_t res, uint32_t lr) {  // caller holds g_rcMu; timeline of placed/released events only
  if (g_rcEv >= 400) return;
  ++g_rcEv;
  MCLA_LOG_ERROR("RELCYCLE-EV {} res={:08X} upd={} lr={:08X}", what, res, g_rcUpd, lr);
}
uint32_t sObjG() { uint32_t o = 0; (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(0x8286CE50u, &o); return o; }
bool RelCycleOn() { static const bool on = [] { const char* e = std::getenv("MCLA_TEMP_RELCYCLE"); return e && e[0] == '1'; }(); return on; }
void RelCycleDump() {
  static uint64_t tick = 0;
  const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  if (now < tick + 5000) return;
  tick = now;
  uint64_t req = 0, place = 0, rel = 0, zero = 0; uint32_t cyc = 0;
  std::vector<std::pair<uint32_t, RelRow>> rows(g_rc.begin(), g_rc.end());
  for (auto& r : rows) { req += r.second.req; place += r.second.place; rel += r.second.rel; zero += r.second.zero; if (r.second.place >= 3) ++cyc; }
  uint32_t sObj = 0; (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(0x8286CE50u, &sObj);
  uint16_t relCnt = 0; (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU16BE(sObj + 6228, &relCnt);
  MCLA_LOG_ERROR("RELCYCLE distinct={} req={} place={} rel={} toZero={} placedGE3={} relListCount={}", rows.size(), req, place, rel, zero, cyc, relCnt);
  {
    uint32_t cd = 0, w = 0; (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(sObjG() + 6252, &cd);
    MCLA_LOG_ERROR("RELCYCLE-UPD updates={} (+{} in 5s) releaseCountdown={}", g_rcUpd, g_rcUpd - g_rcUpdPrev, cd); (void)w;
    g_rcUpdPrev = g_rcUpd;
    { uint32_t ev = 0, fl = 0, t0 = 0, t1 = 0, cur = 0; auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
      (void)gm.ReadU32BE(0x8287C7ECu, &ev); (void)gm.ReadU32BE(0x827E0E34u, &cur); (void)gm.ReadU32BE(0x827E0E48u, &t0); (void)gm.ReadU32BE(0x827E0E4Cu, &t1); (void)fl;
      float fc, f0, f1; std::memcpy(&fc, &cur, 4); std::memcpy(&f0, &t0, 4); std::memcpy(&f1, &t1, 4);
      auto u = [&](uint32_t a) { uint32_t v = 0; (void)gm.ReadU32BE(a, &v); return v; };
      auto f = [&](uint32_t a) { uint32_t v = u(a); float x; std::memcpy(&x, &v, 4); return x; };
      MCLA_LOG_ERROR("RELCYCLE-SECT2 lock82879510={} cnt8780={} cand8790={} 79C={} 7A0={} 7A8={} f7C0={} f798={} f7BC={} f7C4={} flt0E44={} failFlagByte={:08X} placedN={} placedKiB={}",
                     static_cast<int>(u(0x82879510)), static_cast<int>(u(0x8287C780)), static_cast<int>(u(0x8287C790)), static_cast<int>(u(0x8287C79C)),
                     static_cast<int>(u(0x8287C7A0)), static_cast<int>(u(0x8287C7A8)), f(0x8287C7C0), f(0x8287C798), f(0x8287C7BC), f(0x8287C7C4), f(0x827E0E44),
                     u(0x8287C7C8), static_cast<int>(u(0x8287C7E4)), u(0x8287C7DC) >> 10);
      {  // sector table: dword_827E0DC8 -> +20 array, +26 u16 count, stride 304; id +244, res +248, state +252, dist +260, flags u16 +292
        uint32_t tab = u(0x827E0DC8), arr = 0; uint16_t n16 = 0;
        if (tab) { (void)gm.ReadU32BE(tab + 20, &arr); (void)gm.ReadU16BE(tab + 26, &n16); }
        uint32_t st[8] = {0}, st3need = 0, st3far = 0, st2 = 0; uint64_t kib3 = 0; std::string rows;
        for (uint32_t i = 0; arr && i < n16 && i < 400; ++i) {
          uint32_t a = arr + i * 304, state = u(a + 252), res = u(a + 248), fl = 0; uint16_t f16 = 0; (void)gm.ReadU16BE(a + 292, &f16); fl = f16;
          ++st[state & 7]; const uint32_t rw = res ? (u(res + 8) & 0x30000000u) >> 28 : 9;
          if (state == 3) { kib3 += (u(res + 8) & 0x0FFFFFE0u) >> 10; if (fl & 1) ++st3need; else ++st3far; }
          if (state == 2) ++st2;
          if ((state == 3 || state == 2) && rows.size() < 1600) rows += fmt::format("[{} s{} rs{} d{:.0f} f{:X} r{:X} {}K] ", u(a + 244), state, rw, f(a + 260), fl, res, res ? (u(res + 8) & 0x0FFFFFE0u) >> 10 : 0);
        }
        MCLA_LOG_ERROR("RELCYCLE-SECT3 n={} states0..5={},{},{},{},{},{} s3needed(bit0)={} s3notneeded={} s3KiB={} pending(s2)={}", n16, st[0], st[1], st[2], st[3], st[4], st[5], st3need, st3far, kib3, st2);
        MCLA_LOG_ERROR("RELCYCLE-SECT4 {}", rows);
      }
      {  // streaming focus (flt_8287C8D0/D4/D8, refreshed when the camera moves >20, sub_822D79E0) vs the camera (view +240/244/248)
        const uint32_t camBase = u(0x8287E064) + (u(0x827E5640) << 6);
        const uint32_t cp = u(camBase + 0);  // dword_8287E064 + (idx<<6) is an array of camera ptrs? print raw chain below
        (void)cp;
        const uint32_t cam = u(camBase + 0);
        MCLA_LOG_ERROR("RELCYCLE-FOCUS focus=({:.1f},{:.1f},{:.1f}) camBlk={:08X} cam+240=({:.1f},{:.1f},{:.1f}) refresh795={} 7DA={}",
                       f(0x8287C8D0), f(0x8287C8D4), f(0x8287C8D8), camBase, f(camBase + 240), f(camBase + 244), f(camBase + 248),
                       u(0x8287C795) >> 24, u(0x8287C7DA) >> 24); (void)cam;
      }
      MCLA_LOG_ERROR("RELCYCLE-SECT evictionCount={} streamDist cur={} normal={} allocFail={}", static_cast<int>(ev), fc, f0, f1); }
    std::vector<std::pair<uint32_t, uint64_t>> lv; for (auto& kv : g_rcLr) lv.emplace_back(kv.second, kv.first);
    std::sort(lv.rbegin(), lv.rend());
    for (size_t i = 0; i < lv.size() && i < 8; ++i)
      MCLA_LOG_ERROR("RELCYCLE-LR #{} lr={:08X} resState={} n={}", i, static_cast<uint32_t>(lv[i].second >> 2), static_cast<uint32_t>(lv[i].second & 3), lv[i].first);
  }
  {
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> byLr;  // lr -> (count, KiB)
    uint64_t tot = 0;
    for (auto& kv : g_rcLive) { auto& e = byLr[kv.second.lr]; ++e.first; e.second += kv.second.kib; tot += kv.second.kib; }
    MCLA_LOG_ERROR("RELCYCLE-LIVE placedNow={} totalKiB={}", g_rcLive.size(), tot);
    for (auto& kv : byLr) MCLA_LOG_ERROR("RELCYCLE-LIVE lr={:08X} n={} KiB={}", kv.first, kv.second.first, kv.second.second);
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.place > b.second.place; });
  for (size_t i = 0; i < rows.size() && i < 6; ++i)
    MCLA_LOG_ERROR("RELCYCLE-TOP #{} res={:08X} req={} place={} rel={} toZero={} lastRelLr={:08X} lastRelState={:X}", i, rows[i].first,
                   rows[i].second.req, rows[i].second.place, rows[i].second.rel, rows[i].second.zero, rows[i].second.lastRelLr, rows[i].second.lastRelState);
}
}  // namespace
void RelCycleReq(uint32_t res) {
  if (!RelCycleOn()) return;
  std::lock_guard<std::mutex> lk(g_rcMu); ++g_rc[res].req; RelCycleDump();
}
PPC_FUNC_IMPL(__imp__sub_821D3920);
PPC_FUNC(sub_821D3920) {
  if (RelCycleOn()) {
    uint32_t w = 0; (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(ctx.r4.u32 + 8, &w);
    std::lock_guard<std::mutex> lk(g_rcMu);
    auto& r = g_rc[ctx.r4.u32]; ++r.rel; r.lastRelLr = static_cast<uint32_t>(ctx.lr); r.lastRelState = (w >> 28) & 3; if (r.lastRelState == 3) RelEv("release", ctx.r4.u32, static_cast<uint32_t>(ctx.lr)); ++g_rcLr[(uint64_t(static_cast<uint32_t>(ctx.lr)) << 2) | r.lastRelState]; RelCycleDump();
  }
  __imp__sub_821D3920(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_821D3A68);
PPC_FUNC(sub_821D3A68) {
  if (RelCycleOn()) { std::lock_guard<std::mutex> lk(g_rcMu); ++g_rcUpd; RelCycleDump(); }
  __imp__sub_821D3A68(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_821E2808);
PPC_FUNC(sub_821E2808) {
  if (RelCycleOn()) {
    std::lock_guard<std::mutex> lk(g_rcMu);
    if (ctx.r4.u32 == 0x30000000u) { ++g_rc[ctx.r3.u32].place; { uint32_t w = 0; (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(ctx.r3.u32 + 8, &w); g_rcLive[ctx.r3.u32] = {static_cast<uint32_t>(ctx.lr), (w & 0x0FFFFFE0u) >> 10}; } RelEv("place", ctx.r3.u32, static_cast<uint32_t>(ctx.lr)); } else if (ctx.r4.u32 == 0) { g_rcLive.erase(ctx.r3.u32); if (g_rc[ctx.r3.u32].place) RelEv("zero", ctx.r3.u32, static_cast<uint32_t>(ctx.lr)); ++g_rc[ctx.r3.u32].zero; }
  }
  __imp__sub_821E2808(ctx, base);
}
// Sector eviction fix. sub_822D5A30 (per frame) force-evicts, on repeated placement failure, the FARTHEST sector that is
// resident (state 3) OR pending (state 2) and not "needed" (flags bit0 clear) -- sub_822D7D00 picks it into dword_8287C790.
// Measured (se2): the farthest are pending sectors whose streamable is state 0 and holds NO memory, so evicting them frees
// nothing, the 10-14 MB nearer sector never places (PLACE-FAIL 1,900+) while 7 resident not-needed sectors keep their pool
// space. Fix: when the guest asks to evict a pending (state 2) sector, evict the farthest resident not-needed sector instead.
// OPT-IN: MCLA_SECTOR_EVICT_FIX=1 (default off = stock behaviour).
PPC_FUNC_IMPL(__imp__sub_822D9D30);
PPC_FUNC(sub_822D9D30) {
  static const bool off = [] { const char* e = std::getenv("MCLA_SECTOR_EVICT_FIX"); return !(e && e[0] == '1'); }();  // opt-in: se4 froze (fps 0) ~10 s after 30 evictions, and the evicted sectors re-paged at once
  if (!off) {
    auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
    auto u32 = [&](uint32_t a) { uint32_t v = 0; (void)gm.ReadU32BE(a, &v); return v; };
    const uint32_t cand = ctx.r3.u32;
    if (u32(cand + 252) == 2) {
      uint32_t tab = u32(0x827E0DC8), arr = 0; uint16_t n = 0;
      if (tab && gm.ReadU32BE(tab + 20, &arr) && gm.ReadU16BE(tab + 26, &n) && arr) {
        uint32_t best = 0; float bestD = -1.0f;
        for (uint32_t i = 0; i < n && i < 1024; ++i) {
          const uint32_t a = arr + i * 304; uint16_t fl = 0;
          if (u32(a + 252) != 3 || !gm.ReadU16BE(a + 292, &fl) || (fl & 1)) continue;
          const uint32_t res = u32(a + 248);
          if (!res || (u32(res + 8) & 0x30000000u) != 0x30000000u) continue;
          const uint32_t db = u32(a + 260); float d; std::memcpy(&d, &db, 4);
          if (d > bestD) { bestD = d; best = a; }
        }
        if (best) {
          static std::atomic<uint32_t> cnt{0};
          const uint32_t c = cnt.fetch_add(1) + 1;
          if (c <= 40 || c % 200 == 0) MCLA_LOG_ERROR("SECTOR-EVICT-FIX #{} pending sector {} replaced by resident sector {} dist={:.0f}", c, u32(cand + 244), u32(best + 244), bestD);
          ctx.r3.u32 = best;
        }
      }
    }
  }
  __imp__sub_822D9D30(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_821E5598);
PPC_FUNC(sub_821E5598) {
  if (FreeGateOn()) {
    const uint32_t n = s_fgChunkFree.fetch_add(1) + 1;
    if (n <= 40) MCLA_LOG_ERROR("FREEGATE chunkFree #{} desc={:08X} lr={:08X}", n, ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
    FreeGateRow();
  }
  __imp__sub_821E5598(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_822AA5C0);
PPC_FUNC(sub_822AA5C0) {
  if (FreeGateOn()) {
    s_fgEvictSm.fetch_add(1);
    uint32_t st = 99;
    (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(ctx.r3.u32 + 656, &st);
    if (st < 8) s_fgSt[st].fetch_add(1);
    FreeGateRow();
  }
  __imp__sub_822AA5C0(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82372C20);
PPC_FUNC(sub_82372C20) {
  if (FreeGateOn()) { s_fgCarEvict.fetch_add(1); FreeGateRow(); }
  __imp__sub_82372C20(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82720AC8);
PPC_FUNC(sub_82720AC8) {
  if (FreeGateOn()) { s_fgNoStream.fetch_add(1); FreeGateRow(); }
  __imp__sub_82720AC8(ctx, base);
}

// MCLA_TEMP_SIMMAP=1 (TEMP, read-only census, error level, 1 row/s): calls/s of the vehicle-sim chain + sim clock globals.
// Chain (static, IDA): Car update sub_82335AA8 (Car vtbl slot 5; gate dword_827D754C!=0 -> W-stage path incl. V vtbl+16, else a1[3]/a1[5] path),
// V=mcCarSim vtbl 0x82045D34: slot3 sub_82330378 (sim/sleep), slot4 sub_8232EA78 (input stage); wheel drive sub_82565320/sub_825652B8;
// input copy sub_822A1058. Clock block 0x827D7540 (init sub_827A8C60): +0x0C flag, +0x10 t, +0x14 scale(1.0), +0x18..0x28 dt=1/60.
static bool SimMapOn() { static const bool on = [] { const char* e = std::getenv("MCLA_TEMP_SIMMAP"); return e && e[0] == '1'; }(); return on; }
static std::atomic<uint32_t> g_sm[12];
static void SimMapRow() {
  static std::atomic<long long> s_last{0};
  const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  long long last = s_last.load();
  if (now - last < 1000 || !s_last.compare_exchange_strong(last, now)) return;
  auto& h = mcla::kernel::GuestMemoryHeap::Instance();
  auto U = [&](uint32_t a) { uint32_t v = 0xDEADBEEFu; (void)h.ReadU32BE(a, &v); return v; };
  auto F = [&](uint32_t a) { float v = -12345.f; (void)h.ReadF32BE(a, &v); return v; };
  MCLA_LOG_ERROR("TEMP-SIMMAP calls/s carUpd={} v_in={} v_sim={} wheelDrv={} wheelFrc={} inCopy={} v_gearTbl={} | flag754C={} 8288E6F0={} 8288E614={} "
                 "clk t={:.4f} scale={:.3f} dt={:.5f}/{:.5f}/{:.5f}/{:.5f}/{:.5f} 7508={:.5f} 7548={:02X}{:02X} taskN={}",
                 g_sm[0].exchange(0), g_sm[1].exchange(0), g_sm[2].exchange(0), g_sm[3].exchange(0), g_sm[4].exchange(0), g_sm[5].exchange(0),
                 g_sm[6].exchange(0), U(0x827D754C), U(0x8288E6F0), U(0x8288E614), F(0x827D7550), F(0x827D7554), F(0x827D7558), F(0x827D755C),
                 F(0x827D7560), F(0x827D7564), F(0x827D7568), F(0x827D7508), (unsigned)(U(0x827D7548) >> 24), (unsigned)((U(0x827D7548) >> 16) & 0xFF),
                 U(0x82855E98));
}
#define SIMMAP_HOOK(VA, IDX) \
  PPC_FUNC_IMPL(__imp__sub_##VA); \
  PPC_FUNC(sub_##VA) { if (SimMapOn()) { g_sm[IDX].fetch_add(1); SimMapRow(); } __imp__sub_##VA(ctx, base); }
SIMMAP_HOOK(82335AA8, 0)
SIMMAP_HOOK(8232EA78, 1)
SIMMAP_HOOK(82330378, 2)
SIMMAP_HOOK(82565320, 3)
SIMMAP_HOOK(825652B8, 4)
SIMMAP_HOOK(822A1058, 5)
SIMMAP_HOOK(8232D800, 6)

PPC_FUNC_IMPL(__imp__sub_8218DCE8);







PPC_FUNC(sub_8218DCE8) {







  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();







  const uint32_t obj = ctx.r3.u32;







  uint32_t fmtWord = 0;







  if (!obj || !mem.ReadU32BE(obj + 4u, &fmtWord)) {







    __imp__sub_8218DCE8(ctx, base);







    return;







  }







  const uint32_t fmt = fmtWord & 0x7FFFFFFFu;







  const uint32_t idx = fmt - 1;







  uint16_t w = 0;







  (void)mem.ReadU16BE(obj + 0u, &w);







  uint32_t pitch = 0;







  bool store = true;







  if (idx > 21) {







    store = false; // bgtlr: pitch untouched







  } else if (fmt == 2) {







    pitch = 2;







  } else if (fmt >= 3 && fmt <= 8) {







    pitch = (w >= 4) ? fmt : 4u;







  } else if (fmt == 9 || fmt == 10 || fmt == 21) {







    store = false;







  } else if (fmt == 11 || fmt == 18 || fmt == 22) {







    pitch = static_cast<uint32_t>(w) << 3;







  } else if (fmt == 12 || fmt == 13) {







    pitch = static_cast<uint32_t>(w) << 4;







  } else if (fmt == 20) {







    pitch = static_cast<uint32_t>(w) << 1;







  } else { // 1, 14, 15, 16, 17, 19







    pitch = static_cast<uint32_t>(w) << 2;







  }







  if (store && obj) {







    (void)mem.WriteU16BE(obj + 12u, static_cast<uint16_t>(pitch));







  }







  __imp__sub_8218DCE8(ctx, base);







}

// MCLA_TEMP_CARS2_BIGPOOL=<KiB> (TEMP, env-gated, default off): override the guest's physical game-pool size.
// sub_82131228 (+0xe4/+0x1c0/+0x1d4) sizes the pool as (sub_82130EC0() + 306688 [lis 4; ori 0xAE00 @0x821312D4..DC]
// - (extraheapmem?0x2000:0)) KiB, plus a metadata block of 6*(that/32) bytes and that/32 32-KiB pages. sub_82130EC0 is a
// video-mode bonus (0 at 720p, ~6.4 MiB at 640x480). Returning (BIGPOOL - 306688) enlarges all three consistently.
// HARD CAP: console physical is 512 MiB (kConsolePhysicalBytes = 0x20000000, GPU addresses are pa & 0x1FFFFFFF), so
// clamp to 306688 + 40960 KiB (+40 MiB) unless MCLA_TEMP_CARS2_BIGPOOL_FORCE=1; everything else must fit in what remains.
PPC_FUNC_IMPL(__imp__sub_82130EC0);
PPC_FUNC(sub_82130EC0) {
    static const long long s_kib = [] { const char* e = std::getenv("MCLA_TEMP_CARS2_BIGPOOL"); return e ? std::atoll(e) : 347136ll; }();  // default +39.5 MiB: with the native resource path the cars stream in (owner-tested); 0 = stock
    static const bool s_force = [] { const char* e = std::getenv("MCLA_TEMP_CARS2_BIGPOOL_FORCE"); return e && e[0] == '1'; }();
    if (s_kib <= 0) { __imp__sub_82130EC0(ctx, base); return; }
    long long want = s_kib;
    if (!s_force && want > 306688ll + 40960ll) want = 306688ll + 40960ll;
    // The guest derives the 32 KiB page count with a truncating /32 and the byte size with <<10, so a size that is
    // not page-aligned leaves pages and bytes disagreeing (the 337000 KiB attempt corrupted memory). The stock size
    // 0x4AE00 KiB is 599 x 512 KiB: keep that granule.
    want &= ~511ll;
    const long long extra = want - 306688ll;
    static bool s_logged = false;
    if (!s_logged) { s_logged = true; MCLA_LOG_ERROR("TEMP-CARS2-BIGPOOL pool KiB {} -> {} (extra {}) force={}", 306688, want, extra, (int)s_force); }
    ctx.r3.u64 = static_cast<uint64_t>(extra < 0 ? 0 : extra);
}

// MCLA_TEMP_BLENDSET=1 (diagnostic): guest SetRenderState(ALPHABLENDENABLE) sub_824139E8 —
// how often the guest enables blending and what word it stores into the RB_BLENDCONTROL shadow.
static std::atomic<uint32_t> s_bsOn{0}, s_bsOff{0}, s_bsLastWord{0};
static std::atomic<uint64_t> s_bsTick{0};
PPC_FUNC_IMPL(__imp__sub_824139E8);
PPC_FUNC(sub_824139E8) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  const uint32_t dev = ctx.r3.u32, enable = ctx.r4.u32;
  __imp__sub_824139E8(ctx, base);
  if (!on) return;
  (enable ? s_bsOn : s_bsOff).fetch_add(1);
  uint32_t w = 0;
  if (enable) { (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(dev + 10552, &w); s_bsLastWord.store(w); }
  const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  uint64_t last = s_bsTick.load();
  if (now >= last + 2000 && s_bsTick.compare_exchange_strong(last, now))
    MCLA_LOG_ERROR("BLENDSET enable={} disable={} lastEnabledWord={:08X} dev={:08X}", s_bsOn.load(), s_bsOff.load(),
                   s_bsLastWord.load(), dev);
}

// MCLA_TEMP_BLENDSET=1: call counts of the other blend render-state setters/getter (table 0x827EC3E0).
static std::atomic<uint32_t> s_bsCnt[11];
static void BlendSetRow() {
  static std::atomic<uint64_t> tick{0};
  const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  uint64_t last = tick.load();
  if (now < last + 2000 || !tick.compare_exchange_strong(last, now)) return;
  MCLA_LOG_ERROR("BLENDSET2 82413A78={} 82413B08={} 82413B98={} 82413C28={} 82413C98={} 82413D08={} 82413D78={} 824150E0={} 82415110={} 82415238={} 82413A68={}", s_bsCnt[0].load(), s_bsCnt[1].load(), s_bsCnt[2].load(), s_bsCnt[3].load(), s_bsCnt[4].load(), s_bsCnt[5].load(), s_bsCnt[6].load(), s_bsCnt[7].load(), s_bsCnt[8].load(), s_bsCnt[9].load(), s_bsCnt[10].load());
}
PPC_FUNC_IMPL(__imp__sub_82413A78);
PPC_FUNC(sub_82413A78) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[0].fetch_add(1); BlendSetRow(); }
  __imp__sub_82413A78(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82413B08);
PPC_FUNC(sub_82413B08) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[1].fetch_add(1); BlendSetRow(); }
  __imp__sub_82413B08(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82413B98);
PPC_FUNC(sub_82413B98) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[2].fetch_add(1); BlendSetRow(); }
  __imp__sub_82413B98(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82413C28);
PPC_FUNC(sub_82413C28) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[3].fetch_add(1); BlendSetRow(); }
  __imp__sub_82413C28(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82413C98);
PPC_FUNC(sub_82413C98) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[4].fetch_add(1); BlendSetRow(); }
  __imp__sub_82413C98(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82413D08);
PPC_FUNC(sub_82413D08) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[5].fetch_add(1); BlendSetRow(); }
  __imp__sub_82413D08(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82413D78);
PPC_FUNC(sub_82413D78) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[6].fetch_add(1); BlendSetRow(); }
  __imp__sub_82413D78(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_824150E0);
PPC_FUNC(sub_824150E0) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[7].fetch_add(1); BlendSetRow(); }
  __imp__sub_824150E0(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82415110);
PPC_FUNC(sub_82415110) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[8].fetch_add(1); BlendSetRow(); }
  __imp__sub_82415110(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82415238);
PPC_FUNC(sub_82415238) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[9].fetch_add(1); BlendSetRow(); }
  __imp__sub_82415238(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82413A68);
PPC_FUNC(sub_82413A68) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_BLENDSET"); return e && *e == '1'; }();
  if (on) { s_bsCnt[10].fetch_add(1); BlendSetRow(); }
  __imp__sub_82413A68(ctx, base);
}

// MCLA_TEMP_ZSET=1: distinct ZFUNC values the guest requests (setter sub_824140E8, RB_DEPTHCONTROL bits 4-6).
PPC_FUNC_IMPL(__imp__sub_824140E8);
PPC_FUNC(sub_824140E8) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_ZSET"); return e && *e == '1'; }();
  if (on) {
    static std::atomic<uint32_t> cnt[8]{};
    const uint32_t f = ctx.r4.u32 & 7u;
    if (cnt[f].fetch_add(1) == 0 || (cnt[f].load() % 5000) == 0)
      MCLA_LOG_ERROR("ZSET zfunc={} calls={} all=[{} {} {} {} {} {} {} {}]", f, cnt[f].load(), cnt[0].load(), cnt[1].load(),
                     cnt[2].load(), cnt[3].load(), cnt[4].load(), cnt[5].load(), cnt[6].load(), cnt[7].load());
  }
  __imp__sub_824140E8(ctx, base);
}
// TEMP-SUBSTEP (MCLA_TEMP_SUBSTEP=1): sub_821BD910(timer, enable, substeps) sets timer dt = dt/substeps. LARecomp notes that a
// substep count of 0 leaves the player car without wheels. Log distinct (enable, substeps, lr) and the resulting dt.
PPC_FUNC_IMPL(__imp__sub_821BD910);
PPC_FUNC(sub_821BD910) {
  static const bool on = [] { const char* e = std::getenv("MCLA_TEMP_SUBSTEP"); return e && e[0] == '1'; }();
  const uint32_t t = ctx.r3.u32, en = ctx.r4.u32 & 0xFF, n = ctx.r5.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_821BD910(ctx, base);
  if (!on) return;
  static std::mutex mu; static std::unordered_map<uint64_t, uint32_t> seen;
  std::lock_guard<std::mutex> lk(mu);
  const uint64_t key = (uint64_t(lr) << 32) | (uint64_t(en) << 24) | (n & 0xFFFFFF);
  const uint32_t c = ++seen[key];
  if (c <= 2 || (c & (c - 1)) == 0) {
    uint32_t a = 0, b = 0; auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
    (void)gm.ReadU32BE(t + 8, &a); (void)gm.ReadU32BE(t + 12, &b); float dt, inv; std::memcpy(&dt, &a, 4); std::memcpy(&inv, &b, 4);
    MCLA_LOG_ERROR("TEMP-SUBSTEP #{} timer={:08X} enable={} substeps={} lr={:08X} -> dt={} 1/dt={}", c, t, en, n, lr, dt, inv);
  }
}
// TEMP-WHEEL (MCLA_TEMP_WHEEL=1): per-step wheel force function sub_825666A0 (wheel vtable 0x8207E4C4 slot +12). It zeroes all
// tyre forces when wheel+96 (ground record) is 0; LARecomp names wheel+529 the contact byte. Counts calls/contact + world pointers.
PPC_FUNC_IMPL(__imp__sub_825666A0);
PPC_FUNC(sub_825666A0) {
  static const bool on = [] { const char* e = std::getenv("MCLA_TEMP_WHEEL"); return e && e[0] == '1'; }();
  if (!on) { __imp__sub_825666A0(ctx, base); return; }
  auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
  auto U = [&](uint32_t a) { uint32_t v = 0; (void)gm.ReadU32BE(a, &v); return v; };
  auto F = [&](uint32_t a) { uint32_t v = U(a); float f; std::memcpy(&f, &v, 4); return f; };
  const uint32_t w = ctx.r3.u32, g96 = U(w + 96);
  __imp__sub_825666A0(ctx, base);
  uint8_t c = 0; (void)gm.ReadU8(w + 529, &c);
  static std::atomic<uint32_t> n{0}, gNZ{0}, cNZ{0};
  const uint32_t k = n.fetch_add(1) + 1; if (g96) gNZ.fetch_add(1); if (c) cNZ.fetch_add(1);
  if (k <= 4 || (k & 0x3FF) == 0)
    MCLA_LOG_ERROR("TEMP-WHEEL calls={} g96!=0:{} contact529!=0:{} w={:08X} g96={:08X} w+288={:08X} w+296={:.3f} w+304={:.3f} level828DA464={:08X} cityBnd828742E0={:08X}",
                   k, gNZ.load(), cNZ.load(), w, g96, U(w + 288), F(w + 296), F(w + 304), U(0x828DA464u), U(0x828742E0u));
}
// TEMP-GROUND (MCLA_TEMP_GROUND=1, TEMP, read-only): wheel+96 is the head of an INLINE intersection record filled by the vehicle ProbeWheels
// method (vtbl slot +28; thunk 0x8232CC18 -> sub_8255DA58). Batched sub_82574840 -> sub_82572D30 does the world test. Logs why it stays empty.
static bool GroundOn() { static const bool on = [] { const char* e = std::getenv("MCLA_TEMP_GROUND"); return e && e[0] == '1'; }(); return on; }
PPC_FUNC_IMPL(__imp__sub_8255DA58);
PPC_FUNC(sub_8255DA58) {
  {  // TEMP A/B (MCLA_TEMP_PROBE_MODE1=1): force the per-wheel batched probe (mode 1) when the vehicle is in mode 2.
    static const bool m1 = [] { const char* e = std::getenv("MCLA_TEMP_PROBE_MODE1"); return e && e[0] == '1'; }();
    uint32_t m = 0; auto& g = mcla::kernel::GuestMemoryHeap::Instance();
    if (m1 && g.ReadU32BE(ctx.r3.u32 + 544, &m) && m == 2) (void)g.WriteU32BE(ctx.r3.u32 + 544, 1);
  }
  if (!GroundOn()) { __imp__sub_8255DA58(ctx, base); return; }
  auto& gm = mcla::kernel::GuestMemoryHeap::Instance();
  auto U = [&](uint32_t a) { uint32_t v = 0; (void)gm.ReadU32BE(a, &v); return v; };
  auto H = [&](uint32_t a) { uint16_t v = 0; (void)gm.ReadU16BE(a, &v); return uint32_t(v); };
  auto F = [&](uint32_t a) { uint32_t v = U(a); float f; std::memcpy(&f, &v, 4); return f; };
  const uint32_t veh = ctx.r3.u32, lvl = U(0x828DA464u), nw = U(veh + 24), arr = U(veh + 28);
  __imp__sub_8255DA58(ctx, base);
  static std::atomic<uint32_t> n{0}, withHit{0};
  const uint32_t k = n.fetch_add(1) + 1;
  uint32_t hits = 0; for (uint32_t i = 0; i < nw && i < 4; ++i) hits += U(U(arr + 4 * i) + 96) != 0;
  if (hits) withHit.fetch_add(1);
  if (k <= 6 || (k & 0x3FF) == 0) {
    const uint32_t w0 = nw ? U(arr) : 0, pose = w0 ? U(w0 + 84) : 0, inst = U(U(veh + 4) + 28), idx = inst ? H(inst + 8) : 0xFFFF;
    uint32_t st = 9, hist[4] = {0, 0, 0, 0};
    if (lvl) { const uint32_t tab = U(lvl + 112), mx = H(lvl + 38);
      if (idx < mx) st = U(tab + 8 * idx + 4) & 3;
      for (uint32_t i = 0; i < mx && i < 16384; ++i) ++hist[U(tab + 8 * i + 4) & 3]; }
    MCLA_LOG_ERROR("TEMP-GROUND #{} veh={:08X} mode544={} a3_488={} rayBytes388={:08X} flags376={:08X} types384={:02X} nWheels={} lvl={:08X} lvlMax={} | "
                   "carInst={:08X} idx={:04X} state={} lvlStates[0..3]={}/{}/{}/{} | steps with any hit={} hits_this_step={} | w0={:08X} pose={:08X} "
                   "seg0 start=({:.3f},{:.3f},{:.3f},{:.3f}) end=({:.3f},{:.3f},{:.3f},{:.3f}) w+564={} w+96={:08X}",
                   k, veh, U(veh + 544), U(veh + 488), U(veh + 388), U(veh + 376), U(veh + 384) >> 24, nw, lvl, lvl ? H(lvl + 38) : 0,
                   inst, idx, st, hist[0], hist[1], hist[2], hist[3], withHit.load(), hits, w0, pose,
                   F(pose), F(pose + 4), F(pose + 8), F(pose + 12), F(pose + 16), F(pose + 20), F(pose + 24), F(pose + 28), w0 ? U(w0 + 564) : 0, w0 ? U(w0 + 96) : 0);
  }
}
// batched probe core: return value = number of hits (<=0 -> sub_82574840 returns 0)
PPC_FUNC_IMPL(__imp__sub_82572D30);
PPC_FUNC(sub_82572D30) {
  __imp__sub_82572D30(ctx, base);
  if (!GroundOn()) return;
  static std::atomic<uint32_t> n{0}, pos{0};
  const uint32_t k = n.fetch_add(1) + 1; if (ctx.r3.s32 > 0) pos.fetch_add(1);
  if (k <= 3 || (k & 0x3FFF) == 0) MCLA_LOG_ERROR("TEMP-GROUND core82572D30 calls={} ret>0={} lastRet={}", k, pos.load(), ctx.r3.s32);
}
// single-segment world probe: hit rate per caller (teleport snap 822A9A6C / 8232D65C, PLAYER_IN_AIR 823268F8, chassis plane 8255CD0C ...)
PPC_FUNC_IMPL(__imp__sub_82574418);
PPC_FUNC(sub_82574418) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82574418(ctx, base);
  if (!GroundOn()) return;
  static std::mutex mu; static std::map<uint32_t, std::pair<uint32_t, uint32_t>> m;
  std::lock_guard<std::mutex> lk(mu);
  auto& e = m[lr]; ++e.first; if (ctx.r3.u32 & 0xFF) ++e.second;
  if (e.first <= 2 || (e.first & (e.first - 1)) == 0) MCLA_LOG_ERROR("TEMP-GROUND probe82574418 lr={:08X} calls={} hits={}", lr, e.first, e.second);
}
// TEMP-INTEG (MCLA_TEMP_INTEG=1, TEMP, read-only): player collider V+128 (672B, vtbl 0x82081554). slot 0x1C sub_8259E768 = integrate (pos+=vel*dt),
// slot 0x2C sub_8259EA40 = ApplyForceAtPoint (+336 force, +352 torque, byte +656 touched). Does either ever run for the player collider?
static uint32_t IntegPlayerCollider() {
  auto& g = mcla::kernel::GuestMemoryHeap::Instance(); uint32_t v = 0;
  auto U = [&](uint32_t a) { uint32_t x = 0; (void)g.ReadU32BE(a, &x); return x; };
  v = U(0x82874374u); v = U(v + 8); v = v ? U(v + 896) : 0; v = v ? U(v + 72) : 0; v = v ? U(v + 8) : 0; return v ? U(v + 128) : 0;  // path = xam.cpp TEMP-VEH
}
static bool IntegOn() { static const bool on = [] { const char* e = std::getenv("MCLA_TEMP_INTEG"); return e && e[0] == '1'; }(); return on; }
static void IntegLog(const char* tag, uint32_t c, uint64_t n) {
  auto& g = mcla::kernel::GuestMemoryHeap::Instance();
  auto F = [&](uint32_t a) { float f = 0; (void)g.ReadF32BE(a, &f); return f; };
  uint8_t t = 0; (void)g.ReadU8(c + 656, &t);
  uint32_t lvl = 0, nAct = 0, list = 0, inst = 0; (void)g.ReadU32BE(0x828DA464u, &lvl);
  uint16_t na = 0; if (lvl) { (void)g.ReadU16BE(lvl + 48, &na); (void)g.ReadU32BE(lvl + 124, &list); } nAct = na;
  (void)g.ReadU32BE(c + 16, &inst); uint16_t idx = 0xFFFF; if (inst) (void)g.ReadU16BE(inst + 8, &idx);
  bool inList = false; for (uint32_t i = 0; i < nAct && i < 512; ++i) { uint16_t e = 0; (void)g.ReadU16BE(list + 2 * i, &e); inList |= (e == idx); }
  MCLA_LOG_ERROR("TEMP-INTEG {} n={} coll={:08X} mass={:.1f} pos=({:.3f},{:.3f},{:.3f}) vel=({:.3f},{:.3f},{:.3f}) F+336=({:.3f},{:.3f},{:.3f}) T+352=({:.3f},{:.3f},{:.3f}) touched656={} | numActive={} idx={:04X} inActiveList={}",
                 tag, n, c, F(c + 192), F(c + 256), F(c + 260), F(c + 264), F(c + 272), F(c + 276), F(c + 280), F(c + 336), F(c + 340), F(c + 344), F(c + 352), F(c + 356), F(c + 360), t, nAct, idx, (int)inList);
}
PPC_FUNC_IMPL(__imp__sub_8259E768);
PPC_FUNC(sub_8259E768) {
  const uint32_t c = ctx.r3.u32;
  __imp__sub_8259E768(ctx, base);
  if (!IntegOn()) return;
  static std::atomic<uint64_t> total{0}, car{0}; total.fetch_add(1);
  if (c == IntegPlayerCollider() && c) { const uint64_t k = car.fetch_add(1) + 1; if (k <= 3 || (k & 0x3FF) == 0) IntegLog("INTEGRATE(slot28)", c, k); }
  else if ((total.load() & 0xFFFF) == 0) MCLA_LOG_ERROR("TEMP-INTEG integrate calls total={} playerCalls={}", total.load(), car.load());
}
PPC_FUNC_IMPL(__imp__sub_8259EA40);
PPC_FUNC(sub_8259EA40) {
  const uint32_t c = ctx.r3.u32;
  if (IntegOn() && c && c == IntegPlayerCollider()) { static std::atomic<uint64_t> k{0}; const uint64_t n = k.fetch_add(1) + 1; if (n <= 3 || (n & 0x3FF) == 0) IntegLog("FORCE-IN(slot44)", c, n); }
  __imp__sub_8259EA40(ctx, base);
}
