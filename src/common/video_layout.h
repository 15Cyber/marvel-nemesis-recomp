#pragma once
#include <cstdint>

namespace game::pc_video {

struct Extent { float x = 1, y = 1; };

// Normalised video size; caller validates decoded dimensions before dividing.
inline Extent VideoExtent(uint32_t scale, float aspect, float film_aspect) {
    Extent result;
    if (scale == 3) {
        if (film_aspect > aspect) result.x = film_aspect / aspect;
        else result.y = aspect / film_aspect;
    } else {
        float canvas = aspect;
        if (scale == 2) {
            canvas = 4.0f / 3;
            if (aspect > canvas) result.x = canvas / aspect;
            else result.y = aspect / canvas;
        }
        if (film_aspect > canvas) result.y *= canvas / film_aspect;
        else result.x *= film_aspect / canvas;
    }
    return result;
}

}  // namespace game::pc_video
