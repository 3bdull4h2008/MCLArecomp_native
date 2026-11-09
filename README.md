# MCLA PC

Native D3D12 port of Midnight Club: Los Angeles for Xbox 360 (Xenon) to PC. Guest PowerPC code is statically recompiled ahead of time to native C++ with XenonRecomp. The host implements Xbox 360 kernel, XAM, audio, input, and GPU intent natively. No interpreter. No Rexglue SDK.

Goal is boot to menu to playable. The guest is `default.xex`, the retail Xbox 360 build of Midnight Club: Los Angeles. No retail content is bundled. You supply your own legally dumped `game_data` files.

## What differs from other Xenon emulators

* Single guest `default.xex` recompiled to C++ per translation unit. Host code does not interpret guest code at runtime.
* Graphics is D3D12 intent replay, not full Xenos GPU emulation. The command processor captures guest draw intent and the host issues it directly.
* Kernel objects, XAM UI, APU and XMA audio, and the RPF3 file system are host implementations. They are not guest code.

## Requirements

* Windows 10 or 11, 64-bit.
* Visual Studio 2022 Build Tools with `clang-cl`.
* LLVM tools with `llvm-rc`.
* CMake 3.25 or newer and Ninja.
* Python 3.14 for build scripts.
* Legally dumped game files in `game_data`, including `default.xex` and `xarchive_cache.rpf`.

## Build

From the repo root on a clean clone.

```bat
build_on_e.bat
build\mcla.exe
```

`build_on_e.bat` without arguments runs configure plus build. `build_on_e.bat build` builds only and assumes `build\build.ninja` already exists. The two underlying steps are:

```bat
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build mcla.exe
```

Delete `build` if you change compilers. The exe is `build\mcla.exe`. It needs `game_data` beside the build output as resolved by the host file code.

## How it runs

The host loads the guest image, rebuilds guest memory, then runs recompiled guest code for CPU. Boot goes through guest kernel startup, then the title path, then 2D and 3D rendering. Guest facts are taken from raw bytes in `build/cache/mcla_pe.bin` with offset `VA - 0x82000000`, then confirmed against disassembly. Docs are treated as intent only. See `docs` in the tree for the evidence ledger and handoff notes.

## Tech notes

* Recompiler pipeline. `generated/ppc_xenon` holds 176 translation units plus `ppc_func_mapping.cpp`. Host headers declare guest functions. Do not hand edit `generated`.
* CP and IB packet flow. The guest command processor writes packets through indirect buffers. `src/gpu_cp.cpp` and `src/gpu_device.cpp` capture register writes and buffer pointers. The render thread turns captured draws into D3D12 draws. Readback is used for census only.
* Shader translator. `src/renderer/shader_translator.cpp` decodes Xenos microcode to IR to HLSL to DXIL through DXC at runtime with a PSO cache. Constant banks are split by stage. The pixel bank is built from the ALU register file. Fade handling keeps the guest intended texture multiply by per vertex alpha.
* Renderer. `src/d3d12_backend.*` owns device, pipelines, and presentation. `src/render_thread.*` owns draw submission. `src/renderer/vertex_decode.cpp` decodes guest vertex buffers. `src/fs/vfs_rpf.cpp` decodes the RPF3 archive.

## Status and blockers

* 2D and UI reach the window through the present path. Boot reaches loading and garage paths with real assets.
* Star texture width is unproven. Star texcoords arrive in guest pixels. Dividing by 64 was tried as a guess and painted grey stars with the wrong texels. That divide is reverted. Stars currently sample raw and read black where the guess read grey. Width is still open.
* City stall after ArrowBlit is open. After the blit the guest stops issuing useful draws.
* Spinner half empty texture is open. The loading spinner shows half an empty texture.
* AES TOC decrypt has a slow start. First boot spends a long time before first progress markers. This is a startup cost, not a hang.

## Legal

Retail content only. This repo contains no game assets, no game executables, and no ripped content. You must provide your own copy. Midnight Club and other names belong to their owners. This project is an unofficial fan recompilation effort. No endorsement is implied. The icon in this repo is original to this project. `LICENSE.txt` covers host code only.

## Thanks

* XenonRecomp for the PowerPC to C++ recompiler.
* Xenia for public reference on Xbox 360 behavior where cited in code.
* LARecomp and BadassBaboon for guest research facts such as addresses and quirks.
* The vendored libraries credited in `third_party` and `CMakeLists.txt`.
