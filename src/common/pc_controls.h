#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
namespace input { struct Pad; }
namespace game::pc_controls {
constexpr std::size_t kButtons = 16, kActions = 24;
struct Settings {
    uint32_t keyboard_player = 1;  // 0 off, 1 P1, 2 P2; shares a port with its controller
    std::array<uint32_t, 2> controller = {0, 1};  // physical SDL slots 0..3; 4 none
    std::array<uint32_t, kActions> keys{};
    std::array<std::array<uint32_t, kButtons>, 2> buttons{};
    Settings();
    bool operator==(const Settings&) const = default;
};
Settings Read();
void Set(const Settings& settings);  // normalizes, stops old motors and clears held keys
void Load(const void* table);
void Save(void* table);
int ControllerPort(int logical_port);
void ApplyPad(int logical_port, input::Pad& pad);
void WindowReady(void* hwnd);
void ClearKeys();
void CancelCapture();
void BeginCapture(std::size_t action);
bool Capturing();
bool TakeCapture(Settings& draft);
const char* ActionLabel(std::size_t action);
std::string KeyLabel(uint32_t key);
bool Message(uint32_t msg, uintptr_t wp, intptr_t lp, bool panel_open);
void Draw(Settings& draft);
}  // namespace game::pc_controls
