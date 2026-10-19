#pragma once
#include <atomic>
#include <cstdint>

namespace gpu {

// Value snapshot recorded with the scene, never read from UI state by the worker.
struct BloomParameters {
    uint32_t enabled = 0;
    uint32_t strength = 25;
    uint32_t threshold = 75;
    uint32_t radius = 100;
};

enum class BloomStatus { Off, Pending, Active, Unavailable };
inline std::atomic<BloomStatus> g_bloom_status{BloomStatus::Off};

}  // namespace gpu
