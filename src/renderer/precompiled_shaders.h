#pragma once
#include <cstddef>
#include <cstdint>

namespace mcla {
namespace renderer {

struct PrecompiledShaderView {
    const uint8_t* dxil = nullptr;
    std::size_t dxilSize = 0;
    uint32_t specConstantsMask = 0;
};

uint64_t MicrocodeCacheHash(const uint8_t* microcode, std::size_t size, bool isVertex);
bool FindPrecompiledShader(uint64_t hash, PrecompiledShaderView& out);
bool NotePrecompiledShaderLookup(uint64_t hash, bool hit);
void DumpShaderContainer(uint64_t hash, const uint8_t* container, std::size_t size,
                        const uint8_t* microcode, std::size_t microcodeSize);

} // namespace renderer
} // namespace mcla