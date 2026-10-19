// Optional read-only diagnostics for this XBE. No distances, model choices or guest state are changed.
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include "core/log.h"
#include "xdk/hle.h"

namespace {

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("XBE_LOD_TRACE");
        const bool on = value && value[0] == '1' && value[1] == 0;
        if (on) LOGR("lod-trace: enabled (read-only; original rendering)");
        return on;
    }();
    return enabled;
}

struct Counters {
    std::atomic<uint64_t> scenes{0}, objects{0}, ranged{0}, models{0}, accepted{0}, updates{0}, depth_calls{0};
    std::atomic<uint64_t> levels[5]{};  // Last slot is an unexpected index, not an invented LOD level.
    std::atomic<uint64_t> boxes{0}, outside{0}, legacy_a{0}, legacy_b{0};
} g_counts;
std::mutex g_samples_lock;
std::set<uint32_t> g_sampled_models, g_sampled_objects, g_sampled_depth;

float ReadFloat(uint32_t va) { return kernel::G<float>(va); }

std::string ModelName(uint32_t instance) {
    const uint32_t model = kernel::R32(instance);
    if (!model) return "<none>";
    std::string name;
    for (uint32_t i = 0; i < 32; ++i) {
        const auto c = kernel::G<uint8_t>(model + 0x8C + i);
        if (!c) break;
        name += c >= 32 && c < 127 ? char(c) : '?';
    }
    return name;
}

void SampleModel(uint32_t instance, float requested) {
    if (!instance) return;
    const unsigned level = kernel::G<uint8_t>(instance + 0x0C);
    ++g_counts.levels[level < 4 ? level : 4];
    std::lock_guard lock(g_samples_lock);
    if (g_sampled_models.size() >= 12 || !g_sampled_models.insert(instance).second) return;
    const std::string name = ModelName(instance);
    LOGR("lod-trace: model=%08X name=%s requested=%.5f stored=%.5f level=%u size_bias=%.5f flags=%08X",
         instance, name.c_str(), double(requested), double(ReadFloat(instance + 8)), level,
         double(ReadFloat(instance + 0x30)), kernel::R32(instance + 4));
}

void Report() {
    const auto n = g_counts.scenes.load();
    if (n != 1 && n % 300) return;
    LOGR("lod-trace: cumulative scenes=%llu objects=%llu range_flag=%llu model_calls=%llu accepted=%llu depth_calls=%llu lod_sets=%llu levels=%llu/%llu/%llu/%llu invalid=%llu boxes=%llu outside=%llu legacy=%llu/%llu",
         (unsigned long long)n, (unsigned long long)g_counts.objects.load(),
         (unsigned long long)g_counts.ranged.load(), (unsigned long long)g_counts.models.load(),
         (unsigned long long)g_counts.accepted.load(), (unsigned long long)g_counts.depth_calls.load(),
         (unsigned long long)g_counts.updates.load(),
         (unsigned long long)g_counts.levels[0].load(), (unsigned long long)g_counts.levels[1].load(),
         (unsigned long long)g_counts.levels[2].load(), (unsigned long long)g_counts.levels[3].load(),
         (unsigned long long)g_counts.levels[4].load(), (unsigned long long)g_counts.boxes.load(),
         (unsigned long long)g_counts.outside.load(), (unsigned long long)g_counts.legacy_a.load(),
         (unsigned long long)g_counts.legacy_b.load());
    const uint32_t camera = kernel::R32(0x0053BCE0);
    if (camera) LOGR("lod-trace: camera=%08X near=%.5f far=%.5f fov=%.5f lod_min=%.5f lod_max=%.5f",
                     camera, double(ReadFloat(camera + 0x288)), double(ReadFloat(camera + 0x28C)),
                     double(ReadFloat(camera + 0x29C)), double(ReadFloat(0x0053BCA0)), double(ReadFloat(0x0053BCA4)));
}

}  // namespace

X86_FUNC(__imp__sub_00029530);
X86_FUNC(sub_00029530) {
    if (!Enabled()) { __imp__sub_00029530(ctx, base); return; }
    ++g_counts.scenes;
    __imp__sub_00029530(ctx, base);  // Retains the existing mid-function AO hook.
    Report();
}

X86_FUNC(__imp__sub_00028EF0);
X86_FUNC(sub_00028EF0) {
    if (!Enabled()) { __imp__sub_00028EF0(ctx, base); return; }
    ++g_counts.objects;
    const uint32_t object = ctx.ecx;
    const uint32_t flags = kernel::R32(object + 0x40);
    if (flags & 0x10) {
        ++g_counts.ranged;
        std::lock_guard lock(g_samples_lock);
        if (g_sampled_objects.size() < 12 && g_sampled_objects.insert(object).second)
            LOGR("lod-trace: range object=%08X type=%u flags=%08X far_field=%.5f instance=%08X",
                 object, unsigned(kernel::G<uint8_t>(object + 0x10)), flags,
                 double(ReadFloat(object + 0x17C)), kernel::R32(object + 0x1CC));
    }
    __imp__sub_00028EF0(ctx, base);
}

X86_FUNC(__imp__sub_0020B5A0);
X86_FUNC(sub_0020B5A0) {
    if (!Enabled()) { __imp__sub_0020B5A0(ctx, base); return; }
    ++g_counts.models;
    __imp__sub_0020B5A0(ctx, base);
    if (ctx.eax) ++g_counts.accepted;  // Native success, not a count of GPU draws or polygons.
}

X86_FUNC(__imp__sub_0020AD70);
X86_FUNC(sub_0020AD70) {
    if (!Enabled()) { __imp__sub_0020AD70(ctx, base); return; }
    const uint32_t instance = ctx.ecx;
    const float requested = ReadFloat(ctx.esp + 4);
    ++g_counts.updates;
    __imp__sub_0020AD70(ctx, base);
    SampleModel(instance, requested);
}

X86_FUNC(__imp__sub_00205460);
X86_FUNC(sub_00205460) {
    if (!Enabled()) { __imp__sub_00205460(ctx, base); return; }
    ++g_counts.boxes;
    __imp__sub_00205460(ctx, base);
    if (!(ctx.eax & 0xFF)) ++g_counts.outside;  // Native transformed-box/frustum result, not distance alone.
}

X86_FUNC(__imp__sub_00022A90);
X86_FUNC(sub_00022A90) {
    if (Enabled()) ++g_counts.legacy_a;
    __imp__sub_00022A90(ctx, base);
}
X86_FUNC(__imp__sub_00022BC0);
X86_FUNC(sub_00022BC0) {
    if (Enabled()) ++g_counts.legacy_b;
    __imp__sub_00022BC0(ctx, base);
}

X86_FUNC(__imp__sub_0020B3C0);
X86_FUNC(sub_0020B3C0) {
    if (!Enabled()) { __imp__sub_0020B3C0(ctx, base); return; }
    ++g_counts.depth_calls;
    const uint32_t instance = ctx.ecx;
    const float depth = ReadFloat(ctx.esp + 4);
    bool sample = false;
    {
        std::lock_guard lock(g_samples_lock);
        sample = g_sampled_depth.size() < 12 && g_sampled_depth.insert(instance).second;
    }
    __imp__sub_0020B3C0(ctx, base);
    if (sample) LOGR("lod-trace: depth model=%08X view_z=%.5f quality=%.5f level=%u size_bias=%.5f flags=%08X",
                     instance, double(depth), double(ReadFloat(instance + 8)),
                     unsigned(kernel::G<uint8_t>(instance + 0x0C)),
                     double(ReadFloat(instance + 0x30)), kernel::R32(instance + 4));
}
