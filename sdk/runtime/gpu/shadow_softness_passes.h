#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace gpu {

struct ShadowSoftnessShader {
    std::string source;
    float sigma = 0;
    uint32_t samples = 0;  // bilinear reads per pixel, per axis
};

// A normalized, separable Gaussian on the colour silhouette, not depth PCF.
// Adjacent taps are paired into one bilinear read, with coefficients generated
// once per map size/mode instead of evaluating exp for every GPU pixel.
inline ShadowSoftnessShader MakeShadowSoftnessShader(uint32_t width, uint32_t mode) {
    ShadowSoftnessShader result;
    const float guest_sigma = mode == 1 ? 0.5f : 1.0f;
    result.sigma = std::clamp(guest_sigma * float(width) / 256.0f, 0.5f, 12.0f);
    const uint32_t radius = uint32_t(std::ceil(2 * result.sigma));
    std::vector<double> weights(radius + 1);
    double sum = 0;
    for (uint32_t i = 0; i <= radius; ++i) {
        weights[i] = std::exp(-0.5 * double(i) * i / (double(result.sigma) * result.sigma));
        sum += weights[i] * (i ? 2 : 1);
    }
    std::ostringstream hlsl;
    hlsl.imbue(std::locale::classic());  // HLSL numeric literals always use a decimal point
    hlsl << std::fixed << std::setprecision(10);
    hlsl << R"hlsl(
cbuffer B : register(b1) { float4 metrics; float4 unused; };
Texture2D<float4> image : register(t0);
SamplerState linearSampler : register(s0);
struct O { float4 p : SV_Position; };
O vs(uint id : SV_VertexID) {
    O o;
    o.p = float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
    return o;
}
float4 mask(float2 uv) {
    // Outside the silhouette mask is neutral black, never a repeated silhouette.
    if (any(uv < 0) || any(uv > 1)) return 0;
    return image.SampleLevel(linearSampler, uv, 0);
}
float4 shadow_soft(O i) : SV_Target {
    float2 uv = i.p.xy * metrics.xy;
    float4 centre = image.SampleLevel(linearSampler, uv, 0);
    // Keep a neutral outer texel so the game's clamp sampler cannot extend a
    // nonzero filtered edge across the scene. Alpha remains the source's.
    if (any(i.p.xy < 1) || any(i.p.xy > 1.0 / metrics.xy - 1))
        return float4(0, 0, 0, centre.a);
    float2 step = metrics.xy * metrics.zw;
)hlsl";
    hlsl << "    float4 value = centre * " << weights[0] / sum << ";\n";
    result.samples = 1;
    for (uint32_t i = 1; i <= radius; i += 2) {
        const double a = weights[i], b = i + 1 <= radius ? weights[i + 1] : 0;
        const double weight = (a + b) / sum;
        const double offset = double(i) + b / (a + b);
        hlsl << "    value += (mask(uv + step * " << offset << ") + mask(uv - step * "
             << offset << ")) * " << weight << ";\n";
        result.samples += 2;
    }
    hlsl << "    return float4(value.rgb, centre.a);\n}\n";
    result.source = hlsl.str();
    return result;
}

}  // namespace gpu
