// FFmpeg backend for the XMA FrameDecoderFn seam (src/apu/xma_decoder.h).
// Drives Xenia's patched AV_CODEC_ID_XMAFRAMES decoder (third_party/ffmpeg,
// libavcodec/wmaprodec.c ff_xmaframes_decoder) exactly as
// .research/xenia/src/xenia/apu/xma_context.cc does:
//   Setup          :62-89   one packet/codec-context/frame per XMA context,
//                           channels/sample_rate start at 0, codec not opened
//   PrepareDecoder :830-855 reopen the codec when rate or channel count change
//   Decode         :573-624 send the assembled frame as one packet, receive
//                           exactly one AV_SAMPLE_FMT_FLTP frame
//   ConvertFrame   :857-932 planar -> interleaved (the seam wants float; the
//                           caller clamps and converts to BE int16)
// Calls for one contextId are serialised by that context's lock (Work() holds
// it), so each slot below is only ever touched by one thread at a time.
#include "audio.h"
#include "logging.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace apu::xma {
namespace {

constexpr uint32_t kContexts = 320;        // xma_decoder.h kContextCount (xma_decoder.cc:83)
constexpr uint32_t kSamplesPerFrame = 512; // xma_context.h:140

struct Slot {
    const AVCodec* codec = nullptr;  // kept: avcodec_close() clears ctx->codec
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    bool failed = false;  // setup failed once; do not retry every frame
};
std::array<Slot, kContexts> g_slots;
std::atomic<uint32_t> g_ok{0};
std::atomic<uint32_t> g_fail{0};

bool LogThis(uint32_t n) { return n <= 16 || (n % 1000) == 0; }

int Fail(uint32_t contextId, const char* why, int av, size_t bytes) {
    const uint32_t n = g_fail.fetch_add(1, std::memory_order_relaxed) + 1;
    if (LogThis(n)) {
        MCLA_LOG_ERROR("XMA-DECODE fail #{} ctx={} why={} av={} bytes={}", n, contextId,
                       why, av, bytes);
    }
    return av < 0 ? av : -1;
}

// xma_context.cc:62-89.
bool SetupSlot(Slot& s) {
    s.codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
    if (s.codec == nullptr) return false;
    s.packet = av_packet_alloc();
    s.ctx = avcodec_alloc_context3(s.codec);
    s.frame = av_frame_alloc();
    if (s.packet == nullptr || s.ctx == nullptr || s.frame == nullptr) return false;
    s.ctx->channels = 0;
    s.ctx->sample_rate = 0;
    return true;
}

static std::atomic<uint64_t> g_xmaDecodeUs{0};
static std::atomic<uint64_t> g_xmaDecodeCalls{0};
static std::atomic<int64_t> g_xmaLastLogTimeQpc{0};

static int64_t GetXmaQpcFreq() {
    static const int64_t s_freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return s_freq;
}

static int FfmpegDecodeFrameInternal(void* /*user*/, uint32_t contextId, const uint8_t* frame,
                                    size_t frameBytes, uint32_t sampleRateHz, uint32_t channels, float* out,
                                    size_t outSampleCapacity) {
    if (contextId >= kContexts) return Fail(contextId, "ctx-range", -1, frameBytes);
    if (frame == nullptr || frameBytes == 0 || out == nullptr)
        return Fail(contextId, "null-arg", -1, frameBytes);
    if (channels < 1 || channels > 2) return Fail(contextId, "channels", -1, frameBytes);
    if (outSampleCapacity < size_t(kSamplesPerFrame) * channels)
        return Fail(contextId, "out-cap", -1, frameBytes);

    Slot& s = g_slots[contextId];
    if (s.failed) return Fail(contextId, "setup-failed", -1, frameBytes);
    if (s.ctx == nullptr && !SetupSlot(s)) {
        s.failed = true;
        return Fail(contextId, "setup", -1, frameBytes);
    }

    // xma_context.cc:839-850 -- reopen on a rate/channel change.
    if (s.ctx->sample_rate != int(sampleRateHz) || s.ctx->channels != int(channels) ||
        !avcodec_is_open(s.ctx)) {
        avcodec_close(s.ctx);
        s.ctx->sample_rate = int(sampleRateHz);
        s.ctx->channels = int(channels);
        const int rc = avcodec_open2(s.ctx, s.codec, nullptr);
        if (rc < 0) return Fail(contextId, "open", rc, frameBytes);
    }

    // xma_context.cc:573-574 -- the assembled frame is the whole packet. FFmpeg
    // only reads it (send_packet copies a non-refcounted packet).
    s.packet->data = const_cast<uint8_t*>(frame);
    s.packet->size = int(frameBytes);
    int rc = avcodec_send_packet(s.ctx, s.packet);  // :588
    s.packet->data = nullptr;
    s.packet->size = 0;
    if (rc < 0) return Fail(contextId, "send", rc, frameBytes);
    rc = avcodec_receive_frame(s.ctx, s.frame);  // :594
    if (rc < 0) return Fail(contextId, "receive", rc, frameBytes);

    // xma_context.cc:614 asserts FLTP; :857-932 consume exactly kSamplesPerFrame.
    if (s.ctx->sample_fmt != AV_SAMPLE_FMT_FLTP || s.frame->nb_samples < int(kSamplesPerFrame)) {
        const int got = s.frame->nb_samples;
        av_frame_unref(s.frame);
        return Fail(contextId, s.ctx->sample_fmt != AV_SAMPLE_FMT_FLTP ? "fmt" : "short", got,
                    frameBytes);
    }
    for (uint32_t c = 0; c < channels; ++c) {
        const auto* in = reinterpret_cast<const float*>(s.frame->data[c]);
        for (uint32_t i = 0; i < kSamplesPerFrame; ++i) out[i * channels + c] = in[i];
    }
    av_frame_unref(s.frame);

    const uint32_t n = g_ok.fetch_add(1, std::memory_order_relaxed) + 1;
    if (LogThis(n)) {
        MCLA_LOG_INFO("XMA-DECODE ok #{} ctx={} bytes={} rate={} ch={} samples={}", n,
                      contextId, frameBytes, sampleRateHz, channels,
                      kSamplesPerFrame * channels);
    }
    return 0;
}

}  // namespace

int FfmpegDecodeFrame(void* user, uint32_t contextId, const uint8_t* frame,
                      size_t frameBytes, uint32_t sampleRateHz, uint32_t channels, float* out,
                      size_t outSampleCapacity) {
    static const bool s_noXmaDecode = [] {
        const char* e = std::getenv("MCLA_NO_XMA_DECODE");
        return e && e[0] == '1';
    }();
    static const bool s_emuCostProbe = [] {
        const char* e = std::getenv("MCLA_EMU_COST_PROBE");
        return e && e[0] == '1';
    }();

    if (!s_emuCostProbe) {
        if (s_noXmaDecode) {
            if (out != nullptr && channels >= 1 && channels <= 2) {
                std::memset(out, 0, size_t(kSamplesPerFrame) * channels * sizeof(float));
            }
            return 0;
        }
        return FfmpegDecodeFrameInternal(user, contextId, frame, frameBytes, sampleRateHz, channels, out, outSampleCapacity);
    }

    LARGE_INTEGER qpc0;
    QueryPerformanceCounter(&qpc0);

    int rc = 0;
    if (s_noXmaDecode) {
        if (out != nullptr && channels >= 1 && channels <= 2) {
            std::memset(out, 0, size_t(kSamplesPerFrame) * channels * sizeof(float));
        }
    } else {
        rc = FfmpegDecodeFrameInternal(user, contextId, frame, frameBytes, sampleRateHz, channels, out, outSampleCapacity);
    }

    LARGE_INTEGER qpc1;
    QueryPerformanceCounter(&qpc1);

    const int64_t freq = GetXmaQpcFreq();
    const uint64_t elapsedUs = static_cast<uint64_t>(
        ((qpc1.QuadPart - qpc0.QuadPart) * 1000000LL) / freq);
    g_xmaDecodeUs.fetch_add(elapsedUs, std::memory_order_relaxed);
    g_xmaDecodeCalls.fetch_add(1, std::memory_order_relaxed);

    int64_t lastLog = g_xmaLastLogTimeQpc.load(std::memory_order_relaxed);
    if (lastLog == 0) {
        g_xmaLastLogTimeQpc.store(qpc1.QuadPart, std::memory_order_relaxed);
    } else if (qpc1.QuadPart - lastLog >= freq * 2) {
        if (g_xmaLastLogTimeQpc.compare_exchange_strong(lastLog, qpc1.QuadPart, std::memory_order_relaxed)) {
            const uint64_t us = g_xmaDecodeUs.exchange(0, std::memory_order_relaxed);
            const uint64_t calls = g_xmaDecodeCalls.exchange(0, std::memory_order_relaxed);
            const double winMs = (static_cast<double>(qpc1.QuadPart - lastLog) * 1000.0) / static_cast<double>(freq);
            MCLA_LOG_INFO("[EMU-COST-XMA] winMs={:.1f} xmaDecodeUs={} xmaCalls={} disabled={}",
                          winMs, us, calls, s_noXmaDecode ? 1 : 0);
        }
    }

    return rc;
}

}  // namespace apu::xma
