#include "precompiled_shaders.h"

namespace mcla {
namespace renderer {

uint64_t MicrocodeCacheHash(const uint8_t*, std::size_t, bool) {
    return 0;
}

bool FindPrecompiledShader(uint64_t, PrecompiledShaderView&) {
    return false;
}

bool NotePrecompiledShaderLookup(uint64_t, bool) {
    return false;
}

void DumpShaderContainer(uint64_t, const uint8_t*, std::size_t, const uint8_t*, std::size_t) {
}

} // namespace renderer
} // namespace mcla