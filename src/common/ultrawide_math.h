#pragma once
#include <cmath>

namespace game::ultrawide {

inline bool WidenProjection(float (&projection)[16], float horizontal_scale) {
    if (!std::isfinite(horizontal_scale) || horizontal_scale <= 0 || horizontal_scale > 1 ||
        projection[0] <= 0 || projection[5] <= 0 ||
        std::abs(projection[11]) < 1e-6f || std::abs(projection[15]) > 1e-6f) return false;
    for (float value : projection) if (!std::isfinite(value)) return false;
    // Row-vector D3D matrices: only the clip-X column changes; vertical FOV and depth stay native.
    for (unsigned row = 0; row < 4; ++row) projection[row * 4] *= horizontal_scale;
    return true;
}

inline float WidenConeSine(float sine, float expansion) {
    return sine * expansion / std::sqrt(1 + sine * sine * (expansion * expansion - 1));
}

}  // namespace game::ultrawide
