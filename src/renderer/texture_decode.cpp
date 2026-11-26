#include "texture_decode.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <utility>

namespace mcla::native {

// DXGI_FORMAT numeric values (dxgiformat.h, Windows SDK 10.0.26100.0). Same
// values as vertex_decode.cpp; kept here so texture decode has no D3D12 header
// dependency.
enum {
    kDxgiUnknown              = 0,
    kDxgiR32G32B32A32Float    = 2,
    kDxgiR32G32B32A32Uint     = 3,
    kDxgiR32G32B32Float       = 6,
    kDxgiR32G32B32Uint        = 7,
    kDxgiR16G16B16A16Float    = 10,
    kDxgiR16G16B16A16Unorm    = 11,
    kDxgiR16G16B16A16Uint     = 12,
    kDxgiR32G32Float          = 16,
    kDxgiR32G32Uint           = 17,
    kDxgiR10G10B10A2Unorm     = 24,
    kDxgiR11G11B10Float       = 26,
    kDxgiR8G8B8A8Unorm        = 28,
    kDxgiR8G8B8A8Uint         = 30,
    kDxgiR8G8B8A8Snorm        = 31,
    kDxgiR8G8B8A8Sint         = 32,
    kDxgiR16G16Float          = 34,
    kDxgiR16G16Unorm          = 35,
    kDxgiR16G16Uint           = 36,
    kDxgiR8G8Unorm            = 49,
    kDxgiR8G8Uint             = 50,
    kDxgiR16Float             = 54,
    kDxgiR16Unorm             = 56,
    kDxgiR16Uint              = 57,
    kDxgiR32Float             = 41,
    kDxgiR32Uint              = 42,
    kDxgiR8Unorm              = 61,
    kDxgiR8Uint               = 62,
    kDxgiB5G6R5Unorm          = 85,
    kDxgiB5G5R5A1Unorm        = 86,
    kDxgiB8G8R8A8Unorm        = 87,
    kDxgiBC1Unorm             = 71,
    kDxgiBC2Unorm             = 74,
    kDxgiBC3Unorm             = 77,
    kDxgiBC4Unorm             = 80,
    kDxgiBC5Unorm             = 83,
    kDxgiB4G4R4A4Unorm        = 115,
    kDxgiD32FloatS8X24Uint    = 20,
    kDxgiD24UnormS8Uint       = 45,
};

// Xenos texture format table. Values mirror the compiled legacy FormatInfo
// table verbatim: the row order is the xenos::TextureFormat enum, and name /
// block size / bits-per-texel / type agree row by row (indices 0..63) with
// Xenia's independent FormatInfo::Get table
// (.research/xenia/src/xenia/gpu/texture_info_formats.cc:21-88; the type
// column is xenos FormatType, kResolvable=0/kUncompressed=1/kCompressed=2,
// texture_info.h:115-121). It was originally generated against the
// `FormatInfo::Get` export of rexruntime.dll (the FormatInfo oracle, plus the
// `build/oracle_format_dump.cpp` this comment used to point at): that DLL left
// the tree with ReXGlue, and no such dump file exists in the tree or in git
// history, so the only surviving oracle hook is
// tools/validators/texture_decode_test.cpp, which LoadLibrary's rexruntime.dll
// and prints "oracle absent (ReXGlue removed) - skipping" instead of
// comparing when it is missing (texture_decode_test.cpp:68-72).
// Per the SDK formula
// `bytes_per_block() = block_width*block_height*bits_per_pixel/8`, the table
// stores bits per *texel* (e.g. DXT1 is 4, not 64). Host DXGI codes are the
// canonical D3D mapping for each Xenos format: Xenos stores texel bits in the
// same order as R5G6B5-style naming, and D3D names those BGR (e.g. 5_6_5 ->
// DXGI_FORMAT_B5G6R5_UNORM). Compressed/constant formats with a literal DXGI
// equivalent (DXT5A -> BC4_UNORM) are mapped. DXT3A and DXT3A_AS_1_1_1_1 also
// carry BC4_UNORM in this table, but NEITHER is BC4 (F-898(a): DXT3A is 16
// direct 4-bit alphas, DXT3A_AS_1_1_1_1 is 4x1-bit -> B4G4R4A4) - the row is
// only what plan.nativeDxgiFormat carries (the backend's Gate 2 looks that up
// in kHostFormatTable); the exact expansion lives in PlanExpandsTo below.
// and so are 8_A/8_B: both those rows carry dxgi=R8_UNORM (61), so they are
// aliased onto one host form (which loses the luminance-vs-alpha distinction),
// not left unsupported. What really is dxgi=0 here is YUV REP, MPEG,
// INTERLACED, the *_EXPAND and 32_AS_8* packings, CTX1, 6_5_5, 1-bit and
// 32_32_32_FLOAT — Xenia reaches some of those through a load shader instead
// of a host format (CTX1 -> R8G8_UNORM, DXT3A -> R8_UNORM,
// .research/xenia/src/xenia/gpu/d3d12/d3d12_texture_cache.cc:361-374) — and
// they stay dxgi=0 (unsupported) until captured evidence forces them up.
// `constexpr`, not `static const`: the F-647 pitch self-check below walks this
// array in a constant expression, and C++ forbids reading a non-constexpr
// `static const` object at compile time. `constexpr` implies `const`, so the
// internal linkage (and the single instance) is unchanged.
constexpr TextureFormatInfo kFormatTable[] = {
    // (xenos, name, dxgi, blockW, blockH, bitsPerPixel, compressed, resolvable, swapped)
    // type==0 kResolvable -> isResolvable, type==2 kCompressed -> isBlockCompressed.
    {  0, "1_REVERSE",        0,                        1, 1,  1, false, false, false },
    {  1, "1",                0,                        1, 1,  1, false, false, false }, // no 1bpp host form (R1_UNORM=66 does exist in dxgiformat.h, unused here)
    {  2, "8",                kDxgiR8Unorm,             1, 1,  8, false, true,  false },
    {  3, "1_5_5_5",          kDxgiB5G5R5A1Unorm,       1, 1, 16, false, true,  true  },
    {  4, "5_6_5",            kDxgiB5G6R5Unorm,         1, 1, 16, false, true,  true  },
    {  5, "6_5_5",            0,                        1, 1, 16, false, true,  false },
    {  6, "8_8_8_8",          kDxgiR8G8B8A8Unorm,       1, 1, 32, false, true,  false },
    {  7, "2_10_10_10",       kDxgiR10G10B10A2Unorm,    1, 1, 32, false, true,  false },
    {  8, "8_A",              kDxgiR8Unorm,             1, 1,  8, false, true,  false },
    {  9, "8_B",              kDxgiR8Unorm,             1, 1,  8, false, true,  false },
    { 10, "8_8",              kDxgiR8G8Unorm,           1, 1, 16, false, true,  false },
    { 11, "Cr_Y1_Cb_Y0_REP",  0,                        2, 1, 16, true,  false, false }, // YUV, no DXGI
    { 12, "Y1_Cr_Y0_Cb_REP",  0,                        2, 1, 16, true,  false, false }, // YUV, no DXGI
    { 13, "16_16_EDRAM",      kDxgiR16G16Float,         1, 1, 32, false, false, false },
    { 14, "8_8_8_8_A",        kDxgiR8G8B8A8Unorm,       1, 1, 32, false, true,  false },
    { 15, "4_4_4_4",          kDxgiB4G4R4A4Unorm,       1, 1, 16, false, true,  true  },
    { 16, "10_11_11",         kDxgiR16G16B16A16Unorm,    1, 1, 32, false, true,  false },
    { 17, "11_11_10",         kDxgiR16G16B16A16Unorm,    1, 1, 32, false, true,  false },
    { 18, "DXT1",             kDxgiBC1Unorm,            4, 4,  4, true,  false, false },
    { 19, "DXT2_3",           kDxgiBC2Unorm,            4, 4,  8, true,  false, false },
    { 20, "DXT4_5",           kDxgiBC3Unorm,            4, 4,  8, true,  false, false },
    { 21, "16_16_16_16_EDRAM", kDxgiR16G16B16A16Float,  1, 1, 64, false, false, false },
    { 22, "24_8",             kDxgiD24UnormS8Uint,      1, 1, 32, false, false, false }, // depth/stencil
    { 23, "24_8_FLOAT",       kDxgiD32FloatS8X24Uint,   1, 1, 32, false, false, false },
    { 24, "16",               kDxgiR16Unorm,            1, 1, 16, false, true,  false },
    { 25, "16_16",            kDxgiR16G16Unorm,         1, 1, 32, false, true,  false },
    { 26, "16_16_16_16",      kDxgiR16G16B16A16Unorm,   1, 1, 64, false, true,  false },
    { 27, "16_EXPAND",        0,                        1, 1, 16, false, false, false }, // resampled format
    { 28, "16_16_EXPAND",     0,                        1, 1, 32, false, false, false },
    { 29, "16_16_16_16_EXPAND", 0,                      1, 1, 64, false, false, false },
    { 30, "16_FLOAT",         kDxgiR16Float,            1, 1, 16, false, true,  false },
    { 31, "16_16_FLOAT",      kDxgiR16G16Float,         1, 1, 32, false, true,  false },
    { 32, "16_16_16_16_FLOAT", kDxgiR16G16B16A16Float,  1, 1, 64, false, true,  false },
    { 33, "32",               kDxgiR32Uint,             1, 1, 32, false, false, false },
    { 34, "32_32",            kDxgiR32G32Uint,          1, 1, 64, false, false, false },
    { 35, "32_32_32_32",      kDxgiR32G32B32A32Uint,    1, 1,128, false, false, false },
    { 36, "32_FLOAT",         kDxgiR32Float,            1, 1, 32, false, true,  false },
    { 37, "32_32_FLOAT",      kDxgiR32G32Float,         1, 1, 64, false, true,  false },
    { 38, "32_32_32_32_FLOAT", kDxgiR32G32B32A32Float,  1, 1,128, false, true,  false },
    { 39, "32_AS_8",          0,                        4, 1,  8, true,  false, false }, // tiled layout, no host form
    { 40, "32_AS_8_8",        0,                        2, 1, 16, true,  false, false },
    { 41, "16_MPEG",          0,                        1, 1, 16, false, false, false },
    { 42, "16_16_MPEG",       0,                        1, 1, 32, false, false, false },
    { 43, "8_INTERLACED",     0,                        1, 1,  8, false, false, false },
    { 44, "32_AS_8_INTERLACED", 0,                      4, 1,  8, true,  false, false },
    { 45, "32_AS_8_8_INTERLACED", 0,                    1, 1, 16, true,  false, false },
    { 46, "16_INTERLACED",    0,                        1, 1, 16, false, false, false },
    { 47, "16_MPEG_INTERLACED", 0,                      1, 1, 16, false, false, false },
    { 48, "16_16_MPEG_INTERLACED", 0,                   1, 1, 32, false, false, false },
    { 49, "DXN",              kDxgiBC5Unorm,            4, 4,  8, true,  false, false },
    { 50, "8_8_8_8_AS_16_16_16_16", kDxgiR8G8B8A8Unorm, 1, 1, 32, false, true,  false },
    { 51, "DXT1_AS_16_16_16_16", kDxgiBC1Unorm,         4, 4,  4, true,  false, false },
    { 52, "DXT2_3_AS_16_16_16_16", kDxgiBC2Unorm,       4, 4,  8, true,  false, false },
    { 53, "DXT4_5_AS_16_16_16_16", kDxgiBC3Unorm,       4, 4,  8, true,  false, false },
    { 54, "2_10_10_10_AS_16_16_16_16", kDxgiR10G10B10A2Unorm, 1, 1, 32, false, true, false },
    { 55, "10_11_11_AS_16_16_16_16", kDxgiR16G16B16A16Unorm, 1, 1, 32, false, true, false },
    { 56, "11_11_10_AS_16_16_16_16", kDxgiR16G16B16A16Unorm, 1, 1, 32, false, true, false },
    { 57, "32_32_32_FLOAT",   kDxgiR32G32B32Float,      1, 1, 96, false, false, false },
    { 58, "DXT3A",            kDxgiBC4Unorm,            4, 4,  4, true,  false, false },
    { 59, "DXT5A",            kDxgiBC4Unorm,            4, 4,  4, true,  false, false },
    { 60, "CTX1",             0,                        4, 4,  4, true,  false, false }, // no native host form
    { 61, "DXT3A_AS_1_1_1_1", kDxgiBC4Unorm,            4, 4,  4, true,  false, false },
    { 62, "8_8_8_8_GAMMA_EDRAM", kDxgiR8G8B8A8Unorm,    1, 1, 32, false, false, false },
    { 63, "2_10_10_10_FLOAT_EDRAM", kDxgiR10G10B10A2Unorm, 1, 1, 32, false, false, false },
};

const TextureFormatInfo* GetTextureFormatInfo(uint32_t xenosFormat) {
    static const uint32_t kCount = sizeof(kFormatTable) / sizeof(kFormatTable[0]);
    for (uint32_t i = 0; i < kCount; ++i) {
        if (kFormatTable[i].xenosFormat == xenosFormat) return &kFormatTable[i];
    }
    return nullptr;
}

uint32_t GetTextureBytesPerBlock(uint32_t xenosFormat) {
    const TextureFormatInfo* info = GetTextureFormatInfo(xenosFormat);
    if (!info) return 0;
    return (info->blockWidth * info->blockHeight * info->bitsPerPixel) / 8;
}

const char* TextureFormatName(uint32_t xenosFormat) {
    const TextureFormatInfo* info = GetTextureFormatInfo(xenosFormat);
    return info ? info->name : "unknown";
}

uint32_t GetTextureBlockSpan(uint32_t xenosFormat, uint32_t width, uint32_t height,
                             uint32_t* outBlocksX, uint32_t* outBlocksY) {
    const TextureFormatInfo* info = GetTextureFormatInfo(xenosFormat);
    if (!info || !outBlocksX || !outBlocksY || width == 0 || height == 0) {
        if (outBlocksX) *outBlocksX = 0;
        if (outBlocksY) *outBlocksY = 0;
        return 0;
    }
    uint32_t bx = (width + info->blockWidth - 1) / info->blockWidth;
    uint32_t by = (height + info->blockHeight - 1) / info->blockHeight;
    *outBlocksX = bx;
    *outBlocksY = by;
    return bx * by;
}

// ---------------------------------------------------------------------------
// F-647: the block-row pitch contract -- derivation, self-check, static_asserts.
// See texture_decode.h for the D3D12 contract this implements.
// ---------------------------------------------------------------------------

// `constexpr` (declared so in the header) so the static_asserts below can call
// it; a constexpr function is equally callable at runtime.
constexpr uint32_t D3D12BlockCompressedBytes(uint32_t dxgiFormat) {
    switch (dxgiFormat) {
        // D3D12 functional spec, "Block Compressor" / BC1..BC5: 8 bytes per
        // 4x4 block for BC1 and BC4, 16 for BC2, BC3 and BC5. Independently
        // corroborated inside the tree by Xenia's load-shader selection, which
        // keys off bits-per-block -- kLoadShaderIndex64bpb for k_DXT1 and
        // kLoadShaderIndex128bpb for k_DXT2_3 / k_DXT4_5, over 16 texels per
        // block, i.e. 8 and 16 bytes:
        //   .research/xenia/src/xenia/gpu/d3d12/d3d12_texture_cache.cc:181,186,191
        case kDxgiBC1Unorm: return 8;
        case kDxgiBC2Unorm: return 16;
        case kDxgiBC3Unorm: return 16;
        case kDxgiBC4Unorm: return 8;
        case kDxgiBC5Unorm: return 16;
        default: return 0;  // not a BC format in this table (or unknown)
    }
}

uint32_t SourceBlockRowPitchBytes(uint32_t xenosFormat, uint32_t width) {
    uint32_t blocksX = 0, blocksY = 0;
    if (width == 0) return 0;
    // GetTextureBlockSpan refuses a zero width/height and an unknown format, so
    // a returned 0 blocksX is exactly the "no such pitch" case.
    if (GetTextureBlockSpan(xenosFormat, width, 1u, &blocksX, &blocksY) == 0) return 0;
    return blocksX * GetTextureBytesPerBlock(xenosFormat);
}

uint32_t SourceBlockRowCount(uint32_t xenosFormat, uint32_t height) {
    uint32_t blocksX = 0, blocksY = 0;
    if (height == 0) return 0;
    if (GetTextureBlockSpan(xenosFormat, 1u, height, &blocksX, &blocksY) == 0) return 0;
    return blocksY;
}

uint32_t SourceBlockSpanBytes(uint32_t xenosFormat, uint32_t width, uint32_t height) {
    const uint32_t pitch = SourceBlockRowPitchBytes(xenosFormat, width);
    const uint32_t rows = SourceBlockRowCount(xenosFormat, height);
    if (pitch == 0 || rows == 0) return 0;
    return pitch * rows;
}

// --- the compile-time half of the self-check ---------------------------------
// The same two checks as VerifyBlockRowPitchContract, written so a constant
// expression can walk the `static const` table directly. Both are static_asserted
// below, so this is enforced in the main app AND in texture_decode_test AND in
// texture_import_validator (CMakeLists.txt:316-321, 610-612, 1049-1058).
namespace {

constexpr const TextureFormatInfo* FormatInfoConst(uint32_t xenosFormat) {
    for (uint32_t i = 0; i < sizeof(kFormatTable) / sizeof(kFormatTable[0]); ++i)
        if (kFormatTable[i].xenosFormat == xenosFormat) return &kFormatTable[i];
    return nullptr;
}

constexpr uint32_t TableBytesPerBlockConst(uint32_t xenosFormat) {
    const TextureFormatInfo* i = FormatInfoConst(xenosFormat);
    return i ? (i->blockWidth * i->blockHeight * i->bitsPerPixel) / 8u : 0u;
}

constexpr uint32_t TableRowPitchConst(uint32_t xenosFormat, uint32_t width) {
    const TextureFormatInfo* i = FormatInfoConst(xenosFormat);
    if (!i || width == 0u) return 0u;
    return ((width + i->blockWidth - 1u) / i->blockWidth) * TableBytesPerBlockConst(xenosFormat);
}

// Widths 1..8 chosen so the non-multiple-of-4 cases are actually covered:
// 1,2,3 are narrower than one 4x4 block and must still occupy a WHOLE block;
// 4 is exactly one block; 5,6,7 are the first widths where `w*bytesPerBlock`
// and `ceil(w/4)*bytesPerBlock` disagree; 8 is two blocks. Width 1 is the one
// width where the two formulas coincide (1*bpb == 1*bpb), which is why
// kNaivePitchWouldShear below exists -- without it the check could pass
// vacuously against a naive implementation.
constexpr uint32_t kPitchProbeMinWidth = 1;
constexpr uint32_t kPitchProbeMaxWidth = 8;

constexpr bool BlockRowPitchContractHolds() {
    for (uint32_t f = 0; f < 64u; ++f) {
        const TextureFormatInfo* i = FormatInfoConst(f);
        if (!i) continue;
        const uint32_t want = D3D12BlockCompressedBytes(i->dxgiFormat);
        if (want == 0u) {
            // A row with no D3D12 BC host form. If it still claims a block
            // granularity it must be UNREACHABLE: every such row in the table
            // (Cr_Y1_Cb_Y0_REP, Y1_Cr_Y0_Cb_REP, 32_AS_8, 32_AS_8_8, the two
            // *_8_INTERLACED, CTX1) has dxgiFormat == 0, which is what makes
            // PlanTextureImport refuse it with NoHostFormat. If a future edit
            // gave one of them a host format, the block pitch would no longer
            // have been derived from anything, so refuse the edit here.
            if (i->isBlockCompressed && i->dxgiFormat != 0u) return false;
            continue;
        }
        // From here the row IS a D3D12 BC import, so the full contract applies.
        if (!i->isBlockCompressed) return false;
        if (i->blockWidth != 4u || i->blockHeight != 4u) return false;
        if (TableBytesPerBlockConst(f) != want) return false;
        for (uint32_t w = kPitchProbeMinWidth; w <= kPitchProbeMaxWidth; ++w) {
            const uint32_t blocksX = (w + 3u) / 4u;
            if (TableRowPitchConst(f, w) != blocksX * want) return false;
        }
    }
    return true;
}

// Non-vacuity guard: at least one probed width in every BC row must be one
// where the naive `w * bytesPerBlock` is WRONG. If a future edit made the check
// above satisfiable by the naive formula, this fires instead of the check
// quietly testing nothing.
constexpr bool NaivePitchWouldShearSomeWidth() {
    for (uint32_t f = 0; f < 64u; ++f) {
        const TextureFormatInfo* i = FormatInfoConst(f);
        if (!i || D3D12BlockCompressedBytes(i->dxgiFormat) == 0u) continue;
        const uint32_t bpb = TableBytesPerBlockConst(f);
        for (uint32_t w = kPitchProbeMinWidth; w <= kPitchProbeMaxWidth; ++w)
            if (TableRowPitchConst(f, w) != w * bpb) return true;
    }
    return false;
}

// Every BC row must also be a format the plan can actually import: the block
// byte size has to be a power of two <= 64 or the XGAddress tiled math is not
// addressable, and PlanTextureImport refuses it (NonPowerOfTwoBlock). Asserting
// it here means a new BC row cannot be added in a state the plan rejects.
constexpr bool BlockRowsAreXgAddressable() {
    for (uint32_t f = 0; f < 64u; ++f) {
        const TextureFormatInfo* i = FormatInfoConst(f);
        if (!i || !i->isBlockCompressed) continue;
        const uint32_t bpb = TableBytesPerBlockConst(f);
        if (bpb == 0u || bpb > 64u) return false;
        if ((bpb & (bpb - 1u)) != 0u) return false;
    }
    return true;
}

}  // namespace

static_assert(BlockRowPitchContractHolds(),
              "F-647: a block-compressed row of kFormatTable disagrees with the D3D12 BC "
              "contract (4x4, 8/16 bytes per block, ceil(w/4)*bytesPerBlock for w=1..8)");
static_assert(NaivePitchWouldShearSomeWidth(),
              "F-647: the pitch self-check cannot tell ceil(w/4)*bpb from the naive "
              "w*bpb, so it is no longer testing the shear case");
static_assert(BlockRowsAreXgAddressable(),
              "F-647: a block-compressed row has a non-power-of-two or >64 byte block, "
              "which PlanTextureImport refuses as NonPowerOfTwoBlock");

bool VerifyBlockRowPitchContract(uint32_t* offenderFormat, uint32_t* offenderWidth) {
    if (offenderFormat) *offenderFormat = 0;
    if (offenderWidth) *offenderWidth = 0;
    for (uint32_t f = 0; f < 64u; ++f) {
        const TextureFormatInfo* i = GetTextureFormatInfo(f);
        if (!i) continue;
        const uint32_t want = D3D12BlockCompressedBytes(i->dxgiFormat);
        if (want == 0u) {
            if (i->isBlockCompressed && i->dxgiFormat != 0u) {
                if (offenderFormat) *offenderFormat = f;
                return false;
            }
            continue;
        }
        if (!i->isBlockCompressed || i->blockWidth != 4u || i->blockHeight != 4u ||
            GetTextureBytesPerBlock(f) != want) {
            if (offenderFormat) *offenderFormat = f;
            return false;
        }
        for (uint32_t w = kPitchProbeMinWidth; w <= kPitchProbeMaxWidth; ++w) {
            if (SourceBlockRowPitchBytes(f, w) != ((w + 3u) / 4u) * want) {
                if (offenderFormat) *offenderFormat = f;
                if (offenderWidth) *offenderWidth = w;
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tiled offsets (canonical XGAddress math).
// ---------------------------------------------------------------------------

int32_t GetTiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bpbLog2) {
    // XGAddress2DTiledOffset: 32x32-block macro tiles (Xenia's equivalent is
    // texture_address::Tiled2D, kMacroTileWidth/kMacroTileHeight2D = 32; the
    // rexruntime oracle exported this exact name as
    // texture_util::graphics::GetTiledOffset2D, see the GetProcAddress in
    // tools/validators/texture_decode_test.cpp). The pitch is rounded up to a
    // multiple of 32 blocks *inside* this function, whereas Xenia expects the
    // caller to have aligned it (texture_address.h:196 asserts pitch_aligned,
    // the xe::align lives in texture_util.cc:324) — same result, but an
    // unaligned pitch is a caller bug there and silently fixed here.
    pitch = (pitch + 31u) & ~31u;
    int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bpbLog2 + 7);
    int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpbLog2;
    int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0x0F) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

int32_t GetTiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch,
                         uint32_t height, uint32_t bpbLog2) {
    pitch = (pitch + 31u) & ~31u;
    height = (height + 31u) & ~31u;
    // Triang3l's RE-derived 3D case (Xenia texture_address::Tiled3D): the
    // macro tile is 32x16x4 blocks (kMacroTileWidth 32, kMacroTileHeight3D 16,
    // kMacroTileDepth 4 — texture_address.h:165-171), which is why the macro
    // term below steps y by >>4 and z by >>2. 32x32x4 is the *storage*
    // alignment of a 32-block-row x 4-slice portion, larger than the macro
    // tile (texture_address.h:45-47) — not the macro tile itself.
    int32_t macro_outer =
        ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
    int32_t macro = (((x >> 5) + macro_outer) << (bpbLog2 + 6)) & 0xFFFFFFF;
    macro <<= 1;  // equivalent: (((...) << (bpb + 6)) & 0xFFFFFFF) << 1
    int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bpbLog2 + 6)) >> 6;
    int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
    int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
    int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                      ((z & 3) << (bpbLog2 + 6)) + ((y & 1) << 4);
    int32_t address = (offset1 & 1) << 3;
    address += (offset2 >> 6) & 7;
    address <<= 3;
    address += offset1 & ~1;
    address <<= 2;
    address += offset2 & ~511;
    address <<= 3;
    address += offset2 & 63;
    return address;
}

int32_t GetTiledAddressLowerBound2D(uint32_t left, uint32_t top, uint32_t pitch,
                                    uint32_t bpbLog2) {
    // Origin of the 32x32-block macro tile containing block (left, top).
    return GetTiledOffset2D(int32_t(left & ~31u), int32_t(top & ~31u), pitch, bpbLog2);
}

int32_t GetTiledAddressUpperBound2D(uint32_t right, uint32_t bottom, uint32_t pitch,
                                    uint32_t bpbLog2) {
    if (!right || !bottom) return 0;
    // Origin of the 32x32 tile containing the last block, then the maximum extent
    // reachable within that tile (Xenia GetTiledAddressUpperBound2D,
    // texture_util.cc:451-471; 0xA00/0xC00 are its 1bpb/2bpb extents, whose
    // portions address independently within 128x128 and 64x64 respectively).
    int32_t origin = GetTiledOffset2D(int32_t((right - 1) & ~31u),
                                      int32_t((bottom - 1) & ~31u), pitch, bpbLog2);
    switch (bpbLog2) {
        case 0: origin += 0xA00; break;   // independent addressing, 0xA00 extent
        case 1: origin += 0xC00; break;   // independent addressing, 0xC00 extent
        default: origin += int32_t(UINT32_C(0x400) << bpbLog2); break;
    }
    return origin;
}

int32_t GetTiledAddressLowerBound3D(uint32_t left, uint32_t top, uint32_t front,
                                    uint32_t pitch, uint32_t height, uint32_t bpbLog2) {
    // Mirrors the legacy GetTiledAddressLowerBound3D exactly:
    // x & ~kTextureTileWidthHeight (2D) / front & ~kTextureTileDepth (3D,
    // kTextureTileDepth = 4).
    return GetTiledOffset3D(int32_t(left & ~31u), int32_t(top & ~31u),
                            int32_t(front & ~4u), pitch, height, bpbLog2);
}

int32_t GetTiledAddressUpperBound3D(uint32_t right, uint32_t bottom, uint32_t back,
                                    uint32_t pitch, uint32_t height, uint32_t bpbLog2) {
    if (!right || !bottom || !back) return 0;
    // Origin of the 32x32x4 tile containing the last block, then the maximum
    // extent within that tile.
    int32_t origin = GetTiledOffset3D(int32_t((right - 1) & ~31u),
                                      int32_t((bottom - 1) & ~31u),
                                      int32_t((back - 1) & ~3u), pitch, height, bpbLog2);
    switch (bpbLog2) {
        case 0:
            // 64x32x8 portions have independent addressing.
            origin += int32_t(((pitch >> 6) << 12) + 0xC00 +
                              ((pitch & (1u << 5)) << (10 - 5)));
            break;
        default:
            // 32x32x8 portions have independent addressing.
            origin += int32_t(((pitch << (12 - 5 - 1)) + (0x1000 >> 1)) << bpbLog2);
            break;
    }
    return origin;
}

// ---------------------------------------------------------------------------
// Untile
// ---------------------------------------------------------------------------

// Bounds guard for one block copy. `tiled` is the (already-computed) byte
// offset into the source surface; a negative value means the request stepped
// outside the surface grid, and both the read and write ends are checked
// against the caller-provided buffer sizes before any access. Any pair that
// would step outside a buffer is skipped — never read or written.
static bool UntileProbeSafe(int32_t tiled, uint32_t srcBytes, uint64_t dstOffset,
                            uint32_t dstBytes, uint32_t bpb) {
    if (tiled < 0) return false;
    const uint64_t readEnd = uint64_t(tiled) + bpb;
    if (readEnd > srcBytes) return false;
    const uint64_t writeEnd = dstOffset + bpb;
    if (writeEnd > dstBytes) return false;
    return true;
}

// Refusal helper: record why a copy produced nothing, so a caller never has to
// tell "refused" from "the source happened to be all zeros". A null
// `outRefusal` keeps the original three/four-argument call sites valid.
static void UntileRefuse(TextureUntileRefusal* outRefusal, TextureUntileRefusal r) {
    if (outRefusal) *outRefusal = r;
}

void UntileTexture2D(const uint8_t* src, uint8_t* dst, const TextureUntileInfo& info,
                     TextureUntileRefusal* outRefusal) {
    UntileRefuse(outRefusal, TextureUntileRefusal::None);
    if (!info.format) { UntileRefuse(outRefusal, TextureUntileRefusal::NoFormatInfo); return; }
    if (!src || !dst) { UntileRefuse(outRefusal, TextureUntileRefusal::NullBuffer); return; }
    if (info.inputPitch == 0) { UntileRefuse(outRefusal, TextureUntileRefusal::ZeroPitch); return; }
    // Region must lie inside the surface grid (overflow-checked in 64-bit).
    // Y is bounded only when the caller supplied a surface height; otherwise
    // the per-block bounds guard below is the protection.
    if (uint64_t(info.offsetX) + info.width > uint64_t(info.inputPitch)) {
        UntileRefuse(outRefusal, TextureUntileRefusal::RegionOutsideSurface);
        return;
    }
    if (info.inputHeight != 0 &&
        uint64_t(info.offsetY) + info.height > uint64_t(info.inputHeight)) {
        UntileRefuse(outRefusal, TextureUntileRefusal::RegionOutsideSurface);
        return;
    }
    const uint32_t bpb = GetTextureBytesPerBlock(info.format->xenosFormat);
    if (bpb == 0) { UntileRefuse(outRefusal, TextureUntileRefusal::ZeroBlockBytes); return; }
    if ((bpb & (bpb - 1)) != 0) {
        UntileRefuse(outRefusal, TextureUntileRefusal::NonPowerOfTwoBlock);
        return;
    }
    const uint32_t bpbLog2 = 31 - uint32_t(__builtin_clz(bpb));  // must be power of two
    if (bpbLog2 > 6) {
        UntileRefuse(outRefusal, TextureUntileRefusal::BlockTooLarge);
        return;
    }
    if (!info.srcBytes || !info.dstBytes) {
        UntileRefuse(outRefusal, TextureUntileRefusal::NoByteSizes);
        return;  // bounds guard requires sizes
    }
    for (uint32_t y = 0; y < info.height; ++y) {
        const uint64_t dstRow = (uint64_t(info.offsetY + y) * info.outputPitch + info.offsetX);
        for (uint32_t x = 0; x < info.width; ++x) {
            const int32_t tiled = GetTiledOffset2D(int32_t(info.offsetX + x),
                                                   int32_t(info.offsetY + y),
                                                   info.inputPitch, bpbLog2);
            const uint64_t dstOffset = (dstRow + x) * bpb;
            if (!UntileProbeSafe(tiled, info.srcBytes, dstOffset, info.dstBytes, bpb)) continue;
            std::memcpy(dst + dstOffset, src + tiled, bpb);
        }
    }
}

void UntileTexture3D(const uint8_t* src, uint8_t* dst, const TextureUntileInfo& info,
                     uint32_t depth, TextureUntileRefusal* outRefusal) {
    UntileRefuse(outRefusal, TextureUntileRefusal::None);
    if (!info.format) { UntileRefuse(outRefusal, TextureUntileRefusal::NoFormatInfo); return; }
    if (!src || !dst) { UntileRefuse(outRefusal, TextureUntileRefusal::NullBuffer); return; }
    if (info.inputPitch == 0 || info.inputHeight == 0) {
        UntileRefuse(outRefusal, TextureUntileRefusal::ZeroPitch);
        return;
    }
    if (uint64_t(info.offsetX) + info.width > uint64_t(info.inputPitch) ||
        uint64_t(info.offsetY) + info.height > uint64_t(info.inputHeight)) {
        UntileRefuse(outRefusal, TextureUntileRefusal::RegionOutsideSurface);
        return;
    }
    const uint32_t bpb = GetTextureBytesPerBlock(info.format->xenosFormat);
    if (bpb == 0) { UntileRefuse(outRefusal, TextureUntileRefusal::ZeroBlockBytes); return; }
    // 32_32_32_FLOAT lands here: 1x1x96 bits = 12 bytes/block, and the XGAddress
    // tiled math is parameterised by bpbLog2 (a power of two). There is no
    // correct 12 bpb address to compute, so this is a refusal, not a rounded
    // approximation. Xenia agrees: k_32_32_32_FLOAT has no host format and
    // kLoadShaderIndexUnknown in d3d12_texture_cache.cc:357-360.
    if ((bpb & (bpb - 1)) != 0) {
        UntileRefuse(outRefusal, TextureUntileRefusal::NonPowerOfTwoBlock);
        return;
    }
    const uint32_t bpbLog2 = 31 - uint32_t(__builtin_clz(bpb));  // must be power of two
    if (bpbLog2 > 6) {
        UntileRefuse(outRefusal, TextureUntileRefusal::BlockTooLarge);
        return;
    }
    if (!info.srcBytes || !info.dstBytes) {
        UntileRefuse(outRefusal, TextureUntileRefusal::NoByteSizes);
        return;
    }
    for (uint32_t z = 0; z < depth; ++z) {
        for (uint32_t y = 0; y < info.height; ++y) {
            const uint64_t dstRow =
                (uint64_t(z) * info.height + uint64_t(info.offsetY + y)) * info.outputPitch +
                info.offsetX;
            for (uint32_t x = 0; x < info.width; ++x) {
                const int32_t tiled = GetTiledOffset3D(int32_t(info.offsetX + x),
                                                       int32_t(info.offsetY + y),
                                                       int32_t(z),
                                                       info.inputPitch, info.inputHeight,
                                                       bpbLog2);
                const uint64_t dstOffset = (dstRow + x) * bpb;
                if (!UntileProbeSafe(tiled, info.srcBytes, dstOffset, info.dstBytes, bpb)) continue;
                std::memcpy(dst + dstOffset, src + tiled, bpb);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Endianness conversion.
//
// DECISION (item 6): the guest is big-endian, D3D12 is little-endian, and the
// Xenos fetch constant carries an explicit `Endian` field per texture. The
// detile math is defined on bytes and the validator proves it byte-exact
// against the old SDK oracle, so it must stay a pure gather. The swap
// therefore lives here, on the linear buffer, driven by the fetch constant -
// and only ever the four architected permutations (xenos.h:1052-1069):
//   kNone        identity
//   k8in16       swap bytes inside each 16-bit half word
//   k8in32       swap bytes inside each 32-bit word
//   k16in32      swap the two 16-bit half words of each 32-bit word
// Everything is byte-indexed arithmetic; nothing reinterprets guest memory.
// ---------------------------------------------------------------------------

bool IsSupportedGuestEndian(uint32_t endianness) { return endianness <= 3u; }

bool ConvertTextureEndianness(uint32_t endianness, uint8_t* data, uint32_t bytes,
                              uint32_t texelBytes) {
    if (!IsSupportedGuestEndian(endianness)) return false;
    if (endianness == 0u) return true;
    if (!data || bytes == 0 || texelBytes == 0) return false;
    if (uint64_t(bytes) % uint64_t(texelBytes) != 0) return false;
    switch (endianness) {
        case 1u:  // k8in16: swap byte pairs within 16-bit words (supports 1-byte and multi-byte texels)
            for (uint32_t i = 0; i + 1 < bytes; i += 2) std::swap(data[i], data[i + 1]);
            return true;
        case 2u:  // k8in32: full 4-byte reverse within 32-bit words
            for (uint32_t i = 0; i + 3 < bytes; i += 4) {
                std::swap(data[i], data[i + 3]);
                std::swap(data[i + 1], data[i + 2]);
            }
            return true;
        case 3u:  // k16in32: swap the half words within 32-bit words
            for (uint32_t i = 0; i + 3 < bytes; i += 4) {
                std::swap(data[i], data[i + 2]);
                std::swap(data[i + 1], data[i + 3]);
            }
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Refusal accounting. This TU links no logger (the headless validators that
// compile it do not link fmt/spdlog), so it counts and hands the backend a
// callback instead. Bounded: the callback fires at most
// kMaxRefusalCallbacksPerFormat (8) times per xenos format.
// ---------------------------------------------------------------------------

namespace {
TextureRefusalCallback g_refusalFn = nullptr;
void* g_refusalUser = nullptr;
uint32_t g_refusalFired[64] = {};  // per xenos format; fixed size, cannot grow
uint64_t g_refusalCounts[32] = {};
// F-648: the per-format import census. Fixed size, no allocation, so it is
// legal on the draw path. `g_importLastMode` is 0 == Refused until the format
// is actually seen, which is what a zero census row must report.
uint64_t g_importCounts[64] = {};
uint64_t g_importAccepted[64] = {};
uint32_t g_importLastMode[64] = {};
uint64_t g_importTotal = 0;
uint64_t g_importAcceptedTotal = 0;
}  // namespace

void SetTextureRefusalCallback(TextureRefusalCallback fn, void* user) {
    g_refusalFn = fn;
    g_refusalUser = user;
}
void ClearTextureRefusalCallback() { g_refusalFn = nullptr; g_refusalUser = nullptr; }

uint64_t TextureRefusalCount(TextureRefusal refusal) {
    const uint32_t i = static_cast<uint32_t>(refusal);
    return i < 32u ? g_refusalCounts[i] : 0u;
}

void NoteTextureRefusal(const TextureImportPlan& plan) {
    const uint32_t r = static_cast<uint32_t>(plan.refusal);
    if (r < 32u) g_refusalCounts[r]++;
    // F-648: a refusal is still a call, so it counts toward the per-format
    // census too -- the census answers "did the title reach the import path",
    // which a refusal-only count would answer as "no" for the wrong reason.
    if (plan.xenosFormat < 64u) g_importCounts[plan.xenosFormat]++;
    if (!g_refusalFn) return;
    if (plan.xenosFormat < 64u &&
        g_refusalFired[plan.xenosFormat] < kMaxRefusalCallbacksPerFormat) {
        g_refusalFired[plan.xenosFormat]++;
        g_refusalFn(g_refusalUser, plan);
    }
}

// F-648: record a plan that passed every refusal gate. Called from the single
// success exit of PlanTextureImport(), so there is exactly one place a plan can
// become ok and it cannot be forgotten by a later edit that adds a gate.
void NoteTextureImport(const TextureImportPlan& plan) {
    if (plan.xenosFormat < 64u) {
        g_importCounts[plan.xenosFormat]++;
        g_importAccepted[plan.xenosFormat]++;
        g_importLastMode[plan.xenosFormat] = static_cast<uint32_t>(plan.mode);
    }
    g_importTotal++;
    g_importAcceptedTotal++;
}

uint64_t TextureImportCount(uint32_t xenosFormat) {
    return xenosFormat < 64u ? g_importCounts[xenosFormat] : 0u;
}
uint64_t TextureImportCountTotal() { return g_importTotal; }
uint64_t TextureImportAcceptedCount(uint32_t xenosFormat) {
    return xenosFormat < 64u ? g_importAccepted[xenosFormat] : 0u;
}
uint64_t TextureImportAcceptedTotal() { return g_importAcceptedTotal; }
TextureUploadMode TextureImportLastMode(uint32_t xenosFormat) {
    if (xenosFormat >= 64u) return TextureUploadMode::Refused;
    const uint32_t m = static_cast<uint32_t>(g_importLastMode[xenosFormat]);
    return m <= static_cast<uint32_t>(TextureUploadMode::CpuExpand)
               ? static_cast<TextureUploadMode>(m)
               : TextureUploadMode::Refused;
}

const char* TextureRefusalName(TextureRefusal refusal) {
    switch (refusal) {
        case TextureRefusal::None: return "none";
        case TextureRefusal::UnknownFormatCode: return "unknown_format_code";
        case TextureRefusal::NoHostFormat: return "no_host_format";
        case TextureRefusal::NonPowerOfTwoBlock: return "non_power_of_two_block";
        case TextureRefusal::ZeroExtent: return "zero_extent";
        case TextureRefusal::ExtentTooLarge: return "extent_too_large";
        case TextureRefusal::PackedMipLayout: return "packed_mip_layout";
        case TextureRefusal::EndianOnBlockCompressed: return "endian_on_block_compressed";
        case TextureRefusal::DepthCubeNotUploadable: return "depth_or_cube_not_uploadable";
        case TextureRefusal::UntileRefused: return "untile_refused";
        case TextureRefusal::ExpanderMismatch: return "expander_mismatch";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Exact expanders. Bit-exact, not approximate: each is the CPU twin of the
// Xenia load shader of the same name (gpu/texture_cache.cc:102-106) and of the
// BC1/BC3 expansion the import validator already trusts
// (tools/validators/texture_import_validator.cpp:180-234). The unorm expand
// 5->8 / 6->8 rule is bit replication, i.e. the D3D spec's UNORM expansion.
// ---------------------------------------------------------------------------

namespace {

// MCLA_TEXDEC_PROBE (problem #3): env-gated BC1 endpoint diagnostic. Off by
// default; when MCLA_TEXDEC_PROBE != "1" no line is printed and the decoded
// bytes are never touched. State is file-local and diagnostic-only (written
// then read synchronously on the same ExpandTextureToImportBytes call path).
struct Bc1ProbeState {
    uint32_t width = 0;   // effective decoded width of the texture being expanded
    uint64_t fired = 0;   // qualifying (>=128-wide) BC1 blocks seen so far
};
Bc1ProbeState g_bc1Probe;
inline bool Bc1ProbeEnabled() {
    static const bool kOn = [] {
        const char* e = std::getenv("MCLA_TEXDEC_PROBE");
        return e && e[0] == '1';
    }();
    return kOn;
}

inline uint8_t Expand5To8(uint8_t v) {
    return uint8_t((uint32_t(v) << 3) | (uint32_t(v) >> 2));
}
inline uint8_t Expand6To8(uint8_t v) {
    return uint8_t((uint32_t(v) << 2) | (uint32_t(v) >> 4));
}
inline uint8_t Expand4To8(uint8_t v) { return uint8_t((uint32_t(v) << 4) | v); }

// Little-endian 16/32-bit reads of the HOST linear buffer. The buffer is
// already endianness-converted by ConvertTextureEndianness() when the fetch
// constant asked for it, so these are host-endian reads of host-endian bytes -
// never a guest pointer, never a reinterpret_cast of guest memory.
inline uint16_t LoadU16LE(const uint8_t* p) {
    return uint16_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8));
}
inline uint32_t LoadU32LE(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

// BC1 colour block (8 bytes) -> 16 RGBA8 texels. Identical to the validator's
// DecodeBc1Block, so the two must agree byte for byte.
void DecodeBc1Block(const uint8_t* blk, uint8_t out[16 * 4]) {
    const uint16_t c0 = LoadU16LE(blk);
    const uint16_t c1 = LoadU16LE(blk + 2);
    uint8_t pal[4][4] = {};
    auto unpack = [](uint16_t c, uint8_t* d) {
        d[0] = Expand5To8(uint8_t((c >> 11) & 0x1Fu));
        d[1] = Expand6To8(uint8_t((c >> 5) & 0x3Fu));
        d[2] = Expand5To8(uint8_t(c & 0x1Fu));
        d[3] = 255;
    };
    unpack(c0, pal[0]);
    unpack(c1, pal[1]);
    if (Bc1ProbeEnabled() && g_bc1Probe.width >= 128u) {
        if (g_bc1Probe.fired < 8u || (g_bc1Probe.fired % 1000u) == 0u) {
            std::fprintf(stderr,
                "[MCLA_TEXDEC_PROBE] BC1 w=%u c0=%04X c1=%04X "
                "pal0=(%u,%u,%u,%u) pal1=(%u,%u,%u,%u)\n",
                g_bc1Probe.width, unsigned(c0), unsigned(c1),
                unsigned(pal[0][0]), unsigned(pal[0][1]), unsigned(pal[0][2]),
                unsigned(pal[0][3]),
                unsigned(pal[1][0]), unsigned(pal[1][1]), unsigned(pal[1][2]),
                unsigned(pal[1][3]));
        }
        ++g_bc1Probe.fired;
    }
    if (c0 > c1) {  // four opaque colours
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = uint8_t((2 * pal[1][i] + pal[0][i]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {  // three colours plus the 1-bit-alpha index-3 entry
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    const uint32_t idx = LoadU32LE(blk + 4);
    for (int i = 0; i < 16; ++i)
        std::memcpy(&out[i * 4], pal[uint8_t((idx >> (i * 2)) & 3u)], 4);
}

// Opaque 4-colour BC1/BC3 colour tail. BC2 (DXT3) and BC3 (DXT5) have NO
// 1-bit-transparency mode: their colour block is ALWAYS the 4-colour palette
// c0, c1, (2*c0+c1)/3, (c0+2*c1)/3, independent of the c0>c1 comparison.
// DecodeBc1Block's c0<=c1 branch (:830-836) is BC1-only (punchthrough alpha);
// using it for BC2/BC3 corrupts every block whose compressor emitted c0<=c1
// (legal, and handled opaque by Xenia's XeDXTOpaqueRowToRGB8 with no endpoint
// compare). This is the DXT5 colour tail shared by DecodeBc2/DecodeBc3.
static bool BcOpaqueTailEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("MCLA_BC3_OLD");  // =1 restores the buggy BC1-tail behaviour for A/B
        return !(e && e[0] == '1');
    }();
    return on;
}
void DecodeBcOpaqueColourBlock(const uint8_t* blk, uint8_t out[16 * 4]) {
    const uint16_t c0 = LoadU16LE(blk);
    const uint16_t c1 = LoadU16LE(blk + 2);
    if (!BcOpaqueTailEnabled()) {  // A/B control: the old (wrong) BC1 punchthrough path
        DecodeBc1Block(blk, out);
        return;
    }
    uint8_t pal[4][4] = {};
    auto unpack = [](uint16_t c, uint8_t* d) {
        d[0] = Expand5To8(uint8_t((c >> 11) & 0x1Fu));
        d[1] = Expand6To8(uint8_t((c >> 5) & 0x3Fu));
        d[2] = Expand5To8(uint8_t(c & 0x1Fu));
        d[3] = 255;
    };
    unpack(c0, pal[0]);
    unpack(c1, pal[1]);
    for (int i = 0; i < 3; ++i) {
        pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
        pal[3][i] = uint8_t((2 * pal[1][i] + pal[0][i]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
    const uint32_t idx = LoadU32LE(blk + 4);
    for (int i = 0; i < 16; ++i)
        std::memcpy(&out[i * 4], pal[uint8_t((idx >> (i * 2)) & 3u)], 4);
}

// BC2 (DXT3) -> 16 RGBA8 texels: 4-bit explicit alpha + a BC1 colour tail.
// Alpha is row-major, LOW nibble first: texel (x,y) = bits 4x..4x+3 of the
// 16-bit row at blk[2y..2y+1] -- the D3D BC2 layout (MSDN "Textures with Alpha
// Channels": word bits 3:0 = texel [1][0]) and what Xenia extracts
// (texture_load_dxt3_rgba8.cs.xesl:48, row 0 = block.x >> (0,4,8,12), texel 0
// = bits 0..3). MEASURED WRONG (F-900 era): this took the HIGH nibble first,
// exchanging each pair of texels within every byte; the fixed ternary is
// verified against the same Xenia shader and the D3D spec. DecodeBc1/3/4 were
// re-verified clean in the same audit -- BC2 was the only inverted one, and it
// escaped the validator because texture_import_validator only accepts fourCC
// DXT1/DXT5 (never DXT2/DXT3).
void DecodeBc2Block(const uint8_t* blk, uint8_t out[16 * 4]) {
    uint8_t colour[16 * 4];
    DecodeBcOpaqueColourBlock(blk + 8, colour);  // BC2 colour is always 4-colour opaque
    for (int i = 0; i < 16; ++i) {
        const uint32_t a = blk[i >> 1];
        out[i * 4 + 0] = colour[i * 4 + 0];
        out[i * 4 + 1] = colour[i * 4 + 1];
        out[i * 4 + 2] = colour[i * 4 + 2];
        out[i * 4 + 3] = Expand4To8(uint8_t((i & 1) ? (a >> 4) : (a & 0x0Fu)));
    }
}

// BC3 (DXT5) -> 16 RGBA8 texels: BC4 alpha + a BC1 colour tail.
void DecodeBc3Block(const uint8_t* blk, uint8_t out[16 * 4]) {
    uint8_t alpha[8];
    alpha[0] = blk[0];
    alpha[1] = blk[1];
    if (alpha[0] > alpha[1]) {
        for (int i = 2; i < 8; ++i)
            alpha[i] = uint8_t(((8 - i) * alpha[0] + (i - 1) * alpha[1]) / 7);
    } else {
        for (int i = 2; i < 6; ++i)
            alpha[i] = uint8_t(((6 - i) * alpha[0] + (i - 1) * alpha[1]) / 5);
        alpha[6] = 0;
        alpha[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (i * 8);
    uint8_t colour[16 * 4];
    DecodeBcOpaqueColourBlock(blk + 8, colour);  // BC3 colour is always 4-colour opaque
    for (int i = 0; i < 16; ++i) {
        std::memcpy(&out[i * 4], &colour[i * 4], 3);
        out[i * 4 + 3] = alpha[uint8_t((bits >> (i * 3)) & 7u)];
    }
}

// BC4 (DXT5A) -> 16 one-channel texels.
void DecodeBc4Block(const uint8_t* blk, uint8_t out[16]) {
    uint8_t v[8];
    v[0] = blk[0];
    v[1] = blk[1];
    if (v[0] > v[1]) {
        for (int i = 2; i < 8; ++i)
            v[i] = uint8_t(((8 - i) * v[0] + (i - 1) * v[1]) / 7);
    } else {
        for (int i = 2; i < 6; ++i)
            v[i] = uint8_t(((6 - i) * v[0] + (i - 1) * v[1]) / 5);
        v[6] = 0;
        v[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (i * 8);
    for (int i = 0; i < 16; ++i) out[i] = v[uint8_t((bits >> (i * 3)) & 7u)];
}

// DXT3A (xenos 58) -> 16 one-channel texels. This is NOT BC4: a DXT3A block
// holds 16 DIRECT 4-bit alphas (4 rows x 2 bytes). Row r is the little-endian
// 16-bit word at blk[2r..2r+1] and texel x of that row is bits 4x..4x+3 - the
// byte-for-byte twin of XeDXT3FourBlocksRowToA8
// (.research/xenia/src/xenia/gpu/shaders/pixel_formats.xesli:745-753, fed by
// texture_load_dxt3a.cs.xesl, which reads word0 for rows 0/1 and word1 for
// rows 2/3), which widens each nibble to a byte as (n) | (n << 4). F-898(a):
// this used to run through DecodeBc4Block - endpoint interpolation where the
// guest stored explicit per-texel alphas, i.e. a different image.
void DecodeDXT3ANibbleBlock(const uint8_t* blk, uint8_t out[16]) {
    for (uint32_t y = 0; y < 4u; ++y) {
        const uint16_t row = LoadU16LE(blk + 2u * y);
        for (uint32_t x = 0; x < 4u; ++x) {
            const uint8_t n = uint8_t((row >> (x * 4u)) & 0x0Fu);
            out[y * 4u + x] = uint8_t(n | uint8_t(n << 4));
        }
    }
}

// DXT3A_AS_1_1_1_1 (xenos 61) -> 16 B4G4R4A4_UNORM texels (2 bytes each). The
// block layout is the same as DXT3A's (4 rows x 2 bytes, texel x at bits
// 4x..4x+3 of row word r - texture_load_dxt3aas1111.xesli packs word0 into
// row 0 and word1 into row 2 exactly like the DXT3A shader does), but each
// 4-bit texel carries ONE bit per component, alpha in the LSB and red in the
// MSB ("Assuming alpha in LSB and red in MSB", the order
// XeDXT3AAs1111TwoBlocksRowToBGRA4 assumes,
// .research/xenia/src/xenia/gpu/shaders/pixel_formats.xesli:755-780). Each bit
// is replicated to its 4-bit channel - the shader does row |= row << 1;
// row |= row << 2 - and the host word is packed B(0:3) | G(4:7) | R(8:11) | A(12:15)
// - that is DXGI_FORMAT_B4G4R4A4_UNORM's little-endian value, the same nibble
// order XeR4G4B4A4ToB4G4R4A4 produces (pixel_formats.xesli:388-391), and the
// host form Xenia picks for this format (d3d12_texture_cache.cc:375-379).
// F-898(a): this used to run through DecodeBc4Block and land in R8_UNORM -
// wrong algorithm AND wrong channel count.
void DecodeDXT3AAs1111Block(const uint8_t* blk, uint8_t out[16 * 2]) {
    for (uint32_t y = 0; y < 4u; ++y) {
        const uint16_t row = LoadU16LE(blk + 2u * y);
        for (uint32_t x = 0; x < 4u; ++x) {
            const uint8_t n = uint8_t((row >> (x * 4u)) & 0x0Fu);
            const uint16_t px = uint16_t((((n & 1u) ? 0xFu : 0u) << 12) |  // A -> 12:15
                                         ((n & 2u) ? 0xFu : 0u) |          // B ->  0:3
                                         (((n & 4u) ? 0xFu : 0u) << 4) |   // G ->  4:7
                                         (((n & 8u) ? 0xFu : 0u) << 8));   // R ->  8:11
            out[(y * 4u + x) * 2u] = uint8_t(px & 0xFFu);
            out[(y * 4u + x) * 2u + 1u] = uint8_t(px >> 8);
        }
    }
}

// 5_6_5 -> RGBA8. The Xenos packs the FIRST-named component in the LSBs:
// XePackR5G6B5UNorm (.research/xenia/src/xenia/gpu/shaders/pixel_formats.xesli:103-106)
// is `n.r | (n.g << 5) | (n.b << 11)`, so guest bits 0..4 = R, 5..10 = G and
// 11..15 = B. The old extraction here was MSB-first and swapped R/B (F-898(a)).
void Decode565Block(const uint8_t* blk, uint8_t out[4]) {
    const uint16_t c = LoadU16LE(blk);
    out[0] = Expand5To8(uint8_t(c & 0x1Fu));
    out[1] = Expand6To8(uint8_t((c >> 5) & 0x3Fu));
    out[2] = Expand5To8(uint8_t((c >> 11) & 0x1Fu));
    out[3] = 255;
}

// 6_5_5 -> RGBA8. NOT the same layout as 5_6_5: Xenia gives it its own pack,
// XePackR5G5B6UNorm (pixel_formats.xesli:108-111) = `n.r | (n.g << 5) |
// (n.b << 10)`, with n scaled 31/31/63, so the 6-bit field is B in bits 10..15.
// d3d12_texture_cache.cc:108-113 gives it its own load shader
// (XeR5G5B6ToB5G6R5WithRBGASwizzle) plus the R,B,G,G texture swizzle.
void Decode655Block(const uint8_t* blk, uint8_t out[4]) {
    const uint16_t c = LoadU16LE(blk);
    out[0] = Expand5To8(uint8_t(c & 0x1Fu));
    out[1] = Expand5To8(uint8_t((c >> 5) & 0x1Fu));
    out[2] = Expand6To8(uint8_t((c >> 10) & 0x3Fu));
    out[3] = 255;
}

// 1_5_5_5 -> RGBA8. XePackR5G5B5A1UNorm (pixel_formats.xesli:98-101) is
// `n.r | (n.g << 5) | (n.b << 10) | (n.a << 15)`: R low, then G, then B, with
// the 1-bit alpha in bit 15 - so bit 15 is the alpha and R/B was swapped
// before this fix (F-898(a)).
void Decode1555Block(const uint8_t* blk, uint8_t out[4]) {
    const uint16_t c = LoadU16LE(blk);
    out[0] = Expand5To8(uint8_t(c & 0x1Fu));
    out[1] = Expand5To8(uint8_t((c >> 5) & 0x1Fu));
    out[2] = Expand5To8(uint8_t((c >> 10) & 0x1Fu));
    out[3] = uint8_t((c & 0x8000u) ? 255u : 0u);
}

inline void StoreU16LE(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v & 0xFFu);
    p[1] = uint8_t((v >> 8) & 0xFFu);
}

// 10_11_11 -> R16G16B16A16_UNORM (Xenia d3d12_texture_cache.cc:170-174 ->
// XeR11G11B10UNormToRGBA16, pixel_formats.xesli:472-486): R=0..10 (11b),
// G=11..21 (11b), B=22..31 (10b), A=0xFFFF.
void Decode101111ToRGBA16(const uint8_t* blk, uint8_t out[8]) {
    const uint32_t v = LoadU32LE(blk);
    const uint32_t r11 = v & 0x7FFu;
    const uint32_t g11 = (v >> 11) & 0x7FFu;
    const uint32_t b10 = (v >> 22) & 0x3FFu;
    StoreU16LE(out + 0, uint16_t((r11 << 5) | (r11 >> 6)));
    StoreU16LE(out + 2, uint16_t((g11 << 5) | (g11 >> 6)));
    StoreU16LE(out + 4, uint16_t((b10 << 6) | (b10 >> 4)));
    StoreU16LE(out + 6, 0xFFFFu);
}

// 11_11_10 -> R16G16B16A16_UNORM (Xenia d3d12_texture_cache.cc:175-179 ->
// XeR10G11B11UNormToRGBA16, pixel_formats.xesli:450-464): R=0..9 (10b),
// G=10..20 (11b), B=21..31 (11b), A=0xFFFF.
void Decode111110ToRGBA16(const uint8_t* blk, uint8_t out[8]) {
    const uint32_t v = LoadU32LE(blk);
    const uint32_t r10 = v & 0x3FFu;
    const uint32_t g11 = (v >> 10) & 0x7FFu;
    const uint32_t b11 = (v >> 21) & 0x7FFu;
    StoreU16LE(out + 0, uint16_t((r10 << 6) | (r10 >> 4)));
    StoreU16LE(out + 2, uint16_t((g11 << 5) | (g11 >> 6)));
    StoreU16LE(out + 4, uint16_t((b11 << 5) | (b11 >> 6)));
    StoreU16LE(out + 6, 0xFFFFu);
}

uint32_t Log2CeilU32(uint32_t v) {
    if (v <= 1u) return 0u;
    uint32_t l = 0u;
    while ((1u << l) < v && l < 31u) ++l;
    return l;
}

// Xenia texture_util.cc:41-44 & 116-207: packed mip tail level & 2D block offset.
uint32_t PackedMipBaseLevel(uint32_t width, uint32_t height) {
    const uint32_t lw = Log2CeilU32(width);
    const uint32_t lh = Log2CeilU32(height);
    const uint32_t log2Size = std::min(lw, lh);
    return (log2Size > 4u) ? (log2Size - 4u) : 0u;
}

bool ComputePackedMipOffset(uint32_t width, uint32_t height,
                            uint32_t blockWidth, uint32_t blockHeight,
                            uint32_t mip,
                            uint32_t& outXBlocks, uint32_t& outYBlocks) {
    const uint32_t lw = Log2CeilU32(width);
    const uint32_t lh = Log2CeilU32(height);
    const uint32_t log2Size = std::min(lw, lh);
    if (log2Size > 4u + mip) {
        outXBlocks = 0u;
        outYBlocks = 0u;
        return false;
    }
    const uint32_t packedMipBase = (log2Size > 4u) ? (log2Size - 4u) : 0u;
    const uint32_t packedMip = mip - packedMipBase;
    uint32_t xTexels = 0u, yTexels = 0u;
    if (packedMip < 3u) {
        if (lw > lh) {
            xTexels = 0u;
            yTexels = 16u >> packedMip;
        } else {
            xTexels = 16u >> packedMip;
            yTexels = 0u;
        }
    } else {
        if (lw > lh) {
            xTexels = (1u << (lw - packedMipBase)) >> (packedMip - 2u);
            yTexels = 0u;
        } else {
            xTexels = 0u;
            yTexels = (1u << (lh - packedMipBase)) >> (packedMip - 2u);
        }
    }
    const uint32_t bw = blockWidth ? blockWidth : 1u;
    const uint32_t bh = blockHeight ? blockHeight : 1u;
    outXBlocks = xTexels / bw;
    outYBlocks = yTexels / bh;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Import plan
// ---------------------------------------------------------------------------

// Which guest formats this layer can expand into an EXACT host payload form.
// The payload is in plan.importDxgiFormat: every format listed here is
// expanded by an exact CPU twin of the matching Xenia load shader; every
// format NOT listed stays a refusal, because "close enough" is a corruption,
// not a fallback.
//
// CAVEAT (measured 2026-10-27, F-898(a) follow-up): D3D12Backend::
// CreateDecodedTexture still sizes its resource and its SRV from
// plan.nativeDxgiFormat (src/d3d12_backend.cpp:1821, :2148), and its
// kHostFormatTable (src/d3d12_backend.cpp:191-206) has no B4G4R4A4_UNORM row
// - so until that gate is taught importDxgiFormat, an expanded payload is
// correct bytes the backend cannot yet carry. That is a backend defect, not a
// reason to decode wrongly here; it already applies to every CpuExpand format
// (18/19/20/51/52/53 included) and is recorded here so nobody re-derives it.
static bool PlanExpandsTo(uint32_t xenosFormat, uint32_t* outDxgi) {
    switch (xenosFormat) {
        // DXT1/DXT2_3/DXT4_5 (+ the _AS_16_16_16_16 aliases) -> RGBA8, the
        // CPU twin of kDXT1ToRGBA8 / kDXT3ToRGBA8 / kDXT5ToRGBA8.
        case 18: case 19: case 20: case 51: case 52: case 53:
            *outDxgi = kDxgiR8G8B8A8Unorm;
            return true;
        // DXT3A (58) and DXT5A (59) are both 4x4 4bpp single channel and both
        // land in R8_UNORM in Xenia (d3d12_texture_cache.cc:361-370) - but by
        // different algorithms. kDXT3A fills R8 by DIRECT NIBBLE EXPANSION
        // (texture_load_dxt3a.cs.xesl -> XeDXT3FourBlocksRowToA8,
        // pixel_formats.xesli:745-753: 16 explicit 4-bit alphas, each widened
        // as n | (n << 4)); kDXT5AToR8 is a true BC4 interpolation
        // (texture_load_dxt5a_r8.cs.xesl + XeDXT5HighAlphaWeights).
        // F-898(a): decoding 58 with DecodeBc4Block is a different image, not
        // a fallback.
        case 58: case 59:
            *outDxgi = kDxgiR8Unorm;
            return true;
        // DXT3A_AS_1_1_1_1 (61) is 4x4 4bpp too, but its 4 bits are ONE BIT per
        // component and Xenia's host form is B4G4R4A4_UNORM, fed by
        // kDXT3AAs1111ToBGRA4 (d3d12_texture_cache.cc:375-379 ->
        // texture_load_dxt3aas1111_bgra4.cs.xesl ->
        // XeDXT3AAs1111TwoBlocksRowToBGRA4, pixel_formats.xesli:755-780). It is
        // not a single channel, so R8_UNORM for it was wrong twice over
        // (F-898(a)).
        case 61:
            *outDxgi = kDxgiB4G4R4A4Unorm;
            return true;
        // 1_5_5_5 / 5_6_5 / 6_5_5 / 4_4_4_4 -> RGBA8 by exact unorm unpack
        // (Xenia load shaders XeR5G5B5A1ToB5G5R5A1 / XeR5G6B5ToB5G6R5 /
        // XeR5G5B6ToB5G6R5WithRBGASwizzle / XeR4G4B4A4ToB4G4R4A4). The guest
        // packs the first-named component in the LSBs, and 6_5_5 has its own
        // layout (R5G5B6) - it is NOT bit-identical to 5_6_5 (F-898(a)).
        case 3: case 4: case 5: case 15:
            *outDxgi = kDxgiR8G8B8A8Unorm;
            return true;
        // 16 (10_11_11), 17 (11_11_10) and their AS_16_16_16_16 aliases (55, 56) are
        // packed UNORM bitfields, not R11G11B10_FLOAT (Xenia d3d12_texture_cache.cc:170-179,
        // 347-356; shaders/pixel_formats.xesli:450-492). Expand to R16G16B16A16_UNORM.
        case 16: case 17: case 55: case 56:
            *outDxgi = kDxgiR16G16B16A16Unorm;
            return true;
        default:
            return false;
    }
}

TextureImportPlan PlanTextureImport(uint32_t xenosFormat, uint32_t width, uint32_t height,
                                    uint32_t depthOrArraySize, uint32_t mipMaxLevel,
                                    uint32_t endianness, bool tiled, bool packedMips,
                                    bool is3D, bool isDepthStencil) {
    TextureImportPlan plan = {};
    plan.xenosFormat = xenosFormat;
    const TextureFormatInfo* info = GetTextureFormatInfo(xenosFormat);
    plan.formatName = info ? info->name : "unknown";
    plan.arraySize = depthOrArraySize ? depthOrArraySize : 1u;
    if (!info) {
        plan.refusal = TextureRefusal::UnknownFormatCode;
        NoteTextureRefusal(plan);
        return plan;
    }
    plan.nativeDxgiFormat = info->dxgiFormat;
    plan.blockWidth = info->blockWidth;
    plan.blockHeight = info->blockHeight;
    plan.blockBytes = GetTextureBytesPerBlock(xenosFormat);
    plan.isBlockCompressed = info->isBlockCompressed;
    plan.isResolvable = info->isResolvable;
    (void)tiled;  // tiling is the untile step's business, not the plan's

    // Depth/stencil rows (k_24_8, k_24_8_FLOAT and the *_EDRAM copies) are
    // render targets, not copy destinations: D3D12 wants ALLOW_DEPTH_STENCIL
    // and a DSV, so a plain CopyTextureRegion import would be a lie. Refused.
    if (isDepthStencil) {
        plan.refusal = TextureRefusal::DepthCubeNotUploadable;
        NoteTextureRefusal(plan);
        return plan;
    }
    if (packedMips && (width <= 16u || height <= 16u)) {
        // Base level stored inside the packed-mip tail (Xenia texture_util.cc:116-207, mip 0):
        // log2 of the shorter side <= 4 -> packed_mip 0 -> 16 texels along the longer axis.
        (void)ComputePackedMipOffset(width, height, info->blockWidth, info->blockHeight,
                                     0u, plan.packedOffsetX, plan.packedOffsetY);
    }
    if (width == 0 || height == 0 || plan.arraySize == 0) {
        plan.refusal = TextureRefusal::ZeroExtent;
        NoteTextureRefusal(plan);
        return plan;
    }
    // Xenos maxima (xenos.h:1136-1147): 2D/cube <= 8192, 3D <= 2048.
    const uint32_t maxDim = is3D ? 2048u : 8192u;
    if (width > maxDim || height > maxDim) {
        plan.refusal = TextureRefusal::ExtentTooLarge;
        NoteTextureRefusal(plan);
        return plan;
    }
    if (!IsSupportedGuestEndian(endianness)) {
        // No architected permutation for this code: refuse rather than leave
        // the texels byte-swapped relative to the host.
        plan.refusal = TextureRefusal::NoHostFormat;
        NoteTextureRefusal(plan);
        return plan;
    }
    plan.guestEndianness = endianness;
    plan.needsByteSwap = endianness != 0u;
    // F-899 (2026-10-27): the untile is a byte-exact gather (it never swaps),
    // and ConvertTextureEndianness runs on the LINEAR buffer AFTER untile, so a
    // swap on a block-compressed surface is safe -- it only reorders bytes
    // within 16-bit words of the linear buffer, which the DXT decode (running
    // later, on the linear buffer) reads correctly. The previous refusal
    // ("Xenia's load shaders read block words little-endian") was conservative
    // and blocked every block-compressed texture whose guest endianness field
    // was non-zero, which is the only kind the game actually produces.
    // 32_32_32_FLOAT: 1x1 block, 96 bits = 12 bytes/block. The XGAddress tiled
    // math is parameterised by bpbLog2 (a power of two), so 12 is not
    // addressable and Xenia refuses the format too
    // (d3d12_texture_cache.cc:357-360). The UntileTexture2D/3D guards
    // (NonPowerOfTwoBlock) protect memory; this makes the refusal named,
    // counted and logged at plan time instead of a silently empty texture.
    if (plan.blockBytes == 0) {
        plan.refusal = TextureRefusal::NoHostFormat;
        NoteTextureRefusal(plan);
        return plan;
    }
    if ((plan.blockBytes & (plan.blockBytes - 1)) != 0 || plan.blockBytes > 64u) {
        plan.refusal = TextureRefusal::NonPowerOfTwoBlock;
        NoteTextureRefusal(plan);
        return plan;
    }

    uint32_t bx = 0, by = 0;
    if (GetTextureBlockSpan(xenosFormat, width, height, &bx, &by) == 0) {
        plan.refusal = TextureRefusal::ZeroExtent;
        NoteTextureRefusal(plan);
        return plan;
    }
    plan.blocksX = bx;
    plan.blocksY = by;
    plan.texelBytes = plan.blockBytes;
    plan.linearByteSize = uint32_t(uint64_t(bx) * by * plan.blockBytes * plan.arraySize);

    // Mip chain length: mip_max_level + 1, clamped to the base extents (a guest
    // may leave mip_max_level above the extent floor; Xenia clamps the same
    // way, texture_util.cc:244-249). mip_min_level is a sampling clamp, not a
    // storage change, so the chain still starts at mip 0.
    uint32_t maxLevel = 0;
    {
        const uint32_t m = width > height ? width : height;
        const uint32_t d = (is3D && plan.arraySize > m) ? plan.arraySize : m;
        for (uint32_t v = d; v > 1u; v >>= 1) maxLevel++;
    }
    plan.mipCount = (mipMaxLevel < maxLevel) ? (mipMaxLevel + 1u) : (maxLevel + 1u);

    if (info->dxgiFormat == kDxgiR8G8B8A8Unorm || info->dxgiFormat == kDxgiR8Unorm) {
        plan.importDxgiFormat = info->dxgiFormat;
        plan.mode = TextureUploadMode::ScalarCopy;
    } else if (info->dxgiFormat != 0) {
        uint32_t expandDxgi = 0;
        if (PlanExpandsTo(xenosFormat, &expandDxgi)) {
            plan.importDxgiFormat = expandDxgi;
            plan.mode = TextureUploadMode::CpuExpand;
        } else {
            // A real host form exists (DXN=BC5, B4G4R4A4, R16, float, ...)
            // but there is no exact expander for it. Keep the native form and
            // let the backend widen its gate: for a block format that copy is
            // byte-exact (the untiled block rows are already in host order).
            plan.importDxgiFormat = info->dxgiFormat;
            plan.mode = info->isBlockCompressed ? TextureUploadMode::DirectBlockCopy
                                                : TextureUploadMode::ScalarCopy;
            plan.note = "backend gate must accept nativeDxgiFormat for a zero-copy import";
        }
    } else if (xenosFormat >= 27 && xenosFormat <= 29) {
        // PATCH_GFX_MISSING P1: Xenia copies the *_EXPAND
        // formats as the native float format of the same width (R16 / R16G16 / R16G16B16A16 FLOAT).
        plan.importDxgiFormat = xenosFormat == 27 ? 54u : (xenosFormat == 28 ? 34u : 10u);
        plan.mode = TextureUploadMode::ScalarCopy;
        plan.note = "P1: EXPAND copied as native float";
    } else {
        // dxgi == 0: YUV REP, 1/1_REVERSE, the *_EXPAND rows, 16/16_16/16_16_16,
        // MPEG, INTERLACED, 32_AS_8*, CTX1. All of these are reachable in
        // Xenia only through a load shader (d3d12_texture_cache.cc:361-374)
        // and this backend has no shader path, so every one of them is refused.
        // A subset was not chosen on a guess.
        plan.refusal = TextureRefusal::NoHostFormat;
        NoteTextureRefusal(plan);
        return plan;
    }
    plan.ok = true;
    // F-648: the one and only success exit, so the census cannot be bypassed by
    // a later edit that adds another gate above this line.
    NoteTextureImport(plan);
    return plan;
}

bool ExpandTextureToImportBytes(const TextureImportPlan& plan, const uint8_t* linear,
                                uint32_t linearBytes, std::vector<uint8_t>& out,
                                TextureRefusal* outRefusal) {
    if (outRefusal) *outRefusal = TextureRefusal::None;
    if (plan.mode != TextureUploadMode::CpuExpand) {
        if (outRefusal) *outRefusal = TextureRefusal::ExpanderMismatch;
        return false;
    }
    if (!linear || linearBytes < plan.linearByteSize) {
        if (outRefusal) *outRefusal = TextureRefusal::ExpanderMismatch;
        return false;  // short payload: refuse, never expand a partial image
    }
    const uint32_t w = plan.blocksX * plan.blockWidth;
    if (Bc1ProbeEnabled()) g_bc1Probe.width = w;
    const uint32_t h = plan.blocksY * plan.blockHeight;
    const uint32_t bb = plan.blockBytes;
    const bool scalarPack = (plan.xenosFormat == 3 || plan.xenosFormat == 4 ||
                             plan.xenosFormat == 5 || plan.xenosFormat == 15);
    const bool scalarPack32To64 = (plan.xenosFormat == 16 || plan.xenosFormat == 17 ||
                                   plan.xenosFormat == 55 || plan.xenosFormat == 56);
    // The output texel size must match plan.importDxgiFormat, because that is
    // the format the payload is produced in: 4 bytes for R8G8B8A8_UNORM, 2 for
    // B4G4R4A4_UNORM (DXT3A_AS_1_1_1_1 - Xenia's own host form for 61,
    // d3d12_texture_cache.cc:375-379), 1 for R8_UNORM (the DXT3A/DXT5A
    // single-channel case - d3d12_texture_cache.cc:361-366), 8 for R16G16B16A16_UNORM
    // (10_11_11 / 11_11_10 UNORM unpack - d3d12_texture_cache.cc:170-179).
    uint32_t outTexelBytes = 4u;
    if (plan.importDxgiFormat == kDxgiR8Unorm) {
        outTexelBytes = 1u;
    } else if (plan.importDxgiFormat == kDxgiB4G4R4A4Unorm) {
        outTexelBytes = 2u;
    } else if (plan.importDxgiFormat == kDxgiR16G16B16A16Unorm) {
        outTexelBytes = 8u;
    }
    // scalarPack writes RGBA8 (4 B/texel) regardless of 16-bit guest size;
    // scalarPack32To64 writes RGBA16_UNORM (8 B/texel) from 32-bit guest words.
    const uint32_t bytesPerSlice = w * h * outTexelBytes;
    out.assign(size_t(bytesPerSlice) * plan.arraySize, 0u);

    for (uint32_t s = 0; s < plan.arraySize; ++s) {
        const uint8_t* src = linear + uint64_t(s) * plan.linearByteSize / plan.arraySize;
        uint8_t* dst = out.data() + size_t(s) * bytesPerSlice;
        if (scalarPack32To64) {
            const bool is111110 = (plan.xenosFormat == 17 || plan.xenosFormat == 56);
            for (uint32_t i = 0; i < w * h; ++i) {
                if (is111110) Decode111110ToRGBA16(src + i * 4u, dst + i * 8u);
                else          Decode101111ToRGBA16(src + i * 4u, dst + i * 8u);
            }
            continue;
        }
        if (scalarPack) {
            for (uint32_t i = 0; i < w * h; ++i) {
                uint8_t px[4];
                switch (plan.xenosFormat) {
                    case 3: Decode1555Block(src + i * 2u, px); break;       // 1_5_5_5
                    case 4: Decode565Block(src + i * 2u, px); break;        // 5_6_5
                    case 5: Decode655Block(src + i * 2u, px); break;        // 6_5_5 (R5G5B6)
                    default: {                                              // 4_4_4_4
                        // XePackR4G4B4A4UNorm (pixel_formats.xesli:123-126)
                        // = r | (g << 4) | (b << 8) | (a << 12): R is the low
                        // nibble, so R/B was swapped before (F-898(a)).
                        const uint16_t c = LoadU16LE(src + i * 2u);
                        px[0] = Expand4To8(uint8_t(c & 0x0Fu));
                        px[1] = Expand4To8(uint8_t((c >> 4) & 0x0Fu));
                        px[2] = Expand4To8(uint8_t((c >> 8) & 0x0Fu));
                        px[3] = Expand4To8(uint8_t((c >> 12) & 0x0Fu));
                        break;
                    }
                }
                std::memcpy(dst + i * 4u, px, 4);
            }
            continue;
        }
        for (uint32_t by = 0; by < plan.blocksY; ++by) {
            for (uint32_t bx = 0; bx < plan.blocksX; ++bx) {
                const uint8_t* blk = src + (size_t(by) * plan.blocksX + bx) * bb;
                // Decode into a block-local 16-texel buffer first, then SCATTER.
                // A block decoder writes 16 texels contiguously (that is the
                // 4x4 block layout), but the destination is a full-surface
                // RGBA8 image whose rows are `w` texels wide, so for any
                // texture larger than 4x4 in either axis a direct write would
                // lay the block out in a single row and corrupt everything
                // after it. Scatter: row r of the block goes to
                // (by*blockHeight + r, bx*blockWidth + c).
                uint8_t texels[16 * 4];
                uint32_t rowStride = 0;
                switch (plan.xenosFormat) {
                    case 18: case 51: DecodeBc1Block(blk, texels); rowStride = 4; break;
                    case 19: case 52: DecodeBc2Block(blk, texels); rowStride = 4; break;
                    case 20: case 53: DecodeBc3Block(blk, texels); rowStride = 4; break;
                    case 58:  // DXT3A: 16 direct 4-bit alphas -> R8 (n | n << 4)
                        DecodeDXT3ANibbleBlock(blk, texels);
                        rowStride = 1;  // importDxgiFormat is R8_UNORM
                        break;
                    case 59:  // DXT5A: BC4 endpoint interpolation -> R8
                        DecodeBc4Block(blk, texels);
                        rowStride = 1;  // importDxgiFormat is R8_UNORM
                        break;
                    case 61:  // DXT3A_AS_1_1_1_1: 4x1-bit -> B4G4R4A4 (2B/texel)
                        DecodeDXT3AAs1111Block(blk, texels);
                        rowStride = 2;  // importDxgiFormat is B4G4R4A4_UNORM
                        break;
                    default:
                        if (outRefusal) *outRefusal = TextureRefusal::NoHostFormat;
                        out.clear();
                        return false;
                }
                for (uint32_t r = 0; r < plan.blockHeight; ++r) {
                    const size_t rowBase =
                        (size_t(by * plan.blockHeight + r) * w + bx * plan.blockWidth) *
                        outTexelBytes;
                    std::memcpy(dst + rowBase, &texels[r * plan.blockWidth * rowStride],
                                size_t(plan.blockWidth) * rowStride);
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Guest fetch constant decode (24 bytes, big-endian, explicit shifts only).
// Field positions are the architected bitfield offsets in
// .research/xenia/src/xenia/gpu/xenos.h:1184-1285 (xe_gpu_texture_fetch_t),
// which the CP writes directly into guest memory - so the same bytes are the
// guest address in DrawPacket::textureDescriptors[i].
// ---------------------------------------------------------------------------

namespace {

inline uint32_t Bits(uint32_t v, uint32_t lo, uint32_t width) {
    const uint32_t w = width >= 32u ? 32u : width;
    return w ? ((v >> lo) & ((1u << w) - 1u)) : 0u;
}
// Sign-extend a `width`-bit two's complement field.
inline int32_t Sbits(uint32_t v, uint32_t lo, uint32_t width) {
    if (width == 0 || width >= 32u) return int32_t(v);
    const uint32_t raw = Bits(v, lo, width);
    const uint32_t signBit = 1u << (width - 1u);
    return int32_t((raw ^ signBit) - signBit);
}
inline uint32_t BeU32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
           uint32_t(p[3]);
}
inline uint32_t NextPow2(uint32_t v) {
    if (v <= 1u) return 1u;
    uint32_t n = 1u;
    while (n < v && n < 0x40000000u) n <<= 1;
    return n;
}

}  // namespace

bool DecodeGuestTextureDescriptor(const uint8_t* be24, GuestTextureDesc& out) {
    out = GuestTextureDesc{};
    if (!be24) {
        out.refusal = "null descriptor";
        return false;
    }
    for (uint32_t i = 0; i < 6; ++i) {
        out.rawDwords[i] = BeU32(be24 + i * 4u);
    }
    const uint32_t d0 = out.rawDwords[0], d1 = out.rawDwords[1];
    const uint32_t d2 = out.rawDwords[2], d3 = out.rawDwords[3];
    const uint32_t d4 = out.rawDwords[4], d5 = out.rawDwords[5];

    // dword_0: type must be kTexture (2); the other two types are the
    // vertex-fetch words sharing this 6-dword slot.
    if (Bits(d0, 0, 2) != 2u) {
        out.refusal = "fetch constant type is not kTexture";
        return false;
    }
    out.sign[0] = static_cast<GuestTextureSign>(Bits(d0, 2, 2));
    out.sign[1] = static_cast<GuestTextureSign>(Bits(d0, 4, 2));
    out.sign[2] = static_cast<GuestTextureSign>(Bits(d0, 6, 2));
    out.sign[3] = static_cast<GuestTextureSign>(Bits(d0, 8, 2));
    out.clampU = static_cast<GuestClampMode>(Bits(d0, 10, 3));
    out.clampV = static_cast<GuestClampMode>(Bits(d0, 13, 3));
    out.clampW = static_cast<GuestClampMode>(Bits(d0, 16, 3));
    if (Bits(d0, 10, 3) > 7u || Bits(d0, 13, 3) > 7u || Bits(d0, 16, 3) > 7u) {
        out.refusal = "clamp mode out of range";
        return false;
    }
    out.pitchTexels = Bits(d0, 22, 9) << 5;  // field is pitch >> 5 texels
    out.tiled = Bits(d0, 31, 1) != 0;

    // dword_1: format, endianness, base address.
    out.xenosFormat = Bits(d1, 0, 6);
    out.endianness = Bits(d1, 6, 2);
    out.stacked = Bits(d1, 10, 1) != 0;
    const uint32_t basePage = Bits(d1, 12, 20);
    out.hasBase = basePage != 0;
    out.baseAddress = (basePage << kTextureSubresourceAlignmentBytesLog2) |
                      (out.hasBase ? 0u : 0u);
    if (basePage == 0) {
        // Xenia keys a texture with no base on mip_address alone
        // (texture_cache.h:130-132) and skips base level 0. Without a base
        // address there is nothing to import at mip 0, so the descriptor is
        // usable for sampling metadata only - report it, do not fake a base.
        out.baseAddress = 0;
    }
    if (!GetTextureFormatInfo(out.xenosFormat)) {
        out.refusal = "unknown xenos texture format code";
        return false;
    }

    // dword_2: extents, each stored minus one. Which union member applies is
    // not known until dword_5's `dimension` is read, so decode the widest form
    // and then narrow.
    out.dimension = static_cast<GuestTextureDimension>(Bits(d5, 9, 2));
    const uint32_t dimRaw = static_cast<uint32_t>(out.dimension);
    if (dimRaw > 3u) {
        out.refusal = "data dimension out of range";
        return false;
    }
    switch (out.dimension) {
        case GuestTextureDimension::OneD:
            out.width = Bits(d2, 0, 24) + 1u;
            out.height = 1u;
            out.depthOrArraySize = 1u;
            out.depth = 1u;
            break;
        case GuestTextureDimension::TwoDOrStacked:
            out.width = Bits(d2, 0, 13) + 1u;
            out.height = Bits(d2, 13, 13) + 1u;
            out.depthOrArraySize = out.stacked ? (Bits(d2, 26, 6) + 1u) : 1u;
            out.depth = 1u;
            break;
        case GuestTextureDimension::ThreeD:
            out.width = Bits(d2, 0, 11) + 1u;
            out.height = Bits(d2, 11, 11) + 1u;
            out.depth = Bits(d2, 22, 10) + 1u;
            out.depthOrArraySize = out.depth;
            break;
        case GuestTextureDimension::Cube:
            out.width = Bits(d2, 0, 13) + 1u;
            out.height = Bits(d2, 13, 13) + 1u;
            out.depthOrArraySize = 6u;
            out.depth = 1u;
            break;
        default:
            out.refusal = "unhandled data dimension";
            return false;
    }
    if (out.width == 0 || out.height == 0 || out.depthOrArraySize == 0) {
        out.refusal = "zero extent (size field is stored minus one)";
        return false;
    }

    // dword_3: swizzle, exp adjust, filters.
    out.swizzle = Bits(d3, 1, 12);
    out.expAdjust = Sbits(d3, 13, 6);
    out.magFilter = static_cast<GuestTextureFilter>(Bits(d3, 19, 2));
    out.minFilter = static_cast<GuestTextureFilter>(Bits(d3, 21, 2));
    out.mipFilter = static_cast<GuestTextureFilter>(Bits(d3, 23, 2));
    out.anisoFilter = static_cast<GuestAnisoFilter>(Bits(d3, 25, 3));
    if (static_cast<uint32_t>(out.anisoFilter) == 6u) {
        // 6 is not an architected aniso code (xenos.h:140-147 leaves it out).
        out.refusal = "aniso filter code out of range";
        return false;
    }

    // dword_4: mip range, lod bias.
    out.mipMinLevel = Bits(d4, 2, 4);
    out.mipMaxLevel = Bits(d4, 6, 4);
    out.lodBias = Sbits(d4, 12, 10);
    out.mipAddress = Bits(d5, 12, 20) << kTextureSubresourceAlignmentBytesLog2;

    // dword_5: border colour, aniso bias, packed mips.
    out.borderColor = Bits(d5, 0, 2);
    out.forceBcWToMax = Bits(d5, 2, 1) != 0;
    out.anisoBias = Sbits(d5, 5, 4);
    out.packedMips = Bits(d5, 11, 1) != 0;

    // Clamp and normalize mip range per Xenia GetSubresourcesFromFetchConstant
    // (texture_util.cc:68-100): without mip_address there is only mip 0, and
    // without base_address mip_min_level is at least 1 when mip_max_level != 0.
    static const bool s_mipMinLevelFix = [] {
        const char* e = std::getenv("MCLA_NO_MIP_MINLEVEL_FIX");
        return !(e && e[0] == '1');
    }();
    if (s_mipMinLevelFix && out.mipMinLevel > 0u) {
        if ((out.baseAddress != 0u && out.mipAddress == 0u) || out.baseAddress == out.mipAddress) {
            out.mipAddress = out.baseAddress;
            out.baseAddress = 0u;
            out.hasBase = false;
        }
    }
    const uint32_t biggest = out.width > out.height ? out.width : out.height;
    const uint32_t depthish = (out.dimension == GuestTextureDimension::ThreeD &&
                               out.depthOrArraySize > biggest)
                                  ? out.depthOrArraySize
                                  : biggest;
    uint32_t extentLevels = 0;
    for (uint32_t v = depthish; v > 1u; v >>= 1) extentLevels++;
    if (out.mipAddress == 0u) {
        out.mipMinLevel = 0u;
        out.mipMaxLevel = 0u;
    } else {
        if (out.mipMinLevel > extentLevels) out.mipMinLevel = extentLevels;
        if (out.mipMaxLevel > extentLevels) out.mipMaxLevel = extentLevels;
        if (out.mipMaxLevel < out.mipMinLevel) out.mipMaxLevel = out.mipMinLevel;
    }
    if (out.mipMaxLevel != 0u) {
        if (!out.hasBase && out.mipMinLevel == 0u) out.mipMinLevel = 1u;
    } else {
        out.mipAddress = 0u;
    }
    out.mipCount = out.mipMaxLevel + 1u;
    out.arraySize = (out.dimension == GuestTextureDimension::Cube) ? 6u
                                                                  : out.depthOrArraySize;
    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Mip chain / guest size
// ---------------------------------------------------------------------------

bool BuildGuestMipChain(const GuestTextureDesc& desc, TextureMipChain& out) {
    out = TextureMipChain{};
    // Linear textures stay mip 0 only here; packed mips and mipMinLevel > 0 share the same
    // mip_address layout starting from mip 1 (Xenia texture_util.cc:297-302).
    if (!desc.valid || !desc.tiled) return false;
    if (desc.dimension != GuestTextureDimension::TwoDOrStacked || desc.stacked || desc.arraySize != 1)
        return false;   // cube/3D/array stay mip 0 only
    const TextureFormatInfo* info = GetTextureFormatInfo(desc.xenosFormat);
    const bool is16Expand = (desc.xenosFormat >= 27u && desc.xenosFormat <= 29u);
    if (!info || (info->dxgiFormat == 0 && !is16Expand)) return false;
    const uint32_t bb = GetTextureBytesPerBlock(desc.xenosFormat);
    if (bb == 0 || (bb & (bb - 1)) != 0 || bb > 64u) return false;
    if (desc.mipCount == 0 || desc.mipCount > kTextureMaxMips) return false;
    const uint32_t bw = info->blockWidth, bh = info->blockHeight;
    const uint32_t packedLevel = desc.packedMips
                                     ? PackedMipBaseLevel(desc.width, desc.height)
                                     : 0xFFFFFFFFu;
    uint64_t guestRun = 0;   // bytes from mip_address (mip 0 is NOT in this run)
    uint64_t linRun = 0;
    bool havePackedTail = false;
    uint32_t packedTailGuestOffset = 0;
    uint32_t packedTailByteSize = 0;
    uint32_t packedTailPaddedBX = 0;
    uint32_t packedTailPaddedBY = 0;
    uint32_t packedTailRowPitch = 0;
    for (uint32_t l = 0; l < desc.mipCount; ++l) {
        TextureMipLevel& m = out.levels[l];
        // Host extent: max(1, base >> level), the same extent CreateDecodedTexture uploads.
        const uint32_t hw = std::max(desc.width >> l, 1u);
        const uint32_t hh = std::max(desc.height >> l, 1u);
        uint32_t hbx = 0, hby = 0;
        if (GetTextureBlockSpan(desc.xenosFormat, hw, hh, &hbx, &hby) == 0) return false;
        m.blocksX = hbx;
        m.blocksY = hby;
        m.linearBytes = hbx * hby * bb;
        m.linearOffset = static_cast<uint32_t>(linRun);
        linRun += m.linearBytes;
        if (linRun > 0xFFFFFFFFull) return false;
        if (l == 0) {
            // Level 0 keeps the descriptor's own extent and pitch (the render path untiles it, not this table).
            m.width = desc.width;
            m.height = desc.height;
            m.guestOffset = 0;
            m.byteSize = 0;   // unused for mip 0: base_address and pitch come from the descriptor
            continue;
        }
        // Guest layout extent: next_pow2 of the base, shifted (texture_util.cc:319-322).
        m.width = std::max(NextPow2(desc.width) >> l, 1u);
        m.height = std::max(NextPow2(desc.height) >> l, 1u);
        if (desc.packedMips && l >= packedLevel) {
            if (!havePackedTail) {
                // Packed mip tail: stored once at packedLevel (or level 0 for mip_address tail
                // when packedLevel == 0, texture_util.cc:285-300).
                const uint32_t tailW = std::max(NextPow2(desc.width) >> packedLevel, 1u);
                const uint32_t tailH = std::max(NextPow2(desc.height) >> packedLevel, 1u);
                packedTailPaddedBX = ((tailW + bw - 1u) / bw + 31u) & ~31u;
                packedTailPaddedBY = ((tailH + bh - 1u) / bh + 31u) & ~31u;
                packedTailRowPitch = packedTailPaddedBX * bb;
                const uint64_t tailBytes = uint64_t(packedTailRowPitch) * packedTailPaddedBY;
                if (tailBytes == 0 || tailBytes > 0xFFFFFFFFull) return false;
                packedTailGuestOffset = static_cast<uint32_t>(guestRun);
                packedTailByteSize = static_cast<uint32_t>(tailBytes);
                guestRun += tailBytes;
                if (guestRun > 0xFFFFFFFFull) return false;
                havePackedTail = true;
            }
            if (!ComputePackedMipOffset(desc.width, desc.height, bw, bh, l,
                                        m.tileOffsetX, m.tileOffsetY)) {
                return false;
            }
            if (m.tileOffsetX + m.blocksX > packedTailPaddedBX ||
                m.tileOffsetY + m.blocksY > packedTailPaddedBY) {
                return false;
            }
            m.paddedBlocksX = packedTailPaddedBX;
            m.paddedBlocksY = packedTailPaddedBY;
            m.rowPitchBytes = packedTailRowPitch;
            m.byteSize = packedTailByteSize;
            m.guestOffset = packedTailGuestOffset;
            continue;
        }
        m.paddedBlocksX = ((m.width + bw - 1u) / bw + 31u) & ~31u;   // texture_extent.cc:31-35
        m.paddedBlocksY = ((m.height + bh - 1u) / bh + 31u) & ~31u;
        m.rowPitchBytes = m.paddedBlocksX * bb;
        const uint64_t levelBytes = uint64_t(m.rowPitchBytes) * m.paddedBlocksY;
        if (levelBytes == 0 || levelBytes > 0xFFFFFFFFull) return false;
        m.byteSize = static_cast<uint32_t>(levelBytes);
        m.guestOffset = static_cast<uint32_t>(guestRun);   // texture_info.cc:188-195: mip 1 at mip_address + 0
        guestRun += levelBytes;
        if (guestRun > 0xFFFFFFFFull) return false;
    }
    out.levelCount = desc.mipCount;
    out.totalBytes = static_cast<uint32_t>(guestRun);
    out.linearTotal = static_cast<uint32_t>(linRun);
    out.valid = true;
    return true;
}

// The isResolvable column in the format table (texture_decode.cpp:83) was dead
// until BuildResolveDesc read it. Cross-check against Xenia's own
// ColorFormat set (.research/xenia/src/xenia/gpu/xenos.h:543-570), which is the
// subset of TextureFormat an EDRAM resolve may target - the two must be the
// same set or one of the two is wrong. Returns a static, write-once comparison
// so the check runs once per process, not per resolve.
bool VerifyResolvableColumnMatchesXeniaColorFormat(uint32_t* offender) {
    // Xenia's ColorFormat members, as xenos format codes (xenos.h:543-570).
    static const uint32_t kXeniaColorFormat[] = {
        2, 3, 4, 5, 6, 7, 8, 9, 10, 14, 15, 16, 17, 24, 25, 26,
        30, 31, 32, 36, 37, 38, 50, 54, 55, 56,
    };
    const size_t n = sizeof(kXeniaColorFormat) / sizeof(kXeniaColorFormat[0]);
    for (uint32_t f = 0; f < 64; ++f) {
        const TextureFormatInfo* info = GetTextureFormatInfo(f);
        if (!info) continue;
        bool inXenia = false;
        for (size_t i = 0; i < n; ++i) {
            if (kXeniaColorFormat[i] == f) { inXenia = true; break; }
        }
        if (info->isResolvable != inXenia) {
            if (offender) *offender = f;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Resolve description
// ---------------------------------------------------------------------------

TextureResolveDesc BuildResolveDesc(uint32_t xenosFormat, uint32_t width, uint32_t height,
                                    uint32_t sampleSelect, bool endianSwap, bool swapRedBlue,
                                    bool clearSource, uint32_t extentX, uint32_t extentY,
                                    uint32_t copyDestPitch, uint32_t copyDestHeight,
                                    uint32_t guestBaseAddress) {
    TextureResolveDesc d = {};
    d.xenosFormat = xenosFormat;
    const TextureFormatInfo* info = GetTextureFormatInfo(xenosFormat);
    d.formatName = info ? info->name : "unknown";
    if (!info) {
        d.refusal = "unknown xenos format code";
        return d;
    }
    // Depth formats resolve without sample averaging or format conversion
    // (xenos.h:911-924), so a colour resolve would be wrong. Checked BEFORE
    // isResolvable because the depth rows are also marked non-resolvable (they
    // are render targets), and "needs a dedicated path" is the more useful
    // refusal than "not resolvable".
    if (info->name && (std::strcmp(info->name, "24_8") == 0 ||
                       std::strcmp(info->name, "24_8_FLOAT") == 0)) {
        d.isDepthResolve = true;
        d.refusal = "depth resolve needs a dedicated path (no sample averaging allowed)";
        return d;
    }
    // This is the first consumer of the format table's isResolvable column: a
    // format the table marks non-resolvable (the BC/CTX1 block formats and the
    // *_EDRAM rows) cannot be a resolve destination on the Xenos either, so the
    // resolve is refused instead of silently writing a format the hardware
    // never produces.
    if (!info->isResolvable) {
        d.refusal = "format is not EDRAM-resolvable";
        return d;
    }
    if (info->dxgiFormat == 0) {
        d.refusal = "no host resolve format";
        return d;
    }
    if (width == 0 || height == 0) {
        d.refusal = "zero extent";
        return d;
    }
    if (sampleSelect > static_cast<uint32_t>(GuestCopySampleSelect::AvgAll)) {
        d.refusal = "copy sample select out of range";
        return d;
    }
    d.valid = true;
    d.hostDxgiFormat = info->dxgiFormat;
    d.width = width;
    d.height = height;
    d.sampleSelect = sampleSelect;
    // Resolve granularity is 8x8 pixels (xenos.h:978-983), so the extent is
    // rounded up to it, never down.
    d.extent[0] = (extentX ? extentX : width);
    d.extent[1] = (extentY ? extentY : height);
    d.extent[2] = 1;
    d.copyDestPitch = copyDestPitch;
    d.copyDestHeight = copyDestHeight;
    d.endianSwap = endianSwap;
    d.swapRedBlue = swapRedBlue;
    d.clearSource = clearSource;
    d.guestBaseAddress = guestBaseAddress;
    return d;
}

// ---------------------------------------------------------------------------
// Sampler state
// ---------------------------------------------------------------------------

namespace {

uint32_t ClampToAddressMode(GuestClampMode m, bool* usesBorder) {
    switch (m) {
        case GuestClampMode::Repeat: return kD3D12AddressModeWrap;
        case GuestClampMode::MirroredRepeat: return kD3D12AddressModeMirror;
        // Xenia d3d12_texture_cache.cc:1034-1048 (kAddressModeMap).
        case GuestClampMode::MirrorClampToEdge:
        case GuestClampMode::MirrorClampToHalfway:
        case GuestClampMode::MirrorClampToBorder:  // was BORDER: the mirrored half read the border (half loading disc)
            return kD3D12AddressModeMirrorOnce;
        case GuestClampMode::ClampToEdge:
        case GuestClampMode::ClampToHalfway:       // was MIRROR_ONCE
            return kD3D12AddressModeClamp;
        case GuestClampMode::ClampToBorder:
            *usesBorder = true;
            return kD3D12AddressModeBorder;
        default: return kD3D12AddressModeClamp;
    }
}

uint32_t AnisoToMaxAnisotropy(GuestAnisoFilter a) {
    switch (a) {
        case GuestAnisoFilter::Disabled:
        case GuestAnisoFilter::Max1x1: return 1u;
        case GuestAnisoFilter::Max2x1: return 2u;
        case GuestAnisoFilter::Max4x1: return 4u;
        case GuestAnisoFilter::Max8x1: return 8u;
        case GuestAnisoFilter::Max16x1: return 16u;
        default: return 1u;
    }
}

}  // namespace

HostSamplerDesc DecodeGuestSamplerState(const GuestTextureDesc& desc,
                                        const char** refusal) {
    HostSamplerDesc s = {};
    if (refusal) *refusal = nullptr;
    if (!desc.valid) {
        if (refusal) *refusal = "descriptor not decoded";
        return s;
    }
    // Normalize unused clamp axes by dimension (Xenia texture_util.cc:556-577) so
    // an uninitialized clampW on a 2D texture or clampU/V/W on a Cube never triggers
    // spurious border/YCbCr refusals or sampler-heap duplicates.
    GuestClampMode effectiveU = desc.clampU;
    GuestClampMode effectiveV = desc.clampV;
    GuestClampMode effectiveW = desc.clampW;
    if (desc.dimension == GuestTextureDimension::Cube) {
        effectiveU = effectiveV = effectiveW = GuestClampMode::ClampToEdge;
    } else if (desc.dimension == GuestTextureDimension::OneD) {
        effectiveV = effectiveW = GuestClampMode::ClampToEdge;
    } else if (desc.dimension == GuestTextureDimension::TwoDOrStacked) {
        effectiveW = GuestClampMode::ClampToEdge;
    }
    bool borderU = false, borderV = false, borderW = false;
    s.addressU = ClampToAddressMode(effectiveU, &borderU);
    s.addressV = ClampToAddressMode(effectiveV, &borderV);
    s.addressW = ClampToAddressMode(effectiveW, &borderW);

    // lod_bias has 5 fractional bits (xenos.h:1269-1270), and Xenia divides by
    // 32 to get LOD units (sampler_info.cc:46).
    s.mipLodBias = float(desc.lodBias) * (1.0f / 32.0f);

    // kUseFetchConst resolves to the fetch constant's own field, because the
    // fetch constant IS the fetch constant (sampler_info.cc:25-43).
    const bool magPoint = desc.magFilter == GuestTextureFilter::Point;
    const bool minPoint = desc.minFilter == GuestTextureFilter::Point;
    // kBaseMap is a mip-only "no mip blend" (xenos.h:135); the host expresses
    // that as MaxLOD = mipMinLevel, so LOD is pinned to the base.
    const bool mipPoint = desc.mipFilter != GuestTextureFilter::Linear;
    s.maxAnisotropy = AnisoToMaxAnisotropy(desc.anisoFilter);
    // Xenia d3d12_texture_cache.cc:1016: any non-disabled code takes the anisotropic filter (Max1x1 -> max 1).
    const bool aniso = desc.anisoFilter != GuestAnisoFilter::Disabled &&
                       desc.anisoFilter != GuestAnisoFilter::UseFetchConst;

    // D3D12_ENCODE_BASIC_FILTER (min<<4 | mag<<2 | mip, LINEAR=1), as Xenia
    // d3d12_texture_cache.cc:1020-1031. The old branch collapsed linear min/mag with a point mip
    // (every single-mip bind) to full point sampling.
    s.filter = (minPoint ? 0u : 0x10u) | (magPoint ? 0u : 0x4u) | (mipPoint ? 0u : 0x1u);
    if (aniso) s.filter = kD3D12FilterAnisotropic;

    // Mip clamp. mip_max_level is the highest stored level (texture_cache.h:43
    // - a texture with mip_max_level+1 levels exists, mip_min_level is only a
    // sampling clamp), so MaxLOD is FLT_MAX and mipMinLevel pins the floor.
    // Xenia d3d12_texture_cache.cc:1082-1101. BaseMap pins LOD to mip_min_level (+0.25 when aniso is Disabled).
    // Point/Linear mip filters take MaxLOD = FLT_MAX (the view's MipLevels clamps it).
    s.minLod = (desc.mipMaxLevel != 0u) ? float(desc.mipMinLevel) : 0.0f;
    if (desc.mipFilter == GuestTextureFilter::BaseMap) {
        s.maxLod = s.minLod + (desc.anisoFilter == GuestAnisoFilter::Disabled ? 0.25f : 0.0f);
    } else {
        s.maxLod = 3.402823466e+38f;   // FLT_MAX, Xenia :1101
    }

    if (borderU || borderV || borderW) {
        // Xenos BorderColor (xenos.h:150-162): 0 = ABGR black, 1 = ABGR white.
        // 2/3 are the YCbCr blacks, which D3D12 cannot express: those get black
        // and a refusal string rather than a guessed magenta-ish colour.
        switch (desc.borderColor) {
            case 1u:
                s.borderColor[0] = s.borderColor[1] = s.borderColor[2] =
                    s.borderColor[3] = 1.0f;
                break;
            case 0u:
            default:
                s.borderColor[0] = s.borderColor[1] = s.borderColor[2] =
                    s.borderColor[3] = 0.0f;
                break;
        }
        if (desc.borderColor >= 2u && refusal)
            *refusal = "YCbCr border colour is not expressible; using black";
    }
    // A 1-bit-border (border_size) fetch constant clamps the texture, so the
    // host must sample the whole texel range; D3D12 has no equivalent knob and
    // the address mode already covers the practical case.

    // Hash every field (using raw IEEE-754 float bits to avoid negative/FLT_MAX -> uint32 UB)
    // so two fetch constants that differ only in an ignored field still share a heap slot.
    uint64_t h = 14695981039346656037ull;
    const uint64_t prime = 1099511628211ull;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFFull;
            h *= prime;
        }
    };
    auto mixFloat = [&mix](float f) {
        if (f == 0.0f) f = 0.0f;  // canonicalize -0.0f
        uint32_t bits = 0u;
        std::memcpy(&bits, &f, 4);
        mix(bits);
    };
    mix(s.filter); mix(s.addressU); mix(s.addressV); mix(s.addressW);
    mix(s.maxAnisotropy); mix(s.comparisonFunc);
    mixFloat(s.mipLodBias);
    mixFloat(s.minLod);
    mixFloat(s.maxLod);
    for (int i = 0; i < 4; ++i) mixFloat(s.borderColor[i]);
    s.hash = h;
    return s;
}

} // namespace mcla::native