#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace input { struct Pad; }

namespace game::settings_panel {
bool& FpsCounter();
void Initialize();
void WindowReady(void* hwnd);
// May be called from a guest menu callback; opening stays on the window thread.
void RequestOpen();
void PrepareInput();
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void Draw(double fps);
void FilterPad(int port, input::Pad& pad);
void FilterRumble(int port, uint16_t& left, uint16_t& right);

// Host-only model for the game's APT renderer. These calls hold UiLock only
// internally; the caller must release it before calling any guest function.
enum class MenuAction { Up, Down, Left, Right, Select, Back };
struct MenuView {
    bool open = false;
    std::string title, hint;
    std::vector<std::string> rows;
    int selected = 0;
};
bool NativeBegin(bool development = false);
void NativeAction(MenuAction action);
std::optional<MenuAction> TakeNativeSound(); // drain on the guest thread, outside UiLock
unsigned TakeNativeMusicToggles(); // apply music gain on the guest thread
MenuView NativeView();
void NativeEnd();  // cancels an outstanding display preview, never saves implicitly
}  // namespace game::settings_panel
