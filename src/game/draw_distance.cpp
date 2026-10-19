// Range control for the supported default.xbe. Preserve the native object state,
// frustum checks, sector traversal, fade update and every other distance caller.
#include <atomic>
#include <cmath>
#include <cstdlib>

#include "common/pc_render.h"
#include "core/log.h"
#include "xdk/hle.h"

namespace {

bool TraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("XBE_LOD_TRACE");
        return value && value[0] == '1' && value[1] == 0;
    }();
    return enabled;
}

std::atomic<uint64_t> g_gate_calls{0}, g_scaled{0}, g_outer_lifted{0};
std::atomic<uint32_t> g_samples[4]{}, g_extended_samples[4]{};

void Trace(uint32_t object, double original, double effective, float range_limit, uint32_t percent) {
    if (!TraceEnabled()) return;
    const auto calls = ++g_gate_calls;
    if (effective != original) ++g_scaled;
    // This is a changed comparison, not proof that a draw reached the GPU.
    const bool lifted = range_limit > 0 && original > double(range_limit) + 10 &&
                        effective <= double(range_limit) + 10;
    if (lifted) ++g_outer_lifted;
    const unsigned slot = percent == 150 ? 1 : percent == 200 ? 2 : percent == 300 ? 3 : 0;
    if ((lifted ? g_extended_samples[slot]++ < 6 : g_samples[slot]++ < 3))
        LOGR("distance-trace: gate object=%08X type=%u range=%.5f actual=%.5f effective=%.5f percent=%u outer_limit_lifted=%u",
             object, unsigned(kernel::G<uint8_t>(object + 0x10)), double(range_limit), original, effective,
             percent, unsigned(lifted));
    if (calls == 1 || calls % 30000 == 0)
        LOGR("distance-trace: cumulative gate_calls=%llu scaled=%llu outer_limit_lifted=%llu percent=%u",
             (unsigned long long)calls, (unsigned long long)g_scaled.load(),
             (unsigned long long)g_outer_lifted.load(), percent);
}

}  // namespace

X86_FUNC(__imp__sub_00014A80);
X86_FUNC(sub_00014A80) {
    const uint32_t return_address = kernel::R32(ctx.esp);
    if (return_address != 0x0002900C) {
        __imp__sub_00014A80(ctx, base);
        return;
    }
    // 0x29007 is reached after the object's native state/frustum checks and bit
    // 0x10 test. The property loader maps THING_FARZDIST (116) to object+0x17C.
    const uint32_t object = ctx.ebx;
    __imp__sub_00014A80(ctx, base);
    const uint32_t percent = game::pc_render::ObjectDistancePercent();
    if (percent == 100 && !TraceEnabled()) return;

    const float range_limit = kernel::G<float>(object + 0x17C);
    double& distance = ctx.st[ctx.top & 7];  // Native cdecl result is x87 ST(0).
    const double original = distance;
    if (percent != 100 && std::isfinite(range_limit) && range_limit > 0 &&
        std::isfinite(original) && original >= 0)
        distance = x87_prec(ctx, original * (100.0 / percent));

    // Native comparisons against far and far+10 now cover multiplier times the
    // original range, including its hysteresis band. The time based fade in
    // 0x20AE90 remains native. Do not write the object's property or alpha.
    Trace(object, original, distance, range_limit, percent);
}
