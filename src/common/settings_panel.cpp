// Shared PC settings panel, accessible through native APT entries and F10.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <initializer_list>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "app/app.h"
#include "common/settings_panel.h"
#include "common/pc_controls.h"
#include "common/pc_video.h"
#include "common/pc_render.h"
#include "common/pc_cheats.h"
#include "common/pc_development.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/smaa.h"
#include "gpu/msaa.h"
#include "gpu/shadow_resolution.h"
#include "gpu/ambient_occlusion.h"

namespace game::settings_panel {
namespace {

constexpr UINT_PTR kConfirmTimer = 0x4D4E4631;
constexpr UINT kUpdateCursor = WM_APP + 0x46;
constexpr UINT kOpenFromMenu = WM_APP + 0x47;
constexpr ULONGLONG kConfirmMs = 15000;
std::atomic<bool> g_open{false};
std::atomic<bool> g_open_pending{false};
std::array<std::atomic<bool>, 2> g_release{};
bool g_nav_ready = false, g_back_down = false;
thread_local input::Pad* g_ui_poll = nullptr;

bool Neutral(const input::Pad& pad) {
    return !pad.buttons && std::all_of(std::begin(pad.analog), std::end(pad.analog),
        [](uint8_t v) { return v < 128; }) && std::abs(int(pad.lx)) < 10000 &&
        std::abs(int(pad.ly)) < 10000 && std::abs(int(pad.rx)) < 10000 && std::abs(int(pad.ry)) < 10000;
}

// The draft excludes unrelated settings (such as system-link join_ip).
struct Video {
    app::DisplayMode mode = app::DisplayMode::Borderless;
    uint32_t width = 0, height = 0, scale = 100, cap = 60, aniso = 0;
    bool vsync = true, counter = false, pause_on_focus_loss = true;
    std::string monitor;
    bool auto_fps = false;
    bool smaa = false;
    uint32_t msaa = 0, shadow_resolution = 0, shadow_softness = 0;
    uint32_t ao = 0, ao_strength = 50, ao_radius = 100, ao_debug = 0;
    uint32_t brightness = 50, contrast = 50;
    pc_controls::Settings input;
    pc_video::Settings movies;
    pc_render::Settings rendering;
    pc_cheats::Settings cheats;
    bool operator==(const Video&) const = default;
};
struct Resolution {
    uint32_t width, height;
    bool supported;
};
Video g_draft, g_before;
std::vector<Resolution> g_modes;
std::vector<app::Display> g_displays;
ULONGLONG g_next_displays = 0;
ULONGLONG g_next_refresh = 0;
HMONITOR g_monitor = nullptr;
bool g_confirm = false, g_focus = false, g_save_failed = false, g_timer_ready = false;
ULONGLONG g_deadline = 0;
std::string g_status;

enum class Page { Home, Display, Graphics, Lighting, Controls, Keyboard, Pad1, Pad2, Videos, Leave, Defaults,
                  Cheats, Unlocks, CheatP1, CheatP2, ConfirmUnlock,
                  Development, Missions, TestLevels, ConfirmLevel };
pc_cheats::Unlock g_unlock_choice = pc_cheats::Unlock::None;
bool g_development = false;
uint32_t g_native_controller = 0;
std::optional<pc_development::Destination> g_destination;
std::atomic<bool> g_native{false};
Page g_page = Page::Home;
int g_selected = 0;
bool g_editing = false, g_waiting_key = false;
Video g_edit_before;
std::vector<std::pair<Page, int>> g_history;
uint32_t g_native_held = 0;
std::array<ULONGLONG, 6> g_native_repeat{};

Video Current() {
    const auto s = app::ReadSettings();
    Video video;
    video.mode = s.display_mode;
    video.width = s.width;
    video.height = s.height;
    video.scale = s.render_percent;
    video.cap = s.fps_cap;
    video.aniso = s.aniso;
    video.vsync = s.vsync;
    video.pause_on_focus_loss = s.pause_on_focus_loss;
    video.counter = FpsCounter();
    video.monitor = s.monitor;
    video.auto_fps = s.fps_auto;
    video.smaa = s.smaa;
    video.msaa = s.msaa;
    video.shadow_resolution = s.shadow_resolution;
    video.shadow_softness = s.shadow_softness;
    video.ao = s.ambient_occlusion;
    video.ao_strength = s.ao_strength;
    video.ao_radius = s.ao_radius;
    video.ao_debug = s.ao_debug;
    video.brightness = s.brightness;
    video.contrast = s.contrast;
    video.input = pc_controls::Read();
    video.movies = pc_video::Read();
    video.rendering = pc_render::Read();
    video.cheats = pc_cheats::Read();
    return video;
}

// Called with UiLock held. Render/guest/window readers take the same lock.
void Set(const Video& v) {
    auto& s = app::Get();
    s.display_mode = v.mode;
    s.width = v.width;
    s.height = v.height;
    s.render_percent = v.scale;
    s.fps_cap = v.cap;
    s.aniso = v.aniso;
    s.vsync = v.vsync;
    s.pause_on_focus_loss = v.pause_on_focus_loss;
    s.monitor = v.monitor;
    s.fps_auto = v.auto_fps;
    s.smaa = v.smaa;
    s.msaa = v.msaa;
    s.shadow_resolution = v.shadow_resolution;
    s.shadow_softness = v.shadow_softness;
    s.ambient_occlusion = v.ao;
    s.ao_strength = v.ao_strength;
    s.ao_radius = v.ao_radius;
    s.ao_debug = v.ao_debug;
    s.brightness = v.brightness;
    s.contrast = v.contrast;
    FpsCounter() = v.counter;
    pc_controls::Set(v.input);
    pc_video::Set(v.movies);
    pc_render::Set(v.rendering);
    pc_cheats::Set(v.cheats);
}

void RefreshModes() {
    g_displays = app::Displays();
    g_next_displays = GetTickCount64() + 1000;
    g_monitor = static_cast<HMONITOR>(app::MonitorHandle(g_draft.monitor));
    g_modes.clear();
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(g_monitor, &info)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(info.szDevice, i, &dm); ++i) {
            if (dm.dmBitsPerPel >= 24 && dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480)
                g_modes.push_back({dm.dmPelsWidth, dm.dmPelsHeight, true});
        }
    }
    std::sort(g_modes.begin(), g_modes.end(), [](const Resolution& a, const Resolution& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    g_modes.erase(std::unique(g_modes.begin(), g_modes.end(), [](const Resolution& a, const Resolution& b) {
        return a.width == b.width && a.height == b.height;
    }), g_modes.end());
    // Preserve a manually configured window size without claiming monitor support.
    if (g_draft.width && g_draft.height &&
        std::none_of(g_modes.begin(), g_modes.end(), [](const Resolution& m) {
            return m.width == g_draft.width && m.height == g_draft.height;
        })) g_modes.push_back({g_draft.width, g_draft.height, false});
}

void Revert(const char* reason) {
    if (!g_confirm) return;
    g_confirm = false;
    Set(g_before);
    g_draft = g_before;
    app::ApplyDisplay();
    g_status = reason;
    LOGR("settings: display preview reverted (%s)", reason);
}

void Save() {
    g_save_failed = !app::Save();
    g_status = g_save_failed
        ? "Applied, but settings.toml could not be saved. Try Apply again."
        : "Settings applied and saved.";
    if (!g_save_failed) app::LogSettings();
}

// Cursor ownership stays on the window thread. Never change it over another app,
// the title bar, or resize borders; only the game's own client area.
void UpdateCursor(HWND hwnd) {
    if (GetForegroundWindow() != hwnd) return;
    POINT point{};
    RECT client{};
    if (!GetCursorPos(&point) || WindowFromPoint(point) != hwnd ||
        !ScreenToClient(hwnd, &point) || !GetClientRect(hwnd, &client) || !PtInRect(&client, point)) return;
    SetCursor(g_open.load() ? LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) : nullptr);
}

void RequestCursorUpdate() {
    PostMessageW(static_cast<HWND>(app::Window()), kUpdateCursor, 0, 0);
}

void Close() {
    Revert("Display change cancelled.");
    pc_cheats::CancelConfirmation();
    g_draft = Current();
    pc_controls::ClearKeys();
    pc_controls::CancelCapture();
    g_open = false;
    for (auto& release : g_release) release = true;
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    RequestCursorUpdate();
}

void Open() {
    pc_controls::ClearKeys();
    pc_controls::CancelCapture();
    g_draft = Current();
    g_status = g_save_failed ? "Some applied settings have not been saved." : "";
    RefreshModes();
    g_focus = true;
    g_open = true;
    g_nav_ready = g_back_down = false;
    if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().ClearEventsQueue();
        ImGui::GetIO().ClearInputKeys();
    }
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    RequestCursorUpdate();
    input::StopRumble();
}

void Apply() {
    if (g_draft.mode == app::DisplayMode::Borderless) g_draft.width = g_draft.height = 0;
    RefreshModes();
    if (g_draft.mode == app::DisplayMode::Fullscreen && g_draft.width &&
        std::none_of(g_modes.begin(), g_modes.end(), [](const Resolution& m) {
            return m.supported && m.width == g_draft.width && m.height == g_draft.height;
        })) {
        g_status = "Resolution unavailable on this monitor. Choose another or use Windowed mode.";
        return;
    }
    g_before = Current();
    bool display = g_before.mode != g_draft.mode || g_before.width != g_draft.width ||
                   g_before.height != g_draft.height || g_before.monitor != g_draft.monitor;
    Set(g_draft);
    if (display) {
        g_confirm = true;
        g_deadline = GetTickCount64() + kConfirmMs;
        // The timer was created on the window thread; Apply runs on the render thread.
        if (!g_timer_ready) {
            Revert("Could not start display confirmation. Changes reverted.");
            return;
        }
        g_status.clear();
        app::ApplyDisplay();
        LOGR("settings: display preview started (15 seconds)");
    } else Save();
}

std::string SizeLabel(uint32_t w, uint32_t h) {
    return std::to_string(w) + " x " + std::to_string(h);
}

void ResolutionControl() {
    const auto desktop = app::Desktop(g_draft.monitor);
    std::string automatic = "Automatic (" + SizeLabel(desktop.width, desktop.height) + ")";
    bool borderless = g_draft.mode == app::DisplayMode::Borderless;
    std::string preview = !borderless && g_draft.width ? SizeLabel(g_draft.width, g_draft.height) : automatic;
    ImGui::BeginDisabled(borderless);
    if (ImGui::BeginCombo("Resolution", preview.c_str())) {
        bool selected = g_draft.width == 0;
        if (ImGui::Selectable(automatic.c_str(), selected)) g_draft.width = g_draft.height = 0;
        if (selected) ImGui::SetItemDefaultFocus();
        for (const auto& m : g_modes) {
            bool unavailable = !m.supported && g_draft.mode == app::DisplayMode::Fullscreen;
            ImGui::BeginDisabled(unavailable);
            selected = m.width == g_draft.width && m.height == g_draft.height;
            std::string label = SizeLabel(m.width, m.height);
            if (!m.supported) label += " (janela)";
            if (ImGui::Selectable(label.c_str(), selected)) {
                g_draft.width = m.width;
                g_draft.height = m.height;
            }
            if (selected) ImGui::SetItemDefaultFocus();
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (borderless) ImGui::TextWrapped("Borderless uses the desktop resolution. Adjust render scale below.");
}

struct OutputInfo {
    HMONITOR monitor = nullptr;
    uint32_t width = 0, height = 0, hz = 0;
};

OutputInfo ActiveOutput() {
    static OutputInfo cached;
    static ULONGLONG next_query = 0;
    auto now = GetTickCount64();
    auto monitor = MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTOPRIMARY);
    if (monitor == cached.monitor && now < next_query) return cached;
    cached = {monitor};
    next_query = now + 1000;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(monitor, &info) && EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        cached.width = dm.dmPelsWidth;
        cached.height = dm.dmPelsHeight;
        cached.hz = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
    }
    return cached;
}

// Two bounded samples after startup/each applied setting or output change.
// These are actual renderer FPS estimates, not the requested cap or a bottleneck diagnosis.
void ReportDisplay(double fps, const OutputInfo& output) {
    static bool initialized = false;
    static Video previous;
    static OutputInfo previous_output;
    static ULONGLONG changed_at = 0;
    static unsigned reports = 0;
    const auto current = Current();
    const auto now = GetTickCount64();
    if (!initialized || !(current == previous) || output.monitor != previous_output.monitor ||
        output.width != previous_output.width || output.height != previous_output.height ||
        output.hz != previous_output.hz) {
        initialized = true;
        previous = current;
        previous_output = output;
        changed_at = now;
        reports = 0;
    }
    if (reports < 2 && now - changed_at >= (reports == 0 ? 2000 : 5000) && fps > 0 && std::isfinite(fps)) {
        const uint32_t msaa_state = gpu::g_msaa_state.load();
        LOGR("display: cap=%u auto=%u effective_cap=%u vsync=%u measured_fps=%.1f output=%ux%u refresh_hz=%u scale=%u brightness=%u contrast=%u smaa=%u smaa_status=%u msaa_requested=%u msaa_effective=%u msaa_fallback=%u ao=%u ao_status=%u ao_render_mode=%u ao_strength=%u ao_radius=%u ao_debug=%u shadow_requested=%u shadow_effective=%u shadow_fallback=%u shadow_softness=%u shadow_softness_status=%u",
             current.cap, unsigned(current.auto_fps), app::EffectiveFpsCap(), unsigned(current.vsync), fps,
             output.width, output.height, output.hz, current.scale, current.brightness, current.contrast,
             unsigned(current.smaa), unsigned(gpu::g_smaa_status.load()), current.msaa,
             (msaa_state >> 8) & 0xFF, (msaa_state >> 16) & 1, current.ao, unsigned(gpu::g_ao_status.load()), gpu::g_ao_mode.load(),
             current.ao_strength, current.ao_radius, current.ao_debug, current.shadow_resolution,
             uint32_t((gpu::g_shadow_resolution_state.load() >> 16) & 0xFFFF),
             uint32_t((gpu::g_shadow_resolution_state.load() >> 32) & 1), current.shadow_softness,
             (gpu::g_shadow_softness_state.load() >> 8) & 0xFF);
        ++reports;
    }
}

std::string MonitorLabel(const app::Display& display) {
    std::string label = "Monitor " + std::to_string(display.number) + " - " +
        SizeLabel(display.desktop.width, display.desktop.height);
    if (display.refresh_hz) label += " / " + std::to_string(display.refresh_hz) + " Hz";
    if (display.primary) label += " (primary)";
    return label;
}

void MonitorControl() {
    std::string preview = "Primary (automatic)";
    if (!g_draft.monitor.empty()) {
        preview = "Unavailable (using primary)";
        for (const auto& display : g_displays)
            if (display.device == g_draft.monitor) preview = MonitorLabel(display);
    }
    auto choose = [](const std::string& device) {
        if (g_draft.monitor == device) return;
        g_draft.monitor = device;
        g_draft.width = g_draft.height = 0;
        RefreshModes();
    };
    if (ImGui::BeginCombo("Monitor", preview.c_str())) {
        if (ImGui::Selectable("Primary (automatic)", g_draft.monitor.empty())) choose("");
        // RefreshModes replaces g_displays; defer selection until iteration ends.
        std::string selected;
        bool picked = false;
        for (const auto& display : g_displays) {
            bool active = display.device == g_draft.monitor;
            const auto label = MonitorLabel(display);
            if (ImGui::Selectable(label.c_str(), active)) { selected = display.device; picked = true; }
            if (active) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
        if (picked) choose(selected);
    }
}

void Controls(const OutputInfo& output_info) {
    MonitorControl();
    const char* modes[] = {"Fullscreen", "Borderless", "Windowed"};
    int mode = int(g_draft.mode);
    if (ImGui::Combo("Screen mode", &mode, modes, 3)) g_draft.mode = app::DisplayMode(mode);
    ResolutionControl();
    int scale = int(g_draft.scale);
    if (ImGui::SliderInt("Render scale", &scale, 20, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp))
        g_draft.scale = uint32_t(std::clamp(scale, 20, 200));
    const auto output = g_draft.width && g_draft.mode != app::DisplayMode::Borderless
        ? app::Mode{g_draft.width, g_draft.height} : app::Desktop(g_draft.monitor);
    ImGui::TextWrapped("Scale relative to output height (%u px). Above 100%% increases GPU load.", output.height);
    int brightness = int(g_draft.brightness), contrast = int(g_draft.contrast);
    if (ImGui::SliderInt("Brightness", &brightness, 0, 100, "%d", ImGuiSliderFlags_AlwaysClamp))
        g_draft.brightness = uint32_t(std::clamp(brightness, 0, 100));
    if (ImGui::SliderInt("Contrast", &contrast, 0, 100, "%d", ImGuiSliderFlags_AlwaysClamp))
        g_draft.contrast = uint32_t(std::clamp(contrast, 0, 100));
    ImGui::TextWrapped("Brightness and contrast: 50 / 50 preserves the original image. Apply to see changes.");
    ImGui::Separator();
    const uint32_t caps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, 0};
    std::string cap_label = g_draft.cap ? std::to_string(g_draft.cap) + " FPS" : "Unlimited";
    if (g_draft.auto_fps) cap_label = "Automatic (display refresh rate)";
    if (ImGui::BeginCombo("Frame rate limit", cap_label.c_str())) {
        if (ImGui::Selectable("Automatic (display refresh rate)", g_draft.auto_fps)) g_draft.auto_fps = true;
        for (auto cap : caps) {
            std::string label = cap ? std::to_string(cap) + " FPS" : "Unlimited";
            bool selected = !g_draft.auto_fps && g_draft.cap == cap;
            if (ImGui::Selectable(label.c_str(), selected)) { g_draft.cap = cap; g_draft.auto_fps = false; }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    int cap = int(g_draft.cap);
    ImGui::BeginDisabled(g_draft.auto_fps);
    if (ImGui::InputInt("Custom FPS", &cap, 1, 10)) g_draft.cap = uint32_t(std::clamp(cap, 0, 1000));
    ImGui::EndDisabled();
    if (g_draft.auto_fps) ImGui::TextWrapped("Automatic follows the current display. Your custom limit is retained.");
    ImGui::TextWrapped("0 = unlimited. A limit does not guarantee that frame rate. Recommended default: 60 FPS.");
    ImGui::Checkbox("V-Sync", &g_draft.vsync);
    if (output_info.hz) {
        ImGui::TextWrapped("Active display: %u x %u, approximately %u Hz. V-Sync may cap FPS at this refresh rate.",
                           output_info.width, output_info.height, output_info.hz);
    } else ImGui::TextWrapped("V-Sync may cap FPS at the monitor refresh rate.");
    ImGui::Separator();
    const char* names[] = {"Game default", "1x", "2x", "4x", "8x", "16x"};
    const uint32_t values[] = {0, 1, 2, 4, 8, 16};
    int aniso = 0;
    for (int i = 0; i < 6; ++i) if (values[i] == g_draft.aniso) aniso = i;
    if (ImGui::Combo("Anisotropic filtering", &aniso, names, 6)) g_draft.aniso = values[aniso];
    constexpr uint32_t msaa_values[] = {0, 2, 4, 8};
    const char* msaa_names[] = {"Off", "2x", "4x", "8x"};
    int msaa = 0;
    for (int i = 0; i < 4; ++i) if (msaa_values[i] == g_draft.msaa) msaa = i;
    if (ImGui::Combo("MSAA (geometry)", &msaa, msaa_names, 4)) g_draft.msaa = msaa_values[msaa];
    ImGui::TextWrapped("%s", "Can be combined with SMAA. Start with 2x and 100% scale; higher values use more GPU memory.");
    const uint32_t msaa_state = gpu::g_msaa_state.load();
    const uint32_t requested = msaa_state & 0xFF, effective = (msaa_state >> 8) & 0xFF;
    if (requested != Current().msaa) ImGui::TextUnformatted("MSAA: waiting for the renderer.");
    else if (requested) {
        if (effective > 1) ImGui::Text("MSAA requested: %ux; effective: %ux.", requested, effective);
        else ImGui::TextUnformatted("MSAA unavailable; using single-sample rendering. See the log.");
        if (msaa_state & (1u << 16)) ImGui::TextWrapped("MSAA was reduced or disabled. See the log for details.");
    }
    ImGui::Checkbox("SMAA (anti-aliasing)", &g_draft.smaa);
    ImGui::TextWrapped("Smooths the game image, including HUD and text. May increase GPU load.");
    if (Current().smaa) {
        switch (gpu::g_smaa_status.load()) {
            case gpu::SmaaStatus::Active: ImGui::TextUnformatted("SMAA active."); break;
            case gpu::SmaaStatus::Unavailable:
                ImGui::TextWrapped("SMAA unavailable this session; using the original image. See the log. To retry: disable, apply, enable and apply.");
                break;
            default: ImGui::TextUnformatted("SMAA waiting for the next game image."); break;
        }
    } else if (g_draft.smaa) ImGui::TextUnformatted("Apply to enable SMAA.");
    ImGui::Separator();
    pc_render::Draw(g_draft.rendering);
    ImGui::Separator();
    const uint32_t shadow_sizes[] = {0, 256, 512, 1024, 2048};
    const char* shadow_names[] = {"Follow render scale (default)", "256 x 256 (Xbox base)", "512 x 512", "1024 x 1024", "2048 x 2048"};
    int shadow = 0;
    for (int i = 0; i < 5; ++i) if (g_draft.shadow_resolution == shadow_sizes[i]) shadow = i;
    if (ImGui::Combo("Shadow resolution", &shadow, shadow_names, 5))
        g_draft.shadow_resolution = shadow_sizes[shadow];
    ImGui::TextWrapped("Adjusts character shadow mask resolution. Shadow distance is unchanged.");
    ImGui::TextWrapped("Follow render scale preserves current quality. A low fixed size may reduce definition at high render scales.");
    const char* softness_names[] = {"Original", "Light", "Medium"};
    int softness = int(g_draft.shadow_softness);
    if (ImGui::Combo("Shadow softness", &softness, softness_names, 3))
        g_draft.shadow_softness = uint32_t(softness);
    ImGui::TextWrapped("Softens shadow mask edges independently of resolution. Original preserves the previous appearance.");
    ImGui::TextWrapped("Light and Medium use more GPU time, especially with 2048 maps. Compare in the same scene.");
    const uint32_t softness_state = gpu::g_shadow_softness_state.load();
    if ((softness_state & 0xFF) != Current().shadow_softness)
        ImGui::TextUnformatted("Softness: waiting for the renderer.");
    else if (Current().shadow_softness) {
        switch (gpu::ShadowSoftnessStatus((softness_state >> 8) & 0xFF)) {
            case gpu::ShadowSoftnessStatus::Active:
                ImGui::TextUnformatted("Shadow filter ready for the current mask."); break;
            case gpu::ShadowSoftnessStatus::Unavailable:
                ImGui::TextWrapped("Softness unavailable; using the original mask. See the log. Apply Original, then Light/Medium to retry."); break;
            default:
                ImGui::TextUnformatted("Softness waiting for a projected shadow."); break;
        }
    }
    const uint64_t shadow_state = gpu::g_shadow_resolution_state.load();
    const uint32_t shadow_requested = uint32_t(shadow_state & 0xFFFF);
    const uint32_t shadow_effective = uint32_t((shadow_state >> 16) & 0xFFFF);
    if (shadow_requested != Current().shadow_resolution)
        ImGui::TextUnformatted("Shadows: waiting for the renderer.");
    else if (shadow_effective)
        ImGui::Text("Effective shadow map: %u x %u.", shadow_effective, shadow_effective);
    else ImGui::TextWrapped("Shadows: waiting for a compatible scene.");
    if (shadow_state & (1ull << 32))
        ImGui::TextWrapped("Could not allocate the selected size; following render scale. See the log. Change the option and apply to retry.");
    ImGui::Separator();
    const char* ao_names[] = {"Off", "SSAO (experimental)", "GTAO (experimental)"};
    int ao = int(g_draft.ao);
    if (ImGui::Combo("Ambient occlusion", &ao, ao_names, 3)) g_draft.ao = uint32_t(ao);
    if (g_draft.ao) {
        int strength = int(g_draft.ao_strength), radius = int(g_draft.ao_radius), debug = int(g_draft.ao_debug);
        if (ImGui::SliderInt("Occlusion strength", &strength, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp))
            g_draft.ao_strength = uint32_t(std::clamp(strength, 0, 100));
        if (ImGui::SliderInt("Occlusion radius", &radius, 25, 400, "%d%%", ImGuiSliderFlags_AlwaysClamp))
            g_draft.ao_radius = uint32_t(std::clamp(radius, 25, 400));
        const char* diagnostics[] = {"Normal image", "Depth", "Normals", "Occlusion only"};
        if (ImGui::Combo("Occlusion view", &debug, diagnostics, 4)) g_draft.ao_debug = uint32_t(debug);
        ImGui::TextWrapped("Darkens contacts and corners. Start with strength 50 and radius 100. Compatible with MSAA and SMAA.");
        if (g_draft.ao == 2) ImGui::TextWrapped("GTAO integrates horizons around surfaces. Compare with SSAO in the same scene; appearance and cost may vary.");
        ImGui::TextWrapped("Diagnostic views show the scene before shadows and later effects. Use Normal image for gameplay.");
    }
    const uint32_t applied_ao = Current().ao;
    if (applied_ao) {
        const char* method = gpu::AoModeName(applied_ao);
        if (gpu::g_ao_mode.load() != applied_ao) {
            ImGui::Text("%s: waiting for the renderer.", method);
        } else switch (gpu::g_ao_status.load()) {
            case gpu::AoStatus::Active: ImGui::Text("%s: pass submitted in the scene.", method); break;
            case gpu::AoStatus::Unavailable:
                ImGui::TextWrapped("%s unavailable; using the original image. See the log. Switch methods or disable/apply/enable/apply to retry.", method);
                break;
            default: ImGui::TextWrapped("%s waiting for a compatible 3D scene. The effect is disabled in 2D menus.", method); break;
        }
    } else if (g_draft.ao) ImGui::TextUnformatted("Apply to enable ambient occlusion.");
    ImGui::Checkbox("Show FPS counter", &g_draft.counter);
}

// The APT menu edits the same draft and uses the same Apply/Save/Revert paths
// as F10. No guest calls, rendering calls or asset writes belong in this model.
struct NativeRow {
    std::string name, value;
    std::function<void(int)> change;
    std::function<void()> enter;
};
void Go(Page page) {
    if (page == Page::Development || page == Page::Missions || page == Page::TestLevels)
        g_status.clear();
    g_history.emplace_back(g_page, g_selected);
    g_page = page;
    g_selected = 0;
    g_editing = false;
    g_waiting_key = false;
    LOGR("pc-menu: page=%u row=0", unsigned(g_page));
}
void Parent() {
    if (g_history.empty()) { g_page = Page::Home; g_selected = 0; }
    else {
        const auto previous = g_history.back();
        g_history.pop_back();
        g_page = previous.first; g_selected = previous.second;
    }
    g_editing = g_waiting_key = false;
    LOGR("pc-menu: back to page=%u row=%d", unsigned(g_page), g_selected);
}
void EndNative() {
    Revert("Display change cancelled.");
    pc_cheats::CancelConfirmation();
    pc_controls::CancelCapture();
    pc_controls::ClearKeys();
    g_draft = Current();
    g_native = false;
    g_development = false;
    g_destination.reset();
    g_editing = g_waiting_key = false;
    g_history.clear();
    for (auto& release : g_release) release = true;
}
void Cycle(uint32_t& value, const std::vector<uint32_t>& values, int direction) {
    const auto found = std::find(values.begin(), values.end(), value);
    int index = found == values.end() ? 0 : int(found - values.begin());
    index = (index + direction + int(values.size())) % int(values.size());
    value = values[size_t(index)];
}
std::vector<NativeRow> NativeRows() {
    std::vector<NativeRow> rows;
    auto action = [&](const char* name, std::function<void()> enter) {
        rows.push_back({name, "", {}, std::move(enter)});
    };
    auto page = [&](const char* name, Page destination) { action(name, [destination] { Go(destination); }); };
    auto toggle = [&](const char* name, bool& value) {
        auto* target = &value;
        rows.push_back({name, value ? "ON" : "OFF", [target](int) { *target = !*target; }, {}});
    };
    auto range = [&](const char* name, uint32_t& value, uint32_t low, uint32_t high, int step = 1) {
        auto* target = &value;
        rows.push_back({name, std::to_string(value), [=](int direction) {
            *target = uint32_t(std::clamp(int(*target) + step * direction, int(low), int(high)));
        }, {}});
    };
    auto choice = [&](const char* name, uint32_t& value, std::vector<uint32_t> values,
                      std::vector<std::string> labels) {
        auto* target = &value;
        const auto found = std::find(values.begin(), values.end(), value);
        std::string shown = found == values.end() ? std::to_string(value) : labels[size_t(found - values.begin())];
        rows.push_back({name, std::move(shown), [target, values = std::move(values)](int direction) {
            Cycle(*target, values, direction);
        }, {}});
    };
    if (g_confirm) {
        action("KEEP AND SAVE", [] { g_confirm = false; Save(); g_selected = 0; });
        action("REVERT", [] { Revert("Previous settings restored."); g_selected = 0; });
        return rows;
    }
    switch (g_page) {
    case Page::Home:
        page("DISPLAY", Page::Display);
        page("GRAPHICS", Page::Graphics);
        page("LIGHTING AND SHADOWS", Page::Lighting);
        page("CONTROLS", Page::Controls);
        page("VIDEOS", Page::Videos);
        page("CHEATS", Page::Cheats);
        if (g_development) page("DEVELOPMENT", Page::Development);
        action("APPLY AND SAVE", [] { Apply(); if (g_confirm) g_selected = 0; });
        page("RESTORE DEFAULTS", Page::Defaults);
        action("BACK", [] { if (g_draft == Current()) EndNative(); else Go(Page::Leave); });
        break;
    case Page::Display: {
        const std::string shown = g_draft.monitor.empty() ? "AUTOMATIC" :
            "DISPLAY " + std::to_string(1 + std::distance(g_displays.begin(),
                std::find_if(g_displays.begin(), g_displays.end(), [](const auto& d) { return d.device == g_draft.monitor; })));
        rows.push_back({"MONITOR", shown, [](int direction) {
            std::vector<std::string> devices{""};
            for (const auto& display : g_displays) devices.push_back(display.device);
            const auto found = std::find(devices.begin(), devices.end(), g_draft.monitor);
            int index = found == devices.end() ? 0 : int(found - devices.begin());
            g_draft.monitor = devices[size_t((index + direction + int(devices.size())) % int(devices.size()))];
            RefreshModes();
        }, {}});
        const char* modes[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
        rows.push_back({"SCREEN MODE", modes[uint32_t(g_draft.mode)], [](int direction) {
            g_draft.mode = app::DisplayMode((int(g_draft.mode) + direction + 3) % 3);
        }, {}});
        rows.push_back({"RESOLUTION", g_draft.mode == app::DisplayMode::Borderless ? "DESKTOP" :
            (g_draft.width ? std::to_string(g_draft.width) + "X" + std::to_string(g_draft.height) : "AUTOMATIC"),
            [](int direction) {
                if (g_draft.mode == app::DisplayMode::Borderless) {
                    g_status = "Borderless uses desktop resolution."; return;
                }
                std::vector<Resolution> modes{{0, 0, true}};
                for (const auto& mode : g_modes)
                    if (mode.supported || g_draft.mode == app::DisplayMode::Windowed) modes.push_back(mode);
                auto found = std::find_if(modes.begin(), modes.end(), [](const auto& mode) {
                    return mode.width == g_draft.width && mode.height == g_draft.height;
                });
                int index = found == modes.end() ? 0 : int(found - modes.begin());
                const auto mode = modes[size_t((index + direction + int(modes.size())) % int(modes.size()))];
                g_draft.width = mode.width; g_draft.height = mode.height;
            }, {}});
        range("RENDER SCALE %", g_draft.scale, 20, 200, 5);
        toggle("AUTO FPS", g_draft.auto_fps);
        choice("FPS LIMIT", g_draft.cap, {30, 60, 75, 90, 120, 144, 165, 240, 360, 0},
               {"30", "60", "75", "90", "120", "144", "165", "240", "360", "UNLIMITED"});
        auto preset = rows.back().change;
        rows.back().change = [preset](int direction) { preset(direction); g_draft.auto_fps = false; };
        range("CUSTOM FPS", g_draft.cap, 0, 1000);
        auto custom = rows.back().change;
        rows.back().change = [custom](int direction) { custom(direction); g_draft.auto_fps = false; };
        toggle("V-SYNC", g_draft.vsync);
        toggle("FPS COUNTER", g_draft.counter);
        toggle("PAUSE ON FOCUS LOSS", g_draft.pause_on_focus_loss);
        break;
    }
    case Page::Graphics:
        choice("ANISOTROPIC", g_draft.aniso, {0, 1, 2, 4, 8, 16}, {"GAME", "1X", "2X", "4X", "8X", "16X"});
        choice("MSAA", g_draft.msaa, {0, 2, 4, 8}, {"OFF", "2X", "4X", "8X"});
        toggle("SMAA", g_draft.smaa);
        choice("OBJECT DISTANCE", g_draft.rendering.object_distance, {100, 150, 200, 300}, {"1X", "1.5X", "2X", "3X"});
        range("BRIGHTNESS", g_draft.brightness, 0, 100);
        range("CONTRAST", g_draft.contrast, 0, 100);
        break;
    case Page::Lighting:
        choice("SHADOW SIZE", g_draft.shadow_resolution, {0, 256, 512, 1024, 2048}, {"AUTO", "256", "512", "1024", "2048"});
        choice("SHADOW SOFTNESS", g_draft.shadow_softness, {0, 1, 2}, {"ORIGINAL", "LIGHT", "MEDIUM"});
        choice("OCCLUSION", g_draft.ao, {0, 1, 2}, {"OFF", "SSAO", "GTAO"});
        range("AO STRENGTH", g_draft.ao_strength, 0, 100);
        range("AO RADIUS %", g_draft.ao_radius, 25, 400, 5);
        choice("AO VIEW", g_draft.ao_debug, {0, 1, 2, 3}, {"NORMAL", "DEPTH", "NORMALS", "AO ONLY"});
        // Existing bloom parameters only: no change to its effect or defaults.
        toggle("BLOOM", g_draft.rendering.bloom);
        range("BLOOM STRENGTH", g_draft.rendering.bloom_strength, 0, 100);
        range("BLOOM THRESHOLD", g_draft.rendering.bloom_threshold, 0, 95);
        range("BLOOM RADIUS %", g_draft.rendering.bloom_radius, 25, 300, 5);
        break;
    case Page::Controls:
        choice("KEYBOARD PLAYER", g_draft.input.keyboard_player, {0, 1, 2}, {"OFF", "PLAYER 1", "PLAYER 2"});
        for (size_t player = 0; player < 2; ++player) {
            const auto slot = g_draft.input.controller[player];
            rows.push_back({player ? "PLAYER 2 DEVICE" : "PLAYER 1 DEVICE", slot < 4 ? "PAD " + std::to_string(slot + 1) : "NONE",
                [player](int direction) {
                    auto& slots = g_draft.input.controller;
                    const auto old = slots[player];
                    const auto next = uint32_t((int(old) + direction + 5) % 5);
                    auto proposed = slots;
                    if (next < 4 && proposed[1 - player] == next) proposed[1 - player] = old;
                    proposed[player] = next;
                    if (proposed[0] == 4 && g_draft.input.keyboard_player != 1) return;
                    slots = proposed;
                }, {}});
        }
        page("KEYBOARD BINDINGS", Page::Keyboard);
        page("PLAYER 1 BUTTONS", Page::Pad1);
        page("PLAYER 2 BUTTONS", Page::Pad2);
        action("DEFAULT CONTROLS", [] { g_draft.input = pc_controls::Settings{}; });
        break;
    case Page::Keyboard:
        for (size_t i = 0; i < pc_controls::kActions; ++i) {
            auto label = pc_controls::KeyLabel(g_draft.input.keys[i]);
            if (std::any_of(label.begin(), label.end(), [](unsigned char c) { return c >= 128; }))
                label = "VK " + std::to_string(g_draft.input.keys[i]);
            rows.push_back({pc_controls::ActionLabel(i), std::move(label),
                [i](int) { g_draft.input.keys[i] = 0; }, [i] {
                    g_edit_before = g_draft; g_editing = g_waiting_key = true; pc_controls::BeginCapture(i);
                }});
        }
        break;
    case Page::Pad1:
    case Page::Pad2: {
        const size_t player = g_page == Page::Pad1 ? 0 : 1;
        for (size_t i = 0; i < pc_controls::kButtons; ++i) {
            const auto value = g_draft.input.buttons[player][i];
            rows.push_back({pc_controls::ActionLabel(i), value < pc_controls::kButtons ? pc_controls::ActionLabel(value) : "UNBOUND", [player, i](int direction) {
                auto& value = g_draft.input.buttons[player][i];
                value = uint32_t((int(value) + direction + 17) % 17);
            }, {}});
        }
        break;
    }
    case Page::Videos:
        choice("VIDEO FRAMING", g_draft.movies.scale, {0, 1, 2, 3}, {"ORIGINAL", "FIT ASPECT", "FIT 4:3", "FILL / CROP"});
        toggle("SKIP INTRO LOGOS", g_draft.movies.skip_logos);
        action("DEFAULT VIDEOS", [] { g_draft.movies = pc_video::Settings{}; });
        break;
    case Page::Cheats:
        page("UNLOCK CONTENT", Page::Unlocks);
        page("PLAYER 1", Page::CheatP1);
        page("PLAYER 2", Page::CheatP2);
        toggle("LOG PLAYER DIAGNOSTICS", g_draft.cheats.diagnostics);
        action("APPLY GAMEPLAY CHEATS", [] { pc_cheats::Set(g_draft.cheats); g_status = "SESSION CHEATS APPLIED"; });
        action("DISABLE GAMEPLAY CHEATS NOW", [] {
            g_draft.cheats = pc_cheats::Settings{}; pc_cheats::Set(g_draft.cheats);
            g_status = "GAMEPLAY CHEATS OFF";
        });
        break;
    case Page::Unlocks: {
        const char* labels[] = {"ALL CONTENT", "ALL CHARACTERS", "ALL ARENAS", "ALL CARDS", "ALL COMICS"};
        for (uint32_t i = 1; i <= 5; ++i) {
            const auto value = pc_cheats::Unlock(i);
            rows.push_back({labels[i - 1], "", {}, [value] { g_unlock_choice = value; Go(Page::ConfirmUnlock); }});
        }
        break;
    }
    case Page::CheatP1:
    case Page::CheatP2: {
        const int player = g_page == Page::CheatP1 ? 0 : 1;
        toggle("INFINITE HEALTH", g_draft.cheats.health[player]);
        toggle("INFINITE ENERGY", g_draft.cheats.energy[player]);
        action("REFILL HEALTH AND ENERGY NOW", [player] { pc_cheats::RequestRefill(player); g_status.clear(); });
        action("APPLY GAMEPLAY CHEATS", [] { pc_cheats::Set(g_draft.cheats); g_status = "SESSION CHEATS APPLIED"; });
        break;
    }
    case Page::ConfirmUnlock:
        action("CANCEL", [] { Parent(); });
        action(pc_cheats::UnlockLabel(g_unlock_choice), [] {
            pc_cheats::RequestUnlock(g_unlock_choice); Parent(); g_status.clear();
        });
        break;
    case Page::Development:
        page("CAMPAIGN MISSION SELECT", Page::Missions);
        page("TEST LEVELS (EXPERIMENTAL)", Page::TestLevels);
        action("BACK", [] { Parent(); });
        break;
    case Page::Missions:
    case Page::TestLevels: {
        const auto catalog = pc_development::Read();
        const auto& destinations = g_page == Page::Missions ? catalog.missions : catalog.tests;
        if (destinations.empty()) action("NO DESTINATIONS AVAILABLE", [] {});
        for (const auto& destination : destinations) {
            rows.push_back({destination.name, destination.available ? "" : "UNAVAILABLE", {}, [destination] {
                if (!destination.available) {
                    g_status = destination.unavailable_reason.empty() ? "REQUIRED LEVEL / SCENE FILE MISSING" :
                        destination.unavailable_reason;
                    return;
                }
                g_destination = destination;
                g_status.clear();
                Go(Page::ConfirmLevel);
            }});
        }
        action("BACK", [] { Parent(); });
        break;
    }
    case Page::ConfirmLevel:
        action("CANCEL", [] { g_destination.reset(); Parent(); });
        if (g_destination) {
            rows.push_back({"LOAD " + g_destination->name, "", {}, [] {
                if (!g_development || !g_destination) return;
                if (!(g_draft == Current())) {
                    g_status = "APPLY OR DISCARD SETTINGS BEFORE LOADING";
                    return;
                }
                if (pc_development::Request(*g_destination, g_native_controller)) EndNative();
                else g_status = pc_development::Status();
            }});
        }
        break;
    case Page::Leave:
        action("CONTINUE EDITING", [] { Parent(); });
        action("APPLY AND SAVE", [] { Apply(); if (g_confirm) g_selected = 0; });
        action("DISCARD AND BACK", [] { EndNative(); });
        break;
    case Page::Defaults:
        action("CANCEL", [] { Parent(); });
        action("RESTORE DEFAULTS", [] { g_draft = Video{}; Parent(); RefreshModes(); });
        break;
    }
    return rows;
}

void Counter(double fps) {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12, viewport->WorkPos.y + 12), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    if (ImGui::Begin("##MarvelFPS", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing)) {
        if (fps > 0 && std::isfinite(fps)) ImGui::Text("%.1f FPS", fps);
        else ImGui::TextUnformatted("-- FPS");
    }
    ImGui::End();
}

}  // namespace

bool& FpsCounter() {
    static bool enabled = false;
    return enabled;
}

bool NativeBegin(bool development) {
    std::lock_guard lock(app::UiLock());
    if (g_open || g_open_pending || g_native) return false;
    g_draft = Current();
    g_status.clear();
    g_development = development;
    g_native_controller = 0;
    g_destination.reset();
    g_page = Page::Home; g_selected = 0; g_editing = g_waiting_key = false; g_history.clear();
    RefreshModes();
    pc_controls::ClearKeys();
    pc_controls::CancelCapture();
    input::StopRumble();
    g_native = true;
    g_nav_ready = false;
    g_native_held = 0;
    g_native_repeat.fill(0);
    for (auto& release : g_release) release = true;
    LOGR("pc-menu: native settings session started");
    return true;
}

void NativeEnd() {
    std::lock_guard lock(app::UiLock());
    if (g_native) EndNative();
}

void NativeAction(MenuAction action) {
    std::lock_guard lock(app::UiLock());
    if (!g_native) return;
    if (g_confirm && GetTickCount64() >= g_deadline) Revert("Time expired. Previous settings restored.");
    if (pc_controls::TakeCapture(g_draft.input)) g_editing = g_waiting_key = false;
    // Transitions require a fresh press. In particular the A that opens a page
    // must not also edit its first row, and held B must not unwind every level.
    if (action == MenuAction::Select || action == MenuAction::Back) {
        g_nav_ready = false;
        for (auto& release : g_release) release = true;
    }
    if (pc_controls::Capturing()) {
        if (action == MenuAction::Back) { pc_controls::CancelCapture(); g_editing = g_waiting_key = false; }
        return;
    }
    if (action == MenuAction::Back) {
        if (g_confirm) { Revert("Previous settings restored."); g_selected = 0; }
        else if (g_editing) { g_draft = g_edit_before; g_editing = g_waiting_key = false; RefreshModes(); }
        else if (g_page == Page::Home) {
            if (g_draft == Current()) EndNative(); else Go(Page::Leave);
        } else Parent();
        return;
    }
    auto rows = NativeRows();
    if (rows.empty()) return;
    g_selected = std::clamp(g_selected, 0, int(rows.size()) - 1);
    auto& row = rows[size_t(g_selected)];
    if (action == MenuAction::Up || action == MenuAction::Down) {
        if (!g_editing) g_selected = (g_selected + (action == MenuAction::Up ? -1 : 1) + int(rows.size())) % int(rows.size());
    } else if (action == MenuAction::Select) {
        if (g_editing) { g_editing = false; g_status.clear(); }
        else if (row.enter) row.enter();
        else if (row.change) { g_edit_before = g_draft; g_editing = true; g_status.clear(); }
    } else if (row.change) {
        if (!g_editing) { g_edit_before = g_draft; g_editing = true; }
        g_status.clear();
        row.change(action == MenuAction::Left ? -1 : 1);
        // Keep a route into P1 even if its pad is disabled in the draft.
        if (g_draft.input.controller[0] == 4 && g_draft.input.keyboard_player != 1)
            g_draft.input.keyboard_player = 1;
    }
}

MenuView NativeView() {
    std::lock_guard lock(app::UiLock());
    MenuView view;
    if (!g_native) return view;
    if (g_confirm && GetTickCount64() >= g_deadline) Revert("Time expired. Previous settings restored.");
    if (pc_controls::TakeCapture(g_draft.input)) {
        g_editing = g_waiting_key = false; g_nav_ready = false; pc_controls::ClearKeys();
    }
    if (g_waiting_key && !pc_controls::Capturing()) g_editing = g_waiting_key = false;
    if (GetTickCount64() >= g_next_displays && !g_confirm) RefreshModes();
    view.open = true;
    switch (g_page) {
    case Page::Home: view.title = "PC SETTINGS"; break;
    case Page::Display: view.title = "DISPLAY"; break;
    case Page::Graphics: view.title = "GRAPHICS"; break;
    case Page::Lighting: view.title = "LIGHTING / SHADOWS"; break;
    case Page::Controls: view.title = "CONTROLS"; break;
    case Page::Keyboard: view.title = "KEYBOARD BINDINGS"; break;
    case Page::Pad1: view.title = "PLAYER 1 BUTTONS"; break;
    case Page::Pad2: view.title = "PLAYER 2 BUTTONS"; break;
    case Page::Videos: view.title = "VIDEOS"; break;
    case Page::Leave: view.title = "UNSAVED CHANGES"; break;
    case Page::Defaults: view.title = "RESTORE DEFAULTS?"; break;
    case Page::Cheats: view.title = "CHEATS (EXPERIMENTAL)"; break;
    case Page::Unlocks: view.title = "UNLOCK CONTENT"; break;
    case Page::CheatP1: view.title = "PLAYER 1 CHEATS"; break;
    case Page::CheatP2: view.title = "PLAYER 2 CHEATS"; break;
    case Page::ConfirmUnlock: view.title = "CONFIRM UNLOCK"; break;
    case Page::Development: view.title = "DEVELOPMENT"; break;
    case Page::Missions: view.title = "CAMPAIGN MISSIONS"; break;
    case Page::TestLevels: view.title = "TEST LEVELS"; break;
    case Page::ConfirmLevel: view.title = "LOAD DESTINATION?"; break;
    }
    if (g_confirm) {
        const auto now = GetTickCount64();
        view.title = "KEEP DISPLAY? " + std::to_string(g_deadline > now ? (g_deadline - now + 999) / 1000 : 0) + "S";
        view.hint = "A: SELECT   B: REVERT";
    } else if (pc_controls::Capturing()) view.hint = "PRESS A KEY   B / ESC: CANCEL";
    else if (g_editing) view.hint = "LEFT / RIGHT: CHANGE   A: KEEP   B: CANCEL";
    else view.hint = "UP / DOWN: MOVE   A: SELECT   B: BACK";
    if (!g_status.empty() && !g_editing && !g_confirm) view.hint = g_status;
    if (!g_confirm && !g_editing) {
        if (g_page == Page::ConfirmLevel && g_status.empty())
            view.hint = "TEST SESSION; AUTOSAVE DISABLED";
        else if (g_page == Page::Development && g_status.empty())
            view.hint = pc_development::AutosaveBlocked()
                ? "AUTOSAVE OFF; LOAD A SAVE TO RESUME CAMPAIGN" : pc_development::Status();
        else if (g_page == Page::Home && g_status.empty() && pc_development::AutosaveBlocked())
            view.hint = "DEVELOPMENT SESSION: AUTOSAVE OFF";
        else if ((g_page == Page::Missions || g_page == Page::TestLevels) && g_status.empty()) {
            const auto catalog = pc_development::Read();
            const auto& destinations = g_page == Page::Missions ? catalog.missions : catalog.tests;
            if (g_selected >= 0 && size_t(g_selected) < destinations.size()) {
                const auto& destination = destinations[size_t(g_selected)];
                view.hint = destination.unavailable_reason.empty() ?
                    "LEVEL: " + destination.level + "   A: SELECT   B: BACK" : destination.unavailable_reason;
            }
        }
        else if (g_page == Page::ConfirmUnlock) view.hint = "MAY CHANGE SAVE; CANNOT UNDO HERE";
        else if (g_page == Page::Unlocks) view.hint = pc_cheats::Status();
        else if ((g_page == Page::Cheats || g_page == Page::CheatP1 || g_page == Page::CheatP2) && g_status.empty()) {
            const auto result = pc_cheats::LastResult();
            view.hint = result == pc_cheats::Result::Refilled || result == pc_cheats::Result::NoPlayer || result == pc_cheats::Result::Queued
                ? pc_cheats::Status() : "SESSION ONLY; APPLY GAMEPLAY CHEATS";
        }
    }
    const auto rows = NativeRows();
    for (const auto& row : rows)
        view.rows.push_back(row.name + (row.value.empty() ? "" : ": " + row.value));
    view.selected = std::clamp(g_selected, 0, std::max(0, int(view.rows.size()) - 1));
    if (!g_confirm && !(g_draft == Current())) view.title += " *";
    return view;
}

void Initialize() {
    std::lock_guard lock(app::UiLock());
    auto& s = app::Get();
    s.render_percent = std::clamp(s.render_percent, 20u, 200u);
    s.fps_cap = std::min(s.fps_cap, 1000u);
    s.brightness = std::min(s.brightness, 100u);
    s.contrast = std::min(s.contrast, 100u);
    s.msaa = gpu::NormalizeMsaa(s.msaa);
    s.shadow_resolution = gpu::NormalizeShadowResolution(s.shadow_resolution);
    s.shadow_softness = gpu::NormalizeShadowSoftness(s.shadow_softness);
    s.ambient_occlusion = gpu::NormalizeAoMode(s.ambient_occlusion);
    s.ao_strength = std::min(s.ao_strength, 100u);
    s.ao_radius = std::clamp(s.ao_radius, 25u, 400u);
    s.ao_debug = std::min(s.ao_debug, 3u);
    if (uint32_t(s.display_mode) > 2) s.display_mode = app::DisplayMode::Borderless;
    if (!s.width || !s.height) s.width = s.height = 0;
    if (s.aniso != 0 && s.aniso != 1 && s.aniso != 2 && s.aniso != 4 && s.aniso != 8 && s.aniso != 16) s.aniso = 0;
    if (ImGui::GetCurrentContext()) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
}

void WindowReady(void* hwnd) {
    std::lock_guard lock(app::UiLock());
    pc_controls::WindowReady(hwnd);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // A timer associated with a window must be created by its owning thread.
    // Keep it alive across previews so render-thread buttons never create/kill it.
    g_timer_ready = SetTimer(static_cast<HWND>(hwnd), kConfirmTimer, 250, nullptr) != 0;
    UpdateCursor(static_cast<HWND>(hwnd));
}

void RequestOpen() {
    g_open_pending = true;
    if (!PostMessageW(static_cast<HWND>(app::Window()), kOpenFromMenu, 0, 0)) g_open_pending = false;
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    std::lock_guard lock(app::UiLock());
    if (msg == kOpenFromMenu) {
        if (!g_open.load() && !g_native) Open();
        g_open_pending = false;
        *result = 0;
        return true;
    }
    if (msg == kUpdateCursor) {
        UpdateCursor(static_cast<HWND>(hwnd));
        *result = 0;
        return true;
    }
    if (msg == WM_DISPLAYCHANGE) g_next_displays = 0;
    if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) UpdateCursor(static_cast<HWND>(hwnd));
    if (msg == WM_TIMER && wp == kConfirmTimer) {
        const auto now = GetTickCount64();
        if (now >= g_next_refresh) {
            g_next_refresh = now + 1000;
            app::RefreshDisplayInfo(true);
        }
        // Also clear a wait cursor left by the splash without needing mouse movement.
        // With the panel open, leave ImGui's arrow/text cursor selection alone.
        if (!g_open.load()) UpdateCursor(static_cast<HWND>(hwnd));
        if (g_confirm && GetTickCount64() >= g_deadline) Revert("Time expired. Previous settings restored.");
        *result = 0;
        return true;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F10) {
        if (!(lp & (intptr_t(1) << 30))) {
            if (g_open) Close(); else if (!g_native) Open();
        }
        *result = 0;
        return true;
    }
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wp == VK_F10) {
        *result = 0;
        return true;
    }
    if (pc_controls::Message(msg, wp, lp, g_open.load() || (g_native && pc_controls::Capturing()))) {
        *result = 0;
        return true;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && g_native) {
        if (!(lp & (intptr_t(1) << 30))) NativeAction(MenuAction::Back);
        *result = 0;
        return true;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && g_open) {
        Close();
        *result = 0;
        return true;
    }
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_KEYMENU) {
        *result = 0;
        return true;
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(g_open.load() ? LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) : nullptr);
        *result = TRUE;
        return true;
    }
    if (msg == WM_DESTROY) KillTimer(static_cast<HWND>(hwnd), kConfirmTimer);
    return false;
}

void FilterPad(int port, input::Pad& pad) {
    pc_controls::ApplyPad(port, pad);
    if (g_ui_poll) *g_ui_poll = pad;
    bool releasing = false;
    if (port >= 0 && port < int(g_release.size())) {
        releasing = g_release[size_t(port)].load();
        if (releasing && Neutral(pad)) g_release[size_t(port)] = false;
    }
    // PrepareInput polls the same logical ports on the UI thread. Preserve those
    // snapshots locally there; guest polls never receive panel navigation.
    if (g_open.load() || g_open_pending.load() || g_native.load() || releasing || app::FocusPaused())
        pad = input::Pad{.connected = pad.connected};
}

void FilterRumble(int, uint16_t& left, uint16_t& right) {
    if (g_open.load() || g_open_pending.load() || g_native.load() || app::FocusPaused()) left = right = 0;
}

void PrepareInput() {
    // Called under UiLock after the Win32 backend and before NewFrame. Remove
    // only its XInput-slot-0 events; keyboard/mouse/focus events remain intact.
    // The pinned ImGui version exposes this queue in imgui_internal.h.
    auto& queue = ImGui::GetCurrentContext()->InputEventsQueue;
    for (int i = queue.Size - 1; i >= 0; --i)
        if (queue[i].Source == ImGuiInputSource_Gamepad) queue.erase(queue.Data + i);
    auto& io = ImGui::GetIO();
    input::Pad pads[2]{};
    const bool active = (g_open.load() || g_native.load()) && !app::FocusPaused() &&
        GetForegroundWindow() == static_cast<HWND>(app::Window());
    if (active) {
        for (int port = 0; port < 2; ++port) {
            g_ui_poll = &pads[port];
            input::Poll(port);  // FilterPad records mapped input before suppressing it for the guest.
            g_ui_poll = nullptr;
        }
        if (!g_nav_ready && Neutral(pads[0]) && Neutral(pads[1])) g_nav_ready = true;
    } else g_nav_ready = false;
    // APT supplies the native visuals; its Options page does not define every
    // direction callback. Read the same mapped SDL/keyboard ports as the game,
    // before FilterPad suppresses them for the guest, and edit the host model.
    // No guest call is made under UiLock. The APT update draws the next snapshot.
    if (g_native.load()) {
        uint32_t held = 0;
        if (active && g_nav_ready) {
            const auto buttons = pads[0].buttons | pads[1].buttons;
            held = buttons & 0x0F;
            for (const auto& pad : pads) {
                if (pad.ly > 16000) held |= 1u << 0;
                if (pad.ly < -16000) held |= 1u << 1;
                if (pad.lx < -16000) held |= 1u << 2;
                if (pad.lx > 16000) held |= 1u << 3;
                if (pad.analog[0] >= 128) held |= 1u << 4;
                if (pad.analog[1] >= 128) held |= 1u << 5;
            }
        }
        const auto now = GetTickCount64();
        const uint32_t pressed = held & ~g_native_held;
        // One command per frame. B/A are press-only; directions repeat after a
        // delay. Opposing directions cancel rather than alternate every frame.
        if ((held & 3) == 3) held &= ~3u;
        if ((held & 12) == 12) held &= ~12u;
        for (unsigned i : {5u, 4u, 0u, 1u, 2u, 3u}) {
            const uint32_t bit = 1u << i;
            if (!(held & bit)) { g_native_repeat[i] = 0; continue; }
            if (!(pressed & bit) && (i >= 4 || !g_native_repeat[i] || now < g_native_repeat[i])) continue;
            g_native_repeat[i] = now + ((pressed & bit) ? 350 : 90);
            // Preserve the logical owner of Select for the original campaign
            // screen reached after finishing a development mission.
            if (i == 4)
                for (uint32_t port = 0; port < 2; ++port)
                    if (pads[port].analog[0] >= 128) { g_native_controller = port; break; }
            NativeAction(MenuAction(i));
            break;
        }
        g_native_held = held;
    }
    // Never feed native navigation back into ImGui as well.
    const bool enabled = active && g_nav_ready && g_open.load();
    if (enabled && (pads[0].connected || pads[1].connected)) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    auto button = [&](int bit) {
        return enabled && ((pads[0].buttons | pads[1].buttons) & (1u << bit));
    };
    auto analog = [&](int index) {
        return enabled && std::max(pads[0].analog[index], pads[1].analog[index]) >= 128;
    };
    auto axis = [&](bool y, bool positive, bool right = false) {
        if (!enabled) return 0.0f;
        float value = 0;
        for (const auto& p : pads) {
            int raw = right ? (y ? p.ry : p.rx) : (y ? p.ly : p.lx);
            if (!positive) raw = -raw;
            value = std::max(value, std::clamp((float(raw) - 10000.0f) / 22767.0f, 0.0f, 1.0f));
        }
        return value;
    };
    io.AddKeyEvent(ImGuiKey_GamepadStart, button(4));
    io.AddKeyEvent(ImGuiKey_GamepadBack, button(5));
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, button(0));
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, button(1));
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, button(2));
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, button(3));
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, analog(0));
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, analog(1));
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, analog(2));
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, analog(3));
    io.AddKeyEvent(ImGuiKey_GamepadR1, analog(4));
    io.AddKeyEvent(ImGuiKey_GamepadL1, analog(5));
    io.AddKeyEvent(ImGuiKey_GamepadL3, button(6));
    io.AddKeyEvent(ImGuiKey_GamepadR3, button(7));
    for (const auto& [key, value] : std::initializer_list<std::pair<ImGuiKey, float>>{
        {ImGuiKey_GamepadL2, enabled ? float(std::max(pads[0].analog[6], pads[1].analog[6])) / 255 : 0},
        {ImGuiKey_GamepadR2, enabled ? float(std::max(pads[0].analog[7], pads[1].analog[7])) / 255 : 0},
        {ImGuiKey_GamepadLStickLeft, axis(false, false)}, {ImGuiKey_GamepadLStickRight, axis(false, true)},
        {ImGuiKey_GamepadLStickUp, axis(true, true)}, {ImGuiKey_GamepadLStickDown, axis(true, false)},
        {ImGuiKey_GamepadRStickLeft, axis(false, false, true)}, {ImGuiKey_GamepadRStickRight, axis(false, true, true)},
        {ImGuiKey_GamepadRStickUp, axis(true, true, true)}, {ImGuiKey_GamepadRStickDown, axis(true, false, true)}})
        io.AddKeyAnalogEvent(key, value > 0.1f, value);
    const bool back = analog(1);
    if (back && !g_back_down && ImGui::GetCurrentContext()->OpenPopupStack.empty()) Close();
    g_back_down = back;
}

void Draw(double fps) {
    // Runtime calls under UiLock. No GPU/guest lock may be acquired from here.
    const auto output_info = ActiveOutput();
    ReportDisplay(fps, output_info);
    if (FpsCounter()) Counter(fps);
    if (!g_open.load()) return;
    if (g_confirm && GetTickCount64() >= g_deadline) Revert("Time expired. Previous settings restored.");
    if (GetTickCount64() >= g_next_displays && !g_confirm) {
        const bool invalidated = g_next_displays == 0;
        auto list = app::Displays();
        const bool same = list.size() == g_displays.size() && std::equal(list.begin(), list.end(), g_displays.begin(),
            [](const app::Display& a, const app::Display& b) {
                return a.device == b.device && a.handle == b.handle && a.primary == b.primary &&
                       a.desktop.width == b.desktop.width && a.desktop.height == b.desktop.height;
            });
        g_displays = std::move(list);
        g_next_displays = GetTickCount64() + 1000;
        if (invalidated || !same) RefreshModes();
    }
    if (!g_confirm) {
        auto chosen = std::find_if(g_displays.begin(), g_displays.end(), [](const app::Display& display) {
            return !g_draft.monitor.empty() && display.device == g_draft.monitor;
        });
        if (chosen == g_displays.end())
            chosen = std::find_if(g_displays.begin(), g_displays.end(), [](const app::Display& display) { return display.primary; });
        if (chosen != g_displays.end() && chosen->handle != g_monitor) RefreshModes();
    }
    const auto* viewport = ImGui::GetMainViewport();
    float width = std::max(280.0f, std::min(680.0f, viewport->WorkSize.x - 24));
    float height = std::max(240.0f, std::min(640.0f, viewport->WorkSize.y - 24));
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    if (g_focus) { ImGui::SetNextWindowFocus(); g_focus = false; }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025f, 0.075f, 0.08f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.06f, 0.26f, 0.28f, 1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.96f, 0.77f, 0.26f, 1));
    bool visible = true;
    if (ImGui::Begin("Marvel Nemesis | PC Settings", &visible,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("DISPLAY, GRAPHICS AND CONTROLS");
        ImGui::TextWrapped("Use mouse, keyboard or controller. B / Esc / F10: close. Pause gameplay before making changes.");
        if (fps > 0 && std::isfinite(fps)) ImGui::Text("Current performance: %.1f FPS", fps);
        ImGui::Separator();
        const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetTextLineHeightWithSpacing() * 3;
        if (ImGui::BeginChild("VideoOptions", ImVec2(0, -footer))) {
            ImGui::BeginDisabled(g_confirm);
            ImGui::PushItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.46f));
            if (ImGui::BeginTabBar("PcOptionsTabs")) {
                if (ImGui::BeginTabItem("General")) {
                    ImGui::Checkbox("Pause when unfocused", &g_draft.pause_on_focus_loss);
                    ImGui::TextWrapped("Pauses gameplay and audio when using Alt+Tab or minimizing. Resumes on return. Apply to save.");
                    ImGui::TextWrapped("The display reversion timer continues while paused. Opening F10 alone does not pause gameplay.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Graphics")) { Controls(output_info); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Controls")) { pc_controls::Draw(g_draft.input); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Videos")) { pc_video::Draw(g_draft.movies); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Cheats")) { pc_cheats::Draw(g_draft.cheats); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            ImGui::PopItemWidth();
            ImGui::EndDisabled();
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (g_confirm) {
            const auto now = GetTickCount64();
            auto remaining = g_deadline > now ? (g_deadline - now + 999) / 1000 : 0;
            ImGui::TextWrapped("Keep these display settings? Reverting in %u s.", unsigned(remaining));
            if (ImGui::Button("Keep and Save")) {
                g_confirm = false;
                Save();
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert")) Revert("Previous settings restored.");
        } else {
            bool dirty = !(g_draft == Current());
            ImGui::TextUnformatted(dirty ? "Unsaved changes." : "Current settings.");
            ImGui::BeginDisabled(!dirty && !g_save_failed);
            if (ImGui::Button("Apply")) Apply();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel / Close")) Close();
            ImGui::SameLine();
            if (ImGui::Button("Restore Defaults")) { g_draft = Video{}; pc_controls::CancelCapture(); }
        }
        if (!g_status.empty()) ImGui::TextWrapped("%s", g_status.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor(3);
    if (!visible) Close();
}

}  // namespace game::settings_panel
