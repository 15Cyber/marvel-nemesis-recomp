// Retail camera builder: keep projection, inverse matrices and native CPU frustum in agreement.
#include <cstring>
#include "app/app.h"
#include "common/ultrawide_math.h"
#include "game/ultrawide.h"
#include "core/log.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

X86_FUNC(__imp__sub_001CFD60);
X86_FUNC(__imp__sub_00205690);

namespace {
thread_local uint32_t g_scene_camera = 0;
thread_local uint32_t g_camera = 0;
thread_local float g_expansion = 1;

bool FullWidth() {
    std::lock_guard lock(d3d::Lock());
    return d3d::State().full_width;
}
}  // namespace

game::ultrawide::SceneScope::SceneScope(uint32_t camera) : canvas_(camera != 0), previous_camera_(g_scene_camera) {
    g_scene_camera = camera;
}
game::ultrawide::SceneScope::~SceneScope() { g_scene_camera = previous_camera_; }

// Camera argument is cdecl. This call follows the native projection/view-projection rebuild in 0x208940.
X86_FUNC(sub_001CFD60) {
    if (kernel::R32(ctx.esp) == 0x00208B41) {
        g_camera = 0;
        g_expansion = 1;
        const float scale = app::PresentationLayout().ui_scale_x;
        const uint32_t camera = kernel::R32(ctx.esp + 4);
        if (scale < 1 && FullWidth() && camera && camera == g_scene_camera &&
            !(kernel::R32(camera + 0x298) & 0x10)) {
            float projection[16];
            std::memcpy(projection, kernel::HostPtr(camera + 0x80), sizeof(projection));
            if (game::ultrawide::WidenProjection(projection, scale)) {
                std::memcpy(kernel::HostPtr(camera + 0x80), projection, sizeof(projection));
                const float cone = kernel::G<float>(camera + 0x2AC);
                if (std::isfinite(cone) && cone >= 0 && cone <= 1)
                    kernel::G<float>(camera + 0x2AC) = game::ultrawide::WidenConeSine(cone, 1 / scale);
                // 0x1CFD60 builds inverses from both matrices. Rebuild the combined matrix first.
                const X86Context saved = ctx;
                kernel::CallGuest(ctx, 0x00119A7A, {camera + 0xC0, camera + 0x40, camera + 0x80});
                ctx = saved;
                g_camera = camera;
                g_expansion = 1 / scale;
                static float logged_scale = 1;
                if (scale != logged_scale) {
                    LOGR("ultrawide: scene camera=%08X horizontal_scale=%.6f; vertical FOV/depth unchanged, UI centred 16:9",
                         camera, double(scale));
                    logged_scale = scale;
                }
            }
        }
        if (scale < 1 && camera == g_scene_camera && !g_camera) {
            // Unknown/custom cameras retain the centred original frame rather than stretching it.
            std::lock_guard lock(d3d::Lock());
            d3d::State().full_width = false;
            static unsigned fallback_logs = 0;
            if (fallback_logs++ < 4) LOGR("ultrawide: unsupported scene projection; using centred 16:9");
        }
    }
    __imp__sub_001CFD60(ctx, base);
}

// thiscall(frustum, camera, focal length, half height, half width, near, far), ret 24.
X86_FUNC(sub_00205690) {
    const uint32_t width_address = ctx.esp + 0x10;
    const uint32_t width_bits = kernel::R32(width_address);
    const float half_width = kernel::G<float>(width_address);
    const bool widen = kernel::R32(ctx.esp) == 0x00208BDA && g_camera &&
                       ctx.ecx == g_camera + 0x190 && FullWidth() && g_expansion > 1 &&
                       std::isfinite(half_width) && half_width > 0;
    if (widen) kernel::G<float>(width_address) = half_width * g_expansion;
    __imp__sub_00205690(ctx, base);
    if (widen) kernel::W32(width_address, width_bits);
}
