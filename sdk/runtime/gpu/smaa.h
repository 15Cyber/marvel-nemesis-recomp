#pragma once
#include <atomic>
#include <cstdint>

namespace gpu {
enum class SmaaStatus : uint32_t { Off, Pending, Active, Unavailable };
// Renderer writes; the host panel only reads. Active means the passes were recorded.
inline std::atomic<SmaaStatus> g_smaa_status{SmaaStatus::Off};
}  // namespace gpu
