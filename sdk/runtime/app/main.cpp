// the host: window, message loop, crash dumps; the game is described by app::kGame
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cwchar>
#include <thread>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace app {

namespace {

HWND g_hwnd = nullptr;
constexpr UINT kApplyDisplay = WM_APP + 1, kOpenWindow = WM_APP + 2;
bool g_mode_changed = false;
std::wstring g_mode_device;  // restore the display that was actually switched
bool g_applying_display = false;
bool g_apply_posted = false;  // a display apply is waiting in the queue or running

// crash dump in CrashDumps beside the exe
LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* e) {
    // the dump holds no guest memory: the guest registers and return addresses go to the log
    if (kernel::Thread* t = kernel::Current(); e && t && t->ctx) {
        auto* r = e->ExceptionRecord;
        LOGE("exception %08lX at %p on guest thread %X", r->ExceptionCode, r->ExceptionAddress, t->id);
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
            uint64_t va = r->ExceptionInformation[1], g = uint64_t(reinterpret_cast<uintptr_t>(kernel::g_base));
            LOGE("  %s %llX, guest %08llX", r->ExceptionInformation[0] ? "write" : "read", (unsigned long long)va,
                 (unsigned long long)(va - g));
        }
        kernel::LogGuestStack(*t->ctx, 256);
    }
    auto dir = ExeDir() / "CrashDumps";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"%ls.%04u.%02u.%02u_%02u.%02u.%02u.dmp", kGame.name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    HANDLE f = CreateFileW((dir / name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), e, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      MINIDUMP_TYPE(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                    MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory),
                      e ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    logx::Flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGI("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
}

// Window thread only. Restore the old monitor before applying a different one.
void RestoreDisplayMode() {
    if (!g_mode_changed) return;
    g_mode_changed = false;
    const auto device = std::exchange(g_mode_device, std::wstring{});
    const bool applying = g_applying_display;
    g_applying_display = true;
    LONG result = ChangeDisplaySettingsExW(device.c_str(), nullptr, nullptr, 0, nullptr);
    g_applying_display = applying;
    if (result != DISP_CHANGE_SUCCESSFUL) LOGR("display: restoring %ls returned %ld", device.c_str(), result);
}

void ApplyNow() {
    struct Guard {
        Guard() { g_applying_display = true; }
        ~Guard() { g_applying_display = false; }
    } guard;
    const Settings s = ReadSettings();
    RestoreDisplayMode();
    HMONITOR mon = static_cast<HMONITOR>(MonitorHandle(s.monitor));
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(mon, &mi)) {
            LOGE("display: no available monitor for the game window");
            return;
        }
    }
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &desktop)) {
        desktop.dmPelsWidth = mi.rcMonitor.right - mi.rcMonitor.left;
        desktop.dmPelsHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    Mode out = s.width && s.height ? Mode{s.width, s.height} : Mode{desktop.dmPelsWidth, desktop.dmPelsHeight};
    LONG_PTR visible = IsWindowVisible(g_hwnd) ? WS_VISIBLE : 0;
    if (s.display_mode == DisplayMode::Fullscreen && s.width && s.height &&
        (s.width != desktop.dmPelsWidth || s.height != desktop.dmPelsHeight)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = s.width;
        dm.dmPelsHeight = s.height;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        LONG result = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr);
        g_mode_changed = result == DISP_CHANGE_SUCCESSFUL;
        if (g_mode_changed) g_mode_device = mi.szDevice;
        else LOGR("display: mode %ux%u on %ls rejected (%ld)", s.width, s.height, mi.szDevice, result);
        GetMonitorInfoW(mon, &mi);
    }
    if (s.display_mode == DisplayMode::Windowed) {
        RECT rect{0, 0, LONG(out.width), LONG(out.height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        int w = rect.right - rect.left, h = rect.bottom - rect.top;
        const RECT& work = mi.rcWork;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | visible);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, work.left + (work.right - work.left - w) / 2,
                     work.top + (work.bottom - work.top - h) / 2, w, h, SWP_FRAMECHANGED);
    } else {
        const RECT& bounds = mi.rcMonitor;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | visible);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, bounds.left, bounds.top, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_FRAMECHANGED);
    }
    RefreshDisplayInfo(true);
    LOGR("display: applied device=%ls output=%ux%u refresh_hz=%u", mi.szDevice, out.width, out.height, RefreshRate());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    {
        std::lock_guard lock(UiLock());
        if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    }
    if (intptr_t result = 0; kGame.message && kGame.message(hwnd, msg, wp, lp, &result)) return result;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_F3) DumpThreads();
            return 0;
        case WM_ACTIVATE: {
            if (g_applying_display) break;
            const Settings s = ReadSettings();
            if (LOWORD(wp) == WA_INACTIVE) {
                // a changed display mode steps aside for the desktop, minimized, and comes back on return; not while
                // handing over to the next instance, which runs in the same mode
                if (g_mode_changed && !kernel::g_relaunching) {
                    RestoreDisplayMode();
                    ShowWindow(hwnd, SW_MINIMIZE);
                }
            } else if (s.display_mode == DisplayMode::Fullscreen && s.width && s.height && !g_mode_changed &&
                       IsWindowVisible(hwnd) && !g_apply_posted) {
                // once: restoring the window activates it again from inside the apply, and a second apply switched
                // the mode back and forth, after which presents were not taken for a while (the game stood still
                // until the next alt-tab)
                g_apply_posted = true;
                PostMessageW(hwnd, kApplyDisplay, 0, 0);
            }
            break;
        }
        case WM_WINDOWPOSCHANGED:
            RefreshDisplayInfo();
            break;
        case WM_DISPLAYCHANGE: {
            RefreshDisplayInfo(true);
            if (g_applying_display || g_apply_posted || !IsWindowVisible(hwnd) || IsIconic(hwnd)) break;
            const Settings settings = ReadSettings();
            auto target = static_cast<HMONITOR>(MonitorHandle(settings.monitor));
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            RECT current{};
            bool wrong_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY) != target;
            bool wrong_bounds = settings.display_mode != DisplayMode::Windowed &&
                GetMonitorInfoW(target, &info) && GetWindowRect(hwnd, &current) && !EqualRect(&current, &info.rcMonitor);
            if (wrong_monitor || wrong_bounds) {
                g_apply_posted = true;
                PostMessageW(hwnd, kApplyDisplay, 0, 0);
            }
            break;
        }
        case kApplyDisplay:
            g_apply_posted = true;
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            ApplyNow();
            g_apply_posted = false;
            return 0;
        case kOpenWindow:
            CloseSplash();
            if (wp) ApplyNow();
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

std::filesystem::path ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
}

int Instance() {
    static const int n = [] {
        const wchar_t* p = std::wcsstr(GetCommandLineW(), L"--instance");
        return p ? std::max(0, std::min(8, _wtoi(p + 10))) : 0;
    }();
    return n;
}

void* Window() { return g_hwnd; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() { PostMessageW(g_hwnd, kApplyDisplay, 0, 0); }
void OpenGameWindow(bool apply) { SendMessageW(g_hwnd, kOpenWindow, apply, 0); }

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = EffectiveFpsCap();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) Sleep(1);
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(app::WriteCrashDump);
    std::signal(SIGABRT, [](int) { app::WriteCrashDump(nullptr); });
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // one log a session: a relaunch into another image carries on its predecessor's
    bool relaunched = std::wcsstr(GetCommandLineW(), L"--relaunched") != nullptr;
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    logx::Init((app::ExeDir() / "logs").string(), log, relaunched);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;

    bool splash = app::OpenSplash(), handover = app::StartHandover();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = app::WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = app::kGame.name;
    RegisterClassExW(&wc);
    app::g_hwnd = CreateWindowExW(0, wc.lpszClassName, app::kGame.title, WS_POPUP, 0, 0, 1280, 720,
                                  nullptr, nullptr, inst, nullptr);
    // behind a splash the window stays hidden until the game's first frame; handed over, it is set up hidden
    if (handover) {
        app::ApplyNow();
    } else if (!splash) {
        app::ApplyNow();
        ShowWindow(app::g_hwnd, SW_SHOW);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplWin32_Init(app::g_hwnd);

    input::Init();
    if (app::kGame.window) app::kGame.window(app::g_hwnd);
    std::filesystem::path storage;
    std::string storage_error;
    if (!app::PrepareStorage(storage, storage_error)) {
        LOGE("storage: %s", storage_error.c_str());
        MessageBoxW(app::g_hwnd, L"Unable to prepare the saves folder. Existing files were preserved. See the log in the logs folder.",
                    app::kGame.short_title, MB_ICONERROR);
        return 1;
    }
    if (app::Instance())
        SetWindowTextW(app::g_hwnd, (std::wstring(app::kGame.title) + L" [" + std::to_wstring(app::Instance()) + L"]").c_str());
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), storage.string())) {
        MessageBoxW(app::g_hwnd, L"The game failed to start, see the log in the logs folder.", app::kGame.short_title,
                    MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    app::RestoreDisplayMode();
    logx::Flush();
    kernel::Shutdown(0);
}
