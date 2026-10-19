#pragma once
#include <cstdint>
#include <limits>
namespace console::traffic {
// Use 64-bit intermediate arithmetic: malformed or unusually large index counts
// must not wrap into a tiny DMA budget and pass admission checks.
inline constexpr uint64_t PS2MeshBytes64(unsigned indices) {
    return (uint64_t(indices) / 3u) * 5u * 16u * 3u + 16u * 1024u;
}
inline constexpr unsigned PS2MeshBytes(unsigned indices) {
    return PS2MeshBytes64(indices) > std::numeric_limits<unsigned>::max()
        ? std::numeric_limits<unsigned>::max()
        : static_cast<unsigned>(PS2MeshBytes64(indices));
}
inline bool AdmitPS2Mesh(unsigned indices, int64_t available, unsigned& remaining) {
    const uint64_t bytes = PS2MeshBytes64(indices);
    if (bytes > remaining || available < 0 ||
        uint64_t(available) < bytes + 192u * 1024u) return false;
    remaining -= static_cast<unsigned>(bytes);
    return true;
}
}
