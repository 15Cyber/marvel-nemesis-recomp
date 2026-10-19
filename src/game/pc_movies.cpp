// Marvel's VP6 player, scoped to this XBE. Never identify a movie by a generic fullscreen quad.
#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
#include "app/app.h"
#include "common/pc_video.h"
#include "core/log.h"
#include "xdk/hle.h"

using namespace kernel;
X86_FUNC(__imp__sub_001108D0);
X86_FUNC(__imp__sub_001104C0);
X86_FUNC(__imp__sub_00110F60);

namespace {
std::recursive_mutex g_movie_lock;
struct Layout {
    uint32_t player = 0, scale = 0, width = 0, height = 0, output_w = 0, output_h = 0;
    bool valid = false;
} g_layout;
uint32_t g_skip_logs = 0;
uint32_t g_layout_logs = 0;

uint32_t GetInfo(X86Context& ctx, uint32_t player) {
    uint32_t frame = R32(player + 0x68);
    uint32_t surface = frame ? R32(frame + 4) : 0;
    if (!surface) return 0;
    const X86Context saved = ctx;
    ctx.ecx = surface;
    uint32_t info = CallGuest(ctx, 0x001B7529, {});
    ctx = saved;
    return info;
}
void AdjustLayout(X86Context& ctx, uint32_t player) {
    const auto settings = game::pc_video::Read();
    const auto output = game::pc_video::Output();
    uint32_t screen = R32(0x005246C8);
    uint32_t w = screen ? R16(screen + 0x78) : 0, h = screen ? R16(screen + 0x76) : 0;
    g_layout = {player, settings.scale, w, h, output.width, output.height, settings.scale == 0};
    if (!settings.scale) return;
    uint32_t info = GetInfo(ctx, player);
    // Confirmed by native quad X/Y extents: screen+0x78 width/+0x76 height;
    // decoder info+0x18 width/+0x1C height. Do not transpose these pairs.
    uint32_t film_w = info ? R32(info + 0x18) : 0, film_h = info ? R32(info + 0x1C) : 0;
    if (!w || !h || !output.width || !output.height || !film_w || !film_h ||
        film_w > 8192 || film_h > 8192) {
        if (g_skip_logs++ < 4) LOGR("movies: layout unchanged; dimensions unavailable guest=%ux%u film=%ux%u",
                                    w, h, film_w, film_h);
        return;
    }
    float aspect = float(output.width) / output.height, film_aspect = float(film_w) / film_h;
    float nx = 1, ny = 1;
    if (settings.scale == 3) {
        if (film_aspect > aspect) nx = film_aspect / aspect;
        else ny = aspect / film_aspect;
    } else {
        float canvas = aspect;
        if (settings.scale == 2) {
            canvas = 4.0f / 3;
            if (aspect > canvas) nx = canvas / aspect;
            else ny = aspect / canvas;
        }
        if (film_aspect > canvas) ny *= canvas / film_aspect;
        else nx *= film_aspect / canvas;
    }
    float left = (1 - nx) * w * 0.5f - 0.5f, top = (1 - ny) * h * 0.5f - 0.5f;
    float right = left + nx * w, bottom = top + ny * h;
    const float xy[4][2] = {{left, top}, {right, top}, {right, bottom}, {left, bottom}};
    // The image quad is four 28-byte XYZRHW/color/UV vertices at player+0x70.
    // Leave Z/RHW/colour/UV and the native full-screen black background at +0xE0 intact.
    for (uint32_t i = 0; i < 4; ++i) {
        W32(player + 0x70 + i * 0x1C, std::bit_cast<uint32_t>(xy[i][0]));
        W32(player + 0x74 + i * 0x1C, std::bit_cast<uint32_t>(xy[i][1]));
    }
    g_layout.valid = true;
    if (g_layout_logs++ < 16)
        LOGR("movies: layout scale=%u guest=%ux%u output=%ux%u film=%ux%u quad=%.1f,%.1f..%.1f,%.1f",
             settings.scale, w, h, output.width, output.height, film_w, film_h, left, top, right, bottom);
}
}  // namespace

// Native geometry builder, thiscall(player), no stack arguments.
X86_FUNC(sub_001108D0) {
    std::lock_guard lock(g_movie_lock);
    uint32_t player = ctx.ecx;
    __imp__sub_001108D0(ctx, base);
    AdjustLayout(ctx, player);
}

// Native frame upload and drawing, thiscall(player, decoded_frame).
X86_FUNC(sub_001104C0) {
    std::lock_guard lock(g_movie_lock);
    const uint32_t player = ctx.ecx;
    auto settings = game::pc_video::Read();
    auto output = game::pc_video::Output();
    uint32_t screen = R32(0x005246C8);
    uint32_t w = screen ? R16(screen + 0x78) : 0, h = screen ? R16(screen + 0x76) : 0;
    bool changed = !g_layout.valid || g_layout.player != player || g_layout.scale != settings.scale ||
                   g_layout.width != w || g_layout.height != h ||
                   g_layout.output_w != output.width || g_layout.output_h != output.height;
    if (changed && (settings.scale || g_layout.scale)) {
        // Rebuild from native geometry, never accumulate transforms. CallGuest supplies a real call frame.
        const X86Context saved = ctx;
        ctx.ecx = player;
        CallGuest(ctx, 0x001108D0, {});
        ctx = saved;
    }
    __imp__sub_001104C0(ctx, base);
}

// Native player state dispatch. State 5 performs sub_00110860 (release) and calls +0x20 completion.
// IDs 0..4 are regional EA logos, ID 5 Marvel in the XBE table at 0x00489928.
X86_FUNC(sub_00110F60) {
    uint32_t player = ctx.ecx;
    if (game::pc_video::Read().skip_logos && R32(player + 4) == 3) {
        uint32_t id = R32(player + 0x18);
        if (id <= 5) {
            W32(player + 4, 5);
            LOGR("movies: skip logo id=%u via native completion", id);
        }
    }
    __imp__sub_00110F60(ctx, base);
}
