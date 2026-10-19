#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited
    bool vsync = true;
    bool pause_on_focus_loss = true;  // hold simulation, sound and guest clocks in the background
    uint32_t aniso = 0;              // 0 as the game asks
    std::string monitor;            // empty follows the primary monitor, else a Windows device name
    uint32_t msaa = 0;              // 0 off, 2/4/8 samples on screen-sized targets
    uint32_t shadow_softness = 0;  // 0 original, 1 light, 2 medium; independent of map resolution
    uint32_t shadow_resolution = 0;  // 0 follows render scale, else explicit square pixels
    uint32_t ambient_occlusion = 0;  // 0 off, 1 SSAO, 2 GTAO
    uint32_t ao_strength = 50, ao_radius = 100, ao_debug = 0;
    bool smaa = false;              // optional SMAA 1x, off preserves the original presentation
    bool fps_auto = false;          // cap follows the active window monitor; fps_cap keeps its manual value
    uint32_t brightness = 50, contrast = 50;  // 0..100, neutral at 50
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
};

// Get() writes require UiLock; use ReadSettings() for a snapshot on other threads.
Settings& Get();
Settings ReadSettings();
void Load();
bool Save();  // atomic replacement; false preserves the previous settings file
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
struct Display {
    std::string device;
    int number = 0;
    Mode desktop{};
    uint32_t refresh_hz = 0;
    bool primary = false;
    void* handle = nullptr;  // HMONITOR, valid until the display topology changes
};
std::vector<Display> Displays();
void* MonitorHandle(const std::string& device);  // falls back to the primary monitor
Mode Desktop();
Mode Desktop(const std::string& device);
// Window thread: refresh after an applied mode/topology change or a move to another monitor.
void RefreshDisplayInfo(bool force = false);
uint32_t RefreshRate();  // cached active window monitor rate; 0 if unavailable
uint32_t EffectiveFpsCap();  // auto uses RefreshRate, or 60 if unavailable
Mode Output();
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();

std::filesystem::path ExeDir();
// Portable data beside the executable. Optional title folder; old hdd/E data
// is copied once without replacing an existing destination or removing it.
bool PrepareStorage(std::filesystem::path& directory, std::string& error);
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after reporting missing or incompatible files
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
// Check the bytes about to be loaded against the build hash and an optional title pin.
// Shared by asset discovery and Boot, including hosts that bypass FindGame.
bool ValidateGameImage(const std::vector<uint8_t>& xbe, std::string& sha1, std::string& error,
                       const char* build_sha1 = nullptr);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // Optional title routing: logical Xbox port -> physical SDL slot; -1 has no physical pad.
    int (*controller_port)(int port) = nullptr;
    // Optional title input bridge after platform input, before ImGui::NewFrame.
    void (*ui_input)() = nullptr;
    // Optional reviewed XBE revision for title-specific fixed-address hooks. Unlike the
    // generated build hash, this must not change automatically when assets are replaced.
    const char* supported_xbe_sha1 = nullptr;
    const char* data_directory = nullptr; // nullptr keeps hdd; relative folder beside the exe
    const char* mods_directory = nullptr; // optional read-only D: file overlay beside the exe
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();
// Guest presentation thread only, before taking renderer/UI locks. Window messages keep running.
void WaitWhileUnfocused();
bool FocusPaused();

}  // namespace app
