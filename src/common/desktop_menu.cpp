#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include "app/app.h"
#include "common/desktop_menu.h"
#include "core/log.h"

namespace game::desktop_menu {
namespace {
constexpr UINT kDesktopAction = WM_APP + 0x48;
std::atomic<bool> g_pending{false};
}
void Request(Action action) {
    if (g_pending.exchange(true)) return;
    if (!PostMessageW(static_cast<HWND>(app::Window()), kDesktopAction, WPARAM(action), 0)) g_pending = false;
}
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t, intptr_t* result) {
    if (msg != kDesktopAction) return false;
    *result = 0;
    if (wp == uintptr_t(Action::Quit)) {
        LOGR("desktop-menu: quit requested from main menu");
        PostMessageW(static_cast<HWND>(hwnd), WM_CLOSE, 0, 0);
    }
    g_pending = false;
    return true;
}
} // namespace game::desktop_menu
