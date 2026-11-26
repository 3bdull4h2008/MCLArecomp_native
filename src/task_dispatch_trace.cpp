// Task-runner dispatch forensics (P1 + P4) - 2026-08-24 session.
//
// P1: log-only census on sub_825FDB30 (task-runner dispatch). For every
// invocation record caller LR, container, task, entry, head, node, obj, fn,
// obj+8, obj+12 - ALL via checked guest reads BEFORE passthrough. Goal:
// discriminate (A) registrar ran but this container instance has an
// uninitialized handler vs (B) the registrar/initialization path never ran.
// NO state is mutated.
//
// P4: one-shot scan of the decompressed XEX image for big-endian dwords equal
// to the family entry points (0x825FDB30 et al.) to locate the vtable/data
// table that reaches this dispatcher. Reference-only evidence; no inference.

#include "diag_threads.h"
#include "generated/ppc_xenon/ppc_recomp_shared.h"
#include <cpu/ppc_context.h>
#include "logging.h"
#include "guest_memory.h"
#include "kernel/memory.h"
#include "patches.h"
#include <windows.h>  // F-360: GetCurrentThreadId(), to tie a hook entry to WAIT[..] rows

#include <cmath>
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <mutex>

// w9: PSTREAM virtualâ†’physical map published by gpu_device.cpp at RSC-HEAD
// parse. sub_8217D890 uses it to resolve leftover 0x50xxxxxx virtuals.
struct MclaRebaseRegion {
    uint32_t vbase, pbase, size;
};
extern MclaRebaseRegion g_mclaRebaseMap[];
extern int g_mclaRebaseCount;

// w24: place-pass Resource-fatal gates (defined in gpu_device.cpp).
extern bool MclaPtrIsGateBad(uint32_t p);
static bool MclaPureVm() { static const bool v = [] { const char* e = std::getenv("MCLA_NO_HOST_SERVE"); return ((e ? std::atoi(e) : 55) & 32) != 0; }(); return v; }  // bit 32: place-walk/script hooks pass straight to the guest body
extern bool MclaPtrInRebaseMap(uint32_t p);
extern void MclaCompleteRscRebaseBeforePlace(uint32_t obj);
// w32: open FDA90 walk bound when +24 holds dest-range/SWF-tag candidates.
extern bool MclaW32OpenConstructWalk(uint32_t obj, uint32_t stack,
                                     bool redispatch);

// w15: nested 5500-family resource heads observed by FDBF8-build as CHILDREN
// (lr=825EF1FC). Guest place-pass 825EF100 converts these to 8208xxxx when
// run as ROOT (proven: 44495500â†’82085364 on B7B41000, D0815500â†’82088784 on
// B7B61000) but children only get FDBF8-build, so vt stays C4445500/D0815500.
// Record objects here; gpu_device.cpp re-dispatches the guest place-pass on
// them after the root ARR-RERUN. Never invent a vtable.
constexpr int kMclaNestedPlaceMax = 24;
uint32_t g_mclaNestedPlaceObj[kMclaNestedPlaceMax] = {};
int g_mclaNestedPlaceN = 0;

static void MclaNoteNestedPlaceCand(uint32_t obj, uint32_t vt)
{
    if (obj == 0 || obj == 0xCDCDCDCDu)
        return;
    // Family marker: low 16 bits 0x5500 (RSC/resource head), not already
    // an image/swfC vtable.
    if ((vt & 0xFFFFu) != 0x5500u)
        return;
    if ((vt & 0xFFF00000u) == 0x82000000u)
        return;
    for (int i = 0; i < g_mclaNestedPlaceN; ++i)
        if (g_mclaNestedPlaceObj[i] == obj)
            return;
    if (g_mclaNestedPlaceN >= kMclaNestedPlaceMax)
        return;
    g_mclaNestedPlaceObj[g_mclaNestedPlaceN++] = obj;
}


// ---------------------------------------------------------------------------
// P1: sub_825FDB30 dispatch census (log-only).
// ---------------------------------------------------------------------------
PPC_FUNC_IMPL(__imp__sub_825FDB30);

static std::atomic<uint32_t> s_fdb30Calls{0};

PPC_FUNC(sub_825FDB30) { if (MclaPureVm()) { __imp__sub_825FDB30(ctx, base); return; }
    const uint32_t n = s_fdb30Calls.fetch_add(1) + 1;
    const uint32_t container = ctx.r3.u32;
    const uint32_t task = ctx.r4.u32;
    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();

    // p2f: poison containers (the never-filled preload buffers, e.g.
    // B7B41000 with vt/arr=CDCDCDCD) made the unchecked pointer math in
    // both the census below AND the original body read unmapped guest
    // addresses (entryTable=CDCD + idx<<3) â€” the vectored-handler AV
    // storm at 2+ minutes (crash rva=0x121BF5, caught but noisy).
    // Empty dispatch for poison containers.
    {
        uint32_t arr = 0;
        const bool dead =
            container == 0 || container == 0xCDCDCDCDu || task == 0 ||
            task == 0xCDCDCDCDu ||
            (!mem.ReadU32BE(container + 12, &arr) ||
             (arr & 0xFFFF0000u) == 0xCDCD0000u);
        if (dead)
        {
            static std::atomic<uint32_t> s_fdb30Dead{0};
            const uint32_t dn = s_fdb30Dead.fetch_add(1) + 1;
            if (dn <= 16 || (dn % 500) == 0 || container == 0xA05A6680u || task == 0xA05A6680u)
                MCLA_LOG_ERROR("TEMP-FDB30-DEAD #{:03} cont={:08X} task={:08X} lr={:08X}",
                               dn, container, task, static_cast<uint32_t>(ctx.lr));
            if (container == 0xA05A6680u || task == 0xA05A6680u) {
                MCLA_LOG_ERROR("TEMP-FDB30-DEAD hit A05A6680! letting original run");
            } else {
                return;
            }
        }
    }

    if (n <= 64 || n % 500 == 0)
    {
        uint32_t entryTable = 0;
        uint16_t idx = 0;
        uint8_t verbose = 0;
        uint32_t entry = 0, head = 0, obj = 0, fn = 0, fn8 = 0, fn12 = 0;

        (void)mem.ReadU32BE(container + 12, &entryTable);
        (void)mem.ReadU16BE(task + 164, &idx);
        (void)mem.ReadU8(task + 172, &verbose);
        entry = entryTable + (uint32_t(idx) << 3);
        (void)mem.ReadU32BE(entry + 4, &head);
        if (head != 0)
        {
            (void)mem.ReadU32BE(head, &obj);
            if (obj != 0)
            {
                (void)mem.ReadU32BE(obj + 4, &fn);
                (void)mem.ReadU32BE(obj + 8, &fn8);
                (void)mem.ReadU32BE(obj + 12, &fn12);
            }
        }

        uint32_t entryVal = 0, objVtbl = 0, nodeNext = 0, taskVtbl = 0;
        (void)mem.ReadU32BE(entry, &entryVal);
        (void)mem.ReadU32BE(task, &taskVtbl);
        if (head != 0)
        {
            (void)mem.ReadU32BE(head + 12, &nodeNext);
            (void)mem.ReadU32BE(head, &obj);
            if (obj != 0)
            {
                (void)mem.ReadU32BE(obj + 0, &objVtbl);
                (void)mem.ReadU32BE(obj + 4, &fn);
                (void)mem.ReadU32BE(obj + 8, &fn8);
                (void)mem.ReadU32BE(obj + 12, &fn12);
            }
        }

        MCLA_LOG_INFO("P1-FDB30 #{:05d} lr={:08X} cont={:08X} task={:08X} tbl={:08X} "
                      "idx={} v={} entry={:08X}/{:08X} head={:08X} next={:08X} "
                      "obj={:08X} vtbl={:08X} fn={:08X} "
                      "obj+8={:08X} obj+C={:08X}",
                      n, static_cast<uint32_t>(ctx.lr), container, task, entryTable, idx,
                      verbose, entry, entryVal, head, nodeNext, obj, objVtbl, fn,
                      fn8, fn12);

        // Arm a write-watch on empty method blocks: every guest store into
        // [obj, obj+16) is logged with value + caller LR. Deterministic site
        // (obj=88825500 across runs) => catches the missing initializer live.
        if (obj != 0 && fn == 0 && fn8 == 0 && fn12 == 0)
        {
            static std::atomic<uint32_t> s_armed{0};
            const uint32_t armed = s_armed.fetch_add(1) + 1;
            if (armed <= 4)
            {
                mcla::native::RegisterGuestWatchRange(obj, obj + 16);
                MCLA_LOG_WARN("P1-WATCH-ARMED #{} obj={:08X}", armed, obj);
            }
            // Neighborhood dump: is this a skipped construction inside a pool
            // of otherwise-valid swf objects, or raw untyped memory?
            auto rd = [&](uint32_t a) {
                uint32_t v = 0;
                (void)mem.ReadU32BE(a, &v);
                return v;
            };
            uint8_t typeByte = 0;
            (void)mem.ReadU8(head + 4, &typeByte);
            MCLA_LOG_WARN(
                "P1-OBJWIN head={:08X} type={:02X} | obj-16:{:08X} {:08X} "
                "| obj:{:08X} {:08X} {:08X} {:08X} | obj+16:{:08X} {:08X} "
                "{:08X} {:08X} | +32:{:08X} {:08X} +48:{:08X}",
                head, typeByte, rd(obj - 16), rd(obj - 12), rd(obj), rd(obj + 4),
                rd(obj + 8), rd(obj + 12), rd(obj + 16), rd(obj + 20),
                rd(obj + 24), rd(obj + 28), rd(obj + 32), rd(obj + 36),
                rd(obj + 44), rd(obj + 48));
        }
    }

    __imp__sub_825FDB30(ctx, base);
}

// sub_825FDA90(group=r3, object=r4): tag=u8[object+8]; table @0x825FDAC0.
PPC_FUNC_IMPL(__imp__sub_825FDA90);
static std::atomic<uint32_t> s_hFDA90{0};
// w31: FDA90 type-write census â€” who constructs child swfC?
static std::atomic<uint32_t> s_fda90TagHist[16] = {};
static std::atomic<uint32_t> s_fda90WroteVt{0};
static std::atomic<uint32_t> s_fda90Gated{0};
static std::atomic<uint32_t> s_fda90Passed{0};
PPC_FUNC(sub_825FDA90) { if (MclaPureVm()) { __imp__sub_825FDA90(ctx, base); return; }
    const uint32_t n = s_hFDA90.fetch_add(1) + 1;
    const uint32_t group = ctx.r3.u32; (void)group;
    const uint32_t node = ctx.r4.u32;

    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    uint8_t tag = 0;
    (void)mem.ReadU8(node + 8, &tag);

    // First dispatch => relocations applied; snapshot all three tables.
    if (n == 1)
    {
        auto dumpTable = [&](const char* name, uint32_t base, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t w = 0;
                (void)mem.ReadU32BE(base + i * 4, &w);
                MCLA_LOG_INFO("TBL {} [{:02}] @{:08X} = {:08X}", name, i,
                              base + i * 4, w);
            }
        };
        dumpTable("FDA90", 0x825FDAC0u, 9);
        dumpTable("B588", 0x8260B5B4u, 5);
        dumpTable("DEE40", 0x821DEFD4u, 16);
    }

    uint32_t v0 = 0;
    (void)mem.ReadU32BE(node, &v0);
    const bool gateBad = node != 0 && MclaPtrIsGateBad(node);
    // Guest body: type=u8[node+8]; case=type-1; case>8 â†’ fatal unknown type.
    // Valid types are 1..9. Gate garbage from the 44495500 place walk so
    // EF100 can finish and write the real swfC vtable (never invent one).
    const bool unknownType = (tag < 1u || tag > 9u);
    const bool unreadable = node == 0 || node == 0xCDCDCDCDu;
    if (n <= 48 || (n % 1000) == 0 || gateBad || unknownType)
    {
        MCLA_LOG_INFO("P8-FDA90 #{:05} obj={:08X} tag={:02X} [0]={:08X} "
                      "lr={:08X}{}{}{}",
                      n, node, tag, v0, static_cast<uint32_t>(ctx.lr),
                      gateBad ? " GATE-BAD" : "",
                      unknownType ? " UNKNOWN-TYPE" : "",
                      unreadable ? " UNREADABLE" : "");
    }
    if (unreadable || gateBad || unknownType)
    {
        const uint32_t gn = s_fda90Gated.fetch_add(1) + 1;
        if (gn <= 16 || (gn % 500) == 0 || node == 0xA05A6680u)
            MCLA_LOG_ERROR("TEMP-FDA90-GATE #{:05} node={:08X} tag={:02X} [0]={:08X} lr={:08X} bad={} unk={} unread={}",
                           gn, node, tag, v0, static_cast<uint32_t>(ctx.lr),
                           gateBad ? 1 : 0, unknownType ? 1 : 0, unreadable ? 1 : 0);
        if (node == 0xA05A6680u) {
            MCLA_LOG_ERROR("TEMP-FDA90-GATE hit A05A6680! letting original run");
        } else {
            return;
        }
    }

    // w31: valid type 1..9 â€” census what the guest body writes.
    s_fda90Passed.fetch_add(1);
    if (tag < 16u)
        s_fda90TagHist[tag].fetch_add(1);
    const uint32_t v0Before = v0;

    // Session-28 FINAL: dispatch override removed - the regenerated
    // __imp__ body performs the real in-function goto switch.
    __imp__sub_825FDA90(ctx, base);

    // w31: post-dispatch census â€” did the type handler write a real swfC
    // vtable onto the node? Never invent one; only report what guest wrote.
    {
        uint32_t v0After = 0;
        (void)mem.ReadU32BE(node, &v0After);
        const bool wroteVt = (v0After & 0xFFFF0000u) == 0x82080000u;
        const bool changed = v0After != v0Before;
        if (wroteVt)
            s_fda90WroteVt.fetch_add(1);
        if (wroteVt || changed || n <= 32)
        {
            uint32_t nw[6] = {0};
            for (int k = 0; k < 6; ++k)
                (void)mem.ReadU32BE(node + static_cast<uint32_t>(k * 4),
                                    &nw[k]);
            MCLA_LOG_WARN("W31-FDA90 #{:05} node={:08X} tag={:02X} "
                          "[0] {:08X}->{:08X} wroteVt={} "
                          "[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] "
                          "lr={:08X}",
                          n, node, tag, v0Before, v0After, wroteVt ? 1 : 0,
                          nw[0], nw[1], nw[2], nw[3], nw[4], nw[5],
                          static_cast<uint32_t>(ctx.lr));
        }
    }
    if ((n % 200) == 0)
    {
        MCLA_LOG_WARN("W31-FDA90-SUM n={} passed={} gated={} wroteVt={} "
                      "t1={} t2={} t3={} t4={} t5={} t6={} t7={} t8={} t9={}",
                      n, s_fda90Passed.load(), s_fda90Gated.load(),
                      s_fda90WroteVt.load(),
                      s_fda90TagHist[1].load(), s_fda90TagHist[2].load(),
                      s_fda90TagHist[3].load(), s_fda90TagHist[4].load(),
                      s_fda90TagHist[5].load(), s_fda90TagHist[6].load(),
                      s_fda90TagHist[7].load(), s_fda90TagHist[8].load(),
                      s_fda90TagHist[9].load());
    }
}

// sub_8260B588(group=r3, node=r4): type=u8[node+4]; table @0x8260B5B4.
PPC_FUNC_IMPL(__imp__sub_8260B588);
static std::atomic<uint32_t> s_hB588{0};
PPC_FUNC(sub_8260B588) { if (MclaPureVm()) { __imp__sub_8260B588(ctx, base); return; }
    const uint32_t n = s_hB588.fetch_add(1) + 1;
    const uint32_t group = ctx.r3.u32; (void)group;
    const uint32_t node = ctx.r4.u32;

    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    uint8_t type = 0;
    (void)mem.ReadU8(node + 4, &type);

    uint32_t v0 = 0;
    (void)mem.ReadU32BE(node, &v0);
    const bool watch = node >= 0xB7996E40u && node <= 0xB7996E60u;
    // w24: never dispatch the type-table body on an unmapped gate-bad node
    // (41C4FF12 / 533D00CB from the 44495500 place walk).
    const bool gate = node != 0 && MclaPtrIsGateBad(node);
    if (n <= 64 || (n % 2000) == 0 || watch || gate)
    {
        MCLA_LOG_INFO("P9-B588 #{:05} node={:08X} type={:02X} [0]={:08X}{}{}",
                      n, node, type, v0, watch ? "  <<< CRASH-NODE" : "",
                      gate ? " GATE-SKIP" : "");
    }
    if (gate) {
        static std::atomic<uint32_t> s_b588Gated{0};
        const uint32_t gn = s_b588Gated.fetch_add(1) + 1;
        if (gn <= 16 || (gn % 500) == 0 || node == 0xA05A6680u)
            MCLA_LOG_ERROR("TEMP-B588-GATE #{:05} node={:08X} type={:02X} [0]={:08X}", gn, node, type, v0);
        if (node == 0xA05A6680u) {
            MCLA_LOG_ERROR("TEMP-B588-GATE hit A05A6680! letting original run");
        } else {
            return;
        }
    }

    // Session-28 FINAL: dispatch override removed - regenerated body
    // performs the real in-function goto switch.
    __imp__sub_8260B588(ctx, base);
}

// ---------------------------------------------------------------------------
// Session 72: pointer-rebase via sub_821B5A60(&addr) â†’ sub_8217D890(group,*addr).
// Probe: id=0xCDCDCDCD (uninitialized object field) â†’ lookup miss â†’ fatal
// "Resource 'meshtextures'". Caller lr=0x821B5AB4. Honest short-circuit:
// poison is not a valid guest pointer â€” take the same *param=0 path the
// function already uses on membership miss. Do NOT invent a resource size.
// ---------------------------------------------------------------------------
// F-RSC83: the guest's own loader runs (MCLA_NO_HOSTSERVE / MEMSTREAM_NATIVE default on), so
// host answers to rebase lookups (poison skip, delta-0, stale g_mclaRebaseMap) are damage.
// MCLA_RSC_HOSTFIX=1 restores the old short-circuits for bisecting.
static bool RscHostFix() {
    static const bool on = [] { const char* e = std::getenv("MCLA_RSC_HOSTFIX"); return e && *e == '1'; }();
    return on;
}
// rsc83 MEASURED: native D890/D2378 => guest fatal 'Resource raceeditor/raceeditor' (D890 old=00706164, ASCII "pad"
// walked as a pointer; build/rsc83.log:5986). Opt-in only (MCLA_RSC_NATIVE_LOOKUP=1) until the upstream cause is found.
bool MclaFlowWorldStarted();
static bool RscNativeLookup() {
    // rsc86: native only once the world pass has started (the raceeditor fatal is a pre-world row).
    static const bool all = [] { const char* e = std::getenv("MCLA_RSC_NATIVE_LOOKUP"); return e && *e == '1'; }();
    static const bool bit16 = [] { const char* e = std::getenv("MCLA_NO_HOST_SERVE"); return ((e ? std::atoi(e) : 55) & 16) != 0; }();
    return all || bit16 || MclaFlowWorldStarted();
}
bool MclaRscHostFix() { return RscHostFix(); }
PPC_FUNC_IMPL(__imp__sub_821B5A60);
static std::atomic<uint32_t> s_hB5A60{0};
static std::atomic<uint32_t> s_hB5A60poison{0};
PPC_FUNC(sub_821B5A60)
{
    if (!RscHostFix())
    {
        __imp__sub_821B5A60(ctx, base);
        return;
    }
    const uint32_t n = s_hB5A60.fetch_add(1) + 1;
    const uint32_t param = ctx.r3.u32;
    uint32_t val = 0;
    {
        auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
        (void)mem.ReadU32BE(param, &val);
    }
    if (RscHostFix() && (val & 0xFFu) == 0xCDu)
    {
        auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
        const uint32_t p = s_hB5A60poison.fetch_add(1) + 1;
        MCLA_LOG_WARN("REBASE-POISON #{:05} n={} param={:08X} val={:08X} -> 0 "
                      "lr={:08X} (skip D890 fatal)",
                      p, n, param, val, static_cast<uint32_t>(ctx.lr));
        // TEMP-POISON-CHAIN: guest back-chain (saved LR at [next_sp-8]) so the
        // subclass place-ctor and the resource loader above it are named.
        if (p <= 6)
        {
            std::string chain;
            uint32_t sp = ctx.r1.u32;
            for (int f = 0; f < 12 && sp != 0; ++f)
            {
                uint32_t next = 0, lrv = 0;
                if (!mem.ReadU32BE(sp, &next) || next <= sp || next - sp > 0x10000)
                    break;
                (void)mem.ReadU32BE(next - 8, &lrv);
                chain += fmt::format(" {:08X}", lrv);
                sp = next;
            }
            uint32_t tls = 0, res = 0, res0 = 0, res4 = 0;
            (void)mem.ReadU32BE(ctx.r13.u32, &tls);
            if (tls) (void)mem.ReadU32BE(tls + 60, &res);
            if (res) { (void)mem.ReadU32BE(res, &res0); (void)mem.ReadU32BE(res + 4, &res4); }
            MCLA_LOG_WARN("TEMP-POISON-CHAIN #{} param={:08X} r13={:08X} tls={:08X} "
                          "res(tls+60)={:08X} [res]={:08X} [res+4]={:08X} lrs:{}",
                          p, param, ctx.r13.u32, tls, res, res0, res4, chain);
        }
        // Session 73: dump the surrounding record so the poison scope is
        // visible (one 0x70-stride field vs a fully uninit element).
        if (p <= 3)
        {
            const uint32_t base = param & ~0xFu;
            for (uint32_t row = 0; row < 8; ++row)
            {
                uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
                (void)mem.ReadU32BE(base + row * 16 + 0, &w0);
                (void)mem.ReadU32BE(base + row * 16 + 4, &w1);
                (void)mem.ReadU32BE(base + row * 16 + 8, &w2);
                (void)mem.ReadU32BE(base + row * 16 + 12, &w3);
                MCLA_LOG_WARN("  POISON-DUMP {:08X}: {:08X} {:08X} {:08X} {:08X}",
                              base + row * 16, w0, w1, w2, w3);
            }
        }
        (void)mem.WriteU32BE(param, 0);
        return;
    }
    __imp__sub_821B5A60(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_821D2378);
static std::atomic<uint32_t> s_h1D2378{0};
PPC_FUNC(sub_821D2378)
{
    const uint32_t n = s_h1D2378.fetch_add(1) + 1;
    const uint32_t desc = ctx.r3.u32;
    const uint32_t a4 = ctx.r4.u32;
    const uint32_t a5 = ctx.r5.u32;
    const uint32_t lrIn = static_cast<uint32_t>(ctx.lr);
    // LOG FIRST: the fallback body dereferences [desc+8] unchecked-style and
    // has already AV'd on garbage descriptors - post-call logging never ran.
    // w13: throttle â€” unthrottled flood made place take 41s (same as P10-PHYS).
    if (n <= 32 || (n % 500) == 0)
        MCLA_LOG_WARN("P5-MISSFIX #{:05} desc={:08X} a4={:08X} old={:08X} lr={:08X} "
                      "r13={:08X}",
                      n, desc, a4, a5, lrIn, ctx.r13.u32);
    // w9: this is the "resource group miss â†’ fatal" tail (b 821BD618). After
    // our RSC-NEST walk rewrote 0x50xxxxxx virtual ptrs to physical dests
    // (e.g. 50035E50 â†’ B7996E50), the place-pass still asks the guest rebase
    // table about the ALREADY-PHYSICAL address, misses, and fatals. The
    // caller 8260A830 (loc_8260A8A0) expects either a fatal OR a return that
    // falls into `li r11,0` / loc_8260A8B4 â€” delta 0, pointer already final.
    // Return without invoking the fatal for those.
    // M2: Null or near-null (guard page < 0x1000 or -1): cannot be relocated.
    // Callers (84458, A830, etc.) take the delta-0 path on return.
    // Returning ctx.r3 = 0 skips the fatal dispatcher and prevents the 60k-call fatal loop.
    if (RscNativeLookup()) { __imp__sub_821D2378(ctx, base); return; }
    if (a5 < 0x1000u || a5 == 0xFFFFFFFFu)
    {
        if (n <= 32 || (n % 500) == 0)
            MCLA_LOG_WARN("P5-NULL #{:05} desc={:08X} old={:08X} lr={:08X} - "
                          "null/near-null pointer, skip fatal, delta-0",
                          n, desc, a5, lrIn);
        ctx.r3.u32 = 0;
        return;
    }
    const bool alreadyPhys =
        (a5 >= 0xB7000000u && a5 < 0xB8000000u) ||
        (a5 >= 0xA47F0000u && a5 < 0xA4800000u) ||
        (a5 >= 0x80000000u); // w24: image + all high phys â†’ delta-0
    if (alreadyPhys)
    {
        if (n <= 32 || (n % 500) == 0)
            MCLA_LOG_WARN("P5-PHYS #{:05} old={:08X} lr={:08X} â€” already "
                          "physical, skip fatal (caller takes delta-0 path)",
                          n, a5, lrIn);
        ctx.r3.u32 = 0;
        return;
    }
    // w11: unmapped PSTREAM-family virtual (e.g. 5001CE90 in the package
    // head before the corrected {vbase,pbase,size} parse). Returning 0
    // takes the same delta-0 path alreadyPhys uses so place-pass can
    // finish; nest walk + P10-MAP rewrite the ones we can resolve.
    if (a5 >= 0x50000000u && a5 < 0x70000000u)
    {
        bool mapped = false;
        for (int i = 0; i < g_mclaRebaseCount; ++i)
        {
            const uint32_t vb = g_mclaRebaseMap[i].vbase;
            const uint32_t sz = g_mclaRebaseMap[i].size;
            if (sz != 0 && a5 >= vb && a5 < vb + sz)
            {
                mapped = true;
                break;
            }
        }
        if (n <= 32 || (n % 500) == 0)
            MCLA_LOG_WARN("P5-VIRT #{:05} old={:08X} lr={:08X} mapped={} â€” "
                          "skip fatal, delta-0",
                          n, a5, lrIn, mapped ? 1 : 0);
        ctx.r3.u32 = 0;
        return;
    }
    // w24: gate bad ptrs not in 0x50/0x60/0x80-B phys map (w23: 41C4FF12 /
    // 533D00CB from the 44495500 place walk). Delta-0 family â€” A830/84458
    // already do `li r11,0` after D2378 returns, so place continues.
    if (MclaPtrIsGateBad(a5))
    {
        MCLA_LOG_WARN("P5-GATE #{:05} old={:08X} lr={:08X} â€” not in "
                      "0x50/0x60/0x80-B map, delta-0 (no Resource fatal)",
                      n, a5, lrIn);
        ctx.r3.u32 = 0;
        return;
    }
    __imp__sub_821D2378(ctx, base);
}

// ---------------------------------------------------------------------------
// w11: sub_82184458 place-pass resource walker. It D890-rebases [this+24] /
// [this+28], then D828-looks-up the page at [resource+32] against the guest
// region table. Job #2 left 600E0000 (PSTREAM s[12] â†’ B70A1000) there; the
// empty group table misses and D2378 fatals "Resource '%s': %s (ptr=%p)".
// Pre-rebase +32 through g_mclaRebaseMap so the miss path sees physical
// (alreadyPhys / delta-0) and the walker keeps the final pointer.
// ---------------------------------------------------------------------------
PPC_FUNC_IMPL(__imp__sub_82184458);
static std::atomic<uint32_t> s_h84458{0};
PPC_FUNC(sub_82184458) { if (MclaPureVm()) { __imp__sub_82184458(ctx, base); return; }
    const uint32_t n = s_h84458.fetch_add(1) + 1;
    const uint32_t thisObj = ctx.r3.u32;
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    if (RscHostFix() && (thisObj < 0x1000u || thisObj == 0xCDCDCDCDu || thisObj == 0xFFFFFFFFu))
    {
        ctx.r3.u32 = thisObj;
        return;
    }
    uint32_t res = 0;
    uint32_t f0 = 0, f24 = 0, f28 = 0;
    (void)mem.ReadU32BE(thisObj + 0, &f0);
    (void)mem.ReadU32BE(thisObj + 24, &f24);
    (void)mem.ReadU32BE(thisObj + 28, &f28);

    // If f28 is null / near-null, there is no child descriptor to rebase.
    // Complete the vtable and link safely to avoid 0x20/0x30 FABZERO refusals.
    if (RscHostFix() && f28 < 0x1000u)
    {
        (void)mem.WriteU32BE(thisObj + 0, 0x8200A45Cu);
        (void)mem.WriteU32BE(thisObj + 16, f28);
        ctx.r3.u32 = thisObj;
        return;
    }
    // F-RSC82: the host pre-rebase of +24/+28/+32 through g_mclaRebaseMap was REMOVED.
    // g_mclaRebaseMap is a stale snapshot of ANOTHER resource's table (e.g. 50040000 ->
    // AADDC000 while the group's live table A0960E14 row says 50040000 -> AE694000/B121C000),
    // so it pointed texture/page fields at the wrong buffer before the guest's own D890/D828
    // (which read the live table) ran. The guest now resolves them unmodified.
    // Census when the walker is on a 44495500-family head.
    const bool rsc =
        (f0 & 0xFFFFFF00u) == 0x44495500u ||
        (f0 & 0xFFFFFF00u) == 0x44365500u || (f0 & 0xFFFFu) == 0x5500u;
    if (n <= 16 || rsc || (n % 200) == 0)
    {
        uint32_t page = 0, arr = 0, arr0 = 0;
        if (f28 != 0 && f28 != 0xCDCDCDCDu)
        {
            (void)mem.ReadU32BE(f28 + 32, &page);
            (void)mem.ReadU32BE(f28 + 12, &arr);
            if (arr != 0 && arr != 0xCDCDCDCDu && arr != 0xFFFFFFFFu)
                (void)mem.ReadU32BE(arr, &arr0);
        }
        MCLA_LOG_WARN("P5-84458-CENSUS #{:05} obj={:08X} vt={:08X} "
                      "+24={:08X} +28={:08X} res+32={:08X} arr={:08X} "
                      "[arr]={:08X} rsc={} lr={:08X}{}",
                      n, thisObj, f0, f24, f28, page, arr, arr0,
                      rsc ? 1 : 0, static_cast<uint32_t>(ctx.lr),
                      MclaPtrIsGateBad(page) || MclaPtrIsGateBad(arr0)
                          ? " GATE-BAD"
                          : "");
    }
    __imp__sub_82184458(ctx, base);
}

// ---------------------------------------------------------------------------
// P10 (session 28): delta-provider census with FULL region-table dump on
// any membership miss. sub_8217D890(heapGroup=r3, oldAddr=r4) -> delta or
// -1 (then falls back to 821D2378). Entry layout (TU-verified): stride 12,
// {dupBase@+0, oldBase@+4, newBase@+8}; membership base..base+[ent+8];
// delta = newBase-oldBase. Goal: prove WHY 0x500705E0 misses its region.
// ---------------------------------------------------------------------------
PPC_FUNC_IMPL(__imp__sub_8217D890);
static std::atomic<uint32_t> s_h17D890{0};
PPC_FUNC(sub_8217D890) { if (MclaPureVm()) { __imp__sub_8217D890(ctx, base); return; }
    const uint32_t n = s_h17D890.fetch_add(1) + 1;
    const uint32_t group = ctx.r3.u32;
    const uint32_t oldAddr = ctx.r4.u32;
    const uint32_t lrIn = static_cast<uint32_t>(ctx.lr);
    if (RscNativeLookup()) { __imp__sub_8217D890(ctx, base); return; }

    if (lrIn == 0x82587E3Cu || lrIn == 0x82246B38u)
    {
        static std::atomic<uint32_t> s_6b10D890{0};
        const uint32_t m = s_6b10D890.fetch_add(1) + 1;
        if (m <= 20 || (m % 500) == 0)
        {
            MCLA_LOG_ERROR("TEMP-D890-6B10 #{} m={} oldAddr={:08X} group={:08X} lr={:08X} nmap={}",
                           n, m, oldAddr, group, lrIn, g_mclaRebaseCount);
        }
    }

    if ((oldAddr & 0xFFFF0000u) == 0x00020000u || oldAddr == 0x00020001u || lrIn == 0x82324824u || lrIn == 0x82324890u || lrIn == 0x82325350u)
    {
        static std::atomic<uint32_t> s_sndD890{0};
        const uint32_t sn = s_sndD890.fetch_add(1) + 1;
        if (sn <= 16 || (sn % 2000) == 0)
        {
            auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
            uint32_t tablePtr = 0;
            (void)mem.ReadU32BE(group, &tablePtr);
            uint16_t nVirt = 0, nPhys = 0;
            if (tablePtr != 0) {
                (void)mem.ReadU16BE(tablePtr + 0, &nVirt);
                (void)mem.ReadU16BE(tablePtr + 2, &nPhys);
            }
            MCLA_LOG_ERROR("TEMP-D890-SOUNDBANK #{} oldAddr={:08X} group={:08X} table={:08X} nVirt={} nPhys={} lr={:08X}",
                           sn, oldAddr, group, tablePtr, nVirt, nPhys, lrIn);
            if (tablePtr != 0) {
                const uint32_t total = std::min<uint32_t>(nVirt + nPhys, 16u);
                for (uint32_t ri = 0; ri < total; ++ri) {
                    uint32_t rBase = 0, rTarg = 0, rSize = 0;
                    (void)mem.ReadU32BE(tablePtr + 4 + ri * 12 + 0, &rBase);
                    (void)mem.ReadU32BE(tablePtr + 4 + ri * 12 + 4, &rTarg);
                    (void)mem.ReadU32BE(tablePtr + 4 + ri * 12 + 8, &rSize);
                    MCLA_LOG_ERROR("TEMP-D890-REGION #{} ri={} base={:08X} targ={:08X} size={:08X}",
                                   sn, ri, rBase, rTarg, rSize);
                }
            }
        }
    }

    // Session 72: log BEFORE passthrough. On id=0xCDCDCDCD the original
    // body calls sub_821D2378 which fatals and never returns â€” post-call
    // census stays silent. Pre-call log names the caller.
    if (oldAddr < 0x1000u || (oldAddr & 0xFFu) == 0xCDu || oldAddr == 0xFFFFFFFFu)
    {
        auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
        uint32_t tablePtr = 0;
        (void)mem.ReadU32BE(group, &tablePtr);
        MCLA_LOG_WARN("P10-PRE #{:06} id={:08X} group={:08X} table={:08X} "
                      "lr={:08X}",
                      n, oldAddr, group, tablePtr, lrIn);
        // Skip the original body: it fatals on this id. Return 0 (same as
        // the miss path after fatal) without invoking the fatal dispatcher.
        ctx.r3.u32 = 0;
        return;
    }

    // w7: pointers already in the B7 physical heap (post-RSC-REBASE) have
    // no entry in the rebase group â€” the original body fatals via
    // 821D2378 "Resource '%s'". Return delta 0: the pointer is already at
    // its final physical address. w9: also accept the A47F9xxx dest that
    // job #2 PSTREAM s[4] maps (physical page in the A47F scratch range).
    // w24: any 0x80+ high address (image, host guest-heap, B7, CA) is
    // already final â€” D890 miss must not Resource-fatal the place-pass.
    if ((oldAddr >= 0xB7000000u && oldAddr < 0xB8000000u) ||
        (oldAddr >= 0xA47F0000u && oldAddr < 0xA4800000u) ||
        (oldAddr >= 0x80000000u))
    {
        if (n <= 32 || (n % 500) == 0)
            MCLA_LOG_WARN("P10-PHYS #{:06} id={:08X} group={:08X} lr={:08X} "
                          "-> delta 0 (already physical)",
                          n, oldAddr, group, lrIn);
        ctx.r3.u32 = 0;
        return;
    }

    // CHECK GUEST TABLE FIRST: If this group has an active table with entries,
    // and oldAddr falls within one of its regions, use the guest table's own
    // delta (rTarg - rBase). This ensures soundbanks and other containers with
    // their own rebase tables map to their OWN destination buffers (e.g. A3A74000)
    // rather than being stolen by g_mclaRebaseMap (which holds Job 2's Flash UI mappings).
    auto &gmem = mcla::kernel::GuestMemoryHeap::Instance();
    uint32_t gTablePtr = 0;
    if (group != 0 && gmem.ReadU32BE(group, &gTablePtr) && gTablePtr != 0 && gTablePtr != 0xCDCDCDCDu) {
        uint16_t nVirt = 0, nPhys = 0;
        if (gmem.ReadU16BE(gTablePtr + 0, &nVirt) && gmem.ReadU16BE(gTablePtr + 2, &nPhys)) {
            const uint32_t total = nVirt + nPhys;
            for (uint32_t ri = 0; ri < total; ++ri) {
                uint32_t rBase = 0, rTarg = 0, rSize = 0;
                (void)gmem.ReadU32BE(gTablePtr + 4 + ri * 12 + 0, &rBase);
                (void)gmem.ReadU32BE(gTablePtr + 4 + ri * 12 + 4, &rTarg);
                (void)gmem.ReadU32BE(gTablePtr + 4 + ri * 12 + 8, &rSize);
                if (rSize > 0 && oldAddr >= rBase && oldAddr < rBase + rSize) {
                    const uint32_t delta = rTarg - rBase;
                    if (n <= 64 || (n % 200) == 0) {
                        MCLA_LOG_WARN("D890-GUEST-TABLE #{:06} id={:08X} group={:08X} table={:08X} ri={} "
                                      "base={:08X} targ={:08X} size={:08X} -> delta={:08X}",
                                      n, oldAddr, group, gTablePtr, ri, rBase, rTarg, rSize, delta);
                    }
                    ctx.r3.u32 = delta;
                    return;
                }
            }
        }
    }

    // w9/w11: leftover PSTREAM virtuals (0x50 nested-resource family AND
    // 0x60 stream/page family â€” job #2 s[12] 600E0000â†’B70A1000) still hit
    // the guest's empty rebase table and fatal. Resolve via the PSTREAM map
    // published at RSC-HEAD parse and return the same delta the guest table
    // would have produced (newBase-oldBase). Membership is the map itself â€”
    // do not hard-gate on a single virtual band.
    if (g_mclaRebaseCount > 0)
    {
        for (int i = 0; i < g_mclaRebaseCount; ++i)
        {
            const uint32_t vbase = g_mclaRebaseMap[i].vbase;
            const uint32_t size = g_mclaRebaseMap[i].size;
            if (size == 0)
                continue;
            if (oldAddr >= vbase && oldAddr < vbase + size)
            {
                const uint32_t delta = g_mclaRebaseMap[i].pbase - vbase;
                if (n <= 64 || (n % 200) == 0)
                    MCLA_LOG_WARN("P10-MAP #{:06} id={:08X} vbase={:08X} "
                                  "pbase={:08X} size={:08X} -> delta={:08X}",
                                  n, oldAddr, vbase, g_mclaRebaseMap[i].pbase,
                                  size, delta);
                ctx.r3.u32 = delta;
                return;
            }
        }
    }


    // w24: unmapped bad ptr (41C4FF12 family) â€” delta-0, no fatal.
    if (MclaPtrIsGateBad(oldAddr))
    {
        MCLA_LOG_WARN("P10-GATE #{:06} id={:08X} group={:08X} lr={:08X} "
                      "-> delta 0 (not in rebase map / phys)",
                      n, oldAddr, group, lrIn);
        ctx.r3.u32 = 0;
        return;
    }

    __imp__sub_8217D890(ctx, base);

    const int32_t res = static_cast<int32_t>(ctx.r3.u32);
    const bool watched =
        (oldAddr & 0xFFFF0000u) == 0x50070000u || oldAddr == 0x50035E50u;
    // Session 72: resource-miss path returns 0 (after fatal) with id=0xCDCDCDCD.
    // Dump those plus the historic -1/watch cases.
    const bool poisonId = (oldAddr & 0xFFu) == 0xCDu;
    if (res != -1 && res != 0 && !watched && !poisonId)
    {
        return; // healthy rebase - silent
    }

    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    uint32_t tablePtr = 0, g0 = 0, g4 = 0, g8 = 0;
    (void)mem.ReadU32BE(group, &tablePtr);
    (void)mem.ReadU32BE(group + 0, &g0);
    (void)mem.ReadU32BE(group + 4, &g4);
    (void)mem.ReadU32BE(group + 8, &g8);
    uint32_t regions = tablePtr;
    uint16_t cntA = 0, cntB = 0;
    if (regions != 0)
    {
        (void)mem.ReadU16BE(regions + 0, &cntA);
        (void)mem.ReadU16BE(regions + 2, &cntB);
    }
    MCLA_LOG_WARN("P10-D890 #{:06} {} group={:08X} table={:08X} counts={}/{} "
                  "id={:08X} -> {:08X} lr={:08X} g+4={:08X} g+8={:08X}",
                  n, res == -1 ? "MISS" : (res == 0 && poisonId ? "POISON" : "WATCH"),
                  group, regions, cntA, cntB, oldAddr, ctx.r3.u32, lrIn, g4, g8);
    if (regions != 0)
    {
        constexpr uint32_t kMaxDump = 24;
        for (uint32_t i = 0; i < kMaxDump; ++i)
        {
            uint32_t dupBase = 0, oldBase = 0, newBase = 0;
            (void)mem.ReadU32BE(regions + i * 12, &dupBase);
            (void)mem.ReadU32BE(regions + i * 12 + 4, &oldBase);
            (void)mem.ReadU32BE(regions + i * 12 + 8, &newBase);
            if (dupBase == 0 && oldBase == 0 && newBase == 0)
            {
                break;
            }
            MCLA_LOG_WARN("P10-REGION [{:02}] dup={:08X} old={:08X} new={:08X} "
                          "span-end={:08X} delta={:08X}",
                          i, dupBase, oldBase, newBase, dupBase + newBase,
                          newBase - oldBase);
        }
    }
}

// ---------------------------------------------------------------------------
// P6 (session 26): container-build fixup walker census + node-slot watches.
// rebases each node's [node+0] (an .rdata vtable pointer) by the region
// delta. Runtime fact: one rewritten node[0]=88825500 lands in zeroed heap
// -> dispatch crash. Arming a write-watch on every walked node slot catches
// the rewriting instruction with caller LR, plus dumps the chain shape.
// LOG-ONLY.
// ---------------------------------------------------------------------------
PPC_FUNC_IMPL(__imp__sub_8260A830);
static std::atomic<uint32_t> s_hA830{0};
PPC_FUNC(sub_8260A830) { if (MclaPureVm()) { __imp__sub_8260A830(ctx, base); return; }
    const uint32_t n = s_hA830.fetch_add(1) + 1;
    const uint32_t bucket = ctx.r3.u32;
    const uint32_t buildCtx = ctx.r4.u32;
    if (n <= 48)
    {
        auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
        uint32_t head = 0;
        uint32_t e0 = 0;
        (void)mem.ReadU32BE(bucket, &e0);
        (void)mem.ReadU32BE(bucket + 4, &head);
        MCLA_LOG_INFO("P6-FIXWALK #{:03} bucket={:08X} [0]={:08X} head={:08X}", n,
                      bucket, e0, head);
        uint32_t node = head;
        for (int i = 0; i < 4 && node != 0; ++i)
        {
            uint32_t v0 = 0;
            (void)mem.ReadU32BE(node, &v0);
            MCLA_LOG_INFO("P6-FIXWALK   node={:08X} [0]={:08X}", node, v0);
            mcla::native::RegisterGuestWatchRange(node, node + 4);
            uint32_t next = 0;
            (void)mem.ReadU32BE(node + 12, &next);
            node = next;
        }
    }
    // p2f: the 825FDBF8 build pass walked 27 CDCD buckets then AV'd inside
    // this body (lr=825FDC64, r3=CDCDCEA5). An unmapped/poison bucket base
    // can never be fixed up â€” the container's [+12] array was never filled
    // by the preload task. Skip the original on poison, log as FIXWALK-SKIP.
    // w7: also skip when the bucket ADDRESS is valid but its CONTENTS are
    // poison (RSC-REBASE rewrote a 0x50xxxxxx pointer to a B7 dest that was
    // never inflated â€” B7B61000 stays CDCD).
    {
        auto& memC = mcla::kernel::GuestMemoryHeap::Instance();
        uint32_t b0 = 0;
        if (bucket != 0 && bucket != 0xFFFFFFFFu)
            (void)memC.ReadU32BE(bucket, &b0);
        uint32_t node = 0;
        if (bucket != 0 && bucket != 0xFFFFFFFFu)
            (void)memC.ReadU32BE(bucket + 4, &node);
        const bool bucketGate = MclaPtrIsGateBad(bucket);
        const bool nodeGate = node != 0 && MclaPtrIsGateBad(node);
        if ((bucket & 0xFFFF0000u) == 0xCDCD0000u || bucket == 0 ||
            bucket == 0xFFFFFFFFu || b0 == 0xCDCDCDCDu)
        {
            MCLA_LOG_ERROR("TEMP-FIXWALK-SKIP #{:03} bucket={:08X} [0]={:08X} "
                           "buildCtx={:08X} lr={:08X} (poison bucket contents "
                           "- dest never inflated)",
                           n, bucket, b0, buildCtx,
                           static_cast<uint32_t>(ctx.lr));
            ctx.r3.u32 = 0;
            return;
        }
        // w24: gate bad ptrs the walk would D828/D2378 (41C4FF12 / 533D00CB).
        // Skip the original body so B588 is not invoked on an unmapped node.
        if (bucketGate || nodeGate)
        {
            MCLA_LOG_ERROR("TEMP-FIXWALK-GATE #{:03} bucket={:08X} [0]={:08X} "
                           "node={:08X} buildCtx={:08X} lr={:08X} "
                           "bucketGate={} nodeGate={} â€” delta-0 skip",
                           n, bucket, b0, node, buildCtx,
                           static_cast<uint32_t>(ctx.lr),
                           bucketGate ? 1 : 0, nodeGate ? 1 : 0);
            ctx.r3.u32 = 0;
            return;
        }
        if (n <= 16 || (n % 2000) == 0)
        {
            uint32_t table = 0;
            (void)memC.ReadU32BE(buildCtx, &table);
            MCLA_LOG_ERROR("TEMP-FIXWALK-CENSUS #{:03} bucket={:08X} node={:08X} "
                           "table={:08X} buildCtx={:08X} lr={:08X}",
                           n, bucket, node, table, buildCtx,
                           static_cast<uint32_t>(ctx.lr));
        }
    }
    __imp__sub_8260A830(ctx, base);
}

// ---------------------------------------------------------------------------
// p2f: sub_825FDBF8 = swf-container build pass (vt 0x8208521C family, the
// container whose [+12] entry array stays CDCD when the preload task never
// fills it). Census + poison guard: when the container's own vtable or
// [+12] array is CDCD poison, the whole build pass is garbage-in â€” its
// entry walk would iterate cnt(=0xCDCD)*8 bytes of unmapped memory no
// matter what 8260A830 does per entry. Skip the original body in that
// case (census first logged the state as FDBF8-BUILD).
// ---------------------------------------------------------------------------
PPC_FUNC_IMPL(__imp__sub_825FDBF8);
static std::atomic<uint32_t> s_hFDBF8{0};
PPC_FUNC(sub_825FDBF8) { if (MclaPureVm()) { __imp__sub_825FDBF8(ctx, base); return; }
    const uint32_t n = s_hFDBF8.fetch_add(1) + 1;
    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    const uint32_t obj = ctx.r3.u32;
    uint32_t vt = 0, f4 = 0, f8 = 0, arr = 0;
    uint16_t cnt = 0;
    bool readable = obj != 0 && obj != 0xCDCDCDCDu;
    if (readable)
    {
        (void)mem.ReadU32BE(obj + 0, &vt);
        (void)mem.ReadU32BE(obj + 4, &f4);
        (void)mem.ReadU32BE(obj + 8, &f8);
        (void)mem.ReadU32BE(obj + 12, &arr);
        (void)mem.ReadU16BE(obj + 16, &cnt);
    }
    const bool poison =
        !readable || vt == 0xCDCDCDCDu || arr == 0xCDCDCDCDu || cnt == 0xCDCDu;
    // w7: arr can be a valid B7 address whose CONTENTS are still poison
    // (RSC-REBASE rewrote a 0x50xxxxxx pointer to a dest that was never
    // inflated). Check [arr] too â€” walking it would fatal in 8260A830.
    uint32_t arr0 = 0;
    if (!poison && arr != 0 && arr != 0xFFFFFFFFu)
        (void)mem.ReadU32BE(arr, &arr0);
    const bool arrPoison = (arr0 == 0xCDCDCDCDu);
    const bool arrGate = !poison && !arrPoison && MclaPtrIsGateBad(arr0);
    // F-801 (2026-10-17): dump the SOURCE (r4) the build pass reads. The
    // w792 measurement: FDBF8-BUILD #002/#003 ran on obj=B7B41000 with
    // r4=BA607640/BA607940, wrote the swfC vtable (82085364), but produced
    // cnt=0/arr=0 -- because the DECODED body shows the build pass CONSUMES
    // [obj+12]/[obj+16]; they must be filled by the construct opcode
    // (825EF248) which W33 measures as never-on-place-path. This dump
    // answers the upstream question: is the source the build reads valid
    // (RSC5/5500-family head) or garbage? Prints r4's first 8 dwords and
    // the type byte at [r4+172] (the dispatcher 825FDB30 reads it to pick
    // THIS builder). Budget: first 24 builds.
    //
    // F-801b (2026-10-17, after the w793 read + CodeX mining): [src+0] is a
    // POINTER to the actual movie data (w793: src=BA6074C0 -> [src+0]=
    // BA3F74C0). Per CodeX's Rsc5FlashMovie layout, the movie root at the
    // data base carries: +0 VFT (serialized 0x44495500 'DIU\0'), +12
    // ScenesPosition (u32, base-relative 0x5xxxxxxx), +16 ScenesCount u16,
    // +24 CharactersPosition, +28 CharactersCount u16, +36 Width f32,
    // +44 Height f32, +48 FrameRate u16 (8.8), +50 FrameCount u16. Dump
    // THOSE: if intact, the construct's gate (not the data) is the wall;
    // if garbage, the streamer never staged the decoded bytes here.
    if (n <= 24) {
        const uint32_t src = ctx.r4.u32;
        uint32_t s0=0,s4=0,s8=0,s12=0,s16=0,s20=0,s24=0,s28=0;
        uint32_t type172 = 0;
        uint32_t dataBase = 0;
        if (src != 0 && src != 0xCDCDCDCDu) {
            (void)mem.ReadU32BE(src +  0, &s0);
            (void)mem.ReadU32BE(src +  4, &s4);
            (void)mem.ReadU32BE(src +  8, &s8);
            (void)mem.ReadU32BE(src + 12, &s12);
            (void)mem.ReadU32BE(src + 16, &s16);
            (void)mem.ReadU32BE(src + 20, &s20);
            (void)mem.ReadU32BE(src + 24, &s24);
            (void)mem.ReadU32BE(src + 28, &s28);
            uint8_t tb = 0;
            if (mem.ReadU8(src + 172, &tb))
                type172 = tb;
            dataBase = s0;
        }
        // F-801b: the movie root behind [src+0].
        uint32_t mVFT=0,mScenesPos=0,mCharsPos=0;
        uint16_t mScenesCnt=0,mCharsCnt=0,mFrameRate=0,mFrameCount=0;
        uint32_t mWidth=0,mHeight=0,mHead4=0,mHead8=0;
        if (dataBase != 0 && dataBase != 0xCDCDCDCDu && dataBase != src) {
            (void)mem.ReadU32BE(dataBase +  0, &mVFT);
            (void)mem.ReadU32BE(dataBase +  4, &mHead4);
            (void)mem.ReadU32BE(dataBase +  8, &mHead8);
            (void)mem.ReadU32BE(dataBase + 12, &mScenesPos);
            (void)mem.ReadU16BE(dataBase + 16, &mScenesCnt);
            (void)mem.ReadU32BE(dataBase + 24, &mCharsPos);
            (void)mem.ReadU16BE(dataBase + 28, &mCharsCnt);
            (void)mem.ReadU32BE(dataBase + 36, &mWidth);
            (void)mem.ReadU32BE(dataBase + 44, &mHeight);
            (void)mem.ReadU16BE(dataBase + 48, &mFrameRate);
            (void)mem.ReadU16BE(dataBase + 50, &mFrameCount);
        }
        MCLA_LOG_WARN("FDBF8-SRC #{:03} obj={:08X} src={:08X} "
                      "head=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
                      "{:08X} {:08X}] type@172={:02X}",
                      n, obj, src, s0, s4, s8, s12, s16, s20, s24, s28,
                      type172);
        if (dataBase != 0 && dataBase != src)
            MCLA_LOG_WARN("FDBF8-MOVIEROOT #{} dataBase={:08X} VFT={:08X} "
                          "+4={:08X} +8={:08X} scenesPos={:08X} scenesCnt={} "
                          "charsPos={:08X} charsCnt={} width={} height={} "
                          "frameRate={} frameCount={}",
                          n, dataBase, mVFT, mHead4, mHead8, mScenesPos,
                          mScenesCnt, mCharsPos, mCharsCnt, mWidth, mHeight,
                          mFrameRate, mFrameCount);
        // F-801c (2026-10-17, w794 follow-up): w794 showed [src+0] = a table
        // whose +4=50000000 (the RSC5 virtual-base marker) and +8=A3A6A000
        // (a heap pointer). Chase ONE more level: the heap pointer and a
        // 0x1000-byte scan around it for the movie root's serialized VFT
        // 0x44495500 ('DIU\0') -- the Rsc5FlashMovie root whose +12/+16 are
        // the Scenes fields the construct must copy into [obj+12]/[obj+16].
        if (dataBase != 0 && dataBase != src) {
            uint32_t heapPtr = 0;
            (void)mem.ReadU32BE(dataBase + 8, &heapPtr);
            if (heapPtr != 0 && heapPtr != 0xCDCDCDCDu &&
                heapPtr != 0xFFFFFFFFu && heapPtr != dataBase &&
                heapPtr != 0x50000000u && heapPtr != 0x60000000u) {
                // scan [heapPtr, heapPtr+0x1000) for 0x44495500
                uint32_t diuAt = 0xFFFFFFFFu;
                for (uint32_t off = 0; off < 0x1000; off += 4) {
                    uint32_t w = 0;
                    if (!mem.ReadU32BE(heapPtr + off, &w))
                        break;
                    if (w == 0x44495500u) {
                        diuAt = heapPtr + off;
                        break;
                    }
                }
                uint32_t r0=0, r12=0, r24=0;
                uint16_t r16=0, r28=0, r48=0, r50=0;
                uint32_t r36=0, r44=0;
                uint32_t probe = (diuAt != 0xFFFFFFFFu) ? diuAt : heapPtr;
                (void)mem.ReadU32BE(probe +  0, &r0);
                (void)mem.ReadU32BE(probe + 12, &r12);
                (void)mem.ReadU16BE(probe + 16, &r16);
                (void)mem.ReadU32BE(probe + 24, &r24);
                (void)mem.ReadU16BE(probe + 28, &r28);
                (void)mem.ReadU32BE(probe + 36, &r36);
                (void)mem.ReadU32BE(probe + 44, &r44);
                (void)mem.ReadU16BE(probe + 48, &r48);
                (void)mem.ReadU16BE(probe + 50, &r50);
                MCLA_LOG_WARN("FDBF8-CHASE #{} heapPtr={:08X} DIU@={:08X} "
                              "root=[VFT={:08X} scenes={:08X}/{} chars={:08X}/{} "
                              "w={} h={} rate={} frames={}]",
                              n, heapPtr, diuAt, r0, r12, r16, r24, r28,
                              r36, r44, r48, r50);
            }
        }
    }
    if (n <= 16 || (n % 200) == 0 || poison || arrPoison || arrGate)
        MCLA_LOG_ERROR("TEMP-FDBF8-BUILD #{:03} obj={:08X} vt={:08X} +4={:08X} "
                       "+8={:08X} arr={:08X} [arr]={:08X} cnt={} lr={:08X}{}{}",
                       n, obj, vt, f4, f8, arr, arr0, cnt,
                       static_cast<uint32_t>(ctx.lr),
                       (poison || arrPoison) ? " POISON-SKIP" : "",
                       arrGate ? " ARR-GATE" : "");
    // w15: record nested 5500-family heads for post-place re-dispatch.
    MclaNoteNestedPlaceCand(obj, vt);
    if (poison || arrPoison)
    {
        // Garbage container: neutralize the walk fields the original body
        // would consume ([obj+12] array=NULL, [obj+16] count=0) so both the
        // rebase leg (8217D890) and the entry loop see an empty container.
        //
        // w10 FIX: if the vtable is already a real swfC family pointer
        // (place-pass wrote it) do NOT zero arr/cnt â€” that permanently
        // starved the container (FDBF8-REBUILD then saw arr=0/cnt=0).
        // Only neutralize when the vtable itself is poison/garbage.
        const bool realSwfCVt = (vt & 0xFFFF0000u) == 0x82080000u;
        if (readable && !realSwfCVt)
        {
            (void)mem.WriteU32BE(obj + 12, 0);
            (void)mem.WriteU16BE(obj + 16, 0);
        }
        return; // skip original build pass entirely
    }
    __imp__sub_825FDBF8(ctx, base);
}

// ---------------------------------------------------------------------------
// p2f: sub_825FB0D8 = the swf-container family destructor. Its virtual
// dispatches ([child]=this -> vtable +68/+16/+20) AV'd when the child is
// the never-filled preload container (vt=CDCDCDCD, crash r3=B7B41000,
// lr=825FB14C). Guard: dispatch only when the child's vtable pointer is a
// plausible guest image address (< image top, non-poison); otherwise skip
// â€” same semantics as the function's own [+216]==0 null path.
//
// F-633 (w658.log, read-only): this hook is 27% of that soak â€” 270,216 rows,
// every one `obj=CB139580 child=B7921000 childVt=82085364 +4=0 +12=0
// lr=825EDCF8 POISON-SKIP`, 1.0M lines total, ~2,252 entries/s for 105 s.
// The guest body, re-read from the raw image (tools/ppc_disasm.py 825FB0D8):
//   825FB0F4  mr   r31,r3            ; r31 = obj
//   825FB0F8  lbz  r11,79(r31)       ; [+0x4F] live flag
//   825FB100  beq  -> 825FB328       ; !live -> the whole body is skipped
//   825FB104  lwz  r11,216(r31)      ; [+0xD8]
//   825FB128  lwz  r10,68(r11)       ;   vtable slot +0x44, bctrl
//   825FB134  lwz  r3,4(r31)         ; = the logged `child`
//   825FB140  lwz  r10,16(r11)       ;   child vtable slot +0x10, bctrl
//   825FB14C  <- returns here; the recorded AV (crash-2) was this dispatch
//   825FB260  bl   825F8C70          ; NOT a free -- it builds a stack string
//                                     ; (op59 `*32` runs) and calls 825EF518
// The hot caller is sub_825EDC08: `r29 = lwz 32(r31)` at 825EDCC8 feeds
// `bl sub_825FB0D8` at 825EDCF4 (hence lr=825EDCF8 on every row), and its
// loop 825EDCB0..825EDD40 re-reads that SAME r29 each pass while only ever
// writing 36(r31) and the globals at 0x8290xxxx. Nothing in the loop
// advances the item.
//
// So the loop only terminates if the dtor itself unlinks the node -- and it
// never runs here: `bodyPoison`'s third clause fires on
// `childP4==0 && childP12==0 && childVt!=0`, and those two zeros are what
// F-563's own EF100-PREPOP wrote into B7921000 (see sub_825EF100 below;
// the row is `EF100-PREPOP #001 obj=B7921000` at 19:41:29.162, 0.02 s
// before FB0D8-DTOR #001 at 19:41:29.183). An EMPTY-but-well-formed swfC
// (real vtable 82085364, zero walk fields) is being read as poison.
//
// Verdict: this is a live defect, not a hot path and not a logging bug. The
// short-circuit makes `sub_825EDC08` livelock on one never-destroyed node
// for 105 s. Bounding the row is correct and necessary, but it is NOT the
// fix; the fix is the guard's third clause. Per do-not #1 that change is NOT
// made here (single-variable revert not shown, and the +0x4F early-out at
// 825FB100 means an inert dtor is not provably safe to dispatch) -- this hook
// only makes the spin countable and names it.
//
// Task 3 ruling: sub_825FB0D8 is not a physical-arena allocator and on this
// soak it never even reaches its body -- all 270,216 entries returned at the
// POISON-SKIP below, so it made 0 allocations and 0 frees against the arena
// whose nfree=15 / nalloc=79,985. It is ruled out of the memory exhaustion.
// ---------------------------------------------------------------------------
PPC_FUNC_IMPL(__imp__sub_825FB0D8);
static std::atomic<uint32_t> s_hFB0D8{0};

// F-633: one FB0D8 entry = one object destroyed. w658.log measured 270,216
// entries, 27% of the whole log, ALL of them the single pair
// obj=CB139580/child=B7921000 from the single call site lr=825EDCF8
// (the instruction after `bl sub_825FB0D8` at 825EDCF4, inside the
// 825EDCB0..825EDD40 loop of sub_825EDC08). The old gate was
// `n <= 12 || poisonVt`, and `poisonVt` is STABLY true for this pair
// (bodyPoison's third clause: childVt=82085364 is a real swfC vtable and
// child+4/child+12 are the zeros F-563's EF100-PREPOP wrote into B7921000
// and nothing ever refilled) -- so the gate degenerated to "always print".
//
// So the flood is not a rate, it is a stuck state, and the row's only real
// content is "a NEW (obj,child) pair arrived in this state". This table
// makes that the print rule: the full row prints once per distinct pair,
// and a bounded FB0D8-REPEAT marker names the pathology (an object entered
// the destructor a second time) with the prior destroy's state. The
// counters below keep the 270,216 answer readable after the rows stop.
namespace {
struct Fb0d8Pair {
    std::atomic<uint32_t> obj{0};    // r3 of the first sighting
    std::atomic<uint32_t> child{0};  // [obj+4] of the first sighting
    std::atomic<uint32_t> hits{0};   // destroys of this exact pair
    std::atomic<uint32_t> firstN{0}; // s_hFB0D8 value at the first sighting
    std::atomic<uint32_t> lastN{0};
    std::atomic<uint32_t> lastLr{0};
    std::atomic<uint32_t> lastVt{0};
};
constexpr int kFb0d8Slots = 8;
Fb0d8Pair g_fb0d8[kFb0d8Slots];      // key 0 in BOTH fields == free slot
std::atomic<uint32_t> g_fb0d8Pairs{0};       // distinct (obj,child) pairs seen
std::atomic<uint32_t> g_fb0d8Untracked{0};   // calls with no free slot left
std::atomic<uint32_t> g_fb0d8Suppressed{0};  // rows the OLD gate would have printed

// Find the slot owning (obj,child), else claim a free one. No allocation, no
// guest value is asserted on -- an unreadable obj simply gets its own slot and
// stays countable. Returns nullptr only when all kFb0d8Slots are in use.
Fb0d8Pair* Fb0d8SlotFor(uint32_t obj, uint32_t child, bool* isNew)
{
    for (Fb0d8Pair& s : g_fb0d8)
    {
        if (s.obj.load(std::memory_order_acquire) == obj &&
            s.child.load(std::memory_order_acquire) == child)
        {
            *isNew = false;
            return &s;
        }
    }
    for (Fb0d8Pair& s : g_fb0d8)
    {
        // Claim on the child word (0 = free) and publish obj first, so a
        // reader never sees a half-written key.
        if (s.child.load(std::memory_order_acquire) != 0)
            continue;
        s.obj.store(obj, std::memory_order_relaxed);
        uint32_t expect = 0;
        if (s.child.compare_exchange_strong(expect, child,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire))
        {
            *isNew = true;
            g_fb0d8Pairs.fetch_add(1, std::memory_order_relaxed);
            return &s;
        }
    }
    return nullptr;
}
}  // namespace

PPC_FUNC(sub_825FB0D8) { if (MclaPureVm()) { __imp__sub_825FB0D8(ctx, base); return; }
    const uint32_t n = s_hFB0D8.fetch_add(1) + 1;
    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    const uint32_t obj = ctx.r3.u32;
    uint32_t child = 0, childVt = 0, childP4 = 0, childP12 = 0;
    bool readable = obj != 0 && obj != 0xCDCDCDCDu;
    if (readable)
    {
        (void)mem.ReadU32BE(obj + 4, &child);
        if (child != 0 && child != 0xCDCDCDCDu)
        {
            (void)mem.ReadU32BE(child, &childVt);
            (void)mem.ReadU32BE(child + 4, &childP4);
            (void)mem.ReadU32BE(child + 12, &childP12);
        }
    }
    // w1a: a place-pass can write vt=82085364 onto an otherwise-CDCD
    // buffer. Dispatching the dtor through that vtable AVs at 825FB14C
    // because the body fields are still poison. Require the body to look
    // initialized too (at least one of +4/+12 non-CDCD and non-zero).
    const bool bodyPoison =
        childP4 == 0xCDCDCDCDu || childP12 == 0xCDCDCDCDu ||
        (childP4 == 0 && childP12 == 0 && childVt != 0);
    const bool poisonVt =
        !readable || childVt == 0 || childVt == 0xCDCDCDCDu ||
        (childVt & 0xFFF00000u) != 0x82000000u || bodyPoison;
    // F-633: count by (obj,child) FIRST, so the flood is bounded without
    // losing the one fact the row exists to carry -- WHICH object, how many
    // times, from where. `hits == 2` is the moment this object is entered a
    // second time: that is the pathology (a node whose dtor is short-circuited
    // below is never unlinked, so its caller loop re-enters it forever), so it
    // gets its own named, bounded marker carrying the prior destroy's state
    // rather than one more identical row.
    bool isNew = false;
    Fb0d8Pair* slot = Fb0d8SlotFor(obj, child, &isNew);
    const uint32_t hits = slot
        ? slot->hits.fetch_add(1, std::memory_order_relaxed) + 1
        : g_fb0d8Untracked.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool reEnter = slot && hits == 2;
    if (slot)
    {
        if (isNew)
        {
            slot->firstN.store(n, std::memory_order_relaxed);
            slot->lastVt.store(childVt, std::memory_order_relaxed);
        }
        slot->lastN.store(n, std::memory_order_relaxed);
        slot->lastLr.store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
    }
    // The old gate's `|| poisonVt` term is what let a STABLY-poisoned pair
    // print 270,216 times; the new rule keeps the full row for a new pair and
    // drops the recurring one to the house cadence. `suppressed` keeps the
    // withheld rows countable: printed + suppressed == s_hFB0D8.
    const bool oldGate = (n <= 12 || poisonVt);
    const bool printRow = isNew || n <= 12 || (n % 200) == 0;
    if (oldGate && !printRow)
        g_fb0d8Suppressed.fetch_add(1, std::memory_order_relaxed);
    if (printRow)
        MCLA_LOG_WARN("FB0D8-DTOR #{:03} obj={:08X} child={:08X} childVt={:08X} "
                      "+4={:08X} +12={:08X} lr={:08X} hits={} suppressed={}{}",
                      n, obj, child, childVt, childP4, childP12,
                      static_cast<uint32_t>(ctx.lr), hits,
                      g_fb0d8Suppressed.load(std::memory_order_relaxed),
                      poisonVt ? " POISON-SKIP" : "");
    // slot != nullptr is REQUIRED here, not incidental: the (n % 200) arm can
    // fire on a call that found no free slot. Those are still counted, in
    // g_fb0d8Untracked, which FB0D8-TALLY prints.
    if (slot && (reEnter || (n % 200) == 0))
        MCLA_LOG_WARN("FB0D8-REPEAT #{:06} obj={:08X} child={:08X} hits={} "
                      "first=#{:06} priorVt={:08X} priorLr={:08X} nowLr={:08X} "
                      "dtorSkipped={} span_calls={}",
                      n, obj, child, hits,
                      slot->firstN.load(std::memory_order_relaxed),
                      slot->lastVt.load(std::memory_order_relaxed),
                      slot->lastLr.load(std::memory_order_relaxed),
                      static_cast<uint32_t>(ctx.lr), poisonVt ? 1 : 0,
                      n - slot->firstN.load(std::memory_order_relaxed));
    if ((n % 2000) == 0)
    {
        uint32_t topHits = 0;
        for (Fb0d8Pair& s : g_fb0d8)
        {
            const uint32_t h = s.hits.load(std::memory_order_relaxed);
            if (h > topHits)
                topHits = h;
        }
        // pairs=1 with topHits=n is the "one object, one child" signature.
        MCLA_LOG_WARN("FB0D8-TALLY n={:06} pairs={} untracked={} suppressed={} "
                      "topHits={} peak={}",
                      n, g_fb0d8Pairs.load(std::memory_order_relaxed),
                      g_fb0d8Untracked.load(std::memory_order_relaxed),
                      g_fb0d8Suppressed.load(std::memory_order_relaxed),
                      topHits, topHits == n ? 1 : 0);
    }
    if (poisonVt)
        return;
    __imp__sub_825FB0D8(ctx, base);
}

// ---------------------------------------------------------------------------
// w1a: sub_825EF100 = PLACE pass. Calls FDBF8 (build), sets vt 0x82085364,
// rebases [obj+24], walks entries via 825FDA90. Runs BEFORE job #2 fills
// B7B41000 (still CDCD). w1a tried deferring the whole call â€” that broke
// the boot because the vtable write is load-bearing for FB0D8-DTOR and the
// job #2 enqueue path. Census-only hook: log entry, pass through.
// ---------------------------------------------------------------------------
PPC_FUNC_IMPL(__imp__sub_825EF100);
static std::atomic<uint32_t> s_hEF100{0};

PPC_FUNC(sub_825EF100) { if (MclaPureVm()) { __imp__sub_825EF100(ctx, base); return; }
    const uint32_t n = s_hEF100.fetch_add(1) + 1;
    const uint32_t obj = ctx.r3.u32;
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    uint32_t vt = 0, arr = 0, arr0 = 0;
    uint16_t cnt = 0;
    if (obj && obj != 0xCDCDCDCDu)
    {
        (void)mem.ReadU32BE(obj, &vt);
        (void)mem.ReadU32BE(obj + 12, &arr);
        (void)mem.ReadU16BE(obj + 16, &cnt);
        if (arr != 0 && arr != 0xCDCDCDCDu && arr != 0xFFFFFFFFu)
            (void)mem.ReadU32BE(arr, &arr0);
    }
    // F-806 (2026-10-21): CENSUS THE GUEST'S OWN REBASE TABLE. The guest's
    // place pass resolves every virtual through the group it is handed
    // (ctx.r4 -> [ctx.r4+0] = the table). The host's own re-dispatch uses a
    // SYNTHETIC table (src/gpu_device.cpp MclaPublishGuestRebaseTable) whose
    // 0x50000000 region covers only the job's DECLARED stream chunk, while the
    // guest's table is a different band -- and that band is the missing fact for
    // the place pass (the container's DIU points into it). Layout MEASURED from
    // the raw bytes of sub_8217D828 (tools/ppc_disasm.py 0x8217D828):
    // table+0 u16 countA, table+2 u16 countB, table+4+i*12 {vbase, pbase, size}.
    if (n <= 8)
    {
        const uint32_t grp = ctx.r4.u32;
        uint32_t tbl = 0;
        if (grp != 0 && grp != 0xCDCDCDCDu)
            (void)mem.ReadU32BE(grp + 0, &tbl);
        if (tbl != 0 && tbl != 0xCDCDCDCDu)
        {
            uint16_t cA = 0, cB = 0;
            uint32_t e0v = 0, e0p = 0, e0s = 0;
            uint32_t e1v = 0, e1p = 0, e1s = 0;
            (void)mem.ReadU16BE(tbl + 0, &cA);
            (void)mem.ReadU16BE(tbl + 2, &cB);
            (void)mem.ReadU32BE(tbl + 4, &e0v);
            (void)mem.ReadU32BE(tbl + 8, &e0p);
            (void)mem.ReadU32BE(tbl + 12, &e0s);
            (void)mem.ReadU32BE(tbl + 16, &e1v);
            (void)mem.ReadU32BE(tbl + 20, &e1p);
            (void)mem.ReadU32BE(tbl + 24, &e1s);
            MCLA_LOG_WARN("EF100-REBASE-TBL #{:03} grp={:08X} tbl={:08X} "
                          "countA={} countB={} e0=[{:08X} {:08X} {:08X}] "
                          "e1=[{:08X} {:08X} {:08X}]",
                          n, grp, tbl, cA, cB, e0v, e0p, e0s, e1v, e1p, e1s);
        }
    }
    // F-563: Pre-populate CDCD containers at B7921000/B7B41000 before EF100
    // runs. These objects are allocated but never filled by the streamer
    // before the PLACE pass executes. Writing a minimal valid empty swfC
    // structure prevents the fatal in sub_825FDA90 (swfOBJECT::Fixup) and
    // lets the boot proceed. The guest's own FDBF8 will overwrite [obj+0]
    // with the real vtable (82085364).
    // F-633: the +4=0 / +12=0 written below are read back forever after as
    // "poison" by sub_825FB0D8's bodyPoison guard (see the header on that
    // hook) and livelock sub_825EDC08 on obj=CB139580 -- measured 270,216
    // re-entries in w658.log. An empty container is not poison; that guard's
    // third clause is the defect. NOT changed here (do-not #1).
    const bool isCdcdTarget = (obj == 0xB7921000u || obj == 0xB7B41000u);
    const bool needsPrepop = isCdcdTarget && (vt == 0xCDCDCDCDu);
    if (needsPrepop)
    {
        // Write minimal valid empty swfC container:
        // [obj+0]  = 0 (vtable placeholder; FDBF8 overwrites with 82085364)
        // [obj+4]  = 0
        // [obj+8]  = 0
        // [obj+12] = 0 (child array ptr = NULL)
        // [obj+16] = 0 (child count = 0, u16)
        // [obj+24] = 0 (entry array ptr = NULL)
        // [obj+50] = 1 (entry count = 1; skips the walk loop which checks >1)
        (void)mem.WriteU32BE(obj + 0, 0);
        (void)mem.WriteU32BE(obj + 4, 0);
        (void)mem.WriteU32BE(obj + 8, 0);
        (void)mem.WriteU32BE(obj + 12, 0);
        (void)mem.WriteU16BE(obj + 16, 0);
        (void)mem.WriteU32BE(obj + 24, 0);
        (void)mem.WriteU16BE(obj + 50, 1);
        MCLA_LOG_ERROR("TEMP-EF100-PREPOP #{:03} obj={:08X} â€” wrote empty swfC "
                       "(vt=0 arr=0 cnt=0 +24=0 +50=1) to prevent CDCD fatal",
                       n, obj);
        // Re-read after pre-population so the rest of the hook sees valid data
        vt = 0; arr = 0; arr0 = 0; cnt = 1;
        (void)mem.ReadU32BE(obj, &vt);
        (void)mem.ReadU32BE(obj + 12, &arr);
        (void)mem.ReadU16BE(obj + 16, &cnt);
    }
    const bool rsc =
        (vt & 0xFFFFFF00u) == 0x44495500u ||
        (vt & 0xFFFFFF00u) == 0x44365500u || (vt & 0xFFFFu) == 0x5500u;
    // w24: complete 0x50/0x60 rebase + census on RSC heads BEFORE the guest
    // place body walks them (early EF100, deferred place, nested re-dispatch).
    if (rsc || (g_mclaRebaseCount > 0 && n <= 8))
        MclaCompleteRscRebaseBeforePlace(obj);
    // Gate the guest entry-walk bound when it is RSC residue (w23/w24:
    // +50 read as 0x00A4 â†’ 164 junk iterations; [arr]=533D00CB gate-bad).
    // Cap to 1 so 825EF100 still runs FDBF8 + writes the real swfC vtable
    // then returns â€” never invent a vtable.
    if (rsc && obj && obj != 0xCDCDCDCDu)
    {
        uint16_t cnt50 = 0;
        uint32_t arr50 = 0, arr50c = 0;
        (void)mem.ReadU16BE(obj + 50, &cnt50);
        (void)mem.ReadU32BE(obj + 24, &arr50);
        if (arr50 != 0 && arr50 != 0xCDCDCDCDu && arr50 != 0xFFFFFFFFu)
            (void)mem.ReadU32BE(arr50, &arr50c);
        const bool gateWalk =
            cnt50 > 1 && (MclaPtrIsGateBad(arr0) || MclaPtrIsGateBad(arr) ||
                          MclaPtrIsGateBad(arr50c) || arr0 == 0xCDCDCDCDu);
        MCLA_LOG_WARN("EF100-WALK #{:03} obj={:08X} +24={:08X} +50={} "
                      "arr={:08X} [arr]={:08X} [+24]={:08X} gateWalk={}",
                      n, obj, arr50, cnt50, arr, arr0, arr50c,
                      gateWalk ? 1 : 0);
        if (gateWalk)
        {
            (void)mem.WriteU16BE(obj + 50, 1);
            MCLA_LOG_WARN("EF100-CNT-GATE #{:03} obj={:08X} +50 {} -> 1 "
                          "(RSC-residue walk bound; place still constructs)",
                          n, obj, cnt50);
        }
    }
    if (n <= 16 || rsc || (n % 200) == 0) {
        MCLA_LOG_ERROR("TEMP-EF100 #{:03} obj={:08X} r4={:08X} vt={:08X} "
                       "arr={:08X} [arr]={:08X} cnt={} lr={:08X}{}{}",
                       n, obj, ctx.r4.u32, vt, arr, arr0, cnt,
                       static_cast<uint32_t>(ctx.lr),
                       rsc ? " RSC-44495500" : "",
                       MclaPtrIsGateBad(arr0) ? " ARR-GATE" : "");
    }
    // w32: census-proven gate â€” guest walk is `if (u16[obj+50] <= 1) skip`.
    // After fill, +24 holds dest-range/SWF-tag candidates while +50 stays 1
    // (host CNT-GATE looked only at [+24][0]). Open the bound in-place so
    // THIS EF100 call runs FDA90 on +24[1..]. Never invent tags/vtables.
    if (obj && obj != 0xCDCDCDCDu)
        (void)MclaW32OpenConstructWalk(obj, 0, /*redispatch=*/false);
    __imp__sub_825EF100(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_823043F8);
PPC_FUNC_IMPL(__imp__sub_82304708);
static std::atomic<uint32_t> s_hGfxTick{0};
static std::atomic<uint32_t> s_hGfxGateInstall{0};
PPC_FUNC(sub_823043F8) { if (MclaPureVm()) { __imp__sub_823043F8(ctx, base); return; }
    const uint32_t n = s_hGfxTick.fetch_add(1) + 1;
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    const uint32_t obj = ctx.r3.u32;
    uint32_t pre68 = 0, pre72 = 0, post68 = 0, post72 = 0;
    // F-875: +68 is the REQUEST, +72 the FRAME COUNTDOWN the gate at 0x822C09C0
    // tests. On the first call arm a write watch over obj+64..obj+80 so the
    // PAGEWATCH W rows (src/gpu_cp.cpp:8611) name WHO posts the request, with
    // the guest LR. Rule 4: addr_owners --check 0x823043F8 = 1 site (the
    // generated function map) -- this hook is the only owner. Log-only.
    if (n == 1u && obj != 0u)
        mcla::native::RegisterGuestWatchRange(obj + 64u, obj + 80u);
    if (obj != 0u) {
        (void)mem.ReadU32BE(obj + 68u, &pre68);
        (void)mem.ReadU32BE(obj + 72u, &pre72);
    }
    __imp__sub_823043F8(ctx, base);
    if (obj != 0u) {
        (void)mem.ReadU32BE(obj + 68u, &post68);
        (void)mem.ReadU32BE(obj + 72u, &post72);
    }
    if (n <= 8 || (n % 500) == 0)
        MCLA_LOG_WARN("F875-GFX-TICK #{} obj={:08X} pre68={:08X} pre72={:08X} "
                      "post68={:08X} post72={:08X} lr={:08X}",
                      n, obj, pre68, pre72, post68, post72,
                      static_cast<uint32_t>(ctx.lr));
}

// F-878 (2026-10-23): hook on the gate object installer sub_82304708.
// After the object is constructed (84 bytes, vtable 0x82041D58, +68=+72=0),
// it installs mem[0x8288B9AC] = obj and calls sub_823041E8.
// We write +68=5 and +72=5 to arm the countdown immediately, since the
// front-end state machine (sub_8263EFC0/FA10/C8B0) never reaches its
// poster sites (measured: F875-GFX-TICK sees pre68=0 always).
// This lets the gate at 0x822C09C0 pass and GFx init proceed.
// FIX (2026-10-24): the return value is not the gate object. Read mem[0x8288B9AC]
// directly after the call, since that's where the installer stores the gate object.
PPC_FUNC(sub_82304708) { if (MclaPureVm()) { __imp__sub_82304708(ctx, base); return; }
    const uint32_t n = s_hGfxGateInstall.fetch_add(1) + 1;
    auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
    // Call the real installer first -- it allocates, zeroes +68/+72, installs gate.
    __imp__sub_82304708(ctx, base);
    // After return, the gate object is at mem[0x8288B9AC]. Read it directly.
    uint32_t obj = 0;
    (void)mem.ReadU32BE(0x8288B9ACu, &obj);
    // Verify and write +68=5, +72=5 to arm the countdown.
    if (obj != 0u) {
        (void)mem.WriteU32BE(obj + 68u, 5u);
        (void)mem.WriteU32BE(obj + 72u, 5u);
        if (n <= 8)
            MCLA_LOG_WARN("F878-GFX-GATE-INSTALL #{} obj={:08X} wrote +68=5 +72=5 lr={:08X}",
                          n, obj, static_cast<uint32_t>(ctx.lr));
    } else {
        if (n <= 8)
            MCLA_LOG_WARN("F878-GFX-GATE-INSTALL #{} obj=0 (alloc failed?) lr={:08X}",
                          n, static_cast<uint32_t>(ctx.lr));
    }
}

extern std::atomic<uint32_t> g_step1Tag;
extern std::atomic<uint32_t> g_step1Handle;

PPC_FUNC_IMPL(__imp__sub_821BD600);
PPC_FUNC(sub_821BD600) { if (MclaPureVm()) { __imp__sub_821BD600(ctx, base); return; }
    const uint32_t tag = ctx.r3.u32;
    const uint32_t handle = ctx.r7.u32;
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    if (lr == 0x82138628 || (lr >= 0x82138410 && lr <= 0x82138690)) {
        g_step1Tag.store(tag, std::memory_order_relaxed);
        g_step1Handle.store(handle, std::memory_order_relaxed);
        MCLA_LOG_ERROR("STEP1 sub_82138410 calling sub_821BD600: tag={:08X} handle={:08X} lr={:08X}",
                       tag, handle, lr);
    }
    __imp__sub_821BD600(ctx, base);
}

// ===========================================================================
// F-849 (2026-10-21) -- THE REGISTER-INDIRECT DISPATCH CENSUS.
//
// This is the F-635 census' EXTENSION on the SAME seam (src/guest_memory.h:
// the PPC_CALL_INDIRECT_FUNC override). It is log-only: it counts, it
// classifies, it prints. The only behavioural part is the Begin/End bracket,
// which does not touch the guest.
//
// WHAT IT MEASURES, PER DISPATCH:
//   * the CALL TARGET (the `ctr`),
//   * the LR of the site,
//   * the CLASS of the target:
//       GUESTFN    -- the table slot is non-null and the target is inside the
//                     recompiled code range [0x82130000, 0x827CCBC0], so the
//                     call REACHES THE GUEST'S OWN CODE.
//       HOSTSTUB   -- the table slot is non-null and the target is outside that
//                     range (a host-registered stub: 0x83000000 press-start,
//                     0x8F204000 __xtl_alloc, 0x8F204010 __xtl_free).
//       EMPTY-SLOT -- the table slot is NULL. The host CALLS IT ANYWAY: the
//                     F-635 guard tests the INDEX RANGE, not the slot.
//
// AND, FOR THE CALL THAT NEVER RETURNS:
//   * `BCTR-STUCK`  -- a dispatch whose slot has been ACTIVE for > 2 s. The
//     call has not returned. This row NAMES IT.
//   * `BCTR-LAST`   -- the last dispatch a thread made before it went quiet.
//     This covers the guest's SELF-LOOP traps, where the guest leaves through
//     a `b` to itself and never re-enters this seam, so no bracket closes.
//
// RULE 19: the census has to be able to fail, so it prints a row per dispatch
// (bounded) and a summary row; a zero is a zero, not a blind instrument.
//
// The slot arithmetic is the SAME expression as PPC_LOOKUP_FUNC
// (generated/ppc_xenon/ppc_context.h:127):
//     base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (ctr - PPC_CODE_BASE) * 2
// ===========================================================================

namespace mcla::native::census {

namespace {

constexpr int kIndirectSlotMax = 64;

struct IndirectSlot {
    std::atomic<uint32_t> tid{0};
    std::atomic<uint32_t> ctr{0};
    std::atomic<uint32_t> lr{0};
    std::atomic<uint32_t> cls{0};
    std::atomic<uint64_t> startNs{0};
    std::atomic<uint64_t> lastNs{0};
    std::atomic<uint32_t> active{0};
    std::atomic<uint32_t> stuckReported{0};
    std::atomic<uint32_t> idleReported{0};
    // F-849: the SPIN detector. A thread that is NOT blocked can still be
    // stuck -- in a guest loop that keeps issuing register-indirect calls and
    // never leaves. The boot worker's blocker is exactly that, and neither
    // BCTR-STUCK (a call that never returns) nor BCTR-LAST (a thread that goes
    // quiet) can see it. The ring of recent call SITES can: a thread whose
    // last 64 dispatches came from 8 or fewer distinct sites is spinning.
    static constexpr int kLrRing = 64;
    std::atomic<uint32_t> lrRing[kLrRing];
    std::atomic<uint32_t> lrRingPos{0};
    std::atomic<uint64_t> dispCount{0};
    std::atomic<uint64_t> prevTickCount{0};
    std::atomic<uint32_t> spinReported{0};
};

IndirectSlot g_indirectSlots[kIndirectSlotMax];
std::atomic<uint32_t> g_indirectSlotCursor{0};

std::atomic<uint64_t> g_dispTotal{0};
std::atomic<uint64_t> g_dispGuestFn{0};
std::atomic<uint64_t> g_dispHostStub{0};
std::atomic<uint64_t> g_dispEmptySlot{0};
std::atomic<uint64_t> g_emptyRows{0};
std::atomic<uint64_t> g_stuckRows{0};
std::atomic<uint64_t> g_idleRows{0};
std::atomic<uint64_t> g_spinRows{0};

// LONG-TERM DISPATCH-OVERHEAD SAMPLER (20+ min safe): every dispatch is counted
// with two relaxed atomics + the startNs already stored by IndirectCallBegin
// (no extra chrono call). Printing is gated by WALL clock (~1 row / 60 s) so
// a 280k dispatches/s spin (see ScanIndirectSlots comment) costs ~20 rows in
// 20 min instead of ~82k at kPrintStride=4096, and a 60 s boot costs ~1 row.
// overhead_pct = host ns spent in Begin..End / wall ns in the window: the
// direct answer to "tracer cost vs boot time". Counts are best-effort under
// multi-thread rollover (one window boundary may trade <2 dispatches).
std::atomic<uint64_t> g_ohSumNs{0};
std::atomic<uint64_t> g_ohMaxNs{0};
std::atomic<uint64_t> g_ohWindowSumNs{0};
std::atomic<uint64_t> g_ohWindowN{0};
std::atomic<uint64_t> g_ohWindowStartNs{0};
std::atomic<uint64_t> g_censusBootNs{0};
constexpr uint64_t kOverheadWindowNs = 60000000000ull; // 60 s

// A dispatch is "stuck" after 2 s; a thread is "quiet" after 15 s.
constexpr uint64_t kStuckNs = 2000000000ull;
constexpr uint64_t kIdleNs = 15000000000ull;
// Print policy: the first N rows of the whole census verbatim, then every
// kPrintStride'th, plus EVERY EMPTY-SLOT row up to a cap (that class is the
// one that kills the process, so it is never sampled away).
constexpr uint64_t kPrintHead = 64;
constexpr uint64_t kPrintStride = 4096;
constexpr uint64_t kEmptyRowCap = 256;
constexpr uint64_t kIdleRowCap = 256;

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

const char *ClassName(uint32_t cls) {
    switch (static_cast<IndirectTargetClass>(cls)) {
    case IndirectTargetClass::GuestFn:
        return "GUESTFN";
    case IndirectTargetClass::HostStub:
        return "HOSTSTUB";
    default:
        return "EMPTY-SLOT";
    }
}

// One slot per guest thread, round-robin. With more than 64 guest threads two
// of them share a slot; that is stated, not hidden.
int SlotForThisThread() {
    static thread_local int s_idx = -1;
    if (s_idx >= 0) {
        return s_idx;
    }
    const uint32_t cur =
        g_indirectSlotCursor.fetch_add(1) % (uint32_t)kIndirectSlotMax;
    s_idx = (int)cur;
    IndirectSlot &s = g_indirectSlots[s_idx];
    s.tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    s.stuckReported.store(0, std::memory_order_relaxed);
    s.idleReported.store(0, std::memory_order_relaxed);
    s.spinReported.store(0, std::memory_order_relaxed);
    s.lrRingPos.store(0, std::memory_order_relaxed);
    s.dispCount.store(0, std::memory_order_relaxed);
    s.prevTickCount.store(0, std::memory_order_relaxed);
    for (int k = 0; k < IndirectSlot::kLrRing; ++k)
        s.lrRing[k].store(0, std::memory_order_relaxed);
    return s_idx;
}

void ScanIndirectSlots() {
    const uint64_t now = NowNs();
    for (int i = 0; i < kIndirectSlotMax; ++i) {
        IndirectSlot &s = g_indirectSlots[i];
        const uint32_t tid = s.tid.load(std::memory_order_relaxed);
        if (tid == 0) {
            continue;
        }
        // ---- THE SPIN DETECTOR (F-849) ----------------------------------
        // A thread that keeps dispatching but from only a handful of sites is
        // looping, not working. Measured on the boot worker: 3 distinct call
        // sites, ~280,000 dispatches/second, forever.
        {
            const uint64_t cnt = s.dispCount.load(std::memory_order_relaxed);
            const uint64_t prev = s.prevTickCount.exchange(
                cnt, std::memory_order_relaxed);
            const uint64_t delta = (cnt >= prev) ? (cnt - prev) : 0;
            if (delta <= 5000) {
                // the thread is not spinning any more -- re-arm the row.
                s.spinReported.store(0, std::memory_order_relaxed);
            }
            if (delta > 5000) {
                uint32_t seen[IndirectSlot::kLrRing];
                int nseen = 0;
                for (int k = 0; k < IndirectSlot::kLrRing; ++k) {
                    const uint32_t l =
                        s.lrRing[k].load(std::memory_order_relaxed);
                    if (l == 0) {
                        continue;
                    }
                    bool found = false;
                    for (int j = 0; j < nseen; ++j) {
                        if (seen[j] == l) {
                            found = true;
                            break;
                        }
                    }
                    if (!found && nseen < (int)IndirectSlot::kLrRing) {
                        seen[nseen++] = l;
                    }
                }
                if (nseen > 0 && nseen <= 8 && !s.spinReported.exchange(1)) {
                    const uint64_t n = g_spinRows.fetch_add(1) + 1;
                    uint8_t abortFlag = 0;
                    (void)mcla::kernel::GuestMemoryHeap::Instance().ReadU8(
                        0x8286073Cu, &abortFlag);
                    MCLA_LOG_WARN(
                        "BCTR-SPIN #{} tid={:08X} dispatches_in_500ms={} "
                        "distinct_call_sites={} abortFlag@8286073C={} "
                        "sites=[{}]",
                        n, tid, delta, nseen, static_cast<uint32_t>(abortFlag),
                        [&] {
                            std::string s2;
                            for (int j = 0; j < nseen; ++j) {
                                char buf[16];
                                std::snprintf(buf, sizeof(buf), "%s%08X",
                                              j ? "," : "", seen[j]);
                                s2 += buf;
                            }
                            return s2;
                        }());
                }
            }
        }
        // ---- THE ABORT FLAG (F-849) --------------------------------------
        // The guest's own loop back-edge at 0x821CC3C0 (`bne cr6 ->
        // 0x821CC350`, raw) is gated on the byte at guest 0x8286073C: it loops
        // WHILE the byte is non-zero. The host's only writer that clears it
        // is src/gpu_device.cpp:21049-21050 (the READWRAP r7==0 inflate-refill
        // arm). So the byte's value is a first-class fact, and it is printed
        // on every change and with every spin row.
        {
            static uint32_t s_lastAbort = 0xFFFFFFFFu;
            uint8_t abortFlag = 0;
            if (mcla::kernel::GuestMemoryHeap::Instance().ReadU8(0x8286073Cu,
                                                                &abortFlag)) {
                if (abortFlag != s_lastAbort) {
                    s_lastAbort = abortFlag;
                    MCLA_LOG_WARN("ABORT-FLAG @8286073C = {} (CHANGED)",
                                  static_cast<uint32_t>(abortFlag));
                }
            }
        }
        if (s.active.load(std::memory_order_acquire)) {
            const uint64_t start = s.startNs.load(std::memory_order_relaxed);
            if (start != 0 && now > start && (now - start) > kStuckNs) {
                if (!s.stuckReported.exchange(1)) {
                    const uint64_t n = g_stuckRows.fetch_add(1) + 1;
                    MCLA_LOG_WARN(
                        "BCTR-STUCK #{} tid={:08X} ctr={:08X} lr={:08X} class={} "
                        "age_ms={} -- THIS INDIRECT CALL NEVER RETURNED (the "
                        "bracket is still open)",
                        n, tid, s.ctr.load(std::memory_order_relaxed),
                        s.lr.load(std::memory_order_relaxed),
                        ClassName(s.cls.load(std::memory_order_relaxed)),
                        (now - start) / 1000000ull);
                    // F-914 STALL-TATTLE (docs/analysis-20261028/launch-too-slow.md
                    // Fix 1, instrumentation only -- no behaviour change): the
                    // BCTR-STUCK row above names ONE stuck pair; the launch-stall
                    // claim is that the WHOLE worker population sits parked at once
                    // behind the guest's boot-gate busy word. This row dumps the
                    // two facts that decide it, once per process, the first time a
                    // slot crosses the 2 s gate: the busy-tattle word at guest
                    // 0x8288B9E4 (*(r13)+72 -- boot-gate.md:188/:227 measured the
                    // chain as "if >0 -> sets *(r13)+72 = 1") and the live
                    // ctr/lr of every slot holding an open indirect bracket.
                    // PREDICTION (launch-too-slow.md Fix 1): raw>0 for the whole
                    // 13.6-13.8 s window and openBrackets reaches 8.
                    // FALSIFIER: raw=0 on this row, or openBrackets < 8 when the
                    // storm window opens.
                    static std::atomic<uint32_t> s_stallTattle{0};
                    if (!s_stallTattle.exchange(1, std::memory_order_relaxed)) {
                        auto &gmem = mcla::kernel::GuestMemoryHeap::Instance();
                        uint32_t tattle = 0;
                        const int tattleOk = gmem.ReadU32BE(0x8288B9E4u, &tattle) ? 1 : 0;
                        char parkBuf[512] = {};
                        int pos = 0;
                        int open = 0;
                        for (int k = 0; k < kIndirectSlotMax; ++k) {
                            const IndirectSlot &sl = g_indirectSlots[k];
                            const uint32_t slTid =
                                sl.tid.load(std::memory_order_relaxed);
                            if (slTid == 0 ||
                                sl.active.load(std::memory_order_relaxed) == 0u) {
                                continue;
                            }
                            if (pos < 0 || pos >= (int)sizeof(parkBuf) - 28) {
                                break;
                            }
                            const int w = std::snprintf(
                                parkBuf + pos, sizeof(parkBuf) - (size_t)pos,
                                "%s%08X:%08X/%08X", open ? "," : "", slTid,
                                sl.ctr.load(std::memory_order_relaxed),
                                sl.lr.load(std::memory_order_relaxed));
                            if (w < 0) {
                                break;
                            }
                            pos += w;
                            ++open;
                        }
                        MCLA_LOG_WARN(
                            "STALL-TATTLE #1 tattleVA=8288B9E4 raw={:08X} "
                            "read_ok={} openBrackets={} parks=[{}] -- busy-word + "
                            "every slot with an open indirect bracket at the first "
                            "2 s BCTR-STUCK",
                            tattle, tattleOk, open, parkBuf);
                    }
                }
            }
        } else {
            const uint64_t last = s.lastNs.load(std::memory_order_relaxed);
            if (last != 0 && now > last && (now - last) > kIdleNs) {
                if (!s.idleReported.exchange(1)) {
                    const uint64_t n = g_idleRows.fetch_add(1) + 1;
                    if (n <= kIdleRowCap) {
                        MCLA_LOG_WARN(
                            "BCTR-LAST #{} tid={:08X} ctr={:08X} lr={:08X} "
                            "class={} quiet_ms={} -- LAST INDIRECT DISPATCH THIS "
                            "THREAD MADE (a guest self-loop never re-enters this "
                            "seam, so its bracket cannot close)",
                            n, tid, s.ctr.load(std::memory_order_relaxed),
                            s.lr.load(std::memory_order_relaxed),
                            ClassName(s.cls.load(std::memory_order_relaxed)),
                            (now - last) / 1000000ull);
                        // TEMP-STUCK-STACK: host frames of the quiet thread (RVAs for
                        // tools/host_rva_resolve.py build/mcla.map), so a blocked wait names its chain.
                        if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid)) {
                            std::string fr;
                            if (SuspendThread(h) != (DWORD)-1) {
                                CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
                                if (GetThreadContext(h, &c)) {
                                    const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
                                    for (int k = 0; k < 32 && c.Rip; ++k) {
                                        fr += fmt::format("{:X} ", c.Rip - (c.Rip >= base ? base : 0));
                                        DWORD64 ib = 0;
                                        auto *fe = RtlLookupFunctionEntry(c.Rip, &ib, nullptr);
                                        if (!fe) { c.Rip = *reinterpret_cast<DWORD64 *>(c.Rsp); c.Rsp += 8; continue; }
                                        void *hd = nullptr; DWORD64 ef = 0;
                                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, &hd, &ef, nullptr);
                                    }
                                }
                                ResumeThread(h);
                            }
                            CloseHandle(h);
                            MCLA_LOG_WARN("TEMP-STUCK-STACK tid={:08X} rvas={}", tid, fr);
                        }
                    }
                }
            }
        }
    }
}

void EnsureIndirectWatchdog() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] { mcla::diag::NameThisThread("tdt-scan");
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                ScanIndirectSlots();
            }
        }).detach();
    });
}

}  // namespace

void ReportIndirectDispatch(uint32_t ctr, uint32_t lr, uintptr_t base) {
    const uint64_t total = g_dispTotal.fetch_add(1) + 1;

    // The slot this target selects -- the SAME arithmetic as PPC_LOOKUP_FUNC
    // (generated/ppc_xenon/ppc_context.h:127). The macro only reaches this
    // function for an IN-RANGE ctr, so the slot address is inside the flat
    // guest window and the read is safe.
    const uintptr_t slotAddr =
        base + kFuncTableGuestVa + (uint64_t(uint32_t(ctr) - kCtrLo) * 2);
    const void *fn = nullptr;
    std::memcpy(&fn, reinterpret_cast<const void *>(slotAddr), sizeof(fn));

    uint32_t cls;
    if (fn == nullptr) {
        cls = static_cast<uint32_t>(IndirectTargetClass::EmptySlot);
        g_dispEmptySlot.fetch_add(1);
    } else if (ctr >= kRecompiledCodeLo && ctr <= kRecompiledCodeHi) {
        cls = static_cast<uint32_t>(IndirectTargetClass::GuestFn);
        g_dispGuestFn.fetch_add(1);
    } else {
        cls = static_cast<uint32_t>(IndirectTargetClass::HostStub);
        g_dispHostStub.fetch_add(1);
    }

    IndirectSlot &s = g_indirectSlots[SlotForThisThread()];
    s.cls.store(cls, std::memory_order_relaxed);

    const bool empty =
        (cls == static_cast<uint32_t>(IndirectTargetClass::EmptySlot));
    const uint64_t emptyRow = empty ? g_emptyRows.fetch_add(1) + 1 : 0;
    const bool printEmpty = empty && emptyRow <= kEmptyRowCap;
    const bool printSample =
        (total <= kPrintHead) || ((total % kPrintStride) == 0);
    if (printEmpty || printSample) {
        MCLA_LOG_WARN(
            "BCTR-DISP #{} tid={:08X} ctr={:08X} lr={:08X} class={} slot={:08X} "
            "totals[guestfn={} hoststub={} emptyslot={}]",
            total, GetCurrentThreadId(), ctr, lr, ClassName(cls),
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn)),
            g_dispGuestFn.load(std::memory_order_relaxed),
            g_dispHostStub.load(std::memory_order_relaxed),
            g_dispEmptySlot.load(std::memory_order_relaxed));
    }

    EnsureIndirectWatchdog();
}

void IndirectCallBegin(uint32_t ctr, uint32_t lr) {
    IndirectSlot &s = g_indirectSlots[SlotForThisThread()];
    s.ctr.store(ctr, std::memory_order_relaxed);
    s.lr.store(lr, std::memory_order_relaxed);
    s.startNs.store(NowNs(), std::memory_order_relaxed);
    s.stuckReported.store(0, std::memory_order_relaxed);
    s.idleReported.store(0, std::memory_order_relaxed);
    s.active.store(1, std::memory_order_release);
    // The SPIN detector's ring of recent call sites.
    const uint32_t pos =
        s.lrRingPos.fetch_add(1, std::memory_order_relaxed) %
        IndirectSlot::kLrRing;
    s.lrRing[pos].store(lr, std::memory_order_relaxed);
    s.dispCount.fetch_add(1, std::memory_order_relaxed);
}

void IndirectCallEnd() {
    IndirectSlot &s = g_indirectSlots[SlotForThisThread()];
    const uint64_t endNs = NowNs();
    const uint64_t startNs = s.startNs.load(std::memory_order_relaxed);
    s.active.store(0, std::memory_order_release);
    s.lastNs.store(endNs, std::memory_order_relaxed);
    if (startNs != 0 && endNs >= startNs) {
        const uint64_t durNs = endNs - startNs;
        g_ohSumNs.fetch_add(durNs, std::memory_order_relaxed);
        g_ohWindowSumNs.fetch_add(durNs, std::memory_order_relaxed);
        g_ohWindowN.fetch_add(1, std::memory_order_relaxed);
        {
            uint64_t m = g_ohMaxNs.load(std::memory_order_relaxed);
            while (durNs > m &&
                   !g_ohMaxNs.compare_exchange_weak(
                       m, durNs, std::memory_order_relaxed))
                ;
        }
        {
            uint64_t expect = 0;
            if (g_censusBootNs.compare_exchange_strong(
                    expect, endNs, std::memory_order_relaxed))
                g_ohWindowStartNs.store(endNs, std::memory_order_relaxed);
        }
        const uint64_t winStart =
            g_ohWindowStartNs.load(std::memory_order_relaxed);
        if (winStart != 0 && endNs > winStart &&
            (endNs - winStart) >= kOverheadWindowNs) {
            uint64_t expect = winStart;
            if (g_ohWindowStartNs.compare_exchange_strong(
                    expect, endNs, std::memory_order_relaxed)) {
                const uint64_t winN = g_ohWindowN.exchange(0);
                const uint64_t winSum = g_ohWindowSumNs.exchange(0);
                const uint64_t winNs = endNs - winStart;
                if (winN != 0) {
                    const uint64_t bootNs =
                        g_censusBootNs.load(std::memory_order_relaxed);
                    const double bootS =
                        bootNs ? double(endNs - bootNs) / 1e9 : 0.0;
                    const double winS = double(winNs) / 1e9;
                    const double usPer = double(winSum) / double(winN) / 1e3;
                    const double rate = double(winN) / winS;
                    const double pct = double(winSum) / double(winNs) * 100.0;
                    const uint64_t totN = g_dispTotal.load(
                        std::memory_order_relaxed);
                    const uint64_t totSum =
                        g_ohSumNs.load(std::memory_order_relaxed);
                    const double totUs = totN ? double(totSum) / double(totN) / 1e3
                                              : 0.0;
                    MCLA_LOG_WARN(
                        "DISP-OVERHEAD boot_s={:.1f} window_s={:.1f} n={} "
                        "us_per_dispatch={:.3f} max_us={:.3f} "
                        "dispatches_per_s={:.1f} overhead_pct={:.3f} "
                        "total_n={} total_us_per_dispatch={:.3f}",
                        bootS, winS, winN, usPer,
                        double(g_ohMaxNs.load(std::memory_order_relaxed)) / 1e3,
                        rate, pct, totN, totUs);
                }
            }
        }
    }
}

}  // namespace mcla::native::census
