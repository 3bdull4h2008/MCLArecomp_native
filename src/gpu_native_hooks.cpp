// gpu_native_hooks.cpp -- native D3D-hook renderer for the guest's statically
// linked XDK D3D device layer (identification evidence: docs/GPU_NATIVE_HOOK_MAP.md).
// The hooks run the original guest body, then build/submit DRAW_CAPTURED from the
// device shadow. Native mode suppresses PM4 draw production; bounded GNH-* census
// rows remain available for diagnostics.
//
// Register layout follows the Xbox 360 PPC64 ABI: every argument owns one
// doubleword slot, a float goes in f1.. AND burns its GPR slot (Clear's Z in f1
// skips r8, measured at generated/ppc_xenon/ppc_recomp.77.cpp:10014-10018:
// `stw r7,212(r1)` = Color, `mr r27,r9` = Stencil, `fmr f31,f1` = Z).
//
// Device offsets used (all from decompiled setters, see the map doc):
//   +12436 IB            (SetIndices          sub_82415EE8)
//   +12440+4i RT[i]      (ClearF / Resolve read it)
//   +12456 depth-stencil (SetDepthStencil     sub_824169D0)
//   +12460+4i stream[i]  (SetStreamSource     sub_82415DC8)
//   +12692 PS, +12696 VS (sub_824179B8 / sub_82417C70)
//   +16560 swap counter  (Swap                sub_82419E98 increments it)

//
// GNH-P2 (docs/GPU_NATIVE_HOOK_MAP.md §5): in native mode the
// BeginVertices/EndVertices and DrawVertices hooks build the SAME
// DRAW_CAPTURED record the PM4 bridge can build (gpu_cp.cpp CpBridgeDraw), but
// from the guest's device shadow instead of PM4 packets. MCLA_RENDERER=pm4
// restores PM4 draw production; MCLA_RENDERER=both keeps the comparison path.

#include "generated/ppc_xenon/ppc_recomp_shared.h"
#include <cpu/ppc_context.h>
#include "logging.h"
#include "kernel/memory.h"  // needs PPCFunc/PPC_CODE_BASE from the two above
#include "kernel/heap_arena.h"  // kConsolePhysicalBytes
#include "boot_host.h"          // TranslatePhysViewToPoolVa (the ONE view->pool rule)
#include "gpu_cp.h"             // CpBuildShaderConstantBank (GNH-CMP only)
#include "render_queue.h"
#include "gpu_native/gnh_p2.h"
#include "common/cvar.h"  // Wave 3 (§1.6): live mcla_display_gamma read at swap

#include <set>
#include <windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcla::native { extern std::atomic<uint64_t> g_dcCapBytes; }
// Wave 3 (§1.6): defined in src/kernel/imports.cpp. Rule 1b: extern declaration
// only here; the definition stays in that one translation unit.
namespace mcla::gamma { extern std::atomic<uint32_t> g_appliedType; }
namespace mcla::gpu {
// gpu_cp.cpp (GNH-P2): the PM4 path's own vfetch walker over a supplied image.
uint32_t CpDecodeVertexElementsFromWords(const uint32_t *w, uint32_t dwords,
                                         mcla::native::CpVertexElement *out,
                                         uint32_t maxOut);
}

namespace {

// PERF 2026-11-17: BuildDraw issues ~150 of these per draw; the heap's IsValid+SEH ReadU32BE pair is replaced by the
// inline guest-view fast path (guest_memory.h). MCLA_NO_FASTMEM=1 falls back inside ReadGuestU32 to the checked view.
uint32_t Rd(uint32_t va) { return va != 0 ? mcla::native::ReadGuestU32(va) : 0u; }

// PERF 2026-11-17: the TEMP-* per-draw censuses below (global mutex + linear table scans + log rows on every draw) are
// diagnostics only; opt-in with MCLA_GNH_DIAG=1.
bool GnhDiag() {
  static const bool on = [] { const char *e = std::getenv("MCLA_GNH_DIAG"); return e && e[0] == '1'; }();
  return on;
}

float RdF(uint32_t va) {
  float v = 0.0f;
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  if (va != 0 && mem.IsValid(va, 4))
    (void)mem.ReadF32BE(va, &v);
  return v;
}

// first 16 firings, then every 1000th: a cadence, not a total (the GNH-SUM
// line printed on Swap carries the true totals).
bool Cadence(uint32_t n) { return n <= 16 || (n % 1000) == 0; }

std::atomic<uint32_t> s_clear{0}, s_clearF{0}, s_draw{0}, s_drawIdx{0},
    s_beginV{0}, s_swap{0};

} // namespace

// ===========================================================================
// GNH-P2 native draw producer
// ===========================================================================
namespace {

namespace rq = mcla::native;

// Device-shadow offsets (bytes from dev). Each derived from a decompile, cited
// in docs/GPU_NATIVE_HOOK_MAP.md §5.1.
constexpr uint32_t kDevFetch = 0x480;      // 32 x 24 B fetch constants (= regs 0x4800..)
// VS float constants c0..c255 (SetVertexShaderConstantF sub_82417678 writes
// dev+0x780+16*reg) and PS c0..c255 (sub_82417758: dev+0x1780+16*reg). The CP
// bank (SQ_VS_CONST=000FF000 base 0, SQ_PS_CONST=000FF100 base 256, on every
// CONST-BANK-PRODUCE row of build/gp1.log) indexes the same 512-entry file.
constexpr uint32_t kDevVsConst = 0x780, kDevPsConst = 0x1780;
constexpr uint32_t kDevVs = 12696, kDevPs = 12692;
// RB/PA shadow, register -> dev offset (blocks flushed by sub_82423548).
constexpr uint32_t kSnapOff[rq::kCpSnapRegCount] = {
    10564,  // 0x2204 PA_CL_CLIP_CNTL      (block 0x2200 @ 10548)
    10568,  // 0x2205 PA_SU_SC_MODE_CNTL
    10572,  // 0x2206 PA_CL_VTE_CNTL
    10832,  // 0x2380 poly offset front scale (block 0x2380 @ 10832)
    10836,  // 0x2381
    10840,  // 0x2382
    10844,  // 0x2383
    10376,  // 0x2002 RB_DEPTH_INFO        (block 0x2000 @ 10368)
    10372,  // 0x2001 RB_COLOR_INFO
    10552,  // 0x2201 RB_BLENDCONTROL0
    10460,  // 0x2104 RB_COLOR_MASK        (block 0x2100 @ 10444)
    10548,  // 0x2200 RB_DEPTHCONTROL
    10556,  // 0x2202 RB_COLORCONTROL      (block 0x2200 @ 10548, +2 dwords)
    10500,  // 0x210E RB_ALPHA_REF         (block 0x2100 @ 10444, +14 dwords)
    10504,  // 0x210F PA_CL_VPORT_XSCALE   (vp120 TEMP-VPORT-CENSUS: 640/640/-360/360 on the 1280 target)
    10508,  // 0x2110 PA_CL_VPORT_XOFFSET
    10512,  // 0x2111 PA_CL_VPORT_YSCALE
    10516,  // 0x2112 PA_CL_VPORT_YOFFSET
    10520,  // 0x2113 PA_CL_VPORT_ZSCALE   (z122 TEMP-ZPORT-CENSUS: -1 / +1)
    10524,  // 0x2114 PA_CL_VPORT_ZOFFSET  (z122: 1 / 0)
    10436,  // 0x2081 PA_SC_WINDOW_SCISSOR_TL (setter 0x82415D54; docs/INV_SCISSOR_OFFSETS.md)
    10440,  // 0x2082 PA_SC_WINDOW_SCISSOR_BR
    0,      // 0x2080 PA_SC_WINDOW_OFFSET: no proven dev offset -> value forced 0, presence cleared below
    0,      // 0x231D RB_DEPTH_CLEAR: not proven in the dev shadow (see render_queue.h) -> presence cleared below
    10496,  // 0x210D RB_STENCILREFMASK    (block 0x2100 @ 10444, +13 dwords; same block as RB_ALPHA_REF +14)
    10492,  // 0x210C RB_STENCILREFMASK_BF
    10584,  // 0x2209 RB_BLENDCONTROL1     (block 0x2200 @ 10548, +9 dwords)
    10588,  // 0x220A RB_BLENDCONTROL2
    10592,  // 0x220B RB_BLENDCONTROL3
};
constexpr uint32_t kBank = 4096;  // 256 float4, = kShaderConstantBankBytes

mcla::gnh::Mode ReadMode() {
  const char *e = std::getenv("MCLA_RENDERER");
  const std::string_view v = e ? e : "";
  // Owner decision 2026-11-10: native is the default; pm4 drawing is opt-in.
  if (v == "pm4") return mcla::gnh::Mode::kPm4;
  if (v == "both") return mcla::gnh::Mode::kBoth;
  return mcla::gnh::Mode::kNative;
}
const mcla::gnh::Mode g_mode = ReadMode();
bool Producing() { return g_mode != mcla::gnh::Mode::kPm4; }

// The XDK's own VA->physical rule, as it writes it into IM_LOAD and vertex
// fetch packets (sub_8241CD88 / sub_82424670): ((va>>20)+0x200)&0x1000 + va&0x1FFFFFFF.
uint32_t XdkPhys(uint32_t va) {
  return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu);
}
// PERF (MCLA_NO_LEAN_CENSUS=1 = old): skip the F640 shared calls/translated fetch_adds per translation
// (~3% of the guest render thread); kNoPoolByte is still counted and logged.
bool LeanCensus() {
  static const bool on = [] { const char *e = std::getenv("MCLA_NO_LEAN_CENSUS"); return !(e && e[0] == '1'); }();
  return on;
}
mcla::boot::PhysViewResult PhysView(uint32_t va, uint32_t *out) {
  return LeanCensus() ? mcla::boot::TranslatePhysViewToPoolVaQuiet(va, out)
                      : mcla::boot::TranslatePhysViewToPoolVa(va, out);
}
// Same rule as gpu_cp.cpp CpPhysToPoolVa (F-640/F-680), without its census.
uint32_t PhysToPool(uint32_t pa) {
  if (pa >= mcla::kernel::kConsolePhysicalBytes) return pa;
  uint32_t out = 0;
  return PhysView(pa | 0xC0000000u, &out) == mcla::boot::PhysViewResult::kMapped ? out : 0u;
}
// PERF (MCLA_NO_LEAN_COPY=1 = old): per-draw data movement on the guest thread -- recycle the ~18 KB NativeDraw
// instead of zero-allocating it, reset only program headers (consumers read body[0, captured) only:
// render_thread.cpp HashBlock8/memcmp/assign use `captured`), read the VS patch by reference, and push
// the draw straight into the queue slot copying only the captured body prefixes (render_queue.h push_draw).
bool LeanCopy() {
  static const bool on = [] { const char *e = std::getenv("MCLA_NO_LEAN_COPY"); return !(e && e[0] == '1'); }();
  return on;
}

// PERF sub-stages of the native producer (MCLA_PERF_STAGES=1): QPC ticks per stage, printed with TEMP-PRODUCER.
constexpr int kProdStages = 9;
std::atomic<long long> g_prodStage[kProdStages]{};
thread_local long long t_prodMark = 0;
inline void ProdMark(int i) {
  if (!mcla::gnh::TempProducerOn()) return;
  LARGE_INTEGER c; QueryPerformanceCounter(&c);
  if (i >= 0) g_prodStage[i].fetch_add(c.QuadPart - t_prodMark, std::memory_order_relaxed);
  t_prodMark = c.QuadPart;
}

std::atomic<uint32_t> s_nBuilt{0}, s_nPushed{0}, s_nRefused{0}, s_nPm4Bridged{0},
    s_nPm4Suppressed{0}, s_nIdxUnproduced{0}, s_nCmpMatched{0}, s_nCmpClean{0},
    s_nCmpNativeOnly{0}, s_nCmpPm4Only{0};
constexpr const char *kFieldNames[] = {
    "vbBase", "vbSize", "vbStride", "vbFmt", "elemCount", "elements", "streams",
    "prim", "numIdx", "idxOffset", "initiator", "vsProg", "vsBody", "psProg",
    "psBody", "raster", "texCount", "texDesc", "texPool", "vsConst", "psConst"};
constexpr uint32_t kFields = sizeof(kFieldNames) / sizeof(kFieldNames[0]);
std::atomic<uint32_t> s_fieldMiss[kFields];

// One stage's program from its shader object (see BuildDraw for the layout).
// Body read through the same pool view CpExecImLoad uses.
bool ReadProgram(uint32_t addrVa, uint32_t bytes, rq::CpShaderProgram &p,
                 uint32_t *full, uint32_t fullCap, uint32_t *fullN, bool rawVa = false) {
  if (LeanCopy()) {  // header only: body[0, captured) is written below and nothing reads past it
    p.present = p.addrSpace = p.guestAddr = p.dwords = p.embedded = p.captured = p._pad = 0;
  } else {
    p = {};
  }
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const uint32_t phys = rawVa ? addrVa : XdkPhys(addrVa);
  const uint32_t dw = bytes >> 2;
  if (addrVa == 0 || dw == 0) return false;
  const uint32_t pool = PhysToPool(phys);
  uint32_t src = 0;
  uint32_t guestPhys = phys & ~3u;
  if (pool != 0 && pool != phys && mem.IsValid(pool, bytes)) {
    src = pool;
    p.addrSpace = rq::kCpShaderAddrKernel;
  } else if (mem.IsValid(phys, bytes)) {
    src = phys;
    p.addrSpace = rq::kCpShaderAddrGuest;
  } else if (addrVa >= 0xC0000000u && addrVa < 0xE0000000u) {
    const uint32_t pa = addrVa - 0xC0000000u;
    const uint32_t wcPool = 0xA0000000u + pa;
    if (mem.IsValid(wcPool, bytes)) {
      src = wcPool;
      guestPhys = pa & ~3u;
      p.addrSpace = rq::kCpShaderAddrKernel;
    } else {
      return false;
    }
  } else {
    return false;
  }
  p.present = 1;
  p.guestAddr = guestPhys;
  p.dwords = dw;
  p.captured = dw < 1024u ? dw : 1024u;  // kMicrocodeCaptureDwords (gpu_cp.cpp:485)
  const uint32_t n = full ? (p.captured < fullCap ? p.captured : fullCap)
                          : (p.captured < rq::kCpShaderBodyDwords ? p.captured
                                                                  : rq::kCpShaderBodyDwords);
  // PERF 2026-11-17: one bulk read + swap instead of up to 1024 checked/SEH dword reads per draw (IsValid(src, bytes) held above).
  static thread_local uint32_t raw[1024];
  if (n <= 1024u && mem.ReadBytes(src, raw, n * 4u)) {
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t v = __builtin_bswap32(raw[i]);
      if (full) full[i] = v;
      if (i < rq::kCpShaderBodyDwords) p.body[i] = v;
    }
  } else
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v = 0;
    (void)mem.ReadU32BE(src + 4 * i, &v);
    if (full) full[i] = v;
    if (i < rq::kCpShaderBodyDwords) p.body[i] = v;
  }
  if (fullN) *fullN = n;
  return true;
}

// Raw guest bytes are big-endian float4s, the layout CpWriteShaderConstantFloat4 packs.
void CopyBank(uint32_t src, uint8_t *out) {
  if (!mcla::kernel::GuestMemoryHeap::Instance().ReadBytes(src, out, kBank))
    std::memset(out, 0, kBank);
}

// Immediate-mode stream that BeginVertices writes inline into fetch slot 95
// (type-0 header 0x000548BA = regs 0x48BA..0x48BF = slot 31 dwords 4-5).
struct ImmStream { uint32_t va = 0, bytes = 0; bool on = false; uint32_t stride = 0; };

// Index source of a DrawIndexedVertices draw: pool VA of the first index, bytes,
// element width and the Xenos swap mode (from the IB object header, sub_8241D620).
struct IdxSrc { uint32_t va = 0, bytes = 0, wide = 0, swap = 0; };

bool ReadVertexSlot(uint32_t dev, uint32_t slot, const ImmStream &imm,
                    uint32_t &base, uint32_t &size, uint32_t &endian) {
  if (imm.on && slot == 95) {  // d0 = phys|3, d1 = (bytes/4)<<2 | 2 (k8in32)
    base = PhysToPool(XdkPhys(imm.va));
    size = imm.bytes;
    endian = 2;
    return true;
  }
  const uint32_t d0 = Rd(dev + kDevFetch + 8 * slot);
  const uint32_t d1 = Rd(dev + kDevFetch + 8 * slot + 4);
  if ((d0 & 3u) != 3u) return false;  // FetchConstantType::kVertex
  base = PhysToPool((d0 >> 2) << 2);
  size = ((d1 >> 2) & 0x00FFFFFFu) << 2;
  endian = d1 & 3u;
  return true;
}

// The shader's embedded "def cN" constants are NOT in the device shadow: sub_82424670 ->
// sub_82424480 (decompiled) walks the shader's definition table {u16 reg, u16 dwords, u32 off}
// and emits a PM4 LOAD_ALU_CONSTANT (0xC0022F00) per entry straight into the ring. Native mode
// has no ring, so replay those loads into the bank copy. a2/a3 as the guest passes them:
// VS (vs+872, *(vs+32)); PS (ps+40, *(ps+24)). PS regs carry the +256 bias.
uint32_t ApplyShaderDefs(uint32_t a2, uint32_t a3, bool isPs, uint8_t *bank) {
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  const uint32_t tbl = Rd(a2 + 20);
  if (tbl == 0) return 0;
  const uint32_t v6 = tbl + a2;
  uint32_t p = v6 + 20;
  const uint32_t end = Rd(v6 + 16) + v6 + 20;
  uint32_t applied = 0;
  for (uint32_t g = 0; p < end && g < 256; ++g, p += 8) {
    const uint32_t w0 = Rd(p);
    const uint32_t reg = w0 >> 16, cnt = w0 & 0xFFFFu;
    if (cnt == 0) break;
    const int32_t bankReg = int32_t(reg) - (isPs ? 256 : 0);
    if (bankReg < 0 || bankReg * 4 + cnt > kBank / 4) continue;
    const uint32_t src = Rd(p + 4) + a3;
    const uint32_t pool = PhysToPool(XdkPhys(src));
    if (pool != 0 && mem.IsValid(pool, cnt * 4) &&
        mem.ReadBytes(pool, bank + bankReg * 16, cnt * 4))
      ++applied;
  }
  return applied;
}

struct NativeDraw {
  rq::DrawCapturedCommand dc{};
  std::array<uint8_t, kBank> vs{}, ps{};
};
// LeanCopy: the last submitted record of this thread, reused by the next BuildDraw.
thread_local std::unique_ptr<NativeDraw> t_spareDraw;
// Equivalent of a fresh `NativeDraw{}` for every byte a consumer reads: all of dc zeroed except the two
// 4 KB program bodies (only body[0, captured) is read; captured is zeroed here). Banks are fully
// rewritten by CopyBank (ReadBytes or memset) before use.
std::unique_ptr<NativeDraw> NewDraw() {
  if (!LeanCopy() || !t_spareDraw) return std::make_unique<NativeDraw>();
  std::unique_ptr<NativeDraw> nd = std::move(t_spareDraw);
  auto *b = reinterpret_cast<uint8_t *>(&nd->dc);
  constexpr size_t vs = offsetof(rq::DrawCapturedCommand, vsProgram), ps = offsetof(rq::DrawCapturedCommand, psProgram);
  constexpr size_t body = offsetof(rq::CpShaderProgram, body), prog = sizeof(rq::CpShaderProgram);
  static_assert(vs + prog <= ps, "vsProgram precedes psProgram");
  std::memset(b, 0, vs + body);
  std::memset(b + vs + prog, 0, ps + body - (vs + prog));
  std::memset(b + ps + prog, 0, sizeof(rq::DrawCapturedCommand) - (ps + prog));
  return nd;
}

bool NativeCbEnabled() {
  static const bool on = [] {
    const char *e = std::getenv("MCLA_NO_NATIVE_CB");
    return !(e && e[0] == '1');
  }();
  return on;
}

uint64_t ReadGuestU64BE(uint32_t va) {
  if (va == 0u) return 0ull;
  return (static_cast<uint64_t>(Rd(va)) << 32) | static_cast<uint64_t>(Rd(va + 4u));
}

struct RecordedCbOp {
  enum class Kind : uint8_t { Draw, Resolve, Clear, Query };
  Kind kind = Kind::Draw;
  rq::DrawCapturedCommand dc{};
  std::array<uint8_t, kBank> vsConst{};
  std::array<uint8_t, kBank> psConst{};
  uint64_t inheritVsMask = 0;
  uint64_t inheritPsMask = 0;
  rq::ResolveCommand resolve{};
  rq::ClearCommand clear{};
  rq::QueryOpCommand query{};
};

struct RecordedCbEntry {
  uint32_t dev = 0;
  uint64_t inheritVsMask = 0;
  uint64_t inheritPsMask = 0;
  bool recording = false;
  bool sealed = false;
  std::vector<RecordedCbOp> ops;
};

std::mutex s_cbMtx;
std::unordered_map<uint32_t, RecordedCbEntry> s_cbMap;

bool TryRecordCbDraw(uint32_t dev, std::unique_ptr<NativeDraw> &nd) {
  if (g_mode != mcla::gnh::Mode::kNative || !NativeCbEnabled() || dev == 0u) return false;
  const uint32_t activeCb = Rd(dev + 13516u);
  if (activeCb == 0u) return false;
  const uint64_t vsDirty = ReadGuestU64BE(dev + 0u);
  const uint64_t psDirty = ReadGuestU64BE(dev + 8u);
  std::lock_guard<std::mutex> lk(s_cbMtx);
  auto it = s_cbMap.find(activeCb);
  if (it == s_cbMap.end() || !it->second.recording) return false;
  RecordedCbOp op;
  op.kind = RecordedCbOp::Kind::Draw;
  op.dc = nd->dc;
  op.vsConst = nd->vs;
  op.psConst = nd->ps;
  op.inheritVsMask = it->second.inheritVsMask & ~vsDirty;
  op.inheritPsMask = it->second.inheritPsMask & ~psDirty;
  it->second.ops.push_back(std::move(op));
  return true;
}

bool TryRecordCbResolve(uint32_t dev, const rq::ResolveCommand &rc) {
  if (g_mode != mcla::gnh::Mode::kNative || !NativeCbEnabled() || dev == 0u) return false;
  const uint32_t activeCb = Rd(dev + 13516u);
  if (activeCb == 0u) return false;
  std::lock_guard<std::mutex> lk(s_cbMtx);
  auto it = s_cbMap.find(activeCb);
  if (it == s_cbMap.end() || !it->second.recording) return false;
  RecordedCbOp op;
  op.kind = RecordedCbOp::Kind::Resolve;
  op.resolve = rc;
  it->second.ops.push_back(std::move(op));
  return true;
}

bool TryRecordCbClear(uint32_t dev, const rq::ClearCommand &cc) {
  if (g_mode != mcla::gnh::Mode::kNative || !NativeCbEnabled() || dev == 0u) return false;
  const uint32_t activeCb = Rd(dev + 13516u);
  if (activeCb == 0u) return false;
  std::lock_guard<std::mutex> lk(s_cbMtx);
  auto it = s_cbMap.find(activeCb);
  if (it == s_cbMap.end() || !it->second.recording) return false;
  RecordedCbOp op;
  op.kind = RecordedCbOp::Kind::Clear;
  op.clear = cc;
  it->second.ops.push_back(std::move(op));
  return true;
}

bool TryRecordCbQuery(uint32_t dev, const rq::QueryOpCommand &qc) {
  if (g_mode != mcla::gnh::Mode::kNative || !NativeCbEnabled() || dev == 0u) return false;
  const uint32_t activeCb = Rd(dev + 13516u);
  if (activeCb == 0u) return false;
  std::lock_guard<std::mutex> lk(s_cbMtx);
  auto it = s_cbMap.find(activeCb);
  if (it == s_cbMap.end() || !it->second.recording) return false;
  RecordedCbOp op;
  op.kind = RecordedCbOp::Kind::Query;
  op.query = qc;
  it->second.ops.push_back(std::move(op));
  return true;
}

// Builds the record. Returns nullptr (counted) when the device state cannot
// describe a drawable stream -- never a substituted value.
// Last vfetch-patch target of this guest thread's current draw (set by the sub_82423A38 hook, consumed by BuildDraw).
constexpr uint32_t kVsPatchMax = 32u;
struct VsPatch {
  uint32_t vs = 0, dst = 0, variant = 0, n = 0;
  bool nz = false;
  uint32_t slot[kVsPatchMax] = {}, w[kVsPatchMax][3] = {};
  // VS-LINK: the whole ring copy when dst is not the image. sub_82424320 runs sub_824241D0 on it first,
  // which remaps the VS exports to the bound PS's inputs; the triples above miss that.
  uint32_t fullN = 0, full[1024];
};
thread_local VsPatch t_vsPatch;
thread_local uint32_t t_dbgPatch[4];  // GNH-REFUSE-WHY diag: last patch vs/variant, BuildDraw's v, strideNZ
std::unique_ptr<NativeDraw> BuildDraw(uint32_t dev, uint32_t prim,
                                      uint32_t count, uint32_t idxOffset,
                                      const ImmStream &imm,
                                      const IdxSrc &ix = IdxSrc{}) {
  auto nd = NewDraw();
  ProdMark(0);
  rq::DrawCapturedCommand &dc = nd->dc;
  static thread_local uint32_t vsFull[1024];
  uint32_t vsN = 0;
  const uint32_t vs = Rd(dev + kDevVs), ps = Rd(dev + kDevPs);
  if (vs != 0) {
    // sub_82424670: VS variant v, entry = vs + *(vs+896+8v),
    // IM_LOAD addr = *(entry+872) + *(vs+32), bytes = *(entry+876).
    // shortcut: v = its v10 (1 only with no PS and flag 0x20 at vs+872); the
    // sub_82424530 re-pick is not modelled -- GNH-CMP vsProg shows if it matters.
    const uint32_t v = (ps == 0 && (Rd(vs + 872) & 0x20u)) ? 1u : 0u;
    const uint32_t ent = vs + Rd(vs + 896 + 8 * v);
    // The guest patches the VS's vfetch triples per vertex declaration (sub_82423A38). When the image is still in
    // flight it patches a COPY in the command ring instead (sub_82424320) and the image keeps the stride-0 template;
    // reading the image then refused the draw (GNH-REFUSE-WHY why=stride0). Read the patched copy when there is one.
    auto aliasWcToPool = [](uint32_t va) -> uint32_t {
      return (va >= 0xC0000000u && va < 0xE0000000u)
          ? (0xA0000000u + (va - 0xC0000000u))
          : va;
    };
    const uint32_t img = Rd(ent + 872) + Rd(vs + 32);
    const uint32_t imgPool = aliasWcToPool(img);
    const uint32_t vsBytes = Rd(ent + 876);
    // The patched VS stays loaded on the GPU until the guest patches/loads another, so the last snapshot stays valid
    // for later draws of the same vs (GNH-REFUSE-WHY patchVs=0: patch came on an earlier draw). MCLA_NO_VS_PATCH_KEEP=1 = clear per draw.
    static const bool s_noKeep = [] { const char* e = std::getenv("MCLA_NO_VS_PATCH_KEEP"); return e && e[0] == '1'; }();
    // t_vsPatch only changes in the sub_82423A38 hook on this thread, never inside BuildDraw: a reference is safe.
    static thread_local VsPatch t_ptCopy;
    const bool copyPt = s_noKeep || !LeanCopy();
    if (copyPt) t_ptCopy = t_vsPatch;
    const VsPatch &pt = copyPt ? t_ptCopy : t_vsPatch;
    if (s_noKeep) t_vsPatch = {};
    t_dbgPatch[0] = pt.vs; t_dbgPatch[1] = pt.variant; t_dbgPatch[2] = v; t_dbgPatch[3] = pt.nz ? 1u : 0u;
    static const bool s_noRing = [] { const char* e = std::getenv("MCLA_NO_VS_RING_COPY"); return e && e[0] == '1'; }();
    bool ringOk = false;
    if (!ReadProgram(img, vsBytes, dc.vsProgram, vsFull, 1024, &vsN) && imgPool != img) {
      (void)ReadProgram(imgPool, vsBytes, dc.vsProgram, vsFull, 1024, &vsN, true);
    }
    // Prefer the triples snapshotted inside the patch hook: the guest's own output, read before the ring moves on.
    bool snapUsed = false;
    // VS-LINK (MCLA_NO_VS_LINK_COPY=1 = off): the guest's linked ring copy, same program (cf words match).
    if (!s_noRing && pt.vs == vs && pt.variant == v && pt.fullN == vsN && vsN >= 2 &&
        pt.full[0] == vsFull[0] && pt.full[1] == vsFull[1]) {
      uint32_t diff = 0;
      for (uint32_t i = 0; i < vsN; ++i) {
        diff += vsFull[i] != pt.full[i];
        vsFull[i] = pt.full[i];
        if (i < rq::kCpShaderBodyDwords) dc.vsProgram.body[i] = pt.full[i];
      }
      snapUsed = true;
      static std::atomic<uint32_t> s_lk{0};
      const uint32_t c = s_lk.fetch_add(1) + 1;
      if (c <= 4 || c % 20000 == 0)
        MCLA_LOG_INFO("VS-LINK-COPY #{} vs={:08X} dst={:08X} dwords={} changed={}", c, vs, pt.dst, vsN, diff);
    }
    if (!snapUsed && !s_noRing && pt.vs == vs && pt.variant == v && pt.nz && vsN >= 2) {
      for (uint32_t i = 0; i < pt.n; ++i) {
        const uint32_t o = 3u * pt.slot[i];
        if (o + 2u >= vsN) continue;
        for (uint32_t k = 0; k < 3u; ++k) vsFull[o + k] = dc.vsProgram.body[o + k] = pt.w[i][k];
        snapUsed = true;
      }
      static std::atomic<uint32_t> s_su{0};
      const uint32_t c = s_su.fetch_add(1) + 1;
      if (c <= 4 || c % 20000 == 0)
        MCLA_LOG_INFO("VS-PATCH-APPLIED #{} vs={:08X} dst={:08X} img={:08X} n={} used={}", c, vs, pt.dst, img, pt.n, snapUsed ? 1 : 0);
    }
    if (!snapUsed && !s_noRing && pt.vs == vs && pt.dst != 0 && pt.dst != img &&
        aliasWcToPool(pt.dst) != imgPool && vsN >= 2) {
      // Accept the copy only if it is the same program: its first two dwords (control flow, never patched) match.
      static thread_local uint32_t ringFull[1024];
      rq::CpShaderProgram rp; uint32_t rn = 0;
      const uint32_t dstPool = aliasWcToPool(pt.dst);
      const uint32_t cands[2] = {pt.dst, dstPool};
      const char* why = "read-fail";  // MD-05/37 census: the furthest check the last candidate reached
      for (const uint32_t cand : cands) {
        for (const bool raw : {false, true}) {
          if (!ReadProgram(cand, vsBytes, rp, ringFull, 1024, &rn, raw)) continue;
          if (rn != vsN) { why = "len-mismatch"; continue; }
          if (ringFull[0] != vsFull[0] || ringFull[1] != vsFull[1]) {
            why = "cf-mismatch";
            static std::atomic<uint32_t> s_cfDump{0};  // census: is the ring copy a patched variant or garbage?
            if (s_cfDump.fetch_add(1) < 8) {
              // Raw host bytes at Base()+cand: what generated guest stores to `cand` actually touched (no view translation).
              uint32_t d[3] = {0, 0, 0}; int committed = 0;
              const uint8_t* hp = mcla::kernel::GuestMemoryHeap::Instance().Base() + cand;
              MEMORY_BASIC_INFORMATION mbi{};
              if (VirtualQuery(hp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                committed = 1;
                std::memcpy(d, hp, sizeof(d));
                for (uint32_t& w : d) w = _byteswap_ulong(w);
              }
              MCLA_LOG_ERROR("VS-RING-CF vs={:08X} cand={:08X} raw={} ring={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} img={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} hostRaw(c={})={:08X} {:08X} {:08X}",
                             vs, cand, raw ? 1 : 0, ringFull[0], ringFull[1], ringFull[2], ringFull[3], ringFull[4], ringFull[5],
                             vsFull[0], vsFull[1], vsFull[2], vsFull[3], vsFull[4], vsFull[5], committed, d[0], d[1], d[2]);
            }
            continue;
          }
          dc.vsProgram = rp; std::memcpy(vsFull, ringFull, rn * 4u); ringOk = true; break;
        }
        if (ringOk || dstPool == pt.dst) break;
      }
      static std::atomic<uint32_t> s_rc{0}, s_rf{0};
      const uint32_t c = (ringOk ? s_rc : s_rf).fetch_add(1) + 1;
      if (c <= 4 || c % 20000 == 0)
        MCLA_LOG_ERROR("VS-RING-COPY {} #{} vs={:08X} img={:08X} dst={:08X} variant={} why={} rn={} vsN={}", ringOk ? "read" : "unreadable", c, vs, img, pt.dst, pt.variant, ringOk ? "ok" : why, rn, vsN);
    }

  }
  ProdMark(1);
  if (ps == 0 && GnhDiag()) {  // TEMP-NOPS-DIAG: what are the PS-less draws?
    static std::atomic<uint32_t> s_np{0};
    const uint32_t q = ++s_np;
    if (q <= 8 || q % 5000 == 0)
      MCLA_LOG_INFO("TEMP-NOPS-DIAG #{} vs={:08X} vsFlag20={} colorMask={:08X} colorInfo={:08X} depthCtl={:08X} depthInfo={:08X} blend0={:08X} prim={} count={}",
                    q, vs, (vs ? (Rd(vs + 872) >> 5) & 1u : 0u), Rd(dev + 10460), Rd(dev + 10372),
                    Rd(dev + 10548), Rd(dev + 10376), Rd(dev + 10552), prim, count);
  }
  if (GnhDiag()) {  // TEMP-3DP-RT: which render target / depth surface each native draw writes
    struct K { uint32_t surf, c0, c1, c2, c3, dep, idx, hasPs, cmask, dctl, blend, vte; };
    static std::mutex m; static std::vector<std::pair<K, uint32_t>> tab; static uint32_t total = 0;
    const K k{Rd(dev + 10368), Rd(dev + 10372), Rd(dev + 10380), Rd(dev + 10384), Rd(dev + 10388),
              Rd(dev + 10376), ix.bytes ? 1u : 0u, ps ? 1u : 0u,
              Rd(dev + 10460), Rd(dev + 10548), Rd(dev + 10552), Rd(dev + 10572)};
    std::lock_guard<std::mutex> lk(m);
    ++total;
    bool found = false;
    for (auto &e : tab) if (!std::memcmp(&e.first, &k, sizeof k)) { ++e.second; found = true; break; }
    if (!found && tab.size() < 160) {
      tab.push_back({k, 1});
      MCLA_LOG_WARN("TEMP-3DP-RT new#{} at draw {} surf={:08X}(pitch={} msaa={}) color0={:08X}(base={} fmt={}) c1={:08X} c2={:08X} c3={:08X} depth={:08X}(base={} fmt={}) indexed={} ps={} scissorBR={:08X} scissorTL={:08X}",
        tab.size(), total, k.surf, k.surf & 0x3FFF, (k.surf >> 16) & 3, k.c0, k.c0 & 0xFFF, (k.c0 >> 16) & 0xF,
        k.c1, k.c2, k.c3, k.dep, k.dep & 0xFFF, (k.dep >> 16) & 1, k.idx, k.hasPs, Rd(dev + 10428), Rd(dev + 10424));
    }
    if (total % 100000 == 0) {
      for (size_t i = 0; i < tab.size(); ++i)
        MCLA_LOG_WARN("TEMP-3DP-RT-TAB total={} #{} count={} surf={:08X} c0={:08X} dep={:08X} idx={} ps={} cmask={:08X} dctl={:08X} blend={:08X} vte={:08X}", total, i + 1, tab[i].second,
          tab[i].first.surf, tab[i].first.c0, tab[i].first.dep, tab[i].first.idx, tab[i].first.hasPs,
          tab[i].first.cmask, tab[i].first.dctl, tab[i].first.blend, tab[i].first.vte);
    }
  }
  if (ps != 0) {
    // sub_82424670: hdr = ps + *(ps+64), addr = *(hdr+40) + *(ps+24), bytes = *(hdr+44).
    const uint32_t hdr = ps + Rd(ps + 64);
    (void)ReadProgram(Rd(hdr + 40) + Rd(ps + 24), Rd(hdr + 44), dc.psProgram,
                      nullptr, 0, nullptr);
  }
  if (!dc.vsProgram.present) dc.vsProgram.addrSpace = rq::kCpShaderAddrAbsent;
  if (!dc.psProgram.present) dc.psProgram.addrSpace = rq::kCpShaderAddrAbsent;
  ProdMark(2);

  // Vertex elements from the bound VS, resolved against the fetch shadow.
  rq::CpVertexElement el[rq::kCpVertexElementMax];
  // PERF (MCLA_NO_ELEM_MEMO=1 = decode every draw): the decode is a pure function of vsFull[0, vsN), so the
  // previous draw's result is reused when the words are identical (consecutive draws mostly share a VS).
  // Its vfetch census counters then count distinct programs, not draws.
  static const bool s_noElemMemo = [] { const char* e = std::getenv("MCLA_NO_ELEM_MEMO"); return e && e[0] == '1'; }();
  static thread_local uint32_t t_emWords[1024], t_emN = 0, t_emEl = 0;
  static thread_local rq::CpVertexElement t_emOut[rq::kCpVertexElementMax];
  uint32_t nEl = 0;
  if (vsN && !s_noElemMemo && vsN == t_emN && std::memcmp(vsFull, t_emWords, vsN * 4u) == 0) {
    nEl = t_emEl;
    std::memcpy(el, t_emOut, sizeof(el[0]) * nEl);
  } else if (vsN) {
    nEl = mcla::gpu::CpDecodeVertexElementsFromWords(vsFull, vsN, el, rq::kCpVertexElementMax);
    if (!s_noElemMemo) {
      std::memcpy(t_emWords, vsFull, vsN * 4u);
      t_emN = vsN; t_emEl = nEl;
      std::memcpy(t_emOut, el, sizeof(el[0]) * nEl);
    }
  }
  if (nEl == rq::kCpVertexElementMax) {  // MD-38 census (log-only): does the cap truncate a real VS?
    static thread_local rq::CpVertexElement wide[32];
    const uint32_t all = mcla::gpu::CpDecodeVertexElementsFromWords(vsFull, vsN, wide, 32u);
    static std::atomic<uint32_t> s_capHit{0}, s_capTrunc{0};
    const uint32_t h = s_capHit.fetch_add(1) + 1;
    const uint32_t t = all > nEl ? s_capTrunc.fetch_add(1) + 1 : s_capTrunc.load();
    if (h <= 4 || h % 5000 == 0 || (all > nEl && (t <= 8 || t % 1000 == 0)))
      MCLA_LOG_WARN("GNH-ELEMCAP hit#{} trunc#{} vs={:08X} cap={} actual={}", h, t, vs, nEl, all);
  }
  const rq::CpVertexElement *stream = nullptr;
  uint32_t sBase = 0, sSize = 0;
  bool tooManyStreams = false;
  for (uint32_t i = 0; i < nEl; ++i) {
    uint32_t b = 0, sz = 0, en = 0;
    if (!ReadVertexSlot(dev, el[i].slotIndex, imm, b, sz, en)) continue;
    rq::CpVertexElement &o = dc.vbElements[dc.vbElementCount];
    o = el[i];
    o.elementIndex = dc.vbElementCount++;
    o.endianness = en;
    // The immediate stream is not covered by the sub_82423A38 vfetch patch (VS template keeps stride 0, GNH-REFUSE-WHY
    // vs=40285C50 slot=95); the guest's own BeginVertices stride (r6) is the per-vertex stride. Only when the element fits.
    static const bool s_noImmStride = [] { const char* e = std::getenv("MCLA_NO_IMM_STRIDE"); return e && e[0] == '1'; }();
    if (!s_noImmStride && imm.on && el[i].slotIndex == 95u && o.strideBytes == 0u && imm.stride != 0u &&
        o.offsetBytes >= 0 && uint32_t(o.offsetBytes) + o.fmt.bytes <= imm.stride && o.fmt.bytes != 0u) {
      o.strideBytes = imm.stride;
      static std::atomic<uint32_t> s_is{0};
      const uint32_t c = s_is.fetch_add(1) + 1;
      if (c <= 8 || c % 5000 == 0)
        MCLA_LOG_INFO("IMM-STRIDE-FILL #{} vs={:08X} off={} bytes={} stride={}", c, vs, o.offsetBytes, o.fmt.bytes, imm.stride);
    }
    if (o.miniFetch) continue;
    ++dc.vbFullCount;
    if (!stream) { stream = &o; sBase = b; sSize = sz; }
    bool found = false;
    for (uint32_t s = 0; s < dc.vbStreamCount; ++s)
      found |= dc.vbStreams[s].slotIndex == o.slotIndex &&
               dc.vbStreams[s].vbBase == b && dc.vbStreams[s].vbStride == o.strideBytes;
    if (!found) {
      if (dc.vbStreamCount < 4u) {
        auto &st = dc.vbStreams[dc.vbStreamCount++];
        st.slotIndex = o.slotIndex;
        st.vbBase = b;
        st.vbStride = o.strideBytes;
        st.vbSize = sz;
        st.endianness = en;
      } else {
        tooManyStreams = true;
      }
    }
  }
  dc.vbDescCount = dc.vbElementCount;
  if (dc.vbElementCount != nEl) {  // GNH-ELEMDROP: elements whose fetch slot is not a vertex constant
    static std::atomic<uint32_t> s_drop{0};
    const uint32_t d = s_drop.fetch_add(1) + 1;
    if (d <= 8 || d % 5000 == 0) {
      std::string t;
      for (uint32_t i = 0; i < nEl; ++i)
        t += fmt::format(" [slot={} mini={} d0={:08X} d1={:08X} w1={:08X} fmt={} stride={} off={} dst={} cond={}]", el[i].slotIndex,
                         el[i].miniFetch, Rd(dev + kDevFetch + 8 * el[i].slotIndex),
                         Rd(dev + kDevFetch + 8 * el[i].slotIndex + 4), el[i].fetchWord1, el[i].fmt.xenos,
                         el[i].strideBytes, el[i].offsetBytes, el[i].dstReg, el[i].conditional);  // MD-06: w1==0 => zero triple (false positive)
      MCLA_LOG_WARN("GNH-ELEMDROP #{} idx={} vs={:08X} decoded={} kept={}{}", d,
                    ix.bytes ? 1 : 0, vs, nEl, dc.vbElementCount, t);
    }
  }
  bool badStream = (dc.vbStreamCount == 0u) || !stream || tooManyStreams;
  uint32_t badStreamIdx = 0u;
  if (!badStream) {
    for (uint32_t s = 0; s < dc.vbStreamCount; ++s) {
      if (dc.vbStreams[s].vbBase == 0u || dc.vbStreams[s].vbStride == 0u || dc.vbStreams[s].vbSize == 0u) {
        badStream = true;
        badStreamIdx = s;
        break;
      }
    }
  }
  if (badStream) {
    const uint32_t r = s_nRefused.fetch_add(1) + 1;
    static const bool s_why = [] { const char* e = std::getenv("MCLA_TEMP_REFUSE_WHY"); return e && e[0] == '1'; }();
    if (s_why) {  // one row per distinct (vs, reason) with the fields that decide it, plus running totals
      static std::mutex rm; static std::vector<uint64_t> seenR;
      const uint32_t reasonCode = tooManyStreams ? 4u : (!stream || dc.vbStreamCount == 0u) ? 1u : (dc.vbStreams[badStreamIdx].vbBase == 0u ? 2u : 3u);
      const uint64_t key = (uint64_t(vs) << 8) | reasonCode;
      std::lock_guard<std::mutex> lk(rm);
      if (seenR.size() < 64 && std::find(seenR.begin(), seenR.end(), key) == seenR.end()) {
        seenR.push_back(key);
        std::string t;
        for (uint32_t i = 0; i < nEl; ++i)
          t += fmt::format(" [slot={} mini={} d0={:08X} d1={:08X} off={} fb={} xf={} st={}]", el[i].slotIndex, el[i].miniFetch,
                           Rd(dev + kDevFetch + 8 * el[i].slotIndex), Rd(dev + kDevFetch + 8 * el[i].slotIndex + 4),
                           el[i].offsetBytes, el[i].fmt.bytes, el[i].fmt.xenos, el[i].strideBytes);
        t += fmt::format(" immOn={} immStride={} patchVs={:08X} patchVar={} v={} patchNZ={}", imm.on ? 1 : 0, imm.stride,
                         t_dbgPatch[0], t_dbgPatch[1], t_dbgPatch[2], t_dbgPatch[3]);
        MCLA_LOG_ERROR("GNH-REFUSE-WHY vs={:08X} vsDw={} why={} sBase={:08X} stride={} imm={:08X}+{} kept={} decoded={} refusedTotal={} built={}{}",
                       vs, vsN, tooManyStreams ? ">4-streams" : (!stream || dc.vbStreamCount == 0u) ? "no-stream" : (dc.vbStreams[badStreamIdx].vbBase == 0u ? "base0" : "stride0"),
                       sBase, stream ? stream->strideBytes : 0u, imm.va, imm.bytes, dc.vbElementCount, nEl, r, s_nBuilt.load(), t);
      }
    }
    if (r <= 8 || r % 1000 == 0)
      MCLA_LOG_WARN("GNH-P2-REFUSE #{} {} vs={:08X} vsDw={} elems={} "
                    "decoded={} streams={} imm={:08X}+{} prim={} cnt={}",
                    r, tooManyStreams ? ">4-streams" : "no-vertex-stream", vs, vsN, dc.vbElementCount, nEl, dc.vbStreamCount, imm.va, imm.bytes, prim, count);
    return nullptr;
  }
  dc.vbBase = dc.vbStreams[0].vbBase;
  dc.vbSize = dc.vbStreams[0].vbSize;
  dc.vbStride = dc.vbStreams[0].vbStride;
  dc.vbFormat = stream->fmt;
  if (ix.bytes && ps != 0 && GnhDiag()) {  // TEMP-GEOM-PROBE: capped, one row per distinct (vs,layout)
    static std::mutex gm; static std::vector<uint64_t> seen; static std::atomic<uint32_t> gtot{0};
    uint64_t key = vs;
    for (uint32_t i = 0; i < dc.vbElementCount; ++i)
      key = key * 1099511628211ull ^ (dc.vbElements[i].fmt.xenos | (dc.vbElements[i].offsetBytes << 8) | (dc.vbElements[i].fetchWord1 << 16));
    std::lock_guard<std::mutex> lk(gm);
    gtot.fetch_add(1);
    bool f = false; for (auto k : seen) f |= (k == key);
    if (!f && seen.size() < 24) {
      seen.push_back(key);
      std::string t;
      for (uint32_t i = 0; i < dc.vbElementCount; ++i) {
        const auto &e = dc.vbElements[i];
        t += fmt::format(" [slot={} xfmt={} off={} st={} mini={} w1={:08X} dstReg={} dstSwz={:03X} endian={}]", e.slotIndex, e.fmt.xenos, e.offsetBytes, e.strideBytes, e.miniFetch, e.fetchWord1, e.dstReg, e.dstSwizzle, e.endianness);
      }
      std::string vtx;
      auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
      for (uint32_t v = 0; v < 3; ++v) {
        vtx += " v" + std::to_string(v) + "=";
        for (uint32_t d = 0; d < stream->strideBytes / 4 && d < 12; ++d) {
          uint32_t w = 0; (void)mem.ReadU32BE(sBase + v * stream->strideBytes + 4 * d, &w);
          vtx += fmt::format("{:08X} ", w);
        }
      }
      std::string idx;
      for (uint32_t d = 0; d < 6; ++d) { uint32_t w = 0; (void)mem.ReadU32BE(ix.va + 4 * d, &w); idx += fmt::format("{:08X} ", w); }
      uint32_t nzPs = 0, nzVs = 0; std::string pc;
      for (uint32_t c = 0; c < kBank / 4; ++c) {
        const uint32_t pw = Rd(dev + kDevPsConst + 4 * c), vw = Rd(dev + kDevVsConst + 4 * c);
        nzPs += (pw << 1) != 0; nzVs += (vw << 1) != 0;
        if (pw << 1 && pc.size() < 160 && c < 64) pc += fmt::format("c{}={:08X} ", c, pw);
      }
      MCLA_LOG_WARN("TEMP-GEOM-PROBE new#{} draws={} vs={:08X} ps={:08X} numIdx={} prim={} vbBase={:08X} vbSize={} stride={} elems={}{} | verts:{} | idx:{} | vsConstNZ={} psConstNZ={} ps:{}",
        seen.size(), gtot.load(), vs, ps, count, prim, sBase, sSize, stream->strideBytes, dc.vbElementCount, t, vtx, idx, nzVs, nzPs, pc);
    }
  }

  ProdMark(3);
  dc.ibBase = dc.ibSize = dc.ibFmt = dc.ibSwap = 0;  // auto-index
  dc.numIndices = count;
  dc.idxOffset = idxOffset;
  dc.primitiveTopology = prim & 0x3Fu;
  dc.sourceSelect = 2u;  // kAutoIndex
  dc.opcode = 0x22u;     // DRAW_INDX, as sub_8241CD88 / sub_8241D230 emit it
  dc.initiator = (prim & 0x3Fu) | (count << 16) | 0x80u;
  if (ix.bytes) {  // kDMA: sub_8241D620 sets source_select 0, bit 11 = 32-bit
    dc.ibBase = ix.va;
    dc.ibSize = ix.bytes;
    dc.ibFmt = ix.wide;
    dc.ibSwap = ix.swap;
    dc.sourceSelect = 0u;
    dc.initiator = (prim & 0x3Fu) | (count << 16) | (ix.wide ? 0x800u : 0u);
  }

  dc.rtCaptured = 1;
  dc.rtSurfaceInfo = Rd(dev + 10368);
  dc.rtColorInfo = Rd(dev + 10372);
  dc.rtDepthInfo = Rd(dev + 10376);
  dc.rtScissorBr = Rd(dev + 10428);
  dc.rtColorInfoMrt[0] = dc.rtColorInfo;
  dc.rtColorInfoMrt[1] = Rd(dev + 10380);
  dc.rtColorInfoMrt[2] = Rd(dev + 10384);
  dc.rtColorInfoMrt[3] = Rd(dev + 10388);
  {
    const uint32_t colorMaskReg = Rd(dev + 10460);
    uint32_t boundMask = 0u;
    for (uint32_t i = 0; i < 4u; ++i) {
      const uint32_t surfPtr = Rd(dev + 12440u + 4u * i);
      const uint32_t nibble = (colorMaskReg >> (4u * i)) & 0xFu;
      if (surfPtr != 0u && (i == 0u || nibble != 0u)) {
        boundMask |= (1u << i);
      }
    }
    dc.rtBoundMask = boundMask;
  }
  for (uint32_t i = 0; i < rq::kCpSnapRegCount; ++i)
    dc.rasterState.regs[i] = Rd(dev + kSnapOff[i]);
  dc.rasterState.presentMask = (1u << rq::kCpSnapRegCount) - 1u;
  // Slots with no proven device-shadow offset: not present on this path (consumers test presence).
  dc.rasterState.regs[rq::kCpSnapWinOffset] = 0u;
  dc.rasterState.regs[rq::kCpSnapDepthClear] = 0u;
  dc.rasterState.presentMask &= ~((1u << rq::kCpSnapWinOffset) | (1u << rq::kCpSnapDepthClear));
  {  // MD-08/14: blend factor 0x2105-0x2108 (block 0x2100 @10444, +5..+8 dwords) and SQ_CONTEXT_MISC 0x2181 (block 0x2180 @10528, +1); raw scan of every bl sub_82423548 gives these bases/dsts
    static const bool s_noExtra = [] { const char* e = std::getenv("MCLA_NO_EXTRA_REGS"); return e && e[0] == '1'; }();
    if (!s_noExtra) {
      for (uint32_t k = 0; k < 4u; ++k) dc.blendFactorBits[k] = Rd(dev + 10464u + 4u * k);
      dc.sqContextMisc = Rd(dev + 10532u);
      dc.extraRegPresent = 3u;
      static std::atomic<uint32_t> s_ex{0};
      const uint32_t c = s_ex.fetch_add(1) + 1;
      if (c <= 4 || c % 100000 == 0)
        MCLA_LOG_INFO("EXTRA-REGS #{} blend={:08X} {:08X} {:08X} {:08X} sqMisc={:08X}", c, dc.blendFactorBits[0],
                      dc.blendFactorBits[1], dc.blendFactorBits[2], dc.blendFactorBits[3], dc.sqContextMisc);
    }
  }
  if (GnhDiag()) {   // TEMP-ZPORT-CENSUS: PA_CL_VPORT_ZSCALE/ZOFFSET (0x2113/0x2114 at 10520/10524) + RB_DEPTHCONTROL per unique
      // value -- is the guest using reversed Z (zscale<0) while the host viewport is MinDepth 0 / MaxDepth 1?
    static std::mutex s_m; static std::set<std::array<uint32_t, 5>> s_seen;
    const std::array<uint32_t, 5> k{dc.rasterState.regs[rq::kCpSnapPaClVteCntl], dc.rtSurfaceInfo & 0x3FFFu,
                                    Rd(dev + 10520), Rd(dev + 10524), dc.rasterState.regs[rq::kCpSnapDepthControl]};
    std::lock_guard<std::mutex> lk(s_m);
    if (s_seen.size() < 48 && s_seen.insert(k).second) {
      float f[2]; std::memcpy(f, &k[2], 8);
      MCLA_LOG_DEBUG("TEMP-ZPORT-CENSUS vte={:08X} pitch={} zscale={} zoff={} depthcontrol={:08X} zenable={} zwrite={} zfunc={} depthInfo={:08X}",
                     k[0], k[1], f[0], f[1], k[4], (k[4] >> 1) & 1u, (k[4] >> 2) & 1u, (k[4] >> 4) & 7u, dc.rtDepthInfo);
    }
  }

  ProdMark(4);
  // Texture fetch constants: the bridge's carry rule (type 2 only, BE bytes).
  // PERF (MCLA_NO_TEX_BLOCK=1 = one Rd per dword): the 768-byte fetch block is read through one fast pointer.
  static const bool s_noTexBlock = [] { const char* e = std::getenv("MCLA_NO_TEX_BLOCK"); return e && e[0] == '1'; }();
  const uint8_t *const fb = s_noTexBlock ? nullptr : mcla::native::FastMemPtr(dev + kDevFetch, 32u * 24u);
  auto RdT = [&](uint32_t va) -> uint32_t {
    if (!fb) return Rd(va);
    uint32_t v; std::memcpy(&v, fb + (va - dev - kDevFetch), 4); return __builtin_bswap32(v);
  };
  for (uint32_t slot = 0; slot < 32u; ++slot) {
    const uint32_t a = dev + kDevFetch + 24 * slot;
    if ((RdT(a) & 3u) != 2u) continue;
    if (dc.textureCount >= rq::kCpMaxTextureDescriptors) break;
    if (GnhDiag()) {  // TEMP-3DP-TEX: distinct guest textures sampled by native draws
      struct T { uint32_t base, fmt, w, h; };
      static std::mutex m; static std::vector<std::pair<T, uint32_t>> tab; static uint32_t total = 0;
      const uint32_t d1 = Rd(a + 4), d2 = Rd(a + 8);
      const T t{d1 & 0xFFFFF000u, d1 & 0x3Fu, (d2 & 0x1FFFu) + 1, ((d2 >> 13) & 0x1FFFu) + 1};
      std::lock_guard<std::mutex> lk(m);
      ++total;
      bool f = false;
      for (auto &e : tab) if (!std::memcmp(&e.first, &t, sizeof t)) { ++e.second; f = true; break; }
      if (!f && tab.size() < 200) {
        tab.push_back({t, 1});
        MCLA_LOG_WARN("TEMP-3DP-TEX new#{} base={:08X} fmt={} {}x{} slot={} ps={} (d0={:08X} d1={:08X} d2={:08X} d3={:08X} d4={:08X} d5={:08X})", tab.size(), t.base, t.fmt, t.w, t.h, slot, ps ? 1 : 0, Rd(a), d1, d2, Rd(a + 12), Rd(a + 16), Rd(a + 20));
      }
      if (total % 400000 == 0)
        for (size_t i = 0; i < tab.size(); ++i)
          MCLA_LOG_WARN("TEMP-3DP-TEX-TAB total={} base={:08X} fmt={} {}x{} count={}", total, tab[i].first.base, tab[i].first.fmt, tab[i].first.w, tab[i].first.h, tab[i].second);
    }
    const uint32_t phys = RdT(a + 4) & 0xFFFFF000u;
    uint32_t pool = 0;
    if (phys < 0x20000000u) pool = PhysToPool(phys);
    else if (PhysView(phys, &pool) !=
             mcla::boot::PhysViewResult::kMapped) pool = 0;
    dc.texturePoolVa[dc.textureCount] = pool;
    {   // mip_address (dword 5 bits 12..31), translated exactly like the base (gpu_cp.cpp P2a).
      const uint32_t mipPhys = RdT(a + 20) & 0xFFFFF000u;
      uint32_t mipPool = 0;
      if (mipPhys != 0u) {
        if (mipPhys < 0x20000000u) mipPool = PhysToPool(mipPhys);
        else if (PhysView(mipPhys, &mipPool) !=
                 mcla::boot::PhysViewResult::kMapped) mipPool = 0;
      }
      dc.texturePoolVaMip[dc.textureCount] = mipPool;
    }
    dc.textureFetchSlot[dc.textureCount] = static_cast<uint8_t>(slot);
    for (uint32_t d = 0; d < 6u; ++d) {
      const uint32_t w = RdT(a + 4 * d);
      for (uint32_t k = 0; k < 4u; ++k)
        dc.textureDescriptorBytes[dc.textureCount][4 * d + k] =
            static_cast<uint8_t>(w >> (24 - 8 * k));
    }
    ++dc.textureCount;
  }
  // TEMP-REPLAY tail: 48 dwords at dev+0x2780. Nothing in this tree reads them back
  // (render_queue.h:605 is the only other mention), yet they cost 48 SEH Rd() calls on
  // EVERY draw (~200K per frame at 4.2K draws/frame). Opt-in so a replay run can still
  // capture them without paying for them in every soak.
  static const bool s_tempReplayTail = [] {
    const char* e = std::getenv("MCLA_TEMP_REPLAY_TAIL");
    return e && e[0] == '1';
  }();
  if (s_tempReplayTail) {
    for (uint32_t i = 0; i < 48u; ++i) nd->dc.devTail[i] = Rd(dev + 0x2780 + 4 * i);
  }
  ProdMark(5);
  CopyBank(dev + kDevVsConst, nd->vs.data());
  CopyBank(dev + kDevPsConst, nd->ps.data());
  {
    const uint32_t dv = vs ? ApplyShaderDefs(vs + 872, Rd(vs + 32), false, nd->vs.data()) : 0;
    const uint32_t dp = ps ? ApplyShaderDefs(ps + 40, Rd(ps + 24), true, nd->ps.data()) : 0;
    static std::atomic<uint32_t> s_nd{0};
    const uint32_t q = s_nd.fetch_add(1) + 1;
    if (q <= 6 || (q % 50000) == 0)  // TEMP-DEFS-APPLY
      MCLA_LOG_WARN("TEMP-DEFS-APPLY #{} vs={:08X} defsVs={} ps={:08X} defsPs={} psC252.x(be)={:02X}{:02X}{:02X}{:02X}",
                    q, vs, dv, ps, dp, nd->ps[4032], nd->ps[4033], nd->ps[4034], nd->ps[4035]);
  }
  s_nBuilt.fetch_add(1);
  ProdMark(6);
  return nd;
}

// native mode: constants ride a seq-checked ring keyed by dc.frameId (unused by
// the consumer otherwise; PM4 leaves it 0).
struct ConstSlot { std::atomic<uint32_t> id{0}; uint8_t vs[kBank], ps[kBank]; };
constexpr uint32_t kConstSlots = 8192;  // must exceed CommandQueue capacity (4096): 1024 recycled ~97% of world draws' banks (dx3 TEMP-NATIVE-CONST-FALLBACK)
ConstSlot *const g_constRing =
    g_mode == mcla::gnh::Mode::kNative ? new ConstSlot[kConstSlots] : nullptr;
std::atomic<uint32_t> g_nativeSeq{0};
std::atomic<uint32_t> s_nBankDedup{0};

// both mode: native records waiting for their PM4 twin (PM4 drains the ring in
// guest order, so the twin is near the front).
std::mutex g_cmpMutex;
std::deque<std::unique_ptr<NativeDraw>> g_cmpQueue;

void Submit(std::unique_ptr<NativeDraw> nd, uint32_t dev = 0) {
  if (dev != 0u && TryRecordCbDraw(dev, nd)) {
    if (LeanCopy()) t_spareDraw = std::move(nd);
    return;
  }
  const uint32_t seq = g_nativeSeq.fetch_add(1) + 1;
  nd->dc.cpSeq = seq;
  if (g_mode == mcla::gnh::Mode::kBoth) {
    std::lock_guard<std::mutex> lk(g_cmpMutex);
    if (g_cmpQueue.size() >= 4096) {  // legal screen measured 1,628 draws/frame
      g_cmpQueue.pop_front();
      s_nCmpNativeOnly.fetch_add(1);
    }
    g_cmpQueue.push_back(std::move(nd));
    return;
  }
  // PERF (MCLA_NO_BANK_DEDUP=1 restores): a draw whose VS+PS banks equal this thread's previous
  // draw's reuses that ring slot's id, so the 8 KB copy is skipped here and the consumer sees a
  // repeated frameId (its s_nativeBankId fast path: no 8 KB copy-out, no re-hash). Reuse only
  // while fewer than 2048 seqs have passed, so the slot outlives the 4096-deep queue by >6000
  // allocations; FillNativeConstants still checks the id.
  static const bool s_noBankDedup = [] { const char *e = std::getenv("MCLA_NO_BANK_DEDUP"); return e && e[0] == '1'; }();
  static thread_local uint32_t t_lastSeq = 0;
  uint32_t id = 0;
  if (!s_noBankDedup && t_lastSeq != 0 && seq - t_lastSeq < 2048u) {
    const uint32_t lastId = 0x80000000u | (t_lastSeq & 0x7FFFFFFFu);
    ConstSlot &ls = g_constRing[(t_lastSeq & 0x7FFFFFFFu) % kConstSlots];
    if (ls.id.load(std::memory_order_acquire) == lastId &&
        std::memcmp(ls.ps, nd->ps.data(), kBank) == 0 && std::memcmp(ls.vs, nd->vs.data(), kBank) == 0 &&
        ls.id.load(std::memory_order_acquire) == lastId) {
      id = lastId;
      s_nBankDedup.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (id == 0) {
    id = 0x80000000u | (seq & 0x7FFFFFFFu);
    ConstSlot &s = g_constRing[(seq & 0x7FFFFFFFu) % kConstSlots];
    s.id.store(0, std::memory_order_release);
    std::memcpy(s.vs, nd->vs.data(), kBank);
    std::memcpy(s.ps, nd->ps.data(), kBank);
    s.id.store(id, std::memory_order_release);
    t_lastSeq = seq;
  }
  ProdMark(7);
  nd->dc.frameId = id;
  if (LeanCopy()) {
    rq::g_commandQueue.push_draw(nd->dc);
  } else {
    rq::RenderCommand cmd;
    cmd.type = rq::RenderCommand::DRAW_CAPTURED;
    cmd.data = nd->dc;
    rq::g_commandQueue.push(std::move(cmd));
  }
  ProdMark(8);
  const uint32_t n = s_nPushed.fetch_add(1) + 1;
  if (n % 100000 == 0) MCLA_LOG_INFO("GNH-BANK-DEDUP pushed={} reused={}", n, s_nBankDedup.load(std::memory_order_relaxed));
  if (n <= 8 || n % 5000 == 0) {
    const auto &dc = nd->dc;
    MCLA_LOG_INFO("GNH-P2-DRAW #{} prim={} numIdx={} vb={:08X}+{} stride={} fmtB={} "
                  "elem={} full={} vsProg={:08X}/sp{}/{}d psProg={:08X}/sp{}/{}d "
                  "tex={} rs[clip={:08X} sc={:08X} vte={:08X} color={:08X} blend={:08X} "
                  "mask={:08X} depth={:08X}]",
                  n, dc.primitiveTopology, dc.numIndices, dc.vbBase, dc.vbSize,
                  dc.vbStride, dc.vbFormat.bytes, dc.vbElementCount, dc.vbFullCount,
                  dc.vsProgram.guestAddr, dc.vsProgram.addrSpace, dc.vsProgram.dwords,
                  dc.psProgram.guestAddr, dc.psProgram.addrSpace, dc.psProgram.dwords,
                  dc.textureCount, dc.rasterState.regs[0], dc.rasterState.regs[1],
                  dc.rasterState.regs[2], dc.rasterState.regs[8], dc.rasterState.regs[9],
                  dc.rasterState.regs[10], dc.rasterState.regs[11]);
  }
  if (LeanCopy()) t_spareDraw = std::move(nd);
}

std::atomic<uint32_t> s_nIdxProduced{0};
void Produce(uint32_t dev, uint32_t prim, uint32_t count, uint32_t idxOffset,
             const ImmStream &imm, const IdxSrc &ix = IdxSrc{}) {
  LARGE_INTEGER tp0; const bool tp = mcla::gnh::TempProducerOn(); if (tp) QueryPerformanceCounter(&tp0);
  struct TpEnd { LARGE_INTEGER s; bool on; ~TpEnd() { if (on) { LARGE_INTEGER e; QueryPerformanceCounter(&e); mcla::gnh::ProdTime(0, e.QuadPart - s.QuadPart); } } } tpEnd{tp0, tp};
  {  // MD-19 census (log-only): prim histogram at the native producer (does prim=1 ever arrive here?)
    static std::atomic<uint64_t> s_prim[32]{};
    static std::atomic<uint64_t> s_pt{0};
    s_prim[prim & 31u].fetch_add(1, std::memory_order_relaxed);
    const uint64_t pt = s_pt.fetch_add(1, std::memory_order_relaxed) + 1;
    if (pt == 1 || (pt % 500000) == 0) {
      std::string h;
      for (uint32_t i = 0; i < 32; ++i) if (const uint64_t c = s_prim[i].load()) h += fmt::format(" p{}={}", i, c);
      MCLA_LOG_INFO("GNH-PRIM-HIST n={}{}", pt, h);
    }
  }
  ProdMark(-1);
  if (auto nd = BuildDraw(dev, prim, count, idxOffset, imm, ix)) {
    Submit(std::move(nd), dev);
    if (ix.bytes) {
      const uint32_t n = s_nIdxProduced.fetch_add(1) + 1;
      if (n % 2000 == 0) MCLA_LOG_INFO("GNH-IDXPROD #{} unproduced={} refused={}", n,
                                       s_nIdxUnproduced.load(), s_nRefused.load());
    }
  }
}

void Miss(uint32_t f, uint32_t nat, uint32_t pm4) {
  const uint32_t n = s_fieldMiss[f].fetch_add(1) + 1;
  if (n <= 4 || n % 2000 == 0)
    MCLA_LOG_WARN("GNH-CMP-MISS #{} field={} native={:08X} pm4={:08X}", n,
                  kFieldNames[f], nat, pm4);
}

uint32_t Fnv(const void *p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t *>(p)[i]) * 16777619u;
  return h;
}

void Compare(const NativeDraw &nd, const rq::DrawCapturedCommand &p) {
  const rq::DrawCapturedCommand &n = nd.dc;
  bool clean = true;
  auto chk = [&](uint32_t f, uint32_t a, uint32_t b) {
    if (a != b) { clean = false; Miss(f, a, b); }
  };
  chk(0, n.vbBase, p.vbBase);
  chk(1, n.vbSize, p.vbSize);
  chk(2, n.vbStride, p.vbStride);
  chk(3, n.vbFormat.xenos, p.vbFormat.xenos);
  chk(4, n.vbElementCount, p.vbElementCount);
  chk(5, Fnv(n.vbElements, sizeof n.vbElements), Fnv(p.vbElements, sizeof p.vbElements));
  chk(6, Fnv(n.vbStreams, sizeof n.vbStreams), Fnv(p.vbStreams, sizeof p.vbStreams));
  chk(7, n.primitiveTopology, p.primitiveTopology);
  chk(8, n.numIndices, p.numIndices);
  chk(9, n.idxOffset, p.idxOffset);
  chk(10, n.initiator, p.initiator);
  chk(11, n.vsProgram.guestAddr, p.vsProgram.guestAddr);
  chk(12, Fnv(n.vsProgram.body, sizeof n.vsProgram.body), Fnv(p.vsProgram.body, sizeof p.vsProgram.body));
  chk(13, n.psProgram.guestAddr, p.psProgram.guestAddr);
  chk(14, Fnv(n.psProgram.body, sizeof n.psProgram.body), Fnv(p.psProgram.body, sizeof p.psProgram.body));
  for (uint32_t i = 0; i < rq::kCpSnapRegCount; ++i)
    if ((p.rasterState.presentMask >> i) & 1u)
      chk(15, (i << 24) ^ n.rasterState.regs[i], (i << 24) ^ p.rasterState.regs[i]);
  chk(16, n.textureCount, p.textureCount);
  chk(17, Fnv(n.textureDescriptorBytes, sizeof n.textureDescriptorBytes),
      Fnv(p.textureDescriptorBytes, sizeof p.textureDescriptorBytes));
  chk(18, Fnv(n.texturePoolVa, sizeof n.texturePoolVa), Fnv(p.texturePoolVa, sizeof p.texturePoolVa));
  chk(21, Fnv(n.texturePoolVaMip, sizeof n.texturePoolVaMip), Fnv(p.texturePoolVaMip, sizeof p.texturePoolVaMip));
  static thread_local std::array<uint8_t, kBank> cv, cp;
  if (mcla::gpu::CpBuildShaderConstantBank(mcla::gpu::CpShaderStage::kVertex, cv.data(), kBank, nullptr))
    chk(19, Fnv(nd.vs.data(), kBank), Fnv(cv.data(), kBank));
  if (mcla::gpu::CpBuildShaderConstantBank(mcla::gpu::CpShaderStage::kPixel, cp.data(), kBank, nullptr))
    chk(20, Fnv(nd.ps.data(), kBank), Fnv(cp.data(), kBank));
  if (!clean && s_fieldMiss[19].load() >= 1 && s_fieldMiss[19].load() <= 3) {
    // the first constant disagreements, as words (vs c0..c3 and ps c0..c3)
    auto w = [](const uint8_t *b, int i) {
      return uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 |
             uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
    };
    std::string t;
    auto diff = [&](const char *tag, const uint8_t *n, const uint8_t *c) {
      uint32_t cnt = 0;
      for (int e = 0; e < 256; ++e) {
        if (std::memcmp(n + 16 * e, c + 16 * e, 16) == 0) continue;
        if (cnt++ < 6)
          t += fmt::format(" {}c{}=[{:08X} {:08X} {:08X} {:08X}]/[{:08X} {:08X} {:08X} {:08X}]",
                           tag, e, w(n, 4 * e), w(n, 4 * e + 1), w(n, 4 * e + 2),
                           w(n, 4 * e + 3), w(c, 4 * e), w(c, 4 * e + 1),
                           w(c, 4 * e + 2), w(c, 4 * e + 3));
      }
      t += fmt::format(" {}DiffEntries={}", tag, cnt);
    };
    diff("vs", nd.vs.data(), cv.data());
    diff("ps", nd.ps.data(), cp.data());
    MCLA_LOG_WARN("GNH-CMP-CONST native/pm4{}", t);
  }
  s_nCmpMatched.fetch_add(1);
  if (clean) s_nCmpClean.fetch_add(1);
}

// BeginVertices -> EndVertices hand-off (vertex data is complete at EndVertices).
thread_local ImmStream t_imm;
thread_local uint32_t t_immDev = 0, t_immPrim = 0, t_immCount = 0;

void LogSum(uint32_t swaps) {
  static uint32_t lastSwaps = 0, lastPushed = 0, lastBridged = 0, lastBuilt = 0;
  const uint32_t built = s_nBuilt.load(), pushed = s_nPushed.load(),
                 bridged = s_nPm4Bridged.load();
  const uint32_t frames = swaps > lastSwaps ? swaps - lastSwaps : 1;
  std::string miss;
  for (uint32_t f = 0; f < kFields; ++f)
    if (uint32_t m = s_fieldMiss[f].load())
      miss += std::string(kFieldNames[f]) + "=" + std::to_string(m) + " ";
  MCLA_LOG_INFO("GNH-P2-SUM mode={} swaps={} native built={} pushed={} refused={} "
                "idxUnproduced={} idxProduced={} | pm4 bridged={} suppressed={} | perFrame "
                "nativeBuilt={:.1f} nativePushed={:.1f} pm4Bridged={:.1f} | cmp "
                "matched={} clean={} nativeOnly={} pm4Only={} miss[{}]",
                static_cast<uint32_t>(g_mode), swaps, built, pushed,
                s_nRefused.load(), s_nIdxUnproduced.load(), s_nIdxProduced.load(), bridged,
                s_nPm4Suppressed.load(), double(built - lastBuilt) / frames,
                double(pushed - lastPushed) / frames,
                double(bridged - lastBridged) / frames, s_nCmpMatched.load(),
                s_nCmpClean.load(), s_nCmpNativeOnly.load(), s_nCmpPm4Only.load(),
                miss);
  lastSwaps = swaps; lastPushed = pushed; lastBridged = bridged; lastBuilt = built;
}

} // namespace

namespace mcla::gnh {

Mode GetMode() { return g_mode; }
bool Pm4SubmitsDraws() { return g_mode != Mode::kNative; }
bool TempProducerOn() {
  static const bool on = [] {
    const char *e = std::getenv("MCLA_TEMP_PRODUCER");
    const char *p = std::getenv("MCLA_PERF_STAGES");
    return (e && e[0] == '1') || (p && p[0] == '1');
  }();
  return on;
}
void ProdTime(int which, long long ticks) {
  static std::atomic<long long> t[2]{}; static std::atomic<uint32_t> n[2]{}; static std::atomic<long long> last{0};
  t[which].fetch_add(ticks, std::memory_order_relaxed); n[which].fetch_add(1, std::memory_order_relaxed);
  LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
  long long l = last.load(std::memory_order_relaxed);
  if (l == 0) { last.store(c.QuadPart); return; }
  if (c.QuadPart - l < 2 * f.QuadPart || !last.compare_exchange_strong(l, c.QuadPart)) return;
  const long long t0 = t[0].exchange(0), t1 = t[1].exchange(0); const uint32_t n0 = n[0].exchange(0), n1 = n[1].exchange(0);
  MCLA_LOG_ERROR("TEMP-PRODUCER native: {} draws {:.1f} us/1000  pm4bridge: {} draws {:.1f} us/1000", n0,
                 n0 ? 1e9 * t0 / f.QuadPart / n0 : 0.0, n1, n1 ? 1e9 * t1 / f.QuadPart / n1 : 0.0);
  if (n0) {  // per-stage ns/draw: alloc vs ps elems rt tex banks/defs constring push
    double s[kProdStages];
    for (int i = 0; i < kProdStages; ++i) s[i] = 1e9 * g_prodStage[i].exchange(0) / f.QuadPart / n0;
    MCLA_LOG_ERROR("TEMP-PRODUCER-STAGES ns/draw alloc={:.0f} vs={:.0f} ps={:.0f} elems={:.0f} rt={:.0f} tex={:.0f} banks={:.0f} constring={:.0f} push={:.0f}",
                   s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8]);
  }
}

void OnResolve(uint32_t dev, uint32_t flags, uint32_t rectVa, uint32_t destPointVa) {
  if (g_mode != Mode::kNative || dev == 0) return;
  static std::atomic<uint32_t> s_seq{0};
  const uint32_t n = s_seq.fetch_add(1) + 1;
  rq::ResolveCommand rc{};
  rc.flags = flags;
  rc.surfaceInfo = Rd(dev + 10368);
  const uint32_t rtIdx = flags & 7u;  // 0-3 colour, 4 = depth
  rc.colorInfo = rtIdx == 0 ? Rd(dev + 10372) : (rtIdx <= 3 ? Rd(dev + 10380 + 4 * (rtIdx - 1)) : Rd(dev + 10372));
  rc.depthInfo = Rd(dev + 10376);
  rc.scissorBr = Rd(dev + 10428);
  rc.copyControl = Rd(dev + 10776);
  rc.copyDestBase = Rd(dev + 10780);
  rc.copyDestPitch = Rd(dev + 10784);
  // sub_82420BA8 captures dev+10788 across __imp__sub_82420BA8 before the guest clears it;
  // fall back to the CP register shadow only if the device shadow still reads 0.
  rc.copyDestInfo = Rd(dev + 10788);
  if (rc.copyDestInfo == 0u)
    rc.copyDestInfo = mcla::gpu::CpGuestRenderTargetInfo().copyDestInfo;
  // MEASURED 2026-11-12: dev+10700..10880 never holds RB_COPY_DEST_INFO (only base/pitch), and on the native
  // path the CP file reads 0 too (CP-REGS-EXPORT present=0): rc.copyDestInfo is 0 until a source is found.
  rc.depthClear = Rd(dev + 10796);
  rc.colorClear[0] = Rd(dev + 10800);
  rc.colorClear[1] = Rd(dev + 10804);
  if (rectVa != 0) {
    rc.hasRect = 1;
    for (uint32_t i = 0; i < 4; ++i) rc.rect[i] = static_cast<int32_t>(Rd(rectVa + 4 * i));
  }
  if (destPointVa != 0) {
    rc.destX = static_cast<int32_t>(Rd(destPointVa));
    rc.destY = static_cast<int32_t>(Rd(destPointVa + 4));
  }
  rc.seq = n;
  if (TryRecordCbResolve(dev, rc)) return;
  if (n <= 12 || (n % 1000) == 0)  // TEMP-RESOLVE-PRODUCE: shadow offsets vs. TEMP-3DP-COPY rows
    MCLA_LOG_WARN("TEMP-RESOLVE-PRODUCE #{} flags={:X} surf={:08X} color={:08X} depth={:08X} copyCtl={:08X} destBase={:08X} destPitch={:08X} destInfo={:08X} rect={}({},{},{},{}) dest=({},{}) clr={:08X}/{:08X} dclr={:08X}",
                  n, flags, rc.surfaceInfo, rc.colorInfo, rc.depthInfo, rc.copyControl, rc.copyDestBase,
                  rc.copyDestPitch, rc.copyDestInfo, rc.hasRect, rc.rect[0], rc.rect[1], rc.rect[2], rc.rect[3],
                  rc.destX, rc.destY, rc.colorClear[0], rc.colorClear[1], rc.depthClear);
  rq::RenderCommand cmd;
  cmd.type = rq::RenderCommand::RESOLVE_GUEST;
  cmd.data = rc;
  rq::g_commandQueue.push(std::move(cmd));
}

// Guest D3DDevice_Clear/ClearF -> CLEAR_GUEST (kill-switch MCLA_NO_GUEST_CLEAR=1). Called AFTER the original.
void OnClear(uint32_t dev, uint32_t flags, const float color[4], float z, uint32_t stencil, uint32_t rectVa,
             uint32_t count) {
  static const bool off = [] { const char *e = std::getenv("MCLA_NO_GUEST_CLEAR"); return e && e[0] == '1'; }();
  if (off || g_mode != Mode::kNative || dev == 0) return;
  static std::atomic<uint32_t> s_seq{0};
  rq::ClearCommand cc{};
  cc.flags = flags;
  cc.surfaceInfo = Rd(dev + 10368);
  cc.colorInfo = Rd(dev + 10372);
  cc.depthInfo = Rd(dev + 10376);
  cc.scissorBr = Rd(dev + 10428);
  std::memcpy(cc.color, color, sizeof cc.color);
  cc.z = z;
  cc.stencil = stencil;
  if (rectVa != 0 && count != 0) {
    cc.nRects = count > 8 ? 8 : count;  // shortcut: first 8 rects only, none seen >1 yet
    for (uint32_t r = 0; r < cc.nRects; ++r)
      for (uint32_t i = 0; i < 4; ++i) cc.rect[r][i] = static_cast<int32_t>(Rd(rectVa + 16 * r + 4 * i));
  }
  cc.seq = s_seq.fetch_add(1) + 1;
  if (TryRecordCbClear(dev, cc)) return;
  rq::RenderCommand cmd;
  cmd.type = rq::RenderCommand::CLEAR_GUEST;
  cmd.data = cc;
  rq::g_commandQueue.push(std::move(cmd));
}

bool Pm4BridgeDraw(const mcla::native::DrawCapturedCommand &dc) {
  s_nPm4Bridged.fetch_add(1);
  {  // TEMP-3DP-COPY: PM4 draws by EDRAM mode (native mode suppresses them all)
    static std::atomic<uint32_t> byMode[8]; static std::atomic<uint32_t> nLog{0};
    const auto gt = mcla::gpu::CpGuestRenderTargetInfo();
    const uint32_t mm = byMode[gt.edramMode & 7].fetch_add(1) + 1;
    if (gt.edramMode == 6 || gt.edramMode == 5) {
      struct C { uint32_t base, pitch, info, ctl, cb, cf, sp; };
      static std::mutex m; static std::vector<std::pair<C, uint32_t>> tab;
      const C c{gt.copyDestBase, gt.copyDestPitch, gt.copyDestInfo, gt.copyControl, gt.colorBase, gt.colorFormat, gt.surfacePitch};
      const uint32_t q = nLog.fetch_add(1) + 1;
      std::lock_guard<std::mutex> lk(m);
      bool f = false;
      for (auto &e : tab) if (!std::memcmp(&e.first, &c, sizeof c)) { ++e.second; f = true; break; }
      if (!f && tab.size() < 80) {
        tab.push_back({c, 1});
        MCLA_LOG_WARN("TEMP-3DP-COPY new#{} at copy {} mode={} destBase={:08X} destPitch={:08X}(w={} h={}) destInfo={:08X} copyCtl={:08X} srcColorBase={} srcFmt={} surfPitch={} suppressed={}",
          tab.size(), q, gt.edramMode, c.base, c.pitch, c.pitch & 0x3FFF, (c.pitch >> 16) & 0x3FFF, c.info, c.ctl, c.cb, c.cf, c.sp, g_mode == Mode::kNative ? 1 : 0);
      }
      if (q % 100 == 0)
        for (size_t k = 0; k < tab.size(); ++k)
          MCLA_LOG_WARN("TEMP-3DP-COPY-TAB copies={} destBase={:08X} info={:08X} srcBase={} srcFmt={} count={}", q, tab[k].first.base, tab[k].first.info, tab[k].first.cb, tab[k].first.cf, tab[k].second);
    }
    (void)mm;
  }
  if (g_mode == Mode::kNative) {
    s_nPm4Suppressed.fetch_add(1);
    return false;
  }
  if (g_mode == Mode::kBoth && dc.opcode == 0x22u) {
    // Twin = first queued native record with the same stream + shape; older
    // unmatched ones are native-only (e.g. a draw PM4 refused).
    std::unique_ptr<NativeDraw> twin;
    {
      std::lock_guard<std::mutex> lk(g_cmpMutex);
      for (size_t i = 0; i < g_cmpQueue.size(); ++i) {
        const auto &c = g_cmpQueue[i]->dc;
        if (c.vbBase == dc.vbBase && c.numIndices == dc.numIndices &&
            c.primitiveTopology == dc.primitiveTopology) {
          twin = std::move(g_cmpQueue[i]);
          s_nCmpNativeOnly.fetch_add(static_cast<uint32_t>(i));
          g_cmpQueue.erase(g_cmpQueue.begin(), g_cmpQueue.begin() + i + 1);
          break;
        }
      }
    }
    if (twin) Compare(*twin, dc);
    else s_nCmpPm4Only.fetch_add(1);
  }
  return true;
}

bool FillNativeConstants(uint32_t frameId, uint8_t *vsOut, uint8_t *psOut,
                         uint32_t bankBytes) {
  if (!g_constRing || !(frameId & 0x80000000u) || bankBytes != kBank) return false;
  ConstSlot &s = g_constRing[(frameId & 0x7FFFFFFFu) % kConstSlots];
  if (s.id.load(std::memory_order_acquire) != frameId) return false;
  std::memcpy(vsOut, s.vs, kBank);
  std::memcpy(psOut, s.ps, kBank);
  // recycled while copying -> refuse (the caller falls back to the CP bank)
  return s.id.load(std::memory_order_acquire) == frameId;
}

static std::atomic<float> g_lastZClear{1.0f};
float LastGuestZClear() { return g_lastZClear.load(std::memory_order_relaxed); }
static void NoteZClear(double z) {
  if (z >= 0.0 && z <= 1.0) g_lastZClear.store(static_cast<float>(z), std::memory_order_relaxed);
}

} // namespace mcla::gnh

// D3DDevice_Clear(dev, Count, pRects, Flags, Color, Z, Stencil, EDRAMClear)
PPC_FUNC_IMPL(__imp__sub_824195E8);
PPC_FUNC(sub_824195E8) {
  const uint32_t n = s_clear.fetch_add(1) + 1;
  if (Cadence(n))
    MCLA_LOG_INFO("GNH-CLEAR #{} dev={:08X} count={} rects={:08X} flags={:X} "
                  "color={:08X} z={} stencil={:X} edram={} lr={:08X}",
                  n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                  ctx.f1.f64, ctx.r9.u32, ctx.r10.u32, (uint32_t)ctx.lr);
  mcla::gnh::NoteZClear(ctx.f1.f64);
  const uint32_t dev = ctx.r3.u32, cnt = ctx.r4.u32, rects = ctx.r5.u32, flags = ctx.r6.u32, argb = ctx.r7.u32,
                 stencil = ctx.r9.u32;
  const float z = static_cast<float>(ctx.f1.f64);
  const float col[4] = {((argb >> 16) & 0xFFu) / 255.0f, ((argb >> 8) & 0xFFu) / 255.0f, (argb & 0xFFu) / 255.0f,
                        ((argb >> 24) & 0xFFu) / 255.0f};
  __imp__sub_824195E8(ctx, base);
  mcla::gnh::OnClear(dev, flags, col, z, stencil, rects, cnt);
}

// D3DDevice_ClearF(dev, Flags, pRect, pColor(float4), Z, Stencil)
PPC_FUNC_IMPL(__imp__sub_824194C0);
PPC_FUNC(sub_824194C0) {
  const uint32_t n = s_clearF.fetch_add(1) + 1;
  if (Cadence(n)) {
    const uint32_t dev = ctx.r3.u32, rect = ctx.r5.u32, col = ctx.r6.u32;
    MCLA_LOG_INFO("GNH-CLEARF #{} dev={:08X} flags={:X} rect={:08X}[{},{},{},{}] "
                  "color=({:.3f},{:.3f},{:.3f},{:.3f}) z={} stencil={:X} "
                  "rt0={:08X} ds={:08X}",
                  n, dev, ctx.r4.u32, rect, Rd(rect), Rd(rect + 4), Rd(rect + 8),
                  Rd(rect + 12), RdF(col), RdF(col + 4), RdF(col + 8),
                  RdF(col + 12), ctx.f1.f64, ctx.r8.u32, Rd(dev + 12440),
                  Rd(dev + 12456));
  }
  mcla::gnh::NoteZClear(ctx.f1.f64);
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, pc = ctx.r6.u32, stencil = ctx.r8.u32;
  const float z = static_cast<float>(ctx.f1.f64);
  const float col[4] = {pc ? RdF(pc) : 0.f, pc ? RdF(pc + 4) : 0.f, pc ? RdF(pc + 8) : 0.f, pc ? RdF(pc + 12) : 0.f};
  __imp__sub_824194C0(ctx, base);
  mcla::gnh::OnClear(dev, flags, col, z, stencil, rect, rect ? 1u : 0u);
}

// D3DDevice_DrawVertices(dev, PrimitiveType, StartVertex, VertexCount)
PPC_FUNC_IMPL(__imp__sub_8241D230);
PPC_FUNC(sub_8241D230) {
  const uint32_t n = s_draw.fetch_add(1) + 1;
  if (Cadence(n)) {
    const uint32_t dev = ctx.r3.u32, vb0 = Rd(dev + 12460);
    MCLA_LOG_INFO("GNH-DRAW #{} dev={:08X} prim={} start={} count={} "
                  "vb0={:08X}(base={:08X} size={:08X}) vs={:08X} ps={:08X} lr={:08X}",
                  n, dev, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, vb0, Rd(vb0 + 24),
                  Rd(vb0 + 28), Rd(dev + 12696), Rd(dev + 12692),
                  (uint32_t)ctx.lr);
  }
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r5.u32,
                 cnt = ctx.r6.u32;
  __imp__sub_8241D230(ctx, base);
  // GNH-P2: VGT_INDX_OFFSET = StartVertex (map §1). Unverified: 0 firings so far.
  // shortcut: counts > 0xFFFF are split by the guest body; produced as one draw.
  if (Producing() && cnt != 0) Produce(dev, prim, cnt, start, ImmStream{});
}

// D3DDevice_DrawIndexedVertices(dev, PrimitiveType, BaseVertexIndex,
//                               StartIndex, IndexCount)
PPC_FUNC_IMPL(__imp__sub_8241D620);
PPC_FUNC(sub_8241D620) {
  const uint32_t n = s_drawIdx.fetch_add(1) + 1;
  if (Cadence(n)) {
    const uint32_t dev = ctx.r3.u32, ib = Rd(dev + 12436), vb0 = Rd(dev + 12460);
    MCLA_LOG_INFO("GNH-DRAWIDX #{} dev={:08X} prim={} baseVtx={} startIdx={} "
                  "idxCount={} ib={:08X}(hdr={:08X} base={:08X}) "
                  "vb0={:08X}(base={:08X} size={:08X}) vs={:08X} ps={:08X} lr={:08X}",
                  n, dev, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ib,
                  Rd(ib), Rd(ib + 24), vb0, Rd(vb0 + 24), Rd(vb0 + 28),
                  Rd(dev + 12696), Rd(dev + 12692), (uint32_t)ctx.lr);
  }
  // GNH-P2: args per decompile of sub_8241D620: a2=prim, a3=BaseVertexIndex
  // (-> VGT_INDX_OFFSET 0x2102), a4=StartIndex, a5=IndexCount. IB object: hdr bit31
  // = 32-bit, swap = (hdr>>29)&3, base = ib[6]; address = XdkPhys(base+start*elem).
  const uint32_t pDev = ctx.r3.u32, pPrim = ctx.r4.u32, pBaseV = ctx.r5.u32,
                 pStart = ctx.r6.u32, pCnt = ctx.r7.u32;
  IdxSrc ix;
  bool ixOk = false;
  if (Producing()) {
    const uint32_t ib = Rd(pDev + 12436), hdr = Rd(ib);
    ix.wide = hdr >> 31;
    ix.swap = (hdr >> 29) & 3u;
    const uint32_t el = ix.wide ? 4u : 2u;
    ix.bytes = pCnt * el;
    ix.va = ib ? PhysToPool(XdkPhys(Rd(ib + 24) + pStart * el)) : 0u;
    ixOk = pCnt != 0 && ix.va != 0 && ix.bytes >= el;
    static std::atomic<uint32_t> s_h{0};
    const uint32_t h = s_h.fetch_add(1) + 1;
    if (h <= 12 || h % 100000 == 0)
      MCLA_LOG_INFO("GNH-IDXHDR #{} ib={:08X} hdr={:08X} wide={} swap={} base={:08X} "
                    "start={} cnt={} baseV={} -> pool={:08X}",
                    h, ib, hdr, ix.wide, ix.swap, Rd(ib + 24), pStart, pCnt,
                    static_cast<int32_t>(pBaseV), ix.va);
    if (!ixOk) s_nIdxUnproduced.fetch_add(1);
  }
  const uint32_t t3dDev = ctx.r3.u32, t3dBefore = Rd(t3dDev + 48);
  __imp__sub_8241D620(ctx, base);
  if (ixOk) Produce(pDev, pPrim, pCnt, pBaseV, ImmStream{}, ix);
  {  // TEMP-3D-IDXRING: where does the guest body put the DRAW_INDX packet?
    static std::atomic<uint32_t> s_k{0};
    const uint32_t k = s_k.fetch_add(1) + 1;
    if (k <= 12 || (k % 50000) == 0) {
      const uint32_t after = Rd(t3dDev + 48);
      MCLA_LOG_INFO("TEMP-3D-IDXRING #{} cur_before={:08X} cur_after={:08X} delta_dw={} dev+10940={:02X} "
                    "dev+13248..={:08X} {:08X} {:08X} | at before: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                    k, t3dBefore, after, (int32_t)(after - t3dBefore) / 4, Rd(t3dDev + 10940) >> 24,
                    Rd(t3dDev + 13248), Rd(t3dDev + 13252), Rd(t3dDev + 13256),
                   
                    Rd(t3dBefore), Rd(t3dBefore + 4), Rd(t3dBefore + 8), Rd(t3dBefore + 12),
                    Rd(t3dBefore + 16), Rd(t3dBefore + 20), Rd(t3dBefore + 24), Rd(t3dBefore + 28));
    }
  }
}

// D3DDevice_BeginVertices(dev, PrimitiveType, VertexCount, Stride) -> void*
PPC_FUNC_IMPL(__imp__sub_8241CD88);
PPC_FUNC(sub_8241CD88) {
  const uint32_t n = s_beginV.fetch_add(1) + 1;
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, cnt = ctx.r5.u32,
                 stride = ctx.r6.u32, lr = (uint32_t)ctx.lr;
  __imp__sub_8241CD88(ctx, base);
  // GNH-P2: the guest fills ret[0..count*stride) next; EndVertices closes it.
  if (Producing()) {
    t_imm = ImmStream{ctx.r3.u32, cnt * stride, ctx.r3.u32 != 0, stride};
    t_immDev = dev; t_immPrim = prim; t_immCount = cnt;
  }
  if (Cadence(n))
    MCLA_LOG_INFO("GNH-BEGINV #{} dev={:08X} prim={} count={} stride={} "
                  "ret={:08X} lr={:08X}",
                  n, dev, prim, cnt, stride, ctx.r3.u32, lr);
}

// D3DDevice_EndVertices(dev): body is dev[+48] = dev[+13444] (map §1).
// GNH-P2 only; in pm4 mode it is a pure pass-through.
PPC_FUNC_IMPL(__imp__sub_8241D220);
PPC_FUNC(sub_8241D220) {
  const uint32_t dev = ctx.r3.u32;
  __imp__sub_8241D220(ctx, base);
  if (Producing() && t_imm.on && dev == t_immDev) {
    const uint32_t built0 = s_nBuilt.load();
    Produce(dev, t_immPrim, t_immCount, 0u, t_imm);
    if (t_imm.stride == 108u) {  // PTFX-CENSUS (log-only): rmptfx BeginVertices stride (sub_825F82A8/F8 `li r5,108`)
      static std::atomic<uint32_t> s_n{0}, s_b{0};
      static std::mutex m; static std::vector<uint64_t> seen;
      const uint32_t ok = s_nBuilt.load() != built0 ? 1u : 0u;  // ~: another producer thread could build in between
      const uint32_t n = s_n.fetch_add(1) + 1, b = s_b.fetch_add(ok) + ok;
      const uint32_t vs = Rd(dev + kDevVs), ps = Rd(dev + kDevPs), bl = Rd(dev + 10552), dc = Rd(dev + 10548), cm = Rd(dev + 10460);
      const uint64_t key = (uint64_t(vs) << 32 | ps) ^ (uint64_t(bl) * 0x9E3779B97F4A7C15ull) ^ dc ^ (uint64_t(cm) << 17);
      bool fresh = false;
      { std::lock_guard<std::mutex> lk(m); if (seen.size() < 32 && std::find(seen.begin(), seen.end(), key) == seen.end()) { seen.push_back(key); fresh = true; } }
      if (fresh || n % 200 == 0)
        MCLA_LOG_WARN("PTFX-CENSUS n={} built~{} prim={} cnt={} vs={:08X} ps={:08X} blend0={:08X} depthCtl={:08X} colorMask={:08X} colorInfo={:08X} vte={:08X} vsC27=({},{},{},{}) psC128=({},{},{},{}) psC255=({},{},{},{}) vtx0col={:08X}{}",
                      n, b, t_immPrim, t_immCount, vs, ps, bl, dc, cm, Rd(dev + 10372), Rd(dev + 10572),
                      RdF(dev + kDevVsConst + 27 * 16), RdF(dev + kDevVsConst + 27 * 16 + 4), RdF(dev + kDevVsConst + 27 * 16 + 8), RdF(dev + kDevVsConst + 27 * 16 + 12),
                      RdF(dev + kDevPsConst + 128 * 16), RdF(dev + kDevPsConst + 128 * 16 + 4), RdF(dev + kDevPsConst + 128 * 16 + 8), RdF(dev + kDevPsConst + 128 * 16 + 12),
                      RdF(dev + kDevPsConst + 255 * 16), RdF(dev + kDevPsConst + 255 * 16 + 4), RdF(dev + kDevPsConst + 255 * 16 + 8), RdF(dev + kDevPsConst + 255 * 16 + 12),
                      Rd(t_imm.va + 24), fresh ? " NEW" : "");
    }
    t_imm.on = false;
  }
}

// D3DDevice_Swap(dev, pFrontBuffer, pScalerParameters)
PPC_FUNC_IMPL(__imp__sub_82419E98);
PPC_FUNC(sub_82419E98) {
  const uint32_t n = s_swap.fetch_add(1) + 1;
  if (n <= 16 || (n % 300) == 0) {
    const uint32_t dev = ctx.r3.u32, fb = ctx.r4.u32;
    MCLA_LOG_INFO("GNH-SWAP #{} dev={:08X} front={:08X}(fetch+32={:08X} +36={:08X}) "
                  "params={:08X} devFrame={} lr={:08X}",
                  n, dev, fb, Rd(fb + 32), Rd(fb + 36), ctx.r5.u32,
                  Rd(dev + 16560), (uint32_t)ctx.lr);
    MCLA_LOG_INFO("GNH-SUM swaps={} clears={} clearFs={} draws={} drawIdx={} "
                  "beginV={}",
                  n, s_clear.load(), s_clearF.load(), s_draw.load(),
                  s_drawIdx.load(), s_beginV.load());
    if (Producing()) LogSum(n);
  }

  // Wave 3 (§1.6) live sRGB/TV toggle. VdGetCurrentDisplayGamma is called by
  // the guest at boot, so reading the CVar HERE is what makes a pause-menu
  // change observable. D3D12Backend has no hardware gamma-ramp application:
  // its only gamma-related code is sRGB *texture views*
  // (d3d12_backend.cpp:3874-3887), so no ramp is applied or claimed here —
  // report the status instead of inventing one (spec §1.6).
  const std::string_view wantGamma =
      mcla::cvar::CVarSystem::Instance().GetString("mcla_display_gamma");
  const uint32_t wantedType =
      (wantGamma == "pc" || wantGamma == "PC" ||
       wantGamma == "srgb" || wantGamma == "sRGB") ? 1u : 2u;
  const uint32_t appliedType =
      mcla::gamma::g_appliedType.load(std::memory_order_relaxed);
  if (wantedType != appliedType) {
    static std::atomic<uint32_t> s_gammaRows{0};
    if (s_gammaRows.fetch_add(1) < 8)
      MCLA_LOG_INFO("DISPLAY-GAMMA swap #{} wantedType={} appliedType={} "
                    "hardwareRamp=NONE (backend applies no ramp; guest ramp "
                    "rebuild not attempted)", n, wantedType, appliedType);
  }

  __imp__sub_82419E98(ctx, base);
}

// Per-object recorded command-buffer cache (sub_82323808: records a 0x18000 segment, replays it via
// RunCommandBuffer sub_82178AC8 -> sub_8241B6F0, re-records every ~5 frames). byte_8288D5D1 enables it and is
// set only by this static initializer; with it 0 the guest's own disabled branch draws every object directly
// (sub_82323808 returns 1 with a1[66]=-1, sub_82322DE0 returns 0).
// MEASURED 2026-11-17 (reg_fA vs reg_fB2, 12 consecutive presents, pier title scene): with the cache on, parked/traffic
// cars vanish on the frames that replay a recorded buffer (the native path does not draw RunCommandBuffer replays) and
// its 0x18000 segment allocs are refused ~1,200/s once physical memory is full; with it off every car is stable and
// 0 refusals. Default: off. MCLA_CMDBUF_CACHE=1 restores the stock (cache on) behaviour.
PPC_FUNC_IMPL(__imp__sub_82322C78);
PPC_FUNC(sub_82322C78) {
  static const bool off = [] { const char* e = std::getenv("MCLA_CMDBUF_CACHE"); return !(e && e[0] == '1'); }();
  if (off) { MCLA_LOG_ERROR("CMDBUF-CACHE disabled (byte_8288D5D1 stays 0)"); return; }
  __imp__sub_82322C78(ctx, base);
}

// Native Command Buffer HLE: BeginCommandBuffer / EndCommandBuffer / RunCommandBuffer.
// Captures native RenderCommands during Begin..End (recording per-draw constant banks and
// inherit masks) and replays them on RunCommandBuffer without PM4 emulation. MCLA_NO_NATIVE_CB=1 disables.
PPC_FUNC_IMPL(__imp__sub_8241ABB8);
PPC_FUNC(sub_8241ABB8) {
  const uint32_t dev = ctx.r3.u32;
  const uint32_t pCB = ctx.r4.u32;
  __imp__sub_8241ABB8(ctx, base);
  if (mcla::gnh::GetMode() == mcla::gnh::Mode::kNative && NativeCbEnabled() && dev != 0u && pCB != 0u) {
    std::lock_guard<std::mutex> lk(s_cbMtx);
    RecordedCbEntry &e = s_cbMap[pCB];
    e.dev = dev;
    e.inheritVsMask = ReadGuestU64BE(pCB + 24u);
    e.inheritPsMask = ReadGuestU64BE(pCB + 32u);
    e.recording = true;
    e.sealed = false;
    e.ops.clear();
  }
}

PPC_FUNC_IMPL(__imp__sub_8241B6F0);
PPC_FUNC(sub_8241B6F0) {
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1) + 1;
  const uint32_t dev = ctx.r3.u32;
  const uint32_t pCB = ctx.r4.u32;
  const uint32_t pred = ctx.r5.u32;
  if (n <= 8 || (n % 500) == 0) {
    MCLA_LOG_INFO("TEMP-3D-CB run #{} dev={:08X} cb={:08X} cb[0..7]={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} pred={} lr={:08X}",
                  n, dev, pCB, Rd(pCB), Rd(pCB + 4), Rd(pCB + 8), Rd(pCB + 12), Rd(pCB + 16), Rd(pCB + 20),
                  Rd(pCB + 24), Rd(pCB + 28), pred, (uint32_t)ctx.lr);
  }
  const bool hasPred = (pCB != 0u) && ((Rd(pCB + 108u) & 0x40u) != 0u);
  const bool predPass = !hasPred || (pred == 0u) || ((pred & Rd(dev + 12708u)) != 0u);
  __imp__sub_8241B6F0(ctx, base);
  if (mcla::gnh::GetMode() == mcla::gnh::Mode::kNative && NativeCbEnabled() &&
      dev != 0u && pCB != 0u && predPass) {
    std::vector<RecordedCbOp> opsCopy;
    {
      std::lock_guard<std::mutex> lk(s_cbMtx);
      auto it = s_cbMap.find(pCB);
      if (it != s_cbMap.end() && it->second.sealed) {
        opsCopy = it->second.ops;
      }
    }
    if (!opsCopy.empty()) {
      auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
      for (auto &op : opsCopy) {
        switch (op.kind) {
          case RecordedCbOp::Kind::Draw: {
            auto nd = std::make_unique<NativeDraw>();
            nd->dc = op.dc;
            nd->vs = op.vsConst;
            nd->ps = op.psConst;
            if (op.inheritVsMask != 0ull) {
              for (uint32_t b = 0; b < 64u; ++b) {
                if (op.inheritVsMask & (1ull << (63u - b))) {
                  (void)mem.ReadBytes(dev + kDevVsConst + b * 64u, nd->vs.data() + b * 64u, 64u);
                }
              }
            }
            if (op.inheritPsMask != 0ull) {
              for (uint32_t b = 0; b < 64u; ++b) {
                if (op.inheritPsMask & (1ull << (63u - b))) {
                  (void)mem.ReadBytes(dev + kDevPsConst + b * 64u, nd->ps.data() + b * 64u, 64u);
                }
              }
            }
            Submit(std::move(nd), 0u);
            break;
          }
          case RecordedCbOp::Kind::Resolve: {
            static std::atomic<uint32_t> s_cbResSeq{0};
            op.resolve.seq = s_cbResSeq.fetch_add(1) + 1u;
            rq::RenderCommand cmd;
            cmd.type = rq::RenderCommand::RESOLVE_GUEST;
            cmd.data = op.resolve;
            rq::g_commandQueue.push(std::move(cmd));
            break;
          }
          case RecordedCbOp::Kind::Clear: {
            static std::atomic<uint32_t> s_cbClrSeq{0};
            op.clear.seq = s_cbClrSeq.fetch_add(1) + 1u;
            rq::RenderCommand cmd;
            cmd.type = rq::RenderCommand::CLEAR_GUEST;
            cmd.data = op.clear;
            rq::g_commandQueue.push(std::move(cmd));
            break;
          }
          case RecordedCbOp::Kind::Query: {
            if ((op.query.flags & 2u) != 0u) {
              for (uint32_t s = 0; s < op.query.reportCount && s < 8u; ++s) {
                if (op.query.reportVas[s] != 0u) {
                  for (uint32_t o = 0; o < 64u; o += 4u)
                    mcla::native::WriteGuestU32(op.query.reportVas[s] + o, 0u);
                }
              }
            } else if ((op.query.flags & 1u) != 0u && op.query.reportVa != 0u) {
              mcla::native::WriteGuestU32(op.query.reportVa + 16u, 0xFFFFFEEDu);
              mcla::native::WriteGuestU32(op.query.reportVa + 20u, 0xFFFFFEEDu);
            }
            rq::RenderCommand cmd;
            cmd.type = rq::RenderCommand::QUERY_OP;
            cmd.data = op.query;
            rq::g_commandQueue.push(std::move(cmd));
            break;
          }
        }
      }
      static std::atomic<uint32_t> s_cbReplayed{0};
      const uint32_t rn = s_cbReplayed.fetch_add(1) + 1u;
      if (rn <= 16 || (rn % 500) == 0) {
        MCLA_LOG_INFO("GNH-CB-REPLAY #{} cb={:08X} ops={}", rn, pCB, opsCopy.size());
      }
    }
  }
}

PPC_FUNC_IMPL(__imp__sub_8241B318);
PPC_FUNC(sub_8241B318) {
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1) + 1;
  const uint32_t dev = ctx.r3.u32;
  const uint32_t pCB = dev ? Rd(dev + 13516u) : 0u;
  if (n <= 8 || (n % 500) == 0)
    MCLA_LOG_INFO("TEMP-3D-CB end #{} dev={:08X} cb={:08X} lr={:08X}", n, dev, pCB, (uint32_t)ctx.lr);
  __imp__sub_8241B318(ctx, base);
  if (mcla::gnh::GetMode() == mcla::gnh::Mode::kNative && NativeCbEnabled() && pCB != 0u) {
    std::lock_guard<std::mutex> lk(s_cbMtx);
    auto it = s_cbMap.find(pCB);
    if (it != s_cbMap.end() && it->second.recording) {
      it->second.recording = false;
      it->second.sealed = true;
    }
  }
}

// TEMP-DEVFN: sub_82177DD0(off) calls [[0x82839254] + off + 64]. vk1 crash_dump:
// AV reading -1 from it via sub_82493910 (offsets 360/364/368/372). Log the
// device, offset and target whenever the target is outside the code range.
PPC_FUNC_IMPL(__imp__sub_82177DD0);
PPC_FUNC(sub_82177DD0) {
  auto &mem = mcla::kernel::GuestMemoryHeap::Instance();
  uint32_t dev = 0, fn = 0;
  (void)mem.ReadU32BE(0x82839254u, &dev);
  (void)mem.ReadU32BE(dev + ctx.r3.u32 + 64u, &fn);
  if (fn < 0x82130000u || fn >= 0x827BDE00u) {
    static std::atomic<uint32_t> s_bad{0};
    if (s_bad.fetch_add(1) < 20) {
      uint32_t w[8] = {};
      for (int i = 0; i < 8; ++i) (void)mem.ReadU32BE(dev + 424u + 4u * i, &w[i]);
      MCLA_LOG_WARN("TEMP-DEVFN dev={:08X} off={} fn={:08X} lr={:08X} dev+424=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                    dev, ctx.r3.u32, fn, static_cast<uint32_t>(ctx.lr), w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
  }
  __imp__sub_82177DD0(ctx, base);
}

namespace mcla::gpu { uint32_t ZpdLastHdr(uint32_t *addr); void ZpdWatch(uint32_t, uint32_t); }
// MCLA_TEMP_ZPD=1 probe: D3D query Issue (sub_8241DE08(query, flags): flags&2=BEGIN, &1=END; query+4 = type, 9 = occlusion).
// It emits PM4_EVENT_WRITE_ZPD via sub_824302F0. Passthrough; logs only.
PPC_FUNC_IMPL(__imp__sub_8241DE08);
PPC_FUNC(sub_8241DE08) {
  static const bool on = [] { const char *e = std::getenv("MCLA_TEMP_ZPD"); return e && e[0] == '1'; }();
  if (on) {
    static std::atomic<uint32_t> n{0};
    static std::atomic<uint32_t> byType[16]{};
    const uint32_t q = ctx.r3.u32, ty = Rd(q + 4) & 15u;
    const uint32_t k = n.fetch_add(1) + 1;
    byType[ty].fetch_add(1);
    if (k <= 12 || (k % 500) == 0)
      MCLA_LOG_ERROR("ZPD-ISSUE #{} q={:08X} type={} flags={:X} typeCounts[9]={} [10]={} lr={:08X} cpLastHdr={:08X}@{:08X}", k, q, ty,
                     ctx.r4.u32, byType[9].load(), byType[10].load(), (uint32_t)ctx.lr, [] { uint32_t a = 0; uint32_t h = mcla::gpu::ZpdLastHdr(&a); (void)a; return h; }(), [] { uint32_t a = 0; mcla::gpu::ZpdLastHdr(&a); return a; }());
  }
  const uint32_t q0 = ctx.r3.u32, fl0 = ctx.r4.u32;
  const uint32_t zdev = q0 ? Rd(q0) : 0u;
  const uint32_t zcur0 = on ? Rd(zdev + 48) : 0u;
  __imp__sub_8241DE08(ctx, base);
  {
    static const bool noHwZpd = [] {
      const char *e = std::getenv("MCLA_NO_HW_ZPD");
      const char *e2 = std::getenv("MCLA_NO_OCCLUSION_QUERY");
      return (e && e[0] == '1') || (e2 && e2[0] == '1');
    }();
    static const bool offLegacy = [] { const char *e = std::getenv("MCLA_NO_NATIVE_ZPD"); return e && e[0] == '1'; }();
    const uint8_t qType = q0 ? static_cast<uint8_t>(Rd(q0 + 4u) & 0xFFu) : 0u;
    if (q0 != 0u && !noHwZpd && mcla::gnh::GetMode() == mcla::gnh::Mode::kNative) {
      if ((qType & 15u) == 9u && (fl0 & 3u) != 0u) {
        const uint32_t repCountRaw = Rd(q0 + 148u);
        const uint32_t repCount = (repCountRaw >= 1u && repCountRaw <= 8u) ? repCountRaw : 1u;
        const uint32_t rep = Rd(q0 + 4u * (repCount + 6u));
        rq::QueryOpCommand qc{};
        qc.queryVa = q0;
        qc.reportVa = rep;
        qc.reportCount = repCount;
        for (uint32_t i = 0; i < repCount; ++i)
          qc.reportVas[i] = Rd(q0 + 28u + 4u * i);
        qc.queryType = qType;
        if ((fl0 & 2u) != 0u) {
          qc.flags = static_cast<uint8_t>(rq::QueryOpCommand::Op::Begin);
          for (uint32_t s = 0; s < repCount; ++s) {
            if (qc.reportVas[s] != 0u) {
              for (uint32_t o = 0; o < 64u; o += 4u)
                mcla::native::WriteGuestU32(qc.reportVas[s] + o, 0u);
            }
          }
        } else {
          qc.flags = static_cast<uint8_t>(rq::QueryOpCommand::Op::End);
          if (rep != 0u) {
            mcla::native::WriteGuestU32(rep + 16u, 0xFFFFFEEDu);
            mcla::native::WriteGuestU32(rep + 20u, 0xFFFFFEEDu);
          }
        }
        {  // MD-12 census (log-only): draws the guest produced between Begin and End on THIS side (host OCC-BRACKET saw 0)
          static std::mutex zm; static std::unordered_map<uint32_t, uint32_t> zb;
          std::lock_guard<std::mutex> lk(zm);
          const uint32_t built = s_nBuilt.load(), refused = s_nRefused.load();
          if (qc.flags == static_cast<uint8_t>(rq::QueryOpCommand::Op::Begin)) zb[q0] = built;
          else {
            const auto bi = zb.find(q0);
            static std::atomic<uint32_t> s_zb{0};
            const uint32_t zn = s_zb.fetch_add(1) + 1;
            if (zn <= 16 || (zn % 500) == 0)
              MCLA_LOG_WARN("ZPD-GUEST-BRACKET #{} q={:08X} builtBetween={} refusedTotal={} cbRecording={}", zn, q0,
                            bi == zb.end() ? -1ll : static_cast<long long>(built - bi->second), refused, Rd(zdev + 13516u) != 0u ? 1 : 0);
          }
        }
        if (!TryRecordCbQuery(zdev, qc)) {
          rq::RenderCommand cmd;
          cmd.type = rq::RenderCommand::QUERY_OP;
          cmd.data = qc;
          rq::g_commandQueue.push(std::move(cmd));
        }
      }
    } else if (!offLegacy && (qType & 15u) == 9u && (fl0 & 3u) != 0u) {
      // MCLA_NO_HW_ZPD=1 legacy fallback (hardcoded 1000).
      const uint32_t rep = Rd(q0 + 4u * (Rd(q0 + 148) + 6u));
      if (rep != 0u) {
        for (uint32_t o = 0; o < 32u; o += 4u) mcla::native::WriteGuestU32(rep + o, 0u);
        if ((fl0 & 1u) != 0u) {
          mcla::native::WriteGuestU32(rep + 16u, 1000u);  // ZPass_A
          mcla::native::WriteGuestU32(rep, 1000u);        // Total_A
        }
      }
    }
  }
  if (on) {  // TEMP: where do the EVENT_WRITE_ZPD packets go? dump the dwords Issue appended
    static std::atomic<uint32_t> s_z{0};
    const uint32_t k = s_z.fetch_add(1) + 1, cur1 = Rd(zdev + 48);
    if (cur1 > zcur0 && cur1 - zcur0 <= 256u) mcla::gpu::ZpdWatch(zcur0, cur1);
    if (k <= 8 && cur1 > zcur0 && cur1 - zcur0 <= 256u) {
      std::string d; for (uint32_t a2 = zcur0; a2 <= cur1 && a2 < zcur0 + 160u; a2 += 4u) d += fmt::format(" {:08X}", Rd(a2));
      MCLA_LOG_ERROR("ZPD-APPEND #{} dev={:08X} flags={:X} cur {:08X}->{:08X} 10940={:08X} 13232={:08X} words:{}", k, zdev, fl0, zcur0, cur1, Rd(zdev + 10940), Rd(zdev + 13232), d);
    }
  }
  if (on) {
    static std::atomic<uint32_t> m{0};
    const uint32_t k = m.fetch_add(1) + 1;
    const uint32_t rep = Rd(q0 + 4u * (Rd(q0 + 148) + 6u));
    if (k <= 12 || (k % 500) == 0)
      MCLA_LOG_ERROR("ZPD-REPORT #{} flags={:X} report={:08X} w[0..7]={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", k, fl0, rep,
                     Rd(rep), Rd(rep + 4), Rd(rep + 8), Rd(rep + 12), Rd(rep + 16), Rd(rep + 20), Rd(rep + 24), Rd(rep + 28));
  }
}

// sub_82423A38(vs, dst, decl, dev+12528, variant): patches the VS's vfetch triples at dst (image or ring copy).
PPC_FUNC_IMPL(__imp__sub_82423A38);
PPC_FUNC(sub_82423A38) {
  VsPatch pt{ctx.r3.u32, ctx.r4.u32, ctx.r7.u32};
  __imp__sub_82423A38(ctx, base);
  // Snapshot what the guest just wrote: count = *(ent+900), slot list at ent+872+4*(*(ent+896)+9), each slot's
  // patched triple at dst+12*(slot&0xFFF) (sub_82423A38 tail memcpy). The ring copy is unreadable by draw-build time (F-955).
  const uint32_t ent = pt.vs + Rd(pt.vs + 896u + 8u * pt.variant);
  const uint32_t n = Rd(ent + 900u), list = ent + 872u + 4u * (Rd(ent + 896u) + 9u);
  for (uint32_t i = 0; i < n && pt.n < kVsPatchMax; ++i) {
    const uint32_t slot = Rd(list + 4u * i) & 0xFFFu, at = pt.dst + 12u * slot;
    pt.slot[pt.n] = slot;
    for (uint32_t k = 0; k < 3u; ++k) pt.w[pt.n][k] = Rd(at + 4u * k);
    pt.nz |= (pt.w[pt.n][2] & 0xFFu) != 0u;  // stride byte (sub_82423A38: dword2 |= a4[stream])
    ++pt.n;
  }
  static const bool s_noLink = [] { const char* e = std::getenv("MCLA_NO_VS_LINK_COPY"); return e && e[0] == '1'; }();
  const uint32_t img = Rd(ent + 872u) + Rd(pt.vs + 32u);
  if (!s_noLink && pt.dst != img && (pt.dst & 0x1FFFFFFFu) != (img & 0x1FFFFFFFu)) {
    pt.fullN = std::min<uint32_t>(Rd(ent + 876u) >> 2, 1024u);
    for (uint32_t i = 0; i < pt.fullN; ++i) pt.full[i] = Rd(pt.dst + 4u * i);
  }
  static std::atomic<uint32_t> s_snap{0}, s_snapZero{0};
  const uint32_t c = s_snap.fetch_add(1) + 1, z = pt.nz ? s_snapZero.load() : s_snapZero.fetch_add(1) + 1;
  if (c <= 4 || c % 20000 == 0)
    MCLA_LOG_INFO("VS-PATCH-SNAP #{} vs={:08X} dst={:08X} variant={} n={} cap={} strideNZ={} zeroSnaps={}", c, pt.vs, pt.dst, pt.variant, n, pt.n, pt.nz ? 1 : 0, z);
  t_vsPatch = pt;
}
