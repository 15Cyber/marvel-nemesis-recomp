#pragma once
#include <cstdint>
#include <array>
#include <string>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

struct ControllerInfo { bool connected = false; std::string name; };
std::array<ControllerInfo, 4> Controllers();  // physical slots; no title callbacks
void StopRumble();  // all physical controllers, including devices just reassigned

void Init();  // window thread, before boot
// the controller on that port, then GameInfo::pad
Pad Poll(int port);
void Rumble(int port, uint16_t left, uint16_t right);

}  // namespace input
