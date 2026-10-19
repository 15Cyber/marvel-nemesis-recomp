// Cooperative focus pause at presentation boundaries; never on the window thread.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>

#include "app/app.h"
#include "audio/audio.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace app {
namespace {

std::atomic<bool> g_paused{false};
// Only presentation threads acquire this private ownership lock. UI and audio never wait for it.
std::mutex g_owner;

bool ShouldPause() {
    if (!ReadSettings().pause_on_focus_loss) return false;
    HWND hwnd = static_cast<HWND>(Window());
    // Splash/handover windows are deliberately hidden; a destroyed window must release the wait.
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    return IsIconic(hwnd) || GetForegroundWindow() != hwnd;
}

struct FocusHold {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    FocusHold() {
        g_paused.store(true);
        kernel::HoldGuestTime(true);
        audio::SetPaused(true);
        input::StopRumble();
        LOGR("focus: paused (simulation/audio/clocks)");
    }
    ~FocusHold() {
        kernel::HoldGuestTime(false);
        audio::SetPaused(false);
        g_paused.store(false);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        LOGR("focus: resumed after %.3f seconds", seconds);
    }
};

}  // namespace

bool FocusPaused() { return g_paused.load(); }

void WaitWhileUnfocused() {
    for (;;) {
        if (!ShouldPause()) {
            if (!FocusPaused()) return;
            Sleep(2);  // Let the pause owner restore sound/time before another presenting thread proceeds.
            continue;
        }
        std::unique_lock owner(g_owner, std::try_to_lock);
        if (!owner.owns_lock()) {
            Sleep(10);
            continue;
        }
        if (!ShouldPause()) continue;
        FocusHold hold;
        while (ShouldPause()) Sleep(20);
        // Scope restores all state, including when focus returns on a different presentation thread.
    }
}

}  // namespace app
