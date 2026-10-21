#pragma once
#include <algorithm>
#include <cstdint>

namespace app {

struct FrameLayout {
    float aspect;       // aspect displayed by the final blit
    float ui_scale_x;   // clip-space width of the centred original canvas
};

inline FrameLayout LayoutFor(uint32_t width, uint32_t height, bool expand_widescreen, bool wide) {
    const float original = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    if (!width || !height) return {original, 1};
    const float aspect = expand_widescreen && wide ? std::max(original, float(width) / height) : original;
    return {aspect, original / aspect};
}

}  // namespace app
