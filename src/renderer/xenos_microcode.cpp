#include "xenos_microcode.h"

#include <algorithm>
#include <array>

namespace mcla::renderer {
namespace {

template <typename E, size_t N>
const char* EnumName(const std::array<const char*, N>& names, E value) {
    size_t idx = static_cast<size_t>(value);
    return idx < names.size() ? names[idx] : "?";
}

}  // namespace

bool ControlFlowInstruction::IsExec() const {
    switch (opcode) {
        case ControlFlowOpcode::Exec:
        case ControlFlowOpcode::ExecEnd:
        case ControlFlowOpcode::CondExec:
        case ControlFlowOpcode::CondExecEnd:
        case ControlFlowOpcode::CondExecPred:
        case ControlFlowOpcode::CondExecPredEnd:
        case ControlFlowOpcode::CondExecPredClean:
        case ControlFlowOpcode::CondExecPredCleanEnd:
            return true;
        default:
            return false;
    }
}

const char* ControlFlowOpcodeName(ControlFlowOpcode opcode) {
    static constexpr std::array<const char*, 16> kNames = {
        "Nop", "Exec", "ExecEnd", "CondExec", "CondExecEnd", "CondExecPred",
        "CondExecPredEnd", "LoopStart", "LoopEnd", "CondCall", "Return",
        "CondJmp", "Alloc", "CondExecPredClean", "CondExecPredCleanEnd",
        "MarkVsFetchDone",
    };
    return EnumName(kNames, opcode);
}

ControlFlowInstruction DecodeControlFlow(uint32_t word0, uint16_t word1) {
    ControlFlowInstruction cf;
    cf.address = word0 & 0xFFF;
    cf.count = (word0 >> 12) & 0x7;
    cf.isYield = ((word0 >> 15) & 0x1) != 0;
    cf.sequence = (word0 >> 16) & 0xFFF;
    cf.vertexCacheHigh = (word0 >> 28) & 0xF;
    cf.vertexCacheLow = word1 & 0x3;
    cf.isPredicateClean = ((word1 >> 9) & 0x1) != 0;
    cf.condition = ((word1 >> 10) & 0x1) != 0;
    cf.absoluteAddressing = ((word1 >> 11) & 0x1) != 0;
    cf.opcode = static_cast<ControlFlowOpcode>((word1 >> 12) & 0xF);
    return cf;
}

size_t ComputeControlFlowByteBound(const uint8_t* code, size_t code_size) {
    // Sequential, self-limiting walk that mirrors the XenosRecomp pass-1
    // clamp (.research/XenosRecomp/XenosRecomp/shader_recompiler.cpp
    // 1508-1560, where instrSize shrinks to min(instrSize, address * 12)):
    // the loop limit is the evolving bound, so decode stops at the first
    // executable region instead of scanning the whole payload (where
    // instruction data can decode as a bogus exec with a tiny address and
    // truncate the program to ~1 block).
    size_t bound = code_size;
    // Only walk fully-contained 12-byte blocks. A malformed stream whose
    // shaderSize is not a multiple of 12 would otherwise read up to 11 bytes
    // past the end on the final partial block. Exec-derived bounds are always
    // multiples of 12, so this only ever trims the initial code_size guard.
    for (size_t instr_addr = 0; instr_addr + 12 <= bound; instr_addr += 12) {
        const uint8_t* blk = code + instr_addr;
        uint32_t w0 = AssembleBE32(blk);
        uint32_t w1 = AssembleBE32(blk + 4);
        uint32_t w2 = AssembleBE32(blk + 8);
        for (int slot = 0; slot < 2; ++slot) {
            uint32_t cfw0 = slot == 0 ? w0 : ((w1 >> 16) | (w2 << 16));
            uint16_t cfw1 = slot == 0 ? static_cast<uint16_t>(w1 & 0xFFFF)
                                      : static_cast<uint16_t>(w2 >> 16);
            ControlFlowInstruction cf = DecodeControlFlow(cfw0, cfw1);
            if (cf.IsExec() && cf.address != 0) {
                bound = std::min(bound, size_t(cf.address) * 12);
            }
        }
    }
    return bound;
}

const char* FetchOpcodeName(FetchOpcode opcode) {
    static constexpr std::array<const char*, 32> kNames = {
        "VertexFetch", "TextureFetch", "f?2", "f?3", "f?4", "f?5", "f?6",
        "f?7", "f?8", "f?9", "f?10", "f?11", "f?12", "f?13", "f?14", "f?15",
        "GetTextureBorderColorFrac", "GetTextureComputedLod", "GetTextureGradients",
        "GetTextureWeights", "f?20", "f?21", "f?22", "f?23", "SetTextureLod",
        "SetTextureGradientsHorz", "SetTextureGradientsVert", "f?27", "f?28",
        "f?29", "f?30", "f?31",
    };
    return EnumName(kNames, opcode);
}

const char* AluVectorOpcodeName(AluVectorOpcode opcode) {
    static constexpr std::array<const char*, 30> kNames = {
        "Add", "Mul", "Max", "Min", "Seq", "Sgt", "Sge", "Sne", "Frc", "Trunc",
        "Floor", "Mad", "CndEq", "CndGe", "CndGt", "Dp4", "Dp3", "Dp2Add", "Cube",
        "Max4", "SetpEqPush", "SetpNePush", "SetpGtPush", "SetpGePush", "KillEq",
        "KillGt", "KillGe", "KillNe", "Dst", "MaxA",
    };
    return EnumName(kNames, opcode);
}

const char* AluScalarOpcodeName(AluScalarOpcode opcode) {
    static constexpr std::array<const char*, 51> kNames = {
        "Adds", "AddsPrev", "Muls", "MulsPrev", "MulsPrev2", "Maxs", "Mins",
        "Seqs", "Sgts", "Sges", "Snes", "Frcs", "Truncs", "Floors", "Exp",
        "Logc", "Log", "Rcpc", "Rcpf", "Rcp", "Rsqc", "Rsqf", "Rsq", "MaxAs",
        "MaxAsf", "Subs", "SubsPrev", "SetpEq", "SetpNe", "SetpGt", "SetpGe",
        "SetpInv", "SetpPop", "SetpClr", "SetpRstr", "KillsEq", "KillsGt",
        "KillsGe", "KillsNe", "KillsOne", "Sqrt", "s?41", "Mulsc0", "Mulsc1",
        "Addsc0", "Addsc1", "Subsc0", "Subsc1", "Sin", "Cos", "RetainPrev",
    };
    return EnumName(kNames, opcode);
}

DecodedInstruction DecodeInstruction(uint32_t word0, uint32_t word1,
                                     uint32_t word2, bool isFetch) {
    DecodedInstruction instr;

    if (isFetch) {
        // FetchInstruction (XenosRecomp shader_code.h):
        //   word0: opcode:5, srcReg:6, srcRegAm:1, dstReg:6, dstRegAm:1,
        //          (vfetch: mustBeOne:1 / tfetch: fetchValidOnly:1),
        //          constIndex:5, (vfetch: constIndexSelect:2 / tfetch: texCoordDenorm:1),
        //          srcSwizzle:(vfetch 2 / tfetch 6)
        instr.kind = (word0 & 0x1F) == 0 ? InstructionKind::VertexFetch
                                         : InstructionKind::TextureFetch;
        instr.fetchOpcode = static_cast<FetchOpcode>(word0 & 0x1F);
        instr.srcRegister = (word0 >> 5) & 0x3F;
        instr.srcRelative = ((word0 >> 11) & 0x1) != 0;
        instr.dstRegister = (word0 >> 12) & 0x3F;
        instr.dstRelative = ((word0 >> 18) & 0x1) != 0;
        instr.constIndex = (word0 >> 20) & 0x1F;

        if (instr.kind == InstructionKind::VertexFetch) {
            // VertexFetchInstruction:
            //   word0: ... constIndexSelect:2 (bits 25-26), prefetchCount:3 (27-29),
            //          srcSwizzle:2 (30-31)
            //   word1: dstSwizzle:12, formatCompAll:1(12), numFormatAll:1(13),
            //          signedRfModeAll:1(14), isIndexRounded:1(15),
            //          format:6(16-21), reserved2:2(22-23), expAdjust:6(24-29),
            //          isMiniFetch:1(30), isPredicated:1(31)
            //   word2: stride:8, offset:23, predCondition:1
            // Below, all fields match this layout: isSigned/isNormalized
            // are formatCompAll/numFormatAll (signedRfModeAll at bit 14
            // has no DecodedInstruction field and is not read), and
            // expAdjust is bits 24-29, AFTER the 2 reserved2 bits at
            // 22-23. Reading it two bits low (>>22) folds reserved2 into
            // the low end of the value, i.e. reports 4x the true
            // adjustment on any word where expAdjust != 0.
            instr.constIndexSelect = (word0 >> 25) & 0x3;
            instr.prefetchCount = (word0 >> 27) & 0x7;
            instr.srcSwizzle = (word0 >> 30) & 0x3;

            instr.dstSwizzle = word1 & 0xFFF;
            instr.isSigned = ((word1 >> 12) & 0x1) != 0;
            instr.isNormalized = ((word1 >> 13) & 0x1) == 0;
            instr.isIndexRounded = ((word1 >> 15) & 0x1) != 0;
            instr.vertexFormat = (word1 >> 16) & 0x3F;
            instr.expAdjust = static_cast<int32_t>((word1 >> 24) & 0x3F);
            if (instr.expAdjust >= 32) instr.expAdjust -= 64;
            instr.isMiniFetch = ((word1 >> 30) & 0x1) != 0;
            instr.isPredicated = ((word1 >> 31) & 0x1) != 0;

            instr.stride = word2 & 0xFF;
            instr.offset = static_cast<int32_t>((word2 >> 8) & 0x7FFFFF);
            if (instr.offset >= (1 << 22)) instr.offset -= (1 << 23);
            instr.predicateCondition = ((word2 >> 31) & 0x1) != 0;
        } else {
            // TextureFetchInstruction:
            //   word0: ... fetchValidOnly:1(19), constIndex:5(20-24),
            //          texCoordDenorm:1(25), srcSwizzle:6(26-31)
            //   word1: dstSwizzle:12, mag/min/mip/vol filters, aniso, arbitrary,
            //          useCompLod, useRegLod, unk, isPredicated (bit 31)
            //   word2: useRegGradients:1, sampleLocation:1, lodBias:7, :5,
            //          dimension:2 (bits 14-15), offsetX/Y/Z:5 each,
            //          predCondition:1
            //
            // F-900 (2026-10-27): THE dimension SHIFT IS 14, NOT 13 -- MEASURED,
            // settling F-898(h). The old read took bits 13-14 (the comment above
            // even admitted the discrepancy against its own bitfield line). Raw
            // word from the live psB=48 program's tfetch (CP-SHADER-BODY in
            // build/g944.log and g945.log, body
            // [00000000 2001c400 22000009 10080001 1f1ff688 00004000 ...],
            // fetch instr at dwords 3-5): word2 = 0x00004000.
            //   (0x4000 >> 13) & 3 = 2  -> translator chose tCube (t3)
            //   (0x4000 >> 14) & 3 = 1  -> t2D (t1)
            // The descriptor for the SAME texture decodes dimension = 1 (2D,
            // TEX-SLOT-DECIDE rows: dim=1 fmt=19 256x256) and the bind lands at
            // heap slot 1 (t1), so the CORRECT shader resource is t2D/t1 -- the
            // reference read agrees with both independent facts, and the old read
            // made the shader sample tCube/t3 (a NULL SRV) while the real texture
            // sat at t1: the black screen. ucode.h:868-874 places dimension:2 at
            // bits 14-15 (fields: use_reg_gradients:1, sample_location:1,
            // lod_bias:7, unused:5, dimension:2) -- the reference layout holds.
            instr.srcSwizzle = (word0 >> 26) & 0x3F;
            instr.dstSwizzle = word1 & 0xFFF;
            instr.isPredicated = ((word1 >> 31) & 0x1) != 0;
            instr.dimension = (word2 >> 14) & 0x3;
            instr.predicateCondition = ((word2 >> 31) & 0x1) != 0;
        }
        // Known fetch opcodes are the sparse FetchOpcode enum members
        // {0,1,16,17,18,19,24,25,26}; the rest are the "f?N" placeholders in
        // FetchOpcodeName and are flagged unknown here. Lowering keys on the
        // VertexFetch/TextureFetch kind only, so without this flag an
        // unfilled opcode would be emitted as a plain fetch instead of
        // failing the Phase 5 unknown-opcode gate.
        const uint32_t fo = static_cast<uint32_t>(instr.fetchOpcode);
        instr.unknown = !(fo == 0 || fo == 1 || (fo >= 16 && fo <= 19) ||
                          (fo >= 24 && fo <= 26));
        return instr;
    }

    // ALU (XenosRecomp AluInstruction):
    //   word0: vectorDest:6, vectorDestRelative:1, absConstants:1, scalarDest:6,
    //          scalarDestRelative:1, exportData:1, vectorWriteMask:4,
    //          scalarWriteMask:4, vectorSaturate:1, scalarSaturate:1,
    //          scalarOpcode:6
    //   word1: src3Swizzle:8, src2Swizzle:8, src1Swizzle:8, src3Negate:1,
    //          src2Negate:1, src1Negate:1, predicateCondition:1, isPredicated:1,
    //          constAddressRegisterRelative:1, const1Relative:1, const0Relative:1
    //   word2: src3Register:8, src2Register:8, src1Register:8,
    //          vectorOpcode:5, src3Select:1, src2Select:1, src1Select:1
instr.kind = InstructionKind::Alu;
    instr.vectorDest = word0 & 0x3F;
    instr.vectorDestRelative = ((word0 >> 6) & 0x1) != 0;
    instr.absConstants = ((word0 >> 7) & 0x1) != 0;
    instr.scalarDest = (word0 >> 8) & 0x3F;
    instr.scalarDestRelative = ((word0 >> 14) & 0x1) != 0;
    instr.exportData = ((word0 >> 15) & 0x1) != 0;
    instr.vectorWriteMask = (word0 >> 16) & 0xF;
    instr.scalarWriteMask = (word0 >> 20) & 0xF;
    instr.vectorSaturate = ((word0 >> 24) & 0x1) != 0;
    instr.scalarSaturate = ((word0 >> 25) & 0x1) != 0;
    instr.scalarOpcode = static_cast<AluScalarOpcode>((word0 >> 26) & 0x3F);

    instr.src3Swizzle = word1 & 0xFF;
    instr.src2Swizzle = (word1 >> 8) & 0xFF;
    instr.src1Swizzle = (word1 >> 16) & 0xFF;
    instr.src3Negate = ((word1 >> 24) & 0x1) != 0;
    instr.src2Negate = ((word1 >> 25) & 0x1) != 0;
    instr.src1Negate = ((word1 >> 26) & 0x1) != 0;
    instr.predicateCondition = ((word1 >> 27) & 0x1) != 0;
    instr.isPredicated = ((word1 >> 28) & 0x1) != 0;
    instr.constAddressRegisterRelative = ((word1 >> 29) & 0x1) != 0;
    instr.const1Relative = ((word1 >> 30) & 0x1) != 0;
    instr.const0Relative = ((word1 >> 31) & 0x1) != 0;

    instr.src3Register = word2 & 0xFF;
    instr.src2Register = (word2 >> 8) & 0xFF;
    instr.src1Register = (word2 >> 16) & 0xFF;
    instr.vectorOpcode = static_cast<AluVectorOpcode>((word2 >> 24) & 0x1F);
    instr.src3Select = ((word2 >> 29) & 0x1) != 0;
    instr.src2Select = ((word2 >> 30) & 0x1) != 0;
    instr.src1Select = ((word2 >> 31) & 0x1) != 0;

    const uint32_t scalarRaw = static_cast<uint32_t>(instr.scalarOpcode);
    const bool scalarKnown =
        scalarRaw <= 40 ||
        (scalarRaw >= 42 && scalarRaw <= 50);
    instr.unknown = static_cast<uint32_t>(instr.vectorOpcode) > 29 || !scalarKnown;
    return instr;
}

}  // namespace mcla::renderer
