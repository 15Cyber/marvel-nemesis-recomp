// Host input mapping only; no guest addresses or game assets.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <iterator>
#include <utility>
#include <imgui.h>
#include <toml++/toml.hpp>
#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "common/pc_controls.h"

namespace game::pc_controls {
namespace {
const char* kLabels[kActions] = {
    "D-pad: up", "D-pad: down", "D-pad: left", "D-pad: right",
    "Start", "Back", "Left stick click", "Right stick click",
    "A (Xbox)", "B (Xbox)", "X (Xbox)", "Y (Xbox)", "Black / RB", "White / LB", "Left trigger", "Right trigger",
    "Move: up", "Move: down", "Move: left", "Move: right",
    "Right stick: up", "Right stick: down", "Right stick: left", "Right stick: right"
};
Settings g_settings;
std::array<bool, 256> g_keys{};
HWND g_window = nullptr;
bool g_focused = false;
int g_capture = -1;
uint32_t g_capture_key = 0;
bool g_capture_ready = false;

bool Bindable(uint32_t vk) {
    return vk >= 8 && vk <= 255 && vk != VK_F3 && vk != VK_F10 && vk != VK_ESCAPE &&
           vk != VK_LWIN && vk != VK_RWIN && vk != VK_MENU && vk != VK_LMENU && vk != VK_RMENU;
}
uint32_t Key(uintptr_t wp, intptr_t lp) {
    uint32_t vk = uint32_t(wp);
    if (vk == VK_SHIFT) vk = MapVirtualKeyW(uint32_t(lp >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX);
    else if (vk == VK_CONTROL) vk = (lp & (intptr_t(1) << 24)) ? VK_RCONTROL : VK_LCONTROL;
    return vk < 256 ? vk : 0;
}
void Normalize(Settings& s) {
    if (s.keyboard_player > 2) s.keyboard_player = 1;
    for (auto& device : s.controller) if (device > 4) device = 4;
    if (s.controller[0] < 4 && s.controller[0] == s.controller[1]) s.controller[1] = 4;
    // The main menu needs player 1. Keep a recoverable input if a file disables it entirely.
    if (s.controller[0] == 4 && s.keyboard_player != 1) s.keyboard_player = 1;
    for (auto& vk : s.keys) if (vk && !Bindable(vk)) vk = 0;
    for (auto& player : s.buttons) for (auto& source : player) if (source > kButtons) source = kButtons;
}
std::string KeyName(uint32_t vk) {
    if (!vk) return "Unbound";
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    LONG flags = LONG((scan & 0xFF) << 16);
    if ((scan & 0xFF00) || vk == VK_RIGHT || vk == VK_LEFT || vk == VK_UP || vk == VK_DOWN ||
        vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_DELETE || vk == VK_INSERT)
        flags |= 1 << 24;
    wchar_t wide[96]{};
    int n = GetKeyNameTextW(flags, wide, 96);
    if (n <= 0) return "VK " + std::to_string(vk);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, n, nullptr, 0, nullptr, nullptr);
    std::string text(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, n, text.data(), bytes, nullptr, nullptr);
    return text;
}
uint8_t Button(const input::Pad& p, uint32_t button) {
    if (button < 8) return (p.buttons & (1u << button)) ? 255 : 0;
    return button < kButtons ? p.analog[button - 8] : 0;
}
void PutButton(input::Pad& p, size_t button, uint8_t value) {
    if (button < 8) { if (value >= 128) p.buttons |= uint16_t(1u << button); }
    else if (button < kButtons) p.analog[button - 8] = std::max(p.analog[button - 8], value);
}
int16_t MergeAxis(int16_t pad, int keyboard) {
    return std::abs(keyboard) > std::abs(int(pad)) ? int16_t(keyboard) : pad;
}
}  // namespace

Settings::Settings() {
    keys = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_TAB, VK_LSHIFT, VK_RSHIFT,
            VK_SPACE, 'L', 'J', 'I', 'E', 'Q', 'U', 'K',
            'W', 'S', 'A', 'D', VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4, VK_NUMPAD6};
    for (auto& player : buttons) for (size_t i = 0; i < kButtons; ++i) player[i] = uint32_t(i);
}
Settings Read() {
    std::lock_guard lock(app::UiLock());
    return g_settings;
}
void CancelCapture() {
    std::lock_guard lock(app::UiLock());
    g_capture = -1;
    g_capture_ready = false;
}
void ClearKeys() {
    std::lock_guard lock(app::UiLock());
    g_keys.fill(false);
}
void BeginCapture(size_t action) {
    std::lock_guard lock(app::UiLock());
    ClearKeys();
    g_capture = action < kActions ? int(action) : -1;
    g_capture_ready = false;
}
bool Capturing() {
    std::lock_guard lock(app::UiLock());
    return g_capture >= 0;
}
bool TakeCapture(Settings& draft) {
    std::lock_guard lock(app::UiLock());
    if (!g_capture_ready || g_capture < 0) return false;
    draft.keys[size_t(g_capture)] = g_capture_key;
    CancelCapture();
    return true;
}
const char* ActionLabel(size_t action) { return action < kActions ? kLabels[action] : "Unbound"; }
std::string KeyLabel(uint32_t key) { return KeyName(key); }
void Set(const Settings& settings) {
    std::lock_guard lock(app::UiLock());
    input::StopRumble();
    g_settings = settings;
    Normalize(g_settings);
    ClearKeys();
    CancelCapture();
    LOGR("input: keyboard_player=%u controller_p1=%u controller_p2=%u (0..3 physical, 4 none)",
         g_settings.keyboard_player, g_settings.controller[0], g_settings.controller[1]);
}
void Load(const void* table) {
    const auto& t = *static_cast<const toml::table*>(table);
    const auto& c = t["pc_input"];
    Settings s;
    s.keyboard_player = c["keyboard_player"].value_or(s.keyboard_player);
    s.controller[0] = c["controller_p1"].value_or(s.controller[0]);
    s.controller[1] = c["controller_p2"].value_or(s.controller[1]);
    if (const auto* values = c["keys"].as_array())
        for (size_t i = 0; i < std::min(values->size(), kActions); ++i)
            s.keys[i] = (*values)[i].value_or(s.keys[i]);
    const char* names[] = {"buttons_p1", "buttons_p2"};
    for (size_t p = 0; p < 2; ++p) if (const auto* values = c[names[p]].as_array())
        for (size_t i = 0; i < std::min(values->size(), kButtons); ++i)
            s.buttons[p][i] = (*values)[i].value_or(s.buttons[p][i]);
    Normalize(s);
    g_settings = s;  // Load already owns UiLock; SDL may not be initialized yet.
}
void Save(void* table) {
    toml::array keys, p1, p2;
    for (auto key : g_settings.keys) keys.push_back(int64_t(key));
    for (auto b : g_settings.buttons[0]) p1.push_back(int64_t(b));
    for (auto b : g_settings.buttons[1]) p2.push_back(int64_t(b));
    toml::table c{{"keyboard_player", int64_t(g_settings.keyboard_player)},
                  {"controller_p1", int64_t(g_settings.controller[0])},
                  {"controller_p2", int64_t(g_settings.controller[1])},
                  {"keys", std::move(keys)}, {"buttons_p1", std::move(p1)}, {"buttons_p2", std::move(p2)}};
    static_cast<toml::table*>(table)->insert_or_assign("pc_input", std::move(c));
}
int ControllerPort(int logical_port) {
    std::lock_guard lock(app::UiLock());
    if (logical_port < 0 || logical_port > 1) return -1;
    auto slot = g_settings.controller[size_t(logical_port)];
    return slot < 4 ? int(slot) : -1;
}
void ApplyPad(int port, input::Pad& pad) {
    std::lock_guard lock(app::UiLock());
    if (port < 0 || port > 1) return;
    const input::Pad raw = pad;
    pad.buttons = 0;
    std::fill(std::begin(pad.analog), std::end(pad.analog), 0);
    for (size_t i = 0; i < kButtons; ++i) PutButton(pad, i, Button(raw, g_settings.buttons[size_t(port)][i]));
    if (g_settings.keyboard_player != uint32_t(port + 1)) return;
    // A keyboard is a connected virtual Xbox pad even before the first key press.
    pad.connected = true;
    if (!g_focused || GetForegroundWindow() != g_window) return;
    auto down = [&](size_t i) { auto vk = g_settings.keys[i]; return vk && g_keys[vk]; };
    for (size_t i = 0; i < kButtons; ++i) if (down(i)) PutButton(pad, i, 255);
    int lx = int(down(19)) - int(down(18)), ly = int(down(16)) - int(down(17));
    int rx = int(down(23)) - int(down(22)), ry = int(down(20)) - int(down(21));
    int left = lx && ly ? 23170 : 32767, right = rx && ry ? 23170 : 32767;
    pad.lx = MergeAxis(pad.lx, lx * left); pad.ly = MergeAxis(pad.ly, ly * left);
    pad.rx = MergeAxis(pad.rx, rx * right); pad.ry = MergeAxis(pad.ry, ry * right);
}
void WindowReady(void* hwnd) {
    std::lock_guard lock(app::UiLock());
    g_window = static_cast<HWND>(hwnd);
    g_focused = GetForegroundWindow() == g_window;
    ClearKeys();
}
bool Message(uint32_t msg, uintptr_t wp, intptr_t lp, bool panel_open) {
    std::lock_guard lock(app::UiLock());
    if (msg == WM_ACTIVATE) { g_focused = LOWORD(wp) != WA_INACTIVE; ClearKeys(); }
    if (msg == WM_KILLFOCUS || msg == WM_DESTROY) { g_focused = false; ClearKeys(); CancelCapture(); }
    if (msg == WM_SETFOCUS) { g_focused = true; ClearKeys(); }
    bool down = msg == WM_KEYDOWN, up = msg == WM_KEYUP;
    if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) { ClearKeys(); return false; }
    if (!down && !up) return false;
    const uint32_t vk = Key(wp, lp);
    if (up && vk) g_keys[vk] = false;
    if (panel_open) {
        if (down && g_capture >= 0) {
            // Win32 already queued this key for ImGui before the title callback.
            // Binding Space/Enter/Tab must not also click/navigate the focused UI item.
            if (ImGui::GetCurrentContext()) {
                ImGui::GetIO().ClearEventsQueue();
                ImGui::GetIO().ClearInputKeys();
            }
            if (wp == VK_ESCAPE) CancelCapture();
            else if (!g_capture_ready && !(lp & (intptr_t(1) << 30)) && Bindable(vk)) {
                g_capture_key = vk;
                g_capture_ready = true;
            }
            return true;
        }
        return false;
    }
    // Ignore autorepeat after focus/panel transitions: require a fresh press.
    if (down && vk && Bindable(vk) && !(lp & (intptr_t(1) << 30))) g_keys[vk] = true;
    return false;
}
void Draw(Settings& draft) {
    TakeCapture(draft);
    ImGui::TextWrapped("Keyboard and controller can control the same player. Player 2 remains independent.");
    const char* keyboards[] = {"Off", "Player 1", "Player 2"};
    int keyboard = int(draft.keyboard_player);
    if (ImGui::Combo("Keyboard player", &keyboard, keyboards, 3)) {
        if (keyboard == 1 || draft.controller[0] < 4) draft.keyboard_player = uint32_t(keyboard);
    }
    static std::array<input::ControllerInfo, 4> devices;
    static ULONGLONG next_devices = 0;
    if (GetTickCount64() >= next_devices) { devices = input::Controllers(); next_devices = GetTickCount64() + 500; }
    auto deviceName = [&](uint32_t i) {
        if (i >= 4) return std::string("No controller");
        return "Controller " + std::to_string(i + 1) + (devices[i].connected ? " — " + devices[i].name : " (disconnected)");
    };
    for (size_t player = 0; player < 2; ++player) {
        ImGui::PushID(int(player));
        const char* label = player == 0 ? "Player 1 controller" : "Player 2 controller";
        if (ImGui::BeginCombo(label, deviceName(draft.controller[player]).c_str())) {
            for (uint32_t i = 0; i <= 4; ++i) {
                bool invalid = player == 0 && i == 4 && draft.keyboard_player != 1;
                // Swapping must not leave P1 without any configured input.
                if (player == 1 && i < 4 && draft.controller[0] == i &&
                    draft.controller[1] == 4 && draft.keyboard_player != 1) invalid = true;
                ImGui::BeginDisabled(invalid);
                if (ImGui::Selectable(deviceName(i).c_str(), draft.controller[player] == i)) {
                    uint32_t old = draft.controller[player];
                    if (i < 4 && draft.controller[1 - player] == i) draft.controller[1 - player] = old;
                    draft.controller[player] = i;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    ImGui::TextWrapped("Default: keyboard + controller 1 for player 1; controller 2 for player 2. For keyboard versus controller, assign the connected controller to player 2. Selecting the same device swaps players.");
    ImGui::TextWrapped("New controllers use the first free physical slot. Slot numbers may change after reconnecting.");
    if (ImGui::Button("Restore Default Controls")) { draft = Settings{}; CancelCapture(); }
    if (ImGui::CollapsingHeader("Keyboard bindings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("WASD: movement. Arrows: D-pad/menus. Enter: Start. Space: A. J/I/L: X/Y/B. Num 8/2/4/6: right stick. Num Lock affects the keypad.");
        ImGui::TextWrapped("Button names refer to Xbox controls. F3/F10 are reserved. Esc cancels key capture.");
        if (g_capture >= 0) ImGui::Text("Press a key for: %s", kLabels[g_capture]);
        for (size_t i = 0; i < kActions; ++i) {
            ImGui::PushID(int(i));
            ImGui::TextUnformatted(kLabels[i]);
            ImGui::SameLine();
            if (ImGui::Button(KeyName(draft.keys[i]).c_str())) { g_capture = int(i); g_capture_ready = false; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) { draft.keys[i] = 0; if (g_capture == int(i)) CancelCapture(); }
            ImGui::PopID();
        }
    }
    for (size_t player = 0; player < 2; ++player) {
        ImGui::PushID(int(player));
        const char* title = player ? "Controller buttons - player 2" : "Controller buttons - player 1";
        if (ImGui::CollapsingHeader(title)) {
            ImGui::TextWrapped("Left: Xbox button sent to the game. List: physical button that triggers it. Stick axes remain unchanged.");
            for (size_t target = 0; target < kButtons; ++target) {
                ImGui::PushID(int(target));
                auto source = draft.buttons[player][target];
                if (ImGui::BeginCombo(kLabels[target], source < kButtons ? kLabels[source] : "Unbound")) {
                    for (uint32_t choice = 0; choice <= kButtons; ++choice)
                        if (ImGui::Selectable(choice < kButtons ? kLabels[choice] : "Unbound", source == choice))
                            draft.buttons[player][target] = choice;
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }
}
}  // namespace game::pc_controls
