#include "vertex_decode.h"

namespace mcla::native {

// DXGI_FORMAT numeric values (dxgiformat.h, Windows SDK 10.0.26100.0). Kept
// as integer constants so this module has no D3D12 header dependency and stays
// headlessly testable. Values verified against the installed SDK header:
// C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared\dxgiformat.h
enum {
    kDxgiR32G32B32A32Float  = 2,
    kDxgiR32G32B32A32Uint   = 3,
    kDxgiR32G32B32Float     = 6,
    kDxgiR32G32B32Uint      = 7,
    kDxgiR16G16B16A16Float  = 10,
    kDxgiR16G16B16A16Unorm  = 11,
    kDxgiR16G16B16A16Uint   = 12,
    kDxgiR32G32Float        = 16,
    kDxgiR32G32Uint         = 17,
    kDxgiR10G10B10A2Unorm   = 24,
    kDxgiR11G11B10Float     = 26,
    kDxgiR8G8B8A8Unorm      = 28,
    kDxgiR8G8B8A8Uint       = 30,
    kDxgiR8G8B8A8Snorm      = 31,
    kDxgiR8G8B8A8Sint       = 32,
    kDxgiR16G16Float        = 34,
    kDxgiR16G16Unorm        = 35,
    kDxgiR32Float           = 41,
    kDxgiR32Uint            = 42,
};

// Xenos vertex format codes (.research/xenia/src/xenia/gpu/xenos.h:602 VertexFormat).
// 6-bit "vf" field of a VFETCH instruction.
static VertexFormatDesc MakeDesc(bool valid, VertexType type, uint8_t comps,
                                 uint8_t compBytes, uint16_t totalBytes,
                                 uint32_t dxgi, bool isFloat,
                                 bool isNorm, bool isInt) {
    VertexFormatDesc d;
    d.valid = valid;
    d.type = type;
    d.componentCount = comps;
    d.componentBytes = compBytes;
    d.totalBytes = totalBytes;
    d.dxgiFormat = dxgi;
    d.isFloat = isFloat;
    d.isNormalized = isNorm;
    d.isInteger = isInt;
    return d;
}

// vf table <-> stride mapping (long-term record, measured 2026-11-09):
// The switch below covers EXACTLY the 15 defined Xenos codes
// (.research/xenia/src/xenia/gpu/xenos.h:602-619 VertexFormat); every
// other 6-bit value (incl. 0) is Undefined and takes the fetch-constant /
// unsupported path. totalBytes is the FULL-format size = words*4 from
// GetVertexFormatNeededWords (xenos.h:648-683), i.e. the PER-ELEMENT
// contribution -- it is NOT the whole-vertex stride. The whole stride
// is the VFETCH instruction's 8-bit stride byte
// (xenos_microcode.h:136 stride; xenos_microcode.cpp:170) and the guest
// element stride echoed in capture (fmtX, st36). Summing totalBytes
// therefore does NOT give the stride whenever element offsets gap.
// Stride-36 triple used by the star AND the R/logo quads (same format,
// different counts/topologies):
//   e0 fmt57 (32_32_32_FLOAT) @off0  st36  -> 12B position
//   e1 fmt6  (8_8_8_8)       @off24 st36  ->  4B colour
//   e2 fmt37 (32_32_FLOAT)   @off28 st36  ->  8B UV
//   off12..23 (12B) = UNFETCHED PADDING (zero in every sample below),
//   not a semantic lane; never invent it (Golden Rule 5).
//   Evidence: H1n.log:5278 (TEMP-T75-FIRST #2/23) elems
//   [e0:off0,st36,fmt57;e1:off24,st36,fmt6;e2:off28,st36,fmt37] size=216
//   = 6*36; d3d12_backend.cpp:5647-5670 (STAR probe reads pos @+0/+4,
//   colour @+24, UV @+28/+32 -- the same offsets); H1n.log:4595
//   TEMP-STAR #1 verts=39 stride=36; H1n.log:858 TEMP-G3 stride36
//   verts=4 v0=6866AA43... unpacks (LE) as (340.8,280.2,0,0,0,0,
//   colour 77777717, 0.977, 0.039), confirming off8..23 zero.
//   Counts/topologies of this one layout: star prim4/topo4 verts=39
//   (13 triangles, H1n.log:4595), logo quad prim4/topo4 verts=6
//   (2 triangles, H1n.log:818), R/rect quad prim6/topo5 verts=4
//   (1 quad, H1n.log:820), all indexed=0, size = count*36.
// Controls proving padding is a descriptor fact, not a vf fact:
// stride28 (H1n.log:5277) fmt57@0 + fmt38@12 fills exactly (12+16=28,
// size=84=3*28); stride8 (T75 #23) fmt37@0 fills exactly (8, size=24).
VertexFormatDesc DecodeVertexFormat(uint32_t vfCode) {
    switch (vfCode) {
        case 6:   return MakeDesc(true, VertexType::Unorm8x4,       4, 1, 4,  kDxgiR8G8B8A8Unorm,     false, true,  false); // 8_8_8_8
        case 7:   return MakeDesc(true, VertexType::Unorm2_10_10_10,4, 4, 4,  kDxgiR10G10B10A2Unorm,  false, true,  false); // 2_10_10_10
        case 16:  return MakeDesc(true, VertexType::R11G11B10Float, 3, 4, 4,  kDxgiR11G11B10Float,    true,  false, false); // 10_11_11
        case 17:  return MakeDesc(true, VertexType::R11G11B10Float, 3, 4, 4,  kDxgiR11G11B10Float,    true,  false, false); // 11_11_10
        case 25:  return MakeDesc(true, VertexType::Unorm16,        2, 2, 4,  kDxgiR16G16Unorm,       false, true,  false); // 16_16
        case 26:  return MakeDesc(true, VertexType::Unorm16x4,      4, 2, 8,  kDxgiR16G16B16A16Unorm, false, true,  false); // 16_16_16_16
        case 31:  return MakeDesc(true, VertexType::Float16,        2, 2, 4,  kDxgiR16G16Float,       true,  false, false); // 16_16 FLOAT
        case 32:  return MakeDesc(true, VertexType::Float16x4,      4, 2, 8,  kDxgiR16G16B16A16Float, true,  false, false); // 16_16_16_16 FLOAT
        case 33:  return MakeDesc(true, VertexType::Int32,          1, 4, 4,  kDxgiR32Uint,           false, false, true);  // 32
        case 34:  return MakeDesc(true, VertexType::Int32x2,        2, 4, 8,  kDxgiR32G32Uint,        false, false, true);  // 32_32
        case 35:  return MakeDesc(true, VertexType::Int32x4,        4, 4, 16, kDxgiR32G32B32A32Uint,  false, false, true);  // 32_32_32_32
        case 36:  return MakeDesc(true, VertexType::Float32,        1, 4, 4,  kDxgiR32Float,          true,  false, false); // 32 FLOAT
        case 37:  return MakeDesc(true, VertexType::Float32x2,      2, 4, 8,  kDxgiR32G32Float,       true,  false, false); // 32_32 FLOAT
        case 38:  return MakeDesc(true, VertexType::Float32x4,      4, 4, 16, kDxgiR32G32B32A32Float, true,  false, false); // 32_32_32_32 FLOAT
        case 57:  return MakeDesc(true, VertexType::Float32x3,      3, 4, 12, kDxgiR32G32B32Float,    true,  false, false); // 32_32_32 FLOAT
        default:  return MakeDesc(false, VertexType::Unsupported,   0, 0, 0, 0, false, false, false);
}
}


VertexFormatDesc DecodeVertexFetch(uint32_t vfCode, uint32_t constIndex,
                                   uint32_t constIndexSelect) {
    VertexFormatDesc d = DecodeVertexFormat(vfCode);
    if (d.valid) return d;
    // vf=0 in MCLA VFETCH carries no format: the runtime guest fetch-constant
    // descriptor supplies base/size/endian only, so mark it as needing that
    // descriptor and let the capture path resolve the real layout from the
    // captured declaration (grc_fvf_decode.h). Slot is `const_index * 3 +
    // const_index_select` (96 two-dword entries, 3 per 5-bit const_index).
    if (vfCode == 0) {
        d.fromFetchConstant = true;
        d.fetchConstantSelect = constIndexSelect & 0x3;
        d.fetchConstantIndex = ((constIndex & 0x1F) * 3) + d.fetchConstantSelect;
    }
    return d;
}

const char* VertexFormatName(uint32_t vfCode) {
    switch (vfCode) {
        case 6:  return "8_8_8_8";
        case 7:  return "2_10_10_10";
        case 16: return "10_11_11";
        case 17: return "11_11_10";
        case 25: return "16_16";
        case 26: return "16_16_16_16";
        case 31: return "16_16_float";
        case 32: return "16_16_16_16_float";
        case 33: return "32";
        case 34: return "32_32";
        case 35: return "32_32_32_32";
        case 36: return "32_float";
        case 37: return "32_32_float";
        case 38: return "32_32_32_32_float";
        case 57: return "32_32_32_float";
        default: return "unsupported";
}
}


uint32_t IndexElementBytes(uint32_t indexType) {
    switch (indexType) {
        case 0: return 2;   // 16-bit
        case 1: return 4;   // 32-bit
        default: return 0;  // non-indexed / unknown
}
}


VertexFetchConstant DecodeVertexFetchConstant(const uint8_t* p8) {
    VertexFetchConstant fc;
    uint32_t w0 = ((uint32_t(p8[0]) << 24) | (uint32_t(p8[1]) << 16) |
                   (uint32_t(p8[2]) << 8) | uint32_t(p8[3]));
    uint32_t w1 = ((uint32_t(p8[4]) << 24) | (uint32_t(p8[5]) << 16) |
                   (uint32_t(p8[6]) << 8) | uint32_t(p8[7]));
    if (w0 == 0 && w1 == 0) {
        fc.isZero = true;
        return fc;
    }
    // xe_gpu_vertex_fetch_t bit layout (.research/xenia/src/xenia/gpu/xenos.h:1113):
    //   w0[31:30] type/address_type, w0[29:0]  address (dwords)
    //   w1[31:30] endian,             w1[29:6]  size (words), w1[5:0] pad
    fc.addressType = (w0 >> 30) & 0x3;
    uint32_t addrDwords = w0 & 0x3FFFFFFF;
    fc.addressBytes = addrDwords * 4;
    fc.endian = (w1 >> 30) & 0x3;
    fc.sizeWords = (w1 >> 6) & 0xFFFFFF;
    if (fc.addressBytes <= 0x40000000 && fc.sizeWords != 0) fc.valid = true;
    return fc;
}

uint64_t HashVertexDeclaration(const uint32_t* vfCodes, const uint8_t* usages,
                               const uint8_t* usageIndices, uint32_t count) {
    uint64_t h = 14695981039346656037ULL; // FNV-1a 64-bit
    const uint64_t prime = 1099511628211ULL;
    auto mix = [&](uint8_t b) {
        h ^= uint64_t(b);
        h *= prime;
    };
    for (uint32_t i = 0; i < count; ++i) {
        VertexFormatDesc d = DecodeVertexFormat(vfCodes[i]);
        if (!d.valid) return 0;  // caller must not build a pipeline for unsupported input
        // Hash vfCode (4 bytes) + usage + usageIndex + resolved comps + totalBytes
        mix(uint8_t(vfCodes[i]));
        mix(uint8_t(vfCodes[i] >> 8));
        mix(uint8_t(vfCodes[i] >> 16));
        mix(uint8_t(vfCodes[i] >> 24));
        mix(usages ? usages[i] : 0);
        mix(usageIndices ? usageIndices[i] : 0);
        mix(uint8_t(d.componentCount));
        mix(uint8_t(d.totalBytes));
    }
    return h;
}

} // namespace mcla::native
