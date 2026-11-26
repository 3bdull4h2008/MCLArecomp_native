// tools/validators/texture_import_stubs.cpp
//
// Link seams for building src/guest_memory.cpp into an offline validator.
// guest_memory.cpp calls these from its PAGE-WATCH hooks; gpu_cp.cpp owns the real
// implementations and drags in the whole PPC ABI header chain, which the validator
// does not need. The texture import path never touches a watched page, so these are
// inert by construction.
#include <cstdint>

#include "gpu_cp.h"

namespace mcla::gpu {
void PageWatchOnWrite(uint32_t guestAddr, uint32_t value) {
    (void)guestAddr;
    (void)value;
}
void PageWatchOnRead(uint32_t guestAddr, uint32_t value) {
    (void)guestAddr;
    (void)value;
}
// Added 2026-11-03: heap.cpp / guest_thread.cpp began calling these from the
// allocation and thread-start paths, and the third-party o1heap-fragment wipe
// census, so the validator stopped linking. Inert here for the same reason as the
// pair above: this validator never allocates through Heap and never starts a
// guest thread, so no call site can be reached. If that ever stops being true the
// stub must be replaced with the real body rather than silently doing nothing.
void T44WipeTotalsLine(uint32_t n) { (void)n; }

// The wipe census itself (src/gpu_cp.cpp) is consulted by guest_memory.cpp on
// EVERY checked write, and the CP is not linked into this validator. `overlaps`
// returning false is the correct inert answer -- it means "no armed band covers
// this address", which is true here because nothing ever arms one without the CP.
bool T44WipeOverlaps(uint32_t guestAddr, uint32_t len) {
    (void)guestAddr;
    (void)len;
    return false;
}
bool T44WipeSpan(uint32_t guestAddr, uint32_t len, uint32_t* outStart,
                 uint32_t* outLen) {
    (void)guestAddr;
    (void)len;
    if (outStart) *outStart = 0;
    if (outLen) *outLen = 0;
    return false;
}
void T44WipeNote(const char* op, uint32_t guestAddr, uint32_t len, uint64_t val,
                 uint64_t pre, uint32_t width, const void* ret) {
    // Unreachable while T44WipeOverlaps is false; kept so the signature stays
    // honest if a caller ever reports without testing first.
    (void)op;
    (void)guestAddr;
    (void)len;
    (void)val;
    (void)pre;
    (void)width;
    (void)ret;
}
void HeapInstanceStoreReport(uint32_t guestAddr, uint32_t value) {
    (void)guestAddr;
    (void)value;
}
bool CpReadGuestRegisters(const uint32_t* indices, uint32_t count,
                          uint32_t* outValues, uint32_t* outPresent) {
    (void)indices;
    (void)count;
    (void)outValues;
    (void)outPresent;
    return false;
}
CpGuestRenderTarget CpGuestRenderTargetInfo() { return CpGuestRenderTarget{}; }
}  // namespace mcla::gpu

namespace mcla::native {
// d3d12_backend.cpp calls this when it stamps a guest render target. The real
// table lives in src/renderer/, which this validator does not link; 0 is the
// documented "unknown format" answer callers already handle.
uint32_t MapXenosColorFormatToDxgi(uint32_t xenosFormat) {
    (void)xenosFormat;
    return 0;
}
}  // namespace mcla::native
