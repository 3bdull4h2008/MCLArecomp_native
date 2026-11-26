// gnh_p2.h -- GNH-P2: switch + seams between the native D3D-hook draw producer
// (src/gpu_native_hooks.cpp) and the PM4 path. docs/GPU_NATIVE_HOOK_MAP.md §5.
//
// MCLA_RENDERER=native (default) | pm4 | both, read once.
//   pm4    : unchanged behaviour; the native hooks only log.
//   native : the device hooks build DRAW_CAPTURED from guest device state and
//            push it; the PM4 path still runs (fences, rptr, swap handshake)
//            but submits no draws.
//   both   : PM4 submits as today; the native producer builds every draw and
//            compares it field by field against the PM4 record (GNH-CMP).
#pragma once
#include <cstdint>

namespace mcla::native { struct DrawCapturedCommand; }

namespace mcla::gnh {

enum class Mode : uint32_t { kPm4 = 0, kNative = 1, kBoth = 2 };
Mode GetMode();

// Called by the PM4 bridge right before it would push a DRAW_CAPTURED / marker.
// Returns true when PM4 should still submit it (pm4/both), false in native mode.
bool Pm4BridgeDraw(const mcla::native::DrawCapturedCommand &dc);
bool Pm4SubmitsDraws();
// TEMP (MCLA_TEMP_PRODUCER=1): which 0 = native producer, 1 = PM4 CpBridgeDraw; QPC ticks spent.
bool TempProducerOn();
void ProdTime(int which, long long ticks);

// Render thread: a native-produced draw carries its own VS/PS constant banks
// (snapshotted from the device shadow at draw time). Returns false for a PM4
// draw or a slot that was recycled before the consumer reached it.
bool FillNativeConstants(uint32_t frameId, uint8_t *vsOut, uint8_t *psOut,
                         uint32_t bankBytes);

// D3DDevice_Resolve (sub_82420BA8), called by its owner hook in gpu_device.cpp AFTER the
// original ran (so the RB_COPY shadow is filled). Args are the entry registers.
// Native mode only: queues a RESOLVE_GUEST command in guest order with the draws.
void OnResolve(uint32_t dev, uint32_t flags, uint32_t rectVa, uint32_t destPointVa);

// Z value of the guest's last D3DDevice_Clear/ClearF (the game uses reversed depth: GEQUAL, clear 0).
float LastGuestZClear();

}  // namespace mcla::gnh
