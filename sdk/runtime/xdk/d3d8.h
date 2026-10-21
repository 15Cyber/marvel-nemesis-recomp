#pragma once
#include <mutex>

#include "gpu/state.h"
#include "gpu/bloom.h"

namespace d3d {

gpu::State& State();
void RegisterShadowBuffer(uint32_t texture_header);  // title-owned silhouette map, 0 unregisters
void AmbientOcclusion();  // explicit scene boundary, called by the title hook
void Bloom(const gpu::BloomParameters&);  // after scene effects, before the HUD
std::recursive_mutex& Lock();

// Only the assignment holds the D3D lock; guest calls may take the UI lock.
class FullWidthScope {
    bool previous_;
public:
    explicit FullWidthScope(bool enabled = true) {
        std::lock_guard lock(Lock());
        previous_ = State().full_width;
        State().full_width = enabled;
    }
    ~FullWidthScope() {
        std::lock_guard lock(Lock());
        State().full_width = previous_;
    }
    FullWidthScope(const FullWidthScope&) = delete;
    FullWidthScope& operator=(const FullWidthScope&) = delete;
};

// Recorded geometry using the streams, textures and shaders already bound by the title.
void RunPushBuffer(uint32_t push_buffer, uint32_t fixup);

// Process-wide totals, including rejections beyond the detailed-report limit.
// Atomic snapshots allow shutdown to report without waiting for the D3D lock.
void LogPushBufferStats(const char* context);

}  // namespace d3d
