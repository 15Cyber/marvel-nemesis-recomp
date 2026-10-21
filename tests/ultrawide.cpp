// Standalone geometry contract checks; no game data or renderer/window required.
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <initializer_list>
#include "app/frame_layout.h"
#include "common/ultrawide_math.h"
#include "common/video_layout.h"

namespace {
bool Close(float a, float b) { return std::abs(a - b) <= 1e-5f * std::max(1.0f, std::max(std::abs(a), std::abs(b))); }
struct Size { uint32_t width, height; };
}

int main() {
    const auto standard = app::LayoutFor(1920, 1080, true, true);
    const auto classic = app::LayoutFor(640, 480, true, false);
    assert(Close(standard.aspect, 16.0f / 9) && standard.ui_scale_x == 1);
    assert(Close(classic.aspect, 4.0f / 3) && classic.ui_scale_x == 1);
    assert(app::LayoutFor(0, 0, true, true).ui_scale_x == 1);
    assert(app::LayoutFor(3440, 1440, false, true).ui_scale_x == 1);
    // A deliberate 4:3 console mode on a wide desktop retains its original pillarboxing.
    assert(Close(app::LayoutFor(3440, 1440, true, false).aspect, 4.0f / 3));
    for (const auto size : {Size{3440, 1440}, Size{2560, 1080}}) {
        const auto layout = app::LayoutFor(size.width, size.height, true, true);
        assert(Close(layout.aspect, float(size.width) / size.height));
        const float canvas_width = size.width * layout.ui_scale_x;
        assert(Close(canvas_width / size.height, 16.0f / 9));
        // A legacy square remains square after clip-space scaling and presentation.
        const float relative_width = layout.ui_scale_x * layout.aspect / (16.0f / 9);
        assert(Close(relative_width, 1));
        const float border = (size.width - canvas_width) / 2;
        assert(border > 0 && Close((size.width - border) - border, canvas_width));

        float p[16] = {1.2f, 0, 0, 0, 0, 1.6f, 0, 0, 0.1f, 0, 1.01f, 1, 0, 0, -0.1f, 0};
        float before[16];
        std::memcpy(before, p, sizeof(p));
        assert(game::ultrawide::WidenProjection(p, layout.ui_scale_x));
        for (unsigned i = 0; i < 16; ++i)
            assert(Close(p[i], i % 4 == 0 ? before[i] * layout.ui_scale_x : before[i]));
        // Wider horizontal visibility and unchanged vertical FOV/near/far.
        assert(p[0] < before[0] && p[5] == before[5] && p[10] == before[10] && p[14] == before[14]);
        const float expansion = 1 / layout.ui_scale_x;
        assert(Close(p[0] * expansion, before[0])); // native frustum half-width matches the GPU boundary
        const float half_fov = 0.6f;
        const float widened_angle = std::atan(std::tan(half_fov) * expansion);
        assert(Close(game::ultrawide::WidenConeSine(std::sin(half_fov), expansion), std::sin(widened_angle)));
        for (float film_aspect : {4.0f / 3, 16.0f / 9, 2.76f}) {
            for (uint32_t scale : {1u, 2u, 3u}) {
                const auto video = game::pc_video::VideoExtent(scale, layout.aspect, film_aspect);
                // Displayed width/height equals the decoded aspect in every fit/crop mode.
                assert(Close(video.x * layout.aspect / video.y, film_aspect));
                if (scale == 1) {
                    assert(video.x <= 1 && video.y <= 1);
                    assert(video.x == 1 || video.y == 1); // whole frame, maximum fit
                } else if (scale == 2) {
                    assert(video.x <= layout.ui_scale_x && video.y <= 1);
                    assert(video.x * layout.aspect <= 4.0f / 3 + 1e-5f);
                } else {
                    assert(video.x >= 1 && video.y >= 1);
                    assert(video.x == 1 || video.y == 1); // covers the canvas without stretching
                }
            }
        }
    }
    float ortho[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float unchanged[16];
    std::memcpy(unchanged, ortho, sizeof(ortho));
    assert(!game::ultrawide::WidenProjection(ortho, 0.75f));
    assert(std::memcmp(ortho, unchanged, sizeof(ortho)) == 0);
    assert(!game::ultrawide::WidenProjection(ortho, std::numeric_limits<float>::quiet_NaN()));
    float invalid[16] = {1.2f, 0, 0, 0, 0, 1.6f, 0, 0, 0, 0, 1.01f, 1, 0, 0, -0.1f, 0};
    invalid[8] = std::numeric_limits<float>::infinity();
    std::memcpy(unchanged, invalid, sizeof(invalid));
    assert(!game::ultrawide::WidenProjection(invalid, 0.75f));
    assert(std::memcmp(invalid, unchanged, sizeof(invalid)) == 0);
}
