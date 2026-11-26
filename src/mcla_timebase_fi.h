// Forced-included (/FI) into the 176 generated ppc_recomp.N.cpp TUs only (CMake, patch_5). The recompiler emits
// `rX.u64 = __rdtsc();` for every mftb (XenonRecomp recompiler.cpp:1416). Guest code converts those ticks with
// 1/49,875,000 (disasm 0x821BDA90: dt = (tb - prev) * 2.005e-8f) and KeQueryPerformanceFrequency returns 49,875,000,
// so the raw host TSC (~2.3-2.7 GHz) runs the guest clock ~46-54x fast. This maps it to a true 49.875 MHz timebase.
// intrin.h is included FIRST so the intrinsic's own declaration is not macro-mangled.
#pragma once
#include <intrin.h>
#include <stdint.h>
extern "C" uint64_t mcla_guest_timebase_49875();
#define __rdtsc() mcla_guest_timebase_49875()
