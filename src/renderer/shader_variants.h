#pragma once

#include "renderer/shader_translator.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mcla::renderer {

// Version 1 stores a complete field-wise lookup witness beside each HLSL blob.
// A hash only chooses the bucket; lookup compares every witness field. The raw
// envelope is hashed with the same FNV-1a function used by the render thread.
struct ShaderVariantKey {
    uint32_t formatVersion = 1;
    uint32_t stage = 0;                    // 0 = VS, 1 = PS
    uint64_t programContentHash = 0;       // raw, wrapped shader container/envelope
    uint64_t inputDeclarationHash = 0;     // VS only; zero means no VS inputs
    uint32_t inputLayoutCount = 0;
    uint32_t clipDisableMode = 0;          // 0 = no rescale, 1 = rescale requested
    uint32_t clipExtentW = 0;
    uint32_t clipExtentH = 0;
    uint64_t pairedVsContentHash = 0;      // PS only
    uint32_t psNumReg = 0;
    int32_t paramGenReg = -1;
    uint32_t paramGenPrim = 0;
    uint32_t renderScale = 1;
    uint32_t ucpMask = 0;
    uint32_t ucpCullOnly = 0;
    uint32_t rt7e3Mask = 0;
    int32_t rtExpBias[4] = {0, 0, 0, 0};
    uint32_t xuvRsMask = 0;
    uint32_t xuvRsScale = 1;
    uint32_t vsModifierMask = 0;           // bit 0 XE_RAWVB, bit 1 XE_YFLIP
    uint32_t psAlphaTestFunction = 7;
    uint32_t psAlphaReferenceRaw = 0;
    uint32_t psAlphaFlags = 0;
};

// Wire image: fixed little-endian fields only, no compiler struct padding.
// Every fixed field is explicitly written/read. u32/u64 values occupy their
// exact little-endian width; i32 values occupy u32 storage with two's-complement
// bits. Layout (all offsets hexadecimal):
//   00  magic "MCLASV01"                      8 bytes
//   08  formatVersion u32 = 1
//   0C  headerSize u32 = 152
//   10  recordSize u32 = 216
//   14  payloadChecksum u32 = FNV-1a-32(payload)
//   18  recordCount u32
//   1C  reserved u32 = 0
//   20  stage u32 (0 VS / 1 PS)
//   24  programContentHash u64
//   2C  inputDeclarationHash u64
//   34  inputLayoutCount u32
//   38  clipDisableMode u32
//   3C  clipExtentW u32
//   40  clipExtentH u32
//   44  pairedVsContentHash u64
//   4C  psNumReg u32
//   50  paramGenReg i32
//   54  paramGenPrim u32
//   58  renderScale u32
//   5C  ucpMask u32
//   60  ucpCullOnly u32
//   64  rt7e3Mask u32
//   68  rtExpBias[0] i32
//   6C  rtExpBias[1] i32
//   70  rtExpBias[2] i32
//   74  rtExpBias[3] i32
//   78  xuvRsMask u32
//   7C  xuvRsScale u32
//   80  vsModifierMask u32
//   84  psAlphaTestFunction u32
//   88  psAlphaReferenceRaw u32
//   8C  psAlphaFlags u32
//   90  programHash u64
//   98  containerSize u64
//   A0  hlslSize u64
//   A8  hlslChecksum u32 = FNV-1a-32(HLSL bytes)
//   AC  reserved[2] u32 = 0
//   B0  reserved[16] u32 = 0
//   D0  container bytes (containerSize)
//   D0 + containerSize  HLSL bytes (hlslSize), exact bytes, no terminator
constexpr uint32_t kShaderVariantFormatVersion = 1;
constexpr uint32_t kShaderVariantHeaderSize = 152;
constexpr uint32_t kShaderVariantRecordSize = 216;
constexpr uint32_t kShaderVariantMaxContainerSize = 8u * 1024u * 1024u;
constexpr uint32_t kShaderVariantMaxHlslSize = 16u * 1024u * 1024u;
constexpr uint32_t kShaderVariantMaxRecords = 262144u;
constexpr uint32_t kShaderVariantMaxFileSize =
    kShaderVariantHeaderSize + kShaderVariantMaxRecords *
        (kShaderVariantRecordSize + kShaderVariantMaxContainerSize +
         kShaderVariantMaxHlslSize);

uint64_t ShaderVariantContentHash(const uint8_t* data, size_t size,
                                  uint64_t seed = 0xcbf29ce484222325ULL);

// Refuses incomplete/unsafe key inputs rather than inventing values. `reason`
// names the first missing field. PS callers may omit alpha only when
// `allowPostLoadAlphaTest` is true; that variant stores pre-alpha HLSL and the
// existing post-load alpha modifier remains authoritative.
bool MakeShaderVariantKey(ShaderVariantKey& out, std::string& reason,
                          bool isVertex, const uint8_t* container,
                          size_t containerSize, uint64_t programContentHash,
                          const VertexInputSemantic* layout, size_t layoutCount,
                          uint64_t inputDeclarationHash,
                          bool applyClipDisableRescale, uint32_t clipExtentW,
                          uint32_t clipExtentH, const PixelShaderPairing* pairing,
                          const ShaderTranslateExtras* extras, uint32_t vsModifierMask,
                          bool haveAlphaTest, uint32_t alphaTestFunction,
                          uint32_t alphaReferenceRaw, uint32_t alphaFlags,
                          bool allowPostLoadAlphaTest);

bool ShaderVariantKeyEqual(const ShaderVariantKey& a, const ShaderVariantKey& b);

// Lazily loads once per process. Missing/unreadable files and an explicit
// MCLA_NO_SHADER_VARIANTS=1 disable lookup without preventing dumps. Corrupt
// records are skipped, never partially accepted, and counted.
bool FindShaderVariant(const ShaderVariantKey& key, TranslatedShader& out,
                       uint64_t* matchedProgramHash = nullptr);

// Appends one self-contained record. An existing identical full witness + HLSL
// is left untouched; a same-witness/HLSL mismatch is rejected (not overwritten).
// Safe for concurrent render/prep workers. MCLA_DUMP_SHADER_VARIANTS=1 is
// required; it still works when lookup is disabled.
bool DumpShaderVariant(const ShaderVariantKey& key, const TranslatedShader& shader,
                       const uint8_t* container, size_t containerSize);

struct ShaderVariantCounters {
    uint64_t loadAttempts = 0;
    uint64_t loadsOk = 0;
    uint64_t loadsFailed = 0;
    uint64_t lookups = 0;
    uint64_t lookupDisabled = 0;
    uint64_t keyRefused = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t dumps = 0;
    uint64_t dumpDuplicates = 0;
    uint64_t dumpFailed = 0;
    uint64_t rejectedRecords = 0;
    uint64_t lookupChecksumFailures = 0;
    uint64_t dumpWriteFailures = 0;
    uint64_t dumpLockTimeouts = 0;
    uint64_t headerFailures = 0;
    uint64_t recordFailures = 0;
};

ShaderVariantCounters ShaderVariantCountersSnapshot();

// Throttled rows carry process-lifetime totals. First load always emits a
// positive-control summary, including "disabled", "missing" and zero records.
// `forceSummary` is intended for a diagnostic/explicit lookup boundary.
void LogShaderVariantSummary(bool forceSummary = false);

} // namespace mcla::renderer
