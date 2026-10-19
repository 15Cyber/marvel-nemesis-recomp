#pragma once
#include <cstdint>
#include "gpu/bloom.h"

namespace game::pc_render {

struct Settings {
    uint32_t object_distance = 100;  // Percentage: 100 (native), 150, 200 or 300.
    bool bloom = false;
    uint32_t bloom_strength = 25;
    uint32_t bloom_threshold = 75;
    uint32_t bloom_radius = 100;
    bool operator==(const Settings&) const = default;
};

Settings Read();
void Set(const Settings&);
void Load(const void*);
void Save(void*);
void Draw(Settings&);
uint32_t ObjectDistancePercent();  // Atomic snapshot for the guest render hook; no UI lock.
gpu::BloomParameters BloomSettings();  // Coherent atomic snapshot; no UI/D3D lock inversion.

}  // namespace game::pc_render
