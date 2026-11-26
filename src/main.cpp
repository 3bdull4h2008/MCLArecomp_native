#include "app.h"
#include "logging.h"
#include "common/cvar.h"

#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#ifdef MCLA_REQUIRE_AVX2
#include <windows.h>
#endif

int main(int argc, char* argv[]) {
    // Launcher contract: dump the CVar registry as JSON and exit before any device or guest work.
    const char* dumpPath = std::getenv("MCLA_DUMP_CVARS");
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--dump-cvars") == 0) dumpPath = argv[i + 1];
    if (dumpPath && *dumpPath)
        return mcla::cvar::CVarSystem::Instance().WriteJsonDump(dumpPath) ? 0 : 2;
#ifdef MCLA_REQUIRE_AVX2
    // Guest TUs are built with -march=x86-64-v3 (CMake MCLA_GUEST_OPT=C/D).
    if (!IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)) {
        MessageBoxA(nullptr, "This build requires a CPU with AVX2 (x86-64-v3). Rebuild with -DMCLA_GUEST_OPT=B.", "MCLA", MB_OK | MB_ICONERROR);
        return 1;
    }
#endif

    mcla::App app("MCLA Native");

    if (!app.Initialize()) {
        // W0: park instead of exiting — but Initialize only fails before the HWND
        // exists (app.cpp:43/54; the D3D12 render thread starts at app.cpp:110).
        MCLA_LOG_ERROR("Initialize failed — keeping process alive if window exists");
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(24));
        }
    }

    app.Run();

    // W0: after MainLoop, do not exit immediately — keep window until killed.
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
    return 0;
}
