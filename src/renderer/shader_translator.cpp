#include "shader_translator.h"

#include <filesystem>

#include "precompiled_shaders.h"

// Thread-local flag set when the post-intersection program hash matches the
// interpolator-hashviz target; read by Translator::Generate to override PS output.
static thread_local bool tl_hvInterp = false;
static thread_local bool tl_hvUV = false;
static thread_local int tl_hvUVIdx = 0;

// Phase 5: per-opcode Xenos microcode -> HLSL lowering.
//
// Consumes the normalized IR produced by xenos_shader_ir (DecodedInstruction
// stream in program order) and emits readable HLSL, following the lowering
// semantics of hedge-dev/XenosRecomp (shader_recompiler.cpp):
//
//   - vector ALU ops -> arithmetic/compare/select/dot expressions on float4
//     registers with per-opcode operand masks;
//   - scalar ALU ops -> the float "ps" (previous-scalar) pipeline, with
//     Setp*/predicate stack and Kills -> clip();
//   - fetch (vertex/tfetch) -> vertex-element inputs (VS) or sampler/texture
//     binds with swizzled register writes (PS);
//   - exports -> oC0..oC3 / oDepth (PS) or oPos + interpolators (VS).
//
// This is a generator: it must always emit a complete, tokenizable HLSL body
// so the offline tool can diff and compile it. Every ALU vector/scalar opcode
// has a lowering branch (scalar RetainPrev is deliberately skipped before the
// clause); vertex and texture fetches get the two fetch branches, and the aux
// GET/SETTEXTURE fetch opcodes decode as known and share the texture-fetch
// path. Anything outside the decoder's known table is recorded as unsupported
// (never silently dropped). The emitted HLSL is diagnostic-grade; the helpers
// the body references (select() ternary overloads, FLT_MIN/FLT_MAX macros)
// are emitted inline in AppendHeader so the output stands alone.

// The unwritten-register census (CensusGuestRegisterAccess,
// LogUnwrittenRegisterReads) reports through mcla::log::Warn. That symbol is
// `inline` (src/common/logging.h:76), so including the header adds no link
// dependency for the validator targets that build this file without a logger
// (CMakeLists.txt:605-609 links only d3d12/dxgi/dxguid). Same include spelling
// as dxc_runtime.cpp:16 and pipeline_cache.cpp:12.
#include "common/logging.h"
#include "d3d12_backend.h"  // native::DepthCompareMirrored(): SV_Depth is mirrored with the DSV

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mcla::renderer {

// ---------------------------------------------------------------- reflection
//
// Vertex-input reflection consumed by the render thread's input-layout
// fallback (src/render_thread.cpp). A vertex fetch resolves to a
// (usage, usageIndex) pair by matching the program's vertexElements (12-bit
// stream address) against the fetch's IR instruction address, falling back to
// the first element when no element matches — the same comparison EmitFetch
// makes. ReferencedVertexInputs returns the unique pairs in first-fetch order
// plus the format/stride/offset of the fetch that introduced each pair, so
// the declared signature and the bound layout always agree.

const char* VertexUsageSemanticName(uint32_t usage) {
    // DeclUsage semantic table (the all-caps counterparts of kUsageVariables).
    static constexpr const char* kSemantics[] = {
        "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "POINTSIZE",
        "TEXCOORD", "TANGENT",     "BINORMAL",     "TESSFACTOR", "POSITIONT",
        "COLOR",    "FOG",         "DEPTH",        "SAMPLE",
    };
    if (usage >= 14) usage = 0;  // clamp, mirroring the translator fallback
    return kSemantics[usage];
}

// k-th vertex fetch of a program with no element table -> its input semantic (shared with EmitFetch).
static void FallbackVertexSemantic(uint32_t ordinal, uint32_t& usage, uint32_t& usageIndex) {
    static const uint32_t kUsage[] = {0u, 3u, 6u, 5u, 5u, 10u, 1u, 2u};      // POSITION NORMAL TANGENT TEXCOORD TEXCOORD COLOR BLENDWEIGHT BLENDINDICES
    static const uint32_t kIndex[] = {0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u};
    if (ordinal < 8) { usage = kUsage[ordinal]; usageIndex = kIndex[ordinal]; }
    else { usage = 5u; usageIndex = ordinal - 6; }
}

std::vector<VertexInputRef> ReferencedVertexInputs(const ShaderProgram& prog) {
    std::vector<VertexInputRef> inputs;
    if (!prog.isVertex) return inputs;

    auto resolve = [&prog](uint32_t addr, uint32_t& usage,
                           uint32_t& usageIndex) {
        usage = 0;
        usageIndex = 0;
        // Derived tables (no guest declaration) name a fetch by the first export that reads its
        // register, not by dataflow, so two fetches can collide or swap (composite uv became POSITION).
        // Every fetch is therefore its own input; the name is a label shared with EmitFetch.
        if (prog.vertexElementsDerived) { usage = 5; usageIndex = 16u + addr; return; }
        for (const auto& ve : prog.vertexElements) {
            if (ve.address == addr) {
                usage = ve.usage;
                usageIndex = ve.usageIndex;
                return;
            }
        }
        if (!prog.vertexElements.empty()) {
            usage = 5;  // TEXCOORD; must match EmitFetch: unnamed fetch = its own input
            usageIndex = 16u + addr;
        }
    };

    // A vfetch_mini stores stride 0 in its own word2 and inherits the previous
    // vfetch_full's stride: "Reuse previous vfetch_full if this is a mini"
    // (.research/xenia/src/xenia/gpu/shader_translator.cc:1022-1023), applied
    // at `:1045` as `instr.attributes.stride = full_op.stride();`. The offset
    // is NOT inherited (`instr.attributes.offset = op.offset();`, `:1044`).
    //
    // Reported as 0, a mini fetch's element contributes no bytes and the
    // draw's byte accounting is short by exactly the mini's payload -- which is
    // the measured 16 unaccounted bytes on draw 1 (fmt 38 = 4x32-bit float at
    // offset 3 dwords of a 7-dword stride, from CP-FETCHVB-DESC #1 in w689).
    // The mini bit is read from VertexFetch WORD 1 bit 30, via
    // IrInstruction::isMiniFetch (fetchFlagBits, packed by the shifts in
    // xenos_shader_ir.cpp's decode loop). VertexFetch word 1 only: tfetch word 1
    // is_predicated is also bit 31 (ucode.h:866) and ALU word 1 is_predicated
    // is bit 28 (ucode.h:2076) -- three layouts, and "bit 28" is a true
    // statement about an ALU instruction and a false one about this fetch.
    //
    // MEASURED on this very fetch, not inferred: its word1 is 0x40263688, whose
    // bit 30 is 1 and whose bit 28 is 0, while the preceding full fetch's word1
    // 0x00393A88 has bit 30 = 0 -- and that mini/full split is exactly what the
    // stride inheritance above keys on (build/w686.log, CP-SHADER-BODY #3
    // against CP-FETCHVB-DESC #1 instr=0/instr=1, strideDwords=7 then 0; the
    // same pair recurs in w685). Bits 22-23, reserved2, are 0 on every vfetch
    // word1 reachable from this tree, which is what forces exp_adjust to start
    // at 24 and the flags to 30/31; the argument is written out on
    // IrInstruction::fetchFlagBits in xenos_shader_ir.h.
    //
    // DecodeInstruction now reads bit 30 too (xenos_microcode.cpp:167), as does
    // the CP-side decoder (src/gpu_cp.cpp:1295). An earlier revision of this
    // comment said "DecodedInstruction::isMiniFetch reads bit 28
    // (xenos_microcode.cpp:168) and therefore reports 0 for this very fetch" --
    // both halves were false (168 is not the isMiniFetch read, and the
    // report-0 consequence it described no longer exists), so the sentence this
    // paragraph replaces was licensing a defect with a mechanism that was not
    // there.
    //
    // prog.instructions is walked in stored (control-flow block) order. The
    // inheritance is order-sensitive, so only a full fetch that has ALREADY
    // been seen in that same order can donate its stride; a mini ahead of its
    // full fetch keeps 0 rather than borrowing a later one.
    uint32_t previousFullStride = 0;
    uint32_t previousFullSlot = 0;
    uint32_t fallbackOrdinal = 0;
    for (const auto& ir : prog.instructions) {
        if (!ir.isFetch || ir.decoded.kind != InstructionKind::VertexFetch)
            continue;
        uint32_t usage = 0, usageIndex = 0;
        if (!prog.vertexElements.empty()) {
            resolve(ir.address, usage, usageIndex);
        } else {
            FallbackVertexSemantic(fallbackOrdinal, usage, usageIndex);
            ++fallbackOrdinal;
        }
        if (usage >= 14) usage = 0;
        const uint32_t mergedSlot =
            ir.decoded.constIndex * 3u + ir.decoded.constIndexSelect;
        const uint32_t rawSlot = (mergedSlot < 96u) ? mergedSlot : 95u;
        const uint32_t stride =
            ir.isMiniFetch() ? previousFullStride : ir.decoded.stride;
        const uint32_t slot =
            ir.isMiniFetch() ? previousFullSlot : rawSlot;
        if (!ir.isMiniFetch()) {
            previousFullStride = ir.decoded.stride;
            previousFullSlot = rawSlot;
        }
        bool seen = false;
        for (const auto& in : inputs)
            if (in.usage == usage && in.usageIndex == usageIndex) { seen = true; break; }
        if (seen) continue;
        inputs.push_back(VertexInputRef{usage, usageIndex,
                                        ir.decoded.vertexFormat,
                                        stride, ir.decoded.offset,
                                        rawSlot, slot});
    }
    return inputs;
}

namespace {

constexpr char kSwizzle[] = {'x', 'y', 'z', 'w', '0', '1', '_', '_'};

// VS HLSL carries an `#if defined(XE_RAWVB)` variant in which every vfetch reads a raw ByteAddressBuffer
// at the address the guest ucode computed.
constexpr bool RawVbTranslate() { return true; }

constexpr const char* kUsageVariables[] = {
    "Position", "BlendWeight", "BlendIndices", "Normal", "PointSize",
    "TexCoord", "Tangent",     "Binormal",     "TessFactor", "PositionT",
    "Color",    "Fog",         "Depth",        "Sample",
};

// Xenos VS exports: 62 = position, 0-15 = VSInterpolator0..15, 63 = point
// size / edge flag / kill vertex (no color output). PS exports are 0-3
// (color) and 61 (depth), see ExportRegisterName.

// ---------------------------------------------------------------------------
// Operand selection (mirrors the `op` operand lambda inside XenosRecomp's
// ShaderRecompiler::recompile(), shader_recompiler.cpp).
// ---------------------------------------------------------------------------
enum class OperandKind : uint8_t {
    Vector0,
    Vector1,
    Vector2,
    Scalar0,
    Scalar1,
    ScalarConstant0,
    ScalarConstant1,
};

constexpr uint32_t kRegisterCount = 32;

// ---------------------------------------------------------------------------
// clip_disable NDC rescale (the guest-pixel -> host-clip-space map).
//
// WHY IT EXISTS. For this title the guest runs with PA_CL_CLIP_CNTL = 0x00010000
// and PA_CL_VTE_CNTL = 0x00000300. Both are MEASURED, in the log this tree
// produced: build/w706.log:1676-1678 prints
//     CP-DRAW-REGS #1 [30/57] r2204=00010000
//     CP-DRAW-REGS #1 [31/57] r2205=00010000
//     CP-DRAW-REGS #1 [32/57] r2206=00000300
// and the same three values repeat for #2 (w706.log:1722-1724) and #3
// (w706.log:1769-1771). Decoded against
// .research/xenia/src/xenia/gpu/registers.h:534-583 (bit +16 clip_disable, bits
// +0..+5 of PA_CL_VTE_CNTL the six vport_*_ena):
//     r2204 bit 16 set  -> clip_disable = 1, and NO user clip plane (ucp_ena 0)
//     r2206 bits 8,9   -> vtx_xy_fmt / vtx_z_fmt, and ALL SIX vport_*_ena CLEAR
// so there is no host-side viewport transform in the VTE and no clip volume: the
// vertex shader's POSITION output is in GUEST PIXELS, pre-divided by W. That is
// the case the reference documents at draw_util.cc:255-298 and implements at
// :362-386, and it is why this tree's viewport-only transform cannot place the
// geometry: a D3D12 viewport maps clip space to pixels, and pixel-valued clip
// space is not clip space.
//
// THE ARITHMETIC, verbatim from the reference (draw_util.cc:362-386, then the
// application at spirv_shader_translator.cc:1612-1624, then the y negation at
// draw_util.cc:549-552):
//     xy_offset[i]      = 0
//     extent[i]         = min(kTexture2DCubeMaxWidthHeight = 8192, RT extent[i])
//     pixels_to_ndc[i]  = 2 / extent[i]
//     ndc_scale[i]      = scale_xy[i] * pixels_to_ndc[i]        <- 1, enables clear
//     ndc_offset[i]     = (offset_base[i] - extent[i]*0.5 + offset_add[i])
//                        * pixels_to_ndc[i]                     <- 0 + half-pixel
//     ndc_scale[2]      = scale_z = 1,  ndc_offset[2] = offset_z = 0,
//     z_min = 0, z_max = 1
// and then, because D3D12's clip space has +Y DOWN (MEASURED on an RTX 3070, not
// assumed -- see the note on kNdcYSign below):
//     ndc_scale[1] = -ndc_scale[1];  ndc_offset[1] = -ndc_offset[1]
// applied to XYZ ONLY, with the offset scaled by W (so it happens in clip space,
// before the divide):
//     oPos.xyz = oPos.xyz * ndc_scale + ndc_offset * oPos.w
//
// extent is min(8192, RT extent), NOT 8192. For a 1280x720 target the min does
// not bite and extent IS (1280, 720). A constant of 8192 here is simply a
// different transform: measured on the same device it lands the geometry in a
// 2,475-pixel corner blob at (16,4)-(171,34) instead of at its guest pixels.
//
// .research/xenia has no .git, is gitignored and has 0 files in HEAD, so those
// file:line citations cannot be tied to an upstream commit; they are cited as
// trail. The guest facts above are cited to w706.log rows, which are this tree's
// own output.
//
// The per-draw gate is decoded alongside the pipeline state and passed explicitly.
// The guest-state condition is clip_disable set with all six VTE viewport enables clear.
// The registers vary by draw, so a title-wide assumption is invalid.
// This request comes from the same snapshot used for pipeline-state derivation.
// The rescale is emitted only for draws whose decoded registers require it.
// The extent comes from SetClipDisableViewportExtent and the same target
// dimensions used to set the D3D12 viewport.
struct ClipDisableRescale {
    float scaleX = 0.0f, scaleY = 0.0f, scaleZ = 1.0f;
    float offsetX = 0.0f, offsetY = 0.0f, offsetZ = 0.0f;
    bool valid = false;
};

// D3D12's clip space has +Y DOWN, so the reference's origin_bottom_left negation
// (draw_util.cc:549-552) applies. MEASURED, not assumed: an arm whose kS.y/kO.y
// are negated rasterises a guest-pixel rect (100,50)-(1100,400) at measured
// bbox (100,50)-(1099,399), while the same arm without the negation lands at
// measured bbox (103,320)-(1099,668) -- the exact mirror about y=360 -- and an
// arm with x negated lands at (179,50)-(1178,399), the exact mirror about x=640.
// Both un-negated predictions (the OpenGL convention, +Y up) are refuted by those
// two rows. See %TEMP%\opencode\ndcconv\run2.txt.
constexpr float kNdcYSign = -1.0f;

std::atomic<uint64_t> g_clipExtPacked{0};   // (w<<32)|h of the published window extent; 0 = none

// Shortest decimal form that round-trips the float exactly, so the literal in
// the generated HLSL is the constant that was computed and not a rounded copy of
// it. File scope rather than a member of HlslGenerator because it touches no
// emitter state, which also makes it reachable from a test that includes this
// file (see %TEMP%\opencode\ndcconv\epilogue_check.cpp).
std::string FloatLit(float v) {
    std::string s = fmt::format("{}", v);
    if (std::strtof(s.c_str(), nullptr) == v) return s;
    return fmt::format("{:e}", v);
}

// G0: per-fetch-constant sampler (s<N> for N < 16); default ON (MCLA_TEMP_GUEST_SAMPLER=0 disables).
static bool UseGuestSampler() {
    static const bool on = [] {
        const char* e = std::getenv("MCLA_TEMP_GUEST_SAMPLER");
        return !(e != nullptr && e[0] == '0');
    }();
    return on;
}

// Xenia P8 (dxbc_shader_translator_fetch.cc:752-765): apply 1.5/1024 rounding
// epsilon on every 1D/2D fetch even when texOffset == 0 && !texCoordDenorm.
// Default OFF until A/B verified so baseline HLSL/perf are unchanged.
static bool FetchEpsilonEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("MCLA_TEMP_FETCH_EPSILON");
        return e != nullptr && e[0] == '1';
    }();
    return on;
}

// MD-17: opt-in (MCLA_XUV_RS=1). xuv/xgrad/xweights size the texture with GetDimensions(), which is the
// HOST size; on a render-scaled resolved RT (MCLA_RENDER_SCALE>1) guest-texel offsets/denorm coords then
// span 1/scale of the intended distance. With this on, each helper takes `rs` = XE_RS_<slot>, a macro the
// consumer #defines to the scale (e.g. 2.0) for slots bound to a scaled resolve texture; undefined = 1.0.
// Off by default: it changes every xuv-using shader's HLSL (425 of 1,844 dumped pairs), which would
// invalidate the precompiled shader cache for no gain at scale 1.
static bool XuvRenderScaleEnabled() {
    static const bool on = [] {
        // Default ON only when MCLA_RENDER_SCALE>1 (same parse as d3d12_backend.cpp RenderScale()); at scale 1 the HLSL is
        // untouched. MCLA_NO_XUV_RS=1 = off; MCLA_XUV_RS=1 forces the helper signatures on (mask stays 0 at scale 1).
        const char* off = std::getenv("MCLA_NO_XUV_RS");
        if (off && off[0] == '1') return false;
        const char* e = std::getenv("MCLA_XUV_RS");
        if (e != nullptr && e[0] == '1') return true;
        const char* rs = std::getenv("MCLA_RENDER_SCALE");
        const unsigned long v = rs ? std::strtoul(rs, nullptr, 10) : 1ul;
        return v >= 2ul && v <= 3ul;
    }();
    return on;
}
// ", XE_RS_<n>" for texName "tex<n>", or "" when the feature is off.
static std::string RsArg(const std::string& texName) {
    return XuvRenderScaleEnabled() ? ", XE_RS_" + texName.substr(3) : std::string();
}

// Guarantees a decimal point or exponent so DXC never treats whole floats (e.g. "2")
// as integer literals in overload resolution such as clamp(x, -16.0, 15.99).
static std::string FloatLitDecimal(float v) {
    std::string s = FloatLit(v);
    if (s.find('.') == std::string::npos &&
        s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos) {
        s += ".0";
    }
    return s;
}

// Reported once per process by the backend, so a reader of a soak knows the gate
// is a measured constant and not a per-draw register read. DEFINED OUTSIDE the
// anonymous namespace below, because the header declares it with external linkage
// (see shader_translator.h); a definition inside `namespace {` would be internal
// and the two would be different objects with the same name.
//
// Deliberately names no other tracked marker: a tracked marker named inside
// another marker's prose becomes uncountable, because tools/soak_census.py counts
// matching LINES.;

// Compute the reference's constants for one axis. `extent` is ALREADY the
// min(8192, RT extent) result, computed by the caller, because that min is what
// makes a 1280x720 target use 1280x720 and not 8192x8192.
ClipDisableRescale MakeClipDisableRescale(uint32_t widthPx, uint32_t heightPx) {
    ClipDisableRescale r;
    if (widthPx == 0 || heightPx == 0) return r;   // no target: leave invalid
    // kTexture2DCubeMaxWidthHeightLog2 == 13 (xenia.h:1139-1141) -> 8192.
    constexpr float kMaxExtent = 8192.0f;
    const float halfPixel = 0.5f;   // draw_util.cc:344-348, the only offset_add term
    struct Axis { float rt; float* s; float* o; };
    const Axis axes[2] = {
        {static_cast<float>(widthPx),  &r.scaleX,  &r.offsetX},
        {static_cast<float>(heightPx), &r.scaleY,  &r.offsetY},
    };
    for (const Axis& a : axes) {
        const float extent = a.rt < kMaxExtent ? a.rt : kMaxExtent;
        const float pixelsToNdc = 2.0f / extent;
        *a.s = 1.0f * pixelsToNdc;                                  // scale_xy == 1
        *a.o = (0.0f - extent * 0.5f + halfPixel) * pixelsToNdc;   // offset_base == 0
    }
    r.scaleY *= kNdcYSign;
    r.offsetY *= kNdcYSign;
    r.scaleZ = 1.0f;
    r.offsetZ = 0.0f;
    r.valid = true;
    return r;
}

ClipDisableRescale PublishedClipRescale() {
    const uint64_t p = g_clipExtPacked.load(std::memory_order_acquire);
    return MakeClipDisableRescale(static_cast<uint32_t>(p >> 32), static_cast<uint32_t>(p));
}

// ---------------------------------------------------------------------------
// Unwritten-register census (the SHADER-UNWRITTEN-REG-READ marker).
// ---------------------------------------------------------------------------

// The reference's VECTOR operand count per opcode, i.e.
// GetAluVectorOpcodeInfo(op.vector_opcode()).GetOperandCount() -- the number of
// source operands ParseAluInstruction actually parses and
// GatherAluInstructionInformation actually gathers.
//
// TRANSCRIBED, NOT INFERRED, from
//   .research/xenia/src/xenia/gpu/ucode.cc:83-116  kAluVectorOpcodeInfos[]
//     {"add", {0b1111, 0b1111}, ...}                      -> 2
//     {"frc", {0b1111}, ...}                             -> 1
//     {"mad", {0b1111, 0b1111, 0b1111}, ...}             -> 3
//     {"dst", {0b0110, 0b1010}, ...}                     -> 2
//     {"opcode_30", {}, ...}, {"opcode_31", {}, ...}     -> 0
// with the 0/1/2/3 reduction itself at
//   .research/xenia/src/xenia/gpu/ucode.h:1715-1726  AluVectorOpcodeInfo::GetOperandCount()
// which returns 0 when all three are zero, then 1, then 2, else 3 -- the
// reference's own nesting, quoted rather than paraphrased.
//
// `.research/xenia/` HAS NO .git, IS GITIGNORED, AND HAS 0 FILES IN HEAD: it is
// an untracked snapshot with no recorded provenance. These constants are trail,
// not authority, and the derivation is restated above so a reader can check it
// against the reference if the checkout ever gains a history.
//
// SAFE TO TRUST EVEN IF THE REFERENCE IS WRONG ABOUT ITSELF, which is why the
// bound is this table and not the opcode alone: every entry is <= the 3 sources
// the decoder carries, so the bound can only ever REMOVE reads this module
// previously counted, never add one.
constexpr uint32_t kReferenceVectorOperandCount[32] = {
    /* 0 add          */ 2, /* 1 mul          */ 2, /* 2 max          */ 2,
    /* 3 min          */ 2, /* 4 seq          */ 2, /* 5 sgt          */ 2,
    /* 6 sge          */ 2, /* 7 sne          */ 2, /* 8 frc          */ 1,
    /* 9 trunc        */ 1, /*10 floor        */ 1, /*11 mad          */ 3,
    /*12 cndeq        */ 3, /*13 cndge        */ 3, /*14 cndgt        */ 3,
    /*15 dp4          */ 2, /*16 dp3          */ 2, /*17 dp2add       */ 3,
    /*18 cube         */ 2, /*19 max4         */ 1, /*20 setp_eq_push */ 2,
    /*21 setp_ne_push */ 2, /*22 setp_gt_push */ 2, /*23 setp_ge_push */ 2,
    /*24 kill_eq      */ 2, /*25 kill_gt      */ 2, /*26 kill_ge      */ 2,
    /*27 kill_ne      */ 2, /*28 dst          */ 2, /*29 maxa         */ 2,
    /*30 opcode_30    */ 0, /*31 opcode_31    */ 0,
};
//
// WHAT THIS IS FOR. A pixel shader whose only instruction is
// `oC0 = max(r0, r0)` reads r0 and writes nothing, so its colour is whatever
// the host seeds r0 with. That seed is a HOST CHOICE, and the one thing a
// reader cannot re-derive from the HLSL is whether the guest ever supplied a
// value. This census answers that from the guest's own bytes and says so in
// the log, so the black is attributable instead of looking like a colour bug.
//
// THE SEED IS NOT AN INVENTION. The reference zero-fills every general-purpose
// register at the top of a shader, precisely so that a program which reads a
// register it never wrote has a defined value:
//   .research/xenia/src/xenia/gpu/spirv_shader_translator.cc:2052-2070 (pixel)
//       // Zero general-purpose registers to prevent crashes when the game
//       // references them after only initializing them conditionally, and copy
//       // interpolants to GPRs.
//   .research/xenia/src/xenia/gpu/spirv_shader_translator.cc:1300-1309 (vertex)
// with the value `const_float4_0_`. It seeds from an interpolant ONLY when the
// interpolator mask has that bit, and that mask is an INTERSECTION:
//   .research/xenia/src/xenia/gpu/vulkan/vulkan_command_processor.cc:2206-2211
//       interpolator_mask = vertex_shader->writes_interpolators() &
//                           pixel_shader->GetInterpolatorInputMask(...)
// so a VS that exports no interpolator leaves the mask 0 and the reference
// zeroes r0 too. `writes_interpolators_` is set only by an ALU result whose
// storage target is kInterpolator (shader_translator.cc:597-599); an export to
// vectorDest 62 is POSITION and does not set it.
//
// CENSUSED FROM THE IR, NOT FROM THE OUTPUT. `referencedRegisters_` records what
// the EMITTER touched, and an instruction whose effective write mask is 0
// emits no operand text at all, so its register reads disappear from that set
// while still being reads in the guest's program. Measured: all four ALU
// instructions of CP-SHADER-BODY #1 have vectorWriteMask 0, so its r0/r1 reads
// never reach referencedRegisters_ and would be invisible here. The `emitted`
// list is therefore reported separately from the guest-level list.
void CensusGuestRegisterAccess(const ShaderProgram& prog, std::vector<bool>& read,
                               std::vector<bool>& written) {
    read.assign(kRegisterCount, false);
    written.assign(kRegisterCount, false);
    for (const auto& ir : prog.instructions) {
        if (ir.isFetch) {
            // An unknown FETCH opcode really does contribute nothing, and the
            // reference's reason is a hard RETURN rather than a decode failure:
            // ParseTextureFetchInstruction's `default:` arm is
            // `assert_unhandled_case(fetch_opcode); return;`
            // (.research/xenia/src/xenia/gpu/shader_translator.cc:1102-1104), so
            // `instr.result` keeps its defaults -- storage_target kNone and
            // original_write_mask 0 -- and GatherFetchResultInformation returns on
            // the empty used write mask at :566-568. No source operand is appended
            // either, so the operand loop at :451-453 has nothing to gather.
            // Skipping the whole instruction is therefore correct HERE, and only
            // here. (Measured on the corpus: 0 unknown fetch opcodes in 506
            // containers, so this arm is currently unreachable either way.)
            if (ir.decoded.unknown) continue;
            // A fetch's SOURCE selector addresses the vertex buffer, not the GPR
            // file, so only the destination is a register write. This matches the
            // reference, which gathers fetch results (GatherFetchResultInformation,
            // shader_translator.cc:565-579) and no fetch source register.
            //
            // ... BUT NOT FOR EVERY KNOWN FETCH OPCODE. THREE OF THEM HAVE NO
            // DESTINATION IN THE REFERENCE, and this arm recorded one anyway
            // (2026-10-05). ParseTextureFetchInstruction builds
            //     opcode_info = {"setTexLOD", false, false, false, 1};      // :1093-1095
            //     opcode_info = {"setGradientH", false, false, false, 3};  // :1096-1098
            //     opcode_info = {"setGradientV", false, false, false, 3};  // :1099-1101
            // -- the second field is has_dest, false in all three -- and then
            //     if (opcode_info.has_dest) {
            //       ParseFetchInstructionResult(op.dest(), op.dest_swizzle(),
            //                                           op.is_dest_relative(),
            //                                           instr.result);
            //     } else {
            //       instr.result.storage_target = InstructionStorageTarget::kNone;
            //     }
            // (:1113-1118). With storage_target kNone the result keeps its default
            // original_write_mask 0, so GetUsedWriteMask() is 0 and
            // GatherFetchResultInformation RETURNS at :566-568 having raised
            // nothing: `register_static_address_bound_` is never advanced and
            // `written[dst]` is never claimed.
            //
            // The three opcodes are FetchOpcode::kSetTextureLod = 24,
            // kSetTextureGradientsHorz = 25, kSetTextureGradientsVert = 26
            // (.research/xenia/src/xenia/gpu/ucode.h:670, 672, 674) -- the numeric
            // values are the point, since a name here would be a claim about this
            // tree's enum rather than the reference's.
            //
            // MEASURED over the corpus (215 pixel containers, 734 known fetches):
            // 10 instructions carry opcode 24/25/26. All 10 wrote a register that
            // is ALSO READ somewhere in the same container, so the reference's
            // smaller `written` set turns 10 read-but-never-written reports into
            // 10 reports that are, on the reference's own terms, correct: the
            // destination of a setTexLOD really is a GPR nothing wrote.
            //
            // LOG-ONLY, as with the read bound above: `written` is a local of this
            // function and the emitter never consults it.
            const uint32_t fetchOpcode = static_cast<uint32_t>(ir.decoded.fetchOpcode);
            const bool fetchHasNoDestination =
                fetchOpcode == 24u || fetchOpcode == 25u || fetchOpcode == 26u;
            if (!fetchHasNoDestination) {
                const uint32_t d = ir.decoded.dstRegister & 0x3F;
                if (d < kRegisterCount) written[d] = true;
            }
            continue;
        }
        const DecodedInstruction& in = ir.decoded;
        // Reads, and ONLY when the opcode is known. An unknown opcode reads
        // NOTHING in the reference: kAluVectorOpcodeInfos[30] and [31] carry an
        // empty operand_components_used, so GetOperandCount() is 0
        // (.research/xenia/src/xenia/gpu/ucode.h:1715-1726 over ucode.cc:114-115),
        // and every unknown scalar opcode carries operand_count 0 (ucode.cc:58 for
        // opcode_41, :68-80 for opcode_51..63). ParseAluInstruction then appends no
        // operand for that half (shader_translator.cc:1385-1389 and :1417-1461), so
        // GatherOperandInformation is never reached for it. Dropping the reads of an
        // unknown instruction is CORRECT.
        if (!in.unknown) {
            // sel == 1 is a temp register and sel == 0 is a c-register
            // constant (ucode.h:1909-1921), the same discrimination NoteOperand
            // makes. Bit 7 of the packed index marks an immediate value rather than
            // a register (ucode.h:1890-1892), so that form is not a GPR read either.
            const uint32_t srcReg[3] = {in.src1Register, in.src2Register,
                                        in.src3Register};
            const bool srcSel[3] = {in.src1Select, in.src2Select, in.src3Select};
            // ... BUT ONLY AS MANY AS THE OPCODE ACTUALLY GATHERS.
            //
            // THIS BOUND IS THE FIX (2026-10-05). The loop used to walk all three
            // source slots unconditionally, so it counted a read the reference
            // never makes. The reference gathers src1..src[vector_operand_count]
            // and nothing more --
            //     instr.vector_operand_count = vector_opcode_info.GetOperandCount();
            //     for (uint32_t i = 0; i < instr.vector_operand_count; ++i)
            //       ParseAluInstructionOperand(op, i + 1, 4, instr.vector_operands[i]);
            // (.research/xenia/src/xenia/gpu/shader_translator.cc:1385-1389) and
            // GetOperandCount() returns 0/1/2/3 from the first zero slot in
            // kAluVectorOpcodeInfos[].operand_components_used
            // (ucode.h:1715-1726 over ucode.cc:83-116). So `max` gathers TWO
            // sources and `mad` gathers THREE, while this loop claimed three for
            // both.
            //
            // MEASURED over the corpus (215 pixel containers, 6517 ALU
            // instructions) with the two models compared per (instruction,
            // register): ours records 14894 reads, the reference gathers 11377, and
            // 2410 pairs are OURS ALONE with 0 the other way -- the reference's set
            // is a strict subset of ours, which is what a missing bound predicts.
            // 137 containers are touched. Set-wise, this changes the row set in 38
            // of 215 pixel containers (symmetric difference 56 pairs).
            //
            // LOG-ONLY, and that is the whole of its effect: `read` is a local of
            // LogUnwrittenRegisterReads and is read by nothing else in this
            // translation path. AppendRegisters, AppendSignature and the emitted
            // body never consult it, so no declaration, no input element and no
            // shader value can move.
            //
            // SCALAR OPERANDS ARE STILL NOT GATHERED HERE, and that is a KNOWN
            // GAP, not a claim of completeness: for scalar_operand_count == 1 the
            // reference parses slot 3 (:1420-1422), and for == 2 its register
            // operand is `op.scalar_const_reg_op_src_temp_reg()` (:1455), a
            // DIFFERENT bitfield from src_reg(1). Neither maps onto src1..src3 as
            // this decoder populates them, so counting them would be inventing a
            // field. Counted as 0 here, exactly as before this fix.
            const uint32_t vopIdx = static_cast<uint32_t>(in.vectorOpcode);
            const uint32_t vectorOperandCount =
                kReferenceVectorOperandCount[vopIdx < 32 ? vopIdx : 0u];
            for (uint32_t i = 0; i < vectorOperandCount && i < 3; ++i) {
                if (!srcSel[i]) continue;
                // Bit 7 is is_src_temp_value_absolute (ucode.h:1884-1892), not an immediate.
                const uint32_t r = srcReg[i] & 0x3F;
                if (r < kRegisterCount) read[r] = true;
            }
        }
        // Writes, REGARDLESS of `unknown`. This is the whole point of the change,
        // and it is not a judgement call: the reference's write accounting never
        // consults the opcode at all. ParseAluInstruction builds BOTH results from
        // word0 alone --
        //     original_write_mask = op.GetVectorOpResultWriteMask() | c0 | c1
        //     original_write_mask = op.GetScalarOpResultWriteMask()
        // with storage_index = op.vector_dest() / op.scalar_dest()
        // (.research/xenia/src/xenia/gpu/shader_translator.cc:1358-1383 and
        // :1399-1412) -- and those two masks are the word0 write-mask bits and
        // nothing else (ucode.h:2011-2023). GatherAluInstructionInformation then
        // calls GatherAluResultInformation on both results UNCONDITIONALLY (:502-503),
        // which raises register_static_address_bound_ for a kRegister result whose
        // used write mask is non-empty (:588-596). So an unknown vector opcode with
        // vectorWriteMask 0xF and vector_dest 0 is a WRITE of r0 in the reference, and
        // reporting it as unwritten is a wrong answer -- silently, because the
        // detector's own name promises it can tell those apart.
        //
        // The single `if (ir.decoded.unknown) continue;` this replaces conflated the
        // two halves. It was RIGHT about the reads and WRONG about the writes.
        //
        // WHAT `written` DOES NOT SAY. It says the GUEST wrote the register. It does
        // not say the emitted HLSL did: AppendBody comments an unknown instruction
        // out and counts it (shader_translator.cpp:1151-1155), so until the opcode is
        // lowered the host seed still decides that register in the generated shader.
        // That separate gap is already reported on its own field,
        // TranslatedShader::unsupportedOpcodeCount (shader_translator.h:33, assigned
        // at :2141), and this census deliberately does not merge the two facts:
        // "the guest wrote it" and "the host emitted it" are claims about different
        // artifacts, and F-609 forbids inventing one from the other.
        //
        // With exportData the destination is the EXPORT target, not the GPR, so an
        // exporting instruction writes no GPR at all -- the reference resolves
        // storage_target away from kRegister in that case (shader_translator.cc:1304-
        // 1348). This mirrors the emitter, which takes the RegName(vectorDest) branch
        // only when exportReg is empty (EmitAlu, the vector and scalar write arms).
        if (in.exportData) continue;
        if (in.vectorWriteMask != 0) {
            const uint32_t r = in.vectorDest & 0x3F;
            if (r < kRegisterCount) written[r] = true;
        }
        if (in.scalarWriteMask != 0) {
            const uint32_t r = in.scalarDest & 0x3F;
            if (r < kRegisterCount) written[r] = true;
        }
    }
}

// One marker row per translation that has at least one read-never-written
// register. Rate-limited on the project's own established shape (first 8, then
// every 200th) so a long soak cannot hand one mitigation shape the log; see the
// same guard at src/render_thread.cpp:3141.
//
// THE `seed=` FIELD IS NOW COMPUTED, and that is the only change to the row. It
// used to be the literal `0.0`, which was a statement about the reference's
// behaviour rather than about this translation. It is now the seed the emitted HLSL
// actually used, read back out of `prog.interpolators` -- the post-intersection set,
// which is the same set AppendRegisters seeds from -- so the row can no longer
// disagree with the shader it describes. A row whose seeds are all zero still prints
// `seed=0.0`, byte for byte: the field is only widened when at least one register
// does NOT take the literal `0.0` seed, and then it lists one entry per register in
// ascending order, either `interpolant<N>`, `0.0`, or `none` for a register the
// emitter declared not at all.
//
// CORRECTION (2026-10-05). This block used to end "so the row can no longer
// disagree with the shader it describes", and to say the seed gate needed "no
// re-derivation and no chance of the row and the source disagreeing". Both were
// false. Table membership alone is not the gate AppendRegisters uses -- it also
// requires referencedRegisters_[reg] -- so the row DID disagree, for 25 measured
// (container,register) pairs over the corpus. The gate below is now AppendRegisters'
// own condition, and the disagreement is counted rather than denied; see the block
// at the seed computation for the measurement and for the mechanism (:1489).
//
// NOTHING ELSE MOVED. Every pre-existing `{}` placeholder and its argument keeps
// its position and its value; the new evidence is one clause APPENDED AT THE VERY
// END (`seedRule=`). Two sentences inside the row were corrected rather than left
// standing, because they asserted a value that is now sometimes false ("the
// translator supplies 0.0" / "would also pick 0.0 here"); a row that states a
// falsehood is worse than a prose edit, and neither sentence is a field.
void LogUnwrittenRegisterReads(const ShaderProgram& prog, bool ps,
                               const std::vector<bool>& referenced,
                               const InterpolatorIntersection* pairing) {
    std::vector<bool> read, written;
    CensusGuestRegisterAccess(prog, read, written);

    std::string regs;
    std::string emitted;
    size_t readTotal = 0;
    size_t writeTotal = 0;
    // Per-register seed, in the same ascending order as `regs`.
    std::string seeds;
    // True when EVERY entry is the literal `0.0`, which is the shape this row
    // has always printed for a translation with no interpolant seed at all. It
    // is what keeps the common case byte-identical; anything else prints the
    // full per-register list.
    bool seedsUniformZero = true;
    for (uint32_t r = 0; r < kRegisterCount; ++r) {
        if (written[r]) ++writeTotal;
        if (!read[r]) continue;
        ++readTotal;
        // read and never written: this is what gets the host-chosen seed.
        if (written[r]) continue;
        if (!regs.empty()) regs += ",";
        regs += "r" + std::to_string(r);
        if (r < referenced.size() && referenced[r]) {
            if (!emitted.empty()) emitted += ",";
            emitted += "r" + std::to_string(r);
        }
        // The seed this register's declaration carries -- and, when it has NO
        // declaration, the fact that it has none.
        //
        // THIS GATE IS NOW AppendRegisters' OWN CONDITION, and the previous
        // version of it was wrong. It used to test post-intersection table
        // membership ALONE, on the strength of a claim in the comment here that
        // "the presence of an entry with this reg in prog.interpolators IS the
        // distinction". It is not. AppendRegisters seeds an interpolant only
        // when the register is in the table AND in referencedRegisters_
        // (`:1132-1135` builds `seeded[]` from both, `:1142-1144` re-tests both),
        // so a register that is in the table yet unreferenced gets NO
        // DECLARATION AT ALL -- and this row then printed `interpolant<N>` for a
        // variable the emitted HLSL does not contain.
        //
        // MEASURED over the 164-file corpus (215 pixel containers), driving the
        // real HlslGenerator through the public TranslateShader entry point:
        //   ROW registers = 145 (container,register) pairs in 116 containers
        //   of those, with NO `float4 rN = ...` line in the emitted HLSL = 59
        //     pairs in 46 containers
        //   of THOSE 59, in the interpolator table = 25 pairs in 25 containers
        //   -> 25 rows would name an interpolant seed for a register that is
        //      not in the generated shader
        // The other 34 print `0.0`, which is what AppendRegisters would have
        // given had it declared them, so they are not seed-divergent.
        //
        // THE CAUSE, MEASURED AND NOT INFERRED, because it is not the one this
        // comment used to assert. It is not "a read with a zero write mask": the
        // corpus shows 0 pairs of that shape. It is shader_translator.cpp:1489,
        //     if (instr.exportData) vectorWriteMask &= ~instr.scalarWriteMask;
        // With exportData set and scalarWriteMask covering every bit of
        // vectorWriteMask the EFFECTIVE vector write mask is 0, so the vector
        // write arm at :1496 is skipped, EmitOperand is never called, and
        // NoteOperand never fires -- even though both RAW write masks are
        // non-zero. Every one of the 25 prints `vwm=0xE swm=0xF export=1` or
        // `vwm=0x0 swm=0x8 export=1`, i.e. effective 0x0.
        //
        // `none` is the honest entry for such a register: it has no declaration,
        // so it has no seed, and printing `0.0` would be as false as printing
        // `interpolant<N>` was. `none` is already this row's own vocabulary --
        // the `emittedReads` field below prints it for an empty list -- so no new
        // token enters the log and no tracked marker's name appears here.
        //
        // LOG-ONLY. This changes which string a field prints and nothing else.
        // `AppendRegisters`, `AppendSignature` and the emitted body are all
        // untouched, so no declaration, no input element and no shader value
        // can move.
        uint32_t interpReg = 0xFFFFFFFFu;
        const bool declared =
            r < referenced.size() && referenced[r];
        if (declared) {
            for (const auto& in : prog.interpolators)
                if (in.reg == r) { interpReg = in.usageIndex; break; }
        }
        if (!seeds.empty()) seeds += ",";
        if (!declared) {
            seeds += "none";
            seedsUniformZero = false;
        } else if (interpReg != 0xFFFFFFFFu) {
            seeds += "interpolant" + std::to_string(interpReg);
            seedsUniformZero = false;
        } else {
            seeds += "0.0";
        }
    }
    if (regs.empty()) return;  // nothing unwritten is read; no row is owed.

    static std::atomic<uint32_t> seq{0};
    const uint32_t n = seq.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (n > 8u && (n % 200u) != 0u) return;

    // The appended clause. `psNumReg` and the two count terms are already on
    // `prog`; the two masks are only meaningful with a pairing, and a null pairing
    // prints them as the absent state rather than as a zero that looks measured.
    std::string rule = "no-vs-pairing";
    if (pairing != nullptr) {
        rule = std::string(pairing->vsKnown ? "vs-known " : "vs-unknown ") +
               "vsWrites=0x" + [pairing]() {
                   char buf[16];
                   std::snprintf(buf, sizeof(buf), "%08X",
                                 pairing->vsWritesInterpolators);
                   return std::string(buf);
               }() +
               " psInputMask=0x" + [pairing]() {
                   char buf[16];
                   std::snprintf(buf, sizeof(buf), "%08X", pairing->psInputMask);
                   return std::string(buf);
               }() +
               " intersected=0x" + [pairing]() {
                   char buf[16];
                   std::snprintf(buf, sizeof(buf), "%08X", pairing->intersected);
                   return std::string(buf);
               }() +
               " psNumReg=" + std::to_string(pairing->psNumReg) +
               " staticBound=" + std::to_string(pairing->staticBound) +
               " dynRegCount=" + std::to_string(pairing->dynamicRegCount) +
               " interpCount=" + std::to_string(pairing->interpolatorCount) +
               " kept=" + std::to_string(pairing->kept) + "/" +
               std::to_string(pairing->candidatesBefore);
    }

    MCLA_LOG_WARN(
        "SHADER-UNWRITTEN-REG-READ #{} stage={} regs=[{}] emittedReads=[{}] "
        "seed={} readTotal={} writeTotal={} -- every register in regs is READ "
        "by a guest instruction and NO guest instruction writes it, so its value "
        "is not in the binary and the translator supplies the seed listed in the "
        "seed field, per register. That seed is the reference's own: an unwritten "
        "GPR takes interpolant N when bit N is in interpolator_mask and 0.0 "
        "otherwise (spirv_shader_translator.cc:2052-2070, and "
        "dxbc_shader_translator.cc:685-701 on the DXBC path this backend follows), "
        "where interpolator_mask = VS.writes_interpolators() & "
        "PS.GetInterpolatorInputMask(...) (vulkan_command_processor.cc:2206-2211, "
        "d3d12/d3d12_command_processor.cc:2081-2086). emittedReads is the subset "
        "the generated HLSL actually touches; a register in regs but not "
        "emittedReads had its reads dropped because its write mask was 0. NOT a "
        "fabricated colour: a register nobody wrote has no guest value, and an "
        "interpolant seed is a value the paired VS exported. [seedRule={}]",
        n, ps ? "pixel" : "vertex", regs,
        emitted.empty() ? "none" : emitted,
        seedsUniformZero ? std::string("0.0") : seeds,
        readTotal, writeTotal, rule);
}

// HLSL generator for one shader program. Not thread-safe; one instance per
// translation.
class HlslGenerator {
public:
    HlslGenerator(const ShaderProgram& prog, std::string& out,
                  uint64_t& unsupported, bool applyClipDisableRescale)
        : prog_(prog),
          out_(out),
          unsupported_(unsupported),
          applyClipDisableRescale_(applyClipDisableRescale) {
        for (const auto& c : prog_.constants) {
            if (c.info.registerSet == RegisterSet::Sampler)
                samplerConstants_.emplace(c.info.registerIndex, &c);
        }
    }

    // F-609 item 4. Set before Generate(); a null pointer means "no pin", which
    // is the pre-existing behaviour (semantics from fetch resolution only).
    void SetClipExtent(uint32_t w, uint32_t h) { clipExtW_ = w; clipExtH_ = h; }
    void SetExtras(const ShaderTranslateExtras& e) { extras_ = e; }
    void SetVertexInputLayout(const VertexInputSemantic* layout, size_t count) {
        layout_ = layout;
        layoutCount_ = count;
    }

    // The per-draw interpolator intersection TranslateShaderImpl already applied to
    // `prog_` (ApplyInterpolatorIntersection), handed over so the seed marker can
    // report the arithmetic rather than only its result. A null pointer means "no
    // pairing was attempted", which is what a VERTEX translation is.
    void SetPairingReport(const InterpolatorIntersection* report) {
        pairing_ = report;
    }

    const std::string& error() const { return error_; }

    bool Generate() {
        out_.clear();
        error_.clear();
        const bool ps = !prog_.isVertex;

        // Unique identifiers for named constants, built once so the cbuffer
        // declaration and every operand reference agree. Guest tables can
        // contain duplicate names at different registers (e.g. two "ler");
        // later duplicates get a numeric suffix.
        BuildConstantNames();

        // Pass 1: emit the body into a scratch buffer, recording every
        // register / constant / export / input it references (the real emitter
        // is the source of truth for its own references). Pass 2 then declares
        // exactly that set and pastes the cached body back, so nothing is
        // undeclared and nothing is invented.
        const uint64_t baseUnsupported = unsupported_;
        std::string body;
        {
            std::string scratch;
            scratch.swap(out_);      // out_ -> empty; scratch holds caller's old text
            relConstMode_ = false;
            sawRelConst_ = false;
            ResetAnalysis();
            AppendBody(ps);          // writes into the caller's (now empty) buffer
            if (sawRelConst_) {
                // c[N + a0] / c[N + aL] needs the whole bank as one array: re-emit
                // the body with every float constant routed through cRel[].
                relConstMode_ = true;
                out_.clear();
                unsupported_ = baseUnsupported;
                ResetAnalysis();
                AppendBody(ps);
            }
            body.swap(out_);         // cache the generated body
            out_.swap(scratch);      // restore caller's buffer to empty
            // The pixel-shader multiply of the sampled texture by iTexCoord1
            // (r0 * r1) is guest-intended: the intro fades via per-vertex
            // alpha in the vertex colour. Do not strip it.
        }

        // Pin the VS entry signature to the authoritative input layout BEFORE
        // AppendSignature emits it (see PinVertexInputSemantics). A pin that
        // cannot bind an input is a refusal: emitting a semantic the layout does
        // not supply produces a PSO-create failure or a garbage vertex, and
        // there is no third honest option.
        if (!ps && layout_ != nullptr) {
            if (!PinVertexInputSemantics(layout_, layoutCount_)) {
                error_ = "TranslateShaderPinned: a vertex fetch resolved to a semantic the "
                         "authoritative input layout does not supply";
                out_.clear();
                return false;
            }
        }

        AppendHeader(ps);
        AppendSignature(ps);
        AppendRegisters(ps);
        // After AppendRegisters, so the 0.0 seed this row reports is already
        // decided, and referencedRegisters_ is final (pass 1 ran above). Placed
        // on the success path only: the pin refusal returns earlier, and this
        // marker is new, so no existing marker's cadence can be shortened by it.
        LogUnwrittenRegisterReads(prog_, ps, referencedRegisters_, pairing_);
        // FIX (measured): initialize PS color exports to 0 before the body so
        // channels not written by any export stay 0 instead of being clobbered
        // by a later narrow export's per-instruction zero fill (removed above).
        if (ps && psExportRegs_.count(0)) {
            out_ += "    oC0 = float4(0.0, 0.0, 0.0, 0.0);\n";
        }
        if (ps && psExportRegs_.count(1)) {
            out_ += "    oC1 = float4(0.0, 0.0, 0.0, 0.0);\n";
        }
        if (ps && psExportRegs_.count(2)) {
            out_ += "    oC2 = float4(0.0, 0.0, 0.0, 0.0);\n";
        }
        if (ps && psExportRegs_.count(3)) {
            out_ += "    oC3 = float4(0.0, 0.0, 0.0, 0.0);\n";
        }
            // An unwritten VS output trips the D3D12 VS->GS link check (D3D12-DEBUGLAYER id=664, E_INVALIDARG).
        if (!ps) for (uint32_t vd : vsExportDests_) {
                if (vd == 62 || vd >= 16) continue;
                out_ += "    oTexCoord" + std::to_string(vd) + " = float4(0.0, 0.0, 0.0, 0.0);\n";
            }
        out_ += body;
        AppendEpilogue(ps);
        out_ += "}\n";
        return true;
    }

    // Whether AppendEpilogue emitted the clip_disable rescale (see the
    // TranslatedShader::clipRescaleBaked comment in shader_translator.h). Read
    // after Generate(); it reports what the epilogue DID rather than re-reading
    // the process global, so it cannot drift from the emitted HLSL.
    bool clipRescaleBaked() const { return clipRescaleBaked_; }

    uint8_t psExportColorMask() const {
        uint8_t m = 0;
        for (uint32_t r = 0; r < 4; ++r) {
            if (psExportRegs_.count(r)) m |= static_cast<uint8_t>(1u << r);
        }
        return m;
    }
    bool psExportsDepth() const { return psExportRegs_.count(61) != 0; }

    // The TEXTUAL shape of the position output, as a classification decided while
    // emitting (see AppendEpilogue). Empty for a pixel program, which writes no
    // position. This is a NAME for the emitted code, never a value: F-609 forbids
    // reporting a position this file derived rather than one the pipeline produced,
    // and D3D12 offers no shader-output readback that could produce one.
    const std::string& oPosShape() const { return oPosShape_; }
    // The TEXT of the write that produces it, quoted from out_ rather than
    // reconstructed, so the two cannot disagree.
    std::string oPosWriteExpression() const { return oPosWriteExpression_; }

private:
    const ShaderProgram& prog_;
    std::string& out_;
    uint64_t& unsupported_;
    // True until a vertex epilogue omits the rescale. Pixel programs keep true
    // because they have no position epilogue to transform.
    bool clipRescaleBaked_ = true;
    bool applyClipDisableRescale_ = false;
    uint32_t clipExtW_ = 0, clipExtH_ = 0;   // per-draw guest-px extent; 0 = use published
    ShaderTranslateExtras extras_;           // default = byte-identical HLSL to before it existed

    static bool EnvOff(const char* name) { const char* e = std::getenv(name); return e && e[0] == '1'; }
    // MD-14: PS param_gen register is written only when requested, in range, and read by the body.
    bool ParamGenActive() const {
        return !prog_.isVertex && !EnvOff("MCLA_NO_PARAM_GEN") && extras_.paramGenReg >= 0 &&
               extras_.paramGenReg < 16 && referencedRegisters_[extras_.paramGenReg];
    }
    // MD-21: number of packed user clip planes this VS emits (0 = none).
    uint32_t UcpCount() const {
        if (!prog_.isVertex || EnvOff("MCLA_NO_UCP")) return 0;
        uint32_t m = extras_.ucpMask & 0x3Fu, n = 0;
        for (; m; m &= m - 1) ++n;
        return n;
    }
    // Set by AppendEpilogue. Both are TEXT/classification, never numbers: see the
    // oPosShape() comment above and AppendEpilogue's own block.
    std::string oPosShape_;
    std::string oPosWriteExpression_;
    std::unordered_map<uint32_t, const ConstantEntry*> samplerConstants_;

    // What the body referenced during emission (see Generate pass 1). Used to
    // declare exactly that set: which registers/constants the body reads, which
    // export surfaces it writes (PS color/depth regs, VS interpolator dests),
    // and which VS inputs its fetches resolve to.
    std::vector<bool> referencedRegisters_;      // index -> used
    std::set<uint32_t> referencedConstants_;     // c-registers referenced
    bool relConstMode_ = false;   // body indexes the float bank as cRel[N + a0/aL]
    bool sawRelConst_ = false;    // pass 1 met a register-relative constant operand
    std::set<uint32_t> psExportRegs_;            // 0-3 (oC) or 61 (oDepth)
    std::set<uint32_t> vsExportDests_;           // VS export vectorDest values
    std::vector<VertexInputRef> vsInputs_;       // VS entry inputs (first-fetch order)
    std::unordered_set<uint32_t> usedSamplers_;  // texture-fetch constIndices referenced
    std::set<uint32_t> usedSamplerRegs_;         // sampler registers referenced (< 16)
    std::unordered_map<uint32_t, uint32_t> samplerDim_;
    bool usesXgrad_ = false;
    bool usesXweights_ = false;
    bool usesXfLod_ = false;
    bool usesXfGradH_ = false;
    bool usesXfGradV_ = false;
    // Authoritative input layout, or null for the unpinned path.
    const VertexInputSemantic* layout_ = nullptr;
    size_t layoutCount_ = 0;
    // Per-draw interpolator intersection report, or null when none was attempted.
    const InterpolatorIntersection* pairing_ = nullptr;
    std::string error_;

    void ResetAnalysis() {
        referencedRegisters_.assign(kRegisterCount, false);
        referencedConstants_.clear();
        psExportRegs_.clear();
        vsExportDests_.clear();
        vsInputs_.clear();
        usedSamplers_.clear();
        usedSamplerRegs_.clear();
        samplerDim_.clear();
        usesXgrad_ = false;
        usesXweights_ = false;
        usesXfLod_ = false;
        usesXfGradH_ = false;
        usesXfGradV_ = false;
    }

    void NoteRegister(uint32_t r) {
        if (r < kRegisterCount) referencedRegisters_[r] = true;
    }
    void NoteConstant(uint32_t c) {
        referencedConstants_.insert(c);
    }
    // Record an operand exactly as EmitOperand resolves it: a register read
    // (select) with the high mask bits stripped, or a constant c{N} read.
    void NoteOperand(uint32_t reg, bool select) {
        if (select) {
            NoteRegister(reg & 0x3F);
        } else {
            NoteConstant(reg);
        }
    }
    void NoteExport(bool ps, uint32_t vectorDest) {
        if (ps) psExportRegs_.insert(vectorDest);
        else vsExportDests_.insert(vectorDest);
    }
    // Record a VS vertex-fetch input reference, deduped by (usage, usageIndex)
    // so the entry signature lists each resolved input once.
    void NoteVertexInput(uint32_t usage, uint32_t usageIndex) {
        if (usage >= 14) usage = 0;  // clamp, mirroring the operand fallback
        for (const auto& in : vsInputs_) {
            if (in.usage == usage && in.usageIndex == usageIndex) return;
        }
        vsInputs_.push_back(VertexInputRef{usage, usageIndex, 0, 0, 0, 0, 0});
    }

    // F-609 item 4: pin each entry-signature input's SEMANTIC to the
    // authoritative input layout. The parameter name (iPosition0) is already
    // resolved by the fetch; only the `: SEMANTICn` it is bound to can
    // disagree with the layout, and that disagreement is what produced
    // POSITION0-vs-TEXCOORDn PSO failures. Returns false when an input has no
    // counterpart in the layout, which is a refusal, not a guess.
    bool PinVertexInputSemantics(const VertexInputSemantic* layout, size_t count) {
        for (auto& in : vsInputs_) {
            bool bound = false;
            // 1. exact (usage, usageIndex)
            for (size_t i = 0; i < count && !bound; ++i) {
                if (layout[i].usage == in.usage && layout[i].usageIndex == in.usageIndex) {
                    in.usage = layout[i].usage;
                    in.usageIndex = layout[i].usageIndex;
                    bound = true;
                }
            }
            // 2. the layout's ONLY element with this usage. Refused when the
            //    usage appears more than once: picking one would be arbitrary.
            if (!bound) {
                size_t matches = 0;
                size_t matchIndex = 0;
                for (size_t i = 0; i < count; ++i) {
                    if (layout[i].usage != in.usage) continue;
                    ++matches;
                    matchIndex = i;
                    if (matches > 1) break;
                }
                if (matches == 1) {
                    in.usage = layout[matchIndex].usage;
                    in.usageIndex = layout[matchIndex].usageIndex;
                    bound = true;
                }
            }
            if (!bound) return false;
        }
        // Two inputs pinned onto one semantic would emit a signature with a
        // duplicated SEMANTICn. D3D12 rejects that at PSO creation with an opaque
        // "input layout does not match shader" error; refuse here with a reason.
        for (size_t i = 0; i < vsInputs_.size(); ++i) {
            for (size_t j = i + 1; j < vsInputs_.size(); ++j) {
                if (vsInputs_[i].usage == vsInputs_[j].usage &&
                    vsInputs_[i].usageIndex == vsInputs_[j].usageIndex)
                    return false;
            }
        }
        // F-900: log the PINNED inputs (post exact/fallback rewrite). This is
        // the semantic set the VS signature is emitted from -- if the pin
        // silently re-indexed an input (unique-usage fallback), the HLSL
        // signature still prints the pinned pair, so a layout-vs-shader
        // mismatch would show here as missing/duplicated entries, not in the
        // dump. Throttled: translations repeat per draw.
        {
            static std::atomic<uint32_t> s_pinDiag{0};
            const uint32_t pd = s_pinDiag.fetch_add(1, std::memory_order_relaxed) + 1;
            if (pd <= 4) {
                std::string txt;
                for (const auto& in : vsInputs_) {
                    txt += std::string(VertexUsageSemanticName(in.usage)) +
                           std::to_string(in.usageIndex) + " ";
                }
                MCLA_LOG_INFO("VS-PIN #{} inputs={} [{}]", pd, vsInputs_.size(), txt.c_str());
            }
        }
        return true;
    }

    // ---- helpers ----------------------------------------------------------

    static bool IsIdentifierStart(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool IsIdentifier(char c) {
        return IsIdentifierStart(c) || (c >= '0' && c <= '9');
    }
    // Return a valid HLSL identifier for a constant name, or empty.
    static std::string SanitizedName(const std::string& name) {
        if (name.empty() || !IsIdentifierStart(name[0])) return {};
        for (char c : name)
            if (!IsIdentifier(c)) return {};
        return name;
    }

    // The named float4 constant whose register range contains `reg`, or
    // nullptr when the guest has no nameable constant covering that register
    // (unnamed entry or bogus index). Used by the operand emitter so every
    // c{N} reference stays consistent with the cbuffer declarations.
    const ConstantEntry* FindNamedConstant(uint32_t reg) const {
        for (const auto& c : prog_.constants) {
            if (c.info.registerSet != RegisterSet::Float4) continue;
            if (SanitizedName(c.name).empty()) continue;
            const uint32_t base = c.info.registerIndex;
            const uint32_t count = std::max<uint32_t>(c.info.registerCount, 1);
            if (reg >= base && reg < base + count) return &c;
        }
        return nullptr;
    }
    bool NamedConstantCovers(uint32_t reg) const {
        return FindNamedConstant(reg) != nullptr;
    }

    // Guest constant tables can name multiple float4 entries identically at
    // different registers; a cbuffer cannot redeclare an identifier. Build a
    // per-register unique name once (first use keeps the guest name, later
    // duplicates get "_<reg>") so declarations and operand references agree.
    std::unordered_map<uint32_t, std::string> constantNames_;

    void BuildConstantNames() {
        constantNames_.clear();
        std::set<std::string> used;
        for (const auto& c : prog_.constants) {
            if (c.info.registerSet != RegisterSet::Float4) continue;
            std::string name = SanitizedName(c.name);
            if (name.empty()) continue;
            if (used.count(name))
                name += "_" + std::to_string(c.info.registerIndex);
            used.insert(name);
            constantNames_[c.info.registerIndex] = name;
        }
    }
    std::string ConstantName(uint32_t registerIndex) const {
        auto it = constantNames_.find(registerIndex);
        return it != constantNames_.end() ? it->second : std::string{};
    }
    static std::string RegName(uint32_t reg) {
        return "r" + std::to_string(reg);
    }

    // Build an operand string for `kind`, following the reference op().
    // `forceMask` (when non-zero) overrides the per-kind component mask so a
    // caller can ask for a SINGLE slot component (bit i set = slot i). Without
    // it the per-opcode/write-mask swizzle is emitted as before. Callers must
    // NEVER append ".x"/".w" style suffixes to EmitOperand's result: the
    // returned operand is already fully swizzled, and a second suffix produces
    // `rN.xyzw.w` -- HLSL DXC rejects.
    std::string EmitOperand(const DecodedInstruction& instr, OperandKind kind,
                            uint32_t forceMask = 0) {
        uint32_t reg = 0;
        uint32_t swizzle = 0;
        bool select = true;
        bool negate = false;
        bool abs = false;

        switch (kind) {
        case OperandKind::ScalarConstant0:
            reg = instr.src3Register;
            swizzle = instr.src3Swizzle;
            select = false;
            negate = instr.src3Negate;
            abs = instr.absConstants;
            break;
        case OperandKind::ScalarConstant1:
            reg = (uint32_t(instr.scalarOpcode) & 1) |
                  (uint32_t(instr.src3Select) << 1) |
                  (instr.src3Swizzle & 0x3C);
            swizzle = instr.src3Swizzle;
            select = true;
            negate = instr.src3Negate;
            abs = instr.absConstants;
            break;
        default:
            switch (kind) {
            case OperandKind::Vector0:
                reg = instr.src1Register;
                swizzle = instr.src1Swizzle;
                select = instr.src1Select;
                negate = instr.src1Negate;
                break;
            case OperandKind::Vector1:
                reg = instr.src2Register;
                swizzle = instr.src2Swizzle;
                select = instr.src2Select;
                negate = instr.src2Negate;
                break;
            default:  // Vector2 / Scalar0 / Scalar1
                reg = instr.src3Register;
                swizzle = instr.src3Swizzle;
                select = instr.src3Select;
                negate = instr.src3Negate;
                break;
            }
            if (select) {
                abs = (reg & 0x80) != 0;
                reg &= 0x3F;
            } else {
                abs = instr.absConstants;
            }
            break;
        }

        std::string regFormatted;
        // Record the reference exactly as resolved (register vs constant) so
        // the declarations in pass 2 cover every operand the body emits.
        NoteOperand(reg, select);
        if (select) {
            regFormatted = "r" + std::to_string(reg);
        } else {
            // Constant operand. Named entries resolve by name (arrays by index);
            // anything else falls back to the declared c{N} registers.
            // Xenia ucode.h src_const_is_addressed(): the first constant operand of
            // the instruction takes const_0_rel_abs, any later one const_1_rel_abs.
            const int opIdx = kind == OperandKind::Vector0 ? 1 : kind == OperandKind::Vector1 ? 2 : 3;
            const bool rel = opIdx == 1 ? instr.const0Relative
                           : opIdx == 2 ? (instr.src1Select ? instr.const0Relative : instr.const1Relative)
                           : ((instr.src1Select && instr.src2Select) ? instr.const0Relative : instr.const1Relative);
            if (rel) sawRelConst_ = true;
            const ConstantEntry* named = relConstMode_ ? nullptr : FindNamedConstant(reg);
            if (relConstMode_ && rel) {
                regFormatted = "cRel[clamp(" + std::to_string(reg) +
                               (instr.constAddressRegisterRelative ? " + a0" : " + aL") + ", 0, 255)]";
            } else if (named) {
                const std::string name =
                        ConstantName(named->info.registerIndex);
                if (name.empty()) {
                    regFormatted = "c" + std::to_string(reg);
                } else if (named->info.registerCount > 1) {
                    // Named array constant; relative addressing appends aL/a0.
                    uint32_t offset = reg - named->info.registerIndex;
                    std::string idx = std::to_string(offset);
                    if (rel)
                        idx += instr.constAddressRegisterRelative ? " + a0" : " + aL";
                    regFormatted = name + "[" + idx + "]";
                } else {
                    regFormatted = name;
                }
            } else {
                regFormatted = "c" + std::to_string(reg);
            }
        }

        std::string result;
        if (negate) result += '-';
        if (abs) result += "abs(";
        result += regFormatted;
        result += '.';

        // The swizzle is COMPONENT-RELATIVE: destination component i takes source
        // component ((swizzle >> (2*i)) + i) & 3, NOT (swizzle >> (2*i)) & 3.
        //
        // MEASURED, not inferred from the field name. The reference chain is
        // shader.h:118-123 (GetSwizzledAluSourceComponent) ->
        // ucode::AluInstruction::GetSwizzledComponentIndex at ucode.h:1999-2003,
        // whose body is `((swizzle >> (2 * component_index)) + component_index) & 3`,
        // with the comment "Swizzles are component-relative" at ucode.h:2068. The
        // `+ component_index` is the whole difference and it is load-bearing: for
        // the identity swizzle 0 the reference returns component_index (xyzw), and
        // the old expression below returned 0 for all four (xxxx).
        //
        // WHAT IT WAS DOING, measured on draw #1's real vertex program
        // (build/w710.log:6599 CP-SHADER-BODY #3, 27 dwords, hash-verified against
        // DRAW-CP-PIPE #1's vsHash=22BB5C3AE64A3D71 at w710.log:6735): the oPos
        // export is instruction [3], src1Swizzle=0x0, vectorWriteMask=15,
        // vectorDest=62. It emitted
        //     oPos.xyzw = max(r1.xxxx, r1.xxxx);
        // so oPos.x, oPos.y and oPos.z ALL received iPosition0.x and the guest's Y
        // and Z never reached the output at all. With the correct component-relative
        // read the same instruction emits
        //     oPos.xyzw = max(r1.xyzw, r1.xyzw);
        // Falsifier for the fix: if the identity swizzle 0x00 did NOT mean `xyzw`,
        // then a real program whose export swizzle is 0 would still be mis-emitted.
        // The check that settles it is %TEMP%\opencode\opos_h\gs2.txt, which prints
        // both spellings side by side for the measured 0x0.
        auto swizzledComponent = [swizzle](uint32_t i) {
            return ((swizzle >> (2u * i)) + i) & 0x3u;
        };

        switch (kind) {
        case OperandKind::Vector0:
        case OperandKind::Vector1:
        case OperandKind::Vector2: {
            uint32_t mask;
            if (forceMask != 0) {
                mask = forceMask;
            } else
            switch (instr.vectorOpcode) {
            case AluVectorOpcode::Dp2Add:
                mask = (kind == OperandKind::Vector2) ? 0b1 : 0b11;
                break;
            case AluVectorOpcode::Dp3:
                mask = 0b111;
                break;
            case AluVectorOpcode::Dp4:
            case AluVectorOpcode::Max4:
                mask = 0b1111;
                break;
            default:
                mask = instr.vectorWriteMask != 0 ? instr.vectorWriteMask : 0b1;
                break;
            }
            for (uint32_t i = 0; i < 4; i++) {
                if ((mask >> i) & 0x1)
                    result += kSwizzle[swizzledComponent(i)];
            }
            break;
        }
        case OperandKind::Scalar0:
        case OperandKind::ScalarConstant0:
            // A scalar operand is the component the reference reads with a FIXED
            // index, not with the destination index: shader_translator.cc:1244 and
            // :1448 pass 3 (the W slot) for Scalar0/ScalarConstant0, and :1248
            // passes 0 (the X slot) for the X half of a 2-component scalar pair.
            result += kSwizzle[swizzledComponent(3)];
            break;
        case OperandKind::Scalar1:
        case OperandKind::ScalarConstant1:
            // shader_translator.cc:1247-1248: components[0] is read at index 3 and
            // components[1] at index 0, so the 2-component case is emitted by this
            // case using its own fixed indices below.
            result += kSwizzle[swizzledComponent(0)];
            break;
        }

        if (abs) result += ')';
        return result;
    }

    // Xenia kSetp*Push comparison of src1 against 0 (dxbc_shader_translator_alu.cc:390-505).
    static std::string PushCmp(const DecodedInstruction& instr, const std::string& v) {
        switch (instr.vectorOpcode) {
        case AluVectorOpcode::SetpEqPush: return "(" + v + " == 0.0)";
        case AluVectorOpcode::SetpNePush: return "(" + v + " != 0.0)";
        case AluVectorOpcode::SetpGtPush: return "(0.0 < " + v + ")";
        default:                          return "(" + v + " >= 0.0)";
        }
    }

    static std::string ExportRegisterName(const DecodedInstruction& instr,
                                          uint32_t reg) {
        // Xenos export registers: 0-3 -> color, 61 -> depth.
        if (reg <= 3) return "oC" + std::to_string(reg);
        if (reg == 61) return "oDepth";
        return {};
    }

    // ---- header / setup ---------------------------------------------------

    void AppendHeader(bool ps) {
        out_ += "// Xenos -> HLSL (mcla::renderer Phase 5 translation)\n";
        out_ += "#define FLT_MIN 1.17549435e-38\n";
        out_ += "#define FLT_MAX 3.40282346e+38\n";
        out_ += "\n";
        // Xenos Cnd ops emit select(cond, a, b). The guest instruction is a
        // PER-COMPONENT conditional move -- Xenia's own words, at
        // .research/xenia/src/xenia/gpu/ucode.h:1459-1465:
        //   "Per-Component Floating-Point Conditional Move If Equal /
        //    cndeq dest, src0, src1, src2
        //        dest.x = src0.x == 0.0 ? src1.x : src2.x;  (and .y/.z/.w likewise)"
        // and its interpreter loops i=0..3 over every lane
        // (.research/xenia/src/xenia/gpu/shader_interpreter.cc:415-421), with the
        // SPIR-V path emitting spv::OpSelect per component.
        //
        // So the condition must select lane by lane. `c ? a : b` is a UNIFORM (scalar)
        // select, which is both semantically wrong here and rejected by HLSL outright:
        //   "condition for short-circuiting ternary operator must be scalar, for
        //    non-scalar types use 'select'"
        // That was the whole of the failure: all 517 dumped programs stopped at this
        // prelude. The three vector-condition overloads are now componentwise.
        //
        // Condition arity: EmitOperand builds the predicate from the CND vector write
        // mask, so the condition is 1/2/3/4 lanes wide and each lane is POSITIONAL --
        // condition component j tests data component j. Measured over the 517-program
        // dump set, 1-, 2-, 3- and 4-lane swizzles all occur (e.g. 'z', 'xz', 'yxz',
        // 'xwzz'), so all four widths must be live.
        //
        // DECLARED SIMPLIFICATION: lanes past the condition's width are outside the
        // guest write mask, so the hardware never writes them -- they are don't care.
        // They are filled from `b` here, preserving the previous bool4(c,false,..)
        // padding. Emitting a full-width predicate would be exact rather than
        // don't-care, but it would change the translated BODY, which is out of scope
        // here (the body must stay byte-identical across this fix).
        out_ += "float4 select(bool4 c, float4 a, float4 b) { return float4(c.x ? a.x : b.x, c.y ? a.y : b.y, c.z ? a.z : b.z, c.w ? a.w : b.w); }\n";
        out_ += "float4 select(bool3 c, float4 a, float4 b) { return float4(c.x ? a.x : b.x, c.y ? a.y : b.y, c.z ? a.z : b.z, false ? a.w : b.w); }\n";
        out_ += "float4 select(bool2 c, float4 a, float4 b) { return float4(c.x ? a.x : b.x, c.y ? a.y : b.y, false ? a.z : b.z, false ? a.w : b.w); }\n";
        // A scalar condition broadcast across a vector IS the per-component select
        // HLSL already means, so these three were legal and correct as written.
        out_ += "float4 select(bool c, float4 a, float4 b) { return c ? a : b; }\n";
        out_ += "float3 select(bool c, float3 a, float3 b) { return c ? a : b; }\n";
        out_ += "float2 select(bool c, float2 a, float2 b) { return c ? a : b; }\n";
        out_ += "float select(bool c, float a, float b) { return c ? a : b; }\n";
        out_ += "// Xenos multiply: 0 * anything = 0 (incl. inf/NaN), Xenia alu.cc:91-121; Rcpf/Rsqf flush inf to 0.\n";
        out_ += "float xm(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n";
        out_ += "float2 xm(float2 a, float2 b) { return float2(xm(a.x, b.x), xm(a.y, b.y)); }\n";
        out_ += "float3 xm(float3 a, float3 b) { return float3(xm(a.x, b.x), xm(a.y, b.y), xm(a.z, b.z)); }\n";
        out_ += "float4 xm(float4 a, float4 b) { return float4(xm(a.x, b.x), xm(a.y, b.y), xm(a.z, b.z), xm(a.w, b.w)); }\n";
        out_ += "float xdot(float2 a, float2 b) { return xm(a.x, b.x) + xm(a.y, b.y); }\n";
        out_ += "float xdot(float3 a, float3 b) { return xm(a.x, b.x) + xm(a.y, b.y) + xm(a.z, b.z); }\n";
        out_ += "float xdot(float4 a, float4 b) { return xm(a.x, b.x) + xm(a.y, b.y) + xm(a.z, b.z) + xm(a.w, b.w); }\n";
        // Xenia dxbc_shader_translator_alu.cc:246-353 (kCube): (T, S, 2*major, face). P2; the old passthrough.
        out_ += "float4 xcube(float4 s) {\n"
                "    if (abs(s.z) >= abs(s.x) && abs(s.z) >= abs(s.y))\n"
                "        return float4(-s.y, s.z < 0.0 ? -s.x : s.x, 2.0 * s.z, s.z < 0.0 ? 5.0 : 4.0);\n"
                "    if (abs(s.y) >= abs(s.x))\n"
                "        return float4(s.y < 0.0 ? -s.z : s.z, s.x, 2.0 * s.y, s.y < 0.0 ? 3.0 : 2.0);\n"
                "    return float4(-s.y, s.x < 0.0 ? s.z : -s.z, 2.0 * s.x, s.x < 0.0 ? 1.0 : 0.0);\n"
                "}\n";
        out_ += "float xrcpf(float x) { float r = 1.0 / x; return isinf(r) ? 0.0 : r; }\n";
        out_ += "float xrsqf(float x) { float r = rsqrt(x); return isinf(r) ? 0.0 : r; }\n";
        // tfetch coordinate: texel offset (+ Xenia's 1.5/1024 rounding epsilon, dxbc_shader_translator_fetch.cc:752)
        // and unnormalized (tx_coord_denorm) coordinates, both relative to the bound texture's size.
        if (XuvRenderScaleEnabled()) {
            for (uint32_t n : std::set<uint32_t>(usedSamplers_.begin(), usedSamplers_.end()))  // sorted: stable HLSL text
            {
                if (n < 32u && ((extras_.xuvRsMask >> n) & 1u) && extras_.xuvRsScale > 1u)
                    out_ += "#define XE_RS_" + std::to_string(n) + " " + std::to_string(extras_.xuvRsScale) + ".0\n";
                out_ += "#ifndef XE_RS_" + std::to_string(n) + "\n#define XE_RS_" + std::to_string(n) + " 1.0\n#endif\n";
            }
            // d = host size / rs = guest-pixel size of the texture.
            out_ += "float2 xuv(Texture2D t, float2 uv, float2 o, bool dn, float rs) { uint w, h; t.GetDimensions(w, h); "
                    "float2 d = float2(max(w, 1u), max(h, 1u)) / rs; return dn ? (uv + o) / d : uv + o / d; }\n";
            if (usesXgrad_)
                out_ += "float2 xgrad(Texture2D t, float2 g, float rs) { uint w, h; t.GetDimensions(w, h); "
                        "return g / (float2(max(w, 1u), max(h, 1u)) / rs); }\n";
            if (usesXweights_)
                out_ += "float2 xweights(Texture2D t, float2 uv, float2 o, bool dn, float rs) { uint w, h; t.GetDimensions(w, h); "
                        "float2 d = float2(max(w, 1u), max(h, 1u)) / rs; return frac((dn ? uv : uv * d) + o); }\n";
        } else {
        out_ += "float2 xuv(Texture2D t, float2 uv, float2 o, bool dn) { uint w, h; t.GetDimensions(w, h); "
                "float2 d = float2(max(w, 1u), max(h, 1u)); return dn ? (uv + o) / d : uv + o / d; }\n";
        if (usesXgrad_) {
            out_ += "float2 xgrad(Texture2D t, float2 g) { uint w, h; t.GetDimensions(w, h); "
                    "return g / float2(max(w, 1u), max(h, 1u)); }\n";
        }
        if (usesXweights_) {
            out_ += "float2 xweights(Texture2D t, float2 uv, float2 o, bool dn) { uint w, h; t.GetDimensions(w, h); "
                    "float2 d = float2(max(w, 1u), max(h, 1u)); return frac((dn ? uv : uv * d) + o); }\n";
        }
        }
        out_ += "\n";

        // Constant table. Float4 constants are named where the guest provides
        // a valid identifier; otherwise they fall back to c{N}. The cbuffer is
        // closed over every register the body references (referencedConstants_
        // from Generate pass 1), so no operand's c{N} reference is ever left
        // undeclared.
        out_ += "cbuffer ShaderConstants : register(b0)\n{\n";
        if (relConstMode_) {
            // One array over the whole 256-register bank; plain cN reads become macros.
            out_ += "    float4 cRel[256] : packoffset(c0);\n};\n";
            for (uint32_t r : referencedConstants_) {
                if (HasFloatDef(r) || r > 255) continue;
                out_ += "#define c" + std::to_string(r) + " cRel[" + std::to_string(r) + "]\n";
            }
            out_ += "\n";
        } else {
        for (const auto& c : prog_.constants) {
            if (c.info.registerSet != RegisterSet::Float4) continue;
            std::string name = ConstantName(c.info.registerIndex);
            if (name.empty()) continue;
            if (c.info.registerCount > 1) {
                out_ += "    float4 " + name + "[" +
                        std::to_string(c.info.registerCount) +
                        "] : packoffset(c" +
                        std::to_string(c.info.registerIndex) + ");\n";
            } else {
                out_ += "    float4 " + name + " : packoffset(c" +
                        std::to_string(c.info.registerIndex) + ");\n";
            }
        }
        for (uint32_t r : referencedConstants_) {
            if (NamedConstantCovers(r)) continue;
            if (HasFloatDef(r)) continue;
            out_ += "    float4 c" + std::to_string(r) + " : packoffset(c" +
                    std::to_string(r) + ");\n";
        }
        out_ += "};\n\n";
        }

        if (UcpCount())  // MD-21: PA_CL_UCP_n_{X,Y,Z,W} packed by enabled plane; backend must bind a VS CBV at b1
            out_ += "cbuffer XeUcp : register(b1) { float4 xUcp[6]; };\n\n";

        if (!ps && RawVbTranslate()) {
            // Raw guest VB (root SRV param 4). Dword-aligned loads of the host-order (already endian-swapped) stream.
            out_ += "#if defined(XE_RAWVB)\n"
                    "ByteAddressBuffer g_rawVb : register(t0, space1);\n"
                    "float4 xrvU8(uint v) { return float4(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24) / 255.0; }\n"
                    "float4 xrvU1010(uint v) { return float4(v & 1023u, (v >> 10) & 1023u, (v >> 20) & 1023u, (v >> 30) & 3u) "
                    "/ float4(1023.0, 1023.0, 1023.0, 3.0); }\n"
                    "float4 xrvU16x2(uint v) { return float4(v & 65535u, v >> 16, 0.0, 65535.0) / 65535.0; }\n"
                    "float4 xrvU16x4(uint2 v) { return float4(v.x & 65535u, v.x >> 16, v.y & 65535u, v.y >> 16) / 65535.0; }\n"
                    "float4 xrvF16x2(uint v) { return float4(f16tof32(v & 65535u), f16tof32(v >> 16), 0.0, 1.0); }\n"
                    "float4 xrvF16x4(uint2 v) { return float4(f16tof32(v.x & 65535u), f16tof32(v.x >> 16), "
                    "f16tof32(v.y & 65535u), f16tof32(v.y >> 16)); }\n"
                    "#endif\n\n";
        }

        // Texture/sampler binds for tfetch: populated by EmitFetch only for opcodes
        // that actually address a texture fetch constant (1, 17, 19).
        if (!usedSamplers_.empty()) {
            // One SRV per guest fetch constant: tex<N> at register(tN), N = the
            // tfetch constIndex. The host binds descriptor N at table slot N
            // (render_thread.cpp tex loop, textureFetchSlot). Slots >= 16 do not
            // exist in the table (pipeline worker refuses them as before).
            std::vector<uint32_t> sorted(usedSamplers_.begin(), usedSamplers_.end());
            std::sort(sorted.begin(), sorted.end());
            for (uint32_t n : sorted)
                out_ += "Texture2D   tex" + std::to_string(n) + " : register(t" + std::to_string(n) + ");\n";
            if (UseGuestSampler()) {
                for (uint32_t s : usedSamplerRegs_)
                    out_ += "SamplerState s" + std::to_string(s) + " : register(s" + std::to_string(s) + ");\n";
            } else {
                out_ += "SamplerState s0 : register(s0);\n";
            }
            out_ += "\n";
            // F-900: tex_<slot> placeholders REMOVED (were here). They declared
            // a second Texture2D at the same register(tN) already bound above
            // (t1D/t2D/t3D/tCube), always as Texture2D even for cube/3D/1D, while
            // the emitted body never references tex_* (fetches sample the
            // generic resources). Duplicate-register + wrong-type declarations.
        }
    }

    bool HasFloatDef(uint32_t r) const {
        for (size_t i = 0; i + 4 < prog_.float4DefaultValues.size(); i += 5)
            if (prog_.float4DefaultValues[i] == r) return true;
        return false;
    }

    void AppendFloatDefs() {
        // "def cN": values live in the shader container, not in the guest constant bank.
        for (size_t i = 0; i + 4 < prog_.float4DefaultValues.size(); i += 5) {
            const uint32_t r = prog_.float4DefaultValues[i];
            if (!referencedConstants_.count(r)) continue;
            out_ += fmt::format("    float4 c{} = asfloat(uint4(0x{:X}u, 0x{:X}u, 0x{:X}u, 0x{:X}u));\n", r,
                                prog_.float4DefaultValues[i + 1], prog_.float4DefaultValues[i + 2],
                                prog_.float4DefaultValues[i + 3], prog_.float4DefaultValues[i + 4]);
        }
    }

    void AppendRegisters(bool ps) {
        AppendFloatDefs();
        // Per-register float4 locals (a Xenos shader register is one float4).
        // Registers PS interpolators seed are declared with the seed value
        // directly; everything else starts at 1.0 (VS fetches overwrite it;
        // for a PS without the interpolant, tex*1 is tex).
        std::vector<bool> seeded(kRegisterCount, false);
        for (const auto& interp : prog_.interpolators)
            if (interp.reg < kRegisterCount && referencedRegisters_[interp.reg])
                seeded[interp.reg] = true;

        for (uint32_t r = 0; r < kRegisterCount; r++) {
            if (!referencedRegisters_[r] || seeded[r]) continue;
            out_ += "    float4 r" + std::to_string(r) + " = 1.0;\n";
        }
        if (ps) {
            for (const auto& interp : prog_.interpolators) {
                if (interp.reg >= kRegisterCount || !referencedRegisters_[interp.reg])
                    continue;
                const uint32_t usage = interp.usage >= 14 ? 0 : interp.usage;
                out_ += "    float4 r" + std::to_string(interp.reg) + " = i" +
                        std::string(kUsageVariables[usage]) +
                        std::to_string(interp.usageIndex) + ";\n";
            }
        }
        if (ps && ParamGenActive()) {
            // MD-14, Xenia dxbc_shader_translator.cc:702-780: x = floor(px)/scale, y = floor(py)/scale,
            // z = 0, w = 0; sign bit of x = back face (polygons only), of y = point, of z = line.
            // Point-sprite ZW (the XESPRITETEXCOORD input) has no D3D12 equivalent without a GS: left 0.
            const std::string rn = "r" + std::to_string(extras_.paramGenReg);
            const std::string sc = FloatLit(1.0f / float(extras_.renderScale ? extras_.renderScale : 1u));
            out_ += "    " + rn + " = float4(floor(iPos.xy) * " + sc + ", 0.0, 0.0);\n";
            if (extras_.paramGenPrim == 0)
                out_ += "    if (!iFront) " + rn + ".x = asfloat(asuint(" + rn + ".x) | 0x80000000u);\n";
            else if (extras_.paramGenPrim == 1)
                out_ += "    " + rn + ".z = asfloat(0x80000000u);\n";
            else
                out_ += "    " + rn + ".y = asfloat(asuint(" + rn + ".y) | 0x80000000u);\n";
        }
        // Hardware seeds r0.x with the vertex index (as float) -- the value the guest VS turns into its vfetch address.
        if (!ps && RawVbTranslate() && referencedRegisters_[0])
            out_ += "#if defined(XE_RAWVB)\n    r0.x = (float)xVid;\n#endif\n";
        out_ += "    float ps = 0.0;\n";
        if (tl_hvUV)
            out_ += "    float4 hvUV=0; int hvUVn=0;\n";
        out_ += "    int a0 = 0;\n";
        out_ += "    int aL = 0;\n";
        out_ += "    bool p0 = false;\n";
        if (usesXfLod_)   out_ += "    float xfLod = 0.0;\n";
        if (usesXfGradH_) out_ += "    float3 xfGradH = float3(0.0, 0.0, 0.0);\n";
        if (usesXfGradV_) out_ += "    float3 xfGradV = float3(0.0, 0.0, 0.0);\n";
        out_ += "\n";
    }

    // Entry point whose in/out parameter sets match the body's writes exactly
    // (collected in Generate pass 1): PS consumes iPos + interpolator inputs
    // and writes the oC*/oDepth exports it uses; VS consumes the resolved
    // vertex inputs and writes oPos plus the interpolator outputs it exports.
    void AppendSignature(bool ps) {
        out_ += "void main(\n";
        if (ps) {
            out_ += "    in float4 iPos : SV_Position,\n";
            if (ParamGenActive()) out_ += "    in bool iFront : SV_IsFrontFace,\n";
            std::unordered_set<std::string> emittedInterpolators;
            auto appendInterpolator = [&](const VertexInput& interp) {
                const uint32_t usage = interp.usage >= 14 ? 0 : interp.usage;
                const std::string semantic =
                    std::string(VertexUsageSemanticName(usage)) +
                    std::to_string(interp.usageIndex);
                out_ += "    in float4 i" + std::string(kUsageVariables[usage]) +
                        std::to_string(interp.usageIndex) + " : " + semantic +
                        ",\n";
                emittedInterpolators.insert(semantic);
            };

            if (pairing_ != nullptr && pairing_->vsKnown) {
                for (uint32_t vd = 0; vd < 16; ++vd) {
                    if ((pairing_->vsWritesInterpolators & (1u << vd)) == 0)
                        continue;
                    const std::string semantic =
                        "TEXCOORD" + std::to_string(vd);
                    if (emittedInterpolators.count(semantic) != 0) continue;

                    auto interp = std::find_if(
                        prog_.interpolators.begin(), prog_.interpolators.end(),
                        [&](const VertexInput& candidate) {
                            const uint32_t usage =
                                candidate.usage >= 14 ? 0 : candidate.usage;
                            return std::string(VertexUsageSemanticName(usage)) +
                                       std::to_string(candidate.usageIndex) ==
                                   semantic;
                        });
                    if (interp != prog_.interpolators.end()) {
                        appendInterpolator(*interp);
                    } else {
                        out_ += "    in float4 iUnused" + semantic + " : " +
                                semantic + ",\n";
                        emittedInterpolators.insert(semantic);
                    }
                }
            }

            for (const auto& interp : prog_.interpolators) {
                if (interp.reg >= kRegisterCount || !referencedRegisters_[interp.reg])
                    continue;
                if (pairing_ != nullptr && pairing_->vsKnown) {
                    const uint32_t usage =
                        interp.usage >= 14 ? 0 : interp.usage;
                    const std::string semantic =
                        std::string(VertexUsageSemanticName(usage)) +
                        std::to_string(interp.usageIndex);
                    if (emittedInterpolators.count(semantic) != 0) continue;
                }
                appendInterpolator(interp);
            }
            if (psExportRegs_.count(0)) out_ += "    out float4 oC0 : SV_Target0,\n";
            if (psExportRegs_.count(1)) out_ += "    out float4 oC1 : SV_Target1,\n";
            if (psExportRegs_.count(2)) out_ += "    out float4 oC2 : SV_Target2,\n";
            if (psExportRegs_.count(3)) out_ += "    out float4 oC3 : SV_Target3,\n";
            if (psExportRegs_.count(61)) out_ += "    out float oDepth : SV_Depth,\n";
        } else {
            if (RawVbTranslate()) out_ += "#if defined(XE_RAWVB)\n    in uint xVid : SV_VertexID,\n#endif\n";
            for (const auto& in : vsInputs_) {
                const uint32_t usage = in.usage >= 14 ? 0 : in.usage;
                out_ += "    in float4 i" + std::string(kUsageVariables[usage]) +
                        std::to_string(in.usageIndex) + " : " +
                        VertexUsageSemanticName(usage) +
                        std::to_string(in.usageIndex) + ",\n";
            }
            out_ += "    out float4 oPos : SV_Position,\n";
            for (uint32_t vd : vsExportDests_) {
                if (vd == 62 || vd >= 16) continue;
                out_ += "    out float4 oTexCoord" + std::to_string(vd) +
                        " : TEXCOORD" + std::to_string(vd) + ",\n";
            }
            for (uint32_t i = 0; i < UcpCount(); ++i)
                out_ += "    out float oClip" + std::to_string(i) +
                        (extras_.ucpCullOnly ? " : SV_CullDistance" : " : SV_ClipDistance") +
                        std::to_string(i) + ",\n";
        }
        // Drop the trailing ",\n" so the last parameter has no dangling comma.
        if (out_.size() >= 2 && out_.compare(out_.size() - 2, 2, ",\n") == 0)
            out_.resize(out_.size() - 2);
        out_ += "\n)\n{\n";
    }

    // ---- body -------------------------------------------------------------

    void AppendBody(bool ps) {
        if (!ps) {
            // Guarantee SV_Position is fully written on every code path. The
            // guest's export write mask may omit components (notably w), and
            // DXC rejects a VS whose SV_Position output is not definitely
            // assigned on all paths ("Not all elements of output SV_Position
            // were written", g920_err.log). Unwritten components default to the
            // hardware's unwritten-GPR value for xyz (0.0) and to 1.0 for w,
            // matching the measured guest convention (r1.w = 1.0f before the
            // position export, shader_translator.cpp:1501). Components the guest
            // DOES write override these defaults.
            out_ += "    oPos = float4(0.0, 0.0, 0.0, 1.0);\n";
        }
        for (const auto& ir : prog_.instructions) {
            if (ir.decoded.unknown) {
                ++unsupported_;
                out_ += "    // unsupported instruction decode\n";
                continue;
            }
            if (ir.isFetch) {
                EmitFetch(ir);
            } else {
                EmitAlu(ir.decoded, ps);
            }
        }
    }

    void AppendEpilogue(bool ps) {
        if (ps) {
            // Nothing to fill: PS exports are written inline and
            // AppendSignature only declares an oC*/oDepth target that the
            // body actually wrote.
            out_ += "    // exports emitted inline\n";
            // Alpha test (Xenos RB_COLORCONTROL, xenos.h CompareFunction: 0 never, 1 <, 2 ==, 3 <=,
            // 4 >, 5 !=, 6 >=, 7 always). The consumer prepends the defines only when the guest
            // enables the test, so a disabled draw compiles exactly the old shader.
            if (psExportRegs_.count(0)) {
                out_ += "#if defined(XE_ALPHA_FUNC)\n"
                        "    { const float xa = oC0.a; bool xp = true;\n"
                        "      if (XE_ALPHA_FUNC == 0) xp = false;\n"
                        "      else if (XE_ALPHA_FUNC == 1) xp = xa <  XE_ALPHA_REF;\n"
                        "      else if (XE_ALPHA_FUNC == 2) xp = xa == XE_ALPHA_REF;\n"
                        "      else if (XE_ALPHA_FUNC == 3) xp = xa <= XE_ALPHA_REF;\n"
                        "      else if (XE_ALPHA_FUNC == 4) xp = xa >  XE_ALPHA_REF;\n"
                        "      else if (XE_ALPHA_FUNC == 5) xp = xa != XE_ALPHA_REF;\n"
                        "      else if (XE_ALPHA_FUNC == 6) xp = xa >= XE_ALPHA_REF;\n"
                        "      if (!xp) discard; }\n"
                        "#endif\n";
                // Alpha-to-mask (1x MSAA 2x2 screen-space dither per Xenia dxbc_shader_translator_om.cc:1881-1947
                // + RenanQueiroz rexglue-alpha-to-mask-tolerance.patch 0.001953125f tolerance).
                // Must run on unbiased oC0.a before MIN/MAX premultiply.
                out_ += "#if defined(XE_ALPHA_TO_MASK)\n"
                        "    { const uint xam_idx = ((uint(iPos.x) & 1u) << 1) | (uint(iPos.y) & 1u);\n"
                        "      const float xam_off = float((XE_ALPHA_TO_MASK_OFFSETS >> (xam_idx * 2u)) & 3u);\n"
                        "      if (!(oC0.a >= (1.0f - 0.001953125f - xam_off * 0.25f))) discard; }\n"
                        "#endif\n";
            }
            // Xenia applies SrcAlpha factor in MIN/MAX blend equations; D3D12 ignores blend factors
            // for MIN/MAX, so premultiply every exported colour RT before export (rexglue-minmax-blend-factor.patch).
            for (uint32_t xr = 0; xr < 4; ++xr) {
                if (!psExportRegs_.count(xr)) continue;
                const std::string oc = "oC" + std::to_string(xr);
                out_ += "#if defined(XE_MINMAX_PREMUL_RGB) || defined(XE_MINMAX_PREMUL_ALPHA)\n"
                        "    { const float xm_a = saturate(" + oc + ".a);\n"
                        "#if defined(XE_MINMAX_PREMUL_RGB)\n"
                        "      " + oc + ".rgb *= xm_a;\n"
                        "#endif\n"
                        "#if defined(XE_MINMAX_PREMUL_ALPHA)\n"
                        "      " + oc + ".a *= xm_a;\n"
                        "#endif\n"
                        "    }\n"
                        "#endif\n";
            }
            // MCLA_PS_NANSCRUB=1: non-finite export lanes -> 0; =2: paint the whole pixel magenta (locate NaN/Inf
            // producers). Must run before the MD-26 clamp, which turns +Inf into 31.875 (white) and NaN into 0.
            {
                static const int s_nanScrub = [] { const char* e = std::getenv("MCLA_PS_NANSCRUB"); return (e && *e) ? std::atoi(e) : 0; }();
                for (uint32_t xr = 0; s_nanScrub > 0 && xr < 4; ++xr) {
                    if (!psExportRegs_.count(xr)) continue;
                    const std::string oc = "oC" + std::to_string(xr);
                    out_ += s_nanScrub == 2
                        ? "    if (!all(isfinite(" + oc + "))) " + oc + " = float4(1.0, 0.0, 1.0, 1.0);\n"
                        : "    " + oc + " = select(isfinite(" + oc + "), " + oc + ", float4(0.0, 0.0, 0.0, 0.0));\n";
                }
            }
            // MD-26: k_2_10_10_10_FLOAT targets are R16G16B16A16_FLOAT on the host; the guest format
            // saturates to rgb [0, 31.875], a [0, 1] (render_target_cache.cc:242-245) -- on the value AFTER the
            // RT exp bias, which this host never applies, so the rgb ceiling is 31.875 * 2^-rtExpBias.
            for (uint32_t xr = 0; xr < 4; ++xr)
                if (psExportRegs_.count(xr) && (extras_.rt7e3Mask & (1u << xr)) && !EnvOff("MCLA_NO_RT7E3_CLAMP")) {
                    const std::string hi = FloatLitDecimal(static_cast<float>(std::ldexp(31.875, -extras_.rtExpBias[xr])));
                    // RT-ALPHA-BIAS: the host keeps rgb in unbiased units (consistent through blending and resolves), but
                    // alpha feeds the blend factors, so it must carry the bias as on hardware (Xenia multiplies all four
                    // lanes, dxbc_shader_translator_om.cc:1676-1683). rmptfx particles pre-divide alpha by
                    // gInvColorExpBias (VS c27.x = 1/16) for exp_bias=+4 targets. Off: MCLA_NO_RT_ALPHA_BIAS=1.
                    if (extras_.rtExpBias[xr] != 0 && !EnvOff("MCLA_NO_RT_ALPHA_BIAS"))
                        out_ += "    oC" + std::to_string(xr) + ".a *= " +
                                FloatLitDecimal(static_cast<float>(std::ldexp(1.0, extras_.rtExpBias[xr]))) + ";\n";
                    out_ += "    oC" + std::to_string(xr) + " = clamp(oC" + std::to_string(xr) +
                            ", float4(0.0, 0.0, 0.0, 0.0), float4(" + hi + ", " + hi + ", " + hi + ", 1.0));\n";
                }
            // render_thread.cpp prepends `#define XE_HASHVIZ_RGB float3(r,g,b)`; every colour export becomes flat opaque.
            for (uint32_t xr = 0; xr < 4; ++xr)
                if (psExportRegs_.count(xr))
                    out_ += "#if defined(XE_HASHVIZ_RGB)\n    oC" + std::to_string(xr) + " = float4(XE_HASHVIZ_RGB, 1.0);\n#endif\n"
                            // Internal-value hashviz: paint the indexed constant register a0
                            // so a per-pixel/stepped a0 shows as horizontal bands.
                            "#if defined(XE_HASHVIZ_A0)\n    oC" + std::to_string(xr) +
                                " = float4(frac(a0 * 0.0625), frac(a0 * 0.25), "
                                "clamp((a0 + 256.0) / 512.0, 0.0, 1.0), 1.0);\n#endif\n"
                            "#if defined(XE_ALPHA1)\n    oC" + std::to_string(xr) + ".a = 1.0;\n#endif\n";
            if (tl_hvInterp) {
                // Paint oC0 with the interpolators feeding r2.x (the a0 source):
                // R/G = iTexCoord0.xy (world pos), B = frac(iTexCoord1.x*4) (N.L).
                for (uint32_t xr = 0; xr < 4; ++xr)
                    if (psExportRegs_.count(xr))
                        out_ += "    oC" + std::to_string(xr) +
                                " = float4(iTexCoord0.x, iTexCoord0.y, frac(iTexCoord1.x*4.0), 1.0);\n";
            } else if (tl_hvUV) {
                // R/G = frac of the selected tex15 sample UV; B = floor/16 coarse; A = tap count.
                for (uint32_t xr = 0; xr < 4; ++xr)
                    if (psExportRegs_.count(xr))
                        out_ += "    oC" + std::to_string(xr) +
                                " = float4(frac(hvUV.x), frac(hvUV.y), "
                                "clamp(floor(hvUV.x)/16.0 + 0.5, 0.0, 1.0), clamp(hvUVn/4.0, 0.0, 1.0));\n";
            }
            // SV_Depth: the guest writes its own z, but the host DSV holds the mirrored
            // value (1 - z) whenever DepthCompareMirrored() is on. Mirror exactly once,
            // after every export path, so conditional writes and the constant fills above
            // get the same treatment. Measured live in two programs
            // (pair_7AD220C78C2D21E8_ps, pair_FB8A44C4F80F9694_ps), which wrote raw z.
            if (psExportRegs_.count(61) && native::DepthCompareMirrored())
                out_ += "    oDepth = 1.0f - oDepth;\n";
            return;
        }
        // MD-21 (Xenia dxbc_shader_translator.cc:1040-1053): dot(guest position, plane) per packed plane.
        for (uint32_t i = 0; i < UcpCount(); ++i)
            out_ += "    oClip" + std::to_string(i) + " = dot(oPos, xUcp[" + std::to_string(i) + "]);\n";
        if (!applyClipDisableRescale_) {
            clipRescaleBaked_ = false;
            oPosShape_ = "clipSpacePosition";
            out_ += "    // clip_disable rescale not requested (VS already outputs clip space)\n";
            out_ += "#if defined(XE_YFLIP)\n    oPos.y = -oPos.y;\n#endif\n";
            return;
        }
        // ---- VS: guest-pixel -> host clip space (see ClipDisableRescale) -----
        //
        // This REPLACES the previous `oPos.xy += float2(0.5, 0.5) * oPos.w`. That
        // line was the reference's offset_add term applied to clip space as if
        // clip space were guest pixels: with a viewport of {0,0,W,H,0,1} a clip
        // offset of 0.5 becomes W/2 = 640 pixels of shift, not half a pixel.
        //
        // The transform is applied to XYZ only and the offset is scaled by W, so
        // it happens BEFORE the perspective divide -- the same place the reference
        // puts it (spirv_shader_translator.cc:1612-1624). Nothing here reads,
        // clamps, defaults or sanitises oPos: a NaN or Inf coming out of the guest
        // program stays NaN or Inf on purpose, because hiding it would make the
        // rasterizer's silence look like a colour bug instead of a guest fact.
        const ClipDisableRescale r = (clipExtW_ && clipExtH_)
            ? MakeClipDisableRescale(clipExtW_, clipExtH_) : PublishedClipRescale();
        if (!r.valid) {
            // No extent has been published yet. Emit the rescale as the IDENTITY
            // rather than as a wrong constant: identity is the only choice that
            // does not assert a number nobody measured.
            //
            // MEASURED 2026-10-04, build/w707.log, and it is NOT the startup-order
            // guard the previous revision of this comment claimed. That comment
            // said the backend calls SetClipDisableViewportExtent "before any
            // real draw"; it does not. The single call site is
            // d3d12_backend.cpp:3157, INSIDE
            // D3D12Backend::DrawDynamicMeshWithPipeline (which starts at :2811),
            // and this file's translation for a draw happens earlier than that,
            // in the render thread at src/render_thread.cpp:3471. So for the
            // FIRST draw of a process the two are ordered the wrong way round:
            //   06:57:37.262  draw #1's programs translated -- w707.log:7505,
            //                  SHADER-UNWRITTEN-REG-READ #1, emitted by
            //                  LogUnwrittenRegisterReads at :586, the line
            //                  immediately above this one, so it timestamps
            //                  this call. valid == false -> IDENTITY, permanently.
            //   06:57:37.295  SetClipDisableViewportExtent(1280,720) publishes
            //                  (w707.log:8031 is the next row after it).
            //   06:57:37.298  draw #2's programs translated (w707.log:8074) --
            //                  valid == true -> the real rescale.
            // Verified offline on both real programs: given the same published
            // extent the two emit BYTE-IDENTICAL epilogues, and given the same
            // unpublished state they emit byte-identical identity epilogues
            // (%TEMP%\opencode\clipgate\run1.txt, cases A and B). The two draws
            // are indistinguishable to this function; only the publication order
            // differs. A corrected ordering therefore belongs to the call site,
            // not here -- see the handover note beside
            // TranslatedShader::clipRescaleBaked.
            //
            // One row per process, not one per translation: the condition is a
            // property of the ORDER, so every row after the first would be the
            // same fact. Names no tracked marker on purpose -- a tracked marker
            // named inside another marker's prose becomes uncountable, because
            // tools/soak_census.py counts matching LINES.
            static std::atomic<bool> s_unbakedLogged{false};
            bool expected = false;
            if (s_unbakedLogged.compare_exchange_strong(expected, true)) {
                MCLA_LOG_WARN(
                    "VS-RESCALE-UNBAKED: this vertex program's clip-space "
                    "rescale was emitted as the IDENTITY because no render-target "
                    "extent had been published when it was translated. The "
                    "program is cached under a key that does not contain the "
                    "extent, so this translation stays geometry-less for the life "
                    "of the process. Not fabricated and not a default: the extent "
                    "is not knowable at this point, and the identity asserts "
                    "nothing. The fix is to publish the extent BEFORE translating "
                    "-- the one call site is inside the first draw, which runs "
                    "after this translation. TranslatedShader::clipRescaleBaked "
                    "is false for this program.");
            }
            clipRescaleBaked_ = false;
            out_ += "    // clip_disable rescale: NO EXTENT PUBLISHED YET -- identity\n";
            out_ += "    // (see SetClipDisableViewportExtent)\n";
            out_ += "#if defined(XE_YFLIP)\n    oPos.y = -oPos.y;\n#endif\n";
            return;
        }
        out_ += "    // guest pixels -> host clip space; draw_util.cc:362-386 + :549-552\n";
        out_ += fmt::format(
            "    oPos.xyz = oPos.xyz * float3({}, {}, {})\n", FloatLit(r.scaleX),
            FloatLit(r.scaleY), FloatLit(r.scaleZ));
        out_ += fmt::format(
            "               + float3({}, {}, {}) * oPos.w;\n", FloatLit(r.offsetX),
            FloatLit(r.offsetY), FloatLit(r.offsetZ));
        out_ += "#if defined(XE_YFLIP)\n    oPos.y = -oPos.y;\n#endif\n";

        // What the geometry stage will read out of this program, as TEXT. F-609:
        // the TEXT is reported, never a value derived from it -- D3D12 has no
        // shader-output readback, so anything numeric here would be this file's
        // own arithmetic wearing the pipeline's clothes.
        //
        // WHAT oPos IS, measured from the emitted HLSL of a real program and not
        // from the field name. Draw #1's vertex microcode (build/w710.log:6599,
        // CP-SHADER-BODY #3, 27 dwords; the container this file emits for it
        // reproduces DRAW-CP-PIPE #1's vsHash=22BB5C3AE64A3D71 at w710.log:6735)
        // emits exactly:
        //
        //     r1.xyz = iPosition0.xyz;
        //     r1.w   = 1.0f;
        //     oPos.xyzw = max(r1.xyzw, r1.xyzw);
        //     oPos.xyz = oPos.xyz * float3(0.0015625, -0.0027777778, 1)
        //                + float3(-0.99921876, 0.99861115, 0) * oPos.w;
        //
        // So oPos is a CLIP-SPACE position (host/NDC, post-rescale), XYZ scaled
        // and W-multiplied exactly as the reference's epilogue does
        // (spirv_shader_translator.cc:1612-1624, dxbc_shader_translator.cc:1058-1068),
        // with W left as the guest wrote it. It is NOT a rectangle tuple, NOT a
        // centre+extent, and NOT guest pixels by the time the GS reads it.
        //
        // The geometry stage reads it as THREE CORNERS of a rectangle and mirrors
        // a fourth, not as a rect: pipeline_cache.cc:2550-2694 picks the longest
        // edge, then emits v0,v1,v2 and v3 = -v0+v1+v2 (:2669-2674), passing each
        // input position through unchanged (:2636-2637). W is used by the
        // rectangle case only through that same linear combination; the
        // point-sprite case is the one that scales a radius by W (:2502-2503).
        // That is why the rescale belongs in the VS epilogue rather than at the
        // GS output: an affine map on (x,y) commutes with the mirroring, so a
        // rectangle stays a rectangle either way, and the reference puts it in the
        // epilogue in both of its own backends.
        oPosShape_ = "clipSpacePosition";
        // The TEXT of every emitted line that assigns oPos, quoted out of out_
        // after the fact rather than re-formatted, so this can never claim a
        // different expression from the one the compiler will see. Leading
        // whitespace and the trailing newline are dropped; the text itself is
        // untouched. A pixel program's AppendEpilogue returns before here, so
        // this stays empty for it and no row claims a position it never wrote.
        // Collect the STATEMENTS that assign oPos. Split on `;`, not on newlines: the
        // epilogue emits its rescale as two physical lines, so a per-line scan
        // reported it twice (the harness printed the same line twice before this)
        // and a reader could not tell one write from two.
        //
        // Each piece is a verbatim substring of out_ with only the leading
        // indentation dropped; nothing is re-formatted, renamed or re-derived, so
        // the row cannot claim an expression the compiler will not see. An
        // assignment spanning two lines has its interior newline collapsed to one
        // space, which is whitespace and changes nothing about the code.
        oPosWriteExpression_.clear();
        {
            bool open = false;  // inside an assignment whose `;` has not arrived
            size_t pos = 0;
            bool armed = false;  // seen `oPos` as an assignment TARGET at least once
            while (pos < out_.size()) {
                const size_t eol = out_.find('\n', pos);
                const size_t stop = (eol == std::string::npos ? out_.size() : eol);
                std::string line = out_.substr(pos, stop - pos);
                const bool isDecl =
                        line.find("out float4 oPos : SV_Position") !=
                        std::string::npos;
                size_t b = line.find_first_not_of(" \t");
                if (b == std::string::npos) b = 0;
                line = line.substr(b);
                // The DECLARATION arms the collector: `out float4 oPos : ...` is
                // the only place oPos comes into scope, so until that line is
                // passed nothing else in the body can be a write to it. Without
                // the arm, every earlier line containing an `=` (the register
                // declarations, and the fetches that assign r0/r1) was collected
                // too -- the harness printed 11 statements before this, including
                // `float4 r0 = 0.0;`, which says nothing about the position.
                const bool writesOPos =
                        line.rfind("oPos", 0) == 0 &&   // statement starts with it
                        line.find('=') != std::string::npos;
                if (isDecl) {
                    armed = true;
                    open = false;
                } else if (armed && !line.empty()) {
                    if (open) {
                        oPosWriteExpression_ += " " + line;
                    } else if (writesOPos) {
                        if (!oPosWriteExpression_.empty())
                            oPosWriteExpression_ += " | ";
                        oPosWriteExpression_ += line;
                        open = true;
                    } else {
                        open = false;
                    }
                    if (open && line.find(';') != std::string::npos) open = false;
                }
                pos = stop + 1;
            }
        }
        // Normalise interior whitespace runs so a wrapped statement reads as one
        // line. Whitespace only; no token is touched.
        {
            std::string flat;
            flat.reserve(oPosWriteExpression_.size());
            bool sp = false;
            for (char ch : oPosWriteExpression_) {
                if (ch == ' ' || ch == '\t' || ch == '\r') {
                    sp = true;
                    continue;
                }
                if (sp && !flat.empty()) flat += ' ';
                sp = false;
                flat += ch;
            }
            oPosWriteExpression_ = flat;
        }
    }

    // ---- ALU lowering -----------------------------------------------------

    void EmitAlu(const DecodedInstruction& instr, bool ps) {
        const bool predicated = instr.isPredicated;

        const bool openIf = predicated;
        if (openIf) {
            out_ += std::string("    if (") + (instr.predicateCondition ? "" : "!") +
                    "p0)\n    {\n";
        }

        // Kill vector ops -> clip(), UNDER the instruction's predicate and over all four components (Xenia
        // dxbc_shader_translator_alu.cc:509-574; docs/XENIA_PORT_SHADER.md P0).
        if (instr.vectorOpcode >= AluVectorOpcode::KillEq &&
            instr.vectorOpcode <= AluVectorOpcode::KillNe) {
            const char* rel = nullptr;
            switch (instr.vectorOpcode) {
            case AluVectorOpcode::KillEq: rel = "=="; break;
            case AluVectorOpcode::KillGt: rel = ">"; break;
            case AluVectorOpcode::KillGe: rel = ">="; break;
            case AluVectorOpcode::KillNe: rel = "!="; break;
            default: break;
            }
            if (rel) {
                out_ += "    clip(any(" + EmitOperand(instr, OperandKind::Vector0, 0b1111) +
                        " " + rel + " " + EmitOperand(instr, OperandKind::Vector1, 0b1111) +
                        ") ? -1.0 : 1.0);\n";
            }
        }


        // Export register: writes go to the export target when exportData.
        std::string exportReg;
        if (instr.exportData) {
            if (ps) {
                exportReg = ExportRegisterName(instr, instr.vectorDest);
            } else {
                // Xenos VS export registers: 62 = position, 0-15 =
                // VSInterpolator0..15, 63 = point-size/edge-flag/kill vertex.
                // The 63 write has no color output and is suppressed below.
                if (instr.vectorDest == 62) {
                    exportReg = "oPos";
                } else if (instr.vectorDest < 16) {
                    exportReg = "oTexCoord" + std::to_string(instr.vectorDest);
                }
            }
        }

        // Setp (predicate push).
        if (instr.vectorOpcode >= AluVectorOpcode::SetpEqPush &&
            instr.vectorOpcode <= AluVectorOpcode::SetpGePush) {
            const char* rel = nullptr;
            switch (instr.vectorOpcode) {
            case AluVectorOpcode::SetpEqPush: rel = "=="; break;
            case AluVectorOpcode::SetpNePush: rel = "!="; break;
            case AluVectorOpcode::SetpGtPush: rel = ">"; break;
            case AluVectorOpcode::SetpGePush: rel = ">="; break;
            default: break;
            }
            // Xenia ucode.h:1764-1768: p0 = src0.w == 0 && cmp(src1.w).
            out_ += "    p0 = ((" + EmitOperand(instr, OperandKind::Vector0, 0b1000) +
                    " == 0.0) && " +
                    PushCmp(instr, EmitOperand(instr, OperandKind::Vector1, 0b1000)) + ");\n";
        }

        // MaxA -> address register write.
        // Reference: dxbc_shader_translator_alu.cc:612-624 -- a0 = round_ni(
        // operand0.SelectFromSwizzled(3) + 0.5), clamped to [-256, 255]. The
        // component comes from operand 0 at slot 3, so emit that ONE component
        // (mask 0b1000) instead of appending ".w" to an already-swizzled
        // operand (which produced `rN.xyzw.w`, HLSL DXC rejects).
        if (instr.vectorOpcode == AluVectorOpcode::MaxA) {
            out_ += "    a0 = (int)clamp(floor(" +
                    EmitOperand(instr, OperandKind::Vector0, 0b1000) +
                    " + 0.5), -256.0, 255.0);\n";
        }

        // Vector write.
        uint32_t vectorWriteMask = instr.vectorWriteMask;
        if (instr.exportData) vectorWriteMask &= ~instr.scalarWriteMask;

        // VS export 63 is point-size/edge-flag/kill-vertex state with no color
        // output; drop the write rather than emitting an undeclared r63.
        const bool suppressVsExport =
                instr.exportData && !ps && instr.vectorDest == 63;

        if (vectorWriteMask != 0 && !suppressVsExport) {
            // SV_Depth is a scalar in HLSL (must be float, not float4) and the
            // guest depth register (61) carries only its X lane: the reference
            // writes X to scalar oDepth with used_write_mask == 0b0001
            // (.research/xenia/src/xenia/gpu/dxbc_shader_translator.cc:1591-1594).
            // Declaring float4 oDepth made DXC reject the shader with
            // "SV_Depth must be float" (measured: mcla_pe.bin container at
            // offset 8206072, translator_probe (d)). So the depth export is a
            // plain float, written from the computed vector's X lane only.
            const bool isDepthExport = (exportReg == "oDepth");
            std::string dst;
            if (!exportReg.empty()) {
                dst = exportReg;
                NoteExport(ps, instr.vectorDest);
            } else {
                dst = RegName(instr.vectorDest);
                NoteRegister(instr.vectorDest);
            }
            if (!isDepthExport) {
                dst += '.';
                for (uint32_t i = 0; i < 4; i++) {
                    if ((vectorWriteMask >> i) & 0x1) dst += kSwizzle[i];
                }
            }

            out_ += "    " + dst + " = ";
            if (isDepthExport) out_ += "(";
            if (instr.vectorSaturate) out_ += "saturate(";

            switch (instr.vectorOpcode) {
            case AluVectorOpcode::Add:
                out_ += EmitOperand(instr, OperandKind::Vector0) + " + " +
                        EmitOperand(instr, OperandKind::Vector1);
                break;
            case AluVectorOpcode::Mul:
                out_ += "xm(" + EmitOperand(instr, OperandKind::Vector0) + ", " +
                        EmitOperand(instr, OperandKind::Vector1) + ")";
                break;
            case AluVectorOpcode::Max:
            case AluVectorOpcode::MaxA:
                out_ += "max(" + EmitOperand(instr, OperandKind::Vector0) + ", " +
                        EmitOperand(instr, OperandKind::Vector1) + ")";
                break;
            case AluVectorOpcode::Min:
                out_ += "min(" + EmitOperand(instr, OperandKind::Vector0) + ", " +
                        EmitOperand(instr, OperandKind::Vector1) + ")";
                break;
            case AluVectorOpcode::Seq:
                out_ += EmitOperand(instr, OperandKind::Vector0) + " == " +
                        EmitOperand(instr, OperandKind::Vector1);
                break;
            case AluVectorOpcode::Sgt:
                out_ += EmitOperand(instr, OperandKind::Vector0) + " > " +
                        EmitOperand(instr, OperandKind::Vector1);
                break;
            case AluVectorOpcode::Sge:
                out_ += EmitOperand(instr, OperandKind::Vector0) + " >= " +
                        EmitOperand(instr, OperandKind::Vector1);
                break;
            case AluVectorOpcode::Sne:
                out_ += EmitOperand(instr, OperandKind::Vector0) + " != " +
                        EmitOperand(instr, OperandKind::Vector1);
                break;
            case AluVectorOpcode::Frc:
                out_ += "frac(" + EmitOperand(instr, OperandKind::Vector0) + ")";
                break;
            case AluVectorOpcode::Trunc:
                out_ += "trunc(" + EmitOperand(instr, OperandKind::Vector0) + ")";
                break;
            case AluVectorOpcode::Floor:
                out_ += "floor(" + EmitOperand(instr, OperandKind::Vector0) + ")";
                break;
            case AluVectorOpcode::Mad:
                out_ += "xm(" + EmitOperand(instr, OperandKind::Vector0) + ", " +
                        EmitOperand(instr, OperandKind::Vector1) + ") + " +
                        EmitOperand(instr, OperandKind::Vector2);
                break;
            case AluVectorOpcode::CndEq:
                out_ += "select(" + EmitOperand(instr, OperandKind::Vector0) +
                        " == 0.0, " + EmitOperand(instr, OperandKind::Vector1) + ", " +
                        EmitOperand(instr, OperandKind::Vector2) + ")";
                break;
            case AluVectorOpcode::CndGe:
                out_ += "select(" + EmitOperand(instr, OperandKind::Vector0) +
                        " >= 0.0, " + EmitOperand(instr, OperandKind::Vector1) + ", " +
                        EmitOperand(instr, OperandKind::Vector2) + ")";
                break;
            case AluVectorOpcode::CndGt:
                out_ += "select(" + EmitOperand(instr, OperandKind::Vector0) +
                        " > 0.0, " + EmitOperand(instr, OperandKind::Vector1) + ", " +
                        EmitOperand(instr, OperandKind::Vector2) + ")";
                break;
            case AluVectorOpcode::Dp4:
            case AluVectorOpcode::Dp3:
                out_ += "xdot(" + EmitOperand(instr, OperandKind::Vector0) + ", " +
                        EmitOperand(instr, OperandKind::Vector1) + ")";
                break;
            case AluVectorOpcode::Dp2Add:
                out_ += "xdot(" + EmitOperand(instr, OperandKind::Vector0) + ", " +
                        EmitOperand(instr, OperandKind::Vector1) + ") + " +
                        EmitOperand(instr, OperandKind::Vector2);
                break;
            case AluVectorOpcode::Cube: {
                // All four source components are needed (ucode.h:1761-1763).
                out_ += "xcube(" + EmitOperand(instr, OperandKind::Vector0, 0b1111) + ")";
                break;
            }
            case AluVectorOpcode::Max4:
                // Reference: dxbc_shader_translator_alu.cc:355 -- max over the
                // components of operand 0. Each component is its own
                // single-slot operand (mask bit i = slot i); the old text
                // appended ".x/.y/.z/.w" to the full-swizzled operand, giving
                // `rN.xyzw.x` -- not tokenizable HLSL.
                out_ += "max(max(" + EmitOperand(instr, OperandKind::Vector0, 0b0001) + ", " +
                        EmitOperand(instr, OperandKind::Vector0, 0b0010) + "), max(" +
                        EmitOperand(instr, OperandKind::Vector0, 0b0100) + ", " +
                        EmitOperand(instr, OperandKind::Vector0, 0b1000) + "))";
                break;
            case AluVectorOpcode::SetpEqPush:
            case AluVectorOpcode::SetpNePush:
            case AluVectorOpcode::SetpGtPush:
            case AluVectorOpcode::SetpGePush: {
                // Xenia :390-505: (src0.x == 0 && cmp(src1.x)) ? 0 : src0.x + 1 (broadcast).
                const std::string s0x = EmitOperand(instr, OperandKind::Vector0, 0b0001);
                out_ += "(((" + s0x + " == 0.0) && " +
                        PushCmp(instr, EmitOperand(instr, OperandKind::Vector1, 0b0001)) +
                        ") ? 0.0 : (" + s0x + " + 1.0))";
                break;
            }
            case AluVectorOpcode::KillEq:
            case AluVectorOpcode::KillGt:
            case AluVectorOpcode::KillGe:
            case AluVectorOpcode::KillNe:
                out_ += "0.0";
                break;
            case AluVectorOpcode::Dst:
                // Reference: dxbc_shader_translator_alu.cc:576-604 --
                // result = (1.0,
                //           op0.SelectFromSwizzled(1) * op1.SelectFromSwizzled(1),
                //           op0.SelectFromSwizzled(2),
                //           op1.SelectFromSwizzled(2)).
                // The old emission was wrong twice over: it appended ".y/.z/.w"
                // to already-swizzled operands (invalid HLSL) AND read the Y
                // product from op1 alone (dropping the op0 factor) and the W
                // from op1 slot 3 instead of slot 2.
                out_ += "float4(1.0, " + EmitOperand(instr, OperandKind::Vector0, 0b0010) +
                        " * " + EmitOperand(instr, OperandKind::Vector1, 0b0010) + ", " +
                        EmitOperand(instr, OperandKind::Vector0, 0b0100) + ", " +
                        EmitOperand(instr, OperandKind::Vector1, 0b0100) + ")";
                break;
            default:
                ++unsupported_;
                out_ += "0.0";
                break;
            }

            if (instr.vectorSaturate) out_ += ')';
            if (isDepthExport) out_ += ").x";
            out_ += ";\n";
        }

        // Scalar clause (skip RetainPrev: it only keeps ps alive).
        if (instr.scalarOpcode != AluScalarOpcode::RetainPrev) {
            EmitScalarClause(instr, ps, exportReg, suppressVsExport);
        }

        // Export constant 0/1 fill (Xenia ucode.h:2011-2036):
        //   - oneMask  (vwm & swm): overlapping vector+scalar channels write 1.0.
        //   - zeroMask (scalarDestRelative ? ~(vwm | swm) & 0xF : 0): only when
        //     scalar_dest_rel is set on an export do unwritten channels get 0.0;
        //     normal narrow exports leave earlier oC/oTexCoord channels untouched.
        if (instr.exportData && !exportReg.empty()) {
            // MEASURED 2026-10-01: NoteExport is ALSO called here, not only in
            // the vectorWriteMask != 0 arm above. Those are different sets. On
            // CP-SHADER-BODY #1 (VA 0x82061B28, 24 dwords) raw word0
            // C800C000 at addr=4 gives vectorWriteMask=0 and scalarWriteMask=0,
            // so the arm above is skipped entirely; raw word0 C888C03E at addr=5
            // gives 8/8, and `vwm &= ~swm` (line above) makes the arm's test 0
            // again. So no export was ever noted, `vsExportDests_` stayed empty,
            // AppendSignature declared no oTexCoord0 -- and the fill below still
            // emitted `oTexCoord0.x = 0.0;`. The generated HLSL referenced an
            // undeclared identifier, i.e. a shader that cannot compile, produced
            // with ret=true and a non-empty hlsl, so every gate that tests
            // "translated && hlsl non-empty" passed it. This block is the only
            // place the body can write an export target the signature did not
            // declare, so it is the only place that must note one.
            NoteExport(ps, instr.vectorDest);
            // FIX (measured): per-export zero fill clobbers channels owned by
            // other exports unless scalarDestRelative requests constant-0 fill
            // (GetConstant0WriteMask). Overlapping vector+scalar channels get 1.0;
            // otherwise omitted channels stay untouched (oC0 is zero-initialized
            // before the body, see Generate).
            const uint32_t oneMask = instr.vectorWriteMask & instr.scalarWriteMask;
            const uint32_t zeroMask = instr.scalarDestRelative
                ? (0xFu & ~(instr.vectorWriteMask | instr.scalarWriteMask))
                : 0u;
            const bool isDepthFill = (exportReg == "oDepth");
            for (uint32_t i = 0; i < 4; i++) {
                uint32_t mask = 1u << i;
                if (isDepthFill && i != 0) continue;
                if (zeroMask & mask) {
                    if (isDepthFill) out_ += "    oDepth = 0.0;\n";
                    else out_ += std::string("    ") + exportReg + "." + kSwizzle[i] + " = 0.0;\n";
                } else if (oneMask & mask) {
                    if (isDepthFill) out_ += "    oDepth = 1.0;\n";
                    else out_ += std::string("    ") + exportReg + "." + kSwizzle[i] + " = 1.0;\n";
                }
            }
        }

        // Scalar Kills -> clip on ps.
        if (instr.scalarOpcode >= AluScalarOpcode::KillsEq &&
            instr.scalarOpcode <= AluScalarOpcode::KillsOne) {
            out_ += "    clip(ps != 0.0 ? -1.0 : 1.0);\n";
        }

        if (openIf) {
            out_ += "    }\n";
        }
    }

    void EmitScalarClause(const DecodedInstruction& instr, bool ps,
                          const std::string& exportReg,
                          bool suppressVsExport) {
        // Setp scalar -> predicate write.
        if (instr.scalarOpcode >= AluScalarOpcode::SetpEq &&
            instr.scalarOpcode <= AluScalarOpcode::SetpRstr) {
            out_ += "    p0 = ";
            switch (instr.scalarOpcode) {
            case AluScalarOpcode::SetpEq:
                out_ += EmitOperand(instr, OperandKind::Scalar0) + " == 0.0";
                break;
            case AluScalarOpcode::SetpNe:
                out_ += EmitOperand(instr, OperandKind::Scalar0) + " != 0.0";
                break;
            case AluScalarOpcode::SetpGt:
                out_ += EmitOperand(instr, OperandKind::Scalar0) + " > 0.0";
                break;
            case AluScalarOpcode::SetpGe:
                out_ += EmitOperand(instr, OperandKind::Scalar0) + " >= 0.0";
                break;
            case AluScalarOpcode::SetpInv:
                out_ += EmitOperand(instr, OperandKind::Scalar0) + " == 1.0";
                break;
            case AluScalarOpcode::SetpPop:
                out_ += EmitOperand(instr, OperandKind::Scalar0) + " - 1.0 <= 0.0";
                break;
            case AluScalarOpcode::SetpClr:
                out_ += "false";
                break;
            case AluScalarOpcode::SetpRstr:
                out_ += EmitOperand(instr, OperandKind::Scalar0) + " == 0.0";
                break;
            default:
                break;
            }
            out_ += ";\n";
        }

        // ps write.
        out_ += "    ps = ";
        if (instr.scalarSaturate) out_ += "saturate(";

        switch (instr.scalarOpcode) {
        case AluScalarOpcode::Adds:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " + " +
                    EmitOperand(instr, OperandKind::Scalar1);
            break;
        case AluScalarOpcode::AddsPrev:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " + ps";
            break;
        case AluScalarOpcode::Muls:
            out_ += "xm(" + EmitOperand(instr, OperandKind::Scalar0) + ", " +
                    EmitOperand(instr, OperandKind::Scalar1) + ")";
            break;
        case AluScalarOpcode::MulsPrev:
            out_ += "xm(" + EmitOperand(instr, OperandKind::Scalar0) + ", ps)";
            break;
        case AluScalarOpcode::MulsPrev2: {
            // Xenia dxbc_shader_translator_alu.cc:705-748 (LIT): -FLT_MAX unless ps != -FLT_MAX, ps finite, src0.b finite and > 0.
            const std::string b1 = EmitOperand(instr, OperandKind::Scalar1);
            out_ += "((ps != -3.402823466e+38 && isfinite(ps) && isfinite(" + b1 + ") && " + b1 + " > 0.0) ? xm(" +
                    EmitOperand(instr, OperandKind::Scalar0) + ", ps) : -3.402823466e+38)";
            break;
        }
        case AluScalarOpcode::Maxs:
        case AluScalarOpcode::MaxAs:
        case AluScalarOpcode::MaxAsf:
            out_ += "max(" + EmitOperand(instr, OperandKind::Scalar0) + ", " +
                    EmitOperand(instr, OperandKind::Scalar1) + ")";
            break;
        case AluScalarOpcode::Mins:
            out_ += "min(" + EmitOperand(instr, OperandKind::Scalar0) + ", " +
                    EmitOperand(instr, OperandKind::Scalar1) + ")";
            break;
        case AluScalarOpcode::Seqs:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " == 0.0";
            break;
        case AluScalarOpcode::Sgts:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " > 0.0";
            break;
        case AluScalarOpcode::Sges:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " >= 0.0";
            break;
        case AluScalarOpcode::Snes:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " != 0.0";
            break;
        case AluScalarOpcode::Frcs:
            out_ += "frac(" + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        case AluScalarOpcode::Truncs:
            out_ += "trunc(" + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        case AluScalarOpcode::Floors:
            out_ += "floor(" + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        case AluScalarOpcode::Exp:
            out_ += "exp2(" + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        case AluScalarOpcode::Logc:
        case AluScalarOpcode::Log:
            out_ += "clamp(log2(" + EmitOperand(instr, OperandKind::Scalar0) +
                    "), -FLT_MAX, FLT_MAX)";
            break;
        case AluScalarOpcode::Rcpf:
            out_ += "xrcpf(" + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        case AluScalarOpcode::Rcp:
            out_ += "(1.0 / " + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        case AluScalarOpcode::Rcpc:
            out_ += "clamp(1.0 / " + EmitOperand(instr, OperandKind::Scalar0) +
                    ", -FLT_MAX, FLT_MAX)";
            break;
        case AluScalarOpcode::Rsqf:
            out_ += "xrsqf(" + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        case AluScalarOpcode::Rsq:
            out_ += "rsqrt(" + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        case AluScalarOpcode::Rsqc:
            out_ += "clamp(rsqrt(" + EmitOperand(instr, OperandKind::Scalar0) +
                    "), -FLT_MAX, FLT_MAX)";
            break;
        case AluScalarOpcode::Subs:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " - " +
                    EmitOperand(instr, OperandKind::Scalar1);
            break;
        case AluScalarOpcode::SubsPrev:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " - ps";
            break;
        case AluScalarOpcode::SetpEq:
        case AluScalarOpcode::SetpNe:
        case AluScalarOpcode::SetpGt:
        case AluScalarOpcode::SetpGe:
            out_ += "p0 ? 0.0 : 1.0";
            break;
        case AluScalarOpcode::SetpInv: {
            // Xenia :925-936: p0 = (a == 1) was set above; ps = p0 ? 0 : (a == 0 ? 1 : a).
            const std::string a = EmitOperand(instr, OperandKind::Scalar0);
            out_ += "(p0 ? 0.0 : (" + a + " == 0.0 ? 1.0 : " + a + "))";
            break;
        }
        case AluScalarOpcode::SetpPop:
            out_ += "p0 ? 0.0 : (" + EmitOperand(instr, OperandKind::Scalar0) + " - 1.0)";
            break;
        case AluScalarOpcode::SetpClr:
            out_ += "FLT_MAX";
            break;
        case AluScalarOpcode::SetpRstr:
            out_ += "p0 ? 0.0 : " + EmitOperand(instr, OperandKind::Scalar0);
            break;
        case AluScalarOpcode::KillsEq:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " == 0.0";
            break;
        case AluScalarOpcode::KillsGt:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " > 0.0";
            break;
        case AluScalarOpcode::KillsGe:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " >= 0.0";
            break;
        case AluScalarOpcode::KillsNe:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " != 0.0";
            break;
        case AluScalarOpcode::KillsOne:
            out_ += EmitOperand(instr, OperandKind::Scalar0) + " == 1.0";
            break;
        case AluScalarOpcode::Sqrt:
            out_ += "sqrt(" + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        case AluScalarOpcode::Mulsc0:
        case AluScalarOpcode::Mulsc1:
            out_ += "xm(" + EmitOperand(instr, OperandKind::ScalarConstant0) + ", " +
                    EmitOperand(instr, OperandKind::ScalarConstant1) + ")";
            break;
        case AluScalarOpcode::Addsc0:
        case AluScalarOpcode::Addsc1:
            out_ += EmitOperand(instr, OperandKind::ScalarConstant0) + " + " +
                    EmitOperand(instr, OperandKind::ScalarConstant1);
            break;
        case AluScalarOpcode::Subsc0:
        case AluScalarOpcode::Subsc1:
            out_ += EmitOperand(instr, OperandKind::ScalarConstant0) + " - " +
                    EmitOperand(instr, OperandKind::ScalarConstant1);
            break;
        case AluScalarOpcode::Sin:
            out_ += "sin(" + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        case AluScalarOpcode::Cos:
            out_ += "cos(" + EmitOperand(instr, OperandKind::Scalar0) + ")";
            break;
        default:
            ++unsupported_;
            out_ += "0.0";
            break;
        }

        if (instr.scalarSaturate) out_ += ')';
        out_ += ";\n";

        // MaxAs / MaxAsf -> address register.
        if (instr.scalarOpcode == AluScalarOpcode::MaxAs) {
            out_ += "    a0 = (int)clamp(floor(" +
                    EmitOperand(instr, OperandKind::Scalar0) + " + 0.5), -256.0, 255.0);\n";
        } else if (instr.scalarOpcode == AluScalarOpcode::MaxAsf) {
            out_ += "    a0 = (int)clamp(floor(" +
                    EmitOperand(instr, OperandKind::Scalar0) + "), -256.0, 255.0);\n";
        }

        // Scalar write. oDepth is a scalar float, so a scalar write to the
        // depth export is a plain assignment of the scalar value (the X lane).
        uint32_t scalarWriteMask = instr.scalarWriteMask;
        if (instr.exportData) scalarWriteMask &= ~instr.vectorWriteMask;
        if (scalarWriteMask != 0 && !suppressVsExport) {
            std::string dst;
            if (!exportReg.empty()) {
                dst = exportReg;
                NoteExport(ps, instr.vectorDest);
            } else {
                dst = RegName(instr.scalarDest);
                NoteRegister(instr.scalarDest);
            }
            if (exportReg != "oDepth") {
                dst += '.';
                for (uint32_t i = 0; i < 4; i++) {
                    if ((scalarWriteMask >> i) & 0x1) dst += kSwizzle[i];
                }
            }
            out_ += "    " + dst + " = ps;\n";
        }
    }

    // ---- fetch lowering ---------------------------------------------------

    void NoteTextureSampler(const DecodedInstruction& instr) {
        usedSamplers_.insert(instr.constIndex);
        samplerDim_[instr.constIndex] = instr.dimension;
    }

    static uint32_t SampleRegisterForFetch(const DecodedInstruction& instr) {
        // Host sampler table / root signature provides s0..s{kShaderSamplerCount-1}
        // (pipeline_cache.h; 16 today). Fetch constants beyond it (VS tex16..31) fall back to
        // s0 -- the PS slot-0 sampler -- so DXIL reflection stays inside the root signature.
        // Raising kShaderSamplerCount to 32 (+ filling table entries 16..31 from the VS fetch
        // constants) makes this emit s16..s31 with no further change here (MD-10/38a).
        // MCLA_NO_VS_SAMPLER=1 restores the clamp to 16 whatever the constant says.
        static const bool noHi = [] { const char* e = std::getenv("MCLA_NO_VS_SAMPLER"); return e && e[0] == '1'; }();
        const uint32_t lim = noHi ? 16u : kShaderSamplerCount;
        return (UseGuestSampler() && instr.constIndex < lim) ? instr.constIndex : 0u;
    }

    void NoteSamplerRegister(const DecodedInstruction& instr) {
        usedSamplerRegs_.insert(SampleRegisterForFetch(instr));
    }

    // ABSOLUTE here, unlike the ALU vector operands above. Measured from the
    // reference's own two accessors, which deliberately differ:
    //   - ALU vector sources are COMPONENT-RELATIVE --
    //     ucode.h:1999-2003 ((swizzle >> 2i) + i) & 3, comment at :2068
    //     "Swizzles are component-relative";
    //   - a texture fetch's SOURCE swizzle is ABSOLUTE --
    //     shader_translator.cc:1132-1135 walks `swizzle >>= 2` and takes
    //     `swizzle & 0x3` with no `+ i`.
    // ucode.h:677-680 says the same in prose for the fetch DESTINATION
    // swizzle ("absolute, not relative to the component itself, unlike in
    // ALU operation sources").
    static std::string BuildFetchSrc(const DecodedInstruction& instr, uint32_t comps) {
        std::string src = RegName(instr.srcRegister) + ".";
        for (uint32_t i = 0; i < comps; i++) {
            src += kSwizzle[(instr.srcSwizzle >> (i * 2)) & 0x3];
        }
        return src;
    }

    // Builds 2D texture coordinates for Texture2D ops (1D promotes to row 0.5),
    // applying texel offset (+ Xenia's 1.5/1024 rounding epsilon) and denorm
    // normalization via xuv().
    static std::string BuildSampleCoord2D(const DecodedInstruction& instr,
                                            const std::string& texName,
                                            const std::string& src,
                                            bool applyOffset) {
        const std::string base = instr.dimension == 0 ? "float2(" + src + ", 0.5)" : src;
        const bool needOffset = applyOffset &&
                                (FetchEpsilonEnabled() || instr.texOffsetX != 0.0f || instr.texOffsetY != 0.0f);
        if (!needOffset && !instr.texCoordDenorm) return base;
        const float eps = applyOffset ? (1.5f / 1024.0f) : 0.0f;
        const float offX = applyOffset ? (instr.texOffsetX + eps) : 0.0f;
        const float offY = (applyOffset && instr.dimension != 0) ? (instr.texOffsetY + eps) : 0.0f;
        return "xuv(" + texName + ", " + base + ", float2(" +
               FloatLit(offX) + ", " + FloatLit(offY) + "), " +
               (instr.texCoordDenorm ? "true" : "false") + RsArg(texName) + ")";
    }

    std::string BuildTextureSampleExpr(const DecodedInstruction& instr,
                                       const std::string& texName,
                                       const std::string& samplerName,
                                       const std::string& sampleCoord) {
        const bool ps = !prog_.isVertex;
        // mipFilter == 2 is TextureFilter::kBaseMap (ucode.h:544): always sample mip 0.
        if (instr.mipFilter == 2u) {
            return texName + ".SampleLevel(" + samplerName + ", " + sampleCoord + ", 0.0)";
        }
        const bool useCompLod = instr.useCompLod && (ps || instr.useRegGradients);
        std::string biasExpr;
        if (instr.useRegLod) {
            usesXfLod_ = true;
            biasExpr = "xfLod";
        }
        if (instr.lodBias != 0.0f) {
            if (!biasExpr.empty()) biasExpr += " + ";
            biasExpr += FloatLitDecimal(instr.lodBias);
        }
        if (!useCompLod) {
            return texName + ".SampleLevel(" + samplerName + ", " + sampleCoord + ", " +
                   (biasExpr.empty() ? std::string("0.0") : biasExpr) + ")";
        }
        if (instr.useRegGradients) {
            usesXfGradH_ = true;
            usesXfGradV_ = true;
            std::string gh = instr.dimension == 0 ? "float2(xfGradH.x, 0.0)" : "xfGradH.xy";
            std::string gv = instr.dimension == 0 ? "float2(xfGradV.x, 0.0)" : "xfGradV.xy";
            if (!biasExpr.empty()) {
                const std::string scale = "exp2(" + biasExpr + ")";
                gh = "(" + gh + " * " + scale + ")";
                gv = "(" + gv + " * " + scale + ")";
            }
            if (instr.texCoordDenorm) {
                usesXgrad_ = true;
                gh = "xgrad(" + texName + ", " + gh + RsArg(texName) + ")";
                gv = "xgrad(" + texName + ", " + gv + RsArg(texName) + ")";
            }
            return texName + ".SampleGrad(" + samplerName + ", " + sampleCoord + ", " + gh + ", " + gv + ")";
        }
        if (biasExpr.empty()) {
            return texName + ".Sample(" + samplerName + ", " + sampleCoord + ")";
        }
        return texName + ".SampleBias(" + samplerName + ", " + sampleCoord +
               ", clamp(" + biasExpr + ", -16.0, 15.99))";
    }

    void EmitFetchZeroOne(const std::string& regName, uint32_t dstSwizzle) {
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t s = (dstSwizzle >> (i * 3)) & 0x7;
            if (s == 4) {
                out_ += "    " + regName + "." + kSwizzle[i] + " = 0.0f;\n";
            } else if (s == 5) {
                out_ += "    " + regName + "." + kSwizzle[i] + " = 1.0f;\n";
            }
        }
    }

    void EmitFetchDest(const DecodedInstruction& instr,
                       const std::string& lhsSwz,
                       const std::string& rhsSwz,
                       bool hasZeroOne,
                       const std::string& expr) {
        if (!lhsSwz.empty()) {
            NoteRegister(instr.dstRegister);
            out_ += "    " + RegName(instr.dstRegister) + "." + lhsSwz +
                    " = (" + expr + ")." + rhsSwz + ";\n";
        }
        if (hasZeroOne) {
            NoteRegister(instr.dstRegister);
            EmitFetchZeroOne(RegName(instr.dstRegister), instr.dstSwizzle);
        }
    }

    void EmitTextureFetchOp(const DecodedInstruction& instr,
                            const std::string& lhsSwz,
                            const std::string& rhsSwz,
                            bool hasZeroOne) {
        const bool ps = !prog_.isVertex;
        switch (instr.fetchOpcode) {
        case FetchOpcode::TextureFetch: {
            NoteRegister(instr.srcRegister);
            NoteTextureSampler(instr);
            const uint32_t comps = instr.dimension >= 2 ? 3u : (instr.dimension == 1 ? 2u : 1u);
            const std::string src = BuildFetchSrc(instr, comps);
            const std::string texName = "tex" + std::to_string(instr.constIndex);
            // The host only creates Texture2D SRVs: 1D samples row 0.5, 3D/cube
            // (dimension 2/3) have no host resource and return mid grey.
            const bool noHostDim = instr.dimension >= 2;
            if (noHostDim) {
                EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne, "float4(0.5, 0.5, 0.5, 1.0)");
                break;
            }
            NoteSamplerRegister(instr);
            const std::string samplerName = "s" + std::to_string(SampleRegisterForFetch(instr));
            const std::string sampleCoord = BuildSampleCoord2D(instr, texName, src, /*applyOffset=*/true);
            if (tl_hvUV && instr.constIndex == 15)
                out_ += "    if(hvUVn==" + std::to_string(tl_hvUVIdx) + ") hvUV = float4(" + sampleCoord + ", 0.0, 0.0); hvUVn++;\n";
            EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne,
                          BuildTextureSampleExpr(instr, texName, samplerName, sampleCoord));
            break;
        }
        case FetchOpcode::GetTextureBorderColorFrac:
            // Even reference dxbc_shader_translator_fetch.cc leaves getBCF unimplemented.
            ++unsupported_;
            out_ += "    // fetch opcode 16 (getBCF) unimplemented\n";
            EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne, "float4(0.0, 0.0, 0.0, 0.0)");
            break;
        case FetchOpcode::GetTextureComputedLod: {
            if (!ps || instr.dimension >= 2) {
                EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne, "float4(0.0, 0.0, 0.0, 0.0)");
                break;
            }
            NoteRegister(instr.srcRegister);
            NoteTextureSampler(instr);
            NoteSamplerRegister(instr);
            const uint32_t comps = instr.dimension == 1 ? 2u : 1u;
            const std::string src = BuildFetchSrc(instr, comps);
            const std::string texName = "tex" + std::to_string(instr.constIndex);
            const std::string samplerName = "s" + std::to_string(SampleRegisterForFetch(instr));
            const std::string sampleCoord = BuildSampleCoord2D(instr, texName, src, /*applyOffset=*/false);
            EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne,
                          "float4(" + texName + ".CalculateLevelOfDetailUnclamped(" + samplerName + ", " +
                          sampleCoord + "), 0.0, 0.0, 0.0)");
            break;
        }
        case FetchOpcode::GetTextureGradients: {
            if (!ps) {
                EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne, "float4(0.0, 0.0, 0.0, 0.0)");
                break;
            }
            NoteRegister(instr.srcRegister);
            const std::string reg = RegName(instr.srcRegister);
            const std::string c0 = reg + "." + kSwizzle[instr.srcSwizzle & 0x3];
            const std::string c1 = reg + "." + kSwizzle[(instr.srcSwizzle >> 2) & 0x3];
            // ucode.h:640: XZ = ddx(source.xy), YW = ddy(source.xy).
            EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne,
                          "float4(ddx_coarse(" + c0 + "), ddy_coarse(" + c0 + "), ddx_coarse(" +
                          c1 + "), ddy_coarse(" + c1 + "))");
            break;
        }
        case FetchOpcode::GetTextureWeights: {
            const float eps = 1.5f / 1024.0f;
            if (instr.dimension <= 1) {
                NoteRegister(instr.srcRegister);
                NoteTextureSampler(instr);
                usesXweights_ = true;
                const uint32_t comps = instr.dimension == 1 ? 2u : 1u;
                const std::string src = BuildFetchSrc(instr, comps);
                const std::string texName = "tex" + std::to_string(instr.constIndex);
                const std::string base = instr.dimension == 0 ? "float2(" + src + ", 0.5)" : src;
                const float offX = instr.texOffsetX - 0.5f + eps;
                const float offY = instr.dimension == 0 ? 0.0f : (instr.texOffsetY - 0.5f + eps);
                const std::string w = "xweights(" + texName + ", " + base + ", float2(" +
                                      FloatLit(offX) + ", " + FloatLit(offY) + "), " +
                                      (instr.texCoordDenorm ? "true" : "false") + RsArg(texName) + ")";
                EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne,
                              instr.dimension == 0
                                  ? "float4(" + w + ".x, 0.0, 0.0, 0.0)"
                                  : "float4(" + w + ", 0.0, 0.0)");
            } else if (instr.texCoordDenorm) {
                NoteRegister(instr.srcRegister);
                const std::string src = BuildFetchSrc(instr, 3);
                EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne,
                              "float4(frac(" + src + " + float3(" +
                              FloatLit(instr.texOffsetX - 0.5f + eps) + ", " +
                              FloatLit(instr.texOffsetY - 0.5f + eps) + ", " +
                              FloatLit(instr.texOffsetZ - 0.5f + eps) + ")), 0.0)");
            } else {
                EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne, "float4(0.0, 0.0, 0.0, 0.0)");
            }
            break;
        }
        case FetchOpcode::SetTextureLod:
            usesXfLod_ = true;
            NoteRegister(instr.srcRegister);
            out_ += "    xfLod = " + BuildFetchSrc(instr, 1) + ";\n";
            break;
        case FetchOpcode::SetTextureGradientsHorz:
            usesXfGradH_ = true;
            NoteRegister(instr.srcRegister);
            out_ += "    xfGradH = " + BuildFetchSrc(instr, 3) + ";\n";
            break;
        case FetchOpcode::SetTextureGradientsVert:
            usesXfGradV_ = true;
            NoteRegister(instr.srcRegister);
            out_ += "    xfGradV = " + BuildFetchSrc(instr, 3) + ";\n";
            break;
        default: {
            ++unsupported_;
            const uint32_t fo = static_cast<uint32_t>(instr.fetchOpcode);
            out_ += "    // fetch opcode " + std::to_string(fo) + " not lowered\n";
            if (fo < 24u) {
                EmitFetchDest(instr, lhsSwz, rhsSwz, hasZeroOne, "float4(0.0, 0.0, 0.0, 0.0)");
            }
            break;
        }
        }
    }

    void EmitFetch(const IrInstruction& ir) {
        const DecodedInstruction& instr = ir.decoded;
        const bool predicated = instr.isPredicated;
        if (predicated) {
            out_ += std::string("    if (") + (instr.predicateCondition ? "" : "!") +
                    "p0)\n    {\n";
        }

        // Xenos fetch dest swizzle: 12 bits, 3 bits per component
        // (0-3 = x/y/z/w, 4 = zero, 5 = one, 6/7 = keep). The LHS suffix is
        // written in position order with unique components; the RHS maps to
        // the requested source components and may repeat. Zero/One dest slots
        // are filled by separate constant writes.
        std::string lhsSwz, rhsSwz;
        bool hasZeroOne = false;
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t s = (instr.dstSwizzle >> (i * 3)) & 0x7;
            if (s <= 3) {
                lhsSwz += kSwizzle[i];
                rhsSwz += kSwizzle[s];
            } else if (s == 4 || s == 5) {
                hasZeroOne = true;
            }
        }

        // Zero/One dest slots get explicit constant writes after the fetch.
        auto emitZeroOne = [&](const std::string& regName) {
            EmitFetchZeroOne(regName, instr.dstSwizzle);
        };

        if (instr.kind == InstructionKind::VertexFetch) {
            if (lhsSwz.empty()) {
                lhsSwz = "x";
                rhsSwz = "x";
            }
            // Vertex fetch: read from the matching vertex element input by
            // usage + usageIndex. The input is derived from the guest stream
            // via the program's vertexElements (usage/index packed).
            // Emit from the vertex element whose 12-bit stream address equals
            // this fetch's IR instruction address when known, otherwise fall
            // back to the first element. Diagnostic-grade: the runtime binds
            // real stream data.
            uint32_t usage = 0, ui = 0;
            bool found = false;
            for (const auto& ve : prog_.vertexElements) {
                if (!prog_.vertexElementsDerived && ve.address == ir.address) {
                    usage = ve.usage;
                    ui = ve.usageIndex;
                    found = true;
                    break;
                }
            }
            if (prog_.vertexElements.empty()) {
                // Same ordinal table ReferencedVertexInputs uses (the k-th vertex fetch is the k-th input).
                uint32_t ord = 0;
                for (const auto& o : prog_.instructions) {
                    if (!o.isFetch || o.decoded.kind != InstructionKind::VertexFetch) continue;
                    if (o.address == ir.address) break;
                    ++ord;
                }
                FallbackVertexSemantic(ord, usage, ui);
            }
            if (!found && !prog_.vertexElements.empty()) {
                // A fetch no export names (its register feeds ALU math) is its own input:
                // falling back to element 0 made every such fetch read POSITION.
                usage = 5;  // TEXCOORD
                ui = 16u + ir.address;
            }
            // usage is a 4-bit guest field (0-15) but the table has 14 entries;
            // clamp so a malformed container cannot index past the array.
            if (usage >= 14) usage = 0;
            NoteRegister(instr.dstRegister);
            NoteVertexInput(usage, ui);
            const std::string inputName =
                    "i" + std::string(kUsageVariables[usage]) +
                    std::to_string(ui);
            // The host layout has no signed 10_10_10_2 (and maps signed 8_8_8_8 to
            // UNORM), so a signed normalised fetch re-signs the unorm value here
            // (Xenia: shader unpack, signed = max(s / (2^(n-1)-1), -1)).
            // The guest fetch already applies .zyxw (docs/INV_COLOURS.md), so no shader-side fmt-6 R/B swap.
            // The R/B swap is applied to the FINAL expression (below), never here: the
            // signed-normalise branch overwrites srcExpr, so building the swap as srcExpr made
            // a fmt-6 signed-normalised fetch silently lose the fix. (0 such fetches in the 91
            // VS dumps as of fh1 -- a latent trap, not a live defect.)
            // `in` is the float4 the fetch reads: the IA input (default) or, under RAWVB, the raw-VB load.
            auto fetchAssign = [&](const std::string& in) {
                std::string srcExpr = in;
                // 16_16 / 16_16_16_16 (25/26) are UNORM in the host layout too (gpu_cp.cpp CpXenosFormatToDxgi).
                if (instr.isSigned && instr.isNormalized &&
                    (instr.vertexFormat == 7 || instr.vertexFormat == 6 || instr.vertexFormat == 25 || instr.vertexFormat == 26)) {
                    const bool f10 = instr.vertexFormat == 7;
                    const bool f16 = instr.vertexFormat == 25 || instr.vertexFormat == 26;
                    const char* scale = f10 ? "float4(1023.0,1023.0,1023.0,3.0)" : f16 ? "float4(65535.0,65535.0,65535.0,65535.0)" : "float4(255.0,255.0,255.0,255.0)";
                    const char* half = f10 ? "float4(512.0,512.0,512.0,2.0)" : f16 ? "float4(32768.0,32768.0,32768.0,32768.0)" : "float4(128.0,128.0,128.0,128.0)";
                    const char* full = f10 ? "float4(1024.0,1024.0,1024.0,4.0)" : f16 ? "float4(65536.0,65536.0,65536.0,65536.0)" : "float4(256.0,256.0,256.0,256.0)";
                    const char* mx = f10 ? "float4(511.0,511.0,511.0,1.0)" : f16 ? "float4(32767.0,32767.0,32767.0,32767.0)" : "float4(127.0,127.0,127.0,127.0)";
                    srcExpr = std::string("max((round(") + in + "*" + scale + ")-" + full +
                              "*(round(" + in + "*" + scale + ")>=" + half + "))/" + mx +
                              ",-1.0)";
                }
                // Integer (non-normalised) packed formats read the raw integer, as Xenia does
                // (dxbc_shader_translator_fetch.cc: is_integer keeps IBFE/UBFE values unscaled).
                // The host layout is UNORM, so undo its scale and, if signed, re-sign.
                static const bool s_oldFetch = [] { const char* e = std::getenv("MCLA_OLD_VFETCH"); return e && e[0] == '1'; }();
                if (!s_oldFetch && !instr.isNormalized &&
                    (instr.vertexFormat == 7 || instr.vertexFormat == 6 || instr.vertexFormat == 25 || instr.vertexFormat == 26)) {
                    const bool f10 = instr.vertexFormat == 7;
                    const bool f16 = instr.vertexFormat == 25 || instr.vertexFormat == 26;
                    const char* scale = f10 ? "float4(1023.0,1023.0,1023.0,3.0)" : f16 ? "float4(65535.0,65535.0,65535.0,65535.0)" : "float4(255.0,255.0,255.0,255.0)";
                    const std::string u = std::string("round(") + in + "*" + scale + ")";
                    if (instr.isSigned) {
                        const char* half = f10 ? "float4(512.0,512.0,512.0,2.0)" : f16 ? "float4(32768.0,32768.0,32768.0,32768.0)" : "float4(128.0,128.0,128.0,128.0)";
                        const char* full = f10 ? "float4(1024.0,1024.0,1024.0,4.0)" : f16 ? "float4(65536.0,65536.0,65536.0,65536.0)" : "float4(256.0,256.0,256.0,256.0)";
                        srcExpr = "(" + u + "-" + full + "*(" + u + ">=" + half + "))";
                    } else {
                        srcExpr = u;
                    }
                }
                // Exponent bias of the fetch (Xenia: result *= ldexp(1, exp_adjust)).
                if (!s_oldFetch && instr.expAdjust != 0) {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%.9g", std::ldexp(1.0, instr.expAdjust));
                    srcExpr = "((" + srcExpr + ")*" + buf + ")";
                }
                // Applied LAST so it composes with (and survives) the signed-normalise branch above.
                // Algebraically the same as swapping before the guest swizzle: the emit appends
                // rhsSwz afterwards, and (S o G)(x) == S applied to the R/B-swapped vector.
                return "    " + RegName(instr.dstRegister) + "." + lhsSwz +
                       " = (" + srcExpr + ")." + rhsSwz + ";\n";
            };
            // RAWVB variant: address = (index * stride + offset) dwords from the VB base, where index is the
            // GPR the guest ucode computed (srcRegister.srcSwizzle) -- not SV_VertexID itself. A mini fetch
            // inherits index source + stride from the preceding full vfetch (Xenia). Formats not listed keep the IA read.
            std::string rawLoad;
            if (RawVbTranslate()) {
                switch (instr.vertexFormat) {
                    case 6:  rawLoad = "xrvU8(g_rawVb.Load(xrvA))"; break;
                    case 7:  rawLoad = "xrvU1010(g_rawVb.Load(xrvA))"; break;
                    case 25: rawLoad = "xrvU16x2(g_rawVb.Load(xrvA))"; break;
                    case 26: rawLoad = "xrvU16x4(g_rawVb.Load2(xrvA))"; break;
                    case 31: rawLoad = "xrvF16x2(g_rawVb.Load(xrvA))"; break;
                    case 32: rawLoad = "xrvF16x4(g_rawVb.Load2(xrvA))"; break;
                    case 33: rawLoad = "float4((float)g_rawVb.Load(xrvA), 0.0, 0.0, 1.0)"; break;
                    case 34: rawLoad = "float4((float2)g_rawVb.Load2(xrvA), 0.0, 1.0)"; break;
                    case 35: rawLoad = "(float4)g_rawVb.Load4(xrvA)"; break;
                    case 36: rawLoad = "float4(asfloat(g_rawVb.Load(xrvA)), 0.0, 0.0, 1.0)"; break;
                    case 37: rawLoad = "float4(asfloat(g_rawVb.Load2(xrvA)), 0.0, 1.0)"; break;
                    case 38: rawLoad = "asfloat(g_rawVb.Load4(xrvA))"; break;
                    case 57: rawLoad = "float4(asfloat(g_rawVb.Load3(xrvA)), 1.0)"; break;
                    default: break;
                }
            }
            if (!rawLoad.empty()) {
                const DecodedInstruction* fullF = &instr;
                if (instr.isMiniFetch) {
                    for (const auto& o : prog_.instructions) {
                        if (o.address >= ir.address) break;
                        if (o.isFetch && o.decoded.kind == InstructionKind::VertexFetch && !o.decoded.isMiniFetch)
                            fullF = &o.decoded;
                    }
                }
                NoteRegister(fullF->srcRegister);
                const std::string tag = std::to_string(ir.address);
                auto uniq = [&](std::string s) {  // make the per-fetch locals unique within the function scope
                    for (size_t p = 0; (p = s.find("xrvA", p)) != std::string::npos; p += 4 + tag.size()) s.insert(p + 4, tag);
                    return s;
                };
                out_ += "#if defined(XE_RAWVB)\n";
                out_ += "    uint xrvA" + tag + " = uint(int(" + RegName(fullF->srcRegister) + "." +
                        kSwizzle[fullF->srcSwizzle & 3] + (fullF->isIndexRounded ? " + 0.5" : "") +
                        ") * " + std::to_string(fullF->stride) + " + (" + std::to_string(instr.offset) + ")) * 4u;\n";
                out_ += "    float4 xrvV" + tag + " = " + uniq(rawLoad) + ";\n";
                out_ += fetchAssign("xrvV" + tag);
                out_ += "#else\n";
                out_ += fetchAssign(inputName);
                out_ += "#endif\n";
            } else {
                out_ += fetchAssign(inputName);
            }
            emitZeroOne(RegName(instr.dstRegister));
        } else if (instr.kind == InstructionKind::TextureFetch) {
            EmitTextureFetchOp(instr, lhsSwz, rhsSwz, hasZeroOne);
        }

        if (predicated) {
            out_ += "    }\n";
        }
    }
};

}  // namespace

bool ParseShaderContainer(const uint8_t* data, size_t size,
                          ShaderProgram& prog, TranslatedShader& out,
                          uint32_t psNumReg) {
    if (!ParseShaderProgram(data, size, prog, psNumReg))
        return false;
    out.isVertex = prog.isVertex;
    out.programHash = ComputeShaderProgramHash(prog);
    // Interpolator hashviz: for one target PS program hash, paint oC0 with the
    // per-pixel VS interpolator values that feed the UV/classification chain.
    if (!prog.isVertex) {
        out.psExportColorMask = QueryPsExportColorMask(prog);
        out.psExportsDepth = prog.pixelOutputs.depth;
        for (const auto& ir : prog.instructions) {
            if (!ir.isFetch && ir.decoded.kind == InstructionKind::Alu &&
                ir.decoded.exportData && ir.decoded.vectorDest == 61u) {
                out.psExportsDepth = true;
            }
        }
    } else {
        out.psExportColorMask = 0;
        out.psExportsDepth = false;
    }
    return true;
}

bool HashShaderContainer(const uint8_t* data, size_t size, bool& isVertex, uint64_t& programHash) {
    ShaderProgram prog;
    if (!ParseShaderProgram(data, size, prog))
        return false;
    isVertex = prog.isVertex;
    programHash = ComputeShaderProgramHash(prog);
    return true;
}

// Publishes the render-target extent the clip_disable rescale is built from.
// Called by the backend at the point it sets the D3D12 viewport, from the SAME
// m_width/m_height the viewport uses, so the shader's constants and the host
// viewport cannot disagree.
//
// `extentX`/`extentY` are the RENDER TARGET dimensions in pixels, not the
// viewport rectangle: the reference's `extent_axis_unscaled` is
// min(8192, RT extent), where the RT extent is what bounds the guest pixels, and
// draw_util.cc:350-353 takes it from the target, not from the viewport.
//
// Returns false when either extent is zero, which leaves any previously published
// transform in place rather than installing an unusable one. Returns true when
// the extent was accepted, INCLUDING when it is unchanged from the value already
// published (that is not an error, it is the steady state).
//
// NOT THREAD-SAFE with respect to in-flight translation, and that is called out
// rather than papered over: the constants are baked into generated HLSL, so a
// translation that starts before this call and ends after it can pick up the new
// extent. The backend calls this during setup, before the pipeline worker is
// started, which is the only ordering the current pipeline guarantees.
bool SetClipDisableViewportExtent(uint32_t extentX, uint32_t extentY) {
    if (extentX == 0 || extentY == 0) return false;
    const ClipDisableRescale next = MakeClipDisableRescale(extentX, extentY);
    if (!next.valid) return false;
    g_clipExtPacked.store((uint64_t(extentX) << 32) | extentY, std::memory_order_release);
    return true;
}

// See the note beside the declaration in shader_translator.h for why this is
// defined out here rather than inside the file's anonymous namespace.
const char* const kClipDisableRescaleGateNote =
    "per draw: applied only when PA_CL_CLIP_CNTL is present with clip_disable set and "
    "PA_CL_VTE_CNTL is present with all six vport_*_ena clear. The gate is decoded from "
    "the same register snapshot used to derive the pipeline state; no title-wide "
    "assumption is made.";

bool QueryClipDisableRescale(ClipDisableRescaleConstants* out) {
    if (!out) return false;
    *out = ClipDisableRescaleConstants{};
    const ClipDisableRescale r = PublishedClipRescale();
    if (!r.valid) return false;
    out->scaleX = r.scaleX;
    out->scaleY = r.scaleY;
    out->scaleZ = r.scaleZ;
    out->offsetX = r.offsetX;
    out->offsetY = r.offsetY;
    out->offsetZ = r.offsetZ;
    out->valid = true;
    return true;
}

// Shared body of TranslateShader / TranslateShaderPinned. `havePin == false` is
// the unpinned variant. Returns false with `out`/`error` describing the refusal.
static bool TranslateShaderImpl(const uint8_t* container, size_t size,
                                const VertexInputSemantic* pinLayout, size_t pinCount,
                                bool havePin, TranslatedShader& out,
                                std::string& error,
                                bool applyClipDisableRescale,
                                const PixelShaderPairing* pairing = nullptr,
                                uint32_t clipExtW = 0, uint32_t clipExtH = 0,
                                const ShaderTranslateExtras* extras = nullptr) {
    // Reset all per-call state up front so a failure path never leaks state
    // from a prior call on the same object.
    out.error.clear();
    out.hlsl.clear();
    out.dxil.clear();
    out.dxilOk = false;
    out.instructionCount = 0;
    out.unknownOpcodeCount = 0;
    out.unsupportedOpcodeCount = 0;
    out.clipRescaleBaked = true;   // nothing withheld until an epilogue says so

    ShaderProgram prog;
    // psNumReg is register state and must reach the pixel interpolant derivation,
    // so it goes in at the PARSE rather than being patched onto `prog` afterwards:
    // DerivePixelInterpolatorsFromInstructionStream runs inside ParseShaderProgram
    // and consumes it. 0 (no pairing supplied) is the initialiser, not a guest
    // value -- see ShaderProgram::psNumReg.
    if (!ParseShaderContainer(container, size, prog, out,
                              pairing != nullptr ? pairing->psNumReg : 0u)) {
        error = "container parse failed";
        out.error = error;
        return false;
    }

    // ------------------------------------------------------------------
    // PRE-COMPILED SHADER CENSUS (docs/SHADER_TRANSLATION_PLAN.md steps 5-6).
    //
    // Ask the setup-time cache whether this exact shader already has a DXIL blob
    // compiled by tools/xenosrecomp.
    //
    // The key is the MICROCODE, not the container: the guest builds its own
    // 96..204-byte container around the same code that sits in a >= 236-byte
    // container inside the retail .fxc, so a container hash never matches
    // (measured: 514 entries, 0 hits). `prog` already carries the two fields
    // that locate the code -- virtualSize + physicalOffset, shaderSize bytes.
    //
    // Reported ONCE per distinct shader (NotePrecompiledShaderLookup returns true
    // on first sighting) so a soak stays linear in distinct guest shaders rather
    // than draws.
    //
    // This does NOT yet substitute the blob into the PSO: the generated DXIL is
    // built against shader_common.h's resource model, so using it needs a
    // matching root signature. Until that lands the census is the measurement
    // that says how much of the frame a substitution would cover.
    // ------------------------------------------------------------------
    {
        const size_t codeStart = size_t(prog.virtualSize) + size_t(prog.physicalOffset);
        const uint8_t* microcode = nullptr;
        if (size != 0 && codeStart + prog.shaderSize <= size) {
            microcode = container + codeStart;
        }
        const uint64_t cacheHash =
            mcla::renderer::MicrocodeCacheHash(microcode, prog.shaderSize, out.isVertex);

        mcla::renderer::PrecompiledShaderView pre;
        const bool hit = cacheHash != 0 && mcla::renderer::FindPrecompiledShader(cacheHash, pre);
        out.precompiledCacheHash = cacheHash;
        out.precompiledAvailable = hit;
        out.precompiledDxilSize = uint32_t(pre.dxilSize);
        out.precompiledSpecConstantsMask = pre.specConstantsMask;

        if (mcla::renderer::NotePrecompiledShaderLookup(cacheHash != 0 ? cacheHash : 1u, hit)) {
            MCLA_LOG_INFO("PRECOMP-SHADER hash={:016X} hit={} pixels={} codeBytes={} "
                          "dxilBytes={} specMask=0x{:X}",
                          cacheHash, hit ? 1 : 0, out.isVertex ? 0 : 1,
                          prog.shaderSize, uint32_t(pre.dxilSize), pre.specConstantsMask);
            // MCLA_DUMP_SHADERS=1 puts the container and its microcode on disk so
            // the runtime bytes can be diffed against the retail .fxc's.
            mcla::renderer::DumpShaderContainer(cacheHash, container, size, microcode,
                                               prog.shaderSize);
        }
    }

    // THE PER-DRAW INTERPOLATOR INTERSECTION, applied at the one point where a
    // pixel shader is translated for a known vertex shader. The reference computes
    // exactly this value once per draw and hands the ONE mask to both stages
    // (vulkan_command_processor.cc:2206-2211,
    // d3d12/d3d12_command_processor.cc:2081-2093), then reads it when seeding each
    // GPR (spirv_shader_translator.cc:2052-2070, dxbc_shader_translator.cc:689-700,
    // the DXBC path being the one this project's backend follows).
    //
    // It has to happen HERE and not in the caller, because `prog.interpolators` is
    // what decides both the entry signature's interpolator inputs
    // (AppendSignature) and each GPR's seed (AppendRegisters) -- an entry outside
    // the mask is not declared as an input and its register is seeded with zero,
    // so filtering it anywhere else would leave the two disagreeing.
    //
    // Applied for a PIXEL program only. writes_interpolators() is a vertex-shader
    // property (shader_translator.cc:597-599 is reached only for kInterpolator,
    // which only a vertex result can target, :1319-1330), so a pairing offered for
    // a vertex program is meaningless rather than an error.
    //
    // NOT A REFUSAL PATH. No pairing, or a VS container that will not parse, both
    // fall through to `vsProg == nullptr` and therefore to mask 0, which is the
    // reference's own behaviour -- so this can neither invent an interpolant nor
    // silently delete a tracked marker's row.
    InterpolatorIntersection pairing2;
    const ShaderProgram* vsProg = nullptr;
    ShaderProgram vsLocal;
    if (pairing != nullptr && !prog.isVertex && pairing->vsContainer != nullptr &&
        pairing->vsContainerSize != 0u) {
        // ps_num_reg is pixel state; a VS parse must not inherit it, so 0.
        if (ParseShaderProgram(pairing->vsContainer, pairing->vsContainerSize,
                               vsLocal, 0u)) {
            vsProg = &vsLocal;
        }
    }
    if (!prog.isVertex) {
        pairing2 = ApplyInterpolatorIntersection(prog, vsProg);
        // The hash is taken AFTER the intersection, so a PSO built from this program
        // is keyed on the shader that was actually emitted. Before this the key
        // would have covered the un-intersected table, and two draws pairing one PS
        // with two different VSs would have collided on a key whose HLSL differs.
        out.programHash = ComputeShaderProgramHash(prog);
        // Interpolator hashviz: target program hash -> paint oC0 with iTexCoord inputs.
        {
            static const uint64_t s_hvInterpHash = [] {
                const char* e = std::getenv("MCLA_HV_INTERP_HASH");
                return e && *e ? std::strtoull(e, nullptr, 16) : 0ULL;
            }();
            tl_hvInterp = (s_hvInterpHash != 0 && out.programHash == s_hvInterpHash);
            static const uint64_t s_hvUVHash = [] {
                const char* e = std::getenv("MCLA_HV_UV");
                return e && *e ? std::strtoull(e, nullptr, 16) : 0ULL;
            }();
            static const int s_hvUVIdx = [] {
                const char* e = std::getenv("MCLA_HV_UV_IDX");
                return e && *e ? std::atoi(e) : 0;
            }();
            tl_hvUV = (s_hvUVHash != 0 && out.programHash == s_hvUVHash);
            tl_hvUVIdx = s_hvUVIdx;
        }
        // Gated authoritative IR dump keyed on the POST-intersection program hash
        // (the value that reaches the backend as psOut.programHash).
        {
            static const uint64_t s_dumpIrHash = [] {
                const char* e = std::getenv("MCLA_DUMP_IR_HASH");
                return e && *e ? std::strtoull(e, nullptr, 16) : 0ULL;
            }();
            if (s_dumpIrHash != 0 && out.programHash == s_dumpIrHash) {
                std::error_code ec; std::filesystem::create_directories("irdump", ec);
                char fn[128];
                std::snprintf(fn, sizeof fn, "irdump/%016llX.ir.txt",
                              (unsigned long long)out.programHash);
                if (std::FILE* f = std::fopen(fn, "wb")) {
                    std::fprintf(f, "programHash=%016llX instrs=%zu\n",
                                 (unsigned long long)out.programHash,
                                 prog.instructions.size());
                    for (const auto& ir : prog.instructions) {
                        const auto& d = ir.decoded;
                        std::fprintf(f, "addr=%04X fetch=%d kind=%d ", ir.address,
                                     ir.isFetch ? 1 : 0, static_cast<int>(d.kind));
                        if (ir.isFetch) {
                            std::fprintf(f, "fOp=%d dst=r%d swz=%03X src=r%d sswz=%03X "
                                            "ci=%d cSel=%d dRel=%d sRel=%d pred=%d",
                                         static_cast<int>(d.fetchOpcode), d.dstRegister, d.dstSwizzle,
                                         d.srcRegister, d.srcSwizzle, d.constIndex, d.constIndexSelect,
                                         d.dstRelative ? 1 : 0, d.srcRelative ? 1 : 0,
                                         d.isPredicated ? 1 : 0);
                        } else {
                            std::fprintf(f, "vOp=%d sOp=%d vDst=r%d sDst=%d "
                                            "vmask=%X smask=%X exp=%d "
                                            "s1=r%d/s%03X/%d/%d s2=r%d/s%03X/%d/%d s3=r%d/s%03X/%d/%d "
                                            "c0Rel=%d c1Rel=%d cAddrRel=%d sel1=%d sel2=%d pred=%d",
                                         static_cast<int>(d.vectorOpcode), static_cast<int>(d.scalarOpcode),
                                         d.vectorDest, d.scalarDest,
                                         d.vectorWriteMask, d.scalarWriteMask, d.exportData ? 1 : 0,
                                         d.src1Register, d.src1Swizzle, d.src1Select ? 1 : 0, d.src1Negate ? 1 : 0,
                                         d.src2Register, d.src2Swizzle, d.src2Select ? 1 : 0, d.src2Negate ? 1 : 0,
                                         d.src3Register, d.src3Swizzle, d.src3Select ? 1 : 0, d.src3Negate ? 1 : 0,
                                         d.const0Relative ? 1 : 0, d.const1Relative ? 1 : 0,
                                         d.constAddressRegisterRelative ? 1 : 0,
                                         d.src1Select ? 1 : 0, d.src2Select ? 1 : 0,
                                         d.isPredicated ? 1 : 0);
                        }
                        std::fprintf(f, "\n");
                    }
                    std::fclose(f);
                }
            }
        }
    }

    // MEASURED 2026-10-01 (build/w690.log, sha256 1d5ac2ab...8641): this gate
    // fired on EVERY pixel shader, and the refusal it produced was a claim
    // about the caller that was false.
    //
    // The one caller, src/render_thread.cpp:2828, is
    //     TranslateShaderPinned(psEnv, psEnv.size(), nullptr, 0, psOut, err)
    // -- a NULL layout and a ZERO count, deliberately, with the comment at
    // :2822-2826 saying exactly that ("A pixel shader is not a vertex shader,
    // so it is handed NO layout"). The test below keyed on `havePin`, which
    // means only "the PINNED ENTRY POINT was chosen", not "a layout was
    // supplied". So it refused that call, and the message asserted an input
    // layout had been supplied when none had been -- a refusal that
    // misdescribes its own cause.
    //
    // The log agrees and shows the refusal is structural, not program-specific:
    // `DRAW-CP-PIPE #2 ... stop=ps-translate-refused ... psTr=0` on the same
    // 36-byte program twice (`CP-SHADER-BODY #2` and `#4` are byte-identical,
    // both found at three addresses in the guest image). Reproduced offline on
    // that exact program: ret=0, hlsl.len=0, error=the string below.
    //
    // THE HONEST TEST IS THE ARGUMENT, not the entry point: a layout is
    // supplied only when the pointer is non-null. Generate() already uses
    // precisely that test for the pin (:225, `layout_ != nullptr`) and a null
    // layout means "no pin" everywhere else (:1368 stores it verbatim). This one
    // gate was the sole dissenter. `havePin` is kept in the condition so the
    // unpinned entry point stays documented as never applying this rule.
    if (havePin && pinLayout != nullptr && !prog.isVertex) {
        // A pixel shader has no vertex inputs; a caller pinning VS semantics
        // onto one has misread its own layout, so say so instead of ignoring it.
        // The count is in the message: "a layout" without its size does not let
        // the caller see its own mistake, and the count is the measured
        // difference between this refusal and the (now legal) nullptr call.
        error = "TranslateShaderPinned: program is a pixel shader "
                "(prog.isVertex=0) but a vertex input layout WAS supplied "
                "(layout != nullptr, count=" + std::to_string(pinCount) +
                "); a pixel shader has no vertex inputs, so the supplied layout "
                "cannot be honoured";
        out.error = error;
        return false;
    }

    for (const auto& ir : prog.instructions)
        if (ir.decoded.unknown) ++out.unknownOpcodeCount;

    uint64_t unsupported = 0;
    HlslGenerator gen(prog, out.hlsl, unsupported, applyClipDisableRescale);
    // The pin is applied INSIDE the generator, between pass 1 (which records the
    // fetched inputs in vsInputs_) and AppendSignature (which emits their
    // semantics), so the signature and the body always agree.
    if (havePin) gen.SetVertexInputLayout(pinLayout, pinCount);
    gen.SetClipExtent(clipExtW, clipExtH);
    if (extras) gen.SetExtras(*extras);
    // Same hand-off for the intersection report. `pairing2` is a local of this
    // function and outlives Generate(), so the pointer is valid for the whole call;
    // it is null for a VERTEX translation, which is what makes the seed marker print
    // "no-vs-pairing" there instead of a mask that was never computed.
    gen.SetPairingReport(prog.isVertex ? nullptr : &pairing2);
    const bool ok = gen.Generate();
    out.unsupportedOpcodeCount = unsupported;
    // Census before refusing: nothing reported this count, so its frequency is unmeasured.
    if (unsupported != 0) {
        static std::atomic<uint32_t> s_unsupShaders{0};
        const uint32_t n = s_unsupShaders.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 64 || (n % 256) == 0)
            MCLA_LOG_ERROR("SHADER-UNSUPPORTED #{} {} unsupported={} unknown={} instrs={}",
                           n, prog.isVertex ? "vs" : "ps", unsupported,
                           out.unknownOpcodeCount, prog.instructions.size());
    }
    out.instructionCount = prog.instructions.size();
    // Read the generator's own record of what its epilogue did, not a re-read of
    // the process global: Generate() has already returned, and the two cannot
    // drift. Reset alongside the other per-call fields at the top of this
    // function, so a failed translation cannot inherit a prior call's value.
    out.clipRescaleBaked = gen.clipRescaleBaked();
    out.psExportColorMask = prog.isVertex ? 0u : gen.psExportColorMask();
    out.psExportsDepth = prog.isVertex ? false : gen.psExportsDepth();

    if (!ok) {
        error = gen.error();
        out.hlsl.clear();
        out.error = error;
        return false;
    }
    if (out.hlsl.empty()) {
        error = "translation produced no HLSL";
        out.error = error;
        return false;
    }

    // F-609: report the TEXT of the position output, never a value derived from
    // it. D3D12 offers no shader-output readback, so any number here would be this
    // file's own arithmetic wearing the pipeline's clothes; the shape is a
    // classification and the expression is quoted out of the emitted string.
    //
    // At most once per rescale-request variant, from the render thread. Both
    // guest-pixel and host-clip-space forms may occur in one run, so each gets a
    // bounded row instead of logging every translation.
    //
    // Names no other tracked marker. `oPos` is an HLSL identifier, not a marker
    // token, and soak_census.py counts matching LINES -- a tracked marker named in
    // this row's prose would make that other marker's count uncountable.
    if (prog.isVertex) {
        static std::atomic<uint32_t> s_oPosLoggedVariants{0};
        const uint32_t variantId = applyClipDisableRescale ? 2u : 1u;
        const uint32_t variantBit = 1u << (variantId - 1u);
        const uint32_t previous =
            s_oPosLoggedVariants.fetch_or(variantBit, std::memory_order_relaxed);
        if ((previous & variantBit) == 0u) {
            const std::string& shape = gen.oPosShape();
            const std::string writes = gen.oPosWriteExpression();
            MCLA_LOG_WARN(
                "VS-POSITION-SHAPE #{} rescaleRequested={}: the emitted vertex program writes the "
                "position system-value as {}; the writes are exactly [{}]. "
                "TEXT ONLY, no value: the host API has no shader-output "
                "readback, so any coordinate printed here would be this "
                "translator's arithmetic rather than the pipeline's output. "
                "When requested and baked, the rescale is the reference's affine "
                "map applied to XYZ with the offset scaled by W, in the epilogue. "
                "A consumer of this semantic should read three corners and "
                "generate the fourth; it is not a rectangle tuple and not a "
                "centre-plus-extent. decl=out float4 oPos : SV_Position "
                "rescaleBaked={} expr={}",
                variantId, applyClipDisableRescale ? 1 : 0,
                shape.empty() ? "<no position written>" : shape,
                writes.empty() ? "<no assignment to the position>" : writes,
                out.clipRescaleBaked ? 1 : 0,
                writes.empty() ? "<none>" : writes);
        }
    }

    out.error.clear();
    return true;
}

bool TranslateShader(const uint8_t* container, size_t size,
                     std::string_view /*commonHeader*/, TranslatedShader& out,
                     bool applyClipDisableRescale) {
    std::string error;
    return TranslateShaderImpl(container, size, nullptr, 0, false, out, error,
                               applyClipDisableRescale);
}

bool TranslateShaderPinned(const uint8_t* container, size_t size,
                           const VertexInputSemantic* layout, size_t layoutCount,
                           TranslatedShader& out, std::string& error,
                           bool applyClipDisableRescale,
                           const PixelShaderPairing* pairing,
                           uint32_t clipExtW, uint32_t clipExtH,
                           const ShaderTranslateExtras* extras) {
    if (!layout && layoutCount != 0) {
        error = "TranslateShaderPinned: null layout with a non-zero count";
        return false;
    }
    return TranslateShaderImpl(container, size, layout, layoutCount, true, out,
                               error, applyClipDisableRescale, pairing, clipExtW, clipExtH, extras);
}

uint8_t QueryPsExportColorMask(const ShaderProgram& prog) {
    if (prog.isVertex) return 0u;
    uint8_t mask = 0u;
    for (const auto& ir : prog.instructions) {
        if (ir.isFetch || ir.decoded.kind != InstructionKind::Alu) continue;
        const auto& d = ir.decoded;
        if (!d.exportData || d.vectorDest > 3u) continue;
        const uint32_t eff = (d.vectorWriteMask | d.scalarWriteMask) & 0xFu;
        if (eff != 0u) mask |= static_cast<uint8_t>(1u << d.vectorDest);
    }
    if (mask == 0u) {
        if (prog.pixelOutputs.color0) mask |= 1u << 0;
        if (prog.pixelOutputs.color1) mask |= 1u << 1;
        if (prog.pixelOutputs.color2) mask |= 1u << 2;
        if (prog.pixelOutputs.color3) mask |= 1u << 3;
    }
    return mask;
}

namespace {

bool HasSimpleLinearControlFlow(const uint8_t* container, size_t size, const ShaderProgram& prog) {
    if (!container || size == 0u) return true;
    const size_t codeStart = size_t(prog.virtualSize) + size_t(prog.physicalOffset);
    if (prog.shaderSize == 0u || codeStart + prog.shaderSize > size) return false;
    const uint8_t* code = container + codeStart;
    const size_t cfBytes = ComputeControlFlowByteBound(code, prog.shaderSize);
    const size_t nBlocks = cfBytes / 12u;
    for (size_t b = 0; b < nBlocks; ++b) {
        const uint8_t* blk = code + b * 12u;
        const uint32_t w0 = AssembleBE32(blk);
        const uint32_t w1 = AssembleBE32(blk + 4u);
        const uint32_t w2 = AssembleBE32(blk + 8u);
        for (int slot = 0; slot < 2; ++slot) {
            const uint32_t cfw0 = slot == 0 ? w0 : ((w1 >> 16) | (w2 << 16));
            const uint16_t cfw1 = slot == 0 ? static_cast<uint16_t>(w1 & 0xFFFFu)
                                            : static_cast<uint16_t>(w2 >> 16);
            const ControlFlowInstruction cf = DecodeControlFlow(cfw0, cfw1);
            switch (cf.opcode) {
                case ControlFlowOpcode::Nop:
                case ControlFlowOpcode::Exec:
                case ControlFlowOpcode::ExecEnd:
                case ControlFlowOpcode::Alloc:
                case ControlFlowOpcode::MarkVsFetchDone:
                    break;
                default:
                    return false;
            }
        }
    }
    return true;
}

bool ValidateFlatLinearIr(const ShaderProgram& prog, bool isPs,
                          std::vector<const IrInstruction*>& sortedOut) {
    sortedOut.clear();
    if (prog.instructions.empty()) return false;
    sortedOut.reserve(prog.instructions.size());
    for (const auto& ir : prog.instructions) {
        const DecodedInstruction& d = ir.decoded;
        if (d.unknown || d.isPredicated || d.dstRelative || d.srcRelative ||
            d.vectorDestRelative || d.scalarDestRelative ||
            d.constAddressRegisterRelative || d.const0Relative || d.const1Relative) {
            return false;
        }
        if (ir.isFetch) {
            if (isPs || d.kind != InstructionKind::VertexFetch) return false;
        } else {
            if (d.kind != InstructionKind::Alu) return false;
            if (d.vectorOpcode >= AluVectorOpcode::SetpEqPush &&
                d.vectorOpcode <= AluVectorOpcode::KillNe) {
                return false;
            }
            if (d.vectorOpcode == AluVectorOpcode::MaxA) return false;
            if (d.scalarOpcode >= AluScalarOpcode::SetpEq &&
                d.scalarOpcode <= AluScalarOpcode::KillsOne) {
                return false;
            }
            if (d.scalarOpcode == AluScalarOpcode::MaxAs ||
                d.scalarOpcode == AluScalarOpcode::MaxAsf) {
                return false;
            }
        }
        sortedOut.push_back(&ir);
    }
    std::stable_sort(sortedOut.begin(), sortedOut.end(),
                     [](const IrInstruction* a, const IrInstruction* b) {
                         return a->address < b->address;
                     });
    return true;
}

struct SymLane {
    enum class Kind : uint8_t { Invalid = 0, Const, Uniform, VertAttr, Interp };
    Kind kind = Kind::Invalid;
    float literal = 0.0f;
    float scale = 1.0f;
    float bias = 0.0f;
    uint16_t reg = 0;      // constant reg (Uniform) or usage/usageIndex
    uint8_t comp = 0;
    uint8_t attrUsage = 0;
    uint8_t attrUsageIndex = 0;
    bool saturate = false;

    // Optional affine form for VS oPos: vPos[vertComp] * (inlineScale * c[scaleReg]) + (inlineOffset + c[offsetReg])
    bool hasUniformScale = false;
    uint16_t scaleConstReg = 0;
    uint8_t scaleConstComp = 0;
    bool hasUniformOffset = false;
    uint16_t offsetConstReg = 0;
    uint8_t offsetConstComp = 0;
};

SymLane MakeConstLane(float v) {
    SymLane l;
    l.kind = SymLane::Kind::Const;
    l.literal = v;
    return l;
}

SymLane ApplyNegAbs(SymLane l, bool neg, bool absFlag) {
    if (l.kind == SymLane::Kind::Invalid) return l;
    if (l.kind == SymLane::Kind::Const) {
        if (absFlag) l.literal = std::fabs(l.literal);
        if (neg) l.literal = -l.literal;
        return l;
    }
    if (absFlag) return SymLane{};  // non-constant abs makes linear affine tracking unsound
    if (neg) {
        l.scale = -l.scale;
        l.bias = -l.bias;
        if (l.hasUniformOffset) return SymLane{};  // keep simple; negated uniform offset rejected
    }
    return l;
}

SymLane ApplySaturate(SymLane l, bool sat) {
    if (!sat || l.kind == SymLane::Kind::Invalid) return l;
    if (l.kind == SymLane::Kind::Const) {
        l.literal = std::clamp(l.literal, 0.0f, 1.0f);
        return l;
    }
    if (l.hasUniformScale || l.hasUniformOffset) return SymLane{};
    l.saturate = true;
    return l;
}

SymLane EvalAdd(const SymLane& a, const SymLane& b) {
    if (a.kind == SymLane::Kind::Invalid || b.kind == SymLane::Kind::Invalid) return SymLane{};
    if (a.kind == SymLane::Kind::Const && b.kind == SymLane::Kind::Const) {
        return MakeConstLane(a.literal + b.literal);
    }
    if (b.kind == SymLane::Kind::Const && !a.saturate) {
        SymLane r = a;
        r.bias += b.literal;
        return r;
    }
    if (a.kind == SymLane::Kind::Const && !b.saturate) {
        SymLane r = b;
        r.bias += a.literal;
        return r;
    }
    // VertAttr + pure Uniform (e.g., oPos = vPos * scale + cOffset)
    if (a.kind == SymLane::Kind::VertAttr && !a.saturate && !a.hasUniformOffset &&
        b.kind == SymLane::Kind::Uniform && !b.saturate && b.scale == 1.0f && b.bias == 0.0f) {
        SymLane r = a;
        r.hasUniformOffset = true;
        r.offsetConstReg = b.reg;
        r.offsetConstComp = b.comp;
        return r;
    }
    if (b.kind == SymLane::Kind::VertAttr && !b.saturate && !b.hasUniformOffset &&
        a.kind == SymLane::Kind::Uniform && !a.saturate && a.scale == 1.0f && a.bias == 0.0f) {
        SymLane r = b;
        r.hasUniformOffset = true;
        r.offsetConstReg = a.reg;
        r.offsetConstComp = a.comp;
        return r;
    }
    return SymLane{};
}

SymLane EvalMul(const SymLane& a, const SymLane& b) {
    if (a.kind == SymLane::Kind::Invalid || b.kind == SymLane::Kind::Invalid) return SymLane{};
    if (a.kind == SymLane::Kind::Const && a.literal == 0.0f) return MakeConstLane(0.0f);
    if (b.kind == SymLane::Kind::Const && b.literal == 0.0f) return MakeConstLane(0.0f);
    if (a.kind == SymLane::Kind::Const && b.kind == SymLane::Kind::Const) {
        return MakeConstLane(a.literal * b.literal);
    }
    if (b.kind == SymLane::Kind::Const && !a.saturate && !a.hasUniformOffset) {
        SymLane r = a;
        r.scale *= b.literal;
        r.bias *= b.literal;
        return r;
    }
    if (a.kind == SymLane::Kind::Const && !b.saturate && !b.hasUniformOffset) {
        SymLane r = b;
        r.scale *= a.literal;
        r.bias *= a.literal;
        return r;
    }
    // VertAttr * pure Uniform (e.g., oPos = vPos * cScale + cOffset)
    if (a.kind == SymLane::Kind::VertAttr && !a.saturate && !a.hasUniformScale &&
        !a.hasUniformOffset && a.bias == 0.0f &&
        b.kind == SymLane::Kind::Uniform && !b.saturate && b.bias == 0.0f) {
        SymLane r = a;
        r.scale *= b.scale;
        r.hasUniformScale = true;
        r.scaleConstReg = b.reg;
        r.scaleConstComp = b.comp;
        return r;
    }
    if (b.kind == SymLane::Kind::VertAttr && !b.saturate && !b.hasUniformScale &&
        !b.hasUniformOffset && b.bias == 0.0f &&
        a.kind == SymLane::Kind::Uniform && !a.saturate && a.bias == 0.0f) {
        SymLane r = b;
        r.scale *= a.scale;
        r.hasUniformScale = true;
        r.scaleConstReg = a.reg;
        r.scaleConstComp = a.comp;
        return r;
    }
    return SymLane{};
}

struct SymEnv {
    SymLane regs[32][4];
    SymLane consts[256][4];
    SymLane psLane;

    void Init(const ShaderProgram& prog, bool isPs) {
        for (uint32_t c = 0; c < 256u; ++c) {
            for (uint32_t k = 0; k < 4u; ++k) {
                SymLane u;
                u.kind = SymLane::Kind::Uniform;
                u.reg = static_cast<uint16_t>(c);
                u.comp = static_cast<uint8_t>(k);
                consts[c][k] = u;
            }
        }
        for (size_t i = 0; i + 4 < prog.float4DefaultValues.size(); i += 5) {
            const uint32_t r = prog.float4DefaultValues[i];
            if (r >= 256u) continue;
            for (uint32_t k = 0; k < 4u; ++k) {
                float f = 0.0f;
                const uint32_t bits = prog.float4DefaultValues[i + 1u + k];
                std::memcpy(&f, &bits, sizeof(f));
                consts[r][k] = MakeConstLane(f);
            }
        }
        for (uint32_t r = 0; r < 32u; ++r) {
            for (uint32_t k = 0; k < 4u; ++k) regs[r][k] = MakeConstLane(1.0f);
        }
        if (isPs) {
            for (const auto& interp : prog.interpolators) {
                if (interp.reg >= 32u) continue;
                for (uint32_t k = 0; k < 4u; ++k) {
                    SymLane l;
                    l.kind = SymLane::Kind::Interp;
                    l.attrUsage = static_cast<uint8_t>(interp.usage);
                    l.attrUsageIndex = static_cast<uint8_t>(interp.usageIndex);
                    l.comp = static_cast<uint8_t>(k);
                    regs[interp.reg][k] = l;
                }
            }
        }
        psLane = MakeConstLane(0.0f);
    }

    SymLane ReadVec(const DecodedInstruction& d, int opIdx, uint32_t compIdx) const {
        const uint32_t reg = (opIdx == 0) ? d.src1Register : (opIdx == 1) ? d.src2Register : d.src3Register;
        const uint32_t swz = (opIdx == 0) ? d.src1Swizzle : (opIdx == 1) ? d.src2Swizzle : d.src3Swizzle;
        const bool sel = (opIdx == 0) ? d.src1Select : (opIdx == 1) ? d.src2Select : d.src3Select;
        const bool neg = (opIdx == 0) ? d.src1Negate : (opIdx == 1) ? d.src2Negate : d.src3Negate;
        const uint32_t sc = ((swz >> (2u * compIdx)) + compIdx) & 3u;
        if (sel) {
            const uint32_t r = reg & 0x3Fu;
            const bool absF = (reg & 0x80u) != 0u;
            if (r >= 32u) return SymLane{};
            return ApplyNegAbs(regs[r][sc], neg, absF);
        }
        if (reg >= 256u) return SymLane{};
        return ApplyNegAbs(consts[reg][sc], neg, d.absConstants);
    }

    SymLane ReadScalar(const DecodedInstruction& d, int which) const {
        if (which == 0) {
            return ReadVec(d, 2, 0);
        }
        const uint32_t reg = (uint32_t(d.scalarOpcode) & 1u) |
                             (uint32_t(d.src3Select) << 1u) |
                             (d.src3Swizzle & 0x3Cu);
        if (reg >= 32u) return SymLane{};
        const uint32_t sc = d.src3Swizzle & 3u;
        return ApplyNegAbs(regs[reg][sc], d.src3Negate, d.absConstants);
    }
};

ClearChannelExpr LaneToChannelExpr(const SymLane& l, bool isPs) {
    ClearChannelExpr e;
    if (l.kind == SymLane::Kind::Invalid || l.hasUniformScale || l.hasUniformOffset)
        return e;
    e.saturate = l.saturate;
    switch (l.kind) {
        case SymLane::Kind::Const:
            e.source = ClearChannelSource::Literal;
            e.literal = l.literal;
            break;
        case SymLane::Kind::Uniform:
            e.source = isPs ? ClearChannelSource::PsConstant : ClearChannelSource::VsConstant;
            e.constReg = l.reg;
            e.constComp = l.comp;
            e.scale = l.scale;
            e.bias = l.bias;
            break;
        case SymLane::Kind::VertAttr:
            e.source = ClearChannelSource::VertexAttr;
            e.attrUsage = l.attrUsage;
            e.attrUsageIndex = l.attrUsageIndex;
            e.attrComp = l.comp;
            e.scale = l.scale;
            e.bias = l.bias;
            break;
        default:
            break;
    }
    return e;
}

void EvalAluStep(const DecodedInstruction& d, SymEnv& env,
                 SymLane (*exports)[4], bool* exportWrittenMask, uint32_t maxExports,
                 uint32_t exportOffset) {
    uint32_t vMask = d.vectorWriteMask & 0xFu;
    if (d.exportData) vMask &= ~(d.scalarWriteMask & 0xFu);

    if (vMask != 0u) {
        SymLane out[4];
        const bool same12 = d.src1Select == d.src2Select &&
                            d.src1Register == d.src2Register &&
                            d.src1Swizzle == d.src2Swizzle &&
                            d.src1Negate == d.src2Negate;
        for (uint32_t c = 0; c < 4u; ++c) {
            if (!(vMask & (1u << c))) continue;
            SymLane res;
            switch (d.vectorOpcode) {
                case AluVectorOpcode::Add:
                    res = EvalAdd(env.ReadVec(d, 0, c), env.ReadVec(d, 1, c));
                    break;
                case AluVectorOpcode::Mul:
                    res = EvalMul(env.ReadVec(d, 0, c), env.ReadVec(d, 1, c));
                    break;
                case AluVectorOpcode::Mad:
                    res = EvalAdd(EvalMul(env.ReadVec(d, 0, c), env.ReadVec(d, 1, c)),
                                  env.ReadVec(d, 2, c));
                    break;
                case AluVectorOpcode::Max: {
                    const SymLane a = env.ReadVec(d, 0, c);
                    const SymLane b = env.ReadVec(d, 1, c);
                    if (same12) res = a;
                    else if (a.kind == SymLane::Kind::Const && b.kind == SymLane::Kind::Const)
                        res = MakeConstLane(std::max(a.literal, b.literal));
                    break;
                }
                case AluVectorOpcode::Min: {
                    const SymLane a = env.ReadVec(d, 0, c);
                    const SymLane b = env.ReadVec(d, 1, c);
                    if (same12) res = a;
                    else if (a.kind == SymLane::Kind::Const && b.kind == SymLane::Kind::Const)
                        res = MakeConstLane(std::min(a.literal, b.literal));
                    break;
                }
                default:
                    break;
            }
            out[c] = ApplySaturate(res, d.vectorSaturate);
        }
        if (d.exportData) {
            if (d.vectorDest >= exportOffset && d.vectorDest - exportOffset < maxExports) {
                const uint32_t slot = d.vectorDest - exportOffset;
                for (uint32_t c = 0; c < 4u; ++c) {
                    if (vMask & (1u << c)) {
                        exports[slot][c] = out[c];
                        exportWrittenMask[slot] = true;
                    }
                }
            }
        } else if (d.vectorDest < 32u) {
            for (uint32_t c = 0; c < 4u; ++c) {
                if (vMask & (1u << c)) env.regs[d.vectorDest][c] = out[c];
            }
        }
    }

    if (d.scalarOpcode != AluScalarOpcode::RetainPrev) {
        SymLane ps;
        switch (d.scalarOpcode) {
            case AluScalarOpcode::Adds:
                ps = EvalAdd(env.ReadScalar(d, 0), env.ReadScalar(d, 1));
                break;
            case AluScalarOpcode::AddsPrev:
                ps = EvalAdd(env.ReadScalar(d, 0), env.psLane);
                break;
            case AluScalarOpcode::Muls:
                ps = EvalMul(env.ReadScalar(d, 0), env.ReadScalar(d, 1));
                break;
            case AluScalarOpcode::MulsPrev:
                ps = EvalMul(env.ReadScalar(d, 0), env.psLane);
                break;
            case AluScalarOpcode::Subs: {
                SymLane negB = env.ReadScalar(d, 1);
                if (negB.kind == SymLane::Kind::Const) negB.literal = -negB.literal;
                else negB = SymLane{};
                ps = EvalAdd(env.ReadScalar(d, 0), negB);
                break;
            }
            case AluScalarOpcode::Maxs:
            case AluScalarOpcode::Mins: {
                const SymLane a = env.ReadScalar(d, 0);
                const SymLane b = env.ReadScalar(d, 1);
                if (a.kind == SymLane::Kind::Const && b.kind == SymLane::Kind::Const) {
                    ps = MakeConstLane(d.scalarOpcode == AluScalarOpcode::Maxs
                                           ? std::max(a.literal, b.literal)
                                           : std::min(a.literal, b.literal));
                }
                break;
            }
            case AluScalarOpcode::Addsc0:
            case AluScalarOpcode::Addsc1:
                ps = EvalAdd(env.ReadScalar(d, 0), env.ReadScalar(d, 2));
                break;
            case AluScalarOpcode::Mulsc0:
            case AluScalarOpcode::Mulsc1:
                ps = EvalMul(env.ReadScalar(d, 0), env.ReadScalar(d, 2));
                break;
            default:
                break;
        }
        env.psLane = ApplySaturate(ps, d.scalarSaturate);
        uint32_t sMask = d.scalarWriteMask & 0xFu;
        if (d.exportData) sMask &= ~d.vectorWriteMask;
        if (sMask != 0u) {
            if (d.exportData) {
                if (d.vectorDest >= exportOffset && d.vectorDest - exportOffset < maxExports) {
                    const uint32_t slot = d.vectorDest - exportOffset;
                    for (uint32_t c = 0; c < 4u; ++c) {
                        if (sMask & (1u << c)) {
                            exports[slot][c] = env.psLane;
                            exportWrittenMask[slot] = true;
                        }
                    }
                }
            } else if (d.scalarDest < 32u) {
                for (uint32_t c = 0; c < 4u; ++c) {
                    if (sMask & (1u << c)) env.regs[d.scalarDest][c] = env.psLane;
                }
            }
        }
    }
}

void ResolveVsFetchSemantic(const ShaderProgram& prog, uint32_t addr,
                            uint32_t fallbackOrd, uint32_t& usage, uint32_t& usageIndex) {
    usage = 0;
    usageIndex = 0;
    if (!prog.vertexElements.empty()) {
        if (prog.vertexElementsDerived) {
            usage = 5;
            usageIndex = 16u + addr;
            return;
        }
        for (const auto& ve : prog.vertexElements) {
            if (ve.address == addr) {
                usage = ve.usage >= 14u ? 0u : ve.usage;
                usageIndex = ve.usageIndex;
                return;
            }
        }
        usage = 5;
        usageIndex = 16u + addr;
        return;
    }
    FallbackVertexSemantic(fallbackOrd, usage, usageIndex);
}

ClearVsPlan AnalyzeClearVs(const ShaderProgram& prog, const uint8_t* container, size_t size) {
    ClearVsPlan plan;
    if (!prog.isVertex || !HasSimpleLinearControlFlow(container, size, prog)) return plan;
    std::vector<const IrInstruction*> order;
    if (!ValidateFlatLinearIr(prog, false, order)) return plan;

    SymEnv env;
    env.Init(prog, false);
    // VS exports: slots 0..15 = interpolators, slot 16 = oPos (vectorDest 62 -> map to 16).
    SymLane exports[17][4];
    bool exportTouched[17] = {};
    exports[16][0] = MakeConstLane(0.0f);
    exports[16][1] = MakeConstLane(0.0f);
    exports[16][2] = MakeConstLane(0.0f);
    exports[16][3] = MakeConstLane(1.0f);

    uint32_t fallbackOrd = 0;
    for (const IrInstruction* ir : order) {
        const DecodedInstruction& d = ir->decoded;
        if (ir->isFetch) {
            uint32_t usage = 0, usageIndex = 0;
            ResolveVsFetchSemantic(prog, ir->address, fallbackOrd++, usage, usageIndex);
            if (d.dstRegister < 32u) {
                for (uint32_t c = 0; c < 4u; ++c) {
                    const uint32_t s = (d.dstSwizzle >> (c * 3u)) & 0x7u;
                    if (s <= 3u) {
                        SymLane l;
                        l.kind = SymLane::Kind::VertAttr;
                        l.attrUsage = static_cast<uint8_t>(usage);
                        l.attrUsageIndex = static_cast<uint8_t>(usageIndex);
                        l.comp = static_cast<uint8_t>(s);
                        env.regs[d.dstRegister][c] = l;
                    } else if (s == 4u) {
                        env.regs[d.dstRegister][c] = MakeConstLane(0.0f);
                    } else if (s == 5u) {
                        env.regs[d.dstRegister][c] = MakeConstLane(1.0f);
                    }
                }
            }
            continue;
        }
        if (d.exportData && d.vectorDest == 62u) {
            EvalAluStep(d, env, &exports[16], &exportTouched[16], 1u, 62u);
        } else if (d.exportData && d.vectorDest < 16u) {
            EvalAluStep(d, env, exports, exportTouched, 16u, 0u);
        } else {
            EvalAluStep(d, env, exports, exportTouched, 0u, 0u);
        }
    }

    if (!exportTouched[16]) return plan;
    // Validate oPos.x and oPos.y come from the same POSITION-bearing vertex attribute.
    const SymLane& px = exports[16][0];
    const SymLane& py = exports[16][1];
    if (px.kind != SymLane::Kind::VertAttr || py.kind != SymLane::Kind::VertAttr) return plan;
    if (px.attrUsage != py.attrUsage || px.attrUsageIndex != py.attrUsageIndex) return plan;
    plan.posUsage = px.attrUsage;
    plan.posUsageIndex = px.attrUsageIndex;

    for (uint32_t c = 0; c < 4u; ++c) {
        const SymLane& l = exports[16][c];
        ClearPosAxisPlan& ax = plan.oPos[c];
        if (l.kind == SymLane::Kind::Const) {
            ax.valid = true;
            ax.fromVertexPos = false;
            ax.inlineScale = 0.0f;
            ax.inlineOffset = l.literal;
        } else if (l.kind == SymLane::Kind::VertAttr &&
                   l.attrUsage == plan.posUsage &&
                   l.attrUsageIndex == plan.posUsageIndex &&
                   !l.saturate) {
            ax.valid = true;
            ax.fromVertexPos = true;
            ax.vertComp = l.comp;
            ax.inlineScale = l.scale;
            ax.inlineOffset = l.bias;
            if (l.hasUniformScale) {
                ax.scaleConstReg = static_cast<int16_t>(l.scaleConstReg);
                ax.scaleConstComp = l.scaleConstComp;
            }
            if (l.hasUniformOffset) {
                ax.offsetConstReg = static_cast<int16_t>(l.offsetConstReg);
                ax.offsetConstComp = l.offsetConstComp;
            }
        } else {
            return plan;
        }
    }

    for (uint32_t vd = 0; vd < 16u; ++vd) {
        if (!exportTouched[vd]) continue;
        plan.exportedInterpMask |= (1u << vd);
        for (uint32_t c = 0; c < 4u; ++c) {
            plan.interp[vd][c] = LaneToChannelExpr(exports[vd][c], false);
        }
    }
    plan.eligible = true;
    return plan;
}

ClearPsPlan AnalyzeClearPs(const ShaderProgram* prog, const ClearVsPlan& vsPlan,
                           const uint8_t* container, size_t size) {
    ClearPsPlan plan;
    if (!prog) {
        plan.eligible = true;
        plan.psPresent = false;
        return plan;
    }
    plan.psPresent = true;
    if (prog->isVertex || !HasSimpleLinearControlFlow(container, size, *prog)) return plan;
    std::vector<const IrInstruction*> order;
    if (!ValidateFlatLinearIr(*prog, true, order)) return plan;

    SymEnv env;
    env.Init(*prog, true);
    // Track oC0..oC3 (slots 0..3) and oDepth (slot 4).
    SymLane exports[5][4];
    bool exportTouched[5] = {};
    uint8_t exportCompMask[5] = {};

    for (const IrInstruction* ir : order) {
        const DecodedInstruction& d = ir->decoded;
        if (d.exportData && d.vectorDest <= 3u) {
            const uint32_t slot = d.vectorDest;
            const uint32_t m = (d.vectorWriteMask | d.scalarWriteMask) & 0xFu;
            EvalAluStep(d, env, exports, exportTouched, 4u, 0u);
            exportCompMask[slot] |= static_cast<uint8_t>(m);
        } else if (d.exportData && d.vectorDest == 61u) {
            EvalAluStep(d, env, &exports[4], &exportTouched[4], 1u, 61u);
            exportCompMask[4] |= static_cast<uint8_t>((d.vectorWriteMask | d.scalarWriteMask) & 0xFu);
        } else {
            EvalAluStep(d, env, exports, exportTouched, 0u, 0u);
        }
    }

    for (uint32_t s = 0; s < 4u; ++s) {
        if (exportTouched[s]) plan.writesColorMask |= static_cast<uint8_t>(1u << s);
    }
    if ((plan.writesColorMask & ~1u) != 0u) return plan;  // MRT clears not folded

    auto resolvePsLane = [&](const SymLane& l) -> ClearChannelExpr {
        if (l.kind == SymLane::Kind::Interp) {
            if (l.hasUniformScale || l.hasUniformOffset) return ClearChannelExpr{};
            // VS interpolator exports use TEXCOORD<vd>.
            if (l.attrUsage == 5u && l.attrUsageIndex < 16u &&
                (vsPlan.exportedInterpMask & (1u << l.attrUsageIndex)) != 0u) {
                ClearChannelExpr base = vsPlan.interp[l.attrUsageIndex][l.comp];
                if (base.source == ClearChannelSource::Invalid) return ClearChannelExpr{};
                if (base.source == ClearChannelSource::Literal) {
                    base.literal = base.literal * l.scale + l.bias;
                    if (l.saturate) base.literal = std::clamp(base.literal, 0.0f, 1.0f);
                } else {
                    base.scale *= l.scale;
                    base.bias = base.bias * l.scale + l.bias;
                    base.saturate = base.saturate || l.saturate;
                }
                return base;
            }
            return ClearChannelExpr{};
        }
        return LaneToChannelExpr(l, true);
    };

    if (exportTouched[0]) {
        if (exportCompMask[0] != 0xFu) return plan;  // require full .xyzw oC0 write
        plan.writesColor0 = true;
        for (uint32_t c = 0; c < 4u; ++c) {
            plan.oC0[c] = resolvePsLane(exports[0][c]);
            if (plan.oC0[c].source == ClearChannelSource::Invalid) return plan;
        }
    }
    if (exportTouched[4]) {
        plan.writesDepth = true;
        plan.oDepth = resolvePsLane(exports[4][0]);
        if (plan.oDepth.source == ClearChannelSource::Invalid) return plan;
    }
    if (!plan.writesColor0 && !plan.writesDepth) return plan;
    plan.eligible = true;
    return plan;
}

}  // namespace

ClearShaderClassification ClassifyClearRectShaders(
    const ShaderProgram& vsProg,
    const ShaderProgram* psProg,
    const uint8_t* vsContainer, size_t vsContainerSize,
    const uint8_t* psContainer, size_t psContainerSize) {
    ClearShaderClassification res;
    res.vs = AnalyzeClearVs(vsProg, vsContainer, vsContainerSize);
    if (!res.vs.eligible) return res;
    res.ps = AnalyzeClearPs(psProg, res.vs, psContainer, psContainerSize);
    res.eligible = res.vs.eligible && res.ps.eligible;
    return res;
}

}  // namespace mcla::renderer
