#pragma once

namespace gpu {

// Independently written SDR bloom. RGB-only screen composition preserves the
// scene's per-sample colour differences and alpha; it is not an HDR output mode.
inline constexpr char kBloomHlsl[] = R"hlsl(
cbuffer BloomConstants : register(b1) {
    float4 metrics; // inverse source size, inverse destination size
    float4 tuning;  // threshold, intensity, blur step in source texels (x, y)
};
Texture2D<float4> image : register(t0);
SamplerState linearSampler : register(s0);
struct O { float4 p : SV_Position; };
O vs(uint id : SV_VertexID) {
    O o;
    o.p = float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
    return o;
}
float3 bright(float2 uv) {
    float3 colour = saturate(image.SampleLevel(linearSampler, uv, 0).rgb);
    float peak = max(colour.r, max(colour.g, colour.b));
    // Soft knee avoids a sudden on/off edge when a highlight crosses the threshold.
    const float knee = 0.10;
    float soft = clamp(peak - tuning.x + knee, 0, 2 * knee);
    soft = soft * soft / (4 * knee);
    float weight = saturate(max(peak - tuning.x, soft) / max(peak, 0.0001));
    return colour * weight;
}
float4 bloom_extract(O i) : SV_Target {
    float2 uv = i.p.xy * metrics.zw;
    float2 d = metrics.xy * 0.5;
    // Threshold before averaging, to retain small bright particles.
    return float4((bright(uv + d) + bright(uv - d) +
                   bright(uv + float2(d.x, -d.y)) + bright(uv + float2(-d.x, d.y))) * 0.25, 0);
}
float4 bloom_downsample(O i) : SV_Target {
    float2 uv = i.p.xy * metrics.zw;
    float2 d = metrics.xy * 0.5;
    return (image.SampleLevel(linearSampler, uv + d, 0) + image.SampleLevel(linearSampler, uv - d, 0) +
            image.SampleLevel(linearSampler, uv + float2(d.x, -d.y), 0) +
            image.SampleLevel(linearSampler, uv + float2(-d.x, d.y), 0)) * 0.25;
}
float4 bloom_blur(O i) : SV_Target {
    float2 uv = i.p.xy * metrics.zw;
    float2 step = metrics.xy * tuning.zw;
    // Normalized nine-tap Gaussian, paired into five bilinear reads.
    float3 value = image.SampleLevel(linearSampler, uv, 0).rgb * 0.2270270270;
    value += (image.SampleLevel(linearSampler, uv + step * 1.3846153846, 0).rgb +
              image.SampleLevel(linearSampler, uv - step * 1.3846153846, 0).rgb) * 0.3162162162;
    value += (image.SampleLevel(linearSampler, uv + step * 3.2307692308, 0).rgb +
              image.SampleLevel(linearSampler, uv - step * 3.2307692308, 0).rgb) * 0.0702702703;
    return float4(value, 0);
}
float4 bloom_apply(O i) : SV_Target {
    return float4(saturate(image.SampleLevel(linearSampler, i.p.xy * metrics.zw, 0).rgb * tuning.y), 0);
}
)hlsl";

}  // namespace gpu
