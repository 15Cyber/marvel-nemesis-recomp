#pragma once
#include <cstdint>

namespace game::desktop_menu {
enum class Action { Quit };
// Guest callbacks only post requests; desktop work runs on the window thread.
void Request(Action action);
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
}
