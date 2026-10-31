# mcla-pc

native d3d12 renderer rebuild for **midnight club: los angeles** (xbox 360 → pc).

the recompiled ppc game code keeps running untouched. we capture render intent at the high-level draw boundary and replay it as real d3d12 instead of letting the xenos command processor handle the gpu. legacy mode stays default until every gate passes.

**not rexglue based.** no emulation layer, no sdk wrapper. the game recompiles to native c++ and runs as native code on your pc. native performance, not the overhead of a translation layer.

## what makes this different

most xbox 360 → pc projects (xenia, rexglue, etc.) emulate the gpu. they translate xenos commands into vulkan/d3d11 at runtime, paying a heavy overhead and inheriting every emulation bug. **mcla-pc does not emulate.** it recompiles the game's ppc to native c++, captures what the game *intends* to draw at the high-level boundary, and replays that intent directly through d3d12. the xenos command processor is bypassed entirely for rendering.

| | mcla-pc | typical emulation (xenia/rexglue) |
|---|---|---|
| game code | native c++ (recompiled) | emulated ppc |
| gpu | native d3d12 (intent replay) | xenos → vulkan/d3d11 translation |
| overhead | native | emulation tax |
| graphics goal | run the graphics part **natively** | emulate the gpu |

## 2d: done

the 2d rendering path is **working end-to-end**. the boot host links the full recompiled image (46k+ function mappings), the render thread owns all d3d12, and 2d ui (menus, text, quads) reaches the window. the guest's pixels do reach the screen — present path verified end-to-end with window readbacks reproducing guest-rt rows (F-907). blend is now derived from guest registers (RB_BLENDCONTROL0 + RB_COLOR_MASK), not faked.

## full nativity

everything that can be native, is native:
- **graphics** — d3d12 backend with pso cache, root signature, grcFvf input layout, async DXC shader worker (xenos microcode → IR → DXIL), texture/vertex decode, pipeline cache, mipgen
- **audio** — native apu/xma decode (xma_decoder, audlo extraction, XAudio render-driver callback worker)
- **input** — native xam input (SDL3 gamepads/keyboard, hot-plug, 4-bay port map, keystroke enqueue/dequeue, force-feedback routing)
- **memory** — native guest memory (o1heap arenas, 4 GB window at 0x100000000, guest-virtual region tracking, endian-checked accessors)
- **filesystem** — rpf3 packfile VFS (virtual path index, nested archives, LZX inflate, raw DEFLATE)
- **kernel** — native reimplementation of xbox kernel (xam, xdm, objects, mutexes, ports, net, voice input, user config, heap)
- **cpu** — recompiled ppc as native c++ (vmx128 host hooks, guest threads), zero rexglue sdk
- **shader translation** — xenos microcode → normalized IR → HLSL → DXIL at runtime

## what exists

### graphics pipeline
- xenos shader decode → IR → DXIL translation pipeline (1612 shaders validated)
- d3d12 backend with pso cache + async DXC worker
- texture decode (xenos formats → DXGI, tiling/untile, mipgen pipeline)
- vertex decode (xenos vertex-fetch → component layout/DXGI format)
- grcFvf vertex declaration decoding
- resource cache (keyed by kind/addr/size/format/version)
- offline validators for every phase gate
- device-method capture hooks live + render thread owns all d3d12
- shader → PSO pipeline wired: PipelineCache, root signature, grcFvf input layout
- render graph, dynamic samplers, frame trace, capture hooks

### kernel / runtime
- xam: content registry/roots, notify listeners, content enumeration, input, message box UI
- xdm: dispatcher primitives, kernel thread IDs, main-park publishing
- kernel objects: Event, Semaphore, FileObject, SectionObject, close/identity
- memory: guest heap (o1heap), 4 GB window, virtual region tracking, endian helpers
- ports: kobj, net, xam_user, voice input
- heap: physical allocator with allocation census, phys-range tracking
- guest threads: creation/scheduling, PPCContext ABI

### filesystem
- rpf3 packfile VFS with virtual path index
- nested archive registration and member resolution
- LZX inflate (xmem/MS-LZX) for RSC5 containers
- raw DEFLATE for *.list members
- synthesized RPF3 archive builder for virtual files

### audio
- xma decoder (framing, split-frame reassembly, ring buffer, 320-context pool)
- audlo extraction (RPF3 audlo archive → XMA2 members, LZX bridge)
- XAudio render-driver callback worker thread
- frame-decoder seam (pluggable backend)

### input
- SDL3 gamepad/keyboard with hot-plug (SDL_EVENT_GAMEPAD_ADDED/REMOVED)
- 4-bay controller port map (XamInputPortCount/PortDevice)
- keystroke enqueue/dequeue, keyboard sampling
- force-feedback routing (XInputd exports)
- voice input port (VoiPortXamInputGetKeystrokeEx)

### cpu
- recompiled ppc as native c++ (vmx128 host hooks for missing modes)
- guest thread context (thread-local PPCContext)
- vmx128 pack (UINT_2101010, SHORT_4, SHORT_2, etc.)

### rage asset pipeline
- CSR/TXD/DFF parsers
- RageAssetManager
- grcFvf vertex declaration decoding

## current status (2026-10-31)

boot gate complete. TLS allocator stable, VEH recovery, GFx loader wired. last soak opens real retail packfiles and runs minutes on the loading path. graphics 2d path working end-to-end; 3d/ui containers being decompressed into guest memory (XSF inflate delivered to ~95% garage, 3/4 byte-exact). GFx init path (kernel handle 0x40001905) is the open frontier — see git history for the F-845…F-914 investigation trail.

## building

needs vs 2022 buildtools (vcvars64), cmake 3.25+, ninja, clang-cl for both c and c++. delete the build folder when switching compilers

```bat
configure.bat
ninja_build.bat
build\mcla.exe
```

## repo layout

- `src/` root - boot host, device-boundary hooks (gpu_device, gpu_cp), d3d12 backend, render thread/queue
- `src/kernel`, `src/apu`, `src/user` - kernel framework and guest runtime (xam.cpp = input)
- `src/cpu` - vmx128 host hooks, guest thread, ppc context
- `src/fs` - rpf3 packfile vfs
- `src/rage` - rage asset pipeline
- `src/hooks` - ppc_func hook bodies (split from patches.cpp)
- `src/common` - shared logging/cvar headers
- `src/renderer` - xenos decode, shader translation, texture/vertex decoding, caches
- `tools/validators` - phase-gate validator exes
- `generated/` - recompiler output, input only, never edited by hand
- `third_party/` - sdl3, fmt, spdlog, toml++, dxc, o1heap, xxhash
- `config/` - cmake/xenonrecomp/manifest tomls

## legal

bring your own legally dumped copy of midnight club: la. this project does not ship any game data.

## credits

- **XenonRecomp** - ppc → c++ recompiler
- **XenosRecomp** - xenos shader → hlsl/dxil
- **Xenia** - reference for command processor / gpu behavior
- **LARecomp / BadassBaboon** - guest facts (addresses, timings, quirks)
- **UnleashedRecomp** - reference architecture

## contributing

this project needs help. if you have experience with:
- xbox 360 / xenos gpu emulation
- d3d12 rendering
- ppc recompilation
- kernel/hook frameworks

open a pr or open an issue. dont be shy.