// Null NAD entries are accepted by the retail NMS parser (0x93D70-0x93D75),
// but its motion-set destructor calls Release on them without a null check.
// Combat Test exposes this when quitting. Keep the fix at that exact caller.
#include "core/log.h"
#include "xdk/hle.h"
#include <atomic>
#include <cstring>

X86_FUNC(__imp__sub_0020E840);
X86_FUNC(sub_0020E840) {
    if (!ctx.ecx && kernel::R32(ctx.esp) == 0x00093A6C) {
        static std::atomic<uint32_t> skipped{0};
        const auto count = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 8 || (count & (count - 1)) == 0) {
            char name[33]{};
            // The caller keeps the NMS descriptor in EBP: name[32] at +4.
            if (ctx.ebp >= 0x10000 && ctx.ebp <= 0xFFFFFFDC &&
                mem::IsCommitted(ctx.ebp) && mem::IsCommitted(ctx.ebp + 0x23)) {
                std::memcpy(name, kernel::HostPtr(ctx.ebp + 4), 32);
                for (char& c : name)
                    if (c && (static_cast<unsigned char>(c) < 32 ||
                              static_cast<unsigned char>(c) > 126)) c = '?';
            }
            LOGR("development: skipped null animation release during motion-set cleanup nms=%s cache=%08X entry=%u total=%u",
                 name, ctx.ebp, ctx.edi, count);
        }
        // Native thiscall Release has no stack arguments and returns void.
        // Leave the original destructor running: it frees the entry arrays,
        // unregisters the motion set, and releases every non-null animation.
        ctx.esp += 4;
        return;
    }
    __imp__sub_0020E840(ctx, base);
}
