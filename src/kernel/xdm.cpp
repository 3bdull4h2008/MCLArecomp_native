#include <stdafx.h>
#include "xdm.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

Mutex g_kernelLock;

namespace
{
std::mutex g_waitMultMu;
std::condition_variable g_waitMultCv;
std::atomic<uint32_t> g_waitMultGen{0};

std::mutex g_handleRefMu;
std::unordered_map<uint32_t, uint32_t> g_kernelHandleExtraRefs;
}

void NotifyWaitMultipleGeneration()
{
    {
        std::lock_guard<std::mutex> lock(g_waitMultMu);
        g_waitMultGen.fetch_add(1, std::memory_order_release);
    }
    g_waitMultCv.notify_all();
}

uint32_t WaitMultipleGenerationLoad()
{
    return g_waitMultGen.load(std::memory_order_acquire);
}

bool WaitMultipleGenerationWait(uint32_t observedGen, uint32_t timeoutMs)
{
    if (g_waitMultGen.load(std::memory_order_acquire) != observedGen)
        return true;
    if (timeoutMs == 0)
        return false;

    std::unique_lock<std::mutex> lock(g_waitMultMu);
    if (g_waitMultGen.load(std::memory_order_acquire) != observedGen)
        return true;

    if (timeoutMs == INFINITE)
    {
        g_waitMultCv.wait(lock, [&]() {
            return g_waitMultGen.load(std::memory_order_acquire) != observedGen;
        });
        return true;
    }

    return g_waitMultCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
        return g_waitMultGen.load(std::memory_order_acquire) != observedGen;
    });
}

void AddKernelHandleRef(uint32_t handle)
{
    if (handle == 0 || handle == GUEST_INVALID_HANDLE_VALUE)
        return;
    std::lock_guard<std::mutex> lock(g_handleRefMu);
    ++g_kernelHandleExtraRefs[handle];
}

bool ReleaseKernelHandleRef(uint32_t handle)
{
    if (handle == 0 || handle == GUEST_INVALID_HANDLE_VALUE)
        return false;
    std::lock_guard<std::mutex> lock(g_handleRefMu);
    auto it = g_kernelHandleExtraRefs.find(handle);
    if (it == g_kernelHandleExtraRefs.end() || it->second == 0)
        return false;
    if (--(it->second) == 0)
        g_kernelHandleExtraRefs.erase(it);
    return true;
}

void DestroyKernelObject(KernelObject* obj)
{
    if (!obj) return;
    const uint32_t handle = mcla::kernel::g_memory.MapVirtual(&obj->header);
    {
        std::lock_guard<std::mutex> refLock(g_handleRefMu);
        g_kernelHandleExtraRefs.erase(handle);
    }
    std::lock_guard lock(g_kernelLock);
    // DURABLE WRAPPER IDENTITY (session 64): drop the side-map entry so a
    // recycled guest header cannot resolve to a destroyed wrapper.
    if (obj->identityHdrAddr != 0)
        WrapperIdentityMap().erase(obj->identityHdrAddr);

    obj->~KernelObject();
    g_userHeap.Free(obj);
}

// CONSOLE-CORRECT GUEST-VISIBLE DISPATCHER HEADER (2026-10-21, fix_typebyte).
//
// MEASURED (the guest's own bytes are the oracle):
//  * The console's kernel-object layout that the GUEST ITSELF writes is a
//    16-byte dispatcher header whose CLASS/TYPE byte is at offset +0, the
//    signal state at +4 and the wait list at +8/+12:
//      - the guest's own header initialiser (generated/ppc_xenon/
//        ppc_recomp.72.cpp:19342-19384) stores the Type byte at +0, a word at
//        +4, and SELF-LINKS the wait list (Flink = Blink = &header+8) at
//        +8/+12, then calls __imp__KeInitializeSemaphore on that header;
//      - the guest waits on / sets / resets such a header RAW: 0x8242FCD4
//        `addi r3,r26,32` (r26+32 == 0x40004D7C) / 0x8242FCD8
//        `bl 0x827BD874` = __imp__KeResetEvent, and 0x8242FC18
//        `bl 0x827BD5A4` = __imp__KeWaitForSingleObject on the same +32.
//    => The HANDLE a title holds for a kernel object IS the dispatcher
//       header; there is no separate OBJECT_HEADER in front of it.
//  * The host's own XDISPATCHER_HEADER has exactly that shape
//    (src/kernel/xbox.h:168-199: Type at +0, SignalState at +4, WaitListHead
//    at +8); Xenia's X_DISPATCH_HEADER is 0x10 bytes with `type` at +0
//    (.research/xenia/src/xenia/kernel/xobject.h:38-63).
//
// BEFORE this change the handle was MapVirtual(obj) = the OBJECT BASE, and
// KernelObject is POLYMORPHIC (virtual dtor xdm.h:41, virtual Wait xdm.h:51),
// so the C++ vptr sat at +0 and the embedded header at +8. The guest
// therefore read the VPTR's low byte as the class id: 0x28, measured 61/61
// host-created semaphores in build/w838.log
// ("TYPE-CENSUS[REL] obj=B660F040 type=28") while the host wrote Type=5 at
// obj+8.
//
// NOW the handle IS the dispatcher header, so the guest-visible bytes at the
// handle are the console's 16-byte header and handle+0 IS the class id.
//
// FALSIFIER: the guest-visible Type byte at a host-created handle must read
// the class id (5 for a semaphore), never 0x28.
uint32_t GetKernelHandle(KernelObject* obj)
{
    assert(obj != GetInvalidKernelObject());
    return mcla::kernel::g_memory.MapVirtual(&obj->header);
}

void DestroyKernelObject(uint32_t handle)
{
    auto* obj = GetKernelObject(handle);
    if (!obj) return;
    DestroyKernelObject(obj);
}

bool IsKernelObject(uint32_t handle)
{
    if (handle == GUEST_INVALID_HANDLE_VALUE)
        return false;

    void* translated = mcla::kernel::GuestMemoryHeap::Instance().Translate(handle);
    return translated != nullptr;
}

bool IsKernelObject(void* obj)
{
    return IsKernelObject(mcla::kernel::g_memory.MapVirtual(obj));
}

bool IsInvalidKernelObject(void* obj)
{
    return obj == GetInvalidKernelObject();
}
