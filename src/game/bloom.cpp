// This XBE's main world renderer finishes before the caller draws screen effects/HUD.
#include "common/pc_render.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

X86_FUNC(__imp__sub_000299F0);
X86_FUNC(sub_000299F0) {
    // 0x2CE99 is the world call in sub_0002CE20; 0x297D0 renders previews separately.
    // Apply once after both possible sub_00029530 calls, including their FX flushes.
    const bool main_world = kernel::R32(ctx.esp) == 0x0002CE9E;
    const auto parameters = game::pc_render::BloomSettings();
    __imp__sub_000299F0(ctx, base);
    static bool was_enabled = false;  // The game's render thread owns this hook.
    if (main_world && (parameters.enabled || was_enabled)) {
        d3d::Bloom(parameters);
        was_enabled = parameters.enabled != 0;
    }
}
