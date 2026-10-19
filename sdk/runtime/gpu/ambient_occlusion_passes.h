#pragma once

namespace gpu {
// Depth-only SSAO. Independent implementation: no game textures or reference-repo shader source.
inline constexpr char kAmbientOcclusionHlsl[] = R"hlsl(
cbuffer AO : register(b1) {
    float4 projection; // reciprocal focal lengths, off-centre terms
    float4 depthParams; // projection[14], projection[10], viewport min Z, Z range
    float4 viewport; // host pixels
    float4 fullSize; // reciprocal size, size
    float4 options; // world radius, strength, debug, maximum sample radius in pixels
    float4 aoSize; // reciprocal size, size
};
#ifdef AO_MS_DEPTH
Texture2DMS<float> sceneDepth : register(t0);
#else
Texture2D<float> sceneDepth : register(t0);
#endif
Texture2D<float2> aoImage : register(t1);
struct O { float4 p : SV_Position; };
O vs(uint id : SV_VertexID) {
    O o;
    o.p = float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
    return o;
}
bool Inside(float2 pixel) {
    return all(pixel >= viewport.xy) && all(pixel < viewport.xy + viewport.zw);
}
float ViewZ(float raw) {
    float d = (raw - depthParams.z) / depthParams.w;
    return depthParams.x / min(d - depthParams.y, -0.0000001);
}
float RawDepth(float2 pixel) {
    int2 p = clamp(int2(pixel), int2(0, 0), int2(fullSize.zw) - 1);
#ifdef AO_MS_DEPTH
    return sceneDepth.Load(p, 0);
#else
    return sceneDepth.Load(int3(p, 0));
#endif
}
bool ValidDepth(float raw) {
    return raw >= depthParams.z && raw < depthParams.z + depthParams.w - 0.0000001;
}
float3 Position(float2 pixel, float raw) {
    float z = ViewZ(raw);
    float2 ndc = (floor(pixel) + 0.5 - viewport.xy) / viewport.zw * float2(2, -2) + float2(-1, 1);
    return float3((ndc - projection.zw) * projection.xy * z, z);
}
float3 Normal(float2 pixel, float3 centre) {
    float3 r = Position(pixel + float2(1, 0), RawDepth(pixel + float2(1, 0))) - centre;
    float3 l = centre - Position(pixel - float2(1, 0), RawDepth(pixel - float2(1, 0)));
    float3 d = Position(pixel + float2(0, 1), RawDepth(pixel + float2(0, 1))) - centre;
    float3 u = centre - Position(pixel - float2(0, 1), RawDepth(pixel - float2(0, 1)));
    float3 dx = abs(r.z) < abs(l.z) ? r : l;
    float3 dy = abs(d.z) < abs(u.z) ? d : u;
    // Normalize tangents before testing the angle. A threshold on the raw cross
    // product depends on world units, distance and internal resolution, and could
    // replace valid floor normals with a camera-facing normal across a depth band.
    float dx2 = dot(dx, dx), dy2 = dot(dy, dy);
    if (dx2 <= 1e-20 || dy2 <= 1e-20) return float3(0, 0, 0);
    dx *= rsqrt(max(dx2, 1e-20));
    dy *= rsqrt(max(dy2, 1e-20));
    float3 n = cross(dx, dy);
    float length2 = dot(n, n);
    if (length2 <= 1e-8) return float3(0, 0, 0);
    n *= rsqrt(max(length2, 1e-8));
    return dot(n, -centre) < 0 ? -n : n;
}
float2 ao_raw(O i) : SV_Target {
    float2 pixel = i.p.xy * fullSize.zw / aoSize.zw;
    float raw = RawDepth(pixel);
    if (!Inside(pixel) || !ValidDepth(raw)) return float2(1, 0);
    float3 centre = Position(pixel, raw);
    float3 n = Normal(pixel, centre);
    // Missing/degenerate surface information must not invent a planar occluder.
    if (dot(n, n) < 0.5) return float2(1, centre.z);
    float radius = options.x;
    float2 projectedRadius = radius * viewport.zw / (2 * abs(projection.xy) * max(centre.z, 0.0001));
    projectedRadius = min(projectedRadius, options.ww);
    // Stable 4x4 rotation, filtered spatially; no temporal history or motion vectors required.
    uint2 cell = uint2(i.p.xy) & 3;
    float rotation = (cell.x + cell.y * 4) * 2.39996323;
    float occlusion = 0;
    [unroll] for (int k = 0; k < 16; ++k) {
        float angle = rotation + k * 2.39996323;
        float ring = sqrt((k + 0.5) / 16.0);
        float2 samplePixel = pixel + float2(cos(angle), sin(angle)) * projectedRadius * ring;
        if (!Inside(samplePixel)) continue;
        float sampleRaw = RawDepth(samplePixel);
        if (!ValidDepth(sampleRaw)) continue;
        float3 delta = Position(samplePixel, sampleRaw) - centre;
        float distance2 = dot(delta, delta);
        float distance = sqrt(max(distance2, 0.00000001));
        float falloff = saturate(1 - distance2 / (radius * radius));
        float hemisphere = saturate((dot(n, delta) / distance - 0.08) * 2);
        // Bias suppresses self occlusion on a locally flat surface.
        occlusion += hemisphere * falloff * smoothstep(radius * 0.01, radius * 0.04, distance);
    }
    return float2(max(0.2, 1 - options.y * occlusion / 16.0), centre.z);
}
float DepthWeight(float a, float b) {
    if (a <= 0 || b <= 0) return 0;
    return exp2(-abs(a - b) / max(options.x * 0.1, a * 0.002));
}
float2 ao_blur(O i) : SV_Target {
    int2 p = int2(i.p.xy);
    float2 centre = aoImage.Load(int3(p, 0));
    if (centre.y <= 0) return centre;
    float sum = 0, weight = 0;
    [unroll] for (int x = -2; x <= 2; ++x) {
        int2 q = clamp(p + int2(x, 0), int2(0, 0), int2(aoSize.zw) - 1);
        float2 tap = aoImage.Load(int3(q, 0));
        float w = (3 - abs(x)) * DepthWeight(centre.y, tap.y);
        sum += tap.x * w; weight += w;
    }
    return float2(weight > 0 ? sum / weight : 1, centre.y);
}
float4 ao_apply(O i) : SV_Target {
    float raw = RawDepth(i.p.xy);
    if (!Inside(i.p.xy) || !ValidDepth(raw)) return float4(1, 1, 1, 1);
    float3 centre = Position(i.p.xy, raw);
    if (options.z == 1) {
        float grey = saturate(log2(1 + centre.z) / 12);
        return float4(grey.xxx, 1);
    }
    if (options.z == 2) return float4(Normal(i.p.xy, centre) * 0.5 + 0.5, 1);
    // Vertical bilateral filtering and depth-aware upsampling to the original target.
    float2 at = i.p.xy * aoSize.zw / fullSize.zw - 0.5;
    int2 base = int2(floor(at));
    float2 f = frac(at);
    float sum = 0, weight = 0;
    [unroll] for (int y = -2; y <= 3; ++y) {
        [unroll] for (int x = 0; x <= 1; ++x) {
            int2 q = clamp(base + int2(x, y), int2(0, 0), int2(aoSize.zw) - 1);
            float2 tap = aoImage.Load(int3(q, 0));
            float w = (x ? f.x : 1 - f.x) * max(0, 3 - abs(y - f.y));
            w *= DepthWeight(centre.z, tap.y);
            sum += tap.x * w; weight += w;
        }
    }
    float factor = weight > 0.00001 ? sum / weight : 1;
    return float4(factor.xxx, 1);
}
)hlsl";
}  // namespace gpu
