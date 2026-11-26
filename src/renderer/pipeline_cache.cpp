#include "diag_threads.h"
#include "pipeline_cache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <atomic>
#include <cstdlib> // F-900: getenv for MCLA_FORCE_BLEND probe
#include <vector>
#include "common/logging.h"
#include "d3d12_backend.h"  // DepthCompareMirrored(): the depth bias sign depends on it
#include "dxc_runtime.h"
#include "texture_decode.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <mutex>
// NOTE: d3d12_backend.h is deliberately NOT included. It is needed for nothing
// here any more, and the offline validator targets (CMakeLists.txt:414-421)
// compile this file without the backend.

namespace mcla::renderer {

// ---------------------------------------------------------------------------
// F-642, assertion 2: native::'s D3D12 sampler table, against the SDK itself.
// ---------------------------------------------------------------------------
// renderer/texture_decode.h restates the D3D12 sampler enums as plain numbers
// so that module stays D3D12-free (its own header says so). That is a good
// property and also the reason the rot went unnoticed: the table's only
// cross-checks lived in d3d12_backend.cpp, six of them behind
// `#ifdef MCLA_ASSERT_TEXTURE_DECODE_TABLE`, so the build never saw them.
//
// This translation unit already includes <d3d12.h> (and is compiled by every
// target that builds this file, so the check cannot be configured away), which
// makes it the place where BOTH sides of the comparison are visible. The
// asserts are therefore unconditional and name the SDK enumerator, not a second
// hand-written copy of the value: if either side drifts, the build fails.
// Two spellings are passed because three of the SDK names carry an extra
// MAG_LINEAR/MAG_POINT word this module's constants do not.
// texture_decode.h additionally asserts the same table against d3d12.h's
// documented ENCODING (min/mag/mip at shifts 4/2/0, reduction at 7) in every
// translation unit that includes it, so the two checks fail independently.
#define MCLA_ASSERT_SDK_FILTER(nativeName, sdkName)                            \
    static_assert(                                                              \
        static_cast<uint32_t>(native::kD3D12Filter##nativeName) ==             \
            static_cast<uint32_t>(D3D12_FILTER_##sdkName),                     \
        "renderer/texture_decode.h kD3D12Filter" #nativeName                    \
        " disagrees with d3d12.h D3D12_FILTER_" #sdkName)
MCLA_ASSERT_SDK_FILTER(MinMagMipPoint, MIN_MAG_MIP_POINT);
MCLA_ASSERT_SDK_FILTER(MinMagPointMipLinear, MIN_MAG_POINT_MIP_LINEAR);
// No D3D12_FILTER_MIN_POINT_MAG_MIP_POINT exists; the SDK spells this mode
// ..._MIN_POINT_MAG_LINEAR_MIP_POINT (0x4) and native:: keeps the older
// mis-spelled name because d3d12_backend.cpp and texture_decode.cpp both use it.
MCLA_ASSERT_SDK_FILTER(MinPointMagMipPoint, MIN_POINT_MAG_LINEAR_MIP_POINT);
MCLA_ASSERT_SDK_FILTER(MinPointMagMipLinear, MIN_POINT_MAG_MIP_LINEAR);
MCLA_ASSERT_SDK_FILTER(MinLinearMagMipPoint, MIN_LINEAR_MAG_MIP_POINT);
MCLA_ASSERT_SDK_FILTER(MinLinearMagMipLinear, MIN_LINEAR_MAG_POINT_MIP_LINEAR);
MCLA_ASSERT_SDK_FILTER(MinMagMipLinear, MIN_MAG_MIP_LINEAR);
MCLA_ASSERT_SDK_FILTER(Anisotropic, ANISOTROPIC);
MCLA_ASSERT_SDK_FILTER(ComparisonMinMagMipPoint, COMPARISON_MIN_MAG_MIP_POINT);
MCLA_ASSERT_SDK_FILTER(ComparisonMinMagPointMipLinear, COMPARISON_MIN_MAG_POINT_MIP_LINEAR);
MCLA_ASSERT_SDK_FILTER(ComparisonMinPointMagMipPoint, COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT);
MCLA_ASSERT_SDK_FILTER(ComparisonMinPointMagMipLinear, COMPARISON_MIN_POINT_MAG_MIP_LINEAR);
MCLA_ASSERT_SDK_FILTER(ComparisonMinLinearMagMipPoint, COMPARISON_MIN_LINEAR_MAG_MIP_POINT);
MCLA_ASSERT_SDK_FILTER(ComparisonMinLinearMagMipLinear, COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR);
MCLA_ASSERT_SDK_FILTER(ComparisonMinMagMipLinear, COMPARISON_MIN_MAG_MIP_LINEAR);
MCLA_ASSERT_SDK_FILTER(ComparisonAnisotropic, COMPARISON_ANISOTROPIC);
#undef MCLA_ASSERT_SDK_FILTER

// static_cast on BOTH sides: native::'s table is an unnamed enum and the SDK's
// is a named one, and comparing two different enumeration types directly is a
// deprecated clang construct that warns in every build.
#define MCLA_ASSERT_SDK_VALUE(constant, sdkName)                               \
    static_assert(static_cast<uint32_t>(constant) ==                           \
                      static_cast<uint32_t>(sdkName),                          \
                  "renderer/texture_decode.h " #constant " disagrees with "    \
                  "d3d12.h " #sdkName)
MCLA_ASSERT_SDK_VALUE(native::kD3D12AddressModeWrap, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
MCLA_ASSERT_SDK_VALUE(native::kD3D12AddressModeMirror, D3D12_TEXTURE_ADDRESS_MODE_MIRROR);
MCLA_ASSERT_SDK_VALUE(native::kD3D12AddressModeClamp, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
MCLA_ASSERT_SDK_VALUE(native::kD3D12AddressModeBorder, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
MCLA_ASSERT_SDK_VALUE(native::kD3D12AddressModeMirrorOnce, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE);
MCLA_ASSERT_SDK_VALUE(native::kD3D12ComparisonFuncNone, D3D12_COMPARISON_FUNC_NONE);
MCLA_ASSERT_SDK_VALUE(native::kD3D12ComparisonFuncNever, D3D12_COMPARISON_FUNC_NEVER);
MCLA_ASSERT_SDK_VALUE(native::kD3D12ComparisonFuncLess, D3D12_COMPARISON_FUNC_LESS);
MCLA_ASSERT_SDK_VALUE(native::kD3D12ComparisonFuncEqual, D3D12_COMPARISON_FUNC_EQUAL);
MCLA_ASSERT_SDK_VALUE(native::kD3D12ComparisonFuncLessEqual, D3D12_COMPARISON_FUNC_LESS_EQUAL);
MCLA_ASSERT_SDK_VALUE(native::kD3D12ComparisonFuncGreater, D3D12_COMPARISON_FUNC_GREATER);
MCLA_ASSERT_SDK_VALUE(native::kD3D12ComparisonFuncNotEqual, D3D12_COMPARISON_FUNC_NOT_EQUAL);
MCLA_ASSERT_SDK_VALUE(native::kD3D12ComparisonFuncGreaterEqual, D3D12_COMPARISON_FUNC_GREATER_EQUAL);
MCLA_ASSERT_SDK_VALUE(native::kD3D12ComparisonFuncAlways, D3D12_COMPARISON_FUNC_ALWAYS);
#undef MCLA_ASSERT_SDK_VALUE

// Simple 64-bit hash combiner (splitmix64-inspired).
static uint64_t CombineHash(uint64_t a, uint64_t b) {
    uint64_t h = a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static uint64_t HashBytes(const void* data, size_t len, uint64_t seed) {
    // FNV-1a over raw bytes. Padding is acceptable for checksums and the legacy
    // raw-state hash only. PipelineKey hashing below is field-wise so equal live
    // and persisted keys always land in the same unordered_map bucket.
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed ^ 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t HashState(const PipelineState& state) {
    uint64_t h = 0;
    for (uint32_t i = 0; i < kMaxRenderTargets; ++i)
        h = CombineHash(h, state.targetFormats[i]);
    h = CombineHash(h, state.depthStencilFormat);
    h = CombineHash(h, state.blendState);
    h = CombineHash(h, state.rasterState);
    h = CombineHash(h, state.depthStencilState);
    h = CombineHash(h, state.topology);
    h = CombineHash(h, state.sampleCount);
    h = CombineHash(h, state.geometryStage);
    h = CombineHash(h, state.derivation);
    h = CombineHash(h, state.flags);
    h = CombineHash(h, state.viewportYFlipped ? 1u : 0u);
    // The D3D12 state the PSO is actually built from must be part of the
    // identity. Hash fields, not struct padding: a persisted key is rebuilt
    // field-wise and must hash identically to the live key. The old raw-struct
    // hash remains an A/B off-switch; it can cause safe cache misses, never a
    // different PSO, because equality is still field-wise.
    static const bool s_legacyRawHash = [] {
        const char* e = std::getenv("MCLA_NO_PSO_STABLE_KEY_HASH");
        return e && e[0] == '1';
    }();
    static const bool s_fullRasterKey = [] {
        const char* e = std::getenv("MCLA_NO_PSO_FULL_RASTER_KEY");
        return !(e && e[0] == '1');
    }();
    if (s_legacyRawHash) {
        h = HashBytes(&state.d3d12.raster, sizeof(state.d3d12.raster), h);
        h = HashBytes(&state.d3d12.blend, sizeof(state.d3d12.blend), h);
        h = HashBytes(&state.d3d12.depthStencil, sizeof(state.d3d12.depthStencil), h);
        return h;
    }
    auto mixFloat = [&h](float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        h = CombineHash(h, bits);
    };
    const D3D12_RASTERIZER_DESC& raster = state.d3d12.raster;
    h = CombineHash(h, static_cast<uint32_t>(raster.FillMode));
    h = CombineHash(h, static_cast<uint32_t>(raster.CullMode));
    h = CombineHash(h, static_cast<uint32_t>(raster.FrontCounterClockwise));
    h = CombineHash(h, static_cast<uint32_t>(raster.DepthBias));
    mixFloat(raster.DepthBiasClamp);
    mixFloat(raster.SlopeScaledDepthBias);
    h = CombineHash(h, static_cast<uint32_t>(raster.DepthClipEnable));
    if (s_fullRasterKey) {
        h = CombineHash(h, static_cast<uint32_t>(raster.MultisampleEnable));
        h = CombineHash(h, static_cast<uint32_t>(raster.AntialiasedLineEnable));
        h = CombineHash(h, raster.ForcedSampleCount);
    }
    h = CombineHash(h, static_cast<uint32_t>(raster.ConservativeRaster));
    const D3D12_BLEND_DESC& blend = state.d3d12.blend;
    h = CombineHash(h, static_cast<uint32_t>(blend.AlphaToCoverageEnable));
    h = CombineHash(h, static_cast<uint32_t>(blend.IndependentBlendEnable));
    for (const auto& target : blend.RenderTarget) {
        h = CombineHash(h, static_cast<uint32_t>(target.BlendEnable));
        h = CombineHash(h, static_cast<uint32_t>(target.LogicOpEnable));
        h = CombineHash(h, static_cast<uint32_t>(target.SrcBlend));
        h = CombineHash(h, static_cast<uint32_t>(target.DestBlend));
        h = CombineHash(h, static_cast<uint32_t>(target.BlendOp));
        h = CombineHash(h, static_cast<uint32_t>(target.SrcBlendAlpha));
        h = CombineHash(h, static_cast<uint32_t>(target.DestBlendAlpha));
        h = CombineHash(h, static_cast<uint32_t>(target.BlendOpAlpha));
        h = CombineHash(h, static_cast<uint32_t>(target.LogicOp));
        h = CombineHash(h, target.RenderTargetWriteMask);
    }
    const D3D12_DEPTH_STENCIL_DESC& depthStencil = state.d3d12.depthStencil;
    h = CombineHash(h, static_cast<uint32_t>(depthStencil.DepthEnable));
    h = CombineHash(h, static_cast<uint32_t>(depthStencil.DepthWriteMask));
    h = CombineHash(h, static_cast<uint32_t>(depthStencil.DepthFunc));
    h = CombineHash(h, static_cast<uint32_t>(depthStencil.StencilEnable));
    h = CombineHash(h, depthStencil.StencilReadMask);
    h = CombineHash(h, depthStencil.StencilWriteMask);
    for (const auto* face : {&depthStencil.FrontFace, &depthStencil.BackFace}) {
        h = CombineHash(h, static_cast<uint32_t>(face->StencilFailOp));
        h = CombineHash(h, static_cast<uint32_t>(face->StencilDepthFailOp));
        h = CombineHash(h, static_cast<uint32_t>(face->StencilPassOp));
        h = CombineHash(h, static_cast<uint32_t>(face->StencilFunc));
    }
    return h;
}

PipelineKey ComputePipelineKey(uint64_t vsHash, uint64_t psHash,
                              uint64_t vsHlslHash, uint64_t psHlslHash,
                              uint64_t vertexDeclHash, const PipelineState& state) {
    PipelineKey key;
    key.vsHash = vsHash;
    key.psHash = psHash;
    key.vsHlslHash = vsHlslHash;
    key.psHlslHash = psHlslHash;
    key.vertexDeclHash = vertexDeclHash;
    key.inputLayoutHash = 0;
    key.state = state;
    // A zero-initialised PipelineState is what a caller that never looked at the
    // packet produces (render_thread.cpp builds one by hand and zeroes the
    // three guest fingerprints). It cannot be told apart from "the guest really
    // wrote zero", so it refuses itself instead of silently keying a PSO whose
    // blend/cull/depth nobody derived.
    if (state.derivation == kStateDerivedNone) key.refuseMask |= kRefuseStateNotDerived;
    if (state.sampleCount == 0) key.refuseMask |= kRefuseStateNotDerived;
    return key;
}

PipelineKey ComputePipelineKeyForLayout(
        uint64_t vsHash, uint64_t psHash,
        uint64_t vsHlslHash, uint64_t psHlslHash,
        uint64_t vertexDeclHash,
        const PipelineState& state,
        const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout) {
    PipelineKey key = ComputePipelineKey(vsHash, psHash, vsHlslHash, psHlslHash, vertexDeclHash, state);
    key.inputLayoutHash = HashInputLayout(inputLayout);
    if (inputLayout.empty()) key.refuseMask |= kRefuseEmptyInputLayout;
    return key;
}

uint64_t HashPipelineKey(const PipelineKey& key) {
    uint64_t h = CombineHash(key.vsHash, key.psHash);
    h = CombineHash(h, key.vsHlslHash);
    h = CombineHash(h, key.psHlslHash);
    h = CombineHash(h, key.vertexDeclHash);
    h = CombineHash(h, key.inputLayoutHash);
    h = CombineHash(h, HashState(key.state));
    h = CombineHash(h, key.refuseMask);
    return h;
}

std::size_t PipelineKeyHash::operator()(const PipelineKey& k) const noexcept {
    return static_cast<std::size_t>(HashPipelineKey(k));
}

// Stable 64-bit hash of shader bytecode (FNV-1a 64-bit).
static uint64_t Fnv1a64(const uint8_t* data, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

uint64_t HashShaderBytecode(const std::vector<uint8_t>& bytecode) {
    return Fnv1a64(bytecode.data(), bytecode.size());
}

uint64_t HashShaderBytecode(const std::string& hlsl) {
    // Hashed twice per draw (render_thread derive-pso): 8 bytes per step instead of FNV's 1.
    // In-memory PSO key only, never persisted, so the value change is safe.
    const char* p = hlsl.data();
    size_t n = hlsl.size();
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    return n ? (h ^ Fnv1a64(reinterpret_cast<const uint8_t*>(p), n)) * 0x100000001b3ull : h;
}

// ---------------------------------------------------------------------------
// Refusal text
// ---------------------------------------------------------------------------
void FormatPipelineRefusal(uint32_t mask, char* out, size_t outBytes) {
    if (!out || outBytes == 0) return;
    out[0] = '\0';
    struct Entry { uint32_t bit; const char* name; };
    static constexpr Entry kNames[] = {
        {kRefuseStateNotDerived,        "state-not-derived"},
        {kRefuseEmptyHlsl,              "empty-hlsl"},
        {kRefuseNoDevice,               "no-device"},
        {kRefuseNoRootSignature,        "no-root-signature"},
        {kRefuseEmptyInputLayout,       "empty-input-layout"},
        {kRefuseLayoutDeclHashMismatch, "layout-hash-mismatch"},
        {kRefuseUnsupportedPolyMode,    "unsupported-poly-mode"},
        {kRefuseTopologyUnknown,        "unsupported-topology"},
        {kRefuseOverTextureSlots,       "srv-outside-t0-t3"},
        {kRefuseSamplerRegister,        "sampler-not-s0"},
        {kRefuseRequiredDepthState,     "unmeasured-depth-state-required"},
        {kRefuseQueueSaturated,         "compile-queue-saturated"},
        // F-646: one name per missing guest register, carrying the Xenos index so
        // a grep finds the register file the producer has to wire.
        {kRefuseNoPaSuScModeCntl,       "no-0x2205-pa-su-sc-mode-cntl"},
        {kRefuseNoPaClipCntl,           "no-0x2204-pa-cl-clip-cntl"},
        {kRefuseNoPaClVteCntl,          "no-0x2206-pa-cl-vte-cntl"},
        {kRefuseViewportExtentsUnmeasured, "viewport-extents-unmeasured"},
        // D1: named by the register indices that are actually absent. The old
        // PC-POLYOFF line named RB_DEPTHF_INFO, which is not a register at all.
        {kRefuseNoPolyOffsetFront,  "no-0x2380-0x2381-pa-su-poly-offset-front"},
        {kRefuseNoPolyOffsetBack,   "no-0x2382-0x2383-pa-su-poly-offset-back"},
        {kRefuseNoDepthInfo,        "no-0x2002-rb-depth-info-depth-format"},
        // F-653: the rectangle-list geometry stage. Two names, never merged,
        // because "the device cannot do it" and "the stage was not derivable
        // from this program's VS" have nothing in common as follow-up work.
        {kRefuseGeometryStageUnsupported, "geom-stage-unsupported-at-this-feature-level"},
        {kRefuseGeometryShaderUnbuildable, "geom-stage-not-derivable-from-vs-signature"},
        // F-692/F-716: the name the PSO-creation failures were missing. It used
        // to print "empty-hlsl" for a cause with nothing to do with HLSL.
        {kRefusePsoCreateFailed,       "pso-create-failed"},
        // F-904: constant blend factor requested. OMSetBlendFactor is per-draw
        // dynamic state the backend never sets, so name it rather than baking
        // in a (1,1,1,1) factor.
        {kRefuseBlendConstantFactor,   "blend-constant-factor"},
    };
    // Every bit in use must have a name here, or FormatPipelineRefusal renders
    // "unnamed-0x..." and a soak grepping for a reason name finds none -- which
    // is the failure the guard below exists to make impossible.
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == kPipelineRefusalBitCount,
                  "kNames has one entry per PipelineRefusal bit, no more and no fewer");
    size_t used = 0;
    // D1 FIX: this guard used to sit HERE, before the loop, testing a `used`
    // that is unconditionally 0 at this point -- so it fired for every non-zero
    // mask and the loop below was dead. Every refusal in this file rendered as
    // "unnamed-0x%08X" on stderr and nothing else, which is fatal for the
    // project contract: a refusal's NAME is the only thing that says WHICH
    // register was absent, and a soak grepping for a reason name would have
    // found none. It has to test `used` AFTER the loop, where "the loop matched
    // nothing" is the condition it was written for. Found by a negative test
    // that asserted kRefuseNoPolyOffsetFront renders as a name.
    for (const auto& e : kNames) {
        if (!(mask & e.bit)) continue;
        const size_t n = std::strlen(e.name);
        const size_t need = used ? n + 1 : n;
        if (used + need + 1 >= outBytes) break;
        if (used) out[used++] = '|';
        std::memcpy(out + used, e.name, n);
        used += n;
        out[used] = '\0';
    }
    // No entry above matched: a bit with no name in kNames. Render something
    // greppable rather than an empty string a soak would read as "none"
    // (F-646's original intent, which the misplaced guard above defeated).
    if (used == 0 && mask != kRefuseNone) {
        std::snprintf(out, outBytes, "unnamed-0x%08X", mask);
        return;
    }
    if (used == 0) std::strncpy(out, "none", outBytes - 1);
}

// ---------------------------------------------------------------------------
// State derivation
// ---------------------------------------------------------------------------
namespace {

// --- D1: the Xenos absolute depth bias -> D3D12 relative DepthBias ----------
// A straight transcription of
// `.research/xenia/src/xenia/gpu/draw_util.h:206-225`
// (`GetD3D10IntegerPolygonOffset`), which is the only place the reference
// performs this conversion. Kept as host arithmetic on host floats: no guest
// pointer is read and no guest value is asserted on.
//
// The guest writes an ABSOLUTE floating-point offset (draw_util.h:118-119);
// D3D12 wants a RELATIVE one (draw_util.h:128-131), so the absolute value is
// multiplied by the depth format's minimum resolvable difference:
//   * fixed-point (kD24S8, the reference's default) --
//     kD3D10PolygonOffsetFactorUnorm24 = 2^24 - 1, draw_util.h:171-172;
//   * float24 (kD24FS8) -- kD3D10PolygonOffsetFactorFloat24 = 2^24,
//     draw_util.h:203-204, and then `<<= 3` because one float24 ULP is eight
//     float32 ULPs (draw_util.h:177-181, :221-223).
// `ceil` is the reference's choice and the reason is load-bearing: "more offset
// is better, especially if flooring would result in 0" (draw_util.h:209-211).
constexpr float kD3D10PolygonOffsetFactorUnorm24 =
    static_cast<float>((UINT32_C(1) << 24) - 1);            // draw_util.h:171-172
constexpr float kD3D10PolygonOffsetFactorFloat24 =
    static_cast<float>(UINT32_C(1) << (21 + 3));            // draw_util.h:203-204
// xenos.h:407 -- the slope scale's unit is 1/16 of a subpixel. Multiplied in at
// d3d12/pipeline_cache.cc:1502-1503, i.e. AFTER the selection, never before.
constexpr float kPolygonOffsetScaleSubpixelUnit = 1.0f / 16.0f;  // xenos.h:407

// `isFloat24` is RB_DEPTH_INFO.depth_format == kD24FS8 (xenos.h:367-371,
// registers.h:844). A format value outside {0,1} is impossible: the field is
// one bit, so this needs no range check and adds none.
int32_t D3D10IntegerPolygonOffset(bool isFloat24, float polygonOffset) {
    const float factor = isFloat24
            ? kD3D10PolygonOffsetFactorFloat24 * (1.0f / 8.0f)
            : kD3D10PolygonOffsetFactorUnorm24;
    int32_t v = static_cast<int32_t>(std::ceil(std::abs(polygonOffset) * factor));
    if (isFloat24) v <<= 3;  // draw_util.h:221-223
    return polygonOffset < 0 ? -v : v;  // draw_util.h:224
}

// Guest register bit fields, from the local Xenia checkout (never guessed):
//   PA_SU_SC_MODE_CNTL  .research/xenia/src/xenia/gpu/registers.h:456-489
//   PA_CL_CLIP_CNTL     .research/xenia/src/xenia/gpu/registers.h:534-563
//   PA_CL_VTE_CNTL      .research/xenia/src/xenia/gpu/registers.h:566-584
//   PolygonModeEnable / PolygonType  xenos.h:846-859
//   cull/fill lowering  d3d12/pipeline_cache.cc:1473-1490,
//                       vulkan/vulkan_pipeline_cache.cc:588-621
//   depth_clip          d3d12/pipeline_cache.cc:1508
//
// D2: bits 17-31 used to collapse into `rest : 15`. The 15 was ARITHMETICALLY
// CORRECT for the size (17 named + 15 = 32, which is what F-634 needed after a
// 33-bit field broke the assert) and CONTENT-WRONG for the register: the
// reference names five of those bits, and one of them -- provoking_vtx_last --
// changes which vertex a primitive's provoking attribute comes from, i.e. it
// changes the rasterised image. Naming them changes nothing behaviourally TODAY
// (nothing reads them yet), which is exactly why it had to be done
// deliberately: an unnamed bit is a bit nobody is obliged to check, and this is
// the register the whole rasteriser decodes out of. Each field carries its own
// reference DECLARATION line, per docs/ref/xenos_registers.md §0: declLine is
// the declaration, not the comment above it (registers.h:461 is the comment for
// `face`; registers.h:462 is its declaration).
struct SuScModeCntl {
    uint32_t cullFront       : 1;  // registers.h:459  bit 0
    uint32_t cullBack        : 1;  // registers.h:460  bit 1
    uint32_t face            : 1;  // registers.h:462  bit 2 (0: CCW, 1: CW)
    uint32_t polyMode        : 2;  // registers.h:467  bits 3-4
    uint32_t polyFrontType   : 3;  // registers.h:468  bits 5-7
    uint32_t polyBackType    : 3;  // registers.h:469  bits 8-10
    uint32_t polyOffFrontEn  : 1;  // registers.h:470  bit 11
    uint32_t polyOffBackEn   : 1;  // registers.h:471  bit 12
    uint32_t polyOffParaEn   : 1;  // registers.h:472  bit 13
    uint32_t _pad14          : 1;  // registers.h:473  bit 14
    uint32_t msaaEnable      : 1;  // registers.h:474  bit 15
    uint32_t vtxWindowOffset : 1;  // registers.h:475  bit 16
    uint32_t _pad17          : 2;  // registers.h:477  bits 17-18
    // --- D2: the five fields the reference names; all five were inside `rest` ---
    uint32_t provokingVtxLast  : 1;  // registers.h:478  bit 19
    uint32_t perspCorrDis      : 1;  // registers.h:479  bit 20
    uint32_t multiPrimIbEna    : 1;  // registers.h:480  bit 21 (VGT_MULTI_PRIM_
                                    //  IB_RESET_INDX exists, registers.h:343)
    uint32_t _pad22            : 1;  // registers.h:481  bit 22
    uint32_t quadOrderEnable   : 1;  // registers.h:482  bit 23
    uint32_t scOneQuadPerClock : 1;  // registers.h:483  bit 24
    uint32_t _pad25            : 7;  // registers.h:486  bits 25-31
    // Widths: 1+1+1+2+3+3+1+1+1+1+1+1 = 17 (bits 0-16); +2 (17-18) = 19;
    // +1+1+1 = 22 (bits 19-21); +1 (22) = 23; +1+1 = 25 (bits 23-24);
    // +7 = 32 (bits 25-31). The unconditional static_assert below is what holds
    // that arithmetic; it is not macro-guarded and must never become so (see the
    // note there on how a 33-bit field survived review behind this register).
};
// PA_CL_CLIP_CNTL (.research/xenia/src/xenia/gpu/registers.h:534-563).
// ucp_ena_0..5 +0..+5, _pad_6:8 +6..+13, ps_ucp_mode +14, clip_disable +16,
// ucp_cull_only_ena +17, boundary_edge_flag_ena +18, dx_clip_space_def +19,
// dis_clip_err_detect +20, vtx_kill_or +21, xy_nan_retain +22, z_nan_retain +23,
// w_nan_retain +24, _pad_25:7 +25..+31.
struct ClipCntl {
    uint32_t ucpEna0 : 1;
    uint32_t ucpEna1 : 1;
    uint32_t ucpEna2 : 1;
    uint32_t ucpEna3 : 1;
    uint32_t ucpEna4 : 1;
    uint32_t ucpEna5 : 1;
    uint32_t pad6    : 8;
    uint32_t psUcpMode : 2;
    uint32_t clipDisable : 1;  // +16
    uint32_t ucpCullOnly : 1;
    uint32_t boundaryEdge : 1;
    uint32_t dxClipSpaceDef : 1;
    uint32_t disClipErrDetect : 1;
    uint32_t vtxKillOr : 1;
    uint32_t xyNanRetain : 1;
    uint32_t zNanRetain : 1;
    uint32_t wNanRetain : 1;
    uint32_t pad25 : 7;
};

// PA_CL_VTE_CNTL. F-646: this struct DID NOT EXIST. The header listed
// paClVteCntl as an input and DerivePipelineState stored it as
// `depthStencilState`, but no overlay ever decoded it, so the viewport state was
// a fingerprint and nothing else. Layout from
// .research/xenia/src/xenia/gpu/registers.h:566-584, field by field:
//
//   vport_x_scale_ena  +0   vport_y_scale_ena  +2   vport_z_scale_ena  +4
//   vport_x_offset_ena +1   vport_y_offset_ena +3   vport_z_offset_ena +5
//   _pad_6 :2            +6..+7
//   vtx_xy_fmt  +8   vtx_z_fmt +9   vtx_w0_fmt +10   perfcounter_ref +11
//   _pad_12 :20         +12..31
struct ClVteCntl {
    uint32_t vportXScaleEn  : 1;  // +0
    uint32_t vportXOffsetEn : 1;  // +1
    uint32_t vportYScaleEn  : 1;  // +2
    uint32_t vportYOffsetEn : 1;  // +3
    uint32_t vportZScaleEn  : 1;  // +4
    uint32_t vportZOffsetEn : 1;  // +5
    uint32_t pad6           : 2;  // +6..7
    uint32_t vtxXyFmt       : 1;  // +8
    uint32_t vtxZFmt        : 1;  // +9
    uint32_t vtxW0Fmt       : 1;  // +10
    uint32_t perfcounterRef : 1;  // +11
    uint32_t rest           : 20; // +12..31
};

// F-646: all three size asserts are UNCONDITIONAL, kept in one contiguous block
// after the third overlay so no reader can believe any of them is macro-guarded.
// (The "asserts hidden behind a macro that was never defined" defect in this
// file was the TEXTURE FILTER block at the top, which F-642 already fixed; these
// three were never affected -- they were already unconditional -- but the group
// is deliberately kept together.) Each names the register it protects. A macro
// guard here is precisely how a 33-bit bitfield in a one-dword struct survives
// review, so the guard is not merely absent -- it is called out.
// D2 (width half): the five newly named fields add 1+1+1+1+1 = 5 bits, taken
// out of the old `rest : 15` -> `rest` is now `_pad25 : 7`, and the two padding
// runs 17-18 and 22 are named too. The total is still 32, so the assert above
// still holds -- which is the point: the assert was never the thing that was
// wrong. It held a register that was five fields short.
static_assert(sizeof(SuScModeCntl) == sizeof(uint32_t),
              "PA_SU_SC_MODE_CNTL overlay must stay exactly one dword "
              "(D2: 17 named + 2 (_pad17) + 3 named + 1 (_pad22) + 2 named + "
              "7 (_pad25) = 32)");
static_assert(sizeof(ClipCntl) == sizeof(uint32_t),
              "PA_CL_CLIP_CNTL overlay must stay exactly one dword");
static_assert(sizeof(ClVteCntl) == sizeof(uint32_t),
              "PA_CL_VTE_CNTL overlay must stay exactly one dword");

// F-646: the size assert is the COMPILE-TIME half of the guard. It is
// unconditional and it is what catches the specific rot class the previous audit
// found (a 33-bit bitfield in a struct asserted to be one dword): a width typo
// that pushes the total past 32 fails here at build time, in every target.
//
// It is NOT possible to make the bit POSITIONS compile-time checked, and that
// was measured rather than assumed, on the toolchain this project builds with
// (LLVM clang-cl, `-clang:-std=c++23`):
//   * `std::memcpy` into a bitfield struct -- "cannot constant evaluate
//     'memcpy' from object of type 'uint32_t' to object of type 'SuScModeCntl'".
//   * `__builtin_bit_cast(SuScModeCntl, raw)` -- "constexpr bit_cast involving
//     bit-field is not yet supported".
//   * a `union { uint32_t raw; SuScModeCntl s; }` pun -- "read of member of union
//     with active member 'raw' is not allowed in a constant expression".
// All three were tried; all three are rejected, so a `static_assert` over the
// positions is not available here. VerifyXenosRenderStateLayouts below is the
// runtime half and it is REAL -- do not delete it on the grounds that a
// static_assert would be nicer. The compile-time cross-check against the actual
// Xenia header needs an `#include "xenia/gpu/registers.h"`, which requires a
// `-I` for .research/xenia/src that none of the five targets carry (a
// CMakeLists.txt change this file does not own); the paste-ready version is
// reported instead of being faked here.
struct F646LayoutProbe {
    uint32_t raw;
    uint32_t expectCullFront, expectCullBack, expectFace, expectPolyMode;
    uint32_t expectPolyFrontType, expectPolyBackType, expectMsaa;
    uint32_t expectClipDisable;
    uint32_t expectVportXScaleEn, expectVtxXyFmt, expectVtxZFmt;
};
// Measured in build/w658.log (CP-DRAW-REGS, the guest's own draws):
//   r2204=0x00010000  -> bit 16 only  = clip_disable
//   r2205=0x00010000  -> bit 16 only  = vtx_window_offset_enable
//   r2206=0x00000300  -> bits 8,9     = vtx_xy_fmt, vtx_z_fmt
constexpr F646LayoutProbe kF646Measured = {
    /*raw*/ 0x00010000u,
    /*expectCullFront*/ 0u, /*expectCullBack*/ 0u, /*expectFace*/ 0u,
    /*expectPolyMode*/ 0u, /*expectPolyFrontType*/ 0u, /*expectPolyBackType*/ 0u,
    /*expectMsaa*/ 0u,
    /*expectClipDisable*/ 1u,  // bit 16, registers.h:547
    /*expectVportXScaleEn*/ 0u, /*expectVtxXyFmt*/ 0u, /*expectVtxZFmt*/ 0u,
};
// The viewport value differs from the other two, so it cannot share .raw; it is
// built from the documented shifts (registers.h:569-578) rather than from a
// second copy of the struct, which is the whole point of the check.
constexpr uint32_t kF646VteRaw = (1u << 8) | (1u << 9);  // vtx_xy_fmt + vtx_z_fmt

// --- D2: the probe values for bits 17-31 of PA_SU_SC_MODE_CNTL --------------
// The measured guest value (bit 16 only) says NOTHING about bits 17-31, so
// naming five fields there and never exercising them would be the same defect
// one level down: correct names, no evidence.
//
// ONE RAW VALUE PER FIELD, and that is measured, not stylistic. The first
// version of this probe set all five bits at once (17,18,19,20,21,23,24) and
// asserted all five fields read 1. That check PASSES on a struct where
// quad_order_enable and sc_one_quad_per_clock are SWAPPED, because both
// adjacent bits are set either way -- a negative test caught exactly that.
// Probing each bit ALONE, and requiring the other four named fields to read 0,
// is the only form that makes the names load-bearing. Each raw is one bit, so
// a mis-declared position, a wrong width, or a field absorbed into a pad all
// move a value and fail.
constexpr uint32_t kD2Pad17Raw  = (1u << 17) | (1u << 18);  // _pad_17, 2 bits
constexpr uint32_t kD2ProvokingVtxLastRaw = (1u << 19);
constexpr uint32_t kD2PerspCorrDisRaw     = (1u << 20);
constexpr uint32_t kD2MultiPrimIbEnaRaw   = (1u << 21);
constexpr uint32_t kD2Pad22Raw            = (1u << 22);
constexpr uint32_t kD2QuadOrderEnableRaw  = (1u << 23);
constexpr uint32_t kD2ScOneQuadPerClockRaw = (1u << 24);
constexpr uint32_t kD2Pad25Raw            = (1u << 25);

// Every field of the 17..31 run, read out of the overlay. Compared as a whole
// against an 8-tuple, so a probe cannot pass by matching only the field it was
// written for.
struct D2TailValues {
    uint32_t pad17, provokingVtxLast, perspCorrDis, multiPrimIbEna;
    uint32_t pad22, quadOrderEnable, scOneQuadPerClock, pad25;
};
static D2TailValues ReadSuScModeCntlTail(uint32_t raw) {
    SuScModeCntl s{};
    std::memcpy(&s, &raw, sizeof(s));
    return D2TailValues{s._pad17,  s.provokingVtxLast, s.perspCorrDis,
                        s.multiPrimIbEna, s._pad22,     s.quadOrderEnable,
                        s.scOneQuadPerClock, s._pad25};
}

}  // namespace

bool VerifyXenosRenderStateLayouts() {
    SuScModeCntl su{};
    std::memcpy(&su, &kF646Measured.raw, sizeof(su));
    if (su.cullFront != kF646Measured.expectCullFront) return false;
    if (su.cullBack != kF646Measured.expectCullBack) return false;
    if (su.face != kF646Measured.expectFace) return false;
    if (su.polyMode != kF646Measured.expectPolyMode) return false;
    if (su.polyFrontType != kF646Measured.expectPolyFrontType) return false;
    if (su.polyBackType != kF646Measured.expectPolyBackType) return false;
    if (su.msaaEnable != kF646Measured.expectMsaa) return false;
    // The 0x00010000 in PA_SU_SC_MODE_CNTL is bit 16 = vtx_window_offset_enable,
    // and must NOT land in any other named field. That is the discriminating
    // check: a one-bit value alone cannot tell 16 fields apart.
    if (su.vtxWindowOffset != 1u) return false;

    // --- D2: bits 17-31, the five fields the reference names ---------------
    // Eight raws, one per field, each compared against the WHOLE 8-tuple. The
    // bit positions are registers.h:477-486; the expected values are that same
    // table read literally, so a probe and a struct that disagree cannot both
    // be right. Reference cross-check on the use side: provoking_vtx_last has
    // no lowering in the reference at all (it is listed with no cross-check
    // column entry in docs/ref/xenos_registers.md §2.1), which is why these are
    // decoded for the log and not mapped.
    struct TailCase { uint32_t raw; D2TailValues expect; };
    static constexpr TailCase kD2Cases[] = {
        // _pad_17 is TWO bits (registers.h:477), so its probe sets both.
        {kD2Pad17Raw,             {3u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}},
        {kD2ProvokingVtxLastRaw,  {0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u}},
        {kD2PerspCorrDisRaw,      {0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u}},
        {kD2MultiPrimIbEnaRaw,    {0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u}},
        {kD2Pad22Raw,             {0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u}},
        {kD2QuadOrderEnableRaw,   {0u, 0u, 0u, 0u, 0u, 1u, 0u, 0u}},
        {kD2ScOneQuadPerClockRaw, {0u, 0u, 0u, 0u, 0u, 0u, 1u, 0u}},
        // _pad_25 starts at bit 25 (registers.h:486). This is the case that
        // fails if sc_one_quad_per_clock is ever widened, and it is the case
        // that would fail if the D2 fix were reverted to `rest : 15` and the
        // five names silently re-disappeared into it.
        {kD2Pad25Raw,             {0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u}},
    };
    for (const TailCase& c : kD2Cases) {
        const D2TailValues got = ReadSuScModeCntlTail(c.raw);
        if (got.pad17 != c.expect.pad17 ||
            got.provokingVtxLast != c.expect.provokingVtxLast ||
            got.perspCorrDis != c.expect.perspCorrDis ||
            got.multiPrimIbEna != c.expect.multiPrimIbEna ||
            got.pad22 != c.expect.pad22 ||
            got.quadOrderEnable != c.expect.quadOrderEnable ||
            got.scOneQuadPerClock != c.expect.scOneQuadPerClock ||
            got.pad25 != c.expect.pad25)
            return false;
    }
    // The all-set case as well, so a field declared NARROWER than the reference
    // (spilling into its neighbour) is caught: bits 17-21 and 23-24 all set,
    // bit 22 clear.
    {
        constexpr uint32_t kD2AllNamedRaw = (1u << 17) | (1u << 18) | (1u << 19) |
                                            (1u << 20) | (1u << 21) | (1u << 23) |
                                            (1u << 24);
        const D2TailValues got = ReadSuScModeCntlTail(kD2AllNamedRaw);
        if (got.pad17 != 3u || got.provokingVtxLast != 1u || got.perspCorrDis != 1u ||
            got.multiPrimIbEna != 1u || got.pad22 != 0u || got.quadOrderEnable != 1u ||
            got.scOneQuadPerClock != 1u || got.pad25 != 0u)
            return false;
    }

    ClipCntl cl{};
    std::memcpy(&cl, &kF646Measured.raw, sizeof(cl));
    if (cl.clipDisable != kF646Measured.expectClipDisable) return false;
    if (cl.ucpEna0 || cl.ucpEna1 || cl.ucpEna2 || cl.ucpEna3 || cl.ucpEna4 ||
        cl.ucpEna5 || cl.psUcpMode || cl.ucpCullOnly || cl.boundaryEdge ||
        cl.dxClipSpaceDef || cl.disClipErrDetect || cl.vtxKillOr ||
        cl.xyNanRetain || cl.zNanRetain || cl.wNanRetain)
        return false;

    ClVteCntl vte{};
    std::memcpy(&vte, &kF646VteRaw, sizeof(vte));
    if (vte.vtxXyFmt != 1u) return false;
    if (vte.vtxZFmt != 1u) return false;
    if (vte.vtxW0Fmt != 0u) return false;
    if (vte.vportXScaleEn != 0u) return false;
    if (vte.vportYScaleEn != 0u) return false;
    if (vte.vportZScaleEn != 0u) return false;
    if (vte.vportXOffsetEn != 0u) return false;
    if (vte.vportYOffsetEn != 0u) return false;
    if (vte.vportZOffsetEn != 0u) return false;
    if (vte.perfcounterRef != 0u) return false;
    return true;
}

// The declared baseline D3D12 state: solid fill, no culling, depth clip on,
// blending off, depth/stencil off. Applied by both derivation entry points so a
// zeroed PipelineStateD3D12 can never reach CreateGraphicsPipelineState -- a
// zeroed D3D12_RASTERIZER_DESC has DepthClipEnable == FALSE, which clips
// everything away, and D3D12_BLEND_DESC has no implicit defaults at all.
static void ApplyBaselineD3D12State(PipelineState& s) {
    D3D12_RASTERIZER_DESC& r = s.d3d12.raster;
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = D3D12_CULL_MODE_NONE;
    r.DepthClipEnable = TRUE;
    r.FrontCounterClockwise = TRUE;
    // F-634: no `DepthBiasEnable` field exists on D3D12_RASTERIZER_DESC --
    // depth bias is the three FLOATs set on the next three lines, and the only
    // BOOL near it is `DepthClipEnable` (set above). The invented assignment
    // broke the build; the real bias state is already zeroed below.
    r.DepthBias = 0;
    r.DepthBiasClamp = 0.0f;
    r.SlopeScaledDepthBias = 0.0f;
    // F-634: `ConservativeRaster` is a D3D12_CONSERVATIVE_RASTERIZATION_MODE,
    // not a BOOL -- assigning FALSE (int) is a type error.
    r.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    r.AntialiasedLineEnable = FALSE;
    r.MultisampleEnable = (s.sampleCount > 1) ? TRUE : FALSE;

    D3D12_BLEND_DESC& b = s.d3d12.blend;
    b.AlphaToCoverageEnable = FALSE;
    b.IndependentBlendEnable = FALSE;
    // F-634: `D3D12_COLOR_ATTACHMENT_COUNT` does not exist. The SDK note
    // beside `RenderTarget[8]` names the real one: D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT.
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        b.RenderTarget[i].BlendEnable = FALSE;
        b.RenderTarget[i].LogicOpEnable = FALSE;
        b.RenderTarget[i].SrcBlend = D3D12_BLEND_ONE;
        b.RenderTarget[i].DestBlend = D3D12_BLEND_ZERO;
        b.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD;
        b.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ONE;
        b.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
        b.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
        b.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    // NOTE (F-904): MCLA_FORCE_BLEND lives at the end of DerivePipelineState
    // now, overriding the DECODED state. It used to sit here, where any real
    // decode (which runs later) would have silently discarded it.

    D3D12_DEPTH_STENCIL_DESC& d = s.d3d12.depthStencil;
    d.DepthEnable = FALSE;
    d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d.StencilEnable = FALSE;
    d.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    d.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    d.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    d.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    d.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    d.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d.BackFace = d.FrontFace;
}

// kCountedPrimTypes covers the whole architected Xenos prim_type space that is
// NOT the explicit-major-mode range: 0x00..0x0F, i.e. every value up to and
// including kPolygon, and 0x10 (k2DCopyRectListV0 / kExplicitMajorModeForceStart)
// is the first value that belongs to the 2D/tessellation families. One extra
// slot catches everything above that so the counter never writes out of bounds.
static constexpr uint32_t kCountedPrimTypes = 17;  // 0x00..0x10 + "above"

// The architected name for a Xenos prim_type, read off
// .research/xenia/src/xenia/gpu/xenos.h `enum class PrimitiveType` (kNone=0x00
// .. kPolygon=0x0F, then kExplicitMajorModeForceStart/k2DCopyRectListV0=0x10).
// A refusal that prints "0x04" is a number; one that prints "kTriangleList" is
// something a soak can be grepped for and a reader can check against the header.
static const char* XenosPrimTypeName(uint32_t primType) {
    switch (primType) {
        case 0x00: return "kNone";
        case 0x01: return "kPointList";
        case 0x02: return "kLineList";
        case 0x03: return "kLineStrip";
        case 0x04: return "kTriangleList";
        case 0x05: return "kTriangleFan";
        case 0x06: return "kTriangleStrip";
        case 0x07: return "kTriangleWithWFlags";
        case 0x08: return "kRectangleList";
        case 0x0C: return "kLineLoop";
        case 0x0D: return "kQuadList";
        case 0x0E: return "kQuadStrip";
        case 0x0F: return "kPolygon";
        default:   return primType < 0x10 ? "kUndefined" : "k2DOrTessellation";
    }
}

// F-646: one counted line per register that did not arrive, so the refusal census
// answers "which register" without any reader decoding a state value. House
// pattern n <= K || (n % 200) == 0, and the counter is PER REGISTER so a soak
// can attribute a non-zero count to one missing register (F-054: a soak dominated
// by one repeating message is POISONED, so the counters must not be shared).
namespace {
void CountMissingRegister(const char* regName, const char* regIndex) {
    static std::atomic<uint32_t> s_missing{0};
    const uint32_t n = s_missing.fetch_add(1) + 1;
    if (n <= 4 || (n % 200) == 0)
        MCLA_LOG_WARN("PC-REG-MISSING #{} {} (0x{}) absent from the draw packet; "
                      "PSO refused, raster state NOT defaulted to zero",
                      n, regName, regIndex);
}
}  // namespace

DecodedXenosRenderState DecodeGuestRenderState(const PipelineStateInputs& in,
                                                uint32_t& refuseMask) {
    DecodedXenosRenderState d;
    // A missing register is REFUSED and counted by name; it is never decoded as
    // zero. The reason this is not cosmetic: 0x00000000 is a legal value for all
    // three registers and means something specific and different from "absent"
    // (see the GuestRegisterPresent comment in pipeline_cache.h).
    if (!(in.registersPresent & kGuestRegPaSuScModeCntl)) {
        refuseMask |= kRefuseNoPaSuScModeCntl;
        CountMissingRegister("PA_SU_SC_MODE_CNTL", "2205");
    } else {
        SuScModeCntl su;
        std::memcpy(&su, &in.paSuScModeCntl, sizeof(su));
        d.suPresent   = true;
        d.cullFront   = su.cullFront != 0u;
        d.cullBack    = su.cullBack != 0u;
        d.frontIsCw   = su.face != 0u;
        d.polyMode    = su.polyMode;
        d.polyFrontType = su.polyFrontType;
        d.polyBackType  = su.polyBackType;
        d.polyOffsetFrontEn = su.polyOffFrontEn != 0u;
        d.polyOffsetBackEn  = su.polyOffBackEn != 0u;
        d.polyOffsetParaEn  = su.polyOffParaEn != 0u;
        d.msaaEnable  = su.msaaEnable != 0u;
        d.vtxWindowOffsetEn = su.vtxWindowOffset != 0u;
        // D2: the five fields the reference names (registers.h:478-483). Decoded
        // but not lowered: none of them has a D3D12_RASTERIZER_DESC equivalent
        // this module can apply without a measurement it does not have, and
        // inventing one is exactly the degradation the charter forbids.
        d.provokingVtxLast  = su.provokingVtxLast != 0u;
        d.perspCorrDis      = su.perspCorrDis != 0u;
        d.multiPrimIbEna    = su.multiPrimIbEna != 0u;
        d.quadOrderEnable   = su.quadOrderEnable != 0u;
        d.scOneQuadPerClock = su.scOneQuadPerClock != 0u;
        // D1: bits 11-13 are the REQUEST. Whether the magnitude exists is a
        // separate question answered by registersPresent, because the magnitudes
        // are four other registers entirely.
        d.polyOffsetRequested = su.polyOffFrontEn != 0u || su.polyOffBackEn != 0u ||
                                su.polyOffParaEn != 0u;
    }

    if (!(in.registersPresent & kGuestRegPaClipCntl)) {
        refuseMask |= kRefuseNoPaClipCntl;
        CountMissingRegister("PA_CL_CLIP_CNTL", "2204");
    } else {
        ClipCntl cl;
        std::memcpy(&cl, &in.paClipCntl, sizeof(cl));
        d.clPresent   = true;
        d.ucpEnaMask  = (cl.ucpEna0 ? 1u : 0u) | (cl.ucpEna1 ? 2u : 0u) |
                        (cl.ucpEna2 ? 4u : 0u) | (cl.ucpEna3 ? 8u : 0u) |
                        (cl.ucpEna4 ? 16u : 0u) | (cl.ucpEna5 ? 32u : 0u);
        d.psUcpMode   = cl.psUcpMode;
        d.clipDisable = cl.clipDisable != 0u;
        d.ucpCullOnly = cl.ucpCullOnly != 0u;
    }

    if (!(in.registersPresent & kGuestRegPaClVteCntl)) {
        refuseMask |= kRefuseNoPaClVteCntl;
        CountMissingRegister("PA_CL_VTE_CNTL", "2206");
    } else {
        ClVteCntl vte;
        std::memcpy(&vte, &in.paClVteCntl, sizeof(vte));
        d.vtePresent   = true;
        d.vportXScaleEn  = vte.vportXScaleEn  != 0u;
        d.vportXOffsetEn = vte.vportXOffsetEn != 0u;
        d.vportYScaleEn  = vte.vportYScaleEn  != 0u;
        d.vportYOffsetEn = vte.vportYOffsetEn != 0u;
        d.vportZScaleEn  = vte.vportZScaleEn  != 0u;
        d.vportZOffsetEn = vte.vportZOffsetEn != 0u;
        d.vtxXyFmt = static_cast<uint8_t>(vte.vtxXyFmt);
        d.vtxZFmt  = static_cast<uint8_t>(vte.vtxZFmt);
        d.vtxW0Fmt = static_cast<uint8_t>(vte.vtxW0Fmt);
        // At least one of the six enables (registers.h:569-574). Measured state
        // in build/w658.log is 0x00000300, which sets NONE of them, so the title
        // bakes the transform into the vertex shader -- the D3D9-style case.
        d.vportTransformApplied = d.vportXScaleEn || d.vportXOffsetEn ||
                                  d.vportYScaleEn || d.vportYOffsetEn ||
                                  d.vportZScaleEn || d.vportZOffsetEn;
    }

    // F-646: the enables are measured; the EXTENTS are not (they are a different
    // register pair and no capture carries them yet). An identity viewport is the
    // right answer ONLY when the transform is off AND the target is the full
    // extent. With the transform on and no extents there is no honest answer at
    // all, so it is refused -- and a caller that DOES supply extents clears this
    // by supplying them, not by a flag.
    if (d.vportTransformApplied &&
        (in.viewportWidth == 0u || in.viewportHeight == 0u)) {
        refuseMask |= kRefuseViewportExtentsUnmeasured;
        static std::atomic<uint32_t> s_vport{0};
        const uint32_t n = s_vport.fetch_add(1) + 1;
        if (n <= 4 || (n % 200) == 0)
            MCLA_LOG_WARN("PC-VPORT-UNRESOLVED #{} PA_CL_VTE_CNTL says a viewport "
                          "transform IS applied but no extents were captured; "
                          "PSO refused rather than an identity viewport guessed",
                          n);
    }
    return d;
}

// F-653: the rectangle-offer counter lives at file scope because
// DerivePipelineState is a free function that runs BEFORE any cache exists, and
// "how many rectangles did the guest offer" has to be answerable even when every
// one of them is refused. Relaxed is sufficient and correct: it is a monotone
// tally read for reporting, not a synchronisation edge.
static std::atomic<uint32_t> s_rectangleDrawsOffered{0};

uint32_t RectangleDrawsOffered() {
    return s_rectangleDrawsOffered.load(std::memory_order_relaxed);
}

PipelineState DerivePipelineState(const PipelineStateInputs& in, uint32_t& refuseMask,
                                  DecodedXenosRenderState* decodedOut) {
    if (decodedOut) *decodedOut = DecodedXenosRenderState{};

    // F-646: the bit-layout self-check runs ONCE, on the first real derivation,
    // and speaks up only if it fails. A wrong bit here renders wrong geometry
    // with no error at all, so a silent failure is the one outcome this must
    // not have -- and the house throttle keeps it from flooding a soak.
    static std::atomic<uint32_t> s_layoutChecked{0};
    if (s_layoutChecked.fetch_add(1) == 0) {
        if (!VerifyXenosRenderStateLayouts()) {
            MCLA_LOG_ERROR("PC-REG-LAYOUT-BAD: a PA_SU_SC_MODE_CNTL / "
                           "PA_CL_CLIP_CNTL / PA_CL_VTE_CNTL overlay decodes the "
                           "MEASURED guest value incorrectly. Every shift below is "
                           "wrong somewhere; fix against registers.h:456-489 / "
                           ":534-563 / :566-584 and register_table.inc:540-542. "
                           "Geometry will render wrong with no error until this "
                           "is fixed.");
            refuseMask = kRefuseStateNotDerived;
            return PipelineState{};
        }
    }
    // `= {}` is load-bearing: an uninitialised BOOL reaching
    // CreateGraphicsPipelineState is undefined behaviour rather than "off".
    PipelineState s = {};
    refuseMask = kRefuseNone;
    s.sampleCount = 1;
    ApplyBaselineD3D12State(s);

    for (uint32_t i = 0; i < kMaxRenderTargets; ++i)
        s.targetFormats[i] = in.colorFormats[i];
    s.depthStencilFormat = in.depthStencilFormat;

    // Guest fingerprints, kept for provenance and key identity.
    s.blendState = in.paClipCntl;
    s.rasterState = in.paSuScModeCntl;
    s.depthStencilState = in.paClVteCntl;

    // Topology: only primitives that reach a D3D12 primitive type unchanged are
    // drawable here. A strip/fan/line-strip index buffer fed to the matching
    // primitive type is fine, but feeding it to TRIANGLE without rewriting the
    // indices draws WRONG TRIANGLES, which is the one failure mode a "looks
    // like it renders" mistake produces.
    //
    // F-645: in.primType is the XENOS prim type, and its value space is the
    // architected `xenos::PrimitiveType`
    // (.research/xenia/src/xenia/gpu/xenos.h, `enum class PrimitiveType`:
    // kNone=0x00, kPointList=0x01, kLineList=0x02, kLineStrip=0x03,
    // kTriangleList=0x04, kTriangleFan=0x05, kTriangleStrip=0x06,
    // kTriangleWithWFlags=0x07, kRectangleList=0x08, kLineLoop=0x0C,
    // kQuadList=0x0D, kQuadStrip=0x0E, kPolygon=0x0F, >=0x10 = explicit major
    // mode / 2D / tessellation). The previous table here read `0 -> triangle,
    // 2 -> line, 4 -> point`, which against that enum meant: kNone (0x00) drew a
    // triangle list, kPointList (0x01) was REFUSED, and kTriangleList (0x04) was
    // submitted as a point list -- i.e. every real triangle list lost its
    // triangles, and the type that has no geometry at all invented some. The
    // mapping is therefore spelled out per code here, the same way
    // D3D12Backend::MapXenosPrimType spells out the D3D_PRIMITIVE_TOPOLOGY
    // half, and both must be derived from the xenos.h enum above -- not from
    // DrawPacket::primType's own comment, which is stale.
    //
    // UNMEASURED PREMISE (do not read the refusal log as a bug without
    // checking this first): the value that reaches here is documented as
    // `DrawPacket::primType` (pipeline_cache.h, PipelineStateInputs::primType),
    // and the only writer of that field sets it to `ctx.r4.u32 & 7`
    // (capture_hooks.cpp, `OnDrawBuild`) -- THREE bits of a build descriptor,
    // not the 5+-bit VGT_DRAW_INITIATOR.prim_type, while
    // trace_types.h's `uint32_t primType` comment claims "0=TriList,
    // 1=TriStrip". Two in-repo premises disagree and neither was measured.
    // DerivePipelineState has no caller in src/ (measured 2026-09-28: the only
    // two hits for the symbol are its own declaration and definition), so the
    // architected mapping is the only defensible one to leave in the code; the
    // first soak that wires this up must read the refusal counter below to
    // learn which premise the guest actually satisfies. A non-zero count of
    // 0x04 (kTriangleList) refusals is the signature of the stale `drawFlags`
    // reading, not of a missing D3D12 topology.
    switch (in.primType) {
        case 0x04:  // kTriangleList
        case 0x05:  // kTriangleFan
        case 0x06:  // kTriangleStrip
        case 0x0D:  // kQuadList (expanded to triangles in render_thread)
            s.topology = kTopoTriangle; break;
        case 0x02:  // kLineList
        case 0x03:  // kLineStrip
            s.topology = kTopoLine; break;
        case 0x01:  // kPointList
            s.topology = kTopoPoint; break;
        case 0x08:  // kRectangleList
            // F-653: THIS USED TO BE A REFUSAL, and the reason it gave was
            // correct while the conclusion was wrong. The old note read "Xenos
            // emits two triangles per rectangle with a winding contract D3D12
            // does not have; TRIANGLELIST would draw them in the wrong order,
            // silently". That is exactly right about the CONTRACT and wrong
            // about the conclusion: a contract D3D12 cannot express in a
            // topology is a reason to CARRY A GEOMETRY STAGE, not a reason to
            // refuse. The reference satisfies it in hardware in exactly that
            // way -- the rectangle-list case of TranslateGeometryShader,
            // pipeline_cache.cc:2550-2695 -- and D3D12 has geometry shaders.
            //
            // Measured, not assumed. The shape of the expansion is the thing a
            // wrong port gets wrong, so each term is cited:
            //   * the stage's INPUT primitive is a TRIANGLE of THREE vertices,
            //     pipeline_cache.cc:2300-2301. A rectangle is THREE guest
            //     vertices, and the FOURTH is generated. (The 3-of-3 direction
            //     is what primitive_processor.cc:452's `primitive_count /= 3`
            //     independently agrees with.)
            //   * the generated vertex is v1 + v2 - v0, pipeline_cache.cc:2669-2673.
            //   * max output vertex count 4, pipeline_cache.cc:2303.
            // `topology` is therefore kTopoTriangle -- the input really is
            // triangles -- and the rectangle identity lives in `geometryStage`,
            // which IS key identity (pipeline_cache.h, kGeomStageRectangleList),
            // so a rectangle draw and a triangle draw over the same two shaders
            // are two different PSOs and cannot share one.
            s.topology = kTopoTriangle;
            s.geometryStage = kGeomStageRectangleList;
            {
                // F-653: "the guest OFFERED a rectangle draw" is the denominator
                // every other rectangle counter is read against. Bounded per the
                // F-054 lesson (a soak dominated by one repeating message is
                // POISONED): n <= 8 || n % 200 == 0.
                const uint32_t n = ++s_rectangleDrawsOffered;
                if (n <= 8 || (n % 200) == 0)
                    MCLA_LOG_INFO("PC-RECT-OFFERED #{} rectangle-list draw keyed with "
                                  "the expansion geometry stage (vs/ps unchanged)",
                                  n);
            }
            break;
        // Refused, each with its own name in the log rather than a nearest
        // neighbour (the contract in this file):
        //  0x00 kNone              -- not a geometry type at all; Xenia's
        //                             translation is a zero-index draw
        //  0x07 kTriangleWithWFlags -- per-triangle W flag, which D3D12 carries
        //                             in a PS, not in a topology
        //  0x08 kRectangleList     -- F-653: NO LONGER A REFUSAL. It is the case
        //                             above now. The note that used to sit here
        //                             said "Xenos emits two triangles per
        //                             rectangle with a winding contract D3D12
        //                             does not have; TRIANGLELIST would draw
        //                             them in the wrong order, silently". Every
        //                             clause of that was true and the conclusion
        //                             drawn from it was wrong: a winding contract
        //                             D3D12 has no topology for is precisely
        //                             what a geometry stage is for, and the
        //                             reference satisfies it in one.
        //  0x0C kLineLoop          -- D3D12 has no loop topology
        //  0x0D kQuadList, 0x0E kQuadStrip, 0x0F kPolygon -- no equivalent
        //  >= 0x10                  -- k2DCopyRectListV0 onwards: explicit
        //                             major mode / 2D rect fills / tessellation
        //                             patches, none of which is a D3D12
        //                             primitive type
        default: {
            s.topology = kTopoTriangle;
            refuseMask |= kRefuseTopologyUnknown;
            // Counted per value (not globally) so a soak can name WHICH
            // unmappable type the title actually emits; bounded logging so a
            // key-churning soak cannot launder the log (the F-054 lesson).
            static std::atomic<uint32_t> s_primRefusals[kCountedPrimTypes] = {};
            const uint32_t slot = in.primType < kCountedPrimTypes ? in.primType
                                                                  : kCountedPrimTypes;
            const uint32_t n = s_primRefusals[slot].fetch_add(1) + 1;
            if (n <= 4 || (n % 200) == 0)
                MCLA_LOG_WARN("PC-PRIM-REFUSED #{} xenos primType=0x{:02X} ({}) has no "
                              "D3D12 primitive type; PSO refused, not substituted",
                              n, in.primType, XenosPrimTypeName(in.primType));
            break;
        }
    }

    // MSAA: PA_SU_SC_MODE_CNTL.msaa_enable is decoded (xr.msaaEnable) and its
    // non-zero case is counted at the decode site below, but the sample COUNT is
    // not captured -- it comes from the render target descriptor. Keeping 1 for
    // every draw means the key cannot split a draw in two over a quantity nobody
    // measured; the real count is a follow-up. This must stay BEFORE the decode,
    // because ApplyBaselineD3D12State rewrites the whole rasterizer desc.
    s.sampleCount = 1;
    s.viewportYFlipped = in.viewportYFlipped;
    ApplyBaselineD3D12State(s);  // again, now that sampleCount is final

    D3D12_RASTERIZER_DESC& r = s.d3d12.raster;
    D3D12_DEPTH_STENCIL_DESC& d = s.d3d12.depthStencil;

    if (!in.haveRegisters) {
        // No registers => no derivation. Refuse rather than hand back a
        // plausible-looking default PSO.
        refuseMask |= kRefuseStateNotDerived;
        return s;
    }

    // memcpy rather than a union/reinterpret pun: reading a uint32_t through a
    // bitfield struct is an aliasing violation, and the byte order is the host's
    // for both sides so the copy is a no-op.
    //
    // F-646: the decode moved into DecodeGuestRenderState so the bit layouts have
    // exactly one owner and a validator can reach them. It also sets
    // kRefuseNoPaSuScModeCntl / kRefuseNoPaClipCntl / kRefuseNoPaClVteCntl for
    // every register whose presence bit is clear, which is why the `su`/`cl`
    // overlays are no longer built here: with a register absent, `d.*Present` is
    // false and the corresponding state below is left at the DECLARED BASELINE
    // (never at a decoded zero) and the key already refuses.
    uint32_t regRefusals = kRefuseNone;
    const DecodedXenosRenderState xr = DecodeGuestRenderState(in, regRefusals);
    if (decodedOut) *decodedOut = xr;
    refuseMask |= regRefusals;

    // Cull: cull_front/cull_back, the direct bit mapping Xenia uses
    // (vulkan_pipeline_cache.cc:588-590).
    // F-634: `D3D12_CULL_MODE_FRONT_BACK` does not exist -- d3d12.h defines
    // NONE / FRONT / BACK only, and culling *both* faces has no D3D12
    // equivalent. That case is LOSSY, not mapped, so it is counted: a soak
    // that shows a non-zero count is telling us the title uses dual-sided
    // primitive mode and the host cannot reproduce it.
    if (in.is2DQuad) {
        r.CullMode = D3D12_CULL_MODE_NONE;
    }
    else if (!xr.suPresent) {
        // Baseline (CULL_MODE_NONE) already applied by ApplyBaselineD3D12State.
        // No default is invented and no zero is decoded.
    }
    else if (xr.cullFront && xr.cullBack) {
        r.CullMode = D3D12_CULL_MODE_NONE;
        static std::atomic<uint32_t> s_cullBoth{0};
        const uint32_t n = s_cullBoth.fetch_add(1) + 1;
        if (n <= 4 || (n % 200) == 0)
            MCLA_LOG_WARN("PC-CULL-BOTH #{} no D3D12 equivalent, using NONE", n);
    }
    else if (xr.cullBack)             r.CullMode = D3D12_CULL_MODE_BACK;
    else if (xr.cullFront)            r.CullMode = D3D12_CULL_MODE_FRONT;
    else                            r.CullMode = D3D12_CULL_MODE_NONE;
    // TEMP-CULL-NONE diagnostic (2026-11-20): if missing skyscraper bodies with
    // surviving rooftop caps are a winding/culling fault, forcing CULL_NONE here
    // makes the bodies appear. Gated; unset = byte-for-byte unchanged.
    static const bool s_forceCullNone = [] {
        const char* e = std::getenv("MCLA_TEMP_FORCE_CULL_NONE");
        return e && e[0] == '1';
    }();
    if (s_forceCullNone) r.CullMode = D3D12_CULL_MODE_NONE;

    // face: 0 => front face is CCW (registers.h:461-462), Xenia's mapping. An older inverted mapping only
    // compensated for render states the dispatch guard used to drop.
    if (xr.suPresent) r.FrontCounterClockwise = xr.frontIsCw ? FALSE : TRUE;
    // XE_YFLIP negates oPos.y in VS, inverting screen-space triangle winding; flip FrontCounterClockwise to preserve culling.
    if (in.viewportYFlipped) r.FrontCounterClockwise = r.FrontCounterClockwise ? FALSE : TRUE;

    // Fill: poly_mode 0 renders triangles, 1 (kDualMode) honours the per-face
    // polygon types, 2/3 are undefined (xenos.h:846-853). D3D12 has no point
    // fill mode, and picking lines instead of points is a guess, so anything
    // that is not "triangles" refuses rather than silently drawing a wireframe.
    if (xr.suPresent) {
        if (xr.polyMode == 0) {
            r.FillMode = D3D12_FILL_MODE_SOLID;
        } else if (xr.polyMode == 1) {
            uint32_t polygonType = 2;  // kTriangles
            if (!xr.cullFront) polygonType = std::min(polygonType, xr.polyFrontType);
            if (!xr.cullBack)  polygonType = std::min(polygonType, xr.polyBackType);
            if (polygonType == 2) {
                r.FillMode = D3D12_FILL_MODE_SOLID;
            } else {
                // kLines could be honoured as a wireframe; kPoints cannot be
                // honoured at all. Refuse both and let the log say why.
                refuseMask |= kRefuseUnsupportedPolyMode;
                r.FillMode = D3D12_FILL_MODE_SOLID;
            }
        } else {
            refuseMask |= kRefuseUnsupportedPolyMode;
        }
    }

    // --- D1: polygon / depth offset ----------------------------------------
    // WHAT THIS USED TO SAY, and why it was worse than being wrong:
    //   "Polygon offset: the enable bits are measured; the magnitudes come from
    //    RB_DEPTH_INFO (d3d12/pipeline_cache.cc:1496-1503) which is NOT
    //    captured, so the bias magnitudes stay 0"  ... and then, further down,
    //    "The Xenos poly-offset *magnitudes* live in RB_DEPTHF_INFO".
    // Two claims, and BOTH are structurally incapable of being true:
    //   * `RB_DEPTHF_INFO` IS NOT A REGISTER. `git grep "DEPTHF" --
    //     .research/xenia/src/xenia/gpu/` = 0 hits. Neither is `PA_SU_OFFSET_*`.
    //   * `RB_DEPTH_INFO` (0x2002, register_table.inc:482) has no bias
    //     magnitude either -- registers.h:835-848 is depth_base /
    //     depth_base_bit_11 / _pad_12 / depth_format / _pad_17. The reference
    //     reads RB_DEPTH_INFO exactly ONCE in this path, and only for
    //     `depth_format` (d3d12/pipeline_cache.cc:1500-1501).
    // The magnitudes are four kFloat registers: PA_SU_POLY_OFFSET_{FRONT,BACK}_
    // {SCALE,OFFSET} at 0x2380-0x2383 (register_table.inc:615-618), read by the
    // reference at draw_util.cc:103-104, :108-109 and :115-116. They are
    // ABSOLUTE floats (draw_util.h:118-119), which is why a host that wants a
    // RELATIVE bias has to convert (draw_util.h:128-131, d3d12/pipeline_cache
    // .cc:1500-1503).
    //
    // The old behaviour was the worst of the three options the defect brief
    // allowed: it drew the geometry anyway with a zero bias. That is a
    // degradation to a default -- coplanar geometry z-fights, and nothing says
    // so. So this now either answers the query from the four real registers or
    // REFUSES, naming the registers that are absent. It never renders a
    // requested bias as zero.
    //
    // STATED PREMISE (not measured here, and not a refusal): the reference wraps
    // this whole block in `if (!edram_rov_used)` (d3d12/pipeline_cache.cc:1496).
    // `edram_rov_used` is a host RENDER-TARGET-CACHE path choice
    // (`Path::kPixelShaderInterlock`, d3d12/pipeline_cache.cc:98-99), not a
    // Xenos register, so there is nothing for this module to read and no bit to
    // name. Xenos has no render-order-view hardware, so on this guest the
    // premise is expected to be constant-false; it is recorded rather than
    // asserted.
    if (xr.suPresent && xr.polyOffsetRequested) {
        // Selection rule, reproduced from draw_util.cc:93-121. `polygonal` is
        // the already-derived topology: anything the module refused has
        // kRefuseTopologyUnknown set, and a refused key never becomes a PSO.
        const bool polygonal = (s.topology == kTopoTriangle);
        bool useFront = false;
        if (polygonal) {
            useFront = xr.polyOffsetFrontEn && !xr.cullFront;
        } else {
            // Non-polygonal primitives use the FRONT pair, gated on
            // poly_offset_para_enable instead (draw_util.cc:111-117).
            useFront = xr.polyOffsetParaEn;
        }
        // BACK is the fallback for polygonal primitives only, and only when the
        // front pair was not selected (draw_util.cc:106-110).
        const bool useBack = polygonal && !useFront && xr.polyOffsetBackEn &&
                             !xr.cullBack;
        const uint32_t neededReg =
            useFront ? kGuestRegPolyOffsetFront
                     : (useBack ? kGuestRegPolyOffsetBack : 0u);
        const bool magnitudesPresent = neededReg != 0u &&
                                       (in.registersPresent & neededReg) != 0u;
        if (!magnitudesPresent) {
            // Say WHICH of the four real registers is missing. `neededReg == 0`
            // means the guest enabled a bias the selection rule does not turn
            // into a magnitude for this primitive (e.g. front enable with
            // cull_front), which is a real, honest no-op -- but it is still not
            // a reason to render a bias, so it is logged and the draw is left
            // at zero bias only because there IS no bias to apply.
            if (neededReg == 0u) {
                static std::atomic<uint32_t> s_polyOffInert{0};
                const uint32_t n = s_polyOffInert.fetch_add(1) + 1;
                if (n <= 4 || (n % 200) == 0)
                    MCLA_LOG_WARN("PC-POLYOFF #{} PA_SU_SC_MODE_CNTL bits 11-13 request a "
                                  "bias but draw_util.cc:93-121 selects no magnitude for a "
                                  "{} primitive (front={} back={} cullFront={} cullBack={} "
                                  "para={}); the Xenos hardware likewise applies none",
                                  n, polygonal ? "polygonal" : "non-polygonal",
                                  xr.polyOffsetFrontEn ? 1 : 0,
                                  xr.polyOffsetBackEn ? 1 : 0, xr.cullFront ? 1 : 0,
                                  xr.cullBack ? 1 : 0, xr.polyOffsetParaEn ? 1 : 0);
            } else {
                refuseMask |= useFront ? kRefuseNoPolyOffsetFront
                                       : kRefuseNoPolyOffsetBack;
                CountMissingRegister(useFront ? "PA_SU_POLY_OFFSET_FRONT_SCALE+OFFSET"
                                               : "PA_SU_POLY_OFFSET_BACK_SCALE+OFFSET",
                                     useFront ? "2380-2381" : "2382-2383");
            }
        } else if (!(in.registersPresent & kGuestRegDepthInfo)) {
            // The four magnitudes are here, but the absolute->relative
            // conversion has no factor without RB_DEPTH_INFO.depth_format
            // (draw_util.h:206-215). Guessing a format would put the bias in
            // the wrong units by a factor of 8, so it refuses by name.
            refuseMask |= kRefuseNoDepthInfo;
            CountMissingRegister("RB_DEPTH_INFO", "2002");
        } else {
            const float scale = useFront ? in.polyOffsetFrontScale
                                         : in.polyOffsetBackScale;
            const float offset = useFront ? in.polyOffsetFrontOffset
                                          : in.polyOffsetBackOffset;
            // The host depth is mirrored (1 - z) whenever DepthCompareMirrored() is on
            // (d3d12_backend.h:43-44, default ON), so both bias terms must flip sign or
            // every polygon offset pushes the wrong way -- shadow acne on casters, decals
            // sinking behind the road. When MCLA_NO_STDZ_SHADOW_FIX=1 is unset, standard-Z
            // draws (shadow casters, in.depthMirrored == false) keep positive bias sign.
            static const bool s_noStdZBiasFix = []() {
                const char* e = std::getenv("MCLA_NO_STDZ_SHADOW_FIX");
                return e && e[0] == '1';
            }();
            const bool mirrored = s_noStdZBiasFix ? native::DepthCompareMirrored() : in.depthMirrored;
            const float biasSign = mirrored ? -1.0f : 1.0f;
            r.DepthBias = biasSign * static_cast<float>(
                D3D10IntegerPolygonOffset(in.depthFormat == 1u, offset));
            r.SlopeScaledDepthBias = biasSign * scale * kPolygonOffsetScaleSubpixelUnit;
            r.DepthBiasClamp = 0.0f;
        }
    }

    // depth_clip = !clip_disable (d3d12/pipeline_cache.cc:1508).
    // F-646: only when PA_CL_CLIP_CNTL actually arrived. The measured value is
    // 0x00010000, i.e. clip_disable = 1, so the honest answer for this title is
    // DepthClipEnable = FALSE -- geometry outside the clip volume is NOT
    // discarded, which is why decoding this as 0 (clipping on) would have
    // clipped geometry away and looked like a bug somewhere else entirely.
    if (xr.clPresent)
        r.DepthClipEnable = xr.clipDisable ? FALSE : TRUE;

    // MSAA: paSuScModeCntl.msaa_enable is MEASURED (decoded above into
    // xr.msaaEnable) but the sample COUNT is not -- it comes from the render
    // target descriptor, which is not captured. Keeping 1 for every draw means
    // the key cannot split a draw in two over a quantity nobody measured; the
    // real count is a follow-up. F-646: the enable is now at least named, so a
    // non-zero count is attributable to the register rather than to "someone
    // guessed 1".
    if (xr.suPresent && xr.msaaEnable) {
        static std::atomic<uint32_t> s_msaaEn{0};
        const uint32_t n = s_msaaEn.fetch_add(1) + 1;
        if (n <= 4 || (n % 200) == 0)
            MCLA_LOG_WARN("PC-MSAA-EN #{} PA_SU_SC_MODE_CNTL.msaa_enable=1 but the "
                          "sample COUNT is uncaptured -- keeping 1", n);
    }

    // Depth/stencil. The ztest lives in RB_DEPTHCONTROL (0x2200): stencil_enable
    // bit 0, z_enable bit 1, z_write_enable bit 2, zfunc bits 4-6,
    // backface_enable bit 7 (registers.h:802-808), read by the reference at
    // draw_util.cc:76 and lowered at d3d12/pipeline_cache.cc:1513-1519. It does
    // NOT live in RB_DEPTH_INFO or COLOR0_INFO -- this comment used to say so,
    // and it was wrong in the same way the poly-offset comment was: both names
    // are real registers, but neither carries an enable, a write bit or a
    // compare function. (RB_DEPTH_INFO is now captured for the bias conversion
    // only; see D1 above. Its `depth_format` is not a ztest.)
    // The renderer captures none of RB_DEPTHCONTROL, so there is no measured
    // source for DepthEnable/DepthFunc/StencilEnable here. Emitting Xenia's
    // documented "no depth info" state (always-pass, no writes) is a STATED
    // placeholder, flagged so it stays
    // countable; refusing by default would keep every real PSO permanently
    // unbuildable, which is a different lie.
    s.flags |= kStateFlagUnmeasuredDepth;
    if (in.requireMeasuredDepthState) refuseMask |= kRefuseRequiredDepthState;
    if (s.depthStencilFormat == 0) {
        // D3D12 forbids DepthEnable with no depth-stencil format; fold that into
        // the key so the PSO and the key cannot disagree.
        d.DepthEnable = FALSE;
        d.StencilEnable = FALSE;
    } else {
        // Depth-stencil format is present (e.g. DXGI_FORMAT_D24_UNORM_S8_UINT).
        if (!(in.registersPresent & kGuestRegDepthControl) && (in.is2DQuad || in.samplesFontAtlas)) {  // stride-36 world meshes are not 2D: the guest register decides
            // 2D quads and text overlays render on top without depth testing or depth writes
            d.DepthEnable = FALSE;
            d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            d.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            d.StencilEnable = FALSE;
        } else if (in.registersPresent & kGuestRegDepthControl) {
            const uint32_t dc = in.depthControl;
            const bool zEnable = (dc & (1u << 1)) != 0;
            const bool zWrite = (dc & (1u << 2)) != 0;
            const uint32_t zFunc = (dc >> 4) & 7u;
            d.DepthEnable = zEnable ? TRUE : FALSE;
            d.DepthWriteMask = zWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            d.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(D3D12_COMPARISON_FUNC_NEVER + zFunc);

            const bool stencilEn = (dc & (1u << 0)) != 0;
            d.StencilEnable = stencilEn ? TRUE : FALSE;
            if (stencilEn) {
                // Xenia d3d12/pipeline_cache.cc:1520-1560. Xenos StencilOp 0..7 = D3D12 KEEP..DECR (1..8).
                d.StencilReadMask = static_cast<UINT8>((in.stencilRefMask >> 8) & 0xFFu);
                d.StencilWriteMask = static_cast<UINT8>((in.stencilRefMask >> 16) & 0xFFu);
                auto op = [](uint32_t v) { return static_cast<D3D12_STENCIL_OP>(D3D12_STENCIL_OP_KEEP + (v & 7u)); };
                auto fn = [](uint32_t v) { return static_cast<D3D12_COMPARISON_FUNC>(D3D12_COMPARISON_FUNC_NEVER + (v & 7u)); };
                d.FrontFace.StencilFunc = fn(dc >> 8);
                d.FrontFace.StencilFailOp = op(dc >> 11);
                d.FrontFace.StencilPassOp = op(dc >> 14);
                d.FrontFace.StencilDepthFailOp = op(dc >> 17);
                if (dc & (1u << 7)) {  // backface_enable
                    d.BackFace.StencilFunc = fn(dc >> 20);
                    d.BackFace.StencilFailOp = op(dc >> 23);
                    d.BackFace.StencilPassOp = op(dc >> 26);
                    d.BackFace.StencilDepthFailOp = op(dc >> 29);
                } else {
                    d.BackFace = d.FrontFace;
                }
            }
        } else {
            // 3D world geometry default when explicit RB_DEPTHCONTROL was not snapshotted
            d.DepthEnable = TRUE;
            d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            d.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            d.StencilEnable = FALSE;
        }
    }

    // F-904: BLEND DERIVED from RB_BLENDCONTROL0 (0x2201) + RB_COLOR_MASK
    // (0x2104) when the guest presented them; the unmeasured baseline above
    // (blend-off + kStateFlagUnmeasuredBlend) stays for draws whose packet
    // never carried them. The baseline overwrote transparent texels instead
    // of compositing them (measured black screen, F-901); deriving the real
    // equation is what un-blacks it.
    //
    // Field layout (registers.h:783-791): color_srcblend 5b+0, color_comb_fcn
    // 3b+5, color_destblend 5b+8, alpha_srcblend 5b+16, alpha_comb_fcn 3b+21,
    // alpha_destblend 5b+24. Factor maps are Xenia's
    // d3d12/pipeline_cache.cc:1577-1624 verbatim (kBlendFactorMap for color,
    // kBlendFactorAlphaMap for alpha -- color modes become alpha modes because
    // D3D12 has no separate color/alpha factor namespace the way the indexing
    // there assumes; indices 17-31 are zero-initialised to kZero exactly as
    // in the reference). Op map is d3d12/pipeline_cache.cc:3142-3145.
    // Enable rule is the reference's (:3163-3168): off only when every factor
    // is ONE/ZERO and both ops are ADD. Write mask is the RT0 nibble of
    // RB_COLOR_MASK (registers.h:758-761), which is bit-identical to
    // D3D12_COLOR_WRITE_ENABLE_* (R/G/B/A = bits 0/1/2/3 both sides).
    {
        static constexpr uint32_t kColorFactorMap[32] = {
            0, 1, 0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 10, 11, 12,
            // 17..31 zero-initialised to 0 (kZero), as in the reference.
        };
        static constexpr uint32_t kAlphaFactorMap[32] = {
            0, 1, 0, 0, 4, 5, 4, 5, 8, 9, 8, 9, 10, 11, 10, 11, 12,
            // 17..31 zero-initialised to 0 (kZero), as in the reference.
        };
        // Indexed by PipelineBlendFactor value (kZero=0 .. kSrcAlphaSat=12).
        static constexpr D3D12_BLEND kD3DBlendMap[13] = {
            D3D12_BLEND_ZERO, D3D12_BLEND_ONE,
            D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR,
            D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA,
            D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
            D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
            D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR,
            D3D12_BLEND_SRC_ALPHA_SAT};
        static constexpr D3D12_BLEND_OP kD3DBlendOpMap[8] = {
            D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT,
            D3D12_BLEND_OP_MIN, D3D12_BLEND_OP_MAX,
            D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_OP_ADD,
            D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD};
        const auto decodeGuestRtBlend = [&](uint32_t rt, uint32_t ctl, uint32_t writeMask,
                                              D3D12_RENDER_TARGET_BLEND_DESC& dst) {
            const uint32_t cSrc = ctl & 0x1Fu;
            const uint32_t cOp = (ctl >> 5) & 0x7u;
            const uint32_t cDst = (ctl >> 8) & 0x1Fu;
            const uint32_t aSrc = (ctl >> 16) & 0x1Fu;
            const uint32_t aOp = (ctl >> 21) & 0x7u;
            const uint32_t aDst = (ctl >> 24) & 0x1Fu;
            const uint32_t fCSrc = kColorFactorMap[cSrc];
            const uint32_t fCDst = kColorFactorMap[cDst];
            const uint32_t fASrc = kAlphaFactorMap[aSrc];
            const uint32_t fADst = kAlphaFactorMap[aDst];
            // Constant factors need OMSetBlendFactor (per-draw dynamic state
            // the backend never sets). Baking in (1,1,1,1) would invent the
            // blend constant, so refuse by name instead. Counted: the soak
            // says whether the game even uses them.
            if (fCSrc == 10u || fCSrc == 11u || fCDst == 10u ||
                fCDst == 11u || fASrc == 10u || fASrc == 11u ||
                fADst == 10u || fADst == 11u) {
                refuseMask |= kRefuseBlendConstantFactor;
                static std::atomic<uint32_t> s_blendConst{0};
                const uint32_t n = s_blendConst.fetch_add(1) + 1;
                if (n <= 4 || (n % 200) == 0) {
                    MCLA_LOG_WARN("PC-BLEND-CONSTANT #{} rt={} blendControl={:08X} "
                                  "(a constant factor needs OMSetBlendFactor, "
                                  "which no path sets) -- refused, not defaulted",
                                  n, rt, ctl);
                }
                return;
            }
            dst.SrcBlend = kD3DBlendMap[fCSrc];
            dst.DestBlend = kD3DBlendMap[fCDst];
            dst.BlendOp = kD3DBlendOpMap[cOp];
            dst.SrcBlendAlpha = kD3DBlendMap[fASrc];
            dst.DestBlendAlpha = kD3DBlendMap[fADst];
            dst.BlendOpAlpha = kD3DBlendOpMap[aOp];
            dst.RenderTargetWriteMask = static_cast<UINT8>(writeMask);
            // The reference's enable rule: off only when the equation is
            // the identity (ONE/ZERO + ADD on both color and alpha).
            const bool isIdentity =
                fCSrc == 1u && fCDst == 0u && cOp == 0u && fASrc == 1u &&
                fADst == 0u && aOp == 0u;
            dst.BlendEnable = isIdentity ? FALSE : TRUE;
            // F-904 census: the raw register + the decoded equation, so a
            // soak measures what the game actually asks for. Throttled
            // like PC-BLEND-BASELINE (per derive attempt, not per PSO).
            static std::atomic<uint32_t> s_blendDerived{0};
            const uint32_t n = s_blendDerived.fetch_add(1) + 1;
            if (n <= 4 || (n % 20000) == 0) {  // MD-32: was 200
                MCLA_LOG_WARN("PC-BLEND-DERIVED #{} rt={} blendControl={:08X} "
                              "mask={:01X} src={} dst={} op={} srcA={} "
                              "dstA={} opA={} enable={} -- measured guest "
                              "blend equation (F-904)",
                              n, rt, ctl, writeMask, fCSrc, fCDst, cOp, fASrc,
                              fADst, aOp, dst.BlendEnable ? 1 : 0);
            }
        };

        const uint32_t blendBits[4] = {
            kGuestRegBlendControl, kGuestRegBlendControl1,
            kGuestRegBlendControl2, kGuestRegBlendControl3};
        const uint32_t blendCtls[4] = {
            (in.blendControl[0] != 0u) ? in.blendControl[0] : in.blendControl0,
            in.blendControl[1], in.blendControl[2], in.blendControl[3]};
        const bool hasColorMask = (in.registersPresent & kGuestRegColorMask) != 0u;
        const bool blendPresent =
            (in.registersPresent &
             (kGuestRegBlendControl | kGuestRegBlendControl1 |
              kGuestRegBlendControl2 | kGuestRegBlendControl3)) != 0u;
        bool anyMrtBlendOrMask = blendPresent || hasColorMask;

        for (uint32_t rt = 0; rt < 4u; ++rt) {
            const uint32_t writeMask =
                hasColorMask ? ((in.blendColorMask >> (rt * 4u)) & 0xFu) : 0xFu;
            if ((in.registersPresent & blendBits[rt]) != 0u) {
                decodeGuestRtBlend(rt, blendCtls[rt], writeMask, s.d3d12.blend.RenderTarget[rt]);
                anyMrtBlendOrMask = true;
            } else if (hasColorMask) {
                s.d3d12.blend.RenderTarget[rt].RenderTargetWriteMask = static_cast<UINT8>(writeMask);
            }
        }
        if (anyMrtBlendOrMask) {
            s.d3d12.blend.IndependentBlendEnable = TRUE;
        }

        if (blendPresent) {
            // TEMP F-900 BLEND-PROBE (moved from ApplyBaselineD3D12State so it
            // overrides the DECODED state, not the pre-decode baseline; remove
            // with the other env probes once real blend is default-on verified).
            // MCLA_FORCE_BLEND=1 forces slot 0 to standard UI alpha blending.
            {
                static const int s_forceBlend2 = [] {
                    const char* e = std::getenv("MCLA_FORCE_BLEND");
                    // Default OFF (0) to respect the guest's native enable=0 equation (ga058).
                    return (e && e[0] == '1') ? 1 : 0;
                }();
                if (s_forceBlend2 == 1) {
                    D3D12_RENDER_TARGET_BLEND_DESC& fb0 =
                        s.d3d12.blend.RenderTarget[0];
                    fb0.BlendEnable = TRUE;
                    fb0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                    fb0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                    fb0.SrcBlendAlpha = D3D12_BLEND_ONE;
                    fb0.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                    // F-920: the force had NO log row, so whether it reached the
                    // legals PSO was unmeasurable. Throttled to 1 in 200 like
                    // PC-BLEND-DERIVED (this runs per derive attempt).
                    static std::atomic<uint32_t> s_forceBlendApplied{0};
                    const uint32_t fn2 = s_forceBlendApplied.fetch_add(1) + 1;
                    if (fn2 <= 4 || (fn2 % 200) == 0) {
                        MCLA_LOG_WARN("PC-BLEND-FORCE #{} slot0 <- "
                                      "SRC_ALPHA/INV_SRC_ALPHA on BOTH color "
                                      "and alpha (was declared "
                                      "blend-off/identity by the guest)",
                                      fn2);
                    }
                }
            }
        } else {
            // No RT0 blend register in this packet: the declared blend-off
            // baseline stands, flagged countable (was: stamped unconditionally
            // with a note that it cannot split a key -- it now splits exactly
            // when the guest's equation differs, which is the point).
            s.flags |= kStateFlagUnmeasuredBlend;
            // G5 (default): this baseline left RT write masks at ALL, so a
            // measured RB_COLOR_MASK (0 = depth-only, or partial) was ignored and shadow casters
            // painted the colour target. Per-RT nibbles were applied above; log here as before.
            if (hasColorMask) {
                static std::atomic<uint32_t> s_colorMaskApplied{0};
                const uint32_t cm = s_colorMaskApplied.fetch_add(1) + 1;
                if (cm <= 8 || (cm % 500) == 0)
                    MCLA_LOG_INFO("PC-COLORMASK-APPLIED #{} mask={:04X} depthOnlyPso={}", cm, in.blendColorMask & 0xFFFFu,
                                  (in.blendColorMask & 0xFFFFu) == 0u ? 1 : 0);
            }
            {
                // Throttled like PC-MSAA-EN above: this runs for EVERY derive attempt
                // (render_thread.cpp), not once per PSO, so an unthrottled line would
                // own the log the way one mitigation did in F-054. Counts derives;
                // stats_.unmeasuredBlendState below counts PSOs actually created --
                // read them apart, do not expect them to match.
                static std::atomic<uint32_t> s_blendBaseline{0};
                const uint32_t n = s_blendBaseline.fetch_add(1) + 1;
                if (n <= 4 || (n % 200) == 0)
                    MCLA_LOG_WARN("PC-BLEND-BASELINE #{} blend state is the declared "
                                  "blend-off baseline (kStateFlagUnmeasuredBlend); no "
                                  "Xenos blend register was presented for this draw",
                                  n);
            }
        }

        // Alpha blending for font atlas (0x13BCC000) and 2D UI quads:
        // Enables proper transparency and fading, eliminating tofu boxes and black backgrounds.
        // Applied whether blend registers were present or not.
        {
            static const int s_blendUi = [] {
                const char* e = std::getenv("MCLA_BLEND_TEXT");
                return (e && e[0] == '0') ? 0 : (e && e[0] == '1') ? 1 : 2;  // default 2: keep guest blend/mask (radar mask, text box)
            }();
            // MCLA_BLEND_TEXT=2: never override an explicit guest blend decision. The old
            // test skipped the override only when the guest's blend was ENABLED, so a guest
            // RB_BLENDCONTROL0 with enable=0 ("overwrite the target" -- what every opaque
            // mesh and every RECTLIST post-FX pass sets) was silently rewritten to
            // SRC_ALPHA/INV_SRC_ALPHA. Measured: 3,438 BLEND-UI-ON prints at a 200 cadence
            // = ~686,800 draws. MCLA_BLEND_TEXT=1 stays the explicit "force it anyway" knob.
            const bool guestBlendKnown = (in.registersPresent & kGuestRegBlendControl) != 0;
            if (s_blendUi >= 1 && (s_blendUi == 1 || !guestBlendKnown) &&
                (in.samplesFontAtlas || in.is2DQuad)) {
                D3D12_RENDER_TARGET_BLEND_DESC& tb0 =
                    s.d3d12.blend.RenderTarget[0];
                const UINT8 keepMask = tb0.RenderTargetWriteMask;
                tb0.BlendEnable = TRUE;
                tb0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                tb0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                tb0.BlendOp = D3D12_BLEND_OP_ADD;
                tb0.SrcBlendAlpha = D3D12_BLEND_ONE;
                tb0.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                tb0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                tb0.RenderTargetWriteMask = (s_blendUi == 2 && (in.registersPresent & kGuestRegColorMask)) ? keepMask : D3D12_COLOR_WRITE_ENABLE_ALL;
                s.d3d12.blend.IndependentBlendEnable = TRUE;
                static std::atomic<uint32_t> s_blendUiApplied{0};
                const uint32_t bn = s_blendUiApplied.fetch_add(1) + 1;
                if (bn <= 4 || (bn % 200) == 0) {
                    MCLA_LOG_WARN("BLEND-UI-ON #{} slot0 <- "
                                  "SRC_ALPHA/INV_SRC_ALPHA (font atlas / 2D quad "
                                  "draw; alpha blend enabled)",
                                  bn);
                }
            }
        }
    }

    // Hardware alpha-to-coverage when MSAA > 1x is active (1x MSAA uses PS 2x2 dither instead).
    if (s.sampleCount > 1u && in.alphaToMaskEnable) {
        s.d3d12.blend.AlphaToCoverageEnable = TRUE;
    }

    // MIN/MAX blend equations with SrcAlpha + One: D3D12 ignores blend factors for MIN/MAX,
    // so signal the PS variant to premultiply oC0 by saturate(oC0.a) before export.
    static const bool s_noMinMaxPremul = [] {
        const char* e = std::getenv("MCLA_NO_MINMAX_PREMUL");
        return e && e[0] == '1';
    }();
    if (!s_noMinMaxPremul && decodedOut) {
        for (uint32_t rt = 0; rt < 4u; ++rt) {
            const D3D12_RENDER_TARGET_BLEND_DESC& fb = s.d3d12.blend.RenderTarget[rt];
            if (fb.BlendEnable && fb.SrcBlend == D3D12_BLEND_SRC_ALPHA && fb.DestBlend == D3D12_BLEND_ONE &&
                (fb.BlendOp == D3D12_BLEND_OP_MIN || fb.BlendOp == D3D12_BLEND_OP_MAX)) {
                decodedOut->minMaxPremulRgb = true;
            }
            if (fb.BlendEnable && fb.SrcBlendAlpha == D3D12_BLEND_SRC_ALPHA && fb.DestBlendAlpha == D3D12_BLEND_ONE &&
                (fb.BlendOpAlpha == D3D12_BLEND_OP_MIN || fb.BlendOpAlpha == D3D12_BLEND_OP_MAX)) {
                decodedOut->minMaxPremulAlpha = true;
            }
        }
    }

    s.derivation = kStateDerivedGuest;
    return s;
}

void SetDerivedPipelineState(PipelineState& state) {
    // Fills the D3D12 state with the declared baseline when the caller has not
    // written any, so "I am taking responsibility for this state" cannot turn
    // into "D3D12_RASTERIZER_DESC was zeroed, so DepthClipEnable == FALSE and
    // every primitive is clipped away".
    if (state.sampleCount == 0) state.sampleCount = 1;
    // F-634: D3D12 has no FILL_MODE_UNSPECIFIED or CULL_MODE_UNSPECIFIED --
    // d3d12.h defines SOLID/WIREFRAME and NONE/FRONT/BACK, and the zero value
    // of each IS the solid/none case. A zero-initialised PipelineState
    // therefore reads as SOLID + NONE, so the intent of the original test
    // ("the caller wrote no raster state at all") is preserved exactly by
    // comparing against the zero enumerators instead of a name that does not
    // exist. No behaviour change.
    if (state.d3d12.raster.DepthClipEnable == FALSE &&
        state.d3d12.raster.FillMode == D3D12_FILL_MODE_SOLID &&
        state.d3d12.raster.CullMode == D3D12_CULL_MODE_NONE) {
        ApplyBaselineD3D12State(state);
    }
    if (state.derivation == kStateDerivedNone) state.derivation = kStateDerivedExplicit;
}

// ---------------------------------------------------------------------------
// Authoritative vertex-input layout
// ---------------------------------------------------------------------------
bool BuildGrcFvfInputLayout(const native::GrcFvfDeclaration& decl,
                            std::vector<D3D12_INPUT_ELEMENT_DESC>& out,
                            uint64_t* outDeclHash) {
    out.clear();
    if (outDeclHash) *outDeclHash = 0;
    if (!decl.valid || decl.unknownChannel) return false;
    // A declared stride that disagrees with the decoded one means the decode is
    // incomplete (a packed/aligned channel we do not model) and the offsets this
    // builder hands to the PSO would read the wrong bytes.
    if (decl.sizeMismatch) return false;

    native::GrcFvfLayoutEntry entries[native::kGrcFvfChannelCount];
    uint32_t count = 0;
    if (!native::BuildGrcFvfLayout(decl, entries, count)) return false;

    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        D3D12_INPUT_ELEMENT_DESC e = {};
        // Explicit byteOffset, never D3D12_APPEND_ALIGNED: APPEND would
        // 4-byte-align every element, which silently reinterprets the guest
        // stride (the previous builder used APPEND for all 16 lanes) whenever a
        // channel is 2 bytes wide.
        e.AlignedByteOffset = entries[i].byteOffset;
        e.Format = static_cast<DXGI_FORMAT>(entries[i].dxgiFormat);
        e.InputSlot = 0;
        e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        e.InstanceDataStepRate = 0;
        // SemanticName is a pointer the PSO keeps for its lifetime, so it must
        // outlive the D3D12Backend::BuildInputLayoutFromGrcFvf temporary. The
        // names come from a static table (VertexUsageSemanticName) that lives for
        // the process, which is why the builder is not a local buffer of const
        // char* the way the old per-call switch produced.
        e.SemanticName = VertexUsageSemanticName(entries[i].usage);
        e.SemanticIndex = entries[i].usageIndex;
        out.push_back(e);
    }
    if (outDeclHash) *outDeclHash = native::HashGrcFvfDeclaration(decl);
    return !out.empty();
}

uint64_t HashInputLayout(const std::vector<D3D12_INPUT_ELEMENT_DESC>& layout) {
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&h](const void* data, size_t size) {
        h = HashBytes(data, size, h);
    };
    for (const auto& e : layout) {
        const uint32_t fields[5] = {e.SemanticIndex, static_cast<uint32_t>(e.Format),
                                   e.AlignedByteOffset, e.InputSlot,
                                   static_cast<uint32_t>(e.InputSlotClass)};
        mix(fields, sizeof(fields));
        const uint32_t step = e.InstanceDataStepRate;
        mix(&step, sizeof(step));
        if (e.SemanticName) {
            const uint32_t len = static_cast<uint32_t>(std::strlen(e.SemanticName));
            mix(&len, sizeof(len));
            h = HashBytes(e.SemanticName, len, h);
        }
    }
    const uint32_t n = static_cast<uint32_t>(layout.size());
    mix(&n, sizeof(n));
    return h;
}

// ---------------------------------------------------------------------------
// Xenos constant bank
// ---------------------------------------------------------------------------
ConstantBankPack PackXenosConstantBank(const uint8_t* guestBeBytes, size_t guestBytes,
                                       uint8_t* dstBank, size_t dstBytes) {
    ConstantBankPack r = {};
    if (!dstBank || dstBytes < 16) return r;
    // Zeros first, so a short guest bank cannot leak whatever was in the
    // previous frame's upload ring.
    std::memset(dstBank, 0, dstBytes);

    const size_t usable = std::min(guestBytes, dstBytes);
    r.guestBytes = static_cast<uint32_t>(std::min(guestBytes, dstBytes));
    r.truncated = guestBytes > dstBytes;

    for (size_t off = 0; off + 16 <= usable; off += 16) {
        float f[4];
        for (uint32_t c = 0; c < 4; ++c) {
            // Explicit big-endian assembly per the project rule: a guest pointer
            // is a 32-bit guest VA and Xenos floats are stored big-endian, so
            // every 4-byte group is assembled by hand rather than aliased.
            const uint8_t* p = guestBeBytes + off + c * 4;
            const uint32_t be = (static_cast<uint32_t>(p[0]) << 24) |
                                (static_cast<uint32_t>(p[1]) << 16) |
                                (static_cast<uint32_t>(p[2]) << 8) |
                                 static_cast<uint32_t>(p[3]);
            uint32_t le;
            std::memcpy(&le, &be, sizeof(le));
            std::memcpy(&f[c], &le, sizeof(f[c]));
        }
        std::memcpy(dstBank + off, f, 16);
        ++r.registersPacked;
    }
    const size_t covered = static_cast<size_t>(r.registersPacked) * 16;
    r.zeroFilledBytes = static_cast<uint32_t>(dstBytes - covered);
    r.ok = true;
    return r;
}

ConstantBankBinding ResolveConstantBankBinding(const DxcRuntime::CbvBinding& cbv) {
    ConstantBankBinding b = {};
    b.shaderRegister = cbv.register_;
    b.space = cbv.space_;
    b.requiredBytes = cbv.byteSize;
    // F-644: the CBV is root parameter 2, not 1. CreateTestRootSignature
    // (d3d12_backend.cpp, symbol `CreateTestRootSignature`, `D3D12_ROOT_PARAMETER
    // rootParams[3]`) declares THREE root parameters: 0 = SRV descriptor table
    // t0..t3, 1 = SAMPLER descriptor table (1 descriptor, s0, space 0), 2 = CBV
    // b0 space 0. The sampler used to be a D3D12_STATIC_SAMPLER_DESC at s0
    // (NumStaticSamplers 1) and is now a descriptor table, which pushed the CBV
    // from 1 to 2; the draw path binds it with
    // SetGraphicsRootConstantBufferView(2, ...), so naming 1 here would make
    // every real PSO report an invalid binding and refuse (kRefuseNoRootSignature).
    // Nothing else in this file hardcodes the index, so there is no second site
    // to change: the worker never describes or creates a root signature of its
    // own. It is HANDED the backend's -- `PipelineCache::StartWorker`'s
    // `rootSignature` argument, which lands in `WorkerLoop` and is copied
    // straight into `psoDesc.pRootSignature = rootSignature.Get()` in the
    // compile block below. A future reader looking for a second edit here should
    // look at the draw path's SetGraphicsRootConstantBufferView index instead.
    if (cbv.space_ == 0 && cbv.register_ == kShaderConstantRootRegister) {
        b.rootParameter = kShaderConstantRootSlot;
        b.valid = 1;
    }
    return b;
}

// ---------------------------------------------------------------------------
// Versioned PSO DESCRIPTION persistence (never driver PSO blobs)
// ---------------------------------------------------------------------------
constexpr uint32_t kPsoCacheFileVersion = 1;
constexpr uint32_t kPsoTranslatorCacheVersion = 1;
constexpr uint32_t kPsoDxcCacheVersion = 1;
constexpr uint32_t kPsoCacheHeaderBytes = 12;
constexpr uint32_t kPsoRecordHeaderBytes = 16;
constexpr uint32_t kPsoCacheMaxRecords = 65536;
constexpr uint32_t kPsoCacheMaxRecordBytes = 16 * 1024 * 1024;
constexpr uint64_t kPsoCacheMaxFileBytes = 512ull * 1024 * 1024;
constexpr uint32_t kPsoCacheMaxInputElements = 64;
constexpr uint32_t kPsoCacheMaxSemanticBytes = 128;
constexpr uint32_t kPsoCacheMaxBlobBytes = 8 * 1024 * 1024;
constexpr uint32_t kPsoCacheMaxResources = 256;
constexpr uint32_t kPsoCacheMaxBindings = 64;

struct PsoCacheCounters {
    std::atomic<uint64_t> persistAttempts{0};
    std::atomic<uint64_t> persistSaved{0};
    std::atomic<uint64_t> persistFailed{0};
    std::atomic<uint64_t> persistSkippedDedup{0};
    std::atomic<uint64_t> prewarmExamined{0};
    std::atomic<uint64_t> prewarmCreated{0};
    std::atomic<uint64_t> prewarmFailed{0};
    std::atomic<uint64_t> prewarmStale{0};
    std::atomic<uint64_t> prewarmCorrupt{0};
    std::atomic<uint64_t> prewarmHits{0};
};
PsoCacheCounters g_psoCacheCounters;

bool ShouldLogPsoCensus(uint64_t n) {
    return n <= 4 || (n % 200) == 0;
}

std::string ResolvePsoCachePath() {
    if (const char* env = std::getenv("MCLA_PSO_CACHE"); env && *env)
        return env;
    std::vector<char> exe(32768, '\0');
    const DWORD n = GetModuleFileNameA(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (n > 0 && n < exe.size()) {
        const std::filesystem::path dir =
            std::filesystem::path(std::string(exe.data(), n)).parent_path();
        if (!dir.empty()) return (dir / "pso_cache.bin").string();
    }
    return "pso_cache.bin";
}

// ID3D12RootSignature does not expose its serialized blob. The cache therefore
// hashes a serialization of the exact CreateTestRootSignature contract used by
// this architecture. A root-signature edit must update this mirror and the
// translator/cache version together; the stored hash rejects stale records.
bool HashSerializedRootSignature(uint64_t& hash) {
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = kShaderTextureSlotCount;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    const char* samplerEnv = std::getenv("MCLA_TEMP_GUEST_SAMPLER");
    const bool guestSamplers = !(samplerEnv && samplerEnv[0] == '0');
    D3D12_DESCRIPTOR_RANGE samplerRange = {};
    samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplerRange.NumDescriptors = guestSamplers ? kShaderSamplerCount : 1u;
    samplerRange.BaseShaderRegister = 0;
    samplerRange.RegisterSpace = 0;
    samplerRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParams[5] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &samplerRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 0;
    rootParams[2].Descriptor.RegisterSpace = 0;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[3].Descriptor.ShaderRegister = 0;
    rootParams[3].Descriptor.RegisterSpace = 0;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[4].Descriptor.ShaderRegister = 0;
    rootParams[4].Descriptor.RegisterSpace = 1;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 5;
    desc.pParameters = rootParams;
    desc.NumStaticSamplers = 0;
    desc.pStaticSamplers = nullptr;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errorBlob;
    const HRESULT hr = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errorBlob);
    if (FAILED(hr) || !blob || blob->GetBufferSize() == 0) return false;
    hash = HashBytes(blob->GetBufferPointer(), blob->GetBufferSize(), 0);
    return true;
}

struct PsoDescription {
    PipelineKey key = {};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    std::vector<uint8_t> vsDxil, psDxil, dsDxil, hsDxil, gsDxil;
    std::vector<std::string> semantics;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
    std::vector<DxcRuntime::ResourceBinding> vsResources, psResources;
    std::vector<ConstantBankBinding> cbvBindings;
    uint64_t rootSignatureHash = 0;
    uint32_t translatorVersion = 0;
    uint32_t dxcVersion = 0;
};

struct PsoWriter {
    std::vector<uint8_t> bytes;
    void U8(uint8_t v) { bytes.push_back(v); }
    void U32(uint32_t v) {
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void U64(uint64_t v) {
        for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void F32(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        U32(bits);
    }
    void Bytes(const std::vector<uint8_t>& value) {
        if (value.size() > kPsoCacheMaxBlobBytes) return;
        U32(static_cast<uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void String(std::string_view value) {
        if (value.size() > kPsoCacheMaxSemanticBytes) return;
        U32(static_cast<uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
};

struct PsoReader {
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    size_t offset = 0;
    bool ok = true;
    bool Need(size_t count) {
        if (!ok || count > size - offset) { ok = false; return false; }
        return true;
    }
    bool U8(uint8_t& value) {
        if (!Need(1)) return false;
        value = bytes[offset++];
        return true;
    }
    bool U32(uint32_t& value) {
        if (!Need(4)) return false;
        value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[offset++]) << (i * 8);
        return true;
    }
    bool U64(uint64_t& value) {
        if (!Need(8)) return false;
        value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(bytes[offset++]) << (i * 8);
        return true;
    }
    template <typename T>
    bool E(T& value) {
        uint32_t raw = 0;
        if (!U32(raw)) return false;
        value = static_cast<T>(raw);
        return true;
    }
    bool F32(float& value) {
        uint32_t bits = 0;
        if (!U32(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }
    bool Bytes(std::vector<uint8_t>& value) {
        uint32_t count = 0;
        if (!U32(count) || count > kPsoCacheMaxBlobBytes || !Need(count)) return false;
        value.assign(bytes + offset, bytes + offset + count);
        offset += count;
        return true;
    }
    bool String(std::string& value) {
        uint32_t count = 0;
        if (!U32(count) || count > kPsoCacheMaxSemanticBytes || !Need(count)) return false;
        value.assign(reinterpret_cast<const char*>(bytes + offset), count);
        offset += count;
        return true;
    }
};

void PutResources(PsoWriter& writer,
                  const std::vector<DxcRuntime::ResourceBinding>& resources) {
    writer.U32(static_cast<uint32_t>(resources.size()));
    for (const auto& resource : resources) {
        writer.U32(resource.type);
        writer.U32(resource.register_);
        writer.U32(resource.space_);
        writer.U32(resource.byteSize);
        writer.U32(resource.firstVariableRegister);
    }
}

bool ReadResources(PsoReader& reader,
                   std::vector<DxcRuntime::ResourceBinding>& resources) {
    uint32_t count = 0;
    if (!reader.U32(count) || count > kPsoCacheMaxResources) return false;
    resources.clear();
    resources.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        DxcRuntime::ResourceBinding resource;
        if (!reader.U32(resource.type) || !reader.U32(resource.register_) ||
            !reader.U32(resource.space_) || !reader.U32(resource.byteSize) ||
            !reader.U32(resource.firstVariableRegister)) return false;
        resources.push_back(resource);
    }
    return true;
}

void PutD3D12State(PsoWriter& writer,
                   const D3D12_RASTERIZER_DESC& raster,
                   const D3D12_BLEND_DESC& blend,
                   const D3D12_DEPTH_STENCIL_DESC& depthStencil) {
    writer.U32(blend.AlphaToCoverageEnable);
    writer.U32(blend.IndependentBlendEnable);
    for (const auto& target : blend.RenderTarget) {
        writer.U32(target.BlendEnable);
        writer.U32(target.LogicOpEnable);
        writer.U32(target.SrcBlend);
        writer.U32(target.DestBlend);
        writer.U32(target.BlendOp);
        writer.U32(target.SrcBlendAlpha);
        writer.U32(target.DestBlendAlpha);
        writer.U32(target.BlendOpAlpha);
        writer.U32(target.LogicOp);
        writer.U32(target.RenderTargetWriteMask);
    }
    writer.U32(raster.FillMode);
    writer.U32(raster.CullMode);
    writer.U32(raster.FrontCounterClockwise);
    writer.U32(static_cast<uint32_t>(raster.DepthBias));
    writer.F32(raster.DepthBiasClamp);
    writer.F32(raster.SlopeScaledDepthBias);
    writer.U32(raster.DepthClipEnable);
    writer.U32(raster.MultisampleEnable);
    writer.U32(raster.AntialiasedLineEnable);
    writer.U32(raster.ForcedSampleCount);
    writer.U32(raster.ConservativeRaster);
    writer.U32(depthStencil.DepthEnable);
    writer.U32(depthStencil.DepthWriteMask);
    writer.U32(depthStencil.DepthFunc);
    writer.U32(depthStencil.StencilEnable);
    writer.U8(depthStencil.StencilReadMask);
    writer.U8(depthStencil.StencilWriteMask);
    for (const auto* face : {&depthStencil.FrontFace, &depthStencil.BackFace}) {
        writer.U32(face->StencilFailOp);
        writer.U32(face->StencilDepthFailOp);
        writer.U32(face->StencilPassOp);
        writer.U32(face->StencilFunc);
    }
}

bool ReadD3D12State(PsoReader& reader,
                    D3D12_RASTERIZER_DESC& raster,
                    D3D12_BLEND_DESC& blend,
                    D3D12_DEPTH_STENCIL_DESC& depthStencil) {
    if (!reader.E(blend.AlphaToCoverageEnable) ||
        !reader.E(blend.IndependentBlendEnable)) return false;
    for (auto& target : blend.RenderTarget) {
        static const bool s_noDeserFix = [] {
            const char* e = std::getenv("MCLA_NO_PSO_DESERIALIZATION_FIX");
            return e && e[0] == '1';
        }();
        uint32_t writeMask = 0;
        if (!reader.E(target.BlendEnable) || !reader.E(target.LogicOpEnable) ||
            !reader.E(target.SrcBlend) || !reader.E(target.DestBlend) ||
            !reader.E(target.BlendOp) || !reader.E(target.SrcBlendAlpha) ||
            !reader.E(target.DestBlendAlpha) || !reader.E(target.BlendOpAlpha) ||
            !reader.E(target.LogicOp)) return false;
        if (s_noDeserFix) {
            if (!reader.U8(target.RenderTargetWriteMask)) return false;
        } else {
            if (!reader.U32(writeMask)) return false;
            target.RenderTargetWriteMask = static_cast<UINT8>(writeMask);
        }
    }
    if (!reader.E(raster.FillMode) || !reader.E(raster.CullMode) ||
        !reader.E(raster.FrontCounterClockwise)) return false;
    uint32_t depthBiasBits = 0;
    if (!reader.U32(depthBiasBits)) return false;
    std::memcpy(&raster.DepthBias, &depthBiasBits, sizeof(raster.DepthBias));
    if (!reader.F32(raster.DepthBiasClamp) || !reader.F32(raster.SlopeScaledDepthBias) ||
        !reader.E(raster.DepthClipEnable) || !reader.E(raster.MultisampleEnable) ||
        !reader.E(raster.AntialiasedLineEnable) || !reader.U32(raster.ForcedSampleCount) ||
        !reader.E(raster.ConservativeRaster)) return false;
    if (!reader.E(depthStencil.DepthEnable) || !reader.E(depthStencil.DepthWriteMask) ||
        !reader.E(depthStencil.DepthFunc) || !reader.E(depthStencil.StencilEnable) ||
        !reader.U8(depthStencil.StencilReadMask) ||
        !reader.U8(depthStencil.StencilWriteMask)) return false;
    for (auto* face : {&depthStencil.FrontFace, &depthStencil.BackFace}) {
        if (!reader.E(face->StencilFailOp) || !reader.E(face->StencilDepthFailOp) ||
            !reader.E(face->StencilPassOp) || !reader.E(face->StencilFunc)) return false;
    }
    return true;
}

bool WritePsoPayload(std::vector<uint8_t>& payload, const CompileTask& task,
                     const std::vector<uint8_t>& vsDxil,
                     const std::vector<uint8_t>& psDxil,
                     const std::vector<uint8_t>& gsDxil,
                     const std::vector<DxcRuntime::ResourceBinding>& vsResources,
                     const std::vector<DxcRuntime::ResourceBinding>& psResources,
                     const std::vector<ConstantBankBinding>& bindings,
                     const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                     uint64_t rootSignatureHash) {
    if (rootSignatureHash == 0 || task.inputLayout.size() > kPsoCacheMaxInputElements ||
        vsDxil.size() > kPsoCacheMaxBlobBytes || psDxil.size() > kPsoCacheMaxBlobBytes ||
        gsDxil.size() > kPsoCacheMaxBlobBytes || vsResources.size() > kPsoCacheMaxResources ||
        psResources.size() > kPsoCacheMaxResources || bindings.size() > kPsoCacheMaxBindings ||
        desc.DS.BytecodeLength != 0 || desc.HS.BytecodeLength != 0 ||
        desc.StreamOutput.NumEntries != 0 || desc.StreamOutput.NumStrides != 0 ||
        desc.CachedPSO.CachedBlobSizeInBytes != 0) return false;
    for (const auto& input : task.inputLayout) {
        if (!input.SemanticName || std::strlen(input.SemanticName) > kPsoCacheMaxSemanticBytes)
            return false;
    }

    PsoWriter writer;
    writer.U64(task.key.vsHash);
    writer.U64(task.key.psHash);
    writer.U64(task.key.vsHlslHash);
    writer.U64(task.key.psHlslHash);
    writer.U64(task.key.vertexDeclHash);
    writer.U64(task.key.inputLayoutHash);
    writer.U32(task.key.refuseMask);
    writer.U32(task.key.reserved);
    for (uint32_t format : task.key.state.targetFormats) writer.U32(format);
    writer.U32(task.key.state.depthStencilFormat);
    writer.U32(task.key.state.blendState);
    writer.U32(task.key.state.rasterState);
    writer.U32(task.key.state.depthStencilState);
    writer.U8(task.key.state.topology);
    writer.U8(task.key.state.sampleCount);
    writer.U8(task.key.state.geometryStage);
    writer.U8(task.key.state.derivation);
    writer.U8(task.key.state.flags);
    writer.U8(task.key.state.viewportYFlipped ? 1u : 0u);
    PutD3D12State(writer, task.key.state.d3d12.raster, task.key.state.d3d12.blend,
                  task.key.state.d3d12.depthStencil);

    writer.Bytes(vsDxil);
    writer.Bytes(psDxil);
    writer.Bytes(std::vector<uint8_t>{});
    writer.Bytes(std::vector<uint8_t>{});
    writer.Bytes(gsDxil);
    writer.U32(desc.StreamOutput.NumEntries);
    writer.U32(desc.StreamOutput.NumStrides);
    writer.U32(desc.StreamOutput.RasterizedStream);
    PutD3D12State(writer, desc.RasterizerState, desc.BlendState, desc.DepthStencilState);
    writer.U32(desc.SampleMask);
    writer.U32(static_cast<uint32_t>(task.inputLayout.size()));
    for (const auto& input : task.inputLayout) {
        writer.String(input.SemanticName);
        writer.U32(input.SemanticIndex);
        writer.U32(input.Format);
        writer.U32(input.InputSlot);
        writer.U32(input.AlignedByteOffset);
        writer.U32(input.InputSlotClass);
        writer.U32(input.InstanceDataStepRate);
    }
    writer.U32(desc.IBStripCutValue);
    writer.U32(desc.PrimitiveTopologyType);
    writer.U32(desc.NumRenderTargets);
    for (DXGI_FORMAT format : desc.RTVFormats) writer.U32(format);
    writer.U32(desc.DSVFormat);
    writer.U32(desc.SampleDesc.Count);
    writer.U32(desc.SampleDesc.Quality);
    writer.U32(desc.NodeMask);
    writer.U64(desc.CachedPSO.CachedBlobSizeInBytes);
    writer.U32(desc.Flags);
    writer.U64(rootSignatureHash);
    writer.U32(kPsoTranslatorCacheVersion);
    writer.U32(kPsoDxcCacheVersion);
    PutResources(writer, vsResources);
    PutResources(writer, psResources);
    writer.U32(static_cast<uint32_t>(bindings.size()));
    for (const auto& binding : bindings) {
        writer.U32(binding.shaderRegister);
        writer.U32(binding.space);
        writer.U32(binding.requiredBytes);
        writer.U32(binding.rootParameter);
        writer.U32(binding.valid);
    }
    if (writer.bytes.size() > kPsoCacheMaxRecordBytes) return false;
    payload = std::move(writer.bytes);
    return true;
}

bool ReadPsoPayload(const uint8_t* data, size_t size, PsoDescription& record) {
    PsoReader reader{data, size, 0, true};
    auto fail = [&reader] { return !reader.ok; };
    if (!reader.U64(record.key.vsHash) || !reader.U64(record.key.psHash) ||
        !reader.U64(record.key.vsHlslHash) || !reader.U64(record.key.psHlslHash) ||
        !reader.U64(record.key.vertexDeclHash) || !reader.U64(record.key.inputLayoutHash) ||
        !reader.U32(record.key.refuseMask) || !reader.U32(record.key.reserved)) return false;
    for (uint32_t& format : record.key.state.targetFormats)
        if (!reader.U32(format)) return false;
    if (!reader.U32(record.key.state.depthStencilFormat) ||
        !reader.U32(record.key.state.blendState) || !reader.U32(record.key.state.rasterState) ||
        !reader.U32(record.key.state.depthStencilState)) return false;
    uint8_t viewportYFlipped = 0;
    if (!reader.U8(record.key.state.topology) || !reader.U8(record.key.state.sampleCount) ||
        !reader.U8(record.key.state.geometryStage) || !reader.U8(record.key.state.derivation) ||
        !reader.U8(record.key.state.flags) || !reader.U8(viewportYFlipped) || viewportYFlipped > 1)
        return false;
    record.key.state.viewportYFlipped = viewportYFlipped != 0;
    if (!ReadD3D12State(reader, record.key.state.d3d12.raster,
                        record.key.state.d3d12.blend, record.key.state.d3d12.depthStencil))
        return false;
    if (!reader.Bytes(record.vsDxil) || !reader.Bytes(record.psDxil) ||
        !reader.Bytes(record.dsDxil) || !reader.Bytes(record.hsDxil) ||
        !reader.Bytes(record.gsDxil)) return false;
    if (!reader.U32(record.desc.StreamOutput.NumEntries) ||
        !reader.U32(record.desc.StreamOutput.NumStrides) ||
        !reader.U32(record.desc.StreamOutput.RasterizedStream)) return false;
    if (!ReadD3D12State(reader, record.desc.RasterizerState, record.desc.BlendState,
                        record.desc.DepthStencilState) || !reader.U32(record.desc.SampleMask))
        return false;
    uint32_t inputCount = 0;
    if (!reader.U32(inputCount) || inputCount == 0 || inputCount > kPsoCacheMaxInputElements)
        return false;
    record.semantics.reserve(inputCount);
    record.inputLayout.reserve(inputCount);
    for (uint32_t i = 0; i < inputCount; ++i) {
        std::string semantic;
        D3D12_INPUT_ELEMENT_DESC input = {};
        if (!reader.String(semantic) || semantic.empty() || !reader.U32(input.SemanticIndex) ||
            !reader.E(input.Format) || !reader.U32(input.InputSlot) ||
            !reader.U32(input.AlignedByteOffset) || !reader.E(input.InputSlotClass) ||
            !reader.U32(input.InstanceDataStepRate)) return false;
        record.semantics.push_back(std::move(semantic));
        record.inputLayout.push_back(input);
    }
    for (size_t i = 0; i < record.inputLayout.size(); ++i)
        record.inputLayout[i].SemanticName = record.semantics[i].c_str();
    if (!reader.E(record.desc.IBStripCutValue) ||
        !reader.E(record.desc.PrimitiveTopologyType) ||
        !reader.U32(record.desc.NumRenderTargets)) return false;
    for (DXGI_FORMAT& format : record.desc.RTVFormats)
        if (!reader.E(format)) return false;
    if (!reader.E(record.desc.DSVFormat) || !reader.U32(record.desc.SampleDesc.Count) ||
        !reader.U32(record.desc.SampleDesc.Quality) || !reader.U32(record.desc.NodeMask)) return false;
    uint64_t cachedSize = 0;
    if (!reader.U64(cachedSize) || cachedSize != 0 || !reader.E(record.desc.Flags) ||
        !reader.U64(record.rootSignatureHash) || !reader.U32(record.translatorVersion) ||
        !reader.U32(record.dxcVersion) || !ReadResources(reader, record.vsResources) ||
        !ReadResources(reader, record.psResources)) return false;
    uint32_t bindingCount = 0;
    if (!reader.U32(bindingCount) || bindingCount == 0 || bindingCount > kPsoCacheMaxBindings)
        return false;
    record.cbvBindings.reserve(bindingCount);
    for (uint32_t i = 0; i < bindingCount; ++i) {
        ConstantBankBinding binding;
        if (!reader.U32(binding.shaderRegister) || !reader.U32(binding.space) ||
            !reader.U32(binding.requiredBytes) || !reader.U32(binding.rootParameter) ||
            !reader.U32(binding.valid)) return false;
        record.cbvBindings.push_back(binding);
    }
    if (fail() || reader.offset != reader.size) return false;
    record.desc.VS.pShaderBytecode = record.vsDxil.empty() ? nullptr : record.vsDxil.data();
    record.desc.VS.BytecodeLength = record.vsDxil.size();
    record.desc.PS.pShaderBytecode = record.psDxil.empty() ? nullptr : record.psDxil.data();
    record.desc.PS.BytecodeLength = record.psDxil.size();
    record.desc.DS.pShaderBytecode = record.dsDxil.empty() ? nullptr : record.dsDxil.data();
    record.desc.DS.BytecodeLength = record.dsDxil.size();
    record.desc.HS.pShaderBytecode = record.hsDxil.empty() ? nullptr : record.hsDxil.data();
    record.desc.HS.BytecodeLength = record.hsDxil.size();
    record.desc.GS.pShaderBytecode = record.gsDxil.empty() ? nullptr : record.gsDxil.data();
    record.desc.GS.BytecodeLength = record.gsDxil.size();
    record.desc.InputLayout.pInputElementDescs = record.inputLayout.data();
    record.desc.InputLayout.NumElements = static_cast<UINT>(record.inputLayout.size());
    return true;
}

bool FullRasterizerEqual(const D3D12_RASTERIZER_DESC& a,
                         const D3D12_RASTERIZER_DESC& b) {
    return a.FillMode == b.FillMode && a.CullMode == b.CullMode &&
           a.FrontCounterClockwise == b.FrontCounterClockwise && a.DepthBias == b.DepthBias &&
           a.DepthBiasClamp == b.DepthBiasClamp &&
           a.SlopeScaledDepthBias == b.SlopeScaledDepthBias &&
           a.DepthClipEnable == b.DepthClipEnable && a.MultisampleEnable == b.MultisampleEnable &&
           a.AntialiasedLineEnable == b.AntialiasedLineEnable &&
           a.ForcedSampleCount == b.ForcedSampleCount && a.ConservativeRaster == b.ConservativeRaster;
}

bool ResourcesFitRootSignature(const std::vector<DxcRuntime::ResourceBinding>& resources) {
    const char* samplerEnv = std::getenv("MCLA_TEMP_GUEST_SAMPLER");
    const uint32_t samplerLimit =
        !(samplerEnv && samplerEnv[0] == '0') ? kShaderSamplerCount : 1u;
    for (const auto& resource : resources) {
        if (resource.type == DxcRuntime::kSitSampler) {
            if (resource.space_ != 0 || resource.register_ >= samplerLimit) return false;
        } else if (resource.type == DxcRuntime::kSitCbuffer) {
            if (resource.space_ != 0 || resource.register_ != kShaderConstantRootRegister) return false;
        } else if (resource.type == DxcRuntime::kSitTexture ||
                   resource.type == DxcRuntime::kSitStructuredBuffer ||
                   resource.type == DxcRuntime::kSitByteAddressBuffer) {
            if (resource.type == DxcRuntime::kSitByteAddressBuffer &&
                resource.space_ == 1 && resource.register_ == 0) continue;
            if (resource.space_ != 0 || resource.register_ >= kShaderTextureSlotCount) return false;
        }
    }
    return true;
}

bool ValidatePsoDescription(const PsoDescription& record, std::string& error) {
    error.clear();
    auto reject = [&error](const char* reason) { error = reason; return false; };
    if (record.translatorVersion != kPsoTranslatorCacheVersion || record.dxcVersion != kPsoDxcCacheVersion)
        return reject("cache format version mismatch");
    if (record.rootSignatureHash == 0) return reject("missing root-signature hash");
    if (record.key.refuseMask != kRefuseNone || record.key.reserved != 0)
        return reject("record key carries a refusal/reserved value");
    if ((record.key.state.derivation != kStateDerivedGuest &&
         record.key.state.derivation != kStateDerivedExplicit) || record.key.state.sampleCount == 0)
        return reject("record key has no derived state/sample count");
    if (record.vsDxil.empty()) return reject("record has no vertex DXIL");
    if (!record.dsDxil.empty() || !record.hsDxil.empty()) return reject("unsupported DS/HS stage");
    if (record.desc.StreamOutput.NumEntries != 0 || record.desc.StreamOutput.NumStrides != 0)
        return reject("unsupported stream output");
    if (record.desc.CachedPSO.CachedBlobSizeInBytes != 0) return reject("driver PSO blob is forbidden");
    if (record.inputLayout.empty() || HashInputLayout(record.inputLayout) != record.key.inputLayoutHash)
        return reject("input layout does not match the key");
    const bool wantsGeometry = record.key.state.geometryStage != kGeomStageNone;
    if (wantsGeometry != !record.gsDxil.empty()) return reject("geometry stage/key mismatch");
    if (wantsGeometry && record.key.state.geometryStage != kGeomStageRectangleList)
        return reject("unknown geometry stage");
    if (!record.psDxil.empty() && record.psResources.size() > kPsoCacheMaxResources)
        return reject("pixel reflection exceeds bounds");
    if (record.psDxil.empty() && !record.psResources.empty())
        return reject("PS-less record carries pixel reflection");
    if (!ResourcesFitRootSignature(record.vsResources) ||
        !ResourcesFitRootSignature(record.psResources)) return reject("reflection exceeds root signature");
    if (record.cbvBindings.empty()) return reject("record has no constant-bank binding");
    for (const auto& binding : record.cbvBindings) {
        if (binding.valid != 1 || binding.space != 0 ||
            binding.shaderRegister != kShaderConstantRootRegister ||
            binding.rootParameter != kShaderConstantRootSlot)
            return reject("invalid constant-bank binding");
    }
    if (record.desc.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return reject("render-target count exceeds D3D12 limit");
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        const bool named = i < record.desc.NumRenderTargets;
        if ((record.desc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN) != named)
            return reject("render-target formats are not contiguous");
    }
    if (record.desc.NumRenderTargets == 0 &&
        (record.desc.DSVFormat == DXGI_FORMAT_UNKNOWN || !record.psDxil.empty()))
        return reject("zero render targets on a non-depth-only record");
    if (record.desc.DSVFormat == DXGI_FORMAT_UNKNOWN &&
        record.desc.DepthStencilState.DepthEnable) return reject("depth enabled without DSV format");
    if (record.desc.SampleDesc.Count == 0 || record.desc.SampleDesc.Count > 255)
        return reject("invalid sample count");
    uint8_t topology = kTopoTriangle;
    switch (record.desc.PrimitiveTopologyType) {
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE: topology = kTopoTriangle; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: topology = kTopoLine; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: topology = kTopoPoint; break;
        default: return reject("unknown primitive topology type");
    }
    PipelineState expected = record.key.state;
    expected.topology = topology;
    for (uint32_t i = 0; i < kMaxRenderTargets; ++i) expected.targetFormats[i] = record.desc.RTVFormats[i];
    expected.depthStencilFormat = record.desc.DSVFormat;
    expected.sampleCount = static_cast<uint8_t>(record.desc.SampleDesc.Count);
    expected.d3d12.raster = record.desc.RasterizerState;
    expected.d3d12.blend = record.desc.BlendState;
    PipelineState keyState = record.key.state;
    if (record.psDxil.empty()) {
        static const bool s_noDepthBlendFix = [] {
            const char* e = std::getenv("MCLA_NO_PSO_DEPTH_BLEND_VALIDATION_FIX");
            return e && e[0] == '1';
        }();
        for (auto& target : expected.d3d12.blend.RenderTarget) target.RenderTargetWriteMask = 0;
        if (!s_noDepthBlendFix) {
            for (auto& target : keyState.d3d12.blend.RenderTarget) target.RenderTargetWriteMask = 0;
        }
    }
    expected.d3d12.depthStencil = record.desc.DepthStencilState;
    if (!FullRasterizerEqual(record.key.state.d3d12.raster, record.desc.RasterizerState) ||
        !(expected == keyState)) return reject("PSO description does not match PipelineKey");
    return true;
}

struct PsoCacheScan {
    std::vector<PsoDescription> records;
    uint64_t validBytes = 0;
    uint64_t corrupt = 0;
    uint64_t stale = 0;
    uint64_t examined = 0;
};

PsoCacheScan ScanPsoCache(const std::string& path, uint64_t rootSignatureHash,
                          bool keepRecords) {
    PsoCacheScan result;
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        result.corrupt = 1;
        return result;
    }
    if (!exists) return result;
    const uint64_t fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize > kPsoCacheMaxFileBytes) {
        result.corrupt = 1;
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    char magic[4] = {};
    uint32_t version = 0, headerBytes = 0;
    input.read(magic, 4);
    input.read(reinterpret_cast<char*>(&version), 4);
    input.read(reinterpret_cast<char*>(&headerBytes), 4);
    if (!input || std::memcmp(magic, "MPSC", 4) != 0 ||
        version != kPsoCacheFileVersion || headerBytes != kPsoCacheHeaderBytes) {
        result.corrupt = 1;
        return result;
    }
    uint64_t offset = kPsoCacheHeaderBytes;
    std::unordered_map<PipelineKey, size_t, PipelineKeyHash> recordIndex;
    while (offset < fileSize) {
        if (fileSize - offset < kPsoRecordHeaderBytes || result.examined >= kPsoCacheMaxRecords) {
            ++result.corrupt;
            break;
        }
        char recordMagic[4] = {};
        uint32_t payloadSize = 0;
        uint64_t checksum = 0;
        input.read(recordMagic, 4);
        input.read(reinterpret_cast<char*>(&payloadSize), 4);
        input.read(reinterpret_cast<char*>(&checksum), 8);
        if (!input || std::memcmp(recordMagic, "PSOR", 4) != 0 ||
            payloadSize > kPsoCacheMaxRecordBytes || payloadSize > fileSize - offset - kPsoRecordHeaderBytes) {
            ++result.corrupt;
            break;
        }
        std::vector<uint8_t> payload(payloadSize);
        input.read(reinterpret_cast<char*>(payload.data()), payloadSize);
        offset += kPsoRecordHeaderBytes + payloadSize;
        result.validBytes = offset;
        ++result.examined;
        if (!input || HashBytes(payload.data(), payload.size(), 0) != checksum) {
            ++result.corrupt;
            continue;
        }
        if (!keepRecords) continue;
        PsoDescription record;
        std::string error;
        if (!ReadPsoPayload(payload.data(), payload.size(), record)) {
            ++result.corrupt;
            continue;
        }
        if (record.translatorVersion != kPsoTranslatorCacheVersion ||
            record.dxcVersion != kPsoDxcCacheVersion || record.rootSignatureHash != rootSignatureHash) {
            ++result.stale;
            continue;
        }
        if (!ValidatePsoDescription(record, error)) {
            ++result.corrupt;
            continue;
        }
        auto existing = recordIndex.find(record.key);
        if (existing == recordIndex.end()) {
            recordIndex.emplace(record.key, result.records.size());
            result.records.push_back(std::move(record));
        } else {
            result.records[existing->second] = std::move(record);
        }
    }
    return result;
}

std::vector<uint8_t> MakePsoCacheHeader() {
    PsoWriter writer;
    writer.bytes.push_back('M');
    writer.bytes.push_back('P');
    writer.bytes.push_back('S');
    writer.bytes.push_back('C');
    writer.U32(kPsoCacheFileVersion);
    writer.U32(kPsoCacheHeaderBytes);
    return writer.bytes;
}

std::vector<uint8_t> MakePsoRecordHeader(const std::vector<uint8_t>& payload) {
    PsoWriter writer;
    writer.bytes.push_back('P');
    writer.bytes.push_back('S');
    writer.bytes.push_back('O');
    writer.bytes.push_back('R');
    writer.U32(static_cast<uint32_t>(payload.size()));
    writer.U64(HashBytes(payload.data(), payload.size(), 0));
    return writer.bytes;
}

// PipelineCache implementation

PipelineCache::PipelineCache(std::size_t maxEntries) : maxEntries_(maxEntries) {
    map_.reserve(maxEntries_);
    order_.reserve(maxEntries_);
}

PipelineCache::~PipelineCache() {
    StopWorker();
}

void PipelineCache::StartWorker(Microsoft::WRL::ComPtr<ID3D12Device> device,
                                Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (workerThread_.joinable()) return;
    }

    device_ = device;
    rootSignature_ = rootSignature;
    psoCachePath_ = ResolvePsoCachePath();
    const char* prewarmEnv = std::getenv("MCLA_NO_PSO_PREWARM");
    const char* persistEnv = std::getenv("MCLA_NO_PSO_PERSIST");
    const bool prewarmDisabled = prewarmEnv && prewarmEnv[0] == '1';
    const bool persistDisabled = persistEnv && persistEnv[0] == '1';
    const char* coldFallbackEnv = std::getenv("MCLA_NO_PSO_COLD_TEST_REFUSAL");
    const char* nonGeomFallbackEnv = std::getenv("MCLA_PSO_NONGEOM_FALLBACK");
    const char* rawHashEnv = std::getenv("MCLA_NO_PSO_STABLE_KEY_HASH");
    const char* rasterKeyEnv = std::getenv("MCLA_NO_PSO_FULL_RASTER_KEY");
    MCLA_LOG_INFO("PC-PSO-KEY-HASH #0 mode={} fullRasterKey={} positiveControl=1 "
                  "-- field-wise hashing keeps persisted keys stable",
                  rawHashEnv && rawHashEnv[0] == '1' ? "raw-legacy" : "field-wise",
                  rasterKeyEnv && rasterKeyEnv[0] == '1' ? 0 : 1);
    MCLA_LOG_INFO("PC-PSO-COLD-POLICY #0 allowFixedTestFallback={} nonGeomOptIn={} "
                  "positiveControl=1 -- cold misses wait for a real PSO or refuse",
                  (coldFallbackEnv && coldFallbackEnv[0] == '1') ||
                          (nonGeomFallbackEnv && nonGeomFallbackEnv[0] == '1')
                      ? 1 : 0,
                  nonGeomFallbackEnv && nonGeomFallbackEnv[0] == '1' ? 1 : 0);
    MCLA_LOG_INFO("PSO-PERSIST-SAVE #0 disabled={} path={} positiveControl=1 "
                  "-- description appends are worker-only", persistDisabled ? 1 : 0,
                  psoCachePath_);
    if (!prewarmDisabled || !persistDisabled)
        rootSignatureHashValid_ = HashSerializedRootSignature(rootSignatureHash_);
    if (!rootSignatureHashValid_ && (!prewarmDisabled || !persistDisabled)) {
        if (!persistDisabled)
            g_psoCacheCounters.persistFailed.fetch_add(1, std::memory_order_relaxed);
        MCLA_LOG_WARN("PSO-PERSIST-SAVE #0 rootHashValid=0 failed={} "
                      "-- D3D12SerializeRootSignature failed; persistence/prewarm disabled safely",
                      g_psoCacheCounters.persistFailed.load());
    }

    if (prewarmDisabled) {
        MCLA_LOG_INFO("PSO-PREWARM #0 disabled=1 path={} records=0 created=0 failed=0 "
                      "stale=0 corrupt=0 positiveControl=1", psoCachePath_);
        if (!persistDisabled && rootSignatureHashValid_) {
            const PsoCacheScan scan = ScanPsoCache(psoCachePath_, rootSignatureHash_, true);
            psoCacheValidBytes_ = scan.validBytes;
            g_psoCacheCounters.prewarmCorrupt.fetch_add(scan.corrupt, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(persistMutex_);
            for (const auto& rec : scan.records) {
                persistedKeys_.insert(rec.key);
            }
        }
    } else {
        PrewarmPsoCache(device, rootSignature);
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopWorker_ = false;
        workerThread_ = std::thread(&PipelineCache::WorkerLoop, this, device_, rootSignature_);
    }
}

void PipelineCache::PrewarmPsoCache(Microsoft::WRL::ComPtr<ID3D12Device> device,
                                    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature) {
    if (!device || !rootSignature || !rootSignatureHashValid_) {
        g_psoCacheCounters.prewarmFailed.fetch_add(1, std::memory_order_relaxed);
        MCLA_LOG_WARN("PSO-PREWARM #0 disabled=0 path={} records=0 created=0 failed=1 "
                      "stale=0 corrupt=0 reason=device/root-signature/hash unavailable",
                      psoCachePath_);
        return;
    }
    PsoCacheScan scan = ScanPsoCache(psoCachePath_, rootSignatureHash_, true);
    psoCacheValidBytes_ = scan.validBytes;
    g_psoCacheCounters.prewarmExamined.fetch_add(scan.examined, std::memory_order_relaxed);
    g_psoCacheCounters.prewarmStale.fetch_add(scan.stale, std::memory_order_relaxed);
    g_psoCacheCounters.prewarmCorrupt.fetch_add(scan.corrupt, std::memory_order_relaxed);
    bool geometrySupported = geometryStageSupported_;
    if (!geometrySupported) {
        const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels{1, &requested, D3D_FEATURE_LEVEL_11_0};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,
                                                  &levels, sizeof(levels))))
            geometrySupported = levels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_11_0;
    }

    static const bool s_noPrewarmMt = [] {
        const char* e = std::getenv("MCLA_NO_PSO_PREWARM_MT");
        return e && e[0] == '1';
    }();

    const size_t totalRecords = scan.records.size();
    const size_t hwThreads = std::thread::hardware_concurrency();
    const size_t targetThreads = s_noPrewarmMt
        ? 1
        : std::min<size_t>(4, std::max<size_t>(1, hwThreads));
    const size_t actualThreads = (totalRecords <= 1) ? 1 : targetThreads;

    struct PrewarmOutcome {
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = S_OK;
    };
    std::vector<PrewarmOutcome> outcomes(totalRecords);

    if (actualThreads > 1) {
        std::atomic<size_t> nextIdx{0};
        auto workerFn = [&]() {
            while (true) {
                const size_t i = nextIdx.fetch_add(1, std::memory_order_relaxed);
                if (i >= totalRecords) break;
                PsoDescription& record = scan.records[i];
                record.desc.pRootSignature = rootSignature.Get();
                if (record.key.state.geometryStage != kGeomStageNone && !geometrySupported) {
                    outcomes[i].hr = E_FAIL;
                } else {
                    outcomes[i].hr = device->CreateGraphicsPipelineState(&record.desc,
                                                                         IID_PPV_ARGS(&outcomes[i].pso));
                }
            }
        };
        std::vector<std::thread> workers;
        workers.reserve(actualThreads);
        for (size_t t = 0; t < actualThreads; ++t) {
            workers.emplace_back(workerFn);
        }
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    } else {
        for (size_t i = 0; i < totalRecords; ++i) {
            PsoDescription& record = scan.records[i];
            record.desc.pRootSignature = rootSignature.Get();
            if (record.key.state.geometryStage != kGeomStageNone && !geometrySupported) {
                outcomes[i].hr = E_FAIL;
            } else {
                outcomes[i].hr = device->CreateGraphicsPipelineState(&record.desc,
                                                                     IID_PPV_ARGS(&outcomes[i].pso));
            }
        }
    }

    for (size_t i = 0; i < totalRecords; ++i) {
        PsoDescription& record = scan.records[i];
        const uint64_t attempt = i + 1;
        const HRESULT hr = outcomes[i].hr;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso = std::move(outcomes[i].pso);
        if (FAILED(hr) || !pso) {
            g_psoCacheCounters.prewarmFailed.fetch_add(1, std::memory_order_relaxed);
            if (ShouldLogPsoCensus(attempt)) {
                MCLA_LOG_WARN("PSO-PREWARM #{} ok=0 key={:016X} hr=0x{:08X} records={} "
                              "created={} failed={} stale={} corrupt={}",
                              attempt, HashPipelineKey(record.key), static_cast<uint32_t>(hr),
                              scan.records.size(), g_psoCacheCounters.prewarmCreated.load(),
                              g_psoCacheCounters.prewarmFailed.load(),
                              g_psoCacheCounters.prewarmStale.load(),
                              g_psoCacheCounters.prewarmCorrupt.load());
            }
            continue;
        }
        Insert(record.key, pso);
        {
            std::lock_guard<std::mutex> lock(mapMutex_);
            cbvBindings_[record.key] = std::move(record.cbvBindings);
            uint32_t smpMask = 0;
            for (const auto& res : record.psResources) {
                if (res.type == DxcRuntime::kSitSampler && res.space_ == 0 && res.register_ < 32u) {
                    smpMask |= (1u << res.register_);
                }
            }
            for (const auto& res : record.vsResources) {
                if (res.type == DxcRuntime::kSitSampler && res.space_ == 0 && res.register_ < 32u) {
                    smpMask |= (1u << res.register_);
                }
            }
            samplerMasks_[record.key] = smpMask;
            prewarmedKeys_.insert(record.key);
        }
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            if (record.key.state.flags & kStateFlagUnmeasuredDepth) ++stats_.unmeasuredDepthState;
            if (record.key.state.flags & kStateFlagUnmeasuredBlend) ++stats_.unmeasuredBlendState;
            if (record.key.state.geometryStage != kGeomStageNone) ++stats_.geomStageCreated;
        }
        g_psoCacheCounters.prewarmCreated.fetch_add(1, std::memory_order_relaxed);
        if (ShouldLogPsoCensus(attempt)) {
            MCLA_LOG_INFO("PSO-PREWARM #{} ok=1 key={:016X} records={} created={} failed={} "
                          "stale={} corrupt={}", attempt, HashPipelineKey(record.key),
                          scan.records.size(), g_psoCacheCounters.prewarmCreated.load(),
                          g_psoCacheCounters.prewarmFailed.load(),
                          g_psoCacheCounters.prewarmStale.load(),
                          g_psoCacheCounters.prewarmCorrupt.load());
        }
    }

    {
        std::lock_guard<std::mutex> lock(persistMutex_);
        for (const auto& record : scan.records) {
            persistedKeys_.insert(record.key);
        }
    }

    MCLA_LOG_INFO("PSO-PREWARM #0 disabled=0 path={} records={} examined={} created={} failed={} "
                  "stale={} corrupt={} threads={} positiveControl=1", psoCachePath_, scan.records.size(),
                  scan.examined, g_psoCacheCounters.prewarmCreated.load(),
                  g_psoCacheCounters.prewarmFailed.load(), g_psoCacheCounters.prewarmStale.load(),
                  g_psoCacheCounters.prewarmCorrupt.load(), actualThreads);
}

bool PipelineCache::AppendPsoDescription(
        const CompileTask& task,
        const std::vector<uint8_t>& vsDxil,
        const std::vector<uint8_t>& psDxil,
        const std::vector<uint8_t>& gsDxil,
        const std::vector<DxcRuntime::ResourceBinding>& vsResources,
        const std::vector<DxcRuntime::ResourceBinding>& psResources,
        const std::vector<ConstantBankBinding>& bindings,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
        uint64_t rootSignatureHash) {
    const uint64_t attempt = g_psoCacheCounters.persistAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
    const char* env = std::getenv("MCLA_NO_PSO_PERSIST");
    if (env && env[0] == '1') {
        if (ShouldLogPsoCensus(attempt))
            MCLA_LOG_INFO("PSO-PERSIST-SAVE #{} disabled=1 key={:016X} saved={} failed={}",
                          attempt, HashPipelineKey(task.key),
                          g_psoCacheCounters.persistSaved.load(),
                          g_psoCacheCounters.persistFailed.load());
        return false;
    }

    static const bool s_noPersistDedup = [] {
        const char* e = std::getenv("MCLA_NO_PSO_PERSIST_DEDUP");
        return e && e[0] == '1';
    }();

    std::lock_guard<std::mutex> lock(persistMutex_);

    if (!s_noPersistDedup && persistedKeys_.find(task.key) != persistedKeys_.end()) {
        const uint64_t skipped = g_psoCacheCounters.persistSkippedDedup.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ShouldLogPsoCensus(skipped)) {
            MCLA_LOG_INFO("PSO-PERSIST-DEDUP #{} key={:016X} skipped={} saved={}",
                          skipped, HashPipelineKey(task.key), skipped,
                          g_psoCacheCounters.persistSaved.load());
        }
        return true;
    }

    auto fail = [&](const char* reason, bool latch) {
        g_psoCacheCounters.persistFailed.fetch_add(1, std::memory_order_relaxed);
        if (latch) psoCacheWriteFailed_ = true;
        if (ShouldLogPsoCensus(attempt))
            MCLA_LOG_WARN("PSO-PERSIST-SAVE #{} ok=0 key={:016X} saved={} failed={} reason={}",
                          attempt, HashPipelineKey(task.key), g_psoCacheCounters.persistSaved.load(),
                          g_psoCacheCounters.persistFailed.load(), reason);
        return false;
    };
    if (psoCacheWriteFailed_) return fail("previous append failed; writes latched off", false);
    if (!rootSignatureHashValid_ || rootSignatureHash_ != rootSignatureHash)
        return fail("root-signature hash unavailable/changed", true);
    std::vector<uint8_t> payload;
    if (!WritePsoPayload(payload, task, vsDxil, psDxil, gsDxil, vsResources,
                         psResources, bindings, desc, rootSignatureHash))
        return fail("description exceeds format bounds", false);

    if (!psoCacheWritePrepared_) {
        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(psoCachePath_).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        if (ec) return fail("cache directory create failed", true);
        if (psoCacheValidBytes_ == 0) {
            const std::vector<uint8_t> header = MakePsoCacheHeader();
            std::ofstream out(psoCachePath_, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(header.data()), header.size());
            out.flush();
            if (!out) return fail("cache header write failed", true);
            psoCacheValidBytes_ = header.size();
        } else {
            const uint64_t currentSize = std::filesystem::file_size(psoCachePath_, ec);
            if (ec) return fail("cache size query failed", true);
            if (currentSize != psoCacheValidBytes_) {
                std::filesystem::resize_file(psoCachePath_, psoCacheValidBytes_, ec);
                if (ec) return fail("corrupt tail truncate failed", true);
            }
        }
        psoCacheWritePrepared_ = true;
    }

    const std::vector<uint8_t> recordHeader = MakePsoRecordHeader(payload);
    if (psoCacheValidBytes_ + recordHeader.size() + payload.size() > kPsoCacheMaxFileBytes)
        return fail("cache size bound reached", true);
    std::ofstream out(psoCachePath_, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char*>(recordHeader.data()), recordHeader.size());
    out.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    out.flush();
    if (!out) return fail("record append failed", true);
    psoCacheValidBytes_ += recordHeader.size() + payload.size();
    persistedKeys_.insert(task.key);
    g_psoCacheCounters.persistSaved.fetch_add(1, std::memory_order_relaxed);
    if (ShouldLogPsoCensus(attempt)) {
        MCLA_LOG_INFO("PSO-PERSIST-SAVE #{} ok=1 key={:016X} bytes={} saved={} failed={} path={}",
                      attempt, HashPipelineKey(task.key), payload.size(),
                      g_psoCacheCounters.persistSaved.load(),
                      g_psoCacheCounters.persistFailed.load(), psoCachePath_);
    }
    return true;
}

void PipelineCache::StopWorker() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopWorker_ = true;
        queueCv_.notify_all();
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

bool PipelineCache::RefuseOnce(const PipelineKey& key, uint32_t reason,
                               const char* caller, const char* detail) {
    uint64_t keyHash = HashPipelineKey(key);
    bool print = false;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.refusals;
        for (uint32_t bit = 0; bit < 32; ++bit)
            if (reason & (1u << bit)) ++stats_.refusalsByReason[bit];
        // F-646: the two aggregates a future soak's gate reads, so the gate does
        // not have to sum bits itself and cannot disagree with the loop above.
        if (reason & (kRefuseNoPaSuScModeCntl | kRefuseNoPaClipCntl |
                      kRefuseNoPaClVteCntl))
            ++stats_.missingRegisters;
        if (reason & kRefuseViewportExtentsUnmeasured)
            ++stats_.viewportUnresolved;
        // D1: the depth-bias magnitudes are a different kind of gap from a
        // missing rasteriser register, and the gate reads them apart.
        if (reason & (kRefuseNoPolyOffsetFront | kRefuseNoPolyOffsetBack |
                      kRefuseNoDepthInfo))
            ++stats_.polyOffsetUnresolved;
        // Bounded: the set is capped so a key-churning soak cannot grow it, and
        // so a soak is never dominated by one repeating refusal line (the
        // F-054 lesson).
        if (refusedKeys_.size() < kMaxRefusalKeys)
            print = refusedKeys_.insert(key).second;
    }
    if (print) {
        char reasonText[256] = {};
        FormatPipelineRefusal(reason, reasonText, sizeof(reasonText));
        std::fprintf(stderr,
                     "PipelineCache: REFUSING pipeline key=%016llX vs=%016llX ps=%016llX "
                     "decl=%016llX layout=%016llX topo=%u reason=%s caller=%s%s%s\n",
                     static_cast<unsigned long long>(keyHash),
                     static_cast<unsigned long long>(key.vsHash),
                     static_cast<unsigned long long>(key.psHash),
                     static_cast<unsigned long long>(key.vertexDeclHash),
                     static_cast<unsigned long long>(key.inputLayoutHash),
                     key.state.topology, reasonText, caller ? caller : "?",
                     detail ? " -- " : "", detail ? detail : "");
    }
    return print;
}

namespace {

// ---------------------------------------------------------------------------
// Which PS input semantics does no earlier stage produce?
// ---------------------------------------------------------------------------
// F-692 measured on build/w693_err.log (sha256 566788266910bc57e...c98): the
// refusal there read `reason=empty-hlsl -- CreateGraphicsPipelineState failed`
// while the HLSL for both stages was present and DXC had returned
// `failed=0`. The refusal misdescribed its own cause, which is a defect in
// itself.
//
// The cause, measured offline with the real translator, the real DXC and a real
// D3D12 device (harness `h2`/`h3`/`h4` under
// %TEMP%\opencode\pso, 2026-10-02):
//   VS program AEAF1F096E09B347  decodes to TWO instructions:
//       FETCH VertexFetch vfmt=0x25 dst=0x000
//       ALU  vecDest=0x03E EXPORT=1 vwMask=0xF
//   -- one export, and its destination register is 0x3E = 62, the Xenos position
//   export. PS program 7B0032AD4C5B576D decodes to ONE instruction,
//   `vecDest=0x000 EXPORT=1` (colour 0), and its interpolator table -- derived
//   from its own instruction stream, `interpolatorsDerived=1` -- is
//   `{usage=5 usageIndex=0 address=0 reg=0}` i.e. one input semantic TEXCOORD0.
//   So the pixel shader reads an interpolator the vertex shader never exports.
//   D3D12 links stage signatures by semantic name+index, so that input has no
//   producer and CreateGraphicsPipelineState returns 0x80070057.
//
//   Attribution, one fact changed at a time, same descriptor, same device:
//       real VS + real PS                       -> 0x80070057  (fails)
//       real VS + real PS minus its TEXCOORD0   -> 0x00000000  (PSO returned)
//       real VS plus a TEXCOORD0 output + realPS-> 0x00000000  (PSO returned)
//
// THIS FUNCTION IS THE DIAGNOSTIC, not a fix. It reports the unproduced names so
// the refusal can name its cause instead of guessing at one. It NEVER edits a
// signature: the fix is to pair a vertex program that exports the interpolator
// the pixel program reads, which is a decision about the DRAW, not about this
// cache.
struct StageSemantics {
    // "NAME" for NAME in {SV_Position, TEXCOORD0, ...} -- the form D3D12 links
    // by. SV_* names are system values: D3D12 supplies SV_Position to a pixel
    // shader from the rasteriser and takes SV_Position from a vertex shader, so
    // they are never reported as unproduced.
    std::vector<std::string> names;
    bool has(const std::string& n) const {
        for (const auto& x : names) if (x == n) return true;
        return false;
    }
};

// Parse the `: NAME` out of one `in`/`out` parameter line. Only the shapes this
// project's own translator emits are recognised (shader_translator.cpp:676-681
// for the VS signature, :698-706 for the PS signature); an unrecognised line is
// NOT claimed as a semantic, so a parser change can only ever lose evidence.
bool SemanticFromParamLine(std::string_view line, std::string* out) {
    const size_t colon = line.rfind(':');
    if (colon == std::string_view::npos) return false;
    std::string_view tail = line.substr(colon + 1);
    while (!tail.empty() && (tail.front() == ' ' || tail.front() == '\t')) tail.remove_prefix(1);
    // Strip the trailing comma / brace / comment.
    size_t end = 0;
    while (end < tail.size() &&
           ((tail[end] >= 'A' && tail[end] <= 'Z') || (tail[end] >= 'a' && tail[end] <= 'z') ||
            (tail[end] >= '0' && tail[end] <= '9') || tail[end] == '_')) {
        ++end;
    }
    if (end == 0) return false;
    out->assign(tail.substr(0, end));
    return true;
}

StageSemantics CollectSemantics(std::string_view hlsl, bool vertexStage) {
    StageSemantics s;
    size_t pos = 0;
    while (pos < hlsl.size()) {
        const size_t eol = hlsl.find('\n', pos);
        const std::string_view line =
            hlsl.substr(pos, (eol == std::string_view::npos ? hlsl.size() : eol) - pos);
        pos = (eol == std::string_view::npos) ? hlsl.size() : eol + 1;
        // A parameter line, not a statement: it must start with in/out after
        // leading spaces, and must not contain '=' (a body line would).
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const bool isIn = line.compare(i, 3, "in ") == 0;
        const bool isOut = line.compare(i, 4, "out ") == 0;
        if (!isIn && !isOut) continue;
        if (line.find('=') != std::string_view::npos) continue;
        std::string name;
        if (!SemanticFromParamLine(line, &name)) continue;
        if (vertexStage && !isOut) continue;   // a VS's `in` is the input layout
        if (!vertexStage && !isIn) continue;    // a PS's `out` is the RT
        s.names.push_back(name);
    }
    return s;
}

bool IsSystemValue(const std::string& n) {
    return n.rfind("SV_", 0) == 0;
}

// Names a PS input that no earlier stage's output produces. `produced` is the
// union of the VS output set and, when a geometry stage is bound, the stage's
// own output set -- which this module writes from the VS's set verbatim
// (BuildRectangleGeometryShaderHlsl), so the VS set IS the GS set.
std::vector<std::string> UnproducedPixelInputs(const StageSemantics& vsOut,
                                               const StageSemantics& psIn) {
    std::vector<std::string> missing;
    for (const auto& n : psIn.names) {
        if (IsSystemValue(n)) continue;   // D3D12 supplies/withdraws these
        if (vsOut.has(n)) continue;
        missing.push_back(n);
    }
    return missing;
}

std::string JoinNames(const std::vector<std::string>& v) {
    if (v.empty()) return "none";
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += "|";
        out += v[i];
    }
    return out;
}

std::string JoinBindings(
    const std::vector<DxcRuntime::ResourceBinding>& bindings) {
    if (bindings.empty()) return "none";
    std::string out;
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (i) out += "|";
        const auto& binding = bindings[i];
        out += std::to_string(binding.type) + ":" +
               std::to_string(binding.register_) + ":" +
               std::to_string(binding.space_);
    }
    return out;
}

std::string JoinSignature(
    const std::vector<DxcRuntime::SignatureParameter>& parameters) {
    if (parameters.empty()) return "none";
    std::string out;
    for (size_t i = 0; i < parameters.size(); ++i) {
        if (i) out += "|";
        const auto& parameter = parameters[i];
        out += parameter.semanticName + std::to_string(parameter.semanticIndex) +
               ":" + std::to_string(parameter.systemValueType) + ":" +
               std::to_string(parameter.componentType) + "/" +
               std::to_string(parameter.mask) + "/" +
               std::to_string(parameter.readWriteMask) + "@" +
               std::to_string(parameter.register_);
    }
    return out;
}

void LogPsoDebugMessages(ID3D12InfoQueue* infoQueue, UINT64 firstMessage) {
    static std::atomic<uint32_t> s_psoInfoQueueDumps{0};
    const uint32_t n = s_psoInfoQueueDumps.fetch_add(1) + 1;
    if (n > 4) return;
    if (!infoQueue) {
        MCLA_LOG_WARN("PC-PSO-D3D12-INFO #{} unavailable; enable "
                      "MCLA_D3D12_DEBUG_LAYER=1 before starting the process",
                      n);
        return;
    }

    const UINT64 messageCount =
        infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    if (messageCount <= firstMessage) {
        MCLA_LOG_WARN("PC-PSO-D3D12-INFO #{} no new debug-layer messages "
                      "followed CreateGraphicsPipelineState",
                      n);
        return;
    }

    constexpr UINT64 kMaxMessages = 8;
    const UINT64 lastMessage =
        std::min(messageCount, firstMessage + kMaxMessages);
    for (UINT64 i = firstMessage; i < lastMessage; ++i) {
        SIZE_T messageBytes = 0;
        const HRESULT sizeHr =
            infoQueue->GetMessage(i, nullptr, &messageBytes);
        if (messageBytes < sizeof(D3D12_MESSAGE)) {
            MCLA_LOG_WARN("PC-PSO-D3D12-INFO #{} message={} size-query-hr=0x{:08X} "
                          "returned an invalid message size {}",
                          n, i, static_cast<uint32_t>(sizeHr), messageBytes);
            continue;
        }

        std::vector<uint64_t> storage(
            (messageBytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        const HRESULT messageHr =
            infoQueue->GetMessage(i, message, &messageBytes);
        if (FAILED(messageHr)) {
            MCLA_LOG_WARN("PC-PSO-D3D12-INFO #{} message={} retrieval-hr=0x{:08X}",
                          n, i, static_cast<uint32_t>(messageHr));
            continue;
        }

        MCLA_LOG_WARN("PC-PSO-D3D12-INFO #{} message={} id={} severity={} {}",
                      n, i, static_cast<uint32_t>(message->ID),
                      static_cast<uint32_t>(message->Severity),
                      message->pDescription ? message->pDescription : "");
    }
    if (messageCount > lastMessage) {
        MCLA_LOG_WARN("PC-PSO-D3D12-INFO #{} omitted {} additional messages",
                      n, messageCount - lastMessage);
    }
}

void ProbeFailedPsoDescriptor(
    ID3D12Device* device, DxcRuntime& dxc,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
    const std::vector<uint8_t>& vsDxil,
    const std::vector<uint8_t>& psDxil) {
    static std::atomic<uint32_t> s_psoDescriptorProbes{0};
    const uint32_t n = s_psoDescriptorProbes.fetch_add(1) + 1;
    if (n != 1) return;

    MCLA_LOG_WARN("PC-PSO-FAIL-DESC #{} rtCount={} rt0={} dsv={} samples={}/{} "
                  "topologyType={} inputElements={} rootSignature={}",
                  n, original.NumRenderTargets,
                  static_cast<uint32_t>(original.RTVFormats[0]),
                  static_cast<uint32_t>(original.DSVFormat),
                  original.SampleDesc.Count, original.SampleDesc.Quality,
                  static_cast<uint32_t>(original.PrimitiveTopologyType),
                  original.InputLayout.NumElements,
                  original.pRootSignature ? 1u : 0u);
    const UINT inputCount = std::min(original.InputLayout.NumElements, 8u);
    for (UINT i = 0; i < inputCount; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& input =
            original.InputLayout.pInputElementDescs[i];
        MCLA_LOG_WARN("PC-PSO-FAIL-INPUT #{} index={} semantic={}{} format={} "
                      "slot={} offset={} class={} step={}",
                      n, i, input.SemanticName ? input.SemanticName : "",
                      input.SemanticIndex, static_cast<uint32_t>(input.Format),
                      input.InputSlot, input.AlignedByteOffset,
                      static_cast<uint32_t>(input.InputSlotClass),
                      input.InstanceDataStepRate);
    }

    std::vector<DxcRuntime::SignatureParameter> vsInputs, vsOutputs;
    std::vector<DxcRuntime::SignatureParameter> psInputs, psOutputs;
    std::string vsError, psError;
    const bool vsSignatureOk =
        dxc.ReflectSignature(vsDxil, vsInputs, vsOutputs, vsError);
    const bool psSignatureOk =
        dxc.ReflectSignature(psDxil, psInputs, psOutputs, psError);
    MCLA_LOG_WARN("PC-PSO-SIGNATURE #{} vsOk={} vsInputs={} vsOutputs={} "
                  "psOk={} psInputs={} psOutputs={} vsError={} psError={}",
                  n, vsSignatureOk ? 1u : 0u, JoinSignature(vsInputs),
                  JoinSignature(vsOutputs), psSignatureOk ? 1u : 0u,
                  JoinSignature(psInputs), JoinSignature(psOutputs),
                  vsError.empty() ? "none" : vsError,
                  psError.empty() ? "none" : psError);

    auto probe = [&](const char* variant,
                     const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
        Microsoft::WRL::ComPtr<ID3D12PipelineState> testPso;
        const HRESULT hr =
            device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&testPso));
        MCLA_LOG_WARN("PC-PSO-PROBE #{} variant={} hr=0x{:08X} created={}",
                      n, variant, static_cast<uint32_t>(hr),
                      testPso ? 1u : 0u);
    };

    PipelineState baseline = {};
    baseline.sampleCount = original.SampleDesc.Count;
    ApplyBaselineD3D12State(baseline);

    auto desc = original;
    desc.RasterizerState = baseline.d3d12.raster;
    probe("baseline-raster", desc);
    desc = original;
    desc.BlendState = baseline.d3d12.blend;
    probe("baseline-blend", desc);
    desc = original;
    desc.DepthStencilState = baseline.d3d12.depthStencil;
    probe("baseline-depth", desc);
    desc = original;
    desc.RasterizerState = baseline.d3d12.raster;
    desc.BlendState = baseline.d3d12.blend;
    desc.DepthStencilState = baseline.d3d12.depthStencil;
    probe("baseline-states", desc);
    desc = original;
    desc.InputLayout = {nullptr, 0};
    probe("empty-input-layout", desc);

    constexpr char kProbeVs[] =
        "struct V { float4 p : SV_Position; float4 t1 : TEXCOORD1; };"
        "V main(uint id : SV_VertexID) { V o; o.p=float4(0,0,0,1); "
        "o.t1=0; return o; }";
    constexpr char kProbeVsExtraOutput[] =
        "struct V { float4 p : SV_Position; float4 t0 : TEXCOORD0; "
        "float4 t1 : TEXCOORD1; };"
        "V main(uint id : SV_VertexID) { V o; o.p=float4(0,0,0,1); "
        "o.t0=0; o.t1=0; return o; }";
    constexpr char kProbeVsReorderedExtraOutput[] =
        "struct V { float4 p : SV_Position; float4 t1 : TEXCOORD1; "
        "float4 t0 : TEXCOORD0; };"
        "V main(uint id : SV_VertexID) { V o; o.p=float4(0,0,0,1); "
        "o.t1=0; o.t0=0; return o; }";
    constexpr char kProbePs[] =
        "float4 main() : SV_Target0 { return float4(1,0,1,1); }";
    constexpr char kProbePsWithUnusedInput[] =
        "float4 main(float4 p : SV_Position, float4 t0 : TEXCOORD0, "
        "float4 t1 : TEXCOORD1) : SV_Target0 { return t1; }";
    std::vector<uint8_t> probeVs, probeVsExtraOutput,
        probeVsReorderedExtraOutput, probePs, probePsWithUnusedInput;
    std::string compileError;
    const bool probeVsCompiled = dxc.Compile(
        kProbeVs, "main", DxcRuntime::kVertexProfile, probeVs, compileError, false);
    if (!probeVsCompiled) {
        MCLA_LOG_WARN("PC-PSO-PROBE #{} variant=simple-vs-compile failed={}",
                      n, compileError);
        return;
    }
    if (!dxc.Compile(kProbeVsExtraOutput, "main", DxcRuntime::kVertexProfile,
                     probeVsExtraOutput, compileError, false)) {
        MCLA_LOG_WARN("PC-PSO-PROBE #{} variant=simple-vs-extra-output-compile "
                      "failed={}",
                      n, compileError);
        return;
    }
    if (!dxc.Compile(kProbeVsReorderedExtraOutput, "main",
                     DxcRuntime::kVertexProfile,
                     probeVsReorderedExtraOutput, compileError, false)) {
        MCLA_LOG_WARN("PC-PSO-PROBE #{} variant=reordered-extra-output-vs-compile "
                      "failed={}",
                      n, compileError);
        return;
    }
    const bool probePsCompiled = dxc.Compile(
        kProbePs, "main", DxcRuntime::kPixelProfile, probePs, compileError, false);
    if (!probePsCompiled) {
        MCLA_LOG_WARN("PC-PSO-PROBE #{} variant=simple-ps-compile failed={}",
                      n, compileError);
        return;
    }
    if (!dxc.Compile(kProbePsWithUnusedInput, "main",
                     DxcRuntime::kPixelProfile, probePsWithUnusedInput,
                     compileError, false)) {
        MCLA_LOG_WARN("PC-PSO-PROBE #{} variant=extra-input-ps-compile failed={}",
                      n, compileError);
        return;
    }

    desc = original;
    desc.RasterizerState = baseline.d3d12.raster;
    desc.BlendState = baseline.d3d12.blend;
    desc.DepthStencilState = baseline.d3d12.depthStencil;
    desc.VS = {probeVs.data(), probeVs.size()};
    desc.PS = {probePs.data(), probePs.size()};
    desc.InputLayout = {nullptr, 0};
    probe("simple-shader-pair", desc);

    desc = original;
    desc.RasterizerState = baseline.d3d12.raster;
    desc.BlendState = baseline.d3d12.blend;
    desc.DepthStencilState = baseline.d3d12.depthStencil;
    desc.PS = {probePs.data(), probePs.size()};
    probe("guest-vs-simple-ps", desc);

    desc = original;
    desc.RasterizerState = baseline.d3d12.raster;
    desc.BlendState = baseline.d3d12.blend;
    desc.DepthStencilState = baseline.d3d12.depthStencil;
    desc.VS = {probeVs.data(), probeVs.size()};
    desc.InputLayout = {nullptr, 0};
    probe("simple-vs-guest-ps", desc);

    std::vector<DxcRuntime::SignatureParameter> extraVsInputs, extraVsOutputs;
    std::string extraVsError;
    const bool extraVsSignatureOk = dxc.ReflectSignature(
        probeVsExtraOutput, extraVsInputs, extraVsOutputs, extraVsError);
    MCLA_LOG_WARN("PC-PSO-PROBE-SIGNATURE #{} variant=simple-vs-extra-output "
                  "ok={} outputs={} error={}",
                  n, extraVsSignatureOk ? 1u : 0u,
                  JoinSignature(extraVsOutputs),
                  extraVsError.empty() ? "none" : extraVsError);
    desc = original;
    desc.RasterizerState = baseline.d3d12.raster;
    desc.BlendState = baseline.d3d12.blend;
    desc.DepthStencilState = baseline.d3d12.depthStencil;
    desc.VS = {probeVsExtraOutput.data(), probeVsExtraOutput.size()};
    desc.InputLayout = {nullptr, 0};
    probe("simple-vs-extra-output-guest-ps", desc);

    std::vector<DxcRuntime::SignatureParameter> reorderedVsInputs,
        reorderedVsOutputs;
    std::string reorderedVsError;
    const bool reorderedVsSignatureOk = dxc.ReflectSignature(
        probeVsReorderedExtraOutput, reorderedVsInputs, reorderedVsOutputs,
        reorderedVsError);
    MCLA_LOG_WARN("PC-PSO-PROBE-SIGNATURE #{} "
                  "variant=reordered-extra-output-vs ok={} outputs={} error={}",
                  n, reorderedVsSignatureOk ? 1u : 0u,
                  JoinSignature(reorderedVsOutputs),
                  reorderedVsError.empty() ? "none" : reorderedVsError);
    desc = original;
    desc.RasterizerState = baseline.d3d12.raster;
    desc.BlendState = baseline.d3d12.blend;
    desc.DepthStencilState = baseline.d3d12.depthStencil;
    desc.VS = {probeVsReorderedExtraOutput.data(),
               probeVsReorderedExtraOutput.size()};
    desc.InputLayout = {nullptr, 0};
    probe("reordered-extra-output-vs-guest-ps", desc);

    std::vector<DxcRuntime::SignatureParameter> extraPsInputs, extraPsOutputs;
    std::string extraPsError;
    const bool extraPsSignatureOk = dxc.ReflectSignature(
        probePsWithUnusedInput, extraPsInputs, extraPsOutputs, extraPsError);
    MCLA_LOG_WARN("PC-PSO-PROBE-SIGNATURE #{} variant=unused-extra-ps-input "
                  "ok={} inputs={} error={}",
                  n, extraPsSignatureOk ? 1u : 0u,
                  JoinSignature(extraPsInputs),
                  extraPsError.empty() ? "none" : extraPsError);
    desc = original;
    desc.RasterizerState = baseline.d3d12.raster;
    desc.BlendState = baseline.d3d12.blend;
    desc.DepthStencilState = baseline.d3d12.depthStencil;
    desc.PS = {probePsWithUnusedInput.data(), probePsWithUnusedInput.size()};
    probe("guest-vs-unused-extra-ps-input", desc);
}

// ONE line, bounded (first 4 then every 200th) so a soak can count it without
// the log becoming the fault -- the F-054 lesson, applied to the new marker.
void LogPsoCreateFailed(HRESULT hr, size_t vsDxilBytes, size_t psDxilBytes,
                        size_t gsDxilBytes, bool wantGeomStage,
                        const std::vector<std::string>& unproduced,
                        const std::vector<DxcRuntime::ResourceBinding>& vsBindings,
                        const std::vector<DxcRuntime::ResourceBinding>& psBindings,
                        const StageSemantics& vsOut, const StageSemantics& psIn) {
    static std::atomic<uint32_t> s_psoFail{0};
    const uint32_t n = s_psoFail.fetch_add(1) + 1;
    if (!(n <= 4 || (n % 200) == 0)) return;
    char hrText[16] = {};
    std::snprintf(hrText, sizeof(hrText), "0x%08lX", static_cast<unsigned long>(hr));
    MCLA_LOG_WARN("PC-PSO-CREATE-FAILED #{}: hr={} vsDxil={} psDxil={} gsDxil={} "
                  "geomStage={} psInputs={} vsOutputs={} unproduced={} "
                  "vsBindings={} psBindings={} -- "
                  "CreateGraphicsPipelineState rejected the pipeline after "
                  "successful shader compilation. unproduced is a name-only "
                  "signature check, not a complete diagnosis; use "
                  "PC-PSO-D3D12-INFO for debug-layer details when available.",
                  n, hrText, vsDxilBytes, psDxilBytes,
                  wantGeomStage ? gsDxilBytes : 0, wantGeomStage ? 1 : 0,
                  JoinNames(psIn.names), JoinNames(vsOut.names),
                  JoinNames(unproduced), JoinBindings(vsBindings),
                  JoinBindings(psBindings));
}

// PERF-STAGES: always accumulated (two clock reads per compile task / per miss wait; both rare).
std::atomic<int64_t> g_pcCompileNs{0}, g_pcCompileMaxNs{0}, g_pcWaitNs{0}, g_pcWaitMaxNs{0};
std::atomic<uint32_t> g_pcCompiles{0}, g_pcWaits{0}, g_pcNonGeomFallbacks{0};
int64_t PcNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void PcAdd(std::atomic<int64_t>& sum, std::atomic<int64_t>& mx, std::atomic<uint32_t>& n, int64_t ns) {
    sum.fetch_add(ns, std::memory_order_relaxed);
    n.fetch_add(1, std::memory_order_relaxed);
    int64_t c = mx.load(std::memory_order_relaxed);
    while (ns > c && !mx.compare_exchange_weak(c, ns, std::memory_order_relaxed)) {}
}

}  // namespace

std::string PerfPsoRow() {
    const uint32_t nc = g_pcCompiles.exchange(0), nw = g_pcWaits.exchange(0);
    const uint32_t nf = g_pcNonGeomFallbacks.exchange(0);
    const int64_t cns = g_pcCompileNs.exchange(0), cmx = g_pcCompileMaxNs.exchange(0);
    const int64_t wns = g_pcWaitNs.exchange(0), wmx = g_pcWaitMaxNs.exchange(0);
    return "psoWorker[compiles=" + std::to_string(nc) + " totMs=" + std::to_string(cns / 1000000) +
           " maxMs=" + std::to_string(cmx / 1000000) + "] psoWait[n=" + std::to_string(nw) +
           " totMs=" + std::to_string(wns / 1000000) + " maxMs=" + std::to_string(wmx / 1000000) +
           "] nonGeomFallback=" + std::to_string(nf) + " ";
}

// PERF: Unified shader cache and DXIL disk cache (dxil_cache/ beside the exe's cwd).
// Priority order:
//  1. In-memory precompiled shader cache (build/shader_cache.bin, MCSC) via dxc.TryLookupPrecompiledDxil.
//  2. DXIL disk cache (dxil_cache/hash_profile.dxil), unless disabled via MCLA_NO_DXIL_DISK_CACHE=1 or MCLA_NO_DXIL_CACHE=1.
//  3. JIT compilation via dxc.Compile(..., checkPrecompiled=false) and write-through to dxil_cache/.
static bool CompileDxilCached(const DxcRuntime& dxc, const std::string& hlsl, const char* profile,
                              std::vector<uint8_t>& dxil, std::string& error) {
    static std::atomic<uint32_t> s_mcscHits{0};
    static std::atomic<uint32_t> s_dxilDiskHits{0};
    static std::atomic<uint32_t> s_dxcCompiles{0};
    static std::atomic<uint32_t> s_dxcFailures{0};

    auto LogStats = [](uint32_t total) {
        if (total <= 4 || total % 200 == 0) {
            MCLA_LOG_INFO("[SHADER-CACHE-STATS] total={} mcsc_hits={} dxil_disk_hits={} dxc_compiles={} dxc_failed={}",
                          total, s_mcscHits.load(std::memory_order_relaxed),
                          s_dxilDiskHits.load(std::memory_order_relaxed),
                          s_dxcCompiles.load(std::memory_order_relaxed),
                          s_dxcFailures.load(std::memory_order_relaxed));
        }
    };

    // (a) First check precompiled in-memory shader cache (MCSC)
    if (dxc.TryLookupPrecompiledDxil(hlsl, "main", profile, true, dxil)) {
        const uint32_t hits = s_mcscHits.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint32_t total = hits + s_dxilDiskHits.load(std::memory_order_relaxed) +
                               s_dxcCompiles.load(std::memory_order_relaxed);
        LogStats(total);
        return true;
    }

    // Check if DXIL disk cache is disabled
    static const bool s_noDxilDiskCache = [] {
        const char* e1 = std::getenv("MCLA_NO_DXIL_DISK_CACHE");
        const char* e2 = std::getenv("MCLA_NO_DXIL_CACHE");
        return (e1 && e1[0] == '1') || (e2 && e2[0] == '1');
    }();

    uint64_t h = 14695981039346656037ull;
    for (const char* c = profile; *c; ++c) h = (h ^ uint8_t(*c)) * 1099511628211ull;
    for (const char c : hlsl) h = (h ^ uint8_t(c)) * 1099511628211ull;
    const uint64_t len = hlsl.size(), h2 = std::hash<std::string>{}(hlsl);
    const std::string path = fmt::format("dxil_cache/{:016X}_{}.dxil", h, profile);

    // (b) On MCSC miss, unless disk cache is disabled, check dxil_cache/
    if (!s_noDxilDiskCache) {
        if (std::ifstream in{path, std::ios::binary}) {
            uint64_t hdr[3] = {};
            if (in.read(reinterpret_cast<char*>(hdr), sizeof hdr) && hdr[0] == 0x31434C4958440000ull &&
                hdr[1] == len && hdr[2] == h2) {
                dxil.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                if (!dxil.empty()) {
                    const uint32_t hits = s_dxilDiskHits.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (hits <= 2 || hits % 500 == 0) {
                        MCLA_LOG_INFO("DXIL-CACHE hit#{} {}", hits, path);
                    }
                    const uint32_t total = s_mcscHits.load(std::memory_order_relaxed) + hits +
                                           s_dxcCompiles.load(std::memory_order_relaxed);
                    LogStats(total);
                    return true;
                }
            }
        }
    }

    // (c) On miss, invoke DXC compiler (checkPrecompiled = false to avoid duplicate MCSC lookup/miss count)
    const bool compiled = dxc.Compile(hlsl, "main", profile, dxil, error, true, /*checkPrecompiled=*/false);
    if (!compiled) {
        s_dxcFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uint32_t compiles = s_dxcCompiles.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t total = s_mcscHits.load(std::memory_order_relaxed) +
                           s_dxilDiskHits.load(std::memory_order_relaxed) + compiles;
    LogStats(total);

    if (!s_noDxilDiskCache) {
        std::error_code ec;
        std::filesystem::create_directories("dxil_cache", ec);
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
            const uint64_t hdr[3] = {0x31434C4958440000ull, len, h2};
            out.write(reinterpret_cast<const char*>(hdr), sizeof hdr);
            out.write(reinterpret_cast<const char*>(dxil.data()), static_cast<std::streamsize>(dxil.size()));
            if (!out) return true;  // the compile succeeded; only the cache write failed
        }
        std::filesystem::rename(tmp, path, ec);
    }
    return true;
}

void PipelineCache::WorkerLoop(Microsoft::WRL::ComPtr<ID3D12Device> device,
                               Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature) {  mcla::diag::NameThisThread("pso-worker");
    // DXC COM compiler objects are not thread-safe; the worker owns one runtime
    // for its lifetime, lazily loaded on the first task.
    DxcRuntime dxc;
    bool dxcLoaded = false;
    // Latched on the first load failure so a missing dxcompiler.dll produces ONE
    // loud line instead of one full error text per queued draw.
    bool dxcDead = false;

    // F-716: publish liveness under queueMutex_, which is the only lock
    // GetOrCompile's waiter and this thread both take when reading/writing it.
    // Set BEFORE the first wait so a task queued immediately after StartWorker
    // does not see "not alive" and refuse a PSO that is about to exist.
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        workerAlive_ = true;
    }

    while (true) {
        CompileTask task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] { return stopWorker_ || !taskQueue_.empty(); });
            if (stopWorker_ && taskQueue_.empty()) {
                // F-716: only now, with the queue provably drained, is it true
                // that no queued task will ever be fulfilled. Cleared under the
                // SAME lock the waiter reads, so the two cannot disagree.
                workerAlive_ = false;
                return;
            }
            if (taskQueue_.empty()) continue;
            task = std::move(taskQueue_.front());
            taskQueue_.pop();
        }

        // F-716: drop this key's in-flight future when the task ends, whatever
        // path it took. A waiter that already copied the shared_future keeps a
        // live reference, so erasing here cannot break a wait in progress; it
        // only stops the map growing for every key the run ever saw.
        struct DropInflightFuture {
            PipelineCache* self;
            PipelineKey key;
            int64_t t0;  // PERF-STAGES psoWorker: whole task, refusals included
            ~DropInflightFuture() {
                PcAdd(g_pcCompileNs, g_pcCompileMaxNs, g_pcCompiles, PcNowNs() - t0);
                std::lock_guard<std::mutex> lock(self->inflightMutex_);
                self->inflightFutures_.erase(key);
            }
        } dropInflightFuture{this, task.key, PcNowNs()};

        // Everything below is a refusal path or a success. A failure NEVER
        // reaches Insert() and never caches a null PSO: a cached failure would
        // make every later lookup of this key report a hit on nothing. Instead,
        // permanent key failures go into the bounded negative cache (failedKeys_)
        // so GetOrCompile returns nullptr in O(1) without re-queueing DXC/PSO churn.
        auto failTask = [&](uint32_t reason, const char* detail, bool cacheFailure = true) {
            RefuseOnce(task.key, reason, "pipeline_cache.cpp:WorkerLoop", detail);
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (cacheFailure) RecordFailedKeyLocked(task.key);
                pendingKeys_.erase(task.key);
            }
            task.promise.set_value(nullptr);
        };

        if (!device) {
            failTask(kRefuseNoDevice, "worker has no ID3D12Device (StartWorker device was null)", false);
            continue;
        }
        if (!rootSignature) {
            // The worker must NOT build a private root signature here. The
            // command list binds the backend's root signature at draw time
            // (d3d12_backend.cpp:1457 SetGraphicsRootSignature(m_rootSignature)),
            // so a PSO built against a different one describes descriptor tables
            // that do not exist when the draw executes.
            failTask(kRefuseNoRootSignature,
                     "StartWorker was given no root signature; refusing to fabricate one");
            continue;
        }
        // psHlsl empty = depth-only PSO (PS = null), only the VS is required
        if (task.vsHlsl.empty()) {
            failTask(kRefuseEmptyHlsl, "no translated HLSL for a stage");
            continue;
        }
        if (task.inputLayout.empty()) {
            failTask(kRefuseEmptyInputLayout, "no input layout for a VS-bearing pipeline");
            continue;
        }
        if (task.key.refuseMask != kRefuseNone) {
            failTask(task.key.refuseMask, "key carries a refusal from ComputePipelineKey");
            continue;
        }
        if (HashInputLayout(task.inputLayout) != task.key.inputLayoutHash) {
            // The layout handed to the PSO is not the layout the key hashed.
            failTask(kRefuseLayoutDeclHashMismatch,
                     "input layout changed after ComputePipelineKeyForLayout");
            continue;
        }

        if (!dxcLoaded && !dxcDead) {
            std::string usedDir, error;
            if (!dxc.Load("", usedDir, error)) {
                dxcDead = true;
                {
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    ++stats_.dxcUnavailable;
                }
                // One loud, complete line: every real PSO for the rest of the
                // process will be missing, so this must not look like a
                // per-draw hiccup.
                std::fprintf(stderr,
                             "PipelineCache: FATAL no DXC runtime -- %s. Every real pipeline "
                             "will stay on the test PSO for the life of the process. Set "
                             "MCLA_DXC_DIR to a directory holding dxil.dll + dxcompiler.dll, "
                             "or place them beside mcla.exe (see dxc_runtime.h).\n",
                             error.c_str());
                std::fflush(stderr);
                failTask(kRefuseNoDevice, "DXC runtime unavailable (see the FATAL line above)", false);
                continue;
            }
            dxcLoaded = true;
            std::fprintf(stderr, "PipelineCache: DXC runtime loaded from %s\n", usedDir.c_str());
        }
        if (dxcDead) {
            failTask(kRefuseNoDevice, "DXC runtime unavailable (latched)", false);
            continue;
        }

        std::vector<uint8_t> vsDxil, psDxil;
        std::string error;
        // Compile WITH reflection data: the CBV slots drive both the constant
        // upload (PackXenosConstantBank) and the refusal when a shader binds a
        // resource the shared root signature does not declare.
        if (!CompileDxilCached(dxc, task.vsHlsl, DxcRuntime::kVertexProfile, vsDxil, error)) {
            {
                std::lock_guard<std::mutex> lock(statsMutex_);
                ++stats_.compileFailures;
            }
            RefuseOnce(task.key, kRefuseEmptyHlsl, "pipeline_cache.cpp:WorkerLoop(vs)",
                       error.c_str());
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                RecordFailedKeyLocked(task.key);
                pendingKeys_.erase(task.key);
            }
            task.promise.set_value(nullptr);
            continue;
        }
        // Targeted dump of one PS by hash (MCLA_DUMP_PS_HASH=<hex>): no cadence cost,
        // used to inspect the cutscene character shader's texture sampling.
        {
            static const uint64_t s_dumpPsHash = [] {
                const char* e = std::getenv("MCLA_DUMP_PS_HASH");
                return e && *e ? std::strtoull(e, nullptr, 16) : 0ULL;
            }();
            if (s_dumpPsHash != 0 && task.key.psHash == s_dumpPsHash) {
                std::error_code ec; std::filesystem::create_directories("ps_hlsl", ec);
                char fn[128];
                std::snprintf(fn, sizeof fn, "ps_hlsl/%016llX.ps.hlsl",
                              (unsigned long long)task.key.psHash);
                std::ofstream(fn) << task.psHlsl;
                std::snprintf(fn, sizeof fn, "ps_hlsl/%016llX.vs.hlsl",
                              (unsigned long long)task.key.psHash);
                std::ofstream(fn) << task.vsHlsl;
            }
        }
        {  // TEMP-G4: dump each distinct translated VS/PS pair once (build/shaders/).
            static const bool s_dumpShaders = [] {
                const char* e = std::getenv("MCLA_DUMP_SHADERS");
                return e && e[0] == '1';
            }();
            if (s_dumpShaders) {
                static std::mutex s_g4m; static std::set<size_t> s_g4seen;
                const size_t hk = std::hash<std::string>{}(task.vsHlsl + task.psHlsl);
                bool fresh = false;
                { std::lock_guard<std::mutex> lk(s_g4m); if (s_g4seen.size() < 200) fresh = s_g4seen.insert(hk).second; }
                if (fresh) {
                    std::error_code ec; std::filesystem::create_directories("shaders", ec);
                    const std::string base = fmt::format("shaders/pair_{:016X}", static_cast<uint64_t>(hk));
                    std::ofstream(base + "_vs.hlsl") << task.vsHlsl;
                    std::ofstream(base + "_ps.hlsl") << task.psHlsl;
                }
            }
        }
        if (!task.psHlsl.empty() &&
            !CompileDxilCached(dxc, task.psHlsl, DxcRuntime::kPixelProfile, psDxil, error)) {
            {
                std::lock_guard<std::mutex> lock(statsMutex_);
                ++stats_.compileFailures;
            }
            RefuseOnce(task.key, kRefuseEmptyHlsl, "pipeline_cache.cpp:WorkerLoop(ps)",
                       error.c_str());
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                RecordFailedKeyLocked(task.key);
                pendingKeys_.erase(task.key);
            }
            task.promise.set_value(nullptr);
            continue;
        }

        // Reflect CBV bindings from both stages and check them against the root
        // signature the draw path binds (t0..t3, b0, s0).
        std::vector<DxcRuntime::ResourceBinding> vsRes, psRes;
        if (!dxc.ReflectBindings(vsDxil, vsRes, error) ||
            (!psDxil.empty() && !dxc.ReflectBindings(psDxil, psRes, error))) {
            std::fprintf(stderr, "PipelineCache: reflection failed: %s\n", error.c_str());
        }
        std::vector<ConstantBankBinding> bindings;
        uint32_t resourceRefusal = kRefuseNone;
        uint32_t badReg = 0, badSpace = 0, maxTexReg = 0;
        uint32_t activeSamplerMask = 0;
        for (const std::vector<DxcRuntime::ResourceBinding>* stage : {&vsRes, &psRes}) {
            for (const auto& res : *stage) {
                if (res.type == DxcRuntime::kSitSampler) {
                    if (res.space_ == 0 && res.register_ < 32u) {
                        activeSamplerMask |= (1u << res.register_);
                    }
                    static const bool s_guestSamplers = [] {
                        const char* e = std::getenv("MCLA_TEMP_GUEST_SAMPLER");
                        return !(e != nullptr && e[0] == '0');
                    }();
                    const uint32_t samplerLimit = s_guestSamplers ? kShaderSamplerCount : (kShaderSamplerRegister + 1u);
                    if (res.register_ >= samplerLimit || res.space_ != 0) {
                        resourceRefusal |= kRefuseSamplerRegister;
                        badReg = res.register_;
                        badSpace = res.space_;
                    }
                    continue;
                }
                if (res.type == DxcRuntime::kSitCbuffer) {
                    const ConstantBankBinding b =
                            ResolveConstantBankBinding(DxcRuntime::CbvBinding{
                                res.register_, res.space_, res.byteSize, res.firstVariableRegister});
                    if (!b.valid) {
                        resourceRefusal |= kRefuseNoRootSignature;
                    } else {
                        bindings.push_back(b);
                    }
                    continue;
                }
                // SRV (and anything else): must live inside the t0..t3 table.
                if (res.type == DxcRuntime::kSitTexture ||
                    res.type == DxcRuntime::kSitStructuredBuffer ||
                    res.type == DxcRuntime::kSitByteAddressBuffer) {
                    // RAWVB: the raw guest VB is root SRV param 4 = ByteAddressBuffer t0,space1.
                    if (res.type == DxcRuntime::kSitByteAddressBuffer && res.space_ == 1 && res.register_ == 0)
                        continue;
                    maxTexReg = std::max(maxTexReg, res.register_);
                    if (res.space_ != 0 || res.register_ >= kShaderTextureSlotCount) {
                        resourceRefusal |= kRefuseOverTextureSlots;
                        badReg = res.register_; badSpace = res.space_;
                    }
                }
            }
        }
        if (maxTexReg >= 16u)
            MCLA_LOG_WARN("PSO-TEX-HIGH-SLOT vs={:016X} ps={:016X} maxTexRegister={} refused={}",
                          task.key.vsHash, task.key.psHash, maxTexReg, resourceRefusal != kRefuseNone ? 1 : 0);
        if (resourceRefusal != kRefuseNone) {
            static char s_detail[160];
            const char regPrefix = (resourceRefusal & kRefuseSamplerRegister) &&
                                           !(resourceRefusal & kRefuseOverTextureSlots)
                                       ? 's'
                                       : 't';
            std::snprintf(s_detail, sizeof(s_detail),
                          "reflected resource is outside the shared root signature (t0..t%u, b0, s0..s%u / s0): register %c%u space %u",
                          kShaderTextureSlotCount - 1u, kShaderSamplerCount - 1u, regPrefix, badReg, badSpace);
            failTask(resourceRefusal, s_detail);
            continue;
        }
        // TEMP F-900 (remove after VS->PS linkage is settled): log the COMPILED
        // signatures (DXIL reflection, not HLSL text) for the first few PSOs.
        // The dumped HLSL shows matching TEXCOORD0/1 on both stages while the
        // PS reads zeros at runtime; this names whether the COMPILED blobs
        // agree with the dump. Bounded: first 4 only.
        {
            static std::atomic<uint32_t> s_sigDiag{0};
            const uint32_t sd = s_sigDiag.fetch_add(1) + 1;
            if (sd <= 4) {
                std::vector<DxcRuntime::SignatureParameter> vsIn, vsOut, psIn, psOut;
                std::string sigErr;
                const bool vsSigOk = dxc.ReflectSignature(vsDxil, vsIn, vsOut, sigErr);
                const bool psSigOk = dxc.ReflectSignature(psDxil, psIn, psOut, sigErr);
                MCLA_LOG_WARN("PC-PSO-SIG #{} keyVsHash={:016X} vsSigOk={} vsIn=[{}] vsOut=[{}] "
                              "psSigOk={} psIn=[{}] psOut=[{}] vsHasVsuv={} psHasUvvis={}",
                              sd, task.key.vsHash, vsSigOk ? 1u : 0u,
                              JoinSignature(vsIn), JoinSignature(vsOut),
                              psSigOk ? 1u : 0u, JoinSignature(psIn),
                              JoinSignature(psOut),
                              task.vsHlsl.find("0.9922") != std::string::npos ? 1u : 0u,
                              task.psHlsl.find("frac(r0") != std::string::npos ? 1u : 0u);
            }
        }
        // A VS that declares no CBV still reads ShaderConstants in the generated
        // HLSL body, so the draw path always binds b0; record it so the bind
        // change is uniform.
        if (bindings.empty()) {
            bindings.push_back(ResolveConstantBankBinding(
                DxcRuntime::CbvBinding{kShaderConstantRootRegister, 0, 0, 0}));
        }

        // F-653: the geometry stage, if this key asks for one. Built and
        // compiled HERE, before the PSO, because a PSO that names a stage it
        // does not have is not a smaller mistake than no PSO at all: it is the
        // rectangle draw going through unexpanded, which is the exact outcome
        // the old 0x08 refusal was avoiding, except now with no log line.
        std::vector<uint8_t> gsDxil;
        const bool wantGeomStage = task.key.state.geometryStage != kGeomStageNone;
        if (wantGeomStage) {
            // F-653: the MEASURED device capability gates this, not an
            // assumption about the requested feature level. See
            // SetGeometryStageSupported: the default is false, so a cache nobody
            // told refuses rectangles rather than compiling a PSO the runtime
            // would reject.
            if (!geometryStageSupported_) {
                static std::atomic<uint32_t> s_noGs = {};
                const uint32_t n = s_noGs.fetch_add(1) + 1;
                if (n <= 4 || (n % 200) == 0)
                    MCLA_LOG_WARN("PC-RECT-GS-UNSUPPORTED #{}: this device was "
                                  "measured below the feature level a geometry "
                                  "stage requires (11_0), so the rectangle draw is "
                                  "REFUSED, not drawn unexpanded",
                                  n);
                failTask(kRefuseGeometryStageUnsupported,
                         "pipeline_cache.cpp:WorkerLoop(gs): device measured "
                         "without geometry-stage support");
                continue;
            }
            if (task.key.state.geometryStage != kGeomStageRectangleList) {
                // A geometryStage value this build does not implement. Refused
                // rather than treated as "no stage", because "no stage" is a
                // different pipeline and would be served from the wrong cache
                // entry without a word.
                failTask(kRefuseGeometryStageUnsupported,
                         "pipeline_cache.cpp:WorkerLoop(gs): unknown geometry stage");
                continue;
            }
            const std::string gsHlsl =
                BuildRectangleGeometryShaderHlsl(task.vsHlsl,
                                                 "pipeline_cache.cpp:WorkerLoop(gs)");
            if (gsHlsl.empty()) {
                {
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    ++stats_.geomStageUnbuildable;
                }
                failTask(kRefuseGeometryShaderUnbuildable,
                         "pipeline_cache.cpp:WorkerLoop(gs): stage HLSL not derivable "
                         "from the translated VS output signature");
                continue;
            }
            std::string gsError;
            // kGeometryProfile is "gs_6_0" (pipeline_cache.h), NOT
            // DxcRuntime::kVertexProfile: handing dxc a vertex profile for a
            // shader that declares `maxvertexcount` compiles to something that
            // is not a geometry shader, or fails outright.
            if (!dxc.Compile(gsHlsl, "main", kGeometryProfile, gsDxil, gsError, true)) {
                {
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    ++stats_.geomStageCompileFailed;
                }
                {
                    static std::atomic<uint32_t> s_gsFail = {};
                    const uint32_t n = s_gsFail.fetch_add(1) + 1;
                    if (n <= 4 || (n % 200) == 0)
                        MCLA_LOG_WARN("PC-RECT-GS-COMPILE-FAILED #{}: {} "
                                      "(-- profile {})",
                                      n, gsError, kGeometryProfile);
                }
                failTask(kRefuseGeometryShaderUnbuildable,
                         "pipeline_cache.cpp:WorkerLoop(gs): stage did not compile");
                continue;
            }
        }

        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        const PipelineState& st = task.key.state;
        {
            // The root signature is the BACKEND'S, handed to StartWorker by
            // D3D12Backend::CreateTestRootSignature and threaded through
            // WorkerLoop; this module never describes or serialises one, so
            // F-644's parameter reorder needs no edit here -- only the index
            // ConstantBankBinding::rootParameter names has to move.
            psoDesc.pRootSignature = rootSignature.Get();
            psoDesc.VS = { vsDxil.data(), vsDxil.size() };
            psoDesc.PS = { psDxil.empty() ? nullptr : psDxil.data(), psDxil.size() };
            // F-653: the expansion stage. Set only when the key named one, and
            // only from a blob that compiled above -- there is no path here that
            // binds a stage it does not hold, and no path that silently omits
            // one the key asked for.
            if (wantGeomStage) {
                psoDesc.GS = { gsDxil.data(), gsDxil.size() };
            }
            psoDesc.InputLayout = { task.inputLayout.data(),
                                    static_cast<UINT>(task.inputLayout.size()) };
            switch (st.topology) {
                case 1:  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
                case 2:  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
                default: psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
            }
            // Target formats come from the KEY, not from a hardcoded
            // R8G8B8A8_UNORM: a PSO whose RT format disagrees with the RTV it is
            // used with is rejected by the runtime, and one that agrees by luck
            // is not something to rely on.
            UINT numRt = 0;
            for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
                psoDesc.RTVFormats[i] = static_cast<DXGI_FORMAT>(st.targetFormats[i]);
                if (psoDesc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN) numRt = i + 1;
            }
            const bool depthOnlyNoPs =
                (numRt == 0) &&
                (static_cast<DXGI_FORMAT>(st.depthStencilFormat) != DXGI_FORMAT_UNKNOWN) &&
                task.psHlsl.empty();
            if (numRt == 0 && !depthOnlyNoPs) {
                // No render target in the key and not a valid depth-only PS-less PSO. Substituting a format here would
                // build a PSO the key does not name (and a D3D12 PSO with zero RTs
                // cannot be used with a bound RTV anyway).
                // F-692, FIRST SITE: this used to refuse as kRefuseEmptyHlsl, so
                // the log said `reason=empty-hlsl` for a key whose HLSL is
                // present and whose stages compiled (PC-TRANSLATE failed=0).
                // F-716 supplies the true bit: no PSO exists for this key.
                RefuseOnce(task.key, kRefusePsoCreateFailed, "pipeline_cache.cpp:WorkerLoop(pso)",
                           "key names no render target format: every entry of "
                           "D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT is "
                           "DXGI_FORMAT_UNKNOWN, so the PSO would declare zero RTs");
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    RecordFailedKeyLocked(task.key);
                    pendingKeys_.erase(task.key);
                }
                task.promise.set_value(nullptr);
                continue;
            }
            psoDesc.NumRenderTargets = numRt;
            psoDesc.DSVFormat = static_cast<DXGI_FORMAT>(st.depthStencilFormat);
            psoDesc.SampleDesc.Count = st.sampleCount ? st.sampleCount : 1;
            psoDesc.SampleMask = UINT_MAX;
            psoDesc.RasterizerState = st.d3d12.raster;  // verbatim from the key
            psoDesc.BlendState = st.d3d12.blend;
            if (task.psHlsl.empty())  // PS-less pipeline: guest mask is 0, so no colour may be written
                for (auto& rt : psoDesc.BlendState.RenderTarget) rt.RenderTargetWriteMask = 0;
            psoDesc.DepthStencilState = st.d3d12.depthStencil;
            if (psoDesc.DSVFormat == DXGI_FORMAT_UNKNOWN &&
                psoDesc.DepthStencilState.DepthEnable) {
                // D3D12: DepthEnable must be FALSE with no depth format bound.
                // DerivePipelineState already folds this into the key, so a
                // mismatch here means a hand-built state: refuse rather than
                // silently patch the PSO into something the key does not name.
                // F-692, SECOND SITE: same defect as the site above -- a self-inconsistent
                // KEY, not missing HLSL. F-716 supplies the true bit.
                RefuseOnce(task.key, kRefusePsoCreateFailed, "pipeline_cache.cpp:WorkerLoop(pso)",
                           "key state has DepthEnable=TRUE with DSVFormat="
                           "DXGI_FORMAT_UNKNOWN, which D3D12 forbids; DerivePipelineState "
                           "folds this together, so the state was hand-built");
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    RecordFailedKeyLocked(task.key);
                    pendingKeys_.erase(task.key);
                }
                task.promise.set_value(nullptr);
                continue;
            }

            Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
            (void)device.As(&infoQueue);
            const UINT64 firstInfoQueueMessage =
                infoQueue
                    ? infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter()
                    : 0;
            HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
            if (FAILED(hr) || !pso) {
                {
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    ++stats_.compileFailures;
                    // F-653: a PSO that named a geometry stage and did not come
                    // back is a DIFFERENT failure from a PSO that named none --
                    // the stage is what the driver refused, most often because
                    // the device has no geometry stage at that feature level.
                    // Counted here so "stage created" and "stage refused" cannot
                    // be confused with "stage never attempted".
                    if (wantGeomStage) ++stats_.geomStageCompileFailed;
                }
                // F-692 fix. The reason this site used is the point of the fix:
                // both stages translated, DXC returned failed=0, and the blob it
                // produced was handed over -- so "empty-hlsl" was FALSE and the
                // refusal misdescribed its own cause. What is measured instead is
                // the stage-signature link, read out of the two HLSL sources
                // this task already holds.
                const StageSemantics vsOut = CollectSemantics(task.vsHlsl, true);
                const StageSemantics psIn = CollectSemantics(task.psHlsl, false);
                // A geometry stage writes its output names from the VS's own set
                // (BuildRectangleGeometryShaderHlsl), so the VS set is also the
                // GS set and the comparison below is the same either way.
                const std::vector<std::string> unproduced =
                    UnproducedPixelInputs(vsOut, psIn);
                ProbeFailedPsoDescriptor(device.Get(), dxc, psoDesc,
                                         vsDxil, psDxil);
                LogPsoCreateFailed(hr, vsDxil.size(), psDxil.size(), gsDxil.size(),
                                   wantGeomStage, unproduced, vsRes, psRes,
                                   vsOut, psIn);
                LogPsoDebugMessages(infoQueue.Get(), firstInfoQueueMessage);

                // F-692, THIRD SITE -- and the one the ledger entry is named for.
                // Every one of the 21 bits that existed before F-716 names a
                // CAUSE, and none of them is "CreateGraphicsPipelineState said
                // no". kRefuseEmptyHlsl is the closest by accident and is FALSE:
                // both HLSL sources are present and both compiled. So this line
                // used to print `reason=empty-hlsl` over a failure that has
                // nothing to do with HLSL. F-716 adds kRefusePsoCreateFailed
                // (pipeline_cache.h), which claims only what was measured -- the
                // PSO does not exist -- and leaves the diagnosis to this detail.
                //
                // The bit CHANGED, so a soak that gated on emptyHlsl for this
                // case was gating on a lie. psoCreateFailed is the new column and
                // a census must read it, not emptyHlsl.
                char detail[768] = {};
                if (unproduced.empty()) {
                    std::snprintf(detail, sizeof(detail),
                                  "CreateGraphicsPipelineState failed hr=0x%08lX; the "
                                  "VS output set and the PS input set both read as "
                                  "linkable, so the link is not the cause and the "
                                  "descriptor or the device is -- see "
                                  "PC-PSO-CREATE-FAILED",
                                  static_cast<unsigned long>(hr));
                } else {
                    std::snprintf(detail, sizeof(detail),
                                  "CreateGraphicsPipelineState failed hr=0x%08lX; "
                                  "pixel-shader input(s) with no producing output: "
                                  "%s (psInputs=%s vsOutputs=%s). Read "
                                  "PC-PSO-CREATE-FAILED",
                                  static_cast<unsigned long>(hr),
                                  JoinNames(unproduced).c_str(),
                                  JoinNames(psIn.names).c_str(),
                                  JoinNames(vsOut.names).c_str());
                }
                RefuseOnce(task.key, kRefusePsoCreateFailed,
                           "pipeline_cache.cpp:WorkerLoop(pso)", detail);
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    RecordFailedKeyLocked(task.key);
                    pendingKeys_.erase(task.key);
                }
                task.promise.set_value(nullptr);
                continue;
            }
        }

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            if (st.flags & kStateFlagUnmeasuredDepth) ++stats_.unmeasuredDepthState;
            if (st.flags & kStateFlagUnmeasuredBlend) ++stats_.unmeasuredBlendState;
            // F-653: "the geometry stage WAS created". The single most
            // important rectangle number, because geomStageCreated == 0 with
            // rectangleDrawsOffered > 0 is a black rectangle area with nothing
            // in the log to say why -- the exact difference between "this device
            // has no geometry stage" and "this draw never bound one".
            if (wantGeomStage) ++stats_.geomStageCreated;
        }

        // Insert into cache, record the CBV slots for the draw path, clear the
        // in-flight marker and fulfil the promise.
        task.cbvBindings = bindings;
        Insert(task.key, pso);
        {
            std::lock_guard<std::mutex> lock(mapMutex_);
            cbvBindings_[task.key] = bindings;
            samplerMasks_[task.key] = activeSamplerMask;
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            pendingKeys_.erase(task.key);
        }
        task.promise.set_value(pso);
        AppendPsoDescription(task, vsDxil, psDxil, gsDxil, vsRes, psRes,
                             bindings, psoDesc, rootSignatureHash_);
    }
}

// F-653: how many vertices the rectangle stage emits per invocation.
//
// The reference emits FOUR (pipeline_cache.cc:2390, dcl_maxVertexCount 4,
// value from :2303) as a TRIANGLE STRIP.
//
// F-716 CORRECTION: the second sentence said the strip "is consumed by the
// primitive processor's builtin index buffer with primitive reset enabled
// (primitive_processor.cc:454-459, GetTwoTriangleStripIndexCount)". That is the
// VS-EXPANSION path, and D3D12 does not take it: expand_rectangle_lists_in_vs_
// is false for this backend (d3d12_primitive_processor.cc:31 ->
// primitive_processor.cc:125-126), so :392-397 is skipped, host_primitive_reset_
// enabled stays false (:441), and IBStripCutValue is
// D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED (pipeline_cache.cc:1372, :2836).
// The strip is assembled and terminated entirely inside the geometry stage, and
// the draw is a plain TRIANGLELIST of num_indices vertices
// (d3d12_command_processor.cc:2309, :2338-2339). Four out is right; the consumer
// named here was not the one this backend uses.
//
// D3D12 geometry stages CANNOT emit a strip. A D3D12 geometry shader's output
// topology is one of exactly three values -- POINT / LINE / TRIANGLE
// (D3D12_PRIMITIVE_TOPOLOGY_TYPE) -- and HLSL offers only PointStream,
// LineStream and TriangleStream as output primitives. There is no strip, and
// therefore there is nothing for a strip-restart index to restart. Measured, not
// assumed: see the dxc negative control in this wave's verification, which
// compiles this same shader with a strip output topology and gets a hard
// compile error naming the enum.
//
// So the strip is expanded HERE, in the stage, into the two triangles it
// denotes: strip 0,1,2,3 is triangles (0,1,2) and (1,2,3), i.e. six vertices.
// That is the same two triangles in the same winding as the reference's
// 4-index strip; only the addressing differs.
//
// The primitive RESTART is therefore not reproduced as a mechanism, because
// there is nothing left for it to do: one GS invocation is one rectangle and
// invocations are independent, where the reference's strip was a single shared
// buffer that consecutive rectangles had to be cut apart in. Cutting a per-
// primitive buffer between its own primitives is a no-op. This is the honest
// difference between the two, stated rather than papered over with an index
// buffer the project does not have.
constexpr int kRectGsMaxVertexCount = 6;

// F-653: the D3D12 required ceiling for GS output vertices per invocation is
// 1024 (D3D11/D3D12 functional spec, Geometry Shader). The arithmetic the brief
// asks for: we emit 6, so the headroom is 1018 vertices, a factor of 170. The
// reference needed 4 (pipeline_cache.cc:2303), a factor of 256 -- so this port
// is still two orders of magnitude inside the ceiling.
//
// Note what is NOT measured here: 1024 is a REQUIRED MINIMUM, and D3D12 exposes
// no feature-data field that reports an adapter's actual GS output ceiling. That
// is exactly why "geomStageCreated" in PipelineCacheStats is the counter that
// matters -- a PSO either came back with pGeometryShader bound or it did not,
// and CreateGraphicsPipelineState is the only authority on whether this
// particular device and driver accepted it.
static_assert(kRectGsMaxVertexCount <= 1024,
              "rectangle GS emits more vertices than the D3D12 required ceiling");

namespace {
// F-653: refuse with a name, bounded, and never with a default.
void RefuseGeometryShader(const char* caller, const char* what, uint32_t mask) {
    static std::atomic<uint32_t> s_unbuildable = {};
    const uint32_t n = s_unbuildable.fetch_add(1) + 1;
    if (n <= 8 || (n % 200) == 0)
        MCLA_LOG_WARN("PC-RECT-GS-REFUSED #{} [{}] {}: no geometry stage will be "
                      "bound for this pipeline; the rectangle draw is refused, "
                      "not drawn unexpanded",
                      n, caller ? caller : "?", what);
    (void)mask;
}
}  // namespace

std::string BuildRectangleGeometryShaderHlsl(std::string_view vsHlsl,
                                             const char* caller) {
    // ---- 1. Read the translated VS's OUTPUT signature --------------------
    //
    // WHY THE OUTPUT SIGNATURE AND NOT THE ELEMENT TABLE. The stage reads the
    // vertex shader's OUTPUT, never the vertex buffer, so the input layout is
    // not involved in this binding at all. The hazard the brief names -- a
    // stage bound to the wrong element shearing rectangles with no error --
    // cannot occur on this path, and it is worth being explicit about why: the
    // GS/VS and GS/PS links are made by SYSTEM-VALUE SEMANTIC between stage
    // signatures and are validated by CreateGraphicsPipelineState, not by an
    // input-layout slot index. There is no slot here to get wrong.
    //
    // The reference does exactly the same thing and never looks at the guest's
    // element table either: it AUTHORS the position input itself,
    // pipeline_cache.cc:2035 (`input_register_position = input_register_index`,
    // i.e. "the register after the interpolators") and declares it as a system
    // value at :2322-2324. The position is not discovered; it is placed.
    //
    // This project's translated VS already publishes the matching side of that
    // contract: `out float4 oPos : SV_Position` at shader_translator.cpp:676,
    // bound to guest export register 62 (the Xenos position export) at
    // shader_translator.cpp:756-757. So "which vertex element is POSITION" has
    // an answer, and it is not the one the brief expected: POSITION is the VS's
    // SV_Position OUTPUT. The four scalars the draw carries (CpVertexFormat,
    // render_queue.h:131-136) describe the FETCH format and are the right input
    // to a vertex layout; they are not, and need not be, the input to this stage.
    std::vector<uint32_t> interp;
    bool sawPosition = false;
    {
        static constexpr const char* kPos = "out float4 oPos : SV_Position";
        sawPosition = vsHlsl.find(kPos) != std::string_view::npos;
        static constexpr const char* kPrefix = "out float4 oTexCoord";
        size_t pos = 0;
        while (true) {
            const size_t hit = vsHlsl.find(kPrefix, pos);
            if (hit == std::string_view::npos) break;
            // MEASURED DEFECT THIS REPLACES: the skip was
            // `hit + sizeof(kPrefix) - 1`. `kPrefix` is a `const char*`, so
            // `sizeof` is the POINTER width -- 8 on x64 -- and not the string
            // length, which is 20. The scan therefore started 13 characters
            // early, INSIDE "float4", and the digit loop read 'a':
            //     sizeof(const char*) = 8 ; strlen(kPrefix) = 20 ; off by 13
            // (harness, `sizeof_probe`, against the real translated HLSL of
            // CP-SHADER-BODY #3). `digits` stayed false and EVERY VS that
            // emitted an interpolator took
            // RefuseGeometryShader("oTexCoord export with no index"), so
            // `BuildRectangleGeometryShaderHlsl` returned empty for all of
            // them. The refusal was TRUE in form and FALSE in fact: the
            // signature it claimed was unparsable is the one this module
            // writes itself, at shader_translator.cpp:723-724.
            //
            // CONFIRMED BY CONTROL, on the four live programs, calling the
            // real BuildRectangleGeometryShaderHlsl on the real
            // TranslateShaderPinned output:
            //   hash 00A089169A1B57E8 (declares oTexCoord0) -> EMPTY, refused
            //   hash D99385F5E67A3EB1 (declares oTexCoord0) -> EMPTY, refused
            //   hash AEAF1F096E09B347 (declares NO oTexCoord) -> 8171 bytes
            // The third is the control: with the prefix never matched, the
            // broken skip was never executed and the stage built.
            //
            // F-716, 2026-10-02: THE 8171 IS NOW 8244. The stage built for that
            // same program grew by 73 bytes because the comment INSIDE the emitted
            // shader was corrected -- it used to cite the Vulkan VS-expansion
            // builtin index buffer and primitive reset as the mechanism, which is
            // not what D3D12 does (see the long note above `std::string h`). No
            // HLSL CODE changed, only the comment text carried with it. Re-measured
            // on the real generator with a vertex shader in the translator's own
            // output shape (shader_translator.cpp:694-731): source 8244 B, and the
            // repo's own dxc.exe compiles it as gs_6_0 to 3680 B of DXIL with no
            // diagnostics. The earlier figure of 3708 B DXIL is likewise superseded.
            //
            // Anyone quoting "8171 B -> 3708 B DXIL" for this stage is quoting a
            // pre-correction artifact; the sizes are 8244 B -> 3680 B.
            //
            // So the parse is kept, `strlen` is used because <cstring> is
            // already included (pipeline_cache.cpp:7), and the refusal below is
            // kept as a real guard for a signature this module did not write.
            size_t p = hit + std::strlen(kPrefix);
            uint32_t n = 0;
            bool digits = false;
            while (p < vsHlsl.size() && vsHlsl[p] >= '0' && vsHlsl[p] <= '9') {
                n = n * 10u + static_cast<uint32_t>(vsHlsl[p] - '0');
                ++p;
                digits = true;
            }
            if (!digits) {
                RefuseGeometryShader(caller, "oTexCoord export with no index",
                                     kRefuseGeometryShaderUnbuildable);
                return std::string();
            }
            // The translator emits exactly " : TEXCOORD<n>" (shader_translator.cpp:723-724);
            // anything else is not a signature this module may reinterpret.
            //
            // THE TWO NUMBERING SCHEMES, MEASURED, AND WHY THE PASS-THROUGH
            // BELOW IS THE RIGHT BRIDGE RATHER THAN A NEW CORRESPONDENCE.
            //
            // Scheme A, the VS OUTPUT: the guest EXPORT REGISTER number. The
            // translator names the output `oTexCoord<vd> : TEXCOORD<vd>` for
            // vd in `vsExportDests_` (shader_translator.cpp:721-724), skipping
            // vd == 62 (position) and vd >= 16.
            // Scheme B, the PS INPUT: the DeclUsage `usageIndex`. The
            // translator names the input `i<usageVar><usageIndex> : <semantic>
            // <usageIndex>` (shader_translator.cpp:698-706) from
            // `prog_.interpolators[].usageIndex`, decoded at
            // xenos_shader_ir.cpp:213 (PS table, `word >> 0 & 0xF`).
            //
            // They are DIFFERENT fields in the guest's own tables: the VS
            // interpolator entry carries `reg` at `word >> 8 & 0xF`
            // (xenos_shader_ir.cpp:215) AND `usageIndex` at `word >> 0 & 0xF`,
            // independently. So nothing forces them to agree -- and the
            // reference does not rely on them agreeing either. It GUARANTEES
            // it: .research/xenia/src/xenia/gpu/d3d12/pipeline_cache.cc:1703-1704
            // asserts `vertex_shader_modification.vertex.interpolator_mask ==
            // pixel_shader_modification.pixel.interpolator_mask`, and the mask
            // is dense by construction
            // (.research/xenia/src/xenia/gpu/shader_translator.cc:340-344,
            // `(1 << interpolator_count) - 1`). Every signature is then emitted
            // with a DENSE ORDINAL: `semantic_index = i` for
            // i in [0, interpolator_count), on the GS input
            // (pipeline_cache.cc:2025), the GS output (:2186) and the VS/PS
            // signatures (dxbc_shader_translator.cc:1540-1545, :2843).
            //
            // THE REFERENCE THEREFORE NEVER READS THE VS SIGNATURE AT ALL, and
            // this module does not need a bridge either: because the ordinal is
            // dense, the ordinal EQUALS the guest's interpolator/export number
            // whenever the guest uses a 0..N-1 prefix, which is precisely what
            // scheme A already emits. Passing `n` through unchanged is the
            // reference's own numbering, not a correspondence invented here.
            //
            // MEASURED for the live pair, so the hazard is not merely argued:
            //   VS  D99385F5E67A3EB1 exports register 0  -> TEXCOORD0
            //   PS  7B0032AD4C5B576D usage=5 usageIndex=0 -> TEXCOORD0
            // They coincide at 0, so the GS links VS-out to PS-in through it.
            // UNRESOLVED and deliberately not asserted: this module cannot SEE
            // the PS, so it cannot check the reference's mask equality for a
            // program where the two schemes would differ. Such a draw would
            // fail at CreateGraphicsPipelineState with a signature mismatch --
            // loudly, and not by shearing a rectangle. See the handoff note.
            const std::string want = " : TEXCOORD" + std::to_string(n);
            if (vsHlsl.compare(p, want.size(), want) != 0) {
                RefuseGeometryShader(caller, "interpolator semantic is not TEXCOORD<n>",
                                     kRefuseGeometryShaderUnbuildable);
                return std::string();
            }
            for (uint32_t prior : interp) {
                if (prior == n) {
                    RefuseGeometryShader(caller, "duplicate interpolator index",
                                         kRefuseGeometryShaderUnbuildable);
                    return std::string();
                }
            }
            interp.push_back(n);
            pos = p;
        }
    }
    if (!sawPosition) {
        // A stage that could not find the position has nothing to expand, and a
        // stage invented without it would expand garbage -- which is worse than
        // not expanding, because it looks like it worked.
        RefuseGeometryShader(caller, "translated VS declares no `out float4 oPos : SV_Position`",
                             kRefuseGeometryShaderUnbuildable);
        return std::string();
    }

    // ---- 2. Emit the stage ----------------------------------------------
    //
    // F-716 CORRECTION TO A COMMENT INSIDE THE EMITTED SOURCE. The generated HLSL
    // used to carry, above the four appends, the sentence "its builtin index
    // buffer then draws the strip with primitive reset (primitive_processor.cc:
    // 454-459)". That is the VS-EXPANSION mechanism and D3D12 does not use it.
    // Measured, for THIS backend:
    //   d3d12_primitive_processor.cc:31 passes
    //     InitializeCommon(true, false, false, true, true, true) and the sixth
    //     argument is `rectangle_lists_supported_without_vs_expansion`
    //     (parameter order, primitive_processor.h:249-260);
    //   primitive_processor.cc:125-126 therefore sets expand_rectangle_lists_in_vs_
    //     = false, so the branch at primitive_processor.cc:392-397 is SKIPPED and
    //     host_vertex_shader_type stays kVertex (:377);
    //   host_primitive_reset_enabled therefore stays false
    //     (primitive_processor.cc:441), and d3d12/pipeline_cache.cc:1372 sets
    //     strip_cut_index = kNone, giving IBStripCutValue =
    //     D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED (:2836).
    // There is no builtin index buffer and no restart on this path. The draw is a
    // plain D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST of num_indices vertices
    // (d3d12_command_processor.cc:2309, :2338-2339) and the whole strip is
    // assembled and terminated INSIDE this stage.
    //
    // The conclusion was and remains right -- the cut gives triangles (0,1,2) and
    // (1,2,3) -- but the mechanism cited for it belonged to the other backend.
    //
    // WHY THE CORRECTION IS ALSO IN THE STRING, NOT ONLY HERE: an earlier pass of
    // this fix put the whole correction in a C++ comment above and left the false
    // sentence in the emitted shader. That is worse, not better: the shader is the
    // artifact a reader opens, and RULE 0 says a doc sentence asserting a guest
    // fact that is false is worse than one that is absent. So the string now
    // carries a short correct version, this comment carries the long version with
    // the citations, and the size figures are re-recorded where they are quoted
    // (see the AEAF1F096E09B347 note above).
    std::string h;
    h.reserve(2048 + interp.size() * 96);
    h +=
        "// F-653: the Xenos rectangle-list expansion, ported from the reference's\n"
        "// own direct3d-12 fallback. Every term below cites where it comes from.\n"
        "//\n"
        "// The reference builds this with a DXBC geometry shader. Where its TWO\n"
        "// transcriptions of this stage disagree -- the d3d12 one at\n"
        "// pipeline_cache.cc:2550-2695 and the vulkan one at\n"
        "// vulkan/pipeline_cache.cc:1406-1669 -- the terms below follow the\n"
        "// VULKAN one, and the block that differs says so at the line. One\n"
        "// structural difference is forced by D3D12 and is named where it\n"
        "// occurs:\n"
        "// difference is forced by D3D12 and is named where it occurs:\n"
        "//   - the reference's OUTPUT TOPOLOGY is a Triangle STRIP of 4\n"
        "//     (pipeline_cache.cc:2302-2303, kTriangleStrip and\n"
        "//     max_output_vertex_count = 4). D3D12 exposes streams rather than a\n"
        "//     strip topology, so this emits into a TriangleStream and closes the\n"
        "//     strip with os.RestartStrip() after the last append, which is the\n"
        "//     direct port of the reference's OpCutStream (pipeline_cache.cc:2694).\n"
        "//     Four appends in one strip: two triangles, (s0,s1,s2) and\n"
        "//     (s2,s1,s3), where s0..s2 are the three guest vertices in the\n"
        "//     order the longest-edge selection below chooses.\n"
        "//     kRectGsMaxVertexCount above is still 6, a legal cap for four appends\n"
        "//     and a mismatch against the reference's 4 -- reported, not changed,\n"
        "//     so this edit moves the rasterisation and nothing else.\n"
        "//\n"
        "// NOT PORTED, AND NOT INVENTED -- both are the reference conditioning on\n"
        "// state this project does not carry, so both are no-ops here rather than\n"
        "// guesses:\n"
        "//   - the point-coordinate output the reference writes 0.0 into\n"
        "//     (pipeline_cache.cc:2632-2634, 2665-2667), gated on\n"
        "//     key.has_point_coordinates. That key field is the guest's\n"
        "//     point-size/kill export 63, which this project's translator\n"
        "//     suppresses outright (shader_translator.cpp:755, \"The 63 write has\n"
        "//     no color output and is suppressed below\"), so the branch is not\n"
        "//     reachable and nothing stands in for it.\n"
        "//   - the clip/cull distance copies (pipeline_cache.cc:2638-2647,\n"
        "//     2675-2692) and the cull test (:2413-2435). This project's\n"
        "//     translated signatures carry no SV_ClipDistance/SV_CullDistance at\n"
        "//     all (shader_translator.cpp:676-681 emits only position and the\n"
        "//     TEXCOORD interpolators), so input_clip_distance_count is 0 and\n"
        "//     those loops are empty in the reference too.\n"
        "struct GSIn {\n"
        "    float4 pos : SV_Position;\n";
    for (uint32_t n : interp)
        h += "    float4 i" + std::to_string(n) + " : TEXCOORD" + std::to_string(n) + ";\n";
    h +=
        "};\n"
        "// GSOut's semantics are the VS's OUTPUT semantics verbatim, which is what\n"
        "// keeps the stage transparent: today the VS output links straight to the\n"
        "// PS input, and a stage that emitted any other name would move the link\n"
        "// from one hop to two and could fail with no diagnostic.\n"
        "struct GSOut {\n"
        "    float4 pos : SV_Position;\n";
    for (uint32_t n : interp)
        h += "    float4 o" + std::to_string(n) + " : TEXCOORD" + std::to_string(n) + ";\n";
    h += "};\n";
    h += "[maxvertexcount(" + std::to_string(kRectGsMaxVertexCount) + ")]\n";
    h += "void main(triangle GSIn v[3], inout TriangleStream<GSOut> os) {\n";
    h +=
        "    // Input is a TRIANGLE of 3 vertices -- pipeline_cache.cc:2300-2301\n"
        "    // (`input_primitive = dxbc::Primitive::kTriangle`,\n"
        "    // `input_primitive_vertex_count = 3`). A rectangle is 3 guest\n"
        "    // vertices; the 4th is generated. primitive_processor.cc:452's\n"
        "    // `primitive_count /= 3` is the console side agreeing.\n"
        "\n"
        "    // Discard the WHOLE primitive if any input position is NaN:\n"
        "    // pipeline_cache.cc:2401-2411. The reference tests it as\n"
        "    // `pos != pos` (OpNE of a register with itself, :2404-2405), which is\n"
        "    // true for every non-NaN and false for NaN, then ORs the results\n"
        "    // together and returns. `isnan` is the same predicate spelled\n"
        "    // directly. The reference's own comment notes NaN may also stand in\n"
        "    // for emulated vertex killing.\n"
        "    if (any(isnan(v[0].pos)) || any(isnan(v[1].pos)) || any(isnan(v[2].pos)))\n"
        "        return;\n"
        "\n"
        "    // Squared lengths of the three edges, into the reference's r0.xyz.\n"
        "    // vulkan/pipeline_cache.cc:1435-1471 reads component 0 and\n"
        "    // component 1 of each edge, which is what dot(e.xy, e.xy) below\n"
        "    // reproduces; the .xyz subtraction yields the same value, the\n"
        "    // swizzle only ever selecting the two components dot() reduces.\n"
        "    //     d12 = ||12||^2   from v2 - v1\n"
        "    //     d20 = ||20||^2   from v0 - v2\n"
        "    //     d01 = ||01||^2   from v1 - v0\n"
        "    // d3d12/pipeline_cache.cc:2579-2595 uses three DIFFERENT component\n"
        "    // pairs -- .xy for edge 12, .yz for 20, .zw for 01 -- because\n"
        "    // OpDP2 reduces components 0 and 1 of whatever its source swizzle\n"
        "    // selected (dxbc.h:1614-1617), so its r0.y and r0.z are NOT these\n"
        "    // numbers. With the guest\'s z=0, w=1 the two sets differ. See the\n"
        "    // note on that below.\n"
        "    float3 e12 = v[2].pos.xyz - v[1].pos.xyz;\n"
        "    float3 e20 = v[0].pos.xyz - v[2].pos.xyz;\n"
        "    float3 e01 = v[1].pos.xyz - v[0].pos.xyz;\n"
        "    float d12 = dot(e12.xy, e12.xy);\n"
        "    float d20 = dot(e20.xy, e20.xy);\n"
        "    float d01 = dot(e01.xy, e01.xy);\n"
        "\n"
        "    // The longest-edge selection. The predicate is the reference's own\n"
        "    // stated meaning: the comment at d3d12/pipeline_cache.cc:2558 says\n"
        "    // \"12 is the longest edge, strip 0123\", :2564 \"20 is the longest\n"
        "    // edge, strip 1203\", :2570 \"01 is the longest edge, strip 2013\".\n"
        "    // vulkan/pipeline_cache.cc:1479-1492 implements exactly that, as\n"
        "    // FOrdGreaterThan(edge12, edge20) && FOrdGreaterThan(edge12,\n"
        "    // edge01) -> 0, else FOrdGreaterThan(edge20, edge01) -> 1 else 2, and\n"
        "    // :1493-1505 rotates it into the other two slots.\n"
        "    //\n"
        "    // F-653 CORRECTION 2026-10-04. Two changes, and the SECOND is the\n"
        "    // one that decides the shape. `order` is now CONSUMED, and the\n"
        "    // comparison is now the vulkan one. Both were measured in the same\n"
        "    // untracked reference snapshot (.research/, gitignored, 0 files in\n"
        "    // HEAD, no recorded provenance), and the two d3d12/vulkan\n"
        "    // transcriptions of this stage disagree:\n"
        "    //\n"
        "    //  1. The d3d12 selection is DEAD. Written to r0.xyz at :2610 and\n"
        "    //     :2619-2620; the LAST line in the whole case (:2550-2695) that\n"
        "    //     touches r0 at all is :2619, so nothing in :2621-2695 reads\n"
        "    //     it. The emit loop indexes with the LITERAL i,\n"
        "    //     `dxbc::Index(0, i)` (:2626), and the fourth vertex is built\n"
        "    //     from the literal 0/1/2 (:2669-2673).\n"
        "    //  2. That d3d12 chain also INVERTS its own comment. :2599-2600\n"
        "    //     computes r0.w = (r0.y < r0.x) = (20 > 12) and :2602-2603\n"
        "    //     r0.x = (r0.z < r0.x) = (01 > 12), so :2605-2606 ANDs them\n"
        "    //     into \"12 is longest\" -- but that is 12 SHORTER than both,\n"
        "    //     the opposite of the comment it implements, and its else\n"
        "    //     arm (:2615-2620) swaps the other two cases as well. So that\n"
        "    //     chain cannot be taken as the statement of the predicate. It\n"
        "    //     is also silent in practice: it is dead, and because it is dead\n"
        "    //     nobody saw the inversion. Its agreement on any one input is\n"
        "    //     incidental -- on the measured draw below it does land on\n"
        "    //     (1,2,0), but it lands on (2,0,1)-needing orders wrongly too\n"
        "    //     (harness negative control).\n"
        "    //  3. The vulkan chain is not inverted and is not dead, and it\n"
        "    //     carries the identical comment block word for word\n"
        "    //     (:1407-1428 vs :2551-2572). So the comment, not the d3d12\n"
        "    //     chain, is the reference\'s own statement of this predicate,\n"
        "    //     and that is what the terms below follow.\n"
        "    //\n"
        "    // EDGE-LENGTH COMPONENTS, because the two files differ here too and\n"
        "    //     it matters for the numbers quoted below. d3d12 takes three\n"
        "    //     DIFFERENT component pairs -- .xy for edge 12, .yz for 20, .zw\n"
        "    //     for 01 (:2579-2595, with OpDP2 reducing components 0 and 1 per\n"
        "    //     dxbc.h:1614-1617); vulkan takes .x and .y for all three\n"
        "    //     (:1443-1461). With the guest\'s z=0, w=1 those give different\n"
        "    //     numbers, so this stage uses VULKAN\'s, matching the terms above.\n"
        "    //\n"
        "    // WHY IT DECIDES THE SHAPE. A rectangle\'s corners satisfy\n"
        "    // A + D = B + C, so which corner is v0 decides whether -v0 + v1 + v2\n"
        "    // closes the fourth corner or shears the quad. With the guest\'s\n"
        "    // MEASURED corners (build/w712.log:7913, VS-INPUT-STREAM #1)\n"
        "    // v0=(-0.5,-0.5) v1=(639.5,-0.5) v2=(639.5,359.5):\n"
        "    //     d12 = 360^2      = 129600\n"
        "    //     d20 = 640^2+360^2 = 539200   <- the longest, so the\n"
        "    //                                diagonal is 20\n"
        "    //     d01 = 640^2      = 409600\n"
        "    // first = 1, so order = (1,2,0) and\n"
        "    //     -v[order.x] + v[order.y] + v[order.z] = -v1 + v2 + v0\n"
        "    //         = (-0.5, 359.5)\n"
        "    // the rectangle\'s fourth corner. The literal 0/1/2 gave\n"
        "    // -v0 + v1 + v2 = (1279.5, 359.5): a parallelogram of the SAME\n"
        "    // AREA and twice the width, which is what w712.log recorded --\n"
        "    // changedPx=230400 and changedRows=360 already right, changedCols=\n"
        "    // 1278 instead of 640.\n"
        "    //\n"
        "    // No constant was moved to get that, and no edge length was\n"
        "    // rescaled: d12/d20/d01 above are dot(e.xy, e.xy) exactly as they\n"
        "    // were before this change. Only the comparison direction and the\n"
        "    // use of the result differ. Harness, which executes THIS emitted\n"
        "    // HLSL rather than a copy of it:\n"
        "    // %TEMP%\\opencode\\gs_rect_harness.py\n"
        "    bool twelveLongest = (d12 > d20) && (d12 > d01);\n"
        "    int first = twelveLongest ? 0 : ((d20 > d01) ? 1 : 2);\n"
        "    int3 order = int3(first, (first + 1) % 3, (first + 2) % 3);\n"
        "\n"
        "    // The fourth vertex: v3 = -v[order.x] + v[order.y] + v[order.z].\n"
        "    // The formula is pipeline_cache.cc:2669-2673 (OpAdd -v0+v1, then\n"
        "    // OpAdd that +v2) and vulkan/pipeline_cache.cc:1609-1627, which is\n"
        "    // the same linear combination over the SAME indices the vulkan\n"
        "    // select chain produced; the d3d12 file applies it to the literals\n"
        "    // 0/1/2 (:2670, :2671, :2673) because its selection is dead. This is\n"
        "    // also the same value as the comment's \"v3 = v0 + (v1 - v0) +\n"
        "    // (v2 - v0)\" at :2559.\n"
        "    // The same linear combination is applied to every interpolator, in\n"
        "    // the reference's r1 scratch register, :2655-2664.\n"
        "    GSOut o3;\n"
        "    o3.pos = v[order.y].pos - v[order.x].pos + v[order.z].pos;\n";
    for (uint32_t n : interp)
        h += "    o3.o" + std::to_string(n) + " = v[order.y].i" + std::to_string(n) +
             " - v[order.x].i" + std::to_string(n) + " + v[order.z].i" +
             std::to_string(n) + ";\n";
    h +=
        "\n"
    // The strip s0,s1,s2,s3, as the two triangles it denotes. The reference
    // emits three vertices (:2625-2649, one OpEmitStream per vertex at :2648;
    // vulkan :1520-1570), then the generated one (:2693; vulkan :1668-1669),
    // and CUTS the stream afterwards (OpCutStream, :2694). The cut TERMINATES
    // the 4-vertex strip; it does not separate s2 from s3, and a cut placed
    // after the third append would leave a 1-vertex strip covering NO triangle
    // at all. There is no index buffer and no primitive restart on this path
    // -- the draw is a plain TRIANGLELIST of num_indices vertices
    // (d3d12_command_processor.cc:2309, :2338-2339) and the whole strip is
    // assembled here.
        "    //\n"
        "    // Each output is built FIELD BY FIELD rather than by assigning the input\n"
        "    // vertex to an output variable. That is not a style choice: GSIn and GSOut\n"
        "    // are distinct struct types and HLSL does NOT implicitly convert between\n"
        "    // them even when their members are identical, so `o = v[0]; ostream.Append(o)`\n"
        "    // is a compile error (\"cannot implicitly convert from 'GSIn' to 'GSOut'\").\n"
        "    // Measured, not assumed: this was written the short way first and the dxc\n"
        "    // check rejected it at every Append site.\n"
        "    GSOut t;\n";
    // The emission order, as a table so the strip is legible in the output: the
    // three guest vertices in the order the longest-edge selection above chose,
    // then the generated v3.
    //
    // MEASURED DEFECT THIS TABLE PRECEDES. A TriangleStream appends into a
    // strip, and a strip of N vertices assembles N-2 triangles. This used to
    // append the six indices {0,1,2,1,2,3} with no restart, which is a
    // SIX-vertex strip and so FOUR triangles, where the reference's is two:
    //     i=0  v0,v1,v2   the reference's first triangle
    //     i=1  v1,v1,v2   zero area -- the table repeats v1
    //     i=2  v2,v1,v2   zero area -- the table repeats v2
    //     i=3  v1,v2,v3   the reference's second triangle
    // The repeats are what degenerate the middle pair, so the old form COVERED the
    // same two triangles as the reference: it did not draw the quad twice and it
    // did not reverse the second triangle's winding. What was wrong is that it
    // submitted two zero-area triangles per rectangle, duplicated v1 and v2 to do
    // it, and was described as a TRIANGLE LIST of 6 while this signature is a
    // TriangleStream. Four appends in one strip removes all three.
    //
    // F-653 2026-10-04: entries 0..2 are the SELECTION's indices, not the
    // literals 0,1,2 -- see the block above for the measurement. `nullptr` marks
    // the generated fourth vertex, which has no input index.
    static constexpr const char* kEmitIndex[4] = {"order.x", "order.y",
                                                  "order.z", nullptr};
    for (int k = 0; k < 4; ++k) {
        const char* idx = kEmitIndex[k];
        if (idx == nullptr) {
            h += "    os.Append(o3);\n";  // the generated vertex, already built
        } else {
            h += "    t.pos = v[" + std::string(idx) + "].pos;\n";
            for (uint32_t n : interp)
                h += "    t.o" + std::to_string(n) + " = v[" + idx + "].i" +
                     std::to_string(n) + ";\n";
            h += "    os.Append(t);\n";
        }
        // After the LAST append, NOT after the third: the reference cuts after
        // its fourth OpEmitStream (:2693), so the cut terminates the strip rather
        // than separating s2 from s3. Restarting after the third would leave the
        // trailing 1-vertex strip covering nothing, i.e. half the quad.
        if (k == 3)
            h += "    os.RestartStrip();\n";
    }
    h += "}\n";
    return h;
}


void PipelineCache::RecordFailedKeyLocked(const PipelineKey& key) {
    if (failedKeys_.count(key)) return;
    if (failedOrder_.size() >= kMaxFailedKeys && !failedOrder_.empty()) {
        failedKeys_.erase(failedOrder_.front());
        failedOrder_.erase(failedOrder_.begin());
    }
    failedKeys_.insert(key);
    failedOrder_.push_back(key);
    failedCount_.store(static_cast<uint32_t>(failedKeys_.size()), std::memory_order_release);
    failedGeneration_.fetch_add(1, std::memory_order_release);
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineCache::GetOrCompile(
    const PipelineKey& key,
    const std::string& vsHlsl,
    const std::string& psHlsl,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout,
    const char* caller,
    bool* outCarriesGeometryStage,
    bool isGuestDraw,
    bool* outMapHit,
    bool* outNegativeHit) {
    if (outMapHit) *outMapHit = false;
    if (outNegativeHit) *outNegativeHit = false;

    // The carried geometry-stage capability starts FALSE and is only ever set on
    // the one path that can justify it (the cache hit below). Every early return
    // and the fallback therefore report "no stage", which is the safe answer: a
    // caller that maps prim_type 0x08 to a triangle list on this flag alone
    // cannot do so against a PSO nobody has proved carries the expansion.
    if (outCarriesGeometryStage) *outCarriesGeometryStage = false;

    statLookups_.fetch_add(1, std::memory_order_relaxed);

    // A key that carries a refusal is checked BEFORE the cache: such a key is
    // never inserted, so a lookup could only ever miss, and reporting it as a
    // miss would let it read like "one more compile pending".
    if (key.refuseMask != kRefuseNone) {
        RefuseOnce(key, key.refuseMask, caller,
                   "the key itself says this pipeline cannot be built honestly");
        return nullptr;
    }

    // Fast path: check cache first
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    {
        std::lock_guard<std::mutex> lock(mapMutex_);
        auto it = map_.find(key);
        static const bool s_noPsoCache = [] { const char* e = std::getenv("MCLA_NO_PSOCACHE"); return e && e[0] == '1'; }();
        if (it != map_.end() && !s_noPsoCache) {
            pso = it->second;
            if (pso) {
                statHits_.fetch_add(1, std::memory_order_relaxed);
                    // THE ONE PATH THAT SETS THE CARRIED FLAG. The key names the
                    // geometry stage as identity (PipelineState::geometryStage, and
                    // the == operator at pipeline_cache.h), the worker is the only
                // writer of this map, and the worker binds pGeometryShader
                // exactly when the key named a stage (pipeline_cache.cpp
                // :1716-1718, and only from a blob that compiled at :1681).
                // So a hit on such a key IS a hit on a PSO that has the stage,
                // which is the fact D3D12 cannot be asked for directly.
                //
                // The converse is deliberately NOT asserted: a key with
                // kGeomStageNone could in principle be satisfied by a PSO that
                // has a stage, so the flag is the WEAKER claim (false) and is
                // safe when wrong. It can never be true when the stage is absent.
                if (outCarriesGeometryStage) {
                    *outCarriesGeometryStage =
                        (key.state.geometryStage == kGeomStageRectangleList);
                }
                if (outMapHit) *outMapHit = true;
                if (prewarmedKeys_.count(key)) {
                    const uint64_t n = g_psoCacheCounters.prewarmHits.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                    if (ShouldLogPsoCensus(n)) {
                        MCLA_LOG_INFO("PC-PSO-PREWARM-HIT #{} key={:016X} totalHits={} "
                                      "prewarmFailed={} persistFailed={} -- no psoWait",
                                      n, HashPipelineKey(key), n,
                                      g_psoCacheCounters.prewarmFailed.load(),
                                      g_psoCacheCounters.persistFailed.load());
                    }
                }
                return pso;
            }
            // A cached null can only exist if something bypassed Insert(); treat
            // it as a miss and let the refusal path below re-report it.
            map_.erase(it);
            generation_.fetch_add(1, std::memory_order_release);
        }
    }

    statMisses_.fetch_add(1, std::memory_order_relaxed);

    // O(1) negative cache check: permanent compile/PSO failures never re-enqueue DXC/PSO worker tasks.
    if (failedCount_.load(std::memory_order_acquire) != 0) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (failedKeys_.count(key)) {
            if (outNegativeHit) *outNegativeHit = true;
            return nullptr;
        }
    }

    // Refusals. Each returns null on purpose; none of them is a cacheable miss.
    if (vsHlsl.empty()) {  // empty psHlsl is a depth-only (PS-less) pipeline
        RefuseOnce(key, kRefuseEmptyHlsl, caller,
                   "no translated HLSL; a PSO cannot be compiled from nothing");
        return nullptr;
    }
    if (inputLayout.empty()) {
        RefuseOnce(key, kRefuseEmptyInputLayout, caller,
                   "empty input layout for a pipeline that consumes a VS");
        return nullptr;
    }
    if (HashInputLayout(inputLayout) != key.inputLayoutHash) {
        RefuseOnce(key, kRefuseLayoutDeclHashMismatch, caller,
                   "the layout does not match PipelineKey::inputLayoutHash");
        return nullptr;
    }
    if (!device_) {
        RefuseOnce(key, kRefuseNoDevice, caller, "PipelineCache::StartWorker was never called");
        return nullptr;
    }
    if (!rootSignature_) {
        RefuseOnce(key, kRefuseNoRootSignature, caller,
                   "no root signature: the worker cannot know what the draw will bind");
        return nullptr;
    }

    // T43.24v2 (2026-10-16): REVERTED the nullptr refusal. The audit was
    // right that the test-PSO substitution draws through a layout the
    // guest never declared — but refusing the draw was WORSE: there is
    // only ONE draw per unique pipeline key in this boot, so the first
    // (and only) draw was refused, the async compile had nothing to serve,
    // and ZERO draws happened (w784: PC-PSO-MISS=2, DRAW-CP-DRAWN=0,
    // a strict regression from 2 draws to 0). The correct shape is to
    // return the fallback (test PSO) as before — the draw works when the
    // stride matches (as w783 proved), and the compile populates the cache
    // for any future draw with the same key. The substitution is now
    // COUNTED AND LOGGED (below) instead of silent.
    if (isGuestDraw) {
        static std::atomic<uint32_t> s_testPsoGuestDraws{0};
        const uint32_t tp = s_testPsoGuestDraws.fetch_add(1) + 1;
        if (tp <= 16 || (tp % 100) == 0) {
            MCLA_LOG_WARN("PC-TESTPSO-GUEST #{} key={} (cold miss: real PSO queued; "
                          "fixed test PSO is not an authorised cold-draw fallback)",
                          tp, key.vsHash);
        }
    }

    // Cache miss: grab the fallback PSO up front (may be null before SetFallback)
    Microsoft::WRL::ComPtr<ID3D12PipelineState> fallback;
    {
        std::lock_guard<std::mutex> lock(fallbackMutex_);
        fallback = fallbackPso_;
    }

    // Queue an async compile task for this key. The worker compiles HLSL ->
    // DXIL and creates the PSO in the background. The caller waits by default;
    // only the explicit MCLA_PSO_NONGEOM_FALLBACK opt-in returns the fixed test
    // PSO immediately. A key already queued or in flight is not re-queued.
    CompileTask task;
    task.key = key;
    task.vsHlsl = vsHlsl;
    task.psHlsl = psHlsl;
    task.inputLayout = inputLayout;

    // F-716: hand this task's promise to whoever asks for the key next.
    //
    // CompileTask::promise was set on every worker path but read by nobody
    // (measured: `grep get_future src/` = 0 hits before this change), so the
    // future has to be taken HERE, before the task is moved into the queue --
    // std::promise::get_future may be called exactly once, and after a move the
    // promise object in `task` is a moved-from husk.
    //
    // A shared_future (not a future) because the worker owns the promise and can
    // never hand it back; two callers racing for the same in-flight key both get
    // the same state.
    std::shared_future<Microsoft::WRL::ComPtr<ID3D12PipelineState>> myFuture;
    bool queuedByMe = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (pendingKeys_.size() >= kMaxInflight) {
            // Queue saturated. Previously this silently returned the fallback, so
            // a churn-heavy frame looked like a working cache. It is now a
            // counted refusal: the draw still happens (the backend's own default
            // applies) but the log says the compile queue is the problem.
            std::lock_guard<std::mutex> slock(statsMutex_);
            ++stats_.saturated;
            RefuseOnce(key, kRefuseQueueSaturated, caller,
                       "compile queue saturated (kMaxInflight=256); no task queued");
            return nullptr;
        }
        if (!pendingKeys_.insert(key).second) {
            {
                std::lock_guard<std::mutex> slock(statsMutex_);
                ++stats_.inFlightDuplicate;
            }
            // F-716: the key is ALREADY compiling (queued by an earlier draw this
            // frame, or by a previous one still in the worker). There is no
            // promise of ours to take, but there IS one to wait on. Release BOTH
            // inflightMutex_ and queueMutex_ BEFORE calling WaitForGeomStagePso
            // (which locks queueMutex_ to check workerAlive_ and blocks on the
            // future -- calling it while holding queueMutex_ throws
            // std::system_error(resource_deadlock_would_occur) under MT prep).
            {
                std::lock_guard<std::mutex> ilock(inflightMutex_);
                auto it = inflightFutures_.find(key);
                if (it != inflightFutures_.end()) myFuture = it->second;
            }
        } else {
            myFuture = task.promise.get_future().share();
            queuedByMe = true;
            {
                std::lock_guard<std::mutex> ilock(inflightMutex_);
                inflightFutures_[key] = myFuture;
            }
            taskQueue_.push(std::move(task));
            queueCv_.notify_one();
            std::lock_guard<std::mutex> slock(statsMutex_);
            ++stats_.queued;
        }
    }

    if (!queuedByMe && !myFuture.valid()) {
        std::lock_guard<std::mutex> lock(mapMutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            if (outCarriesGeometryStage) {
                *outCarriesGeometryStage = (key.state.geometryStage == kGeomStageRectangleList);
            }
            return it->second;
        }
    }

    // A cold key waits for its own real PSO by default. Geometry-stage failures
    // always refuse; non-geometry failures also refuse unless the legacy
    // MCLA_NO_PSO_COLD_TEST_REFUSAL off-switch explicitly restores the test PSO.
    return WaitForGeomStagePso(key, caller, fallback, myFuture,
                               outCarriesGeometryStage);
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineCache::WaitForGeomStagePso(
    const PipelineKey& key, const char* caller,
    const Microsoft::WRL::ComPtr<ID3D12PipelineState>& fallback,
    const std::shared_future<Microsoft::WRL::ComPtr<ID3D12PipelineState>>& fut,
    bool* outCarriesGeometryStage) {
    if (outCarriesGeometryStage) *outCarriesGeometryStage = false;

    static std::atomic<uint32_t> s_coldRefusals{0};
    static const bool s_allowColdTestFallback = [] {
        const char* e = std::getenv("MCLA_NO_PSO_COLD_TEST_REFUSAL");
        return e && e[0] == '1';
    }();
    auto refuseCold = [&](const char* reason) {
        if (s_allowColdTestFallback && key.state.geometryStage == kGeomStageNone)
            return fallback;
        const uint32_t n = s_coldRefusals.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4 || (n % 200) == 0) {
            MCLA_LOG_WARN("PC-PSO-COLD-REFUSED #{} key={:016X} reason={} total={} "
                          "-- no real PSO; fixed test PSO refused",
                          n, HashPipelineKey(key), reason, n);
        }
        return Microsoft::WRL::ComPtr<ID3D12PipelineState>{};
    };

    // Owner default (2026-11-24): every cold key waits for its real PSO. The
    // earlier non-geometry immediate fallback remains opt-in only; it was not
    // the measured shadow cause and is not enabled here.
    static const bool s_nonGeomWait = [] {
        // 2026-11-24: default reverted to WAIT (owner: shadows gone with the fallback). Opt-in: MCLA_PSO_NONGEOM_FALLBACK=1.
        const char* e = std::getenv("MCLA_PSO_NONGEOM_FALLBACK");
        return !(e && e[0] == '1');
    }();
    if (!s_nonGeomWait && key.state.geometryStage == kGeomStageNone) {
        g_pcNonGeomFallbacks.fetch_add(1, std::memory_order_relaxed);
        return fallback;
    }

    // Wait for in-flight compile task so the draw receives a real translated PSO
    // matching its input layout rather than the fixed-stride test PSO (which refuses non-28 strides).
    if (!fut.valid()) {
        if (key.state.geometryStage == kGeomStageNone)
            return refuseCold("no-inflight-future");
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.geomStageWaitFailed;
        }
        RefuseOnce(key, kRefuseGeometryStageUnsupported, caller,
                   "geometry-stage key with no in-flight compile to wait for; "
                   "refused rather than drawn against the GS-less fallback");
        return nullptr;
    }
    // If the worker has already exited, no task will ever be fulfilled. Checked
    // under queueMutex_ (workerAlive_ is written there too), NOT via
    // workerThread_.joinable(), which StopWorker mutates without that lock.
    bool workerAlive = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        workerAlive = workerAlive_;
    }
    if (!workerAlive) {
        if (key.state.geometryStage == kGeomStageNone)
            return refuseCold("worker-not-alive");
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.geomStageWaitFailed;
        }
        RefuseOnce(key, kRefuseGeometryStageUnsupported, caller,
                   "the compile worker is not running, so this geometry-stage PSO "
                   "will never arrive; refused rather than drawn against the "
                   "GS-less fallback");
        return nullptr;
    }

    // WAIT HERE, with no lock held.
    //
    // The worker fulfils the promise on every path, including all six refusals
    // (failTask and the four inline set_value sites). A task pushed while the
    // worker is draining a stop request is still popped and fulfilled before it
    // returns, so this cannot block forever; kGeomStageWaitTimeoutMs is the
    // backstop for a wedged DXC.
    const int64_t waitT0 = PcNowNs();
    auto status = fut.wait_for(std::chrono::milliseconds(kGeomStageWaitTimeoutMs));
    PcAdd(g_pcWaitNs, g_pcWaitMaxNs, g_pcWaits, PcNowNs() - waitT0);  // PERF-STAGES psoWait: draw thread blocked on a compile
    if (status != std::future_status::ready) {
        static std::atomic<uint32_t> s_waitTimeout{0};
        const uint32_t n = s_waitTimeout.fetch_add(1) + 1;
        if (n <= 4 || (n % 200) == 0)
            MCLA_LOG_WARN("PC-PSO-WAIT-TIMEOUT #{}: waited {} ms for the "
                          "PSO of key={:016X} and it did not "
                          "arrive. The draw is REFUSED, not drawn against the "
                          "fallback. This is a new marker.",
                          n, static_cast<long>(kGeomStageWaitTimeoutMs),
                          static_cast<unsigned long long>(HashPipelineKey(key)));
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.geomStageWaitFailed;
        }
        return key.state.geometryStage == kGeomStageNone
                   ? refuseCold("wait-timeout")
                   : Microsoft::WRL::ComPtr<ID3D12PipelineState>{};
    }

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso = fut.get();
    if (!pso) {
        // The worker refused this key and said why on its own line.
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.geomStageWaitFailed;
        }
        return key.state.geometryStage == kGeomStageNone
                   ? refuseCold("worker-refused")
                   : Microsoft::WRL::ComPtr<ID3D12PipelineState>{};
    }

    // The claim, and exactly what backs it: this PSO came back from the task
    // whose key named kGeomStageRectangleList, and the worker binds
    // pGeometryShader only for such a key (pipeline_cache.cpp:1870-1872) and only
    // from a blob DXC compiled (:1835). No other writer inserts such a key, so
    // "this PSO carries the stage" is read off the artifact, not inferred from
    // the device.
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.geomStageWaited;
    }
    if (outCarriesGeometryStage) {
        *outCarriesGeometryStage = (key.state.geometryStage == kGeomStageRectangleList);
    }
    return pso;
}

void PipelineCache::SetFallbackPipeline(Microsoft::WRL::ComPtr<ID3D12PipelineState> fallback) {
    std::lock_guard<std::mutex> lock(fallbackMutex_);
    fallbackPso_ = fallback;
}

bool PipelineCache::Find(const PipelineKey& key, Microsoft::WRL::ComPtr<ID3D12PipelineState>& pso) const {
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    pso = it->second;
    return pso != nullptr;
}

void PipelineCache::Insert(const PipelineKey& key, Microsoft::WRL::ComPtr<ID3D12PipelineState> pso) {
    if (!pso) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.nullInsertRejected;
        return;
    }

    std::lock_guard<std::mutex> lock(mapMutex_);

    // Evict if at limit (FIFO). Clear the evicted key's in-flight marker so a
    // later request can recompile it; a pending key is never in the map (the
    // worker clears pendingKeys_ right after Insert), so this is normally a
    // no-op and keeps lock order map_ -> queue_ consistent with the worker.
    if (order_.size() >= maxEntries_ && !order_.empty()) {
        const PipelineKey& oldest = order_.front();
        map_.erase(oldest);
        generation_.fetch_add(1, std::memory_order_release);
        cbvBindings_.erase(oldest);
        samplerMasks_.erase(oldest);
        prewarmedKeys_.erase(oldest);
        order_.erase(order_.begin());
        {
            std::lock_guard<std::mutex> qlock(queueMutex_);
            pendingKeys_.erase(oldest);
        }
    }

    if (map_.count(key)) return;
    map_.emplace(key, pso);
    generation_.fetch_add(1, std::memory_order_release);
    order_.push_back(key);
}

void PipelineCache::Clear() {
    std::lock_guard<std::mutex> lock(mapMutex_);
    map_.clear();
    generation_.fetch_add(1, std::memory_order_release);
    cbvBindings_.clear();
    samplerMasks_.clear();
    prewarmedKeys_.clear();
    order_.clear();
    {
        std::lock_guard<std::mutex> qlock(queueMutex_);
        failedKeys_.clear();
        failedOrder_.clear();
        failedCount_.store(0, std::memory_order_release);
        failedGeneration_.fetch_add(1, std::memory_order_release);
    }
}

PipelineCacheStats PipelineCache::Stats() const {
    PipelineCacheStats out;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        out = stats_;
    }
    out.lookups = statLookups_.load(std::memory_order_relaxed);
    out.hits = statHits_.load(std::memory_order_relaxed);
    out.misses = statMisses_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mapMutex_);
        out.size = static_cast<uint32_t>(map_.size());
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        out.inflight = static_cast<uint32_t>(pendingKeys_.size());
    }
    return out;
}

std::vector<ConstantBankBinding> PipelineCache::ConstantBindingsFor(const PipelineKey& key) const {
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = cbvBindings_.find(key);
    if (it == cbvBindings_.end()) return {};
    return it->second;
}

uint32_t PipelineCache::SamplerMaskFor(const PipelineKey& key) const {
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = samplerMasks_.find(key);
    if (it == samplerMasks_.end()) return 0u;
    return it->second;
}

} // namespace mcla::renderer