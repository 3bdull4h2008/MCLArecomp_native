# mcla-pc

Native D3D12 port/rebuild of **Midnight Club: Los Angeles** for Xbox 360 (Xenon PPC) to PC. Recompiles the guest PPC code to native C++ via [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and implements Xbox 360 kernel/XAM/audio/input subsystems natively.

## Status

- **2D/UI rendering**: Working end-to-end. Render thread owns D3D12, UI quads/menus reach the window. Present path verified.
- **Boot**: Boots into loading/garage with real retail assets from `game_data/`. TLS allocator, VEH recovery, GFx loader wired.
- **3D/Gameplay**: In progress. UI containers decompressing (~95% garage), GFx init (`kernel handle 0x40001905`) is the current frontier.
- **Goal**: Run the graphics natively via D3D12 intent replay (capturing render intent at the high-level boundary), bypassing full Xenos GPU emulation.

**This is not a Rexglue-based project.** No Rexglue SDK. The game code is recompiled to native C++ and runs with native host implementations of Xbox 360 APIs.

## How it works

1. **PPC Recompilation**: 176 translation units of XenonRecomp-generated C++ (`generated/ppc_xenon/ppc_recomp.*.cpp`) plus `ppc_func_mapping.cpp`
2. **Native D3D12 backend**: `src/d3d12_backend.*`, render thread (`src/render_thread.*`), GPU command processor capture (`src/gpu_cp.*`, `src/gpu_device.*`)
3. **Xenos shader translation**: `src/renderer/` - microcode decode → IR → HLSL → DXIL via DXC at runtime, with PSO/pipeline cache
4. **Native Xbox 360 subsystems**: Kernel objects, XAM (input/UI), APU/XMA audio, filesystem (RPF3 VFS), memory management

## Requirements

- Windows 10/11
- Visual Studio 2022 Build Tools with Clang/clang-cl
- CMake 3.25+, Ninja
- DirectX 12 SDK components
- **Legally dumped** Midnight Club: LA files in `game_data/` (`default.xex`, `xarchive_cache.rpf`, etc.)

## Building

```bat
configure.bat
ninja_build.bat
build\mcla.exe
```

Delete `build/` if switching compilers/toolchains.

## Dependencies (vendored)

- [SDL3](https://www.libsdl.org/) - windowing, input (gamepads/keyboard)
- [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) - logging
- [toml++](https://github.com/marzer/tomlplusplus) - config
- [DXC](https://github.com/microsoft/DirectXShaderCompiler) - HLSL to DXIL
- [o1heap](https://github.com/pavel-kirienko/o1heap) - allocator
- [xxHash](https://github.com/Cyan4973/xxHash), [tiny-AES-c](https://github.com/kokke/tiny-AES-c), [libmspack](https://www.cabextract.org.uk/libmspack/) - various utilities
- Research references in `.research/` (XenonRecomp, XenosRecomp, Xenia, etc.)

## Project structure

- `src/` - Host code, kernel/XAM ports, D3D12 backend, renderer, filesystem, audio
- `src/kernel/` - Xbox kernel reimplementation (XAM, objects, memory, threads)
- `src/apu/` - Audio processing unit / XMA decoder
- `src/renderer/` - Xenos shader translation, texture/vertex decode
- `src/fs/` - RPF3 packfile VFS
- `generated/ppc_xenon/` - XenonRecomp output (176 TUs) - do not edit
- `third_party/` - Vendored dependencies
- `tools/` - Validators, analysis tools
- `config/` - Build/config files

## Legal

You must provide your own legally obtained copy of Midnight Club: Los Angeles. This project does not include any game assets, executables, or copyrighted content.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) - PPC to C++ recompilation
- [Xenia](https://github.com/xenia-project/xenia) - Xbox 360 emulation reference
- [LARecomp](https://github.com/sonicdcer/LARecomp) / BadassBaboon - Guest research
