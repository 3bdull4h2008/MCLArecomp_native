#pragma once

#include <cstddef>
#include <cstdint>

namespace apu {
namespace xma {
// Mirrors the seam declared in apu/xma_decoder.h. Declared here (rather than
// including that header) so the import layer can call apu::XmaSetFrameDecoder
// without pulling the whole decoder into every translation unit.
using FrameDecoderFn = int (*)(void* user, uint32_t contextId, const uint8_t* frame, size_t frameBytes,
                               uint32_t sampleRateHz, uint32_t channels, float* out,
                               size_t outSampleCapacity);
// FFmpeg AV_CODEC_ID_XMAFRAMES backend for the seam above (src/apu/xma_ffmpeg.cpp).
int FfmpegDecodeFrame(void* user, uint32_t contextId, const uint8_t* frame, size_t frameBytes,
                      uint32_t sampleRateHz, uint32_t channels, float* out,
                      size_t outSampleCapacity);
}  // namespace xma

// Stores the guest XAudio render-driver callback and starts the host
// worker thread that periodically invokes it (UnleashedRecomp pattern).
uint32_t RegisterClient(uint32_t callback, uint32_t userData);
void UnregisterClient();
void SubmitFrames(void* samples);
void Init();
void Shutdown();

// --- XMA context lifecycle (item 2 of the F-609 brief) ----------------------
// These are the bodies for the log-only XMACreateContext / XMAReleaseContext
// stubs in src/kernel/imports.cpp. A context is a real 64-byte block of guest
// memory, mirroring .research/xenia/src/xenia/apu/xma_decoder.cc:122-136.
// XmaCreateContext returns false on honest OOM rather than handing back a fake
// handle; *outContextPtr receives the guest VA of the 64-byte context, which is
// the same `context` argument the guest's XMA*/XAudio2 APIs take.
bool XmaCreateContext(uint32_t* outContextPtr);
void XmaReleaseContext(uint32_t contextPtr);
void XmaKickContext(uint32_t contextPtr);
void XmaLockContext(uint32_t contextPtr);
void XmaClearContext(uint32_t contextPtr);
bool XmaBlockOnContext(uint32_t contextPtr, bool poll);
// XMA MMIO register file (0x7FEA0000 aperture). Mirrors Xenia
// XmaDecoder::ReadRegister/WriteRegister (xma_decoder.cc). The guest writes
// kick/lock/clear bitmasks here; we dispatch directly (no decoder thread).
uint32_t XmaReadRegister(uint32_t addr);
void XmaWriteRegister(uint32_t addr, uint32_t value);

// Force the XMA context array + MMIO register file into existence before the
// first guest access. The guest reads 0x7FEA1800 (ContextArrayAddress,
// r = 0x600) straight from aperture RAM, so it must already hold the array VA;
// without this the register file is all zeros until the first XMACreateContext.
// Idempotent; call after g_userHeap.Init() and after guest memory is mapped.
void XmaEnsureReady();

// The XMA *bitstream* decoder is NOT provided (src/apu/xma_decoder.h explains
// why, and names the reference that does have it). Registering one is the only
// thing needed to make the framing layer above it produce samples.
void XmaSetFrameDecoder(apu::xma::FrameDecoderFn fn, void* user);
bool XmaHasFrameDecoder();
uint32_t XmaFramesDecoded();

// Telemetry: count of audio callback underruns occurring after the guest has
// registered an XAudio render-driver client (excludes pre-audio startup silence).
uint64_t SteadyUnderruns();

}  // namespace apu

namespace mcla {
namespace apu {
using ::apu::Init;
using ::apu::RegisterClient;
using ::apu::Shutdown;
using ::apu::SteadyUnderruns;
using ::apu::SubmitFrames;
using ::apu::UnregisterClient;
using ::apu::XmaBlockOnContext;
using ::apu::XmaClearContext;
using ::apu::XmaCreateContext;
using ::apu::XmaEnsureReady;
using ::apu::XmaFramesDecoded;
using ::apu::XmaHasFrameDecoder;
using ::apu::XmaKickContext;
using ::apu::XmaLockContext;
using ::apu::XmaReadRegister;
using ::apu::XmaReleaseContext;
using ::apu::XmaSetFrameDecoder;
using ::apu::XmaWriteRegister;
namespace xma = ::apu::xma;
}  // namespace apu
}  // namespace mcla
