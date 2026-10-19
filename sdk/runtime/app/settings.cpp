// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "gpu/msaa.h"
#include "gpu/ambient_occlusion.h"
#include "gpu/shadow_resolution.h"
#include "core/log.h"

namespace app {

namespace {

std::atomic<uint32_t> g_refresh_hz{0};
std::atomic<uint64_t> g_display_revision{0};

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

Settings ReadSettings() {
    std::lock_guard lock(UiLock());
    return Get();
}

void Load() {
    std::lock_guard lock(UiLock());
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.pause_on_focus_loss = t["pause_on_focus_loss"].value_or(s.pause_on_focus_loss);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.monitor = t["monitor"].value_or(s.monitor);
    s.fps_auto = t["fps_auto"].value_or(s.fps_auto);
    s.shadow_softness = gpu::NormalizeShadowSoftness(t["shadow_softness"].value_or(s.shadow_softness));
    s.shadow_resolution = gpu::NormalizeShadowResolution(t["shadow_resolution"].value_or(s.shadow_resolution));
    s.ambient_occlusion = gpu::NormalizeAoMode(t["ambient_occlusion"].value_or(s.ambient_occlusion));
    s.ao_strength = std::min(t["ao_strength"].value_or(s.ao_strength), 100u);
    s.ao_radius = std::clamp(t["ao_radius"].value_or(s.ao_radius), 25u, 400u);
    s.ao_debug = std::min(t["ao_debug"].value_or(s.ao_debug), 3u);
    s.smaa = t["smaa"].value_or(s.smaa);
    s.msaa = gpu::NormalizeMsaa(t["msaa"].value_or(s.msaa));
    s.brightness = std::min(t["brightness"].value_or(s.brightness), 100u);
    s.contrast = std::min(t["contrast"].value_or(s.contrast), 100u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"pause_on_focus_loss", s.pause_on_focus_loss},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"monitor", s.monitor},
                  {"fps_auto", s.fps_auto},
                  {"shadow_softness", int64_t(s.shadow_softness)},
                  {"shadow_resolution", int64_t(s.shadow_resolution)},
                  {"ambient_occlusion", int64_t(s.ambient_occlusion)},
                  {"ao_strength", int64_t(s.ao_strength)},
                  {"ao_radius", int64_t(s.ao_radius)},
                  {"ao_debug", int64_t(s.ao_debug)},
                  {"smaa", s.smaa},
                  {"msaa", int64_t(s.msaa)},
                  {"brightness", int64_t(s.brightness)},
                  {"contrast", int64_t(s.contrast)},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

bool Save() {
    std::lock_guard lock(UiLock());
    const auto target = File();
    auto temporary = target;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
    try {
        std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
        f << Table() << "\n";
        f.flush();
        bool ok = bool(f);
        f.close();
        ok = ok && !f.fail();
        if (ok && MoveFileExW(temporary.c_str(), target.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    } catch (...) {
        // Leave the previous file intact if serialization or writing fails.
    }
    DeleteFileW(temporary.c_str());
    LOGE("settings: could not save %s; previous file preserved", target.string().c_str());
    return false;
}

void LogSettings() {
    std::lock_guard lock(UiLock());
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

namespace {

struct MonitorLookup {
    const std::string* device;
    HMONITOR found = nullptr;
};

BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto& lookup = *reinterpret_cast<MonitorLookup*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    size_t i = 0;
    while (i < lookup.device->size() && info.szDevice[i] &&
           info.szDevice[i] == static_cast<unsigned char>((*lookup.device)[i])) ++i;
    if (i == lookup.device->size() && info.szDevice[i] == 0) {
        lookup.found = monitor;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK CollectDisplay(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    Display d;
    d.handle = monitor;
    d.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    // Windows display device identifiers are ASCII (e.g. a DISPLAY1 device).
    for (const wchar_t* c = info.szDevice; *c; ++c) {
        d.device += char(*c);
        if (!d.number && *c >= L'1' && *c <= L'9') d.number = int(std::wcstoul(c, nullptr, 10));
    }
    DEVMODEW native{}, current{};
    native.dmSize = sizeof(native);
    current.dmSize = sizeof(current);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_REGISTRY_SETTINGS, &native))
        d.desktop = {native.dmPelsWidth, native.dmPelsHeight};
    else d.desktop = {uint32_t(info.rcMonitor.right - info.rcMonitor.left),
                      uint32_t(info.rcMonitor.bottom - info.rcMonitor.top)};
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &current) && current.dmDisplayFrequency > 1)
        d.refresh_hz = current.dmDisplayFrequency;
    if (d.desktop.width && d.desktop.height)
        reinterpret_cast<std::vector<Display>*>(data)->push_back(std::move(d));
    return TRUE;
}

}  // namespace

std::vector<Display> Displays() {
    std::vector<Display> list;
    EnumDisplayMonitors(nullptr, nullptr, CollectDisplay, reinterpret_cast<LPARAM>(&list));
    std::sort(list.begin(), list.end(), [](const Display& a, const Display& b) {
        return a.number != b.number ? a.number < b.number : a.device < b.device;
    });
    return list;
}

void* MonitorHandle(const std::string& device) {
    if (!device.empty()) {
        MonitorLookup lookup{&device};
        EnumDisplayMonitors(nullptr, nullptr, FindMonitor, reinterpret_cast<LPARAM>(&lookup));
        if (lookup.found) return lookup.found;
    }
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

Mode Desktop(const std::string& device) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(static_cast<HMONITOR>(MonitorHandle(device)), &info) &&
        EnumDisplaySettingsW(info.szDevice, ENUM_REGISTRY_SETTINGS, &dm)) return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Desktop() { return Desktop(ReadSettings().monitor); }

void RefreshDisplayInfo(bool force) {
    std::lock_guard lock(UiLock());
    static HMONITOR previous = nullptr;
    HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    if (!force && monitor == previous) return;
    previous = monitor;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    uint32_t hz = 0;
    if (GetMonitorInfoW(monitor, &info) && EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1) hz = dm.dmDisplayFrequency;
    g_refresh_hz = hz;
    ++g_display_revision;
}

uint32_t RefreshRate() { return g_refresh_hz.load(); }

uint32_t EffectiveFpsCap() {
    const auto s = ReadSettings();
    if (!s.fps_auto) return s.fps_cap;
    uint32_t hz = RefreshRate();
    return hz ? std::min(hz, 1000u) : 60u;
}

Mode Output() {
    std::lock_guard lock(UiLock());
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

bool Widescreen() {
    std::lock_guard lock(UiLock());
    // asked per frame and per HUD quad: refresh the desktop shape after resolution/display changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<bool> wide{true};
    static uint64_t revision = ~0ull;
    const Settings& s = Get();
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    const auto next_revision = g_display_revision.load();
    if (key.exchange(k) != k || revision != next_revision) {
        revision = next_revision;
        Mode m = Output();
        wide = m.width * 2 >= m.height * 3;  // 3:2 or wider
    }
    return wide;
}

float RenderScale(uint32_t guest_height) {
    std::lock_guard lock(UiLock());
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
