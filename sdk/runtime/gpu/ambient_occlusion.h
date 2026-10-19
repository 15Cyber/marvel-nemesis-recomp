#pragma once
#include <atomic>
#include <cstdint>

namespace gpu {
inline uint32_t NormalizeAoMode(uint32_t mode) { return mode <= 2 ? mode : 0; }
inline const char* AoModeName(uint32_t mode) { return mode == 2 ? "GTAO" : (mode == 1 ? "SSAO" : "AO"); }

// Host-only settings, copied into the render queue at the scene hook.
struct AoParameters {
    uint32_t strength = 50;  // 0..100
    uint32_t radius = 100;   // 25..400; hundredths of a view-space unit
    uint32_t debug = 0;      // 0 composite, 1 depth, 2 normals, 3 AO factor
    uint32_t mode = 1;       // 1 SSAO, 2 GTAO; captured with the scene, not read later from the UI
};
enum class AoStatus : uint32_t { Off, Pending, Active, Unavailable };
inline std::atomic<AoStatus> g_ao_status{AoStatus::Off};
inline std::atomic<uint32_t> g_ao_mode{0};  // mode selected by the renderer, for pending/UI diagnostics
}  // namespace gpu
