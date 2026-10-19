#pragma once
#include <atomic>
#include <cstdint>

namespace gpu {
// User values: 0 off, 2/4/8 samples. One sample means MSAA disabled internally.
inline uint32_t NormalizeMsaa(uint32_t value) {
    return value == 2 || value == 4 || value == 8 ? value : 0;
}
// Renderer publishes one coherent snapshot: requested low byte, effective next byte,
// fallback flag bit 16. This reports configuration, not proof of correct GPU output.
inline std::atomic<uint32_t> g_msaa_state{1u << 8};
}
