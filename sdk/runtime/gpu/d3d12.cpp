// The title's Direct3D 8 calls drawn with Direct3D 12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/ext.h"
#include "gpu/shaders.h"
#include "gpu/smaa.h"
#include "gpu/msaa.h"
#include "gpu/shadow_resolution.h"
#include "gpu/shadow_softness_passes.h"
#include "gpu/msaa_passes.h"
#include "gpu/ambient_occlusion_passes.h"
#include "gpu/gtao_passes.h"
#include "gpu/bloom_passes.h"
#include "gpu/smaa_passes.h"
#include "gpu/smaa_textures.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
constexpr uint64_t kUploadSize = 64ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr uint32_t kSrvCount = 3 * 32768;
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
constexpr uint32_t kDsvCount = 256;
constexpr uint32_t kSmaaRtv = kRtvCount + kFrames;  // three views after the swapchain views
constexpr uint32_t kAoRtv = kSmaaRtv + 3;  // two shared SSAO/GTAO intermediates
constexpr uint32_t kShadowSoftRtv = kAoRtv + 2;  // two colour-mask blur views
constexpr uint32_t kBloomRtv = kShadowSoftRtv + 2;  // half-size extraction and two quarter-size blur views
constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R32G32_FLOAT;
constexpr DXGI_FORMAT kSmaaEdges = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kSmaaWeights = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kQueries = 8192;  // occlusion queries in flight, several frames of counted draws
constexpr uint32_t kNoQuery = ~0u;

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct Perf {
    // milliseconds
    double draw = 0;
    double assemble = 0;
    double bind = 0;
    double sig = 0;
    double texup = 0;
    double compile = 0;
    double clear = 0;
    double copy = 0;
    double present = 0;
    double wait = 0;
    double swap = 0;
    double sync = 0;
    // counts and bytes
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload = 0;
    uint64_t overflow = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t texup_bytes = 0;
    uint64_t newtex = 0;
    uint64_t targets = 0;
    uint64_t rtups = 0;
    uint64_t shrinks = 0;
    uint64_t syncs = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t raw = 0;
    uint64_t lates = 0;
};
Perf g_perf;
struct PerfScope {
    double& acc;
    PerfClock::time_point t = PerfClock::now();
    ~PerfScope() { acc += PerfMs(t); }
};

// F3 captures only with diag::CaptureDir set
bool CaptureOn() {
    if (diag::CaptureDir().empty()) LOG_FIRST(1, "capture: set XBE_CAPTURE_DIR");
    return !diag::CaptureDir().empty();
}

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

const char* kDepthHlsl = R"(
#ifdef MS_SOURCE
Texture2DMS<float> img : register(t0);
#else
Texture2D<float> img : register(t0);
#endif
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i
#ifdef MS_DEST
, uint sample_index : SV_SampleIndex
#endif
) : SV_Depth {
#ifdef MS_SOURCE
    uint width, height, count;
    img.GetDimensions(width, height, count);
    int2 pixel = clamp(int2(i.uv * float2(width, height)), int2(0, 0), int2(width, height) - 1);
#ifdef SAME_SAMPLES
    return img.Load(pixel, sample_index);
#else
    return img.Load(pixel, 0);
#endif
#else
    return img.SampleLevel(ps, i.uv, 0);
#endif
}
)";

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

// D3DBLEND, the console uses GL values
constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

D3D12_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D12_BLEND_SRC_ALPHA;
        case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D12_BLEND_DEST_ALPHA;
        case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
        default: return D3D12_BLEND_ONE;
    }
}

// D3DBLENDOP, GL values
D3D12_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D12_BLEND_OP_MIN;
        case 0x8008: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207, D3D12's order from 1
D3D12_COMPARISON_FUNC Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return D3D12_COMPARISON_FUNC(n + 1);
}

// D3DSTENCILOP, GL values
D3D12_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_STENCIL_OP_ZERO;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// rows of a texture upload start D3D12_TEXTURE_DATA_PITCH_ALIGNMENT apart
uint32_t UploadPitch(uint32_t row_bytes) { return (row_bytes + 255) & ~255u; }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return bd;
}

// one level, one sample: callers set Flags where they differ
D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT fmt, uint64_t w, uint32_t h) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    return td;
}

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ComPtr<ID3D12Resource> res;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint8_t flags = 0;  // Extension::Decoded's
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t samples;
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
    uint32_t samples = 1;     // destination samples; source is always single-sampled
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    // into: a copy from an earlier call to draw over again, in state into_state
    ID3D12Resource* into = nullptr;
    D3D12_RESOURCE_STATES into_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    TexInfo info[4];
    D3D12_SHADER_RESOURCE_VIEW_DESC sv[4];
    ID3D12Resource* res[4];
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
};

// a draw's shaders: what they are made from, their cache keys, the hashes Compile returned and the compiled blobs
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    ID3DBlob* vs_code = nullptr;
    ID3DBlob* ps_code = nullptr;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

class D3D12Backend final : public Backend, public Context {
public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void AmbientOcclusion(const State& s, const AoParameters& parameters) override;
    void Bloom(const State& s, const BloomParameters& parameters) override;
    void SetShadowBuffer(const Surface& surface) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override { counted_.clear(); }
    // done once the list being recorded, the next fence value, has run
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, fence_value_ + 1}); }

    ID3D12Device* Device() override { return dev_.Get(); }
    ID3D12GraphicsCommandList* List() override { return cl_.Get(); }
    void Release(ComPtr<ID3D12Resource> r) override { CurrentFrame().garbage.push_back(std::move(r)); }
    D3D12_CPU_DESCRIPTOR_HANDLE View(const Target& t) override { return Cpu(rtv_heap_.Get(), t.view, rtv_inc_); }
    void Unbind() override { bound_rt_ = bound_ds_ = nullptr; }
    void Capture(Target& t, const char* tag, float mul) override;
    float Scale() const override { return scale_; }
    void GuestSize(uint32_t& w, uint32_t& h) const override {
        w = guest_w_;
        h = guest_h_;
    }
    uint64_t Presents() const override { return presents_; }

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12Resource> upload;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;
        uint32_t srv = 0;
    };
    struct Alloc {
        ID3D12Resource* res;
        uint64_t off;
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    uint32_t AllocSrvSlots(uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto c = h->GetCPUDescriptorHandleForHeapStart();
        c.ptr += size_t(i) * inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto g = h->GetGPUDescriptorHandleForHeapStart();
        g.ptr += uint64_t(i) * inc;
        return g;
    }
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) override;
    void Begin();
    void Submit(bool wait);
    void WaitFence(uint64_t value);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], bool gamma, bool alpha, uint32_t taps = 1,
              float mul = 1, DXGI_FORMAT rtv = kBgra) override {
        Blit(src, fmt, uvrect, BlitOptions{.gamma = gamma, .alpha = alpha, .taps = taps, .mul = mul, .rtv = rtv});
    }
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o);
    uint32_t EnsureBlitSamplers();
    void DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                        std::optional<uint32_t> sampler_slot = {});
    ID3D12Resource* Feedback(Target& t) override;
    struct Readable { ID3D12Resource* res; D3D12_RESOURCE_STATES* state; DXGI_FORMAT fmt; };
    Readable ReadTarget(Target& t);
    ID3D12Resource* ReadDepth(Target& t);
    uint32_t SelectSamples(uint32_t requested);
    void FailMsaa(const char* reason);
    void PublishMsaa();
    void PublishShadowResolution();
    void PublishShadowSoftness(ShadowSoftnessStatus status);
    void UpdateShadowSoftness();
    void MarkShadowWrite(const Target& target);
    void ReleaseShadowSoftTargets();
    bool EnsureShadowSoftTargets(const Target& target);
    ID3D12Resource* FilterShadowMask(Target& target);
    void DropShadowTarget();
    bool IsShadowBuffer(const Surface& surface, bool depth, uint32_t width, uint32_t height) const;
    void Resolve(Target& t) {
        if (t.range != 1 && ext_) ext_->Resolve(*this, t);
    }
    // Pass, then its step
    void Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
              uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float p[8],
              float blend) override;
    ID3D12PipelineState* EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv, float blend);
    // Draw, in call order, each callee right after its caller
    bool DepthCopy(const State& s);
    ID3D12PipelineState* EnsureDepthCopyPipeline(uint32_t source_samples, uint32_t destination_samples);
    void DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso);
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                      Assembled& a);
    void RenderAssembledDraw(const State& s, Assembled& a, uint32_t prim, uint32_t count);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D12Resource* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D12Resource* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                      DrawTextures& stages);
    ID3D12Resource* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                     DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours,
                       bool same);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    ID3D12PipelineState* EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps);
    void FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    void ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d);
    float PointScale() const;  // host pixels per console pixel of the bound target
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c);
    void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void CaptureSkippedDraw(const State& s, uint32_t prim, uint32_t count, const char* reason);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    uint32_t EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp);
    void SetViewportScissor(const State& s, uint32_t gw);
    void SetBlendFactorStencilRef(const State& s);
    void DrawGeometry(const Assembled& a);
    void UploadVertexBuffers(const Assembled& a);
    // visibility tests
    bool EnsureQueries();
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query);
    float SamplesPerPixel() const;
    void DeliverReports();
    // shaders by source hash: Compile returns the bytecode and sets hash
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    // Blit's pipeline
    ID3D12PipelineState* EnsureBlitPipeline(DXGI_FORMAT rtv, bool over = false, uint32_t samples = 1);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv) const;
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    void CreateHeaps();
    uint32_t CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out);
    void CreateBackBufferViews();
    bool CreateRootSignature();
    void CreateFrames();
    void CreateGammaLut();
    void LoadSettings();
    // Present, in call order, each callee right after its caller; DrawFrame runs the two after it
    void FinishCapture(const Surface& front);
    void StartCapture();
    void ResizeSwap();
    void UploadGammaRamp(uint32_t brightness, uint32_t contrast);
    ID3D12Resource* PrepareSmaa(Target* target);
    bool EnsureSmaaLookups();
    bool CreateSmaaLookup(ComPtr<ID3D12Resource>& texture, DXGI_FORMAT fmt,
                          uint32_t width, uint32_t height, uint32_t bytes_per_pixel, const uint8_t* data);
    bool EnsureSmaaTargets(DXGI_FORMAT fmt, uint32_t width, uint32_t height);
    void ReleaseSmaaTargets();
    void DrawFrame(Target* t, uint32_t bi);
    void BlitFrame(Target& t, ID3D12Resource* image);
    void RenderUi();
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    void UpdateAoSettings(uint32_t mode);
    void ReleaseBloomTargets();
    bool EnsureBloomTargets(uint32_t width, uint32_t height);
    ID3D12PipelineState* EnsureBloomPipeline(uint32_t pass, DXGI_FORMAT format, uint32_t samples);
    void BloomPass(ID3D12PipelineState* pipeline, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                   uint32_t width, uint32_t height, ID3D12Resource* image, DXGI_FORMAT format,
                   const float constants[8]);
    void ReleaseAoTargets();
    bool EnsureAoTargets(uint32_t width, uint32_t height);
    ID3D12PipelineState* EnsureAoPipeline(uint32_t pass, DXGI_FORMAT format,
                                         uint32_t samples, uint32_t depth_samples, bool debug);
    void AoPass(ID3D12PipelineState* pipeline, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                uint32_t width, uint32_t height, Target& depth, ID3D12Resource* image,
                const float constants[24]);
    ComPtr<ID3D12Resource> Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt, uint32_t w,
                                  uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapStep(const Surface& rt);
    // a resource CapSave shrinks to w x h and writes, in state *state, colour times mul
    struct CapImage {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    void CapSave(Target& t, const char* name);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth, uint64_t alt);
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    bool TargetStale(uint32_t data) const;
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t, const Surface& s);
    bool CreateTargetView(Target& t);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D12Resource> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                    const CopyList& l);
    void CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                      const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        ID3D12Resource* res;
        ComPtr<ID3D12Resource> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    ID3D12Resource* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> smp_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    uint32_t srv_inc_ = 0;
    uint32_t smp_inc_ = 0;
    uint32_t rtv_inc_ = 0;
    uint32_t dsv_inc_ = 0;
    std::vector<uint32_t> free_rtv_;
    std::vector<uint32_t> free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psos_;
    std::unordered_map<uint64_t, uint32_t> samplers_;
    uint32_t smp_used_ = 0;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> blit_psos_;
    Extension* ext_ = nullptr;
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    struct CapTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT fmt;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ComPtr<ID3D12Resource> lut_;
    D3D12_RESOURCE_STATES lut_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool ramp_valid_ = false;
    uint32_t ramp_settings_ = ~0u;
    bool gamma_ = false;
    ComPtr<ID3D12Resource> smaa_targets_[3], smaa_lookups_[2];
    D3D12_RESOURCE_STATES smaa_states_[3] = {};
    uint32_t smaa_width_ = 0, smaa_height_ = 0;
    DXGI_FORMAT smaa_format_ = DXGI_FORMAT_UNKNOWN;
    bool smaa_enabled_ = false, smaa_failed_ = false;
    ComPtr<ID3D12Resource> ao_targets_[2];
    D3D12_RESOURCE_STATES ao_states_[2] = {};
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> ao_pipelines_;
    uint32_t ao_width_ = 0, ao_height_ = 0;
    uint64_t ao_frame_ = ~0ull;
    uint32_t ao_mode_ = 0, ao_frame_mode_ = 0;
    bool ao_failed_ = false, ao_logged_ = false;
    uint32_t ao_skip_logs_ = 0;
    ComPtr<ID3D12Resource> bloom_targets_[3];
    D3D12_RESOURCE_STATES bloom_states_[3] = {};
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> bloom_pipelines_;
    uint32_t bloom_width_ = 0, bloom_height_ = 0;
    uint64_t bloom_frame_ = ~0ull;
    bool bloom_enabled_ = false, bloom_failed_ = false, bloom_logged_ = false;
    uint32_t bloom_skip_logs_ = 0;
    uint32_t shadow_data_ = 0, shadow_set_ = 0, shadow_extent_ = 0;
    bool shadow_failed_ = false;
    ComPtr<ID3D12Resource> shadow_soft_targets_[2];
    D3D12_RESOURCE_STATES shadow_soft_states_[2] = {};
    uint32_t shadow_soft_width_ = 0, shadow_soft_height_ = 0, shadow_soft_mode_ = 0;
    uint32_t shadow_soft_shader_mode_ = ~0u;
    DXGI_FORMAT shadow_soft_format_ = DXGI_FORMAT_UNKNOWN;
    ShadowSoftnessShader shadow_soft_shader_;
    bool shadow_soft_dirty_ = true, shadow_soft_failed_ = false, shadow_soft_logged_ = false;
    uint32_t msaa_set_ = 0, msaa_samples_ = 1;
    bool msaa_failed_ = false, msaa_remake_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        ComPtr<ID3D12Resource> rb[kFrames];
        ComPtr<ID3D12Resource> down;
        D3D12_RESOURCE_STATES down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t row_pitch = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                      // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};    // presents_ for DeliverLate
    Assembled asm_;  // kept between draws for its buffers
    // visibility tests: an occlusion query per counted draw, summed into report memory once the GPU is done
    struct Query {
        uint32_t index;
        float samples_per_pixel;  // host samples one console pixel covers
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
        uint64_t fence;
    };
    ComPtr<ID3D12QueryHeap> query_heap_;
    ComPtr<ID3D12Resource> query_rb_;
    const uint64_t* query_counts_ = nullptr;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;  // since the last ClearReport
    std::deque<Report> reports_;
    bool tearing_ = false;
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    CreateHeaps();
    CreateBackBufferViews();
    if (!CreateRootSignature()) return false;
    CreateFrames();
    CreateGammaLut();
    ImGui_ImplDX12_Init(dev_.Get(), kFrames, DXGI_FORMAT_B8G8R8A8_UNORM, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    LoadSettings();
    Begin();
    LOGI("d3d12: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool D3D12Backend::CreateDevice() {
    if (GetEnvironmentVariableW(L"XBE_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("d3d12: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D12Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        LOGE("d3d12: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    return true;
}

uint32_t D3D12Backend::CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible,
                                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = t;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
    return dev_->GetDescriptorHandleIncrementSize(t);
}

void D3D12Backend::CreateBackBufferViews() {
    for (int i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + i, rtv_inc_));
    }
}

void D3D12Backend::CreateHeaps() {
    srv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, true, srv_heap_);
    smp_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, smp_heap_);
    rtv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBloomRtv + 3, false, rtv_heap_);
    dsv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, ui_heap_);
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
}

bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
    dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root_));
    return true;
}

void D3D12Backend::CreateFrames() {
    for (auto& f : frames_) {
        dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&f.upload));
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map));
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&cl_));
}

// gamma ramp as a 256 x 1 lookup
void D3D12Backend::CreateGammaLut() {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&lut_));
}

void D3D12Backend::LoadSettings() {
    shadow_set_ = NormalizeShadowResolution(app::ReadSettings().shadow_resolution);
    PublishShadowResolution();
    shadow_soft_mode_ = NormalizeShadowSoftness(app::ReadSettings().shadow_softness);
    PublishShadowSoftness(shadow_soft_mode_ ? ShadowSoftnessStatus::Pending : ShadowSoftnessStatus::Off);
    msaa_set_ = NormalizeMsaa(app::ReadSettings().msaa);
    msaa_samples_ = SelectSamples(msaa_set_);
    PublishMsaa();
    scale_ = app::RenderScale(guest_h_);
    ext_ = g_extension.get();
    if (ext_) {
        ext_->Settings();
        ext_->Reset(*this);
    }
}

void D3D12Backend::Begin() {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Submit(bool wait) {
    cl_->Close();
    ID3D12CommandList* lists[] = {cl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fence_value_);
    Frame& f = CurrentFrame();
    f.fence = fence_value_;
    if (wait) {
        WaitFence(fence_value_);
        f.alloc->Reset();
        f.garbage.clear();
        f.used = 0;
        f.srv = 0;
        cl_->Reset(f.alloc.Get(), nullptr);
        Begin();
    }
}

void D3D12Backend::Flush() { Submit(true); }

void D3D12Backend::WaitFence(uint64_t value) {
    fence_->SetEventOnCompletion(value, event_);
    WaitForSingleObject(event_, INFINITE);
}

void D3D12Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D12Backend::SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

D3D12Backend::Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.upload->GetGPUVirtualAddress() + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    constexpr uint64_t kOwnAlign = 0x10000;  // a committed buffer's size granularity
    D3D12_RESOURCE_DESC bd = BufferDesc((size + kOwnAlign - 1) & ~(kOwnAlign - 1));
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                  IID_PPV_ARGS(&r));
    uint8_t* p = nullptr;
    r->Map(0, nullptr, reinterpret_cast<void**>(&p));
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

uint32_t D3D12Backend::AllocSrvSlots(uint32_t n) {
    Frame& f = CurrentFrame();
    const uint32_t per = kSrvCount / kFrames;
    if (f.srv + n > per) {
        LOG_FIRST(4, "d3d12: descriptor ring wrapped within a frame");
        f.srv = 0;
    }
    uint32_t i = uint32_t(frame_ % kFrames) * per + f.srv;
    f.srv += n;
    return i;
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (cur == want) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    cur = want;
}

// a resized target gives back its view and its resources
void D3D12Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    MarkShadowWrite(it->second);
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    (it->second.depth ? free_dsv_ : free_rtv_).push_back(it->second.view);
    CurrentFrame().garbage.push_back(it->second.res);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    if (it->second.fb_view != ~0u) free_rtv_.push_back(it->second.fb_view);
    targets_.erase(it);
}

// Common colour/depth formats must all support a sample count. Never mix counts in a draw.
uint32_t D3D12Backend::SelectSamples(uint32_t requested) {
    for (uint32_t count : {8u, 4u, 2u}) {
        if (count > requested) continue;
        bool supported = true;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT depth_read{DXGI_FORMAT_R24_UNORM_X8_TYPELESS};
        if (FAILED(dev_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &depth_read, sizeof(depth_read))) ||
            !(depth_read.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD)) supported = false;
        for (DXGI_FORMAT format : {kBgra, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT}) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
            levels.Format = format;
            levels.SampleCount = count;
            if (FAILED(dev_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) ||
                !levels.NumQualityLevels) { supported = false; break; }
            if (format != DXGI_FORMAT_D24_UNORM_S8_UINT) {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT flags{format};
                const auto required = D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET |
                                      D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE;
                if (FAILED(dev_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &flags, sizeof(flags))) ||
                    (flags.Support1 & required) != required) { supported = false; break; }
            }
        }
        if (supported) {
            LOGR("d3d12: MSAA requested=%u effective=%u quality=0", requested, count);
            return count;
        }
    }
    LOGR("d3d12: MSAA requested=%u effective=1%s", requested, requested ? " (unsupported; disabled)" : " (off)");
    return 1;
}

void D3D12Backend::PublishMsaa() {
    const bool fallback = msaa_failed_ || (msaa_set_ && msaa_samples_ < msaa_set_);
    g_msaa_state.store(msaa_set_ | msaa_samples_ << 8 | uint32_t(fallback) << 16);
}

void D3D12Backend::FailMsaa(const char* reason) {
    if (msaa_samples_ <= 1 || msaa_failed_) return;
    msaa_failed_ = true;
    msaa_remake_ = true;
    PublishMsaa();
    LOGE("d3d12: MSAA fallback scheduled (%s); recreating targets without MSAA at next Present. "
         "Toggle off/apply/on/apply to retry.", reason);
}

void D3D12Backend::PublishShadowSoftness(ShadowSoftnessStatus status) {
    g_shadow_softness_state = shadow_soft_mode_ | (uint32_t(status) << 8);
}

void D3D12Backend::UpdateShadowSoftness() {
    const uint32_t mode = NormalizeShadowSoftness(app::ReadSettings().shadow_softness);
    if (mode == shadow_soft_mode_) return;
    shadow_soft_mode_ = mode;
    shadow_soft_failed_ = shadow_soft_logged_ = false;
    shadow_soft_dirty_ = true;
    if (!mode) ReleaseShadowSoftTargets();
    PublishShadowSoftness(mode ? ShadowSoftnessStatus::Pending : ShadowSoftnessStatus::Off);
    LOGR("d3d12: shadow softness requested=%u (0 original, 1 light, 2 medium)", mode);
}

void D3D12Backend::MarkShadowWrite(const Target& target) {
    if (shadow_data_ && !target.depth && target.data == shadow_data_ && target.gw == 256 && target.gh == 256)
        shadow_soft_dirty_ = true;
}

void D3D12Backend::ReleaseShadowSoftTargets() {
    for (auto& texture : shadow_soft_targets_) if (texture) Release(std::move(texture));
    shadow_soft_width_ = shadow_soft_height_ = 0;
    shadow_soft_format_ = DXGI_FORMAT_UNKNOWN;
    shadow_soft_shader_ = {};
    shadow_soft_shader_mode_ = ~0u;
    shadow_soft_dirty_ = true;
    if (shadow_soft_mode_ && !shadow_soft_failed_) PublishShadowSoftness(ShadowSoftnessStatus::Pending);
}

bool D3D12Backend::EnsureShadowSoftTargets(const Target& target) {
    if (shadow_soft_targets_[1] && shadow_soft_width_ == target.w &&
        shadow_soft_height_ == target.h && shadow_soft_format_ == target.fmt) return true;
    ReleaseShadowSoftTargets();
    // Dedicated RTV descriptors may still be used by previous frames.
    Flush();
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    auto desc = Texture2DDesc(target.fmt, target.w, target.h);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (uint32_t i = 0; i < 2; ++i) {
        shadow_soft_states_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, shadow_soft_states_[i], nullptr,
                                                 IID_PPV_ARGS(&shadow_soft_targets_[i])))) {
            ReleaseShadowSoftTargets();
            return false;
        }
        dev_->CreateRenderTargetView(shadow_soft_targets_[i].Get(), nullptr,
                                     Cpu(rtv_heap_.Get(), kShadowSoftRtv + i, rtv_inc_));
    }
    shadow_soft_width_ = target.w;
    shadow_soft_height_ = target.h;
    shadow_soft_format_ = target.fmt;
    shadow_soft_logged_ = false;
    LOGR("d3d12: shadow softness targets %ux%u format=%u", target.w, target.h, unsigned(target.fmt));
    return true;
}

ID3D12Resource* D3D12Backend::FilterShadowMask(Target& target) {
    ID3D12Resource* original = target.res.Get();  // caller already transitioned it to an SRV
    if (!shadow_soft_mode_ || shadow_soft_failed_) return original;
    auto unavailable = [&](const char* reason) -> ID3D12Resource* {
        shadow_soft_failed_ = true;
        ReleaseShadowSoftTargets();
        PublishShadowSoftness(ShadowSoftnessStatus::Unavailable);
        LOGE("d3d12: shadow softness unavailable (%s); original mask used. Toggle Original/apply to retry.", reason);
        return original;
    };
    if (!target.w || target.w != target.h || target.samples != 1 ||
        (target.fmt != kBgra && target.fmt != DXGI_FORMAT_R8G8B8A8_UNORM))
        return unavailable("unsupported silhouette format, dimensions or samples");
    if (!EnsureShadowSoftTargets(target)) return unavailable("GPU allocation failed");
    if (!shadow_soft_dirty_) return shadow_soft_targets_[1].Get();
    if (shadow_soft_shader_.source.empty() || shadow_soft_shader_mode_ != shadow_soft_mode_) {
        shadow_soft_shader_ = MakeShadowSoftnessShader(target.w, shadow_soft_mode_);
        shadow_soft_shader_mode_ = shadow_soft_mode_;
    }
    const char* shader = shadow_soft_shader_.source.c_str();
    if (!EnsurePassPipeline(shader, "shadow_soft", target.fmt, -1))
        return unavailable("shader/pipeline creation failed");

    // Two passes and the next scene draw must not wrap the upload/SRV rings.
    if (CurrentFrame().srv + 16 > kSrvCount / kFrames ||
        CurrentFrame().used > kUploadSize - kUploadReserve) Flush();
    Transition(original, target.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(shadow_soft_targets_[0].Get(), shadow_soft_states_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* images[3] = {original, nullptr, nullptr};
    const DXGI_FORMAT formats[3] = {target.fmt, kBgra, kBgra};
    float parameters[8] = {1.0f / target.w, 1.0f / target.h, 1, 0, 0, 0, 0, 0};
    Pass(shader, "shadow_soft", target.fmt, Cpu(rtv_heap_.Get(), kShadowSoftRtv, rtv_inc_),
         target.w, target.h, images, formats, parameters, -1);
    Transition(shadow_soft_targets_[0].Get(), shadow_soft_states_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(shadow_soft_targets_[1].Get(), shadow_soft_states_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    images[0] = shadow_soft_targets_[0].Get();
    parameters[2] = 0;
    parameters[3] = 1;
    Pass(shader, "shadow_soft", target.fmt, Cpu(rtv_heap_.Get(), kShadowSoftRtv + 1, rtv_inc_),
         target.w, target.h, images, formats, parameters, -1);
    Transition(shadow_soft_targets_[1].Get(), shadow_soft_states_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // Further receiver meshes reuse this result until a clear, draw, upload or
    // copy writes the shared silhouette again for the next character.
    shadow_soft_dirty_ = false;
    PublishShadowSoftness(ShadowSoftnessStatus::Active);
    if (!shadow_soft_logged_) {
        LOGR("d3d12: shadow softness active mode=%u map=%ux%u sigma=%.3f bilinear_reads_per_axis=%u passes=2",
             shadow_soft_mode_, target.w, target.h, shadow_soft_shader_.sigma, shadow_soft_shader_.samples);
        shadow_soft_logged_ = true;
    }
    return shadow_soft_targets_[1].Get();
}

bool D3D12Backend::IsShadowBuffer(const Surface& surface, bool depth, uint32_t width, uint32_t height) const {
    return shadow_data_ && !depth && width == 256 && height == 256 &&
           ((surface.format >> 8) & 0xFF) == 7 && mem::VirtualFromPhysical(surface.data) == shadow_data_;
}

void D3D12Backend::PublishShadowResolution() {
    g_shadow_resolution_state = uint64_t(shadow_set_) | uint64_t(shadow_extent_) << 16 |
                                uint64_t(shadow_failed_) << 32;
}

void D3D12Backend::DropShadowTarget() {
    ReleaseShadowSoftTargets();
    shadow_soft_logged_ = false;
    if (!shadow_data_) return;
    auto it = targets_.find(SizedTargetKey(shadow_data_, false, 256, 256));
    if (it == targets_.end() || it->second.depth || it->second.gw != 256 || it->second.gh != 256) return;
    // The descriptor can be reused immediately by the next shadow draw, so finish
    // previous uses before returning its slot to the pool. The main scene stays intact.
    Flush();
    draw_rt_ = draw_ds_ = nullptr;
    ReleaseTarget(it);
}

void D3D12Backend::SetShadowBuffer(const Surface& surface) {
    uint32_t width = 0, height = 0;
    if (surface.header) SurfaceSize(surface, width, height);
    const bool valid = surface.header && surface.data && width == 256 && height == 256 &&
                       ((surface.format >> 8) & 0xFF) == 7;
    const uint32_t data = valid ? mem::VirtualFromPhysical(surface.data) : 0;
    if (surface.header && !valid)
        LOGR("shadows: unexpected ShadowBuffer dimensions/format (%ux%u format=%02X); independent resolution not applied",
             width, height, (surface.format >> 8) & 0xFF);
    if (data == shadow_data_) return;
    DropShadowTarget();
    shadow_data_ = data;
    if (shadow_soft_mode_ && !shadow_soft_failed_) PublishShadowSoftness(ShadowSoftnessStatus::Pending);
    shadow_extent_ = 0;
    shadow_failed_ = false;
    PublishShadowResolution();
    if (data) LOGR("shadows: registered ShadowBuffer data=%08X guest=%ux%u format=7", data, width, height);
    else LOGR("shadows: ShadowBuffer unregistered");

}

// the host texture of a new target, at render scale or the extension's
bool D3D12Backend::CreateTargetResource(Target& t, const Surface& s) {
    float sc = scale_;
    if (ext_) ext_->MakeTarget(s, t.depth, t.gw, t.gh, sc, t.fmt);
    const bool shadow = IsShadowBuffer(s, t.depth, t.gw, t.gh);
    const bool custom_shadow = shadow && shadow_set_ && !shadow_failed_;
    if (custom_shadow) sc = float(shadow_set_) / t.gw;
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_UNKNOWN, t.w, t.h);
    // Only back-buffer-sized colour/depth targets participate; maps and small chains keep one sample.
    t.samples = t.gw == guest_w_ && t.gh == guest_h_ ? msaa_samples_ : 1;
    td.SampleDesc.Count = t.samples;
    D3D12_CLEAR_VALUE cv{};
    if (t.depth) {
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        cv.DepthStencil.Depth = 1;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        td.Format = t.fmt;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    HRESULT result = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv,
                                                    IID_PPV_ARGS(&t.res));
    if (FAILED(result) && custom_shadow) {
        // Only this optional map falls back; scene targets and MSAA are unaffected.
        shadow_failed_ = true;
        LOGE("d3d12: shadow resolution %u allocation failed HRESULT=%08X; retrying with normal render scale",
             shadow_set_, uint32_t(result));
        t.w = std::max(1u, uint32_t(std::lround(t.gw * scale_)));
        t.h = std::max(1u, uint32_t(std::lround(t.gh * scale_)));
        td.Width = t.w;
        td.Height = t.h;
        result = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv, IID_PPV_ARGS(&t.res));
    }
    if (shadow) {
        shadow_extent_ = SUCCEEDED(result) ? t.w : 0;
        PublishShadowResolution();
        LOGR("d3d12: ShadowBuffer target data=%08X guest=%ux%u host=%ux%u requested=%u samples=%u fallback=%u allocated=%u",
             shadow_data_, t.gw, t.gh, t.w, t.h, shadow_set_, t.samples, unsigned(shadow_failed_), unsigned(SUCCEEDED(result)));
    }
    if (FAILED(result) && t.samples > 1) {
        LOGE("d3d12: MSAA target allocation HRESULT=%08X", uint32_t(result));
        FailMsaa("target allocation failed");
    }
    if (SUCCEEDED(result) && t.samples > 1)
        LOG_FIRST(12, "d3d12: MSAA %s target guest=%ux%u host=%ux%u samples=%u",
                  t.depth ? "depth" : "colour", t.gw, t.gh, t.w, t.h, t.samples);
    return SUCCEEDED(result);
}

// a view from the free pool, cleared; false when the pool is empty
bool D3D12Backend::CreateTargetView(Target& t) {
    auto& pool = t.depth ? free_dsv_ : free_rtv_;
    if (pool.empty()) return false;
    t.view = pool.back();
    pool.pop_back();
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = t.samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Cpu(dsv_heap_.Get(), t.view, dsv_inc_));
        float one = 1;
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                   D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, one, 0, 0, nullptr);
    } else {
        dev_->CreateRenderTargetView(t.res.Get(), nullptr, Cpu(rtv_heap_.Get(), t.view, rtv_inc_));
        float black[4] = {0, 0, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
    }
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t D3D12Backend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

// alt: the extension's bits of the key
D3D12Backend::TargetSlot D3D12Backend::FindTargetSlot(const Surface& s, bool depth, uint64_t alt) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh) | alt;
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* D3D12Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    if (!slot.sized) return nullptr;
    target_used_[slot.data] = frame_;
    return &slot.it->second;
}

// the surface's target, made again at its size when that changed
Target* D3D12Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    target_used_[slot.data] = frame_;
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, s)) return nullptr;
    g_perf.targets++;
    // an empty pool drops the new resource with t
    if (!CreateTargetView(t)) {
        if (IsShadowBuffer(s, depth, t.gw, t.gh)) {
            shadow_extent_ = 0;
            PublishShadowResolution();
            LOG_FIRST(1, "shadows: could not allocate a target view; see GPU resource usage");
        }
        if (t.samples > 1) FailMsaa("target view pool exhausted");
        return nullptr;
    }
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

bool D3D12Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (rt && ds && rt->samples != ds->samples) {
        FailMsaa("colour/depth sample counts differ");
        return false;  // never silently discard depth testing to make a draw fit
    }
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt->drawn = true;
        MarkShadowWrite(*rt);
    }
    if (ds) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ds->drawn = true;
        MarkShadowWrite(*ds);
    }
    if (rt != bound_rt_ || ds != bound_ds_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rt ? Cpu(rtv_heap_.Get(), rt->view, rtv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? Cpu(dsv_heap_.Get(), ds->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rh : nullptr, FALSE, ds ? &dh : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
    }
    return true;
}

constexpr uint64_t kDepthCopyKey = ~0ull;  // DepthCopy's pipeline in blit_psos_

// DepthCopy's pipeline, kept in blit_psos_: writes depth only, always passing; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureDepthCopyPipeline(uint32_t source_samples, uint32_t destination_samples) {
    auto& pso = blit_psos_[kDepthCopyKey ^ (uint64_t(source_samples) << 8 | uint64_t(destination_samples) << 16)];
    if (pso) return pso.Get();
    uint64_t h;
    std::string source;
    if (source_samples > 1) source += "#define MS_SOURCE\n";
    if (destination_samples > 1) source += "#define MS_DEST\n";
    if (source_samples > 1 && source_samples == destination_samples) source += "#define SAME_SAMPLES\n";
    source += kDepthHlsl;
    ID3DBlob* vs = Compile(source, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(source, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    p.SampleDesc.Count = destination_samples;
    p.RasterizerState.MultisampleEnable = destination_samples > 1;
    if (FAILED(dev_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// src's depth drawn into dst's depth buffer
void D3D12Backend::DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso) {
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    Transition(src.res.Get(), src.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), dst.view, dsv_inc_);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetFullViewport(dst.w, dst.h);
    uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sv.ViewDimension = src.samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (src.samples == 1) sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src.res.Get(), &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    Alloc cb = Upload(256, 256);
    DrawFullScreen(pso, cb.gpu, slot);
    dst.drawn = true;
    MarkShadowWrite(dst);
    bound_rt_ = bound_ds_ = nullptr;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool D3D12Backend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    ID3D12PipelineState* pso = EnsureDepthCopyPipeline(src.samples, dst.samples);
    if (!pso) {
        if (src.samples > 1 || dst.samples > 1) {
            FailMsaa("depth copy pipeline failed");
            return true;  // handled as a failed depth copy, never fall through to a colour draw
        }
        return false;
    }
    DrawDepthCopy(src, dst, pso);
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left as a shader resource
ComPtr<ID3D12Resource> D3D12Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h) {
    ComPtr<ID3D12Resource> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&tex));
    uint32_t row = UploadPitch(w * 4);
    Alloc a = Upload(uint64_t(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], w * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = a.off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, row};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tex.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void D3D12Backend::UploadTarget(Target& t, const Surface& s) {
    MarkShadowWrite(t);
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d12: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D12Resource> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    float mul = 1 / t.range;
    Blit(tex.Get(), kBgra, uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over, .samples = t.samples});
    CurrentFrame().garbage.push_back(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// samplers_ key of the full screen passes' samplers, apart from the stage samplers' hashed keys
constexpr uint64_t kBlitSamplerKey = 0xB117;

// four clamp samplers: linear at slot 0, point at slots 1 to 3
uint32_t D3D12Backend::EnsureBlitSamplers() {
    uint64_t key = kBlitSamplerKey;
    auto it = samplers_.find(key);
    uint32_t si;
    if (it == samplers_.end()) {
        if (smp_used_ + 4 > kSamplerCount) {
            samplers_.clear();
            smp_used_ = 0;
        }
        si = smp_used_;
        smp_used_ += 4;
        for (int i = 0; i < 4; i++) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            dev_->CreateSampler(&sd, Cpu(smp_heap_.Get(), si + i, smp_inc_));
        }
        samplers_[key] = si;
    } else {
        si = it->second;
    }
    return si;
}

// the full screen triangle: cb at b0 and b1, views at srv_slot; without a sampler slot EnsureBlitSamplers gives it
void D3D12Backend::DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                                  std::optional<uint32_t> sampler_slot) {
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(0, cb);
    cl_->SetGraphicsRootConstantBufferView(1, cb);
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), srv_slot, srv_inc_));
    // without a slot the passes take the blit samplers, looked up at this point as they always were
    uint32_t si = sampler_slot ? *sampler_slot : EnsureBlitSamplers();
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), si, smp_inc_));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
}

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o) {
    uint32_t slot = AllocSrvSlots(2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    uint32_t si = EnsureBlitSamplers();
    Alloc cb = Upload(256, 256);
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                  o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    std::memcpy(cb.cpu, c, sizeof(c));
    auto* pso = EnsureBlitPipeline(o.rtv, o.over, o.samples);
    if (!pso) { if (o.samples > 1) FailMsaa("copy/upload pipeline failed"); return; }
    DrawFullScreen(pso, cb.gpu, slot, si);
}

ID3DBlob* D3D12Backend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    // one source can hold several entry points
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    hash = Hash(src.data(), src.size(), entry_seed);
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        LOGE("d3d12: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    blobs_[hash] = code;
    return code.Get();
}

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12Backend::FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps,
                                                                        DXGI_FORMAT rtv) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = 1;
    return d;
}

ID3D12PipelineState* D3D12Backend::EnsureBlitPipeline(DXGI_FORMAT rtv, bool over, uint32_t samples) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32 | uint64_t(samples) << 40];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kBlitHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kBlitHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    d.SampleDesc.Count = samples;
    d.RasterizerState.MultisampleEnable = samples > 1;
    if (over) {
        auto& blend = d.BlendState.RenderTarget[0];
        // premultiplied colour, the target keeps its alpha
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso.Get();
}

// Pass's pipeline for the shader, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv,
                                                      float blend) {
    uint64_t seed = uint64_t(rtv) << 1 | (blend >= 0 ? 1 : 0);
    uint64_t source_seed = Hash(hlsl, std::strlen(hlsl), seed);
    auto& pso = blit_psos_[Hash(entry, std::strlen(entry), source_seed)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(hlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(hlsl, entry, "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (blend >= 0) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

void D3D12Backend::Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh,
                        uint32_t w, uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3],
                        const float p[8], float blend) {
    ID3D12PipelineState* pso = EnsurePassPipeline(hlsl, entry, rtv, blend);
    if (!pso) return;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && src[i] ? fmt[i] : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        if (i < 3 && src[i] && src[i]->GetDesc().SampleDesc.Count > 1) {
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            sv.Texture2DMS.UnusedField_NothingToDefine = 0;
        }
        dev_->CreateShaderResourceView(i < 3 ? src[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, 8 * sizeof(float));
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(w, h);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        cl_->OMSetBlendFactor(f);
    }
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
}

// SMAA lookup images are generic algorithm data, not game textures.
bool D3D12Backend::CreateSmaaLookup(ComPtr<ID3D12Resource>& texture, DXGI_FORMAT fmt,
                                   uint32_t width, uint32_t height, uint32_t bytes_per_pixel, const uint8_t* data) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    auto desc = Texture2DDesc(fmt, width, height);
    auto state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&texture)))) return false;
    const uint32_t row_bytes = width * bytes_per_pixel;
    const uint32_t pitch = (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                           ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    auto upload = Upload(uint64_t(pitch) * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(upload.cpu + size_t(y) * pitch, data + size_t(y) * row_bytes, row_bytes);
    D3D12_TEXTURE_COPY_LOCATION destination{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION source{upload.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    source.PlacedFootprint.Offset = upload.off;
    source.PlacedFootprint.Footprint = {fmt, width, height, 1, pitch};
    cl_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(texture.Get(), state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

bool D3D12Backend::EnsureSmaaLookups() {
    static_assert(sizeof(kSmaaArea) == 160 * 560 * 2);
    static_assert(sizeof(kSmaaSearch) == 64 * 16);
    if (smaa_lookups_[0] && smaa_lookups_[1]) return true;
    // Keep lookup uploads in the existing mapped ring rather than its unchecked overflow allocation.
    if (CurrentFrame().used > kUploadSize - (1ull << 20)) Flush();
    const bool ready = CreateSmaaLookup(smaa_lookups_[0], DXGI_FORMAT_R8G8_UNORM, 160, 560, 2, kSmaaArea) &&
                       CreateSmaaLookup(smaa_lookups_[1], DXGI_FORMAT_R8_UNORM, 64, 16, 1, kSmaaSearch);
    if (!ready) {
        // The first copy may already be recorded if the second allocation failed.
        for (auto& texture : smaa_lookups_) if (texture) Release(std::move(texture));
    }
    return ready;
}

void D3D12Backend::ReleaseSmaaTargets() {
    // Retire behind the current frame's fence; a previous frame may still sample these.
    for (auto& texture : smaa_targets_) if (texture) Release(std::move(texture));
    smaa_width_ = smaa_height_ = 0;
    smaa_format_ = DXGI_FORMAT_UNKNOWN;
}

bool D3D12Backend::EnsureSmaaTargets(DXGI_FORMAT fmt, uint32_t width, uint32_t height) {
    if (smaa_targets_[2] && smaa_width_ == width && smaa_height_ == height && smaa_format_ == fmt) return true;
    ReleaseSmaaTargets();
    // Dedicated RTV slots will be overwritten. Finish all prior uses first.
    Flush();
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const DXGI_FORMAT formats[3] = {kSmaaEdges, kSmaaWeights, fmt};
    for (uint32_t i = 0; i < 3; ++i) {
        auto desc = Texture2DDesc(formats[i], width, height);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = formats[i];
        smaa_states_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, smaa_states_[i], &clear,
                                                 IID_PPV_ARGS(&smaa_targets_[i])))) {
            ReleaseSmaaTargets();
            return false;
        }
        dev_->CreateRenderTargetView(smaa_targets_[i].Get(), nullptr,
                                     Cpu(rtv_heap_.Get(), kSmaaRtv + i, rtv_inc_));
    }
    smaa_width_ = width;
    smaa_height_ = height;
    smaa_format_ = fmt;
    LOGR("d3d12: SMAA targets %ux%u format=%u", width, height, unsigned(fmt));
    return true;
}

ID3D12Resource* D3D12Backend::PrepareSmaa(Target* target) {
    const bool enabled = app::ReadSettings().smaa;
    if (enabled != smaa_enabled_) {
        smaa_enabled_ = enabled;
        smaa_failed_ = false;
        if (!enabled) ReleaseSmaaTargets();
        g_smaa_status = enabled ? SmaaStatus::Pending : SmaaStatus::Off;
        LOGR("d3d12: SMAA %s", enabled ? "requested" : "disabled");
    }
    if (!enabled) g_smaa_status = SmaaStatus::Off;
    if (!target) return nullptr;
    const auto readable = ReadTarget(*target);
    ID3D12Resource* original = readable.res;
    if (!original) return nullptr;
    Transition(original, *readable.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!enabled || smaa_failed_) return original;
    auto unavailable = [&](const char* reason) -> ID3D12Resource* {
        smaa_failed_ = true;
        g_smaa_status = SmaaStatus::Unavailable;
        ReleaseSmaaTargets();
        LOGE("d3d12: SMAA unavailable (%s); original image used. Toggle off/apply/on/apply to retry.", reason);
        return original;
    };
    if (!target->w || !target->h || (target->fmt != kBgra && target->fmt != DXGI_FORMAT_R8G8B8A8_UNORM))
        return unavailable("unsupported final surface format or extent");
    const auto& source = SmaaSource();
    // Preflight all shaders/pipelines so a failed pass never leaves a partial final image.
    if (!EnsurePassPipeline(source.c_str(), "smaa_edges", kSmaaEdges, -1) ||
        !EnsurePassPipeline(source.c_str(), "smaa_weights", kSmaaWeights, -1) ||
        !EnsurePassPipeline(source.c_str(), "smaa_blend", target->fmt, -1))
        return unavailable("shader or pipeline creation failed");
    if (!EnsureSmaaLookups() || !EnsureSmaaTargets(target->fmt, target->w, target->h))
        return unavailable("GPU resource allocation failed");

    // Reserve all three passes plus the final blit before recording any of them.
    // Avoid wrapping SRV descriptors that earlier scene draws still reference.
    if (CurrentFrame().srv + 16 > kSrvCount / kFrames || CurrentFrame().used > kUploadSize - 4096) Flush();
    Transition(original, *readable.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const float parameters[8] = {1.0f / target->w, 1.0f / target->h, float(target->w), float(target->h)};
    const float clear[4] = {};
    const auto edges_view = Cpu(rtv_heap_.Get(), kSmaaRtv, rtv_inc_);
    const auto weights_view = Cpu(rtv_heap_.Get(), kSmaaRtv + 1, rtv_inc_);
    const auto result_view = Cpu(rtv_heap_.Get(), kSmaaRtv + 2, rtv_inc_);

    Transition(smaa_targets_[0].Get(), smaa_states_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(edges_view, clear, 0, nullptr);  // edge PS discards non-edges
    ID3D12Resource* edges_inputs[3] = {original, nullptr, nullptr};
    const DXGI_FORMAT edges_formats[3] = {target->fmt, target->fmt, target->fmt};
    Pass(source.c_str(), "smaa_edges", kSmaaEdges, edges_view, target->w, target->h,
         edges_inputs, edges_formats, parameters, -1);
    Transition(smaa_targets_[0].Get(), smaa_states_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    Transition(smaa_targets_[1].Get(), smaa_states_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(weights_view, clear, 0, nullptr);
    ID3D12Resource* weight_inputs[3] = {smaa_targets_[0].Get(), smaa_lookups_[0].Get(), smaa_lookups_[1].Get()};
    const DXGI_FORMAT weight_formats[3] = {kSmaaEdges, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8_UNORM};
    Pass(source.c_str(), "smaa_weights", kSmaaWeights, weights_view, target->w, target->h,
         weight_inputs, weight_formats, parameters, -1);
    Transition(smaa_targets_[1].Get(), smaa_states_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    Transition(smaa_targets_[2].Get(), smaa_states_[2], D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* blend_inputs[3] = {original, smaa_targets_[1].Get(), nullptr};
    const DXGI_FORMAT blend_formats[3] = {target->fmt, kSmaaWeights, target->fmt};
    Pass(source.c_str(), "smaa_blend", target->fmt, result_view, target->w, target->h,
         blend_inputs, blend_formats, parameters, -1);
    Transition(smaa_targets_[2].Get(), smaa_states_[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (g_smaa_status.exchange(SmaaStatus::Active) != SmaaStatus::Active)
        LOGR("d3d12: SMAA active (1x, high preset)");
    return smaa_targets_[2].Get();
}

// a copy of the target to read while drawing to it
ID3D12Resource* D3D12Backend::Feedback(Target& t) {
    if (t.depth) return t.samples > 1 ? ReadDepth(t) : nullptr;
    const bool multisampled = t.samples > 1;
    if (!t.fb) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        auto desc = Texture2DDesc(t.fmt, t.w, t.h);  // one sample, default alignment
        t.fb_state = multisampled ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            if (multisampled) FailMsaa("colour resolve allocation failed");
            return nullptr;
        }
    } else if (multisampled && t.state == D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
        // A write always transitions the source away from RESOLVE_SOURCE.
        Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return t.fb.Get();
    }
    Transition(t.res.Get(), t.state, multisampled ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(t.fb.Get(), t.fb_state, multisampled ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST);
    if (multisampled) cl_->ResolveSubresource(t.fb.Get(), 0, t.res.Get(), 0, t.fmt);
    else cl_->CopyResource(t.fb.Get(), t.res.Get());
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

ID3D12Resource* D3D12Backend::ReadDepth(Target& t) {
    if (msaa_failed_) return nullptr;
    // Re-read every time: a direct depth copy may also transition the source to SRV.
    constexpr DXGI_FORMAT format = DXGI_FORMAT_R32_FLOAT;
    if (!EnsurePassPipeline(kMsaaDepthReadHlsl, "depth_read", format, -1)) {
        FailMsaa("sampled depth pipeline failed");
        return nullptr;
    }
    if (!t.fb) {
        if (free_rtv_.empty()) { FailMsaa("sampled depth RTV pool exhausted"); return nullptr; }
        auto desc = Texture2DDesc(format, t.w, t.h);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        t.fb_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            FailMsaa("sampled depth allocation failed");
            return nullptr;
        }
        t.fb_view = free_rtv_.back();
        free_rtv_.pop_back();
        dev_->CreateRenderTargetView(t.fb.Get(), nullptr, Cpu(rtv_heap_.Get(), t.fb_view, rtv_inc_));
        LOG_FIRST(4, "d3d12: MSAA depth read uses sample zero (%08X)", t.data);
    }
    if (CurrentFrame().srv + 4 > kSrvCount / kFrames || CurrentFrame().used > kUploadSize - 512) Flush();
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* inputs[3] = {t.res.Get(), nullptr, nullptr};
    const DXGI_FORMAT formats[3] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, kBgra, kBgra};
    const float parameters[8] = {};
    Pass(kMsaaDepthReadHlsl, "depth_read", format, Cpu(rtv_heap_.Get(), t.fb_view, rtv_inc_),
         t.w, t.h, inputs, formats, parameters, -1);
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

D3D12Backend::Readable D3D12Backend::ReadTarget(Target& t) {
    const DXGI_FORMAT format = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    if (t.samples == 1) return {t.res.Get(), &t.state, format};
    return {t.depth ? ReadDepth(t) : Feedback(t), &t.fb_state, t.depth ? DXGI_FORMAT_R32_FLOAT : t.fmt};
}

// the key's vertex attributes, ATTR0 to ATTR15 in register order; returns how many
uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    return n;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t topo) {
    switch (topo) {
        case 0: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

void FillDepthStencilDesc(const PsoKey& k, D3D12_DEPTH_STENCIL_DESC& ds) {
    ds.DepthEnable = k.zen;
    ds.DepthWriteMask = k.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = Cmp(k.zfunc);
    ds.StencilEnable = k.sten;
    ds.StencilReadMask = UINT8(k.smask);
    ds.StencilWriteMask = UINT8(k.swmask);
    D3D12_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    ds.FrontFace = ds.BackFace = so;
}

// everything of the pipeline but its input layout
void D3D12Backend::FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps,
                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = k.fill == kFillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = k.cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = k.ccw;
    d.RasterizerState.DepthBias = k.zbias;
    d.RasterizerState.SlopeScaledDepthBias = k.zslope;
    d.RasterizerState.DepthClipEnable = TRUE;
    FillDepthStencilDesc(k, d.DepthStencilState);
    d.PrimitiveTopologyType = TopologyType(k.topo);
    d.NumRenderTargets = k.has_rt ? 1 : 0;
    d.RTVFormats[0] = k.has_rt ? DXGI_FORMAT(k.rtfmt) : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = k.has_ds ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = k.samples;
    d.RasterizerState.MultisampleEnable = k.samples > 1;
}

// a point is drawn as its whole console pixel by a geometry shader, solid, front facing and never culled
void D3D12Backend::ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    uint64_t hash;
    ID3DBlob* gs = Compile(PointHlsl(), "main", "gs_5_0", hash);
    if (!gs) return;
    d.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = FALSE;
}

float D3D12Backend::PointScale() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) {
        if (!it->second && k.samples > 1) FailMsaa("cached scene pipeline unavailable");
        return it->second.Get();
    }
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    if (k.topo == uint32_t(Topology::Points)) ExpandPoints(d);
    ComPtr<ID3D12PipelineState> pso;
    g_perf.psos++;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
        LOGE("d3d12: pipeline creation failed");
        if (k.samples > 1) FailMsaa("scene pipeline failed");
    }
    psos_[h] = pso;
    return pso.Get();
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

ID3D12Resource* D3D12Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                                DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (ext_) ext_->Sampled(d, &t, 0, info);
    // Multisampled resources are resolved before ordinary Texture2D shaders read them.
    const auto readable = ReadTarget(t);
    sv.Format = readable.fmt;
    if (!readable.res) return nullptr;
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    info.kind = kTex2D;
    if (feedback && t.samples == 1) return Feedback(t);
    Transition(readable.res, *readable.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!feedback && !t.depth && t.data == shadow_data_ && t.gw == 256 && t.gh == 256)
        return FilterShadowMask(t);
    return readable.res;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// false when the texture could not be made, its entry dropped
bool D3D12Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = d.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = UINT16(d.cube ? 6 : d.depth);
    td.MipLevels = UINT16(tx.levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void CopyLevelRows(uint8_t* out, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT rows, UINT64 row_size,
                   const std::vector<uint8_t>& px, uint32_t pitch) {
    uint32_t slices = fp.Footprint.Depth;
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * fp.Footprint.RowPitch;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
            } else {
                std::memset(dst, 0, size_t(row_size));
            }
        }
    }
}

void D3D12Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    D3D12_RESOURCE_DESC rd = tx.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    uint32_t sub = f * tx.levels + l;
    dev_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_size, &bytes);
    Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CopyLevelRows(a.cpu, fp, rows, row_size, px, pitch);
    fp.Offset = a.off;
    D3D12_TEXTURE_COPY_LOCATION dl{tx.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                 const uint32_t* colours, bool same) {
    D3D12_RESOURCE_STATES st = same ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            if (ext_) ext_->Decoded(d, fi, f, l, px, pitch, tx.flags);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void FillTextureView(const Tex& tx, const TextureDesc& d, D3D12_SHADER_RESOURCE_VIEW_DESC& sv) {
    sv.Format = tx.fmt;
    if (d.cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = tx.levels;
    } else if (d.depth > 1) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        sv.Texture2D.MipLevels = tx.levels;
    }
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D12Resource* D3D12Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || !same) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            UploadTexture(tx, d, fi, p, palette.va ? palette.colours : nullptr, same);
        }
    }
    FillTextureView(tx, d, stages.sv[stage]);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (ext_) ext_->Sampled(d, nullptr, tx.flags, info);
    return tx.res.Get();
}

ID3D12Resource* D3D12Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    info = {};
    sv = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.Texture2D.MipLevels = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;

    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    uint64_t alt = 0;
    bool box = true;
    if (ext_) ext_->Sample(d.data, fi.depth, stage, alt, stages.k[stage], box);
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height) | alt);
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, box, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

D3D12_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    D3D12_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D12_FILTER_TYPE fmin = lin(min) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmag = lin(mag) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmip = mip == kTexfLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.sv[i].Texture2D.MipLevels > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : sd.MinLOD;
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, D3D12 tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.Filter = lin(min) || lin(mag) ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC(swap[Cmp(s.rs[kRsShadowFunc]) - 1] + 1);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

uint32_t D3D12Backend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp) {
    D3D12_SAMPLER_DESC sd[4];
    uint32_t aniso = app::ReadSettings().aniso;
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso);
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (smp_used_ + 4 > kSamplerCount) {
        samplers_.clear();
        smp_used_ = 0;
    }
    uint32_t base = smp_used_;
    smp_used_ += 4;
    for (int i = 0; i < 4; i++) dev_->CreateSampler(&sd[i], Cpu(smp_heap_.Get(), base + i, smp_inc_));
    samplers_[key] = base;
    return base;
}

uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl; points never are
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgramHlsl(s.vs->program, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

// sets sh.vh and sh.vs_code, compiling the shader on a cache miss
void D3D12Backend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vs_code = blobs_[vit->second].Get();
        sh.vh = vit->second;
        return;
    }
    std::string src = VertexHlsl(s, a, sh);
    sh.vs_code = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0", sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

bool D3D12Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    auto perf_t = PerfClock::now();
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    g_perf.bind += PerfMs(perf_t);
    // textures may have recorded copies that unbound the targets
    return BindTargets(s);
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

// sets sh.ph and sh.ps_code, compiling the shader on a cache miss
void D3D12Backend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ps_code = blobs_[pit->second].Get();
        sh.ph = pit->second;
        return;
    }
    sh.ps_code = Compile(PixelHlsl(pk), "main", "ps_5_0", sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to D3D12 RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr float kDepth24Max = 16777215.0f;  // 2^24 minus 1: a 24 bit depth buffer's units per 1.0

void D3D12Backend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: bias in depth buffer units, the target is always 24 bit; the slope is per console pixel,
    // and an upscaled target's pixels are smaller
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (bound_ds_ ? float(bound_ds_->w) / bound_ds_->gw : 1.0f);
    }
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void D3D12Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    k.samples = bound_rt_ ? bound_rt_->samples : (bound_ds_ ? bound_ds_->samples : 1);
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// alpha test off: compare always, in Cmp() minus 1 terms like the enabled test
constexpr float kAlphaTestAlways = float(D3D12_COMPARISON_FUNC_ALWAYS - 1);

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void D3D12Backend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, D3D12 half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    c.texsize[i][2] = -0.5f / (float(d.width) * k);
    c.texsize[i][3] = -0.5f / (float(d.height) * k);
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void D3D12Backend::FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// A pretransformed USEW draw already supplies clip W through rhw. The rest of
// the host renderer stores projected Z, so supply the projection alone to map
// that W into the same depth scale. Mixing W/1024 with projected Z makes the
// title's far-depth/stencil quads occlude particles and late object draws.
void FillWBufferProjection(const State& s, DrawConstants& c) {
    const auto& projection = s.transforms[kTsProjection];
    if (std::abs(projection[11]) <= 1e-6f) return;
    float depth_projection[16];
    std::memcpy(depth_projection, projection.data(), sizeof(depth_projection));
    for (int r = 0; r < 4; r++)
        depth_projection[r * 4 + 2] = projection[size_t(r * 4 + 2)] * (s.viewport.max_z - s.viewport.min_z) +
                                       projection[size_t(r * 4 + 3)] * s.viewport.min_z;
    Transpose(depth_projection, c.wvp);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

void D3D12Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestExtent size, DrawConstants& c) {
    Alloc vc = Upload(sizeof(s.vsc), 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    Alloc dc = Upload(sizeof(DrawConstants), 256);
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (k.topo == uint32_t(Topology::Points)) c.surf[3] = PointScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    if (ext_) ext_->Constants(reinterpret_cast<float(*)[4]>(vc.cpu), c.ext, bound_rt_, k.blend ? k.src : 1);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    } else if (!sh.prog && sh.fk.wbuffer) {
        FillWBufferProjection(s, c);
    }
    std::memcpy(dc.cpu, &c, sizeof(c));
    cl_->SetGraphicsRootConstantBufferView(0, vc.gpu);
    cl_->SetGraphicsRootConstantBufferView(1, dc.gpu);
}

void D3D12Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, tg ? " target" : "",
                res ? "" : " unbound");
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res, stages.sv[i].Format, d.data, d.width, d.height});
        }
    }
}

// A failed preparation must remain visible in a frame capture: otherwise missing geometry
// looks like a material problem even though no GPU draw was submitted for it.
void D3D12Backend::CaptureSkippedDraw(const State& s, uint32_t prim, uint32_t count, const char* reason) {
    if (!cap_) return;
    fprintf(cap_, "%04u skip reason %s rt %08X ds %08X prim %u count %u vs %08X ps %08X zmode %u cw %08X",
            cap_i_++, reason, s.rt.data, s.zs.data, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0,
            s.rs[kRsZEnable], SimpleState(s, kMColorWriteEnable));
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d))
            fprintf(cap_, " | t%d %08X fmt %X %ux%u", i, d.data, d.format, d.width, d.height);
    }
    fputc('\n', cap_);
    if (s.vs) {
        fprintf(cap_, "     program words %zu type %X\n", s.vs->program.size(), s.vs->program_type);
        for (int r = 0; r < 16; r++) {
            const auto& in = s.vs->inputs[size_t(r)];
            if (in.stream >= 0 && in.type)
                fprintf(cap_, "     v%d stream %d offset %u type %X\n", r, in.stream, in.offset, in.type);
        }
    }
    for (int i = 0; i < 16; i++) {
        const auto& st = s.streams[size_t(i)];
        if (st.vb) fprintf(cap_, "     stream%d vb %08X stride %u\n", i, st.vb, st.stride);
    }
}

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void D3D12Backend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
    auto values = [&](const char* name, const float* v, size_t n) {
        fprintf(cap_, "\n     %s", name);
        for (size_t i = 0; i < n; i++) fprintf(cap_, " %g", v[i]);
    };
    values("surf", c.surf, 4);
    values("fog", c.fog, 4);
    values("wvp", c.wvp, 16);
    values("wv", c.wv, 16);
    for (int i = 0; i < 4; i++) {
        fprintf(cap_, "\n     tss%d", i);
        for (uint32_t v : s.tss[size_t(i)]) fprintf(cap_, " %X", v);
    }
}

// the draw's line: its state, then its textures
void D3D12Backend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                               const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3]);
    fprintf(cap_, " ; samples %u", k.samples);
    CaptureTextures(s, stages);
    // fixed function lighting as the draw sees it: states, ambient, enabled lights and the material
    fprintf(cap_,
            " ; lit %u cv %u src d%u s%u a%u e%u amb %.2f %.2f %.2f lights %02X mat d %.2f %.2f %.2f %.2f a %.2f %.2f %.2f "
            "e %.2f %.2f %.2f",
            s.rs[kRsLighting], s.rs[kRsColorVertex], s.rs[kRsDiffuseMaterialSource], s.rs[kRsSpecularMaterialSource],
            s.rs[kRsAmbientMaterialSource], s.rs[kRsEmissiveMaterialSource], c.amb[0], c.amb[1], c.amb[2],
            s.light_enable & 0xFF, c.mat[0][0], c.mat[0][1], c.mat[0][2], c.mat[0][3], c.mat[1][0], c.mat[1][1], c.mat[1][2],
            c.mat[3][0], c.mat[3][1], c.mat[3][2]);
    fprintf(cap_, " ; atest ref %.0f func %.0f ; zmode %u stencil %u func %X ref %u mask %X write %X ops %X %X %X",
            c.misc[0], c.misc[1], s.rs[kRsZEnable], k.sten, k.sfunc,
            SimpleState(s, kMStencilRef), k.smask, k.swmask, k.sfail, k.szfail, k.spass);
}

// small draws: their inputs and constants; then ends the draw's line
void D3D12Backend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c) {
    // Include small world meshes such as the ground, as well as screen quads and labels.
    // Their transforms and texture states distinguish missing material data from rejected depth/stencil passes.
    if (count <= 64)
        CaptureInputs(s, a, c);
    // vertices the input assembler reads as the title stored them: each register's host format, buffer and offset, then
    // the first vertices of each buffer in hex, and the first indices of a rebuilt list
    if (a.vertices.empty() && a.regs) {
        fprintf(cap_, "\n     raw");
        for (int r = 0; r < 16; r++) {
            if ((a.regs >> r) & 1) {
                fprintf(cap_, " v%d(fmt %u buf %u off %u)", r, unsigned(a.elements[r].format), unsigned(a.elements[r].buffer),
                        unsigned(a.elements[r].offset));
            }
        }
        for (uint32_t b = 0; b < a.buffer_count && b < 16; b++) {
            const VertexBuffer& vb = a.buffers[b];
            fprintf(cap_, "\n     buf%u stride %u bytes %u:", b, vb.stride, vb.bytes);
            for (uint32_t v = 0; vb.src && vb.stride && v < 6 && (v + 1) * vb.stride <= vb.bytes; v++) {
                fprintf(cap_, " [");
                for (uint32_t k = 0; k < vb.stride; k++) fprintf(cap_, "%02X", vb.src[v * vb.stride + k]);
                fprintf(cap_, "]");
            }
        }
        if (!a.indices.empty()) {
            fprintf(cap_, "\n     idx");
            for (size_t i = 0; i < std::min<size_t>(12, a.indices.size()); i++) fprintf(cap_, " %u", a.indices[i]);
        }
    }
    fputc('\n', cap_);
}

// each shader once, as translated
void D3D12Backend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (cap_sh_.insert(key).second) {
        if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"w")) {
            fwrite(src.data(), 1, src.size(), f);
            fclose(f);
        }
    }
    fprintf(cap_, "     %s\n", name);  // every draw names its shaders, the file is written once
}

void D3D12Backend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = VertexHlsl(s, a, sh);
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader("ps", sh.pkey, PixelHlsl(pk));
}

D3D12_SRV_DIMENSION NullViewDimension(uint8_t kind) {
    if (kind == kTexCube) return D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (kind == kTex3D) return D3D12_SRV_DIMENSION_TEXTURE3D;
    return D3D12_SRV_DIMENSION_TEXTURE2D;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (thin dark lines on HUD sprites).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;  // most vertices a small draw has
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[i];
        if (!stages.res[i]) {
            sv.ViewDimension = NullViewDimension(stages.info[i].kind);
            sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        dev_->CreateShaderResourceView(stages.res[i], &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), slot, srv_inc_));
    uint32_t samplers = EnsureStageSamplers(s, stages, UnitTexcoords(a));
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), samplers, smp_inc_));
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void D3D12Backend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    cl_->RSSetViewports(1, &vp);
    float sc = W / gw;
    LONG x0 = LONG(s.viewport.x * sc);
    LONG y0 = LONG(s.viewport.y * sc);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sc);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(s.scissors[0] * sc));
        y0 = std::max(y0, LONG(s.scissors[1] * sc));
        x1 = std::min(x1, LONG(s.scissors[2] * sc));
        y1 = std::min(y1, LONG(s.scissors[3] * sc));
    }
    D3D12_RECT rect{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)),
                    std::clamp<LONG>(x1, 0, LONG(W)), std::clamp<LONG>(y1, 0, LONG(H))};
    cl_->RSSetScissorRects(1, &rect);
}

void D3D12Backend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    cl_->OMSetBlendFactor(bf);
    cl_->OMSetStencilRef(SimpleState(s, kMStencilRef) & 0xFF);
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void D3D12Backend::UploadVertexBuffers(const Assembled& a) {
    D3D12_VERTEX_BUFFER_VIEW vbv[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        vbv[i] = {vb.gpu, b.bytes, b.stride};
    }
    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);
}

void D3D12Backend::DrawGeometry(const Assembled& a) {
    UploadVertexBuffers(a);
    cl_->IASetPrimitiveTopology(PrimitiveTopology(a));
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        cl_->DrawInstanced(a.draw_count, 1, 0, 0);
    }
}

// calls the extension's EndDraw on every return from Draw once made
struct DrawEnd {
    Extension* e;
    ~DrawEnd() {
        if (e) e->EndDraw();
    }
};

// Assemble, timed and counted
bool D3D12Backend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                const uint8_t* indices, Assembled& a) {
    auto perf_t = PerfClock::now();
    bool assembled = Assemble(s, prim, start, count, indices, a);
    g_perf.assemble += PerfMs(perf_t);
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

bool D3D12Backend::EnsureQueries() {
    if (query_counts_) return true;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kQueries, 0};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(uint64_t(kQueries) * sizeof(uint64_t));
    void* p = nullptr;
    bool made = SUCCEEDED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&query_heap_))) &&
                SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr, IID_PPV_ARGS(&query_rb_))) &&
                SUCCEEDED(query_rb_->Map(0, nullptr, &p));
    if (!made) {
        LOG_FIRST(1, "d3d12: no occlusion queries, visibility tests stay pending");
        return false;
    }
    query_counts_ = static_cast<const uint64_t*>(p);
    return true;
}

// a draw while the title counts visible pixels gets an occlusion query of its own, kNoQuery otherwise
uint32_t D3D12Backend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2] || !EnsureQueries()) return kNoQuery;
    uint32_t query = query_next_++ % kQueries;
    cl_->BeginQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    return query;
}

void D3D12Backend::EndCountedDraw(uint32_t query) {
    if (query == kNoQuery) return;
    cl_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    uint64_t at = uint64_t(query) * sizeof(uint64_t);
    cl_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query, 1, query_rb_.Get(), at);
    counted_.push_back({query, SamplesPerPixel()});
}

// the console counts its own pixels: the bound target's scale and multisampling come out
float D3D12Backend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    float area = float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
    return area * float(t->res->GetDesc().SampleDesc.Count);
}

// reports whose draws the GPU has finished, written as the console's GPU writes them: time, count, then 0 for done
void D3D12Backend::DeliverReports() {
    constexpr uint32_t kTime = 0;
    constexpr uint32_t kTimeHigh = 4;
    constexpr uint32_t kCount = 8;
    constexpr uint32_t kStatus = 12;
    uint64_t done = fence_->GetCompletedValue();
    while (!reports_.empty() && reports_.front().fence <= done) {
        const Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) pixels += double(query_counts_[q.index]) / q.samples_per_pixel;
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

void D3D12Backend::RenderAssembledDraw(const State& s, Assembled& a, uint32_t prim, uint32_t count) {
    // past the ring every upload is a buffer of its own: seconds lost on a frame that draws a whole level once
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    if (!BindTargets(s)) {
        CaptureSkippedDraw(s, prim, count, "targets");
        return;
    }
    if (ext_ && ext_->Bound(*this, s, bound_rt_, bound_ds_, a.pretransformed) && !BindTargets(s)) {
        CaptureSkippedDraw(s, prim, count, "extension-targets");
        return;
    }

    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) {
        CaptureSkippedDraw(s, prim, count, "texture-targets");
        return;
    }
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs_code || !sh.ps_code) {
        CaptureSkippedDraw(s, prim, count, !sh.vs_code ? "vertex-shader" : "pixel-shader");
        return;
    }

    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    ID3D12PipelineState* pso = EnsurePipeline(k, sh.vs_code, sh.ps_code);
    if (!pso) {
        CaptureSkippedDraw(s, prim, count, "pipeline");
        return;
    }
    cl_->SetPipelineState(pso);

    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    UploadConstants(s, sh, k, stages, size, c);

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages);
        EndDrawCapture(s, a, count, c);
        CaptureShaders(s, a, sh, pk);
    }

    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w);
    SetBlendFactorStencilRef(s);

    uint32_t query = BeginCountedDraw(s);
    DrawGeometry(a);
    EndCountedDraw(query);
}

void D3D12Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    if (cap_) CapStep(s.rt);
    if (DepthCopy(s)) {
        CaptureSkippedDraw(s, prim, count, "handled-as-depth-copy");
        return;
    }
    DrawEnd draw_end{ext_};
    if (ext_) ext_->BeginDraw(s);
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) {
        CaptureSkippedDraw(s, prim, count, "vertex-assembly");
        return;
    }
    RenderAssembledDraw(s, a, prim, count);
}

// a guest rect scaled to the target and clamped to it
D3D12_RECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    return {std::clamp<LONG>(LONG(x0 * sc), 0, t.w), std::clamp<LONG>(LONG(y0 * sc), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sc), 0, t.w), std::clamp<LONG>(LONG(y1 * sc), 0, t.h)};
}

// rs: the rects buffer, reused across targets
void D3D12Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    if (t.depth) {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(((c.flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                 ((c.flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, std::clamp(c.z, 0.0f, 1.0f),
                                   UINT8(c.stencil), UINT(rs.size()), rs.data());
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.range != 1 && i < 3; i++) col[i] /= t.range;
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), col, UINT(rs.size()), rs.data());
    }
    t.drawn = true;
    MarkShadowWrite(t);
}

void D3D12Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    PerfScope perf_clear{g_perf.clear};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    std::vector<D3D12_RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ
void D3D12Backend::CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                              Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), dst_t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D12_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_scale, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_scale,
                          0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D12_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        float mul = 1 / dst_t.range;
        BlitOptions o{.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt, .samples = dst_t.samples};
        Blit(src_res, src_t.fmt, uv, o);
    }
    dst_t.drawn = true;
    MarkShadowWrite(dst_t);
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopyTextureRegion per rect, clamped to both targets
void D3D12Backend::CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                                Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{dst_t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{src_res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D12_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        cl_->CopyTextureRegion(&dl, dx, dy, 0, &sl, &b);
    }
    dst_t.drawn = true;
    MarkShadowWrite(dst_t);
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    PerfScope perf_copy{g_perf.copy};
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d12: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    // D3D12 allows whole-resource copies with matching MS layouts. Preserve per-sample
    // colours here; partial rectangles must instead use a resolved sampled draw.
    if (src_t->samples > 1 && src_t->samples == dst_t->samples && src_t->fmt == dst_t->fmt &&
        src_t->w == dst_t->w && src_t->h == dst_t->h && src_t->gw == dst_t->gw && src_t->gh == dst_t->gh &&
        src_t->range == dst_t->range && (!count || !src_rects)) {
        Transition(src_t->res.Get(), src_t->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(dst_t->res.Get(), dst_t->state, D3D12_RESOURCE_STATE_COPY_DEST);
        cl_->CopyResource(dst_t->res.Get(), src_t->res.Get());
        dst_t->drawn = true;
        MarkShadowWrite(*dst_t);
        bound_rt_ = bound_ds_ = nullptr;
        if (cap_) CapSave(*dst_t, "msaa_copy");
        return;
    }
    const auto readable = ReadTarget(*src_t);
    if (!readable.res) return;
    if (src_t->fmt != dst_t->fmt || dst_t->samples > 1 ||
        (src_t->samples > 1 && (src_t->w != dst_t->w || src_t->h != dst_t->h))) {
        // Scaling/format changes and writes to MS targets need a sampled draw.
        CopyByDraw(*src_t, readable.res, *readable.state, *dst_t, l);
    } else {
        CopyByRegion(*src_t, readable.res, *readable.state, *dst_t, l);
    }
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

void D3D12Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (w == bb_w_ && h == bb_h_) return;
    Flush();
    for (auto& f : frames_) {
        if (f.fence) WaitFence(f.fence);
    }
    for (auto& b : bb_) b.Reset();
    swap_->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    CreateBackBufferViews();
    bb_w_ = w;
    bb_h_ = h;
}

void D3D12Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        CapSave({.res = t.res.Get(), .state = &st, .fmt = t.fmt, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGI("d3d12: frame captured to %s", cap_dir_.string().c_str());
}

void D3D12Backend::StartCapture() {
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02u%02u%02u", tm.wHour, tm.wMinute, tm.wSecond);
    std::error_code ec;
    cap_dir_ = std::filesystem::weakly_canonical(app::ExeDir() / diag::CaptureDir() / stamp, ec);
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = _wfopen((cap_dir_ / "frame.txt").c_str(), L"w");
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_sh_.clear();
}

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

void D3D12Backend::UploadGammaRamp(uint32_t brightness, uint32_t contrast) {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    ramp_settings_ = brightness << 8 | contrast;
    const bool neutral = brightness == 50 && contrast == 50;
    const float strength = std::exp2((float(contrast) - 50.0f) / 50.0f);
    const float exponent = std::exp2((50.0f - float(brightness)) / 50.0f);
    auto channel = [&](int offset, int index) -> uint8_t {
        const uint8_t original = ramp_valid_ ? ramp_[offset + index] : uint8_t(index);
        if (neutral) return original;  // exact title ramp, including fades
        const float x = original / 255.0f;
        // Symmetric contrast about mid-gray, preserving black/white endpoints.
        const float lower = 0.5f * std::pow(2.0f * std::min(x, 1.0f - x), strength);
        const float adjusted = std::pow(x <= 0.5f ? lower : 1.0f - lower, exponent);
        return uint8_t(std::lround(std::clamp(adjusted, 0.0f, 1.0f) * 255.0f));
    };
    Alloc a = Upload(kRampBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = channel(kRampBlue, i);
        p[1] = channel(kRampGreen, i);
        p[2] = channel(kRampRed, i);
        p[3] = 255;
        identity &= p[0] == i && p[1] == i && p[2] == i;
    }
    gamma_ = !identity;
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{lut_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1, kRampBytes};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::BlitFrame(Target& t, ID3D12Resource* image) {
    // 16:9 when the console reports a widescreen display, else 4:3
    bool wide = app::Widescreen();
    float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    float uv[4] = {0, 0, 1, 1};
    D3D12_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    // PrepareSmaa supplies a readable single-sample image, even when SMAA is off.
    Blit(image, t.fmt, uv, {.gamma = gamma_});
}

void D3D12Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (app::kGame.ui_input) app::kGame.ui_input();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_.Get());
}

void D3D12Backend::DrawFrame(Target* t, uint32_t bi) {
    ID3D12Resource* image = PrepareSmaa(t);  // may flush/rebind, before binding the back buffer
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), kRtvCount + bi, rtv_inc_);
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(rh, black, 0, nullptr);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t && image) BlitFrame(*t, image);
    RenderUi();
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.res);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12Backend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && fence_->GetCompletedValue() < f.fence) {
        auto perf_wait = PerfClock::now();
        WaitFence(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    f.alloc->Reset();
    f.garbage.clear();
    f.used = 0;
    f.srv = 0;
    cl_->Reset(f.alloc.Get(), nullptr);
    Begin();
}

void D3D12Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el >= 1000.0) {
        Perf& p = g_perf;
        double n = double(perf_frames);
        double host = p.draw + p.clear + p.copy + p.present + p.sync;
        LOGI("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] "
             "compile %.2f; clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f",
             n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n,
             p.compile / n, p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n,
             (el - host) / n);
        LOGI("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits: read-back %.2f "
             "resource %.2f fence %.2f flush %.2f copy %.2f, next frame %.2f) | read-backs waited for: %.1f lock, "
             "%.1f NOOVERWRITE",
             g_perf_capture_ns.exchange(0) / n / 1e6, g_perf_wait_ns.exchange(0) / n / 1e6,
             g_perf_waits.exchange(0) / n, g_perf_wait_kind_ns[0].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[1].exchange(0) / n / 1e6, g_perf_wait_kind_ns[2].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[3].exchange(0) / n / 1e6, g_perf_wait_kind_ns[4].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[5].exchange(0) / n / 1e6, g_perf_sync_block.exchange(0) / n,
             g_perf_sync_late.exchange(0) / n);
        LOGI("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.0f checks %.2f MB "
             "hashed | %.1f tex uploads %.2f MB, %.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, "
             "%.1f read-backs %.1f late | %llu shaders %llu psos this second",
             p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.sigs / n,
             p.sig_bytes / n / 1048576.0, p.texups / n, p.texup_bytes / n / 1048576.0, p.newtex / n,
             p.targets / n, p.rtups / n, p.shrinks / n, p.syncs / n, p.lates / n,
             (unsigned long long)p.shaders, (unsigned long long)p.psos);
        g_perf = Perf{};
        perf_start = PerfClock::now();
        perf_frames = 0;
    }
}

void D3D12Backend::Present(const Surface& front) {
    PerfScope perf_present{g_perf.present};
    if (cap_) {
        FinishCapture(front);
    } else if (g_capture.exchange(false) && CaptureOn()) {
        StartCapture();
    }
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) Resolve(*t);
    const auto image_settings = app::ReadSettings();
    if (ramp_dirty_ || ramp_settings_ != (image_settings.brightness << 8 | image_settings.contrast))
        UploadGammaRamp(image_settings.brightness, image_settings.contrast);
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    uint32_t bi = swap_->GetCurrentBackBufferIndex();
    DrawFrame(t, bi);
    Submit(false);
    DeliverReports();
    bool vsync = app::ReadSettings().vsync;
    auto perf_swap = PerfClock::now();
    swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    g_perf.swap += PerfMs(perf_swap);

    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    CountFps();
    if (diag::Perf()) LogPerf();
    ApplySettings();
}

// Optional scene SSAO/GTAO. It never resolves/replaces colour or changes MSAA settings.
void D3D12Backend::ReleaseAoTargets() {
    for (auto& texture : ao_targets_) if (texture) Release(std::move(texture));
    ao_width_ = ao_height_ = 0;
}

void D3D12Backend::UpdateAoSettings(uint32_t mode) {
    mode = NormalizeAoMode(mode);
    if (mode != ao_mode_) {
        ao_mode_ = mode;
        ao_failed_ = ao_logged_ = false;
        ao_skip_logs_ = 0;
        if (!mode) ReleaseAoTargets();
        LOGR("d3d12: %s %s (explicit scene hook)", AoModeName(mode), mode ? "requested" : "disabled");
    }
    // Publish status before mode: F10 waits for the renderer's mode to catch up.
    if (!mode) g_ao_status = AoStatus::Off;
    else if (ao_failed_) g_ao_status = AoStatus::Unavailable;
    else g_ao_status = ao_frame_mode_ == mode &&
                          (ao_frame_ == presents_ || (presents_ && ao_frame_ == presents_ - 1))
                          ? AoStatus::Active : AoStatus::Pending;
    g_ao_mode = mode;
}

bool D3D12Backend::EnsureAoTargets(uint32_t width, uint32_t height) {
    if (ao_targets_[1] && ao_width_ == width && ao_height_ == height) return true;
    ReleaseAoTargets();
    // The two fixed RTV descriptors may still be referenced by queued frames.
    Flush();
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    auto desc = Texture2DDesc(kAoFormat, width, height);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (uint32_t i = 0; i < 2; ++i) {
        ao_states_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, ao_states_[i], nullptr,
                                                 IID_PPV_ARGS(&ao_targets_[i])))) {
            ReleaseAoTargets();
            return false;
        }
        dev_->CreateRenderTargetView(ao_targets_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kAoRtv + i, rtv_inc_));
    }
    ao_width_ = width;
    ao_height_ = height;
    LOGR("d3d12: AO intermediates %ux%u (half internal resolution, shared SSAO/GTAO)", width, height);
    return true;
}

ID3D12PipelineState* D3D12Backend::EnsureAoPipeline(uint32_t pass, DXGI_FORMAT format,
                                                 uint32_t samples, uint32_t depth_samples, bool debug) {
    const bool gtao = pass == 0 && ao_mode_ == 2;
    uint64_t key = uint64_t(format) | uint64_t(pass) << 16 | uint64_t(samples) << 24 |
                   uint64_t(depth_samples > 1) << 32 | uint64_t(debug) << 33 | uint64_t(gtao) << 34;
    auto& pipeline = ao_pipelines_[key];
    if (pipeline) return pipeline.Get();
    std::string source = depth_samples > 1 ? "#define AO_MS_DEPTH\n" : "";
    source += kAmbientOcclusionHlsl;
    if (gtao) source += kGtaoHlsl;
    const char* entries[] = {gtao ? "gtao_raw" : "ao_raw", "ao_blur", "ao_apply"};
    uint64_t hash;
    auto* vs = Compile(source, "vs", "vs_5_0", hash);
    auto* ps = Compile(source, entries[pass], "ps_5_0", hash);
    if (!vs || !ps) return nullptr;
    auto desc = FullScreenPipelineDesc(vs, ps, format);
    desc.SampleDesc.Count = samples;
    desc.RasterizerState.MultisampleEnable = samples > 1;
    if (pass == 2) {
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                      D3D12_COLOR_WRITE_ENABLE_BLUE;
        // Multiply RGB in place, with no colour SRV feedback. Destination alpha is untouched.
        if (!debug) {
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_ZERO;
            blend.DestBlend = D3D12_BLEND_SRC_COLOR;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
            blend.DestBlendAlpha = D3D12_BLEND_ONE;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
    }
    if (FAILED(dev_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)))) return nullptr;
    return pipeline.Get();
}

void D3D12Backend::AoPass(ID3D12PipelineState* pipeline, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                          uint32_t width, uint32_t height, Target& depth, ID3D12Resource* image,
                          const float constants[24]) {
    const uint32_t slot = AllocSrvSlots(4);
    for (uint32_t i = 0; i < 4; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = i == 0 ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : kAoFormat;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = i == 0 && depth.samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS
                                                       : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) view.Texture2D.MipLevels = 1;
        auto* resource = i == 0 ? depth.res.Get() : (i == 1 ? image : nullptr);
        dev_->CreateShaderResourceView(resource, &view, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    auto cb = Upload(256, 256);
    std::memcpy(cb.cpu, constants, 24 * sizeof(float));
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);  // depth is sampled, never bound for writing here
    SetFullViewport(width, height);
    DrawFullScreen(pipeline, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::AmbientOcclusion(const State& s, const AoParameters& parameters) {
    UpdateAoSettings(parameters.mode);
    if (!ao_mode_ || ao_failed_ || ao_frame_ == presents_) return;
    auto skip = [&](const char* reason) {
        if (ao_skip_logs_++ < 4) {
            const auto& projection = s.transforms[kTsProjection];
            LOGR("d3d12: %s skipped scene (%s) rt=%08X ds=%08X viewport=%u,%u %ux%u z=%.6f..%.6f proj=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
                 AoModeName(ao_mode_), reason, s.rt.data, s.zs.data, s.viewport.x, s.viewport.y, s.viewport.w, s.viewport.h,
                 s.viewport.min_z, s.viewport.max_z, projection[0], projection[5], projection[10],
                 projection[11], projection[14], projection[15]);
        }
    };
    Target* rt = FindTarget(s.rt, false);
    Target* ds = FindTarget(s.zs, true);
    if (!rt || !ds || !rt->drawn || !ds->drawn || rt->gw != guest_w_ || rt->gh != guest_h_ ||
        ds->gw != guest_w_ || ds->gh != guest_h_ || rt->w != ds->w || rt->h != ds->h ||
        rt->samples != ds->samples) {
        skip("not a matching main scene colour/depth pair");
        return;
    }
    const auto& p = s.transforms[kTsProjection];
    const auto& v = s.viewport;
    bool finite = true;
    for (float f : p) finite = finite && std::isfinite(f);
    // Canonical LH perspective, including off-centre terms. Unknown/orthographic projections stay unmodified.
    if (!finite || std::fabs(p[11] - 1) > 0.001f || std::fabs(p[15]) > 0.0001f ||
        p[0] <= 0 || p[5] <= 0 || p[10] < 1 || p[14] >= 0 ||
        std::fabs(p[1]) + std::fabs(p[2]) + std::fabs(p[3]) + std::fabs(p[4]) +
        std::fabs(p[6]) + std::fabs(p[7]) + std::fabs(p[12]) + std::fabs(p[13]) > 0.001f ||
        !v.w || !v.h || v.x > guest_w_ || v.y > guest_h_ || v.w > guest_w_ - v.x || v.h > guest_h_ - v.y ||
        !std::isfinite(v.min_z) || !std::isfinite(v.max_z) || v.max_z - v.min_z < 0.00001f) {
        skip("unsupported projection or viewport");
        return;
    }
    auto fail = [&](const char* reason) {
        ao_failed_ = true;
        g_ao_status = AoStatus::Unavailable;
        ReleaseAoTargets();
        LOGE("d3d12: %s unavailable (%s); scene unchanged. Switch method or toggle off/apply/on/apply to retry.",
             AoModeName(ao_mode_), reason);
    };
    const bool debug = parameters.debug != 0;
    // Preflight every pipeline/resource before writing any scene colour.
    auto* raw = EnsureAoPipeline(0, kAoFormat, 1, ds->samples, false);
    auto* blur = EnsureAoPipeline(1, kAoFormat, 1, ds->samples, false);
    auto* apply = EnsureAoPipeline(2, rt->fmt, rt->samples, ds->samples, debug);
    if (!raw || !blur || !apply) { fail("shader/pipeline creation failed"); return; }
    const uint32_t width = std::max(1u, (rt->w + 1) / 2), height = std::max(1u, (rt->h + 1) / 2);
    if (!EnsureAoTargets(width, height)) { fail("GPU allocation failed"); return; }
    if (CurrentFrame().srv + 16 > kSrvCount / kFrames || CurrentFrame().used > kUploadSize - 2048) Flush();
    const float sx = float(rt->w) / guest_w_, sy = float(rt->h) / guest_h_;
    const float constants[24] = {
        1 / p[0], 1 / p[5], p[8], p[9],
        p[14], p[10], v.min_z, v.max_z - v.min_z,
        v.x * sx, v.y * sy, v.w * sx, v.h * sy,
        1.0f / rt->w, 1.0f / rt->h, float(rt->w), float(rt->h),
        std::clamp(parameters.radius, 25u, 400u) / 100.0f,
        std::min(parameters.strength, 100u) / 50.0f, float(std::min(parameters.debug, 3u)),
        std::max(8.0f, rt->h * (64.0f / 1080)),
        1.0f / width, 1.0f / height, float(width), float(height)
    };
    // Detach the writable depth view before transitioning it for sampling.
    cl_->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ao_targets_[0].Get(), ao_states_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    AoPass(raw, Cpu(rtv_heap_.Get(), kAoRtv, rtv_inc_), width, height, *ds, nullptr, constants);
    Transition(ao_targets_[0].Get(), ao_states_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ao_targets_[1].Get(), ao_states_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    AoPass(blur, Cpu(rtv_heap_.Get(), kAoRtv + 1, rtv_inc_), width, height, *ds, ao_targets_[0].Get(), constants);
    Transition(ao_targets_[1].Get(), ao_states_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    AoPass(apply, Cpu(rtv_heap_.Get(), rt->view, rtv_inc_), rt->w, rt->h, *ds, ao_targets_[1].Get(), constants);
    // Restore the depth state expected by the game's following shadow/effect draws.
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    rt->drawn = true;
    MarkShadowWrite(*rt);
    ao_frame_ = presents_;
    ao_frame_mode_ = ao_mode_;
    g_ao_status = AoStatus::Active;
    if (!ao_logged_) {
        LOGR("d3d12: %s scene pass recorded colour=%08X depth=%08X size=%ux%u samples=%u near=%.5f proj_z=%.6f radius=%.2f strength=%.2f debug=%u",
             AoModeName(ao_mode_), rt->data, ds->data, rt->w, rt->h, rt->samples, -p[14] / p[10], p[10],
             constants[16], constants[17], parameters.debug);
        ao_logged_ = true;
    }
}

// Bloom runs only at the title-supplied end of the world scene. Present already
// contains the native HUD, so applying it there would also illuminate UI pixels.
void D3D12Backend::ReleaseBloomTargets() {
    for (auto& texture : bloom_targets_) if (texture) Release(std::move(texture));
    bloom_width_ = bloom_height_ = 0;
}

bool D3D12Backend::EnsureBloomTargets(uint32_t width, uint32_t height) {
    if (bloom_targets_[2] && bloom_width_ == width && bloom_height_ == height) return true;
    ReleaseBloomTargets();
    Flush();  // Fixed RTV descriptors must be idle before they are overwritten.
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t divisor = i == 0 ? 2 : 4;
        auto desc = Texture2DDesc(kBloomFormat, std::max(1u, (width + divisor - 1) / divisor),
                                  std::max(1u, (height + divisor - 1) / divisor));
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        bloom_states_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, bloom_states_[i], nullptr,
                                                 IID_PPV_ARGS(&bloom_targets_[i])))) {
            ReleaseBloomTargets();
            return false;
        }
        dev_->CreateRenderTargetView(bloom_targets_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kBloomRtv + i, rtv_inc_));
    }
    bloom_width_ = width;
    bloom_height_ = height;
    LOGR("d3d12: bloom intermediates for %ux%u (half/quarter resolution, SDR)", width, height);
    return true;
}

ID3D12PipelineState* D3D12Backend::EnsureBloomPipeline(uint32_t pass, DXGI_FORMAT format, uint32_t samples) {
    const uint64_t key = uint64_t(format) | uint64_t(pass) << 16 | uint64_t(samples) << 24;
    auto& pipeline = bloom_pipelines_[key];
    if (pipeline) return pipeline.Get();
    const char* entries[] = {"bloom_extract", "bloom_downsample", "bloom_blur", "bloom_apply"};
    uint64_t hash;
    auto* vs = Compile(kBloomHlsl, "vs", "vs_5_0", hash);
    auto* ps = Compile(kBloomHlsl, entries[pass], "ps_5_0", hash);
    if (!vs || !ps) return nullptr;
    auto desc = FullScreenPipelineDesc(vs, ps, format);
    desc.SampleDesc.Count = samples;
    desc.RasterizerState.MultisampleEnable = samples > 1;
    if (pass == 3) {
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                      D3D12_COLOR_WRITE_ENABLE_BLUE;
        // Screen: dst + bloom * (1 - dst), bounded for SDR. Blend into each
        // original MSAA sample instead of replacing it with the resolved image.
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
        blend.DestBlend = D3D12_BLEND_ONE;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(dev_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)))) return nullptr;
    return pipeline.Get();
}

void D3D12Backend::BloomPass(ID3D12PipelineState* pipeline, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                             uint32_t width, uint32_t height, ID3D12Resource* image, DXGI_FORMAT format,
                             const float constants[8]) {
    const uint32_t slot = AllocSrvSlots(4);
    for (uint32_t i = 0; i < 4; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = i == 0 ? format : kBloomFormat;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i == 0 ? image : nullptr, &view, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    auto cb = Upload(256, 256);
    std::memcpy(cb.cpu, constants, 8 * sizeof(float));
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    SetFullViewport(width, height);
    DrawFullScreen(pipeline, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Bloom(const State& s, const BloomParameters& parameters) {
    const bool enabled = parameters.enabled != 0;
    if (enabled != bloom_enabled_) {
        bloom_enabled_ = enabled;
        bloom_failed_ = bloom_logged_ = false;
        bloom_skip_logs_ = 0;
        if (!enabled) ReleaseBloomTargets();
        g_bloom_status = enabled ? BloomStatus::Pending : BloomStatus::Off;
        LOGR("d3d12: bloom %s (explicit end-of-world hook)", enabled ? "requested" : "disabled");
    }
    if (!enabled || bloom_failed_ || bloom_frame_ == presents_) return;
    const auto& v = s.viewport;
    Target* rt = FindTarget(s.rt, false);
    Target* ds = FindTarget(s.zs, true);
    // Restrict the effect to an entire main scene; offscreen/partial views are
    // deliberately skipped rather than bleeding their pixels into another view.
    if (!rt || !ds || !rt->drawn || !ds->drawn || rt->gw != guest_w_ || rt->gh != guest_h_ ||
        ds->gw != guest_w_ || ds->gh != guest_h_ || rt->w != ds->w || rt->h != ds->h ||
        rt->samples != ds->samples || rt->range != 1 || v.x || v.y || v.w != guest_w_ || v.h != guest_h_) {
        if (bloom_skip_logs_++ < 4)
            LOGR("d3d12: bloom skipped incompatible scene rt=%08X ds=%08X viewport=%u,%u %ux%u",
                 s.rt.data, s.zs.data, v.x, v.y, v.w, v.h);
        return;
    }
    auto fail = [&](const char* reason) {
        bloom_failed_ = true;
        g_bloom_status = BloomStatus::Unavailable;
        ReleaseBloomTargets();
        LOGE("d3d12: bloom unavailable (%s); bloom not applied. Disable/apply in gameplay then re-enable to retry.", reason);
    };
    // Preflight before writing scene colour. The other effects and guest state
    // remain independent of this optional pass.
    auto* extract = EnsureBloomPipeline(0, kBloomFormat, 1);
    auto* downsample = EnsureBloomPipeline(1, kBloomFormat, 1);
    auto* blur = EnsureBloomPipeline(2, kBloomFormat, 1);
    auto* apply = EnsureBloomPipeline(3, rt->fmt, rt->samples);
    if (!extract || !downsample || !blur || !apply) { fail("shader/pipeline creation failed"); return; }
    if (!EnsureBloomTargets(rt->w, rt->h)) { fail("GPU allocation failed"); return; }
    if (CurrentFrame().srv + 20 > kSrvCount / kFrames || CurrentFrame().used > kUploadSize - 2048) Flush();
    // Resolve a read-only copy of the scene; never copy it back over MSAA colour.
    cl_->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    auto* scene = Feedback(*rt);
    if (!scene) { fail("scene colour copy/resolve failed"); return; }
    const uint32_t hw = std::max(1u, (rt->w + 1) / 2), hh = std::max(1u, (rt->h + 1) / 2);
    const uint32_t qw = std::max(1u, (rt->w + 3) / 4), qh = std::max(1u, (rt->h + 3) / 4);
    const float threshold = std::min(parameters.threshold, 95u) / 100.0f;
    const float strength = std::min(parameters.strength, 100u) / 100.0f;
    const float radius = std::clamp(parameters.radius, 25u, 300u) / 100.0f * (rt->h / 1080.0f);
    auto record = [&](uint32_t destination, uint32_t width, uint32_t height, ID3D12PipelineState* pipeline,
                      ID3D12Resource* image, DXGI_FORMAT format, uint32_t sw, uint32_t sh, float dx, float dy) {
        const float constants[8] = {1.0f / sw, 1.0f / sh, 1.0f / width, 1.0f / height,
                                     threshold, strength, dx, dy};
        Transition(bloom_targets_[destination].Get(), bloom_states_[destination], D3D12_RESOURCE_STATE_RENDER_TARGET);
        BloomPass(pipeline, Cpu(rtv_heap_.Get(), kBloomRtv + destination, rtv_inc_), width, height, image, format, constants);
        Transition(bloom_targets_[destination].Get(), bloom_states_[destination], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };
    record(0, hw, hh, extract, scene, rt->fmt, rt->w, rt->h, 0, 0);
    record(1, qw, qh, downsample, bloom_targets_[0].Get(), kBloomFormat, hw, hh, 0, 0);
    record(2, qw, qh, blur, bloom_targets_[1].Get(), kBloomFormat, qw, qh, radius, 0);
    record(1, qw, qh, blur, bloom_targets_[2].Get(), kBloomFormat, qw, qh, 0, radius);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float constants[8] = {1.0f / qw, 1.0f / qh, 1.0f / rt->w, 1.0f / rt->h, threshold, strength, 0, 0};
    BloomPass(apply, Cpu(rtv_heap_.Get(), rt->view, rtv_inc_), rt->w, rt->h,
              bloom_targets_[1].Get(), kBloomFormat, constants);
    rt->drawn = true;
    MarkShadowWrite(*rt);
    bloom_frame_ = presents_;
    g_bloom_status = BloomStatus::Active;
    if (cap_) fprintf(cap_, "# host bloom end-of-world rt=%08X size=%ux%u samples=%u strength=%u threshold=%u radius=%u\n",
                      rt->data, rt->w, rt->h, rt->samples, parameters.strength, parameters.threshold, parameters.radius);
    if (!bloom_logged_) {
        LOGR("d3d12: bloom scene pass recorded rt=%08X size=%ux%u samples=%u strength=%u threshold=%u radius=%u (SDR screen blend, before HUD)",
             rt->data, rt->w, rt->h, rt->samples, parameters.strength, parameters.threshold, parameters.radius);
        bloom_logged_ = true;
    }
}

void D3D12Backend::ApplySettings() {
    if (bloom_enabled_ && !bloom_failed_ && bloom_frame_ != presents_ &&
        (!presents_ || bloom_frame_ != presents_ - 1)) g_bloom_status = BloomStatus::Pending;
    UpdateAoSettings(app::ReadSettings().ambient_occlusion);
    UpdateShadowSoftness();
    const uint32_t shadow_size = NormalizeShadowResolution(app::ReadSettings().shadow_resolution);
    if (shadow_size != shadow_set_) {
        DropShadowTarget();
        shadow_set_ = shadow_size;
        shadow_failed_ = false;
        shadow_extent_ = 0;
        PublishShadowResolution();
        LOGR("d3d12: shadow resolution requested=%u (0 follows render scale)", shadow_set_);
    }
    float want = app::RenderScale(guest_h_);
    bool remake = ext_ && ext_->Settings();
    const uint32_t requested = NormalizeMsaa(app::ReadSettings().msaa);
    uint32_t samples = msaa_samples_;
    if (requested != msaa_set_) {
        msaa_set_ = requested;
        msaa_failed_ = false;
        msaa_remake_ = false;
        samples = SelectSamples(requested);
    } else if (msaa_remake_) {
        samples = 1;
    }
    if (std::fabs(want - scale_) > 1e-4f || remake || samples != msaa_samples_ || msaa_remake_) {
        Flush();
        for (auto& [key, tg] : targets_) {
            (tg.depth ? free_dsv_ : free_rtv_).push_back(tg.view);
            if (tg.fb_view != ~0u) free_rtv_.push_back(tg.fb_view);
        }
        targets_.clear();
        ReleaseBloomTargets();
        bloom_logged_ = false;
        ReleaseShadowSoftTargets();
        shadow_soft_logged_ = false;
        shadow_extent_ = 0;
        PublishShadowResolution();
        scale_ = want;
        msaa_samples_ = samples;
        msaa_remake_ = false;
        LOGR("d3d12: MSAA targets recreated effective=%u requested=%u fallback=%u",
             msaa_samples_, msaa_set_, unsigned(msaa_failed_));
        if (ext_) ext_->Reset(*this);
        bound_rt_ = bound_ds_ = draw_rt_ = draw_ds_ = nullptr;
    }
    PublishMsaa();
}

// the target resolved, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D12Backend::CopySource D3D12Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    Resolve(t);
    const auto readable = ReadTarget(t);
    CopySource out{readable.res, nullptr};
    if (!readable.res) return out;
    if (t.w != t.gw || t.h != t.gh || readable.fmt != kBgra) {
        out.down = Shrink(readable.res, *readable.state, readable.fmt, t.gw, t.gh, o);
        out.res = out.down.Get();
    } else {
        Transition(readable.res, *readable.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    return out;
}

// long unused: the title freed it and other data is at this address now
bool D3D12Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D12Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    if (diag::Perf()) {
        LOG_FIRST(16, "perf: read-back of %08X (%ux%u guest, %ux%u host)", data, it->second.gw, it->second.gh,
                  it->second.w, it->second.h);
    }
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D12Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D12Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fence_->GetCompletedValue();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        if (diag::Perf()) {
            LOG_FIRST(16, "perf: late %08X nothing finished: done %llu now %llu, slots %llu/%llu %llu/%llu %llu/%llu",
                      data, (unsigned long long)done, (unsigned long long)now, (unsigned long long)l.fence[0],
                      (unsigned long long)l.frame[0], (unsigned long long)l.fence[1], (unsigned long long)l.frame[1],
                      (unsigned long long)l.fence[2], (unsigned long long)l.frame[2]);
        }
        return false;
    }
    if (l.fence[best] > l.delivered) {
        uint8_t* p = nullptr;
        // the last row is not padded to the pitch: a range to the end of the pitch fails to map
        D3D12_RANGE rr{0, size_t(l.row_pitch) * (l.gh - 1) + l.gw * 4};
        if (FAILED(l.rb[best]->Map(0, &rr, reinterpret_cast<void**>(&p)))) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.row_pitch, l.gw * 4);
        D3D12_RANGE none{0, 0};
        l.rb[best]->Unmap(0, &none);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void D3D12Backend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb) {
        if (r) garbage.push_back(r);
    }
    if (l.down) garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
ID3D12Resource* D3D12Backend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down.Get(), .into_state = l.down_state};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) {
        l.down = src.down;
        l.down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void D3D12Backend::RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (!l.rb[slot]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&l.rb[slot])))) {
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dl{l.rb[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    l.row_pitch = fp.Footprint.RowPitch;
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void D3D12Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        if (diag::Perf()) LOG_FIRST(16, "perf: late %08X ring reset, %ux%u -> %ux%u", data, l.gw, l.gh, t.gw, t.gh);
        ResetLate(l, t);
    }
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > fence_->GetCompletedValue()) return;
    ID3D12Resource* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a new w x h target, left as a copy source
ComPtr<ID3D12Resource> D3D12Backend::Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h, const ShrinkOptions& o) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    ComPtr<ID3D12Resource> down = o.into;
    D3D12_RESOURCE_STATES down_state = o.into ? o.into_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_perf.shrinks++;
    if (free_rtv_.empty()) return nullptr;
    if (down) {
        Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&down)))) {
        return nullptr;
    }
    // borrows the next free view without taking it, only for this recording
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), free_rtv_.back(), rtv_inc_);
    dev_->CreateRenderTargetView(down.Get(), nullptr, rh);
    Transition(src, st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetBlitTarget(rh, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, fmt, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D12Backend::ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    rb->Unmap(0, nullptr);
    return true;
}

// BMP file and info header: byte offsets of the fields CapSave writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

void D3D12Backend::CapSave(const CapImage& img, const char* name) {
    ComPtr<ID3D12Resource> down = Shrink(img.res, *img.state, img.fmt, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.Get(), img.w, img.h, px)) return;
    FILE* f = _wfopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};  // BITMAPFILEHEADER and BITMAPINFOHEADER, top down 32 bit
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, img.w);
    put(kBmpHeight, uint32_t(-int32_t(img.h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void D3D12Backend::CapSave(Target& t, const char* name) {
    const auto readable = ReadTarget(t);
    if (!readable.res) return;
    float mul = t.depth ? 1 : t.range;
    CapSave({.res = readable.res, .state = readable.state, .fmt = readable.fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

// the frame moves to another target: save the one it leaves
void D3D12Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetMapKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetMapKey(cap_rt_, true));
    if (cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void D3D12Backend::Capture(Target& t, const char* tag, float mul) {
    if (!cap_) return;
    char name[32];
    snprintf(name, sizeof(name), "%04u_%s_%08X", cap_i_, tag, t.data);
    const auto readable = ReadTarget(t);
    if (!readable.res) return;
    CapSave({.res = readable.res, .state = readable.state, .fmt = readable.fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

void D3D12Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_valid_ = true;
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<D3D12Backend>(); }

}  // namespace gpu
