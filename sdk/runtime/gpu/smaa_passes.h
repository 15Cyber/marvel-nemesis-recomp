#pragma once
#include <string>
#include "gpu/smaa_shader.h"

namespace gpu {

// SMAA 1x, high preset. The SDK's existing Pass() uses a common full-screen VS;
// affine UV/pixel offsets are computed in each PS instead of carried as VS outputs.
inline const std::string& SmaaSource() {
    static const std::string source = std::string(R"(
cbuffer SmaaParameters : register(b1) { float4 metrics; float4 unused; };
#define SMAA_RT_METRICS metrics
#define SMAA_CUSTOM_SL
#define SMAA_PRESET_HIGH
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)") + kSmaaHlsl + R"(
Texture2D source0 : register(t0);
Texture2D source1 : register(t1);
Texture2D source2 : register(t2);
struct SmaaVertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
SmaaVertex vs(uint id : SV_VertexID) {
    SmaaVertex result;
    result.uv = float2((id << 1) & 2, id & 2);
    result.position = float4(result.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return result;
}
float4 smaa_edges(SmaaVertex input) : SV_Target {
    float4 offset[3];
    SMAAEdgeDetectionVS(input.uv, offset);
    return float4(SMAALumaEdgeDetectionPS(input.uv, offset, source0), 0, 0);
}
float4 smaa_weights(SmaaVertex input) : SV_Target {
    float2 pixel;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(input.uv, pixel, offset);
    return SMAABlendingWeightCalculationPS(input.uv, pixel, offset, source0, source1, source2, 0);
}
float4 smaa_blend(SmaaVertex input) : SV_Target {
    float4 offset;
    SMAANeighborhoodBlendingVS(input.uv, offset);
    return SMAANeighborhoodBlendingPS(input.uv, offset, source0, source1);
}
)";
    return source;
}

}  // namespace gpu
