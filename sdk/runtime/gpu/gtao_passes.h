#pragma once

/*
GTAO horizon integration adapted from SCDA-XBERecomp src/gpu/d3d12.cpp
commit e8f8b443e97cc89c8b1f009df5b6b66e50a771ee.

MIT License

Copyright (c) 2026 TGP482, Joshhhuaaa

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

namespace gpu {

// GTAO for the existing half-resolution AO pipeline. Uses Position/Normal/Inside
// from ambient_occlusion_passes.h, including the approved scale-independent normal fix.
// Horizon integration adapted from SCDA's MIT-licensed kAoHlsl, with Marvel depth,
// viewport checks, radius cap, self-occlusion bias and shared strength/denoising.
inline constexpr char kGtaoHlsl[] = R"hlsl(
static const float kGtaoHalfPi = 1.57079632679;
static const float kGtaoDither[16] = {
    0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5
};
float GtaoHorizon(float2 pixel, float3 centre, float3 viewDirection, float radius, float flatHorizon) {
    // Missing/offscreen geometry contributes no invented edge occluder.
    if (!Inside(pixel)) return flatHorizon;
    float raw = RawDepth(pixel);
    if (!ValidDepth(raw)) return flatHorizon;
    float3 delta = Position(pixel, raw) - centre;
    float distance2 = dot(delta, delta);
    if (distance2 <= 1e-10 || distance2 >= radius * radius) return flatHorizon;
    float distance = sqrt(distance2);
    float weight = saturate((radius - distance) / (radius * 0.615));
    // A small angular bias suppresses numerical occlusion on the reconstructed plane.
    float horizon = clamp(dot(delta, viewDirection) / distance - 0.01, -1, 1);
    return lerp(flatHorizon, horizon, weight);
}
float2 gtao_raw(O i) : SV_Target {
    float2 pixel = i.p.xy * fullSize.zw / aoSize.zw;
    float raw = RawDepth(pixel);
    if (!Inside(pixel) || !ValidDepth(raw)) return float2(1, 0);
    float3 centre = Position(pixel, raw);
    float3 normal = Normal(pixel, centre);
    if (dot(normal, normal) < 0.5) return float2(1, centre.z);
    float3 viewDirection = -centre * rsqrt(max(dot(centre, centre), 1e-12));

    float2 pixelRadius = options.x * viewport.zw /
                         (2 * abs(projection.xy) * max(centre.z, 0.0001));
    float span = max(pixelRadius.x, pixelRadius.y);
    if (span < 1.5) return float2(1, centre.z);
    // A single cap factor preserves the projected circle and the matching world radius.
    float cap = min(1, options.w / span);
    pixelRadius *= cap;
    float radius = options.x * cap;
    uint2 cell = uint2(i.p.xy) & 3;
    float sliceNoise = kGtaoDither[cell.y * 4 + cell.x] / 16;
    float stepNoise = kGtaoDither[cell.x * 4 + cell.y] / 16;
    float visibility = 0;

    // Three hemisphere slices, three samples on either side: 18 depth reads,
    // plus centre/normal reconstruction. Stable spatial noise, no temporal history.
    [unroll] for (int slice = 0; slice < 3; ++slice) {
        float angle = (slice + sliceNoise) * (2 * kGtaoHalfPi / 3);
        float3 direction = float3(cos(angle), sin(angle), 0);
        float2 screenDirection = float2(direction.x, -direction.y) * pixelRadius;
        float screenLength = length(screenDirection);
        float3 tangent = direction - dot(direction, viewDirection) * viewDirection;
        float3 axis = cross(tangent, viewDirection);
        float axis2 = dot(axis, axis);
        if (axis2 <= 1e-12 || screenLength < 1.5) {
            visibility += 1;
            continue;
        }
        axis *= rsqrt(axis2);
        float3 projectedNormal = normal - axis * dot(normal, axis);
        float normalLength = length(projectedNormal);
        if (normalLength <= 1e-6) continue;
        float normalCos = saturate(dot(projectedNormal, viewDirection) / normalLength);
        float normalAngle = acos(normalCos);
        if (dot(tangent, projectedNormal) < 0) normalAngle = -normalAngle;

        float2 flatHorizon = float2(cos(normalAngle + kGtaoHalfPi), cos(normalAngle - kGtaoHalfPi));
        float2 horizon = flatHorizon;
        [unroll] for (int step = 0; step < 3; ++step) {
            float fraction = (step + stepNoise) / 3;
            float offset = min(1, fraction * fraction + 1.5 / screenLength);
            float2 delta = screenDirection * offset;
            horizon.x = max(horizon.x, GtaoHorizon(pixel + delta, centre, viewDirection, radius, flatHorizon.x));
            horizon.y = max(horizon.y, GtaoHorizon(pixel - delta, centre, viewDirection, radius, flatHorizon.y));
        }
        // Integrate the visible angular arc against the normal projected into this slice.
        float h0 = normalAngle + clamp(-acos(clamp(horizon.y, -1, 1)) - normalAngle,
                                        -kGtaoHalfPi, kGtaoHalfPi);
        float h1 = normalAngle + clamp(acos(clamp(horizon.x, -1, 1)) - normalAngle,
                                       -kGtaoHalfPi, kGtaoHalfPi);
        float normalSin = sin(normalAngle);
        float arc0 = (normalCos + 2 * h0 * normalSin - cos(2 * h0 - normalAngle)) / 4;
        float arc1 = (normalCos + 2 * h1 * normalSin - cos(2 * h1 - normalAngle)) / 4;
        visibility += normalLength * (arc0 + arc1);
    }
    float factor = pow(saturate(visibility / 3), 2.2);
    return float2(max(0.2, 1 - options.y * (1 - factor)), centre.z);
}
)hlsl";

}  // namespace gpu
