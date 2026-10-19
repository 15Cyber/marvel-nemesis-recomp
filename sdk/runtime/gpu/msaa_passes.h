#pragma once
namespace gpu {
// A depth texture used as a regular sampled texture needs a single value per pixel.
// Use sample zero, matching the existing point-depth path; never average depth/stencil.
inline constexpr char kMsaaDepthReadHlsl[] = R"(
Texture2DMS<float> image : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float depth_read(float4 position : SV_Position) : SV_Target {
    return image.Load(int2(position.xy), 0);
}
)";
}
