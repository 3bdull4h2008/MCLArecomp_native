#pragma once

#include <cstdint>
#include <variant>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

namespace mcla::native {

struct DrawIndexedCommand {
    uint32_t vbAddr;
    uint32_t ibAddr;
    uint32_t vbSize;
    uint32_t ibSize;
    uint32_t indexCount;
    uint32_t startIndexLocation;
    uint32_t baseVertexLocation;
    uint32_t vbStride;
    uint32_t ibFormat;
    uint32_t primitiveTopology;
    // Shader program guest addresses for PSO lookup
    uint32_t vsProgram;       // SQ_VS_PROGRAM guest address (.fxc container)
    uint32_t psProgram;       // SQ_PS_PROGRAM guest address (.fxc container)
    // grcFvf vertex declaration (16 bytes, host-endian)
    uint32_t fvfMask;
    uint8_t  fvfSize;
    uint8_t  fvfFlags;
    uint8_t  fvfDynamicOrder;
    uint8_t  fvfChannelCount;
    uint64_t fvfTypes;
    uint8_t  hasGrcFvf;       // 1 when grcFvf is valid
    uint8_t  _pad[7];
};

struct SetPipelineStateCommand {
    uint64_t psoHash;
};

struct SetVertexBuffersCommand {
    uint32_t vbAddr;
    uint32_t stride;
    uint32_t offset;
};

struct SetIndexBufferCommand {
    uint32_t ibAddr;
    uint32_t format;
    uint32_t size;
};

struct PresentCommand {
    uint32_t frameNumber;
    uint32_t obj;
    uint32_t swapInfo;
    // W7: resolved surface VA (linear color buffer) when swapInfo is a
    // swap-table slot rather than pixels. 0 = no resolved surface.
    uint32_t surfaceVA;
};

struct SetRenderStateCommand {
    uint32_t deviceAddr;
    uint32_t value;
};

struct InitD3D12Command {
    void* hwnd;
    uint32_t width;
    uint32_t height;
};

// F-611: what a PM4_TYPE3_DRAW_INDX actually yields, and nothing more.
//
// The CP draw is CARRIED BY ADDRESS, never by bytes: the producer (the guest CP
// thread, src/gpu_cp.cpp) hands over guest VAs plus the register fields that
// describe them, and the render thread does the checked guest read - the same
// split DRAW_INDEXED already uses (src/render_thread.cpp:361-362). A PM4 index
// buffer can be `num_words(24b) * 4` = 64 MB, so shipping it through the queue
// would put a heap allocation on every guest draw; this way the draw path
// allocates nothing per frame.
//
// F-611 fields, all sourced from the Xenos register file at the moment the
// packet executes (src/gpu_cp.cpp, CpBridgeDraw):
//   sourceSelect      VGT_DRAW_INITIATOR.source_select (registers.h:316). Only
//                     kDMA (0) carries a guest index-buffer address at all;
//                     kAutoIndex sets base=0, length=0 upstream
//                     (xenia command_processor.cc:1357-1361) and kImmediate is
//                     unhandled upstream (:1348-1356).
//   primitiveTopology VGT_DRAW_INITIATOR.prim_type (registers.h:315), xenos.h:44.
//   numIndices        VGT_DRAW_INITIATOR.num_indices (registers.h:327). NOT
//                     derivable from ibSize: a real index buffer is usually
//                     longer than the draw, and the old handler derived
//                     `indexCount = ibSize / elemSize`, which over-drew every
//                     such buffer by the unused tail.
//   ibSwap            VGT_DMA_SIZE.swap_mode (registers.h:306), the hardware
//                     byte-reorder applied to the index stream. It is NOT a
//                     cosmetic field: a wrong reading is a wrong index.
//   idxOffset         VGT_INDX_OFFSET (register 0x2102, register_table.inc:503),
//                     the value the VGT adds to every index before the fetch.
//                     Carried so the consumer can refuse honestly instead of
//                     silently assuming zero.
//
// There is deliberately NO startIndex. Upstream hands IssueDraw the whole index
// buffer plus a count, never a start offset (command_processor.cc:1342-1346);
// the low bits masked off dmaBase are an alignment precaution, not an offset.
// Inventing one would be a guess about guest state.
// F-648: ONE fully-specified vertex element, carried instead of printed.
//
// F-632 decoded the vertex format correctly (CpVertexDescriptor::dxgiFormat,
// src/gpu_cp.cpp) and then only PRINTED it on the bounded CP-DRAW-BRIDGE line.
// Nothing reached the consumer, so the draw path had a stride and no format, and
// the backend's input layout could not be built from the guest's own data.
//
// WHY THIS IS A STRONG, CHECKED VALUE AND NOT AN INDEX (F-609: no fakes):
//   - It is four stored scalars, not an index into a table, so it CANNOT go out
//     of range. There is no lookup to bounds-check at the point of use.
//   - `dxgi == 0` is DXGI_FORMAT_UNKNOWN, which is NOT a legal input-layout
//     element format. It is a refusal, not a default: render_thread.cpp refuses
//     `vb-fmt-unmapped` when known == 0 || bytes == 0, so the zero-initialised
//     state of a partially-filled command can never silently become pixels.
//   - `bytes` is the TIGHT size of the format itself, carried so the consumer can
//     check the format/stride pair for mutual consistency (`stride >= bytes`)
//     without re-deriving it from a format enum it does not own. This is the
//     check that makes "a format and a stride that cannot both be true"
//     structurally unshippable rather than merely unlikely.
//   - `xenos` is carried ALONGSIDE `dxgi`, not instead of it, so the guest's own
//     6-bit data_format() survives to the renderer even if the DXGI mapping is
//     ever revised (F-632 records that this project shipped two hand-written
//     format tables that disagreed with the SDK, both times silently).
struct CpVertexFormat {
    uint32_t xenos;   // VertexFetchInstruction::data_format(), 6 bits
    uint32_t dxgi;    // DXGI_FORMAT_*; 0 == UNKNOWN and never valid
    uint32_t bytes;   // tight size of the format in bytes; 0 == unmapped
    uint32_t known;   // 1 when `xenos` is one of the named xenos::VertexFormats
};

// How many vertex-fetch instructions ONE captured draw can carry. 8 is not a
// guess: it is the producer's own bound - CpBridgeDraw decodes into
// `CpVertexDescriptor desc[8]` and passes maxOut = 8 (src/gpu_cp.cpp), and
// CpDecodeVertexDescriptor STOPS at maxOut rather than overflowing. A program
// with more than 8 fetches is truncated by that pre-existing bound, not by
// this array, so the two cannot disagree.
constexpr uint32_t kCpVertexElementMax = 8u;

// F-899 (2026-10-27): the texture-descriptor carry bound. The fetch window has
// 32 slots; the root signature binds t0..t3, so at most 4 can be sampled, but
// 16 is kept as the carry bound (matching capture_hooks' kMaxTextureDescriptors)
// so an over-bind is visible as a refusal rather than silently dropped.
constexpr uint32_t kCpMaxTextureDescriptors = 16u;

// ONE vertex-fetch instruction's declaration, carried verbatim from the guest's
// own encoded word. Added because DrawCapturedCommand carried only element 0's
// CpVertexFormat (vbFormat above) and NOTHING for elements 1..n, which is why
// the layout builder could see one format and refuse the rest as unaccounted.
//
// WHAT THE FETCH INSTRUCTION ACTUALLY CARRIES (ucode.h:745-777, read in full -
// there is no other field). Every field below is a bit of dword_0/1/2 named
// there; nothing here is inferred:
//   dword_0  bits  0.. 4 opcode_value : 5   (kVertexFetch == 0, ucode.h:531-532)
//            bits  5..10 src_reg : 6
//            bit      11 src_reg_am : 1
//            bits 12..17 dst_reg : 6       <-- accessors dest() /ucode.h:709
//            bit      18 dst_reg_am : 1
//            bit      19 must_be_one : 1
//            bits 20..24 const_index : 5
//            bits 25..26 const_index_sel : 2
//            bits 27..29 prefetch_count : 3  (value is count-1)
//            bits 30..31 src_swiz : 2
//   dword_1  bits  0..11 dst_swiz : 12      <-- accessor dest_swizzle() :710
//            bit      12 fomat_comp_all : 1  [sic] accessor is_signed() :724
//            bit      13 num_format_all : 1  accessor is_normalized() :725
//            bit      14 signed_rf_mode_all : 1  accessor signed_rf_mode() :726
//            bit      15 is_index_rounded : 1  accessor is_index_rounded() :736
//            bits 16..21 format : 6         <-- accessor data_format() :721
//            bits 22..23 reserved2 : 2
//            bits 24..29 exp_adjust : 6, signed [-32,31]  accessor :723
//            bit      30 is_mini_fetch : 1  accessor is_mini_fetch() :719
//            bit      31 is_predicated : 1  accessor is_predicated() :699
//   dword_2  bits  0.. 7 stride : 8         <-- accessor stride() :740, DWORDS
//            bits  8..30 offset : 23 signed <-- accessor offset() :742, DWORDS
//            bit      31 pred_condition : 1  accessor predicate_condition() :701
//
// dst_reg IS NOT THE SEMANTIC, and this struct does not pretend otherwise.
// It is the DESTINATION REGISTER the fetched components are written to
// (ucode.h:709 `dest()`); there is NO element-index field anywhere in the
// instruction, so `elementIndex` below is the fetch's ORDINAL POSITION in the
// program and nothing more. Turning an ordinal into a POSITION/NORMAL/... name
// needs the program parser, not a bit read (see src/renderer/xenos_shader_ir.cpp
// and the reference's own route via SQ_INTERPOLATOR_CNTL, pipeline_cache.cc:882
// and :915). Nothing in this struct invents one.
//
// `fmt` is the SAME CpVertexFormat the first element already carried, so the
// per-element format/byte-size lookup reuses the ONE table (exported below as
// mcla::gpu::CpXenosFormatToDxgi / CpXenosFormatBytes) rather than adding a
// second decoder for one register.
//
// F-751: `endianness` IS THE FOURTH THING THE FETCH CONSTANT CARRIES, and until
// now it was decoded, printed and dropped. It is the 2-bit `Endian endian` at
// +0 of dword_1 of `xe_gpu_vertex_fetch_t`
// (.research/xenia/src/xenia/gpu/xenos.h:1122, union at :1113-1126), read by the
// producer's own working decode at src/gpu_cp.cpp:1705 (and :1735, the
// by-index reader) as `d1 & 0x3u`, carried onto CpVertexDescriptor at :1843 and
// PRINTED on every CP-FETCHVB-DESC row - and then discarded at the enqueue,
// because CpVertexElement had nowhere to put it. MEASURED on the live stream
// (build/w708.log): all 8 CP-FETCHVB-DECODE rows and all 6 CP-FETCHVB-DESC rows
// carry endian=2. So the selector is not an edge case; it is what every fetch
// constant in this boot says.
//
// WHY IT IS HERE AND NOT ON CpVertexFormat. `fmt` describes the ELEMENT (the 6-bit
// data_format of the vfetch instruction); `endianness` describes the SLOT - the
// fetch CONSTANT the instruction bound to, which is a different register
// (SHADER_CONSTANT_FETCH_*) written by PM4_IM_LOAD rather than by the vfetch
// itself. All four of CpVertexFormat's fields are element facts and all four are
// spoken for, so putting a slot fact there would misfile it. Per-ELEMENT is
// still the right home because two vfetch instructions can bind two different
// slots, and the reference reads the selector per instruction
// (shader_interpreter.cc:1048-1049 takes `fetch_constant.endian` from the slot
// that instruction resolved).
//
// THE DEFAULT IS 0 AND THAT IS DELIBERATE. The four selectors are an architected
// 2-bit enum, so 0 is `kNone` - "no swap" - and it is also what a zero-initialised
// or never-filled field holds. Those two meanings COINCIDE, and both mean the
// same operation: leave the dword alone. That is what makes a missing field
// harmless in the only direction that is safe: an unfilled selector can never
// silently become a swap. It is NOT a licence to substitute a default for a
// selector the guest actually set - a real non-zero value is carried verbatim and
// applied verbatim, and a value outside 0..3 (unreachable from a 2-bit field, so
// only a producer bug) is REFUSED BY NAME by the consumer rather than rounded,
// masked or clamped into one of the four.
//
// Provenance of .research/xenia: it has NO .git, is gitignored (.gitignore:14)
// and has 0 files in HEAD - an untracked snapshot with NO recorded provenance.
// Its xenos.h was read at SHA-256
// EF00180E1B69A6233AD07161BDD42A9A623CB2EE88C732CE7FE69C611DFD6389.
enum CpVertexEndian : uint32_t {
    // xenos.h:194  Endian::kNone. No swap. ALSO the "field not filled" value,
    // because 0 is both - see the note above. Never a swap.
    kCpVertexEndianNone = 0u,
    // xenos.h:195  Endian::k8in16. Swap bytes within each 16-bit half word.
    kCpVertexEndian8in16 = 1u,
    // xenos.h:196  Endian::k8in32. Swap ALL FOUR bytes. The value this boot
    // actually carries (build/w708.log, every CP-FETCHVB-DESC row).
    kCpVertexEndian8in32 = 2u,
    // xenos.h:197  Endian::k16in32. Swap the two 16-bit half words.
    kCpVertexEndian16in32 = 3u,
};

// True for the four architected selectors (xenos.h:193-198, and the 2-bit field
// width at :1122). Anything else is refused by the consumer, never rounded.
constexpr bool CpVertexEndianIsArchitected(uint32_t e) { return e <= 3u; }

struct CpVertexElement {
    // Ordinal of this fetch among the program's vertex fetches, in program
    // order. POSITIONAL, NOT DECODED: VertexFetchInstruction has no element
    // index field (see above), so this is `made` at the producer.
    uint32_t elementIndex;
    uint32_t dstReg;       // dword_0[12..17]; dest(), ucode.h:709
    uint32_t dstRegAm;     // dword_0[18]; is_dest_relative(), ucode.h:711
    uint32_t srcReg;       // dword_0[5..10]; src(), ucode.h:714
    uint32_t srcRegAm;     // dword_0[11]; is_src_relative(), ucode.h:716
    uint32_t srcSwizzle;   // dword_0[30..31]; src_swizzle(), 2 bits
    uint32_t dstSwizzle;   // dword_1[0..11]; dest_swizzle(), ucode.h:710
    uint32_t slotIndex;    // const_index*3 + const_index_sel, [0,95]
    uint32_t strideBytes;  // stride() * 4; DWORDS (ucode.h:740) scaled ONCE here
    int32_t  offsetBytes;  // offset() * 4, sign-extended (ucode.h:742)
    uint32_t miniFetch;    // dword_1[30]; 1 = vfetch_mini
    uint32_t predicated;   // dword_1[31]
    uint32_t conditional;  // 1 = the enclosing CF was kCondExec* (may not run)
    uint32_t predCondition;// dword_2[31]; the comparison's required value
    uint32_t indexRounded; // dword_1[15]
    CpVertexFormat fmt;    // xenos / dxgi / bytes / known for THIS element
    // F-751: the fetch constant's byte-order selector, carried so the consumer
    // can APPLY the guest's own transform instead of rasterising a byte-reversed
    // stream. xenos.h:1122 (`Endian endian : 2` at +0 of dword_1); values are
    // CpVertexEndian above and are the raw 2 bits, NOT a normalised enum.
    //
    // 0 MEANS BOTH "kNone" AND "NOT FILLED", AND BOTH MEAN NO SWAP - see the
    // CpVertexEndian note above for why that coincidence is the safe direction.
    uint32_t endianness = kCpVertexEndianNone;
};

// F-652: the shader program a draw selects, as a CHECKED VALUE.
//
// HOW THE GUEST SELECTS A PROGRAM - established from the reference, not assumed,
// and it is NOT what the field name used to suggest:
//   * There is no program-control register holding an index or an address.
//     SQ_PROGRAM_CNTL (register 0x2180, register_table.inc:523) is REGISTER
//     STATE, not a program pointer: registers.h:127-150 decodes it as
//     vs_num_reg/ps_num_reg (GPR counts minus 1), vs/ps_export_mode,
//     vs_export_count, param_gen, gen_index_pix/vtx. It says how MANY registers
//     the program uses, never WHICH program.
//   * The selection is the packet PM4_IM_LOAD (opcode 0x27,
//     xenos.h:1611) and its embedded sibling PM4_IM_LOAD_IMMEDIATE (0x2b,
//     xenos.h:1612). ExecutePacketType3_IM_LOAD
//     (.research/xenia/src/xenia/gpu/command_processor.cc:1528-1557) reads:
//
//       addr_type  = ReadAndSwap<uint32_t>();   // :1534
//       shader_type = addr_type & 0x3;          // :1535  ShaderType, xenos.h:33-36
//       addr        = addr_type & ~0x3;        // :1536  A GUEST ADDRESS
//       start_size  = ReadAndSwap<uint32_t>();  // :1537
//       start       = start_size >> 16;        // :1538  upstream asserts == 0
//       size_dwords = start_size & 0xFFFF;     // :1539  program LENGTH in dwords
//
//     and then binds the stage (:1545-1555) by storing the program as
//     active_vertex_shader_ / active_pixel_shader_. So the low TWO BITS are the
//     stage tag and the REST of the same dword IS the program address. The
//     program bytes live in guest memory at that address. There is no
//     index-to-address table anywhere, so there was nothing to guess wrong.
//   * PM4_IM_LOAD_IMMEDIATE (command_processor.cc:1559-1590) carries the body
//     INSIDE the packet at the reader's own pointer (:1575-1576). That program
//     has NO guest address at all, which is why `embedded` is a first-class
//     field and not folded into a zero address.
//
// WHY IT IS SCALARS AND NOT AN INDEX (F-609: no fakes): four stored scalars, so
// there is no lookup that can go out of range, and `present == 0` is a REFUSAL
// the consumer must honour rather than a zero it may mistake for "program at
// address 0" (which is a readable guest page - the cheapest possible plausible-
// but-wrong shader). addrSpace says WHICH form of the address was verified
// readable, because the reference reads the program through TranslatePhysical
// while this tree publishes the flat guest window through PhysToKernelVA
// (src/gpu_cp.cpp:216-217), so both forms are probed and the winner is reported
// instead of being normalised away.
enum CpShaderAddrSpace : uint32_t {
    kCpShaderAddrAbsent = 0u,  // no PM4_IM_LOAD has ever delivered this stage
    kCpShaderAddrGuest  = 1u,  // `guestAddr` is the address the packet carried
    kCpShaderAddrKernel = 2u,  // `guestAddr` is its kernel-VA alias (PA|0xC0000000)
};

// F-694: the program BODY, carried inline so the consumer can translate the
// program instead of re-deriving it. Nothing about the F-652 decode changes
// here; this only adds the bytes the decode was already describing.
//
// `dwords` IS THE LENGTH. `body` IS THE DATA. They are deliberately different
// names, and the split is not cosmetic: the producer's own mirror of this
// record, gpu_cp.cpp's CpMicrocodeImage, HAS a field called `dwords` and THAT
// one is the body (gpu_cp.cpp:419). The two names are already in one reader's
// path, and the log makes it worse: CP-DRAW-BRIDGE prints this struct's
// `dwords` as the `{}d` tail of `vsProg={:08X}/sp{}/{}d` (gpu_cp.cpp:3753,
// fed by dc.vsProgram.dwords at :3761), so a measured row reads
// `vsProg=00000000/sp1/27d` and 27 there is a COUNT of dwords, never 27 dwords
// of data. So: `dwords` stays a count and is NOT repurposed; the body gets its
// own name.
//
// WHY AN INLINE ARRAY AND NOT A POINTER: CpShaderProgram is copied BY VALUE
// three times per queued draw - into CommandQueue::push's by-value `cmd`
// parameter, into the queue's element storage in `queue_.push`, and back out
// of it in `cmd = std::move(queue_.front())` in pop() - and it is trivially
// copyable, so each of those is a memcpy of the WHOLE struct. A pointer would
// dangle the moment the producer's g_microcode image was overwritten by the
// next PM4_IM_LOAD for that stage, and the failure mode would be a shader
// silently translated from a DIFFERENT shader. The array is the price of not
// having that, and it is a price worth paying only if it is small: see below.
// (Those three sites are named, not numbered, on purpose: this comment lives in
// the same file, so any edit to it - including this one - moves the lines it
// would have cited. Cross-file `file:line` cites stay valid; self-cites rot.)
//
// SIZE, AND WHAT WOULD FALSIFY IT: kCpShaderBodyDwords == 256 dwords == 1 KiB
// per stage, 2 KiB per queued draw.
//   - MEASURED PAYLOAD. 24 CP-IM-LOAD rows across three soaks - w681, w682 and
//     w683, 8 rows each - declare captured = 9, 15, 24, 27 (max 27 dwords =
//     108 B), every row op=0x2B, every row truncated=0. 256 is 9.5x the
//     measured maximum.
//   - THAT MAXIMUM IS NOT AN UPPER BOUND AND MUST NOT BE READ AS ONE. 27 is the
//     maximum of 24 rows from one boot phase, and that phase has not loaded a
//     shading program yet: the same soaks show 6 real draws reaching the render
//     thread and 10 `no-translated-pso` refusals (the literal is live at
//     render_thread.cpp:1343, fired 10x in w682.log), i.e. the translator has
//     never yet been handed one of these bodies. What the 9..27 dword payloads
//     actually ARE is NOT measured here and does not need to be - the only
//     claim made of them is the size, which is why 27 bounds nothing.
//   - UPSTREAM'S OWN CEILING IS FAR ABOVE BOTH. size_dwords = start_size &
//     0xFFFF (command_processor.cc:1539 for PM4_IM_LOAD, :1570 for
//     PM4_IM_LOAD_IMMEDIATE) admits 65535 dwords = 256 KiB. No cap in this
//     header is "safe" against a declared-large packet, so the cap is chosen to
//     be CHEAP and the truncation is made impossible to MISS rather than
//     impossible - which is what the validity contract below is for.
//   - FALSIFIER. Any soak containing a CP-IM-LOAD row with captured >= 256, or
//     any truncated=1. That means this cap is refusing a real program, and the
//     correct response is to raise THIS CONSTANT - never to relax the check
//     that reads it, which is the only thing standing between a truncated
//     program and a wrong pixel.
//   - 1024 WAS REJECTED ON MEASUREMENT, NOT TASTE. 1024 is the producer's own
//     window (kMicrocodeCaptureDwords, gpu_cp.cpp:401) and matching it would
//     make this struct provably lossless w.r.t. the producer, so it was costed
//     rather than waved away. Costs, all measured by compiling the real header
//     and counting global operator new (the 4096-deep column is the queue's own
//     default maxCapacity_):
//
//                    CpShaderProgram  RenderCommand  memcpy/draw  4096-deep queue
//        before          28 B            208 B           624 B        983,187 B
//        N = 256      1052 B           2256 B         6,768 B      9,371,795 B
//        N = 1024     4124 B           8400 B        25,200 B     34,697,363 B
//
//     memcpy/draw is 3 x sizeof(RenderCommand): the move into push()'s
//     by-value parameter, the move into the queue's element, and the move out
//     of it in pop(). The 4096-deep column is heap, counted not estimated, and
//     its ALLOCATION COUNT is 1.003 per element at EVERY size in the table -
//     measured - so a bigger body costs bytes, not extra allocations, and the
//     only question is how many bytes. N=1024 is a 35x regression in the heap
//     footprint of the exact path whose stated design goal is "the draw path
//     allocates nothing per frame" (see the F-611 note above), paid
//     unconditionally to carry 108 bytes of measured real payload. N=256 costs
//     9.5x the BEFORE heap, buys 9.5x headroom over the measured maximum, and
//     keeps N=1024's extra 3.7x regression on the table.
//
// VALIDITY CONTRACT - who fills body, when, under what lock, and when it means
// anything. (gpu_cp.cpp line numbers are as of SHA-256 01546ce9bd682678; that
// file is under concurrent edit, so treat the SYMBOL as the citation and the
// line as the measurement. If a number below no longer resolves, the symbol
// beside it is what to grep.)
//   * FILLED BY the producer, in CpSnapshotDrawState (gpu_cp.cpp:3083), from
//     the stage's g_microcode[stage] image (gpu_cp.cpp:422), INSIDE that
//     function's g_microcodeMutex critical section (gpu_cp.cpp:3102) - the same
//     lock that guards the image store at gpu_cp.cpp:1752-1753. The source is
//     img.dwords[] (gpu_cp.cpp:419), which CpExecImLoad filled with
//     mem.ReadU32BE (gpu_cp.cpp:1741). IMPLEMENTED, not proposed: the copy is
//     gpu_cp.cpp:3133-3137, it writes exactly min(img.captured, cap) dwords,
//     and it leaves dst.captured UNCLAMPED (gpu_cp.cpp:3120) so the truncation
//     witness below survives the crossing. Both stages are copied under ONE
//     lock hold, so a draw cannot pair one stage's body with the other stage's
//     scalars.
//   * WHEN: once per DRAW_CAPTURED, at the enqueue site, on the guest CP thread
//     that executed the PM4_IM_LOAD. It is a SNAPSHOT, not a live view; the
//     image is overwritten by the next PM4_IM_LOAD for that stage.
//   * VALID iff present != 0. CpExecImLoad captures both pointer and immediate
//     forms with ReadU32BE; CpSnapshotDrawState copies those dwords into body[].
//     `embedded` identifies the packet form, not whether body[] is populated.
//     `guestAddr` is the packet address and may be physical even when addrSpace
//     records that the producer had to read through a kernel-VA alias. Consumers
//     must use this captured body rather than dereference guestAddr directly.
//     When present == 0 the record is the zero-initialised `vs = {}` / `ps = {}`
//     at gpu_cp.cpp:3086-3087. That value-initialisation also keeps the TAIL
//     honest: it zeroes the whole body, so a shorter re-upload cannot ship the
//     previous longer program's leftover words past `captured`. Remove the
//     `= {}` and this array becomes a stale-tail source - the producer says the
//     same at gpu_cp.cpp:3145-3157, and that is the falsifier for THIS field.
//   * HOW MANY ARE MEANINGFUL: min(captured, kCpShaderBodyDwords). A consumer
//     MUST test captured <= kCpShaderBodyDwords before reading body, because
//     `captured` is the PRODUCER'S retained count and CAN legitimately exceed
//     this array. If it does, the program was truncated crossing the queue and
//     the honest response is to refuse the draw - the same shape as
//     render_thread.cpp's existing vb-fmt-unmapped refusal. A silently
//     truncated program is the single outcome this design exists to prevent.
//   * BYTE ORDER: logical dwords in HOST order, the same convention as
//     CpRenderStateSnapshot::regs below - mem.ReadU32BE big-endian-READS the
//     guest dword, so an instruction word's bits already arrive in the order a
//     translator wants them and no swap belongs at the point of use.
constexpr uint32_t kCpShaderBodyDwords = 256u;

struct CpShaderProgram {
    uint32_t present;     // 1 = a PM4_IM_LOAD delivered THIS stage
    uint32_t addrSpace;   // CpShaderAddrSpace; kCpShaderAddrAbsent when !present
    uint32_t guestAddr;   // addr_type & ~0x3; meaningless unless present
    uint32_t dwords;      // size_dwords, the program length the packet declared
                          // F-694: this is a COUNT, never the data. The body is
                          // `body[]` below; the producer's identically-named
                          // CpMicrocodeImage::dwords IS the body, which is the
                          // confusion this split exists to end.
    uint32_t embedded;    // 1 = PM4_IM_LOAD_IMMEDIATE: body in the packet, so
                          // guestAddr == 0 BY CONSTRUCTION and is not an address
    uint32_t captured;    // dwords actually retained by the producer; may EXCEED
                          // kCpShaderBodyDwords, which is the truncation witness
    uint32_t _pad;
    // F-694: the program bytes, host-order dwords, at most kCpShaderBodyDwords
    // of them meaningful. Inline by value, never a pointer: this struct is
    // memcpy'd into the queue three times per draw and a pointer into
    // g_microcode would dangle on the next PM4_IM_LOAD. Read the validity
    // contract in the comment above before indexing it.
    uint32_t body[kCpShaderBodyDwords];
};

// F-652: the rasteriser/clipper/viewport/bias registers this draw is judged on,
// SNAPSHOTTED AT THE MOMENT THE DRAW WAS ENQUEUED. Not read by the consumer
// later: CpReadGuestRegisters answers "has this register EVER been written",
// which is a different fact from "was it written when this draw was submitted",
// and the whole point of renderer::PipelineStateInputs::registersPresent
// (renderer/pipeline_cache.h:405) is that distinction.
//
// `presentMask` is bit `i` set == `regs[i]` is the guest's value. A clear bit is
// a refusal with its own name downstream (kRefuseNoPaClipCntl and friends), not
// a zero to decode: PA_SU_SC_MODE_CNTL = 0 is a LEGAL, DIFFERENT rasteriser
// (solid triangles, both faces, CCW front).
//
// Byte order: `regs[]` holds the LOGICAL register dword, in host order - the
// ring path hands CpType0Apply a big-endian-READ value (mem.ReadU32BE,
// src/gpu_cp.cpp:567-575) which is byte-for-byte what the reference's
// ReadAndSwap<uint32_t> produces (command_processor.cc:1534), so a kFloat
// register's bits reinterpret directly as a host float and NO byte swap belongs
// at the point of use. Stated here because "the guest is big-endian" is exactly
// the kind of generalisation that would add a spurious swap here.
enum CpSnapshotReg : uint32_t {
    kCpSnapPaClipCntl         = 0,  // 0x2204 PA_CL_CLIP_CNTL       (dword)
    kCpSnapPaSuScModeCntl     = 1,  // 0x2205 PA_SU_SC_MODE_CNTL    (dword)
    kCpSnapPaClVteCntl        = 2,  // 0x2206 PA_CL_VTE_CNTL        (dword)
    kCpSnapPolyOffFrontScale  = 3,  // 0x2380 PA_SU_POLY_OFFSET_FRONT_SCALE  (kFloat)
    kCpSnapPolyOffFrontOffset = 4,  // 0x2381 PA_SU_POLY_OFFSET_FRONT_OFFSET (kFloat)
    kCpSnapPolyOffBackScale   = 5,  // 0x2382 PA_SU_POLY_OFFSET_BACK_SCALE   (kFloat)
    kCpSnapPolyOffBackOffset  = 6,  // 0x2383 PA_SU_POLY_OFFSET_BACK_OFFSET  (kFloat)
    kCpSnapDepthInfo          = 7,  // 0x2002 RB_DEPTH_INFO, for depth_format only
    // F-723: 0x2001 RB_COLOR_INFO, for color_format ONLY (4 bits at +16). This
    // is the register the PSO's RTVFormats come from; without it
    // PipelineStateInputs::colorFormats[] is all zeroes, DerivePipelineState
    // copies those zeroes straight through (pipeline_cache.cpp:841-842), and the
    // worker's own `numRt == 0` gate refuses the key as `empty-hlsl`
    // (pipeline_cache.cpp:1730-1747, "key names no render target format").
    //
    // WHY IT IS NOT A VALUE CHECK. A slot is meaningful when the guest PRESENTED
    // the register, never when the value is non-zero. The measured reason is
    // concrete: ColorRenderTargetFormat 0 is k_8_8_8_8 (xenos.h:299-301), a
    // perfectly legal colour target, so "color_format == 0" and "the guest never
    // wrote RB_COLOR_INFO" are indistinguishable without the presence bit and the
    // second reading is the plausible-wrong-render-target this project exists to
    // prevent. The same discipline as kCpSnapDepthInfo above.
    //
    // ONLY RT0. RB_COLOR1/2/3_INFO (0x2003/0x2004/0x2005, registers.h:750-751)
    // are the same layout and are NOT captured; render_thread.cpp refuses rather
    // than filling slots 1..3 from a register this snapshot does not carry.
    kCpSnapColorInfo          = 8,  // 0x2001 RB_COLOR_INFO, for color_format only
    // F-904: RT0 blend control + write mask. The blend-off baseline
    // (kStateFlagUnmeasuredBlend) overwrites transparent texels instead of
    // compositing them (measured black screen, F-901); these two registers
    // are the measured source. RB_BLENDCONTROL0 layout: color_srcblend 5b+0,
    // color_comb_fcn 3b+5, color_destblend 5b+8, alpha_srcblend 5b+16,
    // alpha_comb_fcn 3b+21, alpha_destblend 5b+24 (registers.h:783-791).
    // RB_COLOR_MASK: per-channel write enables, RT0 in bits 0-3
    // (registers.h:758-761). RT1-3 blend controls (0x2209/0x220A/0x220B) are
    // NOT carried: the backend binds one RTV (d3d12_backend.cpp:4083), same
    // RT0-only discipline as color info above.
    kCpSnapBlendControl0      = 9,  // 0x2201 RB_BLENDCONTROL0 (dword)
    kCpSnapColorMask          = 10, // 0x2104 RB_COLOR_MASK (dword)
    kCpSnapDepthControl       = 11, // 0x2200 RB_DEPTHCONTROL (dword)
    kCpSnapRegCount           = 12,
};

struct CpRenderStateSnapshot {
    uint32_t presentMask;             // bit kCpSnapX set == regs[kCpSnapX] is the guest's
    uint32_t regs[kCpSnapRegCount];   // logical dwords, host order
};

struct DrawCapturedCommand {
    uint32_t vbBase;       // guest VA of the vertex stream; 0 = none known
    uint32_t vbSize;       // bytes
    uint32_t vbStride;     // bytes per vertex
    // F-648: the decoded vertex format, carried (see CpVertexFormat above).
    CpVertexFormat vbFormat;
    // F-648: how many vfetch descriptors the program really had, so a reader of
    // this command can tell a single-stream program (the measured case) from a
    // multi-descriptor one. The producer REFUSES nFullFull > 1 rather than
    // picking one stream silently, so a command that exists always carries the
    // whole of what the program described.
    uint32_t vbDescCount;  // descriptors decoded (nDesc)
    uint32_t vbFullCount;  // of those, non-mini (nFull)
    // F-649: EVERY fetch the program declared, not just element 0. Before this
    // the command carried one (vbBase, vbStride, vbFormat) triple and nothing
    // else, so a second fetch's format and destination register were decoded at
    // the producer and then DISCARDED - the guest's own declarations stopped at
    // the queue boundary. `vbFormat` above is retained UNCHANGED as element 0's
    // summary, so every existing reader keeps working and no existing count
    // moves.
    //
    // MEASURED on the live stream (build/w689.log, both draws): element 0 is
    // the vfetch_full and element 1, where present, is its vfetch_mini. Order
    // is program order, so elementIndex is a stable key.
    //
    // BOUND: vbElementCount <= kCpVertexElementMax always, and the producer
    // refuses `vbFullCount > 1` (vfetch-multi) before this is ever filled, so a
    // non-zero count with a full count above 1 cannot be observed.
    uint32_t vbElementCount;
    CpVertexElement vbElements[kCpVertexElementMax];
    struct CpVertexStreamBinding {
        uint32_t vbBase = 0;
        uint32_t vbSize = 0;
        uint32_t vbStride = 0;
        uint32_t slotIndex = 0;
    };
    uint32_t vbStreamCount = 0;
    CpVertexStreamBinding vbStreams[4] = {};
    uint32_t ibBase;       // guest VA of the index stream; 0 = none (kAutoIndex)
    uint32_t ibSize;       // bytes (VGT_DMA_SIZE.num_words * index element size)
    uint32_t frameId;
    // F-610: index element format, same encoding as DrawIndexedCommand::ibFormat
    // (0 = 16-bit, bit0 set = 32-bit). Added so this command is self-describing:
    // the handler used to hardcode R16_UINT + ibSize/2 because the format had
    // nowhere to live, which silently mis-drew any 32-bit index buffer.
    // F-611: this IS VGT_DRAW_INITIATOR.index_size (registers.h:324), so the
    // two names are the same field and are deliberately not stored twice.
    uint32_t ibFmt;
    uint32_t ibSwap;             // F-611: VGT_DMA_SIZE.swap_mode
    uint32_t numIndices;         // F-611: VGT_DRAW_INITIATOR.num_indices
    uint32_t idxOffset;          // F-611: VGT_INDX_OFFSET (register 0x2102)
    uint32_t primitiveTopology;  // F-611: VGT_DRAW_INITIATOR.prim_type
    uint32_t sourceSelect;       // F-611: VGT_DRAW_INITIATOR.source_select
    uint32_t cpSeq;              // F-611: CP submission counter, monotonic
    uint32_t opcode;             // F-611: 0x22 DRAW_INDX / 0x36 DRAW_INDX_2
    uint32_t initiator;          // F-611: raw VGT_DRAW_INITIATOR
    // F-610/F-611: psoHash is 0 and is NOT a PSO handle. A PM4 draw names its
    // shader through SQ_PROGRAM_CNTL + the microcode the PM4_IM_LOAD packets
    // carry, and src/gpu_cp.cpp's CpExecImLoad reads those payloads and
    // discards them, so there is no program to hash yet. The consumer
    // therefore calls DrawDynamicMesh, never DrawDynamicMeshWithPipeline, and
    // the field is kept only so the command stays self-describing.
    //
    // F-652 SUPERSEDES the mechanism named in the paragraph above, which is
    // wrong: SQ_PROGRAM_CNTL does NOT name the program (registers.h:127-150 -
    // it is GPR counts and export modes). PM4_IM_LOAD does, and it hands over an
    // ADDRESS, not bytes, so the identity a draw needs is the address - which is
    // what vsProgram/psProgram below carry. psoHash stays 0 because an ADDRESS
    // is not a compiled-PipelineKey: hashing it would invent a key component no
    // other producer agrees on.
    uint64_t psoHash;
    // F-652: the two programs this draw selects, carried as checked values (see
    // CpShaderProgram for the PM4_IM_LOAD decode and for why there is no
    // index-to-address mapping to get wrong). A stage with present == 0 is a
    // REFUSAL the consumer must honour, never guestAddr == 0.
    CpShaderProgram vsProgram;
    CpShaderProgram psProgram;
    // F-652: the raster/clip/viewport/bias registers, snapshotted at enqueue (see
    // CpRenderStateSnapshot). Fixed-size POD on the queue - no allocation, and
    // the consumer never re-reads a register file that may have moved on.
    CpRenderStateSnapshot rasterState;
    // F-899 (2026-10-27): the guest's TEXTURE fetch descriptors, snapshotted at
    // enqueue from the fetch-constant window (registers 0x4800..0x48BF, 32
    // slots x 6 dwords). A slot is a texture descriptor when its dword_0 type
    // field (bits 1:0) == kTexture (2). Each descriptor is 24 bytes stored
    // BIG-ENDIAN - the exact byte layout DecodeGuestTextureDescriptor consumes
    // (renderer/texture_decode.cpp:1185) - so the consumer hands them straight
    // to BindGuestTextures with no re-encode. textureCount is the number of
    // kTexture slots found (0..kCpMaxTextureDescriptors); slot i of the window
    // maps to textureDescriptorBytes[i]. Bound at 16; an over-bound window is
    // refused by the producer (g_textureDescOverBind), never truncated.
    uint32_t textureCount;
    uint8_t textureDescriptorBytes[kCpMaxTextureDescriptors][24];
    // F-899 (2026-10-27): the pool VA of each carried texture's tiled payload,
    // translated at enqueue time by CpBridgeDraw via CpPhysToPoolVa (the F-640
    // A-view rule). The consumer reads guest memory through this VA; storing
    // it here avoids re-translating in the render thread (where the helper is
    // not declared) and keeps the descriptor bytes (format/dims) separate from
    // the address (location).
    uint32_t texturePoolVa[kCpMaxTextureDescriptors];
};

// F-605: pipe-proof marker. The CP auto-draw class (op36/src2, prim 1/8,
// 1-3 implicit indices) carries no vertex data and the register file holds
// none, so no honest geometry exists yet. This command carries only the
// submission identity; the handler draws a small synthetic quad positioned
// by seq. It proves CP -> queue -> backend -> screen flows. SYNTHETIC BY
// DESIGN: retire it when real VB capture (SET_CONSTANT/IM_LOAD) lands.
struct DrawMarkerCommand {
    uint32_t seq;
    uint32_t prim;
    uint32_t numIdx;
};

struct ResizeCommand {
    uint32_t width;
    uint32_t height;
};

struct RenderCommand {
    enum Type : uint8_t {
        DRAW_INDEXED,
        SET_PIPELINE_STATE,
        SET_VERTEX_BUFFERS,
        SET_INDEX_BUFFER,
        PRESENT,
        SET_RENDER_STATE,
        NOOP,
        INIT_D3D12,
        DRAW_CAPTURED,
        DRAW_MARKER,
        RESIZE,
    };
    
    Type type;
    std::variant<
        DrawIndexedCommand,
        SetPipelineStateCommand,
        SetVertexBuffersCommand,
        SetIndexBufferCommand,
        PresentCommand,
        SetRenderStateCommand,
        InitD3D12Command,
        DrawCapturedCommand,
        DrawMarkerCommand,
        ResizeCommand
    > data;
};

// F-610: the heartbeat PRESENT is exactly {frameNumber=0, obj=0, swapInfo=0}
// (src/render_thread.cpp:117-122), so it is told apart from a guest
// PresentKick by value, never by guessing. This is the single definition, so
// the shed policy, the consumer drop policy and the PRESENT log label all
// read it, so they cannot drift apart.
inline bool IsHeartbeatPresent(const RenderCommand& c) {
    if (c.type != RenderCommand::PRESENT) return false;
    const auto& p = std::get<PresentCommand>(c.data);
    return p.frameNumber == 0 && p.obj == 0 && p.swapInfo == 0;
}

// F-610: commands that may be shed once the consumer is at least
// kShedDepth behind. This list is the pre-existing consumer drop list PLUS the
// two types it was missing (DRAW_MARKER and the heartbeat PRESENT), so
// producer and consumer shed the same set at the same depth instead of the
// producer filling a queue the consumer then refuses to drain. DRAW_INDEXED
// stays in the list exactly as before: it was already
// being dropped at depth 60 consumer-side, and moving the decision to the
// producer is strictly earlier and now counted. NOT sheddable: guest PRESENT,
// INIT_D3D12, RESIZE, DRAW_CAPTURED: device state, swap target, or a real
// guest frame, none of which may be discarded on a guess.
//
// F-611: DRAW_CAPTURED stays non-sheddable (it is now a real producer-backed
// command, so dropping it would lose guest draw intent, not a liveness ping),
// and the flood risk that classification implies is bounded at the SOURCE
// instead: src/gpu_cp.cpp's CpBridgeDraw only enqueues a draw that already
// carries a non-empty, in-guest-window index buffer, so the command cannot be
// produced by the degenerate submissions (kAutoIndex with no index buffer at
// all, which is 100% of the measured boot traffic) that would otherwise fill
// the queue. QUEUE-SHED's evictDurable counter would expose it if that ever
// stopped being true.
inline bool IsSheddable(const RenderCommand& c) {
    switch (c.type) {
        case RenderCommand::DRAW_INDEXED:
        case RenderCommand::DRAW_MARKER:
        case RenderCommand::SET_RENDER_STATE:
        case RenderCommand::SET_PIPELINE_STATE:
        case RenderCommand::SET_VERTEX_BUFFERS:
        case RenderCommand::SET_INDEX_BUFFER:
        case RenderCommand::NOOP:
            return true;
        case RenderCommand::PRESENT:
            return IsHeartbeatPresent(c);
        default:
            return false;
    }
}

// F-610: shed accounting. push() is non-blocking, so every refusal and every
// eviction must be countable. A silent drop is indistinguishable from a
// guest that stopped drawing. The render thread drains these into a bounded
// QUEUE-SHED log line (src/render_thread.cpp).
struct ShedCounters {
    uint64_t refusedSheddable = 0;  // producer refused a sheddable at depth
    uint64_t evictedSheddable = 0;  // drop-oldest removed a sheddable entry
    uint64_t evictedDurable = 0;    // drop-oldest removed guest state/pixels
    uint64_t consumerDropped = 0;   // render thread dropped on dequeue
    uint64_t depth() const {
        return refusedSheddable + evictedSheddable + evictedDurable +
               consumerDropped;
    }
};

class CommandQueue {
public:
    // F-610: the producer-side shed threshold. 60 == the consumer's
    // kQueueDepthDrop (src/render_thread.cpp:21): past this depth the render
    // thread is already refusing work, so enqueueing more of the same work
    // only deepens the backlog.
    static constexpr size_t kShedDepth = 60;
    static constexpr uint32_t kFullWaitMs = 250;

    explicit CommandQueue(size_t maxCapacity = 4096) 
        : maxCapacity_(maxCapacity), shutdown_(false) {}
    
    ~CommandQueue() {
        shutdown();
    }
    
    // F-610: enqueue that a stalled consumer cannot hang. This used to wait on notFull_
    // when the queue reached maxCapacity_, which parked the *guest* thread
    // inside push() and hung the game whenever the backend stalled (BeginFrame
    // failure, device lost, vsync backlog). The producers are the guest CP
    // thread (src/gpu_cp.cpp:759), a guest PPC hook
    // (src/capture_hooks.cpp:405), the GPU submit path
    // (src/gpu_device.cpp:3171) and the guest present kick
    // (src/native_renderer.cpp:150). A stall in the consumer must never be
    // able to stop the guest from running. Three tiers, all counted:
    //   depth <  kShedDepth        -> enqueue. Queue has room; nothing lost.
    //   depth >= kShedDepth, and
    //     the command is sheddable -> REFUSE it. It carries no pixels, and the
    //     next guest draw re-emits the same information.
    //   depth >= maxCapacity_      -> wait up to kFullWaitMs, then EVICT THE OLDEST
    //     entry to make room and enqueue. Last-resort backstop, and the only path that can cost guest
    //     content. Dropping the oldest (not the newest) is the standard policy:
    //     it sheds latency rather than freshness, and it preserves the relative
    //     order of everything still queued. A stall that reaches this depth has
    //     already lost the frame, so a hang is the worse outcome by far.
    void push(RenderCommand cmd) {
        // Classify BEFORE taking the lock: IsSheddable only reads the command,
        // and doing it here keeps the lock section to the queue itself.
        const bool sheddable = IsSheddable(cmd);
        std::unique_lock lock(mutex_);
        if (shutdown_) return;

        if (queue_.size() >= kShedDepth && sheddable) {
            refusedSheddable_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // A UI frame is ~1,500 one-glyph draws, so a live consumer that is merely
        // behind reaches capacity; evicting then deleted glyphs from the picture.
        // Wait for it, but keep F-610's guarantee: a consumer that makes no room
        // within kFullWaitMs is stalled, and nothing waits until it pops again.
        if (queue_.size() >= maxCapacity_ && !consumerStalled_) {
            consumerStalled_ = !notFull_.wait_for(lock, std::chrono::milliseconds(kFullWaitMs), [this] {
                return queue_.size() < maxCapacity_ || shutdown_;
            });
            if (shutdown_) return;
        }
        if (queue_.size() >= maxCapacity_ && !queue_.empty()) {
            // A non-empty queue at capacity is the only case that reaches here
            // (a default-constructed queue with maxCapacity_ == 0 is guarded by
            // the !queue_.empty() so it can neither evict from empty nor spin).
            if (IsSheddable(queue_.front())) {
                evictedSheddable_.fetch_add(1, std::memory_order_relaxed);
            } else {
                evictedDurable_.fetch_add(1, std::memory_order_relaxed);
            }
            queue_.pop();
        }
        queue_.push(std::move(cmd));
        notEmpty_.notify_one();
    }
    
    bool pop(RenderCommand& cmd) {
        std::unique_lock lock(mutex_);
        notEmpty_.wait(lock, [this] { 
            return !queue_.empty() || shutdown_; 
        });
        
        if (shutdown_ && queue_.empty()) return false;
        
        cmd = std::move(queue_.front());
        queue_.pop();
        consumerStalled_ = false;
        notFull_.notify_one();
        return true;
    }
    
    void shutdown() {
        std::lock_guard lock(mutex_);
        shutdown_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }
    
    size_t size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }
    
    bool isEmpty() const {
        std::lock_guard lock(mutex_);
        return queue_.empty();
    }

    // F-610: lock-free snapshot for the render thread's QUEUE-SHED line.
    ShedCounters Shed() const {
        ShedCounters s;
        s.refusedSheddable = refusedSheddable_.load(std::memory_order_relaxed);
        s.evictedSheddable = evictedSheddable_.load(std::memory_order_relaxed);
        s.evictedDurable = evictedDurable_.load(std::memory_order_relaxed);
        s.consumerDropped = consumerDropped_.load(std::memory_order_relaxed);
        return s;
    }

    // F-610: the consumer drops on dequeue, from the render thread; the
    // producer refuses on enqueue. Same accounting either way.
    void NoteConsumerDrop() {
        consumerDropped_.fetch_add(1, std::memory_order_relaxed);
    }

    void ResetShed() {
        refusedSheddable_.store(0, std::memory_order_relaxed);
        evictedSheddable_.store(0, std::memory_order_relaxed);
        evictedDurable_.store(0, std::memory_order_relaxed);
        consumerDropped_.store(0, std::memory_order_relaxed);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    bool consumerStalled_ = false;  // guarded by mutex_
    std::queue<RenderCommand> queue_;
    size_t maxCapacity_;
    std::atomic<bool> shutdown_;
    // F-610: atomics, not mutex-protected fields, so the render thread can
    // read them from its command loop without contending with push().
    std::atomic<uint64_t> refusedSheddable_{0};
    std::atomic<uint64_t> evictedSheddable_{0};
    std::atomic<uint64_t> evictedDurable_{0};
    std::atomic<uint64_t> consumerDropped_{0};
};

extern CommandQueue g_commandQueue;

} // namespace mcla::native

// ---------------------------------------------------------------------------
// F-632's vf -> DXGI table, EXPORTED.
//
// The table lived and died inside src/gpu_cp.cpp: the definitions had external
// linkage in namespace mcla::gpu but NO header declared them, so no other
// translation unit could call them without writing its own declaration first.
// That is why the layout builder could only see element 0's `dxgi` - it was
// carried on the command by the producer - and had no way to obtain the format
// for elements past the first.
//
// Declared here rather than in src/gpu_cp.h because this is the only one of the
// two headers the draw path already includes, so a consumer of
// DrawCapturedCommand gets these with no new #include.
//
// THIS IS THE ONE TABLE. Do not add a second vf -> format map anywhere: F-632
// records that this project has already shipped two hand-written format tables
// that disagreed with the installed SDK, and both times the failure was silent.
// A consumer that needs the format of an element past the first CALLS THESE.
//
// Coverage, measured against the reference enum
// (.research/xenia/src/xenia/gpu/xenos.h:602-619): 16 format ids -
//   6, 7, 16, 17, 25, 26, 31, 32, 33, 34, 35, 36, 37, 38, 57.
// That is the complete xenos::VertexFormat enum, so no guest format is
// uncovered; anything else (including kUndefined == 0) returns 0 with
// *known == false, which is a refusal and never a usable format.
namespace mcla::gpu {

// xenos::VertexFormat -> DXGI_FORMAT_*.
// `known` (optional) is set to false for any id outside the enum, and `dxgi` is
// then 0 == DXGI_FORMAT_UNKNOWN, which is NOT a legal input-layout format.
uint32_t CpXenosFormatToDxgi(uint32_t fmt, bool *known);

// TIGHT byte size of a format, 0 for kUndefined and any unnamed id. Derived
// from the reference's GetVertexFormatNeededWords (xenos.h:648-683).
uint32_t CpXenosFormatBytes(uint32_t fmt);

// Human-readable name for logs; "kUnknown" outside the enum.
const char *CpXenosFormatName(uint32_t fmt);

// ---------------------------------------------------------------------------
// F-751: the guest's own byte-order transform, transcribed ONE FOR ONE from the
// reference's `xenos::GpuSwap(uint32_t value, Endian endianness)`, which is
// .research/xenia/src/xenia/gpu/xenos.h:1052-1069:
//
//   1052  inline uint32_t GpuSwap(uint32_t value, Endian endianness) {
//   1053    switch (endianness) {
//   1054      default:
//   1055      case Endian::kNone:
//   1056        // No swap.
//   1057        return value;
//   1058      case Endian::k8in16:
//   1059        // Swap bytes in half words.
//   1060        return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
//   1061      case Endian::k8in32:
//   1062        // Swap bytes.
//   1063        // NOTE: we are likely doing two swaps here. Wasteful. Oh well.
//   1064        return xe::byte_swap(value);
//   1065      case Endian::k16in32:
//   1066        // Swap half words.
//   1067        return ((value >> 16) & 0xFFFF) | (value << 16);
//   1068    }
//   1069  }
//
// THE SELECTOR SEMANTICS ARE READ, NOT ASSUMED. The four cases above are the
// whole definition; there is no table anywhere else to consult and none is
// invented here. Note what each one is NOT: none of them is a floating-point
// operation, none of them negates, and none of them touches a sign bit. They are
// three fixed bit permutations of a 32-bit word.
//
//   k8in32 (the value this boot carries) is `xe::byte_swap(value)`, and
//   `xe::byte_swap` is XENIA_BASE_BYTE_SWAP_32 = `_byteswap_ulong` under MSVC
//   (.research/xenia/src/xenia/base/byte_order.h) - i.e. a FULL 32-bit reversal,
//   byte 0 <-> byte 3 and byte 1 <-> byte 2. It is written out longhand below
//   rather than called, so this header needs no <cstdlib> and no intrinsics; the
//   two are proven equal on every input for all four selectors by the harness in
//   %TEMP%\opencode\bytemark, which compiles the reference's own definition
//   alongside this one and compares them.
//
// WHAT IT IS NOT, AND WHY IT MATTERS: it is not a "reverse the whole buffer"
// operation, not a float negation, and not a byteswap-per-element-width chosen
// by the format. It is applied to ONE 32-bit word at a time, and the reference
// applies it to EVERY dword it reads
// (.research/xenia/src/xenia/gpu/shader_interpreter.cc:1048-1049:
// `dword_value = xenos::GpuSwap(memory_dwords[dword_address_dwords],
// fetch_constant.endian)`; the float overload at xenos.h:1071-1079 is the same
// transform seen through a union, and draw_util.cc:839-849 is the same shape).
//
// `endianness` OUTSIDE 0..3 IS NOT ROUNDED, MASKED OR CLAMPED HERE. The guest
// field is 2 bits wide (xenos.h:1122), so such a value cannot be a guest fact;
// it can only be a producer bug, and silently folding it into one of the four
// would be exactly the F-609 substitution this function exists to prevent. The
// caller refuses by name instead (CpVertexEndianIsArchitected above).
constexpr uint32_t CpGpuSwap32(uint32_t value, uint32_t endianness) {
    switch (endianness) {
        // xenos.h:1055-1057. This case also absorbs every value outside 0..3,
        // which the caller must have already refused - see the note above.
        case native::kCpVertexEndian8in16:
            // xenos.h:1058-1060. Swap bytes in half words.
            return ((value << 8) & 0xFF00FF00u) | ((value >> 8) & 0x00FF00FFu);
        case native::kCpVertexEndian8in32:
            // xenos.h:1061-1064. Swap bytes - a full 32-bit reversal, i.e. the
            // MSVC _byteswap_ulong that xe::byte_swap resolves to, written out
            // longhand so this header needs no <cstdlib> and no intrinsics. The
            // arithmetic below is byte0<->byte3 and byte1<->byte2, which is what
            // that function does; the harness in %TEMP%\opencode\bytemark proves
            // the two agree on every input for all four selectors.
            return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
                   ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
        case native::kCpVertexEndian16in32:
            // xenos.h:1065-1067. Swap half words.
            return ((value >> 16) & 0xFFFFu) | (value << 16);
        case native::kCpVertexEndianNone:
        default:
            // xenos.h:1054-1057. No swap.
            return value;
    }
}

} // namespace mcla::gpu

// ---------------------------------------------------------------------------
// AUDIT ADDENDUM (2026-11-09, file-owned worker, build prohibited).
// DrawCapturedCommand / DrawIndexedCommand layout, heap safety, and the
// kCpShaderBodyDwords truncation contract. This addendum appends only; the
// stale heap-cost table at F-694:391-394 is NOT remeasured here and must be
// re-costed with the compiler before being cited again.
//
// LAYOUT (analytically derived from the field declarations above under
// natural alignment; NOT compiler-measured in this audit because no
// build was permitted -- verify with static_asserts in a follow-up):
//   CpVertexFormat          16 B (4 x u32)
//   CpVertexElement         80 B (15 x u32 + fmt 16 B + endianness 4 B)
//   CpShaderProgram       1052 B (7 x u32 + body[256] 1024 B) -- matches the
//                           F-694 table's CpShaderProgram column; _pad at
//                           render_queue.h:465 is redundant (alignment
//                           already satisfied) but harmless.
//   CpRenderStateSnapshot    52 B (1 + 12 x u32)
//   CpVertexStreamBinding    16 B (4 x u32)
//   DrawIndexedCommand       72 B (13 x u32 + 4 x u8 + u64 + u8 + pad[7],
//                           single u64 at offset 56, no extra pad)
//   DrawCapturedCommand    3416 B (vb 12 B + vbFormat 16 B + counts +
//                           vbElements[8] 640 B + vbStreamCount + vbStreams[4]
//                           64 B + ib 12 B + ibfields 36 B + 4 B pad +
//                           psoHash u64 + vs 1052 B + ps 1052 B + raster
//                           52 B + textureCount + textureDescriptorBytes
//                           [16][24] 384 B + texturePoolVa[16] 64 B; ends
//                           8-aligned). The F-694 2256 B RenderCommand row
//                           predates F-649 (vbElements 640 B + vbStreams
//                           64 B) and F-899 (textures 448 B): 2256 + 1152 + 8
//                           variant/pad = the ~3416 above. RenderCommand
//                           (variant + Type) is therefore ~3424-3432 B, and a
//                           full 4096-deep queue holds ~13.99 MB of
//                           DrawCaptured payload alone (3416 * 4096), plus
//                           deque overhead -- ~4.6 MB more than the table
//                           claims at 9,371,795 B.
//   No struct carries a pointer, reference, or virtual. All are POD by
//   value, trivially copyable, and queued by 3x memcpy per draw
//   (push by-value, queue element, pop) = ~10.2 KB memcpy/draw at 3416 B.
//
// TRUNCATION REPRO (hazard is real; current tree is safe ONLY because of
// one consumer check):
//   1. Guest declares size_dwords = 300, reader retains captured = 300
//      (producer want = min(300, 1024) at gpu_cp.cpp:2266-2267, loop at
//      :2277-2284, img.captured = 300 at :2285).
//   2. Snapshot copies n = min(300, 256) = 256 (gpu_cp.cpp:4924-4926) and
//      keeps dst.captured = 300 unclamped (:4910) + s_shipBody.truncated++
//      (:4950-4955). body[256..299] simply never crosses the queue.
//   3. Naive consumer `for (i = 0; i < p.captured; ++i) use(p.body[i])`
//      without the guard at render_thread.cpp:1295 would read 44 dwords
//      past body[255] into rasterState/textures (44 x 4 = 176 B intra-
//      struct OOB); with dwords where captured belongs (up to 65535) it
//      walks ~256 KiB past a 1 KiB array (corruption / fault).
//   4. Actual consumer refuses: `if (p.captured == 0 || p.captured > 256)`
//      at render_thread.cpp:1295 returns "captured-out-of-range" and the
//      loop at :1306 never runs. Correct. The ONLY body readers in src/
//      are this copy (:4926, bounded by n) and this guarded loop
//      (render_thread.cpp:1306). No dwords-length body read exists.
//   5. Falsifier (unchanged): any soak row with captured >= 256 or
//      truncated=1 means raise kCpShaderBodyDwords, never relax the
//      :1295 check. Two truncation layers exist: guest->producer at
//      1024 (gpu_cp.cpp:2286-2288) and producer->queue at 256; both
//      witnesses are preserved (totalDwords/captured divergence).
//
// HEAP-SAFETY GAPS (why the verdict is not a clean PASS):
//   * textureCount: header at :630-632 says "Bound at 16; over-bound
//     refused by producer". Consumers trust it: render_thread.cpp:2882
//     reserve(dc.textureCount) (unbounded reserve = bad_alloc risk),
//     :2891-2896 indexes textureDescriptorBytes[ti] with ti < count and
//     NO ti < 16 check, and :4978-4988 does the same for texturePoolVa.
//     A count > 16 OOB-reads the 384 B + 64 B arrays. The shader body
//     discipline (consumer-side check) was NOT applied here. vbElementCount
//     is the correct pattern (producer cap at gpu_cp.cpp:5782 + consumer
//     inside-loop break at render_thread.cpp:3238-3240 and outer check at
//     :4070-4072); textures must match it.
//   * vbStreamCount (render_queue.h:578-579): producer caps at 4
//     (gpu_cp.cpp:5762) but no consumer reads it yet; dead 64 B until the
//     multi-stream path lands. Keep the cap documented.
//   * Stale cost table: F-694:391-394 numbers are pre-growth and now
//     wrong; auditability note -- F-694's gpu_cp.cpp:3083/... cites at
//     :413-438 no longer resolve (CpSnapshotDrawState is now at
//     gpu_cp.cpp:4873, g_microcode at :506, store at :2290). Cite
//     SYMBOLS, not lines, from here on.
//   * The w681-683 payload claim (max 27 dwords at :361-364) could NOT
//     be re-verified: no w681-683 logs exist under build/ in this tree
//     (only w10-w18, H1/C-series); H1n.log has 0 CP-IM-LOAD rows.
//     Treat "27" as historical, not as a bound (header already says so).
//
// VERDICT: shader truncation contract PASS; DrawIndexed/DrawCaptured heap
//   CONCERN (bounded but ~14 MB worst-case + 3x memcpy ~10 KB/draw; needs
//   re-cost + static_asserts); textureCount CONSUMER BOUND FAIL (see
//   above; fix lives in render_thread.cpp, which this worker may not
//   edit). Details + repro in the worker's return message.