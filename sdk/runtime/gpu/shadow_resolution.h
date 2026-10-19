#pragma once
#include <atomic>
#include <cstdint>

namespace gpu {
inline uint32_t NormalizeShadowResolution(uint32_t size) {
    switch (size) {
        case 256: case 512: case 1024: case 2048: return size;
        default: return 0;  // follow the normal render scale
    }
}
// One consistent snapshot for F10: requested pixels in bits 0..15, effective
// square extent in 16..31, fallback in bit 32. Extent zero means not allocated.
inline std::atomic<uint64_t> g_shadow_resolution_state{0};
inline uint32_t NormalizeShadowSoftness(uint32_t value) { return value <= 2 ? value : 0; }
enum class ShadowSoftnessStatus : uint32_t { Off, Pending, Active, Unavailable };
// Requested mode in bits 0..7 and status in 8..15, published by the render thread.
inline std::atomic<uint32_t> g_shadow_softness_state{0};
}  // namespace gpu
