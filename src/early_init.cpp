#include <cstdlib>
#include <cstdio>
#include <signal.h>
#include <windows.h>
#include <timeapi.h>  // T43.18: timeBeginPeriod, process-scope 1 ms granularity

static LONG WINAPI EarlyVeh(EXCEPTION_POINTERS* ep) {
    FILE* f = nullptr;
    fopen_s(&f, "crash_log_early.txt", "w");
    if (f) {
        fprintf(f, "VEH exception: code=0x%08lX addr=0x%p\n",
                static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                ep->ExceptionRecord->ExceptionAddress);
        for (ULONG i = 0; i < ep->ExceptionRecord->NumberParameters && i < 15; i++) {
            fprintf(f, "  param[%lu] = 0x%llX\n", i, ep->ExceptionRecord->ExceptionInformation[i]);
        }
        if (ep->ContextRecord) {
            // 2026-11-10: the raw RIP is useless without the module base, which
            // moves under ASLR. Print the RVA so tools/host_rva_resolve.py can
            // name the owner from build/mcla.map.
            const uintptr_t modBase = (uintptr_t)GetModuleHandle(nullptr);
            const uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
            fprintf(f, "RIP=0x%llX RSP=0x%llX\n",
                    (unsigned long long)rip, (unsigned long long)ep->ContextRecord->Rsp);
            fprintf(f, "  module_base=0x%llX rva=0x%llX tid=%lu\n",
                    (unsigned long long)modBase,
                    (unsigned long long)(rip - modBase), GetCurrentThreadId());
            // Caller chain, resolved to RVAs so build/mcla.map names each frame.
            void* frames[24];
            const USHORT nf = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
            for (USHORT i = 0; i < nf; ++i) {
                fprintf(f, "  bt[%u] rva=0x%llX\n", (unsigned)i,
                        (unsigned long long)((uintptr_t)frames[i] - modBase));
            }
        }
        fflush(f);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void EarlyInvalidParam(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
}

static void EarlyAbortSignal(int) {
}

#pragma init_seg(lib)
struct EarlyCrashInit {
    EarlyCrashInit() {
        _set_abort_behavior(0, _CALL_REPORTFAULT);
        _set_invalid_parameter_handler(EarlyInvalidParam);
        signal(SIGABRT, EarlyAbortSignal);
        AddVectoredExceptionHandler(1, EarlyVeh);
        // T43.18 (2026-10-13, taken from LARecomp's pacing notes): process
        // timer granularity to 1 ms for the life of the process. Windows
        // defaults to ~15.6 ms, which turns every short host sleep (the 1 ms
        // semaphore poll slices, vsync pacing) into a 15 ms stall. Host-only
        // (winmm.lib, already linked); no guest behavior changes. Never
        // paired with timeEndPeriod: the requirement outlives every scope.
        timeBeginPeriod(1);
    }
} g_earlyCrashInit;
