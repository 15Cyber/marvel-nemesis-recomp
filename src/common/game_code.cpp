// First-launch preparation adapted from TGP482/SCDA-XBERecomp V2 (MIT); see NOTICE.
// The recompiled game code is built on the player's PC, so a release carries none of it: xbe_recomp translates each
// xbe in assets to C++, clang compiles that and lld links it into <exe>.dll beside the executables, all from the
// compiler folder. The first start does this behind a progress window. Finished objects are kept, so a closed window
// or a crash loses only the files in progress and the next start carries on.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>


#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string_view>
#include <thread>

#include <toml++/toml.hpp>

#include "app/app.h"

#include "core/log.h"
#include "image_symbols.h"
#include "kernel/disc.h"

namespace dispatch {
bool Load(const std::filesystem::path& dll);  // cpu/dispatch.cpp
}

namespace app {

namespace {

namespace fs = std::filesystem;

// the executables and the xbe recompiled into each
struct Image {
    const wchar_t* exe;
    const char* xbe;
    const char* config;
};
constexpr Image kImages[] = {
    {L"MarvelNemesis", "default.xbe", "game.toml"},

};

// as CMakePresets.json and CMakeLists.txt compile it for build.bat
constexpr wchar_t kClangFlags[] =
    L"-target x86_64-pc-windows-msvc -fms-compatibility-version=19.40 -std=c++20 -O2 -march=x86-64-v2 "
    L"-fno-strict-aliasing -DNDEBUG -DX86_GAME_CODE -nostdinc -nostdinc++ -w";
constexpr wchar_t kLinkFlags[] = L"/dll /noentry /nodefaultlib /machine:x64 /opt:noicf";
constexpr wchar_t kTitle[] = L"Marvel Nemesis: Rise of the Imperfects";
constexpr uint64_t kJobMemory = 512ull << 20;  // a compiler's peak on the biggest file is about half of this
constexpr uint64_t kDiskNeeded = 1ull << 30;    // both images' sources, objects and DLLs, with room to spare

// the F10 panel's colours
constexpr COLORREF kBackground = RGB(10, 11, 10);
constexpr COLORREF kText = RGB(227, 232, 219);
constexpr COLORREF kDim = RGB(145, 153, 138);
constexpr COLORREF kBarEmpty = RGB(28, 33, 26);
constexpr COLORREF kBarFull = RGB(145, 158, 130);

enum class Stage { Translating, Compiling, Linking, Ready };

// written by the build thread, read by the window
struct Progress {
    std::atomic<Stage> stage{Stage::Translating};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint32_t> files_done{0};
    std::atomic<uint32_t> files_total{0};
    std::atomic<uint32_t> first_wave{1};  // files compiled before the time left is worth showing
    std::atomic<int64_t> compile_start{0};  // steady clock ms
    std::atomic<bool> cancel{false};
};

Progress g_progress;
std::wstring g_failure;  // the step that failed, written by the build thread before it ends
HWND g_window = nullptr;



constexpr UINT kBuildDone = WM_APP + 1;
HFONT g_prepare_font = nullptr;
void PrepareFont(HWND window) {
    if (g_prepare_font) DeleteObject(g_prepare_font);
    g_prepare_font = CreateFontW(-MulDiv(9, int(GetDpiForWindow(window)), 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

fs::path CompilerDir() { return ExeDir() / "compiler"; }
fs::path WorkDir(const Image& image) { return CompilerDir() / "work" / image.exe; }
fs::path CodeDll(const Image& image) { return ExeDir() / (std::wstring(image.exe) + L".dll"); }
fs::path StampFile(const Image& image) { return WorkDir(image) / "built.txt"; }
std::wstring Quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }
fs::path CompileLog() { return ExeDir() / "logs" / "compile.log"; }

// keeps the failed step for the message the player sees; false, to return
bool RecordFailure(const std::wstring& what) {
    g_failure = what;
    LOGE("Game code: %ls", what.c_str());
    return false;
}

// an image to build, with what its finished DLL is stamped with
struct Target {
    const Image* image = nullptr;
    std::vector<uint8_t> xbe;
    std::string stamp;
};

// a DLL serves the executable it was linked against and the xbe it was translated from
bool StampFor(const Image& image, const std::vector<uint8_t>& xbe, std::string& stamp) {
    WIN32_FILE_ATTRIBUTE_DATA exe{};
    fs::path path = ExeDir() / (std::wstring(image.exe) + L".exe");
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &exe)) return false;
    uint64_t size = (uint64_t(exe.nFileSizeHigh) << 32) | exe.nFileSizeLow;
    uint64_t time = (uint64_t(exe.ftLastWriteTime.dwHighDateTime) << 32) | exe.ftLastWriteTime.dwLowDateTime;
    std::string sha, error;
    if (!ValidateGameImage(xbe, sha, error)) return false;
    stamp = std::to_string(size) + " " + std::to_string(time) + " " + sha;
    return true;
}

// build.bat's DLL has no stamp: it is current when newer than the executable it was linked against
bool Current(const Target& target) {
    std::error_code ec;
    fs::file_time_type built = fs::last_write_time(CodeDll(*target.image), ec);
    if (ec) return false;
    std::ifstream f(StampFile(*target.image));
    if (!f) return built > fs::last_write_time(ExeDir() / (std::wstring(target.image->exe) + L".exe"), ec);
    std::string line;
    return std::getline(f, line) && line == target.stamp;
}

// an image's xbe from the game in assets: the extracted file, else the mounted disc image
bool ReadXbe(const GameFiles& game, const char* name, std::vector<uint8_t>& out) {
    if (std::string_view(name) == IMAGE_XBE) {
        out = game.xbe;
        return true;
    }
    std::ifstream f(game.dir / name, std::ios::binary);
    if (f) {
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }
    const kernel::Disc* disc = kernel::MountedDisc();
    return disc && disc->Load(name, out);
}

std::vector<Target> StaleTargets(const GameFiles& game) {
    std::vector<Target> stale;
    for (const Image& image : kImages) {
        Target target{&image};
        if (!ReadXbe(game, image.xbe, target.xbe)) {
            LOGW("game code: no %s in the game files, %ls is not built", image.xbe, image.exe);
            continue;
        }
        if (!StampFor(image, target.xbe, target.stamp) || Current(target)) continue;
        stale.push_back(std::move(target));
    }
    return stale;
}

// a source to compile and the object it makes
struct Job {
    fs::path source;
    fs::path object;
    uint64_t bytes = 0;
};

uint32_t PartitionCount(const Image& image) {
    try {
        std::ifstream input(CompilerDir() / "config" / image.config, std::ios::binary);
        toml::table t = toml::parse(input);
        return uint32_t(t["xbe"]["partitions"].value_or(int64_t(0)));
    } catch (const toml::parse_error&) {
        return 0;
    }
}

// the newest of what every object of an image is built from besides its own source
fs::file_time_type SharedInputsTime(const Image& image) {
    std::error_code ec;
    fs::file_time_type newest = fs::last_write_time(WorkDir(image) / "x86_recomp_shared.h", ec);
    newest = std::max(newest, fs::last_write_time(CompilerDir() / "clang.exe", ec));
    for (auto& e : fs::recursive_directory_iterator(CompilerDir() / "include", ec))
        newest = std::max(newest, e.last_write_time(ec));
    return newest;
}

// the image's sources whose object is missing or older than what it is built from
void AddJobs(const Image& image, std::vector<Job>& jobs) {
    fs::path work = WorkDir(image);
    std::vector<std::string> names{"x86_func_mapping"};
    for (uint32_t i = 0; i < PartitionCount(image); i++) names.push_back("x86_recomp." + std::to_string(i));
    fs::file_time_type shared = SharedInputsTime(image);
    for (const std::string& name : names) {
        Job job{work / (name + ".cpp"), work / (name + ".obj")};
        std::error_code ec;
        job.bytes = fs::file_size(job.source, ec);
        fs::file_time_type built = fs::last_write_time(job.object, ec);
        if (!ec && built > shared && built > fs::last_write_time(job.source, ec)) continue;
        jobs.push_back(std::move(job));
    }
}

// compilers that run at once: one a logical processor, as many as the free memory holds
size_t Slots() {
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    size_t by_memory = size_t(mem.ullAvailPhys / kJobMemory);
    size_t by_cpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return std::clamp<size_t>(std::min(by_memory, by_cpu), 1, MAXIMUM_WAIT_OBJECTS);
}

// starts and waits on the build's processes; closing the job, or this process ending, ends them all
class Processes {
public:
    Processes() {
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_ && !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit))) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        DWORD attributes = FILE_ATTRIBUTE_NORMAL;
        log_ = CreateFileW(CompileLog().c_str(), FILE_APPEND_DATA, share, &inherit, CREATE_ALWAYS, attributes, nullptr);
    }
    ~Processes() {
        CloseHandle(job_);
        if (log_ != INVALID_HANDLE_VALUE) CloseHandle(log_);
    }

    // null when it did not start; its output goes to logs\compile.log
    HANDLE Start(std::wstring command, const fs::path& dir) {
        if (!job_ || log_ == INVALID_HANDLE_VALUE) return nullptr;
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log_;
        si.hStdError = log_;
        PROCESS_INFORMATION pi{};
        DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS;
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags, nullptr, dir.c_str(), &si, &pi)) {
            LOGE("game code: could not start %ls (%lu)", command.c_str(), GetLastError());
            return nullptr;
        }
        if (!job_ || !AssignProcessToJobObject(job_, pi.hProcess)) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return nullptr;
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    // closes the process; 0 when it exited cleanly
    static DWORD ExitCode(HANDLE process) {
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        return code;
    }
    static bool Succeeded(HANDLE process) { return ExitCode(process) == 0; }

    // false when cancelled, which ends every process of the build
    bool WaitAll(std::vector<HANDLE>& processes) {
        while (WaitForMultipleObjects(DWORD(processes.size()), processes.data(), TRUE, 250) == WAIT_TIMEOUT)
            if (g_progress.cancel) return Cancel();
        return true;
    }

    bool Cancel() {
        TerminateJobObject(job_, 1);
        return false;
    }

private:
    HANDLE job_ = nullptr;
    HANDLE log_ = INVALID_HANDLE_VALUE;
};

// xbe_recomp writes each image's C++ into its work folder, the images side by side
bool Translate(std::vector<Target>& targets, Processes& run) {
    std::vector<HANDLE> processes;
    for (Target& target : targets) {
        fs::path work = WorkDir(*target.image);
        fs::path xbe = work / target.image->xbe;
        std::ofstream(xbe, std::ios::binary).write(reinterpret_cast<const char*>(target.xbe.data()), target.xbe.size());
        fs::path config = CompilerDir() / "config" / target.image->config;
        std::wstring command = Quote(CompilerDir() / "xbe_recomp.exe") + L" " + Quote(config) + L" " + Quote(xbe);
        HANDLE process = run.Start(command + L" " + Quote(work) + L" --xdk " + Quote(CompilerDir() / "xdk"), work);
        if (!process) return RecordFailure(L"xbe_recomp.exe could not be started.");
        processes.push_back(process);
    }
    if (!run.WaitAll(processes)) return false;
    bool ok = true;
    constexpr DWORD kDllNotFound = 0xC0000135;  // STATUS_DLL_NOT_FOUND, the process never started
    const wchar_t* missing_runtime = L"xbe_recomp.exe needs msvcp140.dll and vcruntime140.dll beside it in the compiler folder. Extract the whole release again, then start the game.";
    for (size_t i = 0; i < processes.size(); i++) {
        DWORD code = Processes::ExitCode(processes[i]);
        std::string xbe = targets[i].image->xbe;
        if (code == kDllNotFound) ok = RecordFailure(missing_runtime);
        else if (code) ok = RecordFailure(L"Translating " + std::wstring(xbe.begin(), xbe.end()) + L" failed.");
    }
    return ok;
}

std::wstring CompileCommand(const Job& job) {
    fs::path include = CompilerDir() / "include";
    std::wstring command = Quote(CompilerDir() / "clang.exe") + L" " + kClangFlags;
    command += L" -isystem " + Quote(include / "freestanding") + L" -I" + Quote(include) + L" -c ";
    return command + job.source.filename().wstring() + L" -o " + job.object.filename().wstring() + L".tmp";
}

// an object is written beside its final name and moved there once whole
bool FinishJob(const Job& job, HANDLE process) {
    std::wstring name = job.source.parent_path().filename().wstring() + L"\\" + job.source.filename().wstring();
    if (!Processes::Succeeded(process)) return RecordFailure(L"Compiling " + name + L" failed.");
    fs::path temp = job.object;
    temp += ".tmp";
    std::error_code ec;
    fs::rename(temp, job.object, ec);
    if (ec) return RecordFailure(L"Saving the compiled " + name + L" failed.");
    g_progress.bytes_done += job.bytes;
    g_progress.files_done++;
    return true;
}

// the biggest files first, as many at once as Slots allows
bool Compile(std::vector<Job>& jobs, Processes& run) {
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.bytes > b.bytes; });
    size_t slots = Slots();
    LOGR("Game code: compiling %zu files, %zu at a time", jobs.size(), slots);
    for (const Job& job : jobs) g_progress.bytes_total += job.bytes;
    g_progress.files_total = uint32_t(jobs.size());
    g_progress.first_wave = uint32_t(std::min(slots, jobs.size()));
    g_progress.compile_start = NowMs();
    g_progress.stage = Stage::Compiling;

    std::vector<HANDLE> running;
    std::vector<const Job*> running_jobs;
    size_t next = 0;
    while (next < jobs.size() || !running.empty()) {
        for (; running.size() < slots && next < jobs.size(); next++) {
            HANDLE process = run.Start(CompileCommand(jobs[next]), jobs[next].source.parent_path());
            if (!process) {
                RecordFailure(L"clang.exe could not be started.");
                return run.Cancel();
            }
            running.push_back(process);
            running_jobs.push_back(&jobs[next]);
        }
        DWORD wait = WaitForMultipleObjects(DWORD(running.size()), running.data(), FALSE, 250);
        if (g_progress.cancel) return run.Cancel();
        if (wait == WAIT_TIMEOUT) continue;
        size_t done = wait - WAIT_OBJECT_0;
        if (done >= running.size()) {
            RecordFailure(L"Waiting on the compilers failed.");
            return run.Cancel();
        }
        if (!FinishJob(*running_jobs[done], running[done])) return run.Cancel();
        running.erase(running.begin() + done);
        running_jobs.erase(running_jobs.begin() + done);
    }
    return true;
}

// lld links the objects against the executable's exports into the DLL, then the stamp says what it was built from
bool Link(const Target& target, Processes& run) {
    const Image& image = *target.image;
    fs::path work = WorkDir(image);
    std::wstring command = Quote(CompilerDir() / "lld-link.exe") + L" " + kLinkFlags + L" /def:exports.def";
    fs::path temporary = CodeDll(image);
    temporary += L".tmp";
    command += L" /implib:code.lib /out:" + Quote(temporary);
    command += L" " + Quote(CompilerDir() / (std::wstring(image.exe) + L".lib")) + L" x86_func_mapping.obj";
    for (uint32_t i = 0; i < PartitionCount(image); i++) command += L" x86_recomp." + std::to_wstring(i) + L".obj";
    std::wstring dll = CodeDll(image).filename().wstring();
    std::vector<HANDLE> process{run.Start(command, work)};
    if (!process[0]) return RecordFailure(L"lld-link.exe could not be started.");
    if (!run.WaitAll(process)) return false;
    if (!Processes::Succeeded(process[0])) {
        std::wstring hint = L" If the game is already running, close it, then start it again.";
        return RecordFailure(L"Linking " + dll + L" failed." + hint);
    }
    if (!MoveFileExW(temporary.c_str(), CodeDll(image).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return RecordFailure(L"Saving the game code failed. Close other copies of the game and try again.");
    std::ofstream stamp(StampFile(image));
    stamp << target.stamp << "\n";
    stamp.close();
    if (!stamp) return RecordFailure(L"Saving the preparation record failed. Check free space and folder permissions.");
    return true;
}

bool Build(std::vector<Target>& targets) {
    Processes run;
    if (!Translate(targets, run)) return false;
    std::vector<Job> jobs;
    for (const Target& target : targets) AddJobs(*target.image, jobs);
    if (!Compile(jobs, run)) return false;
    g_progress.stage = Stage::Linking;
    for (const Target& target : targets)
        if (!Link(target, run)) return false;
    return true;
}

// what stops a build before it starts, for the player to fix; empty when nothing does
std::wstring Blocker(const std::vector<Target>& targets) {
    for (const wchar_t* tool : {L"clang.exe", L"lld-link.exe", L"xbe_recomp.exe"}) {
        std::error_code ec;
        if (!fs::exists(CompilerDir() / tool, ec))
            return L"The compiler folder is missing files. Extract the whole release again, then start the game.";
    }
    for (const Target& target : targets) {
        std::error_code ec;
        fs::create_directories(WorkDir(*target.image), ec);
        if (ec)
            return L"The game's folder cannot be written to. Move it somewhere you can write to, such as Documents.";
    }
    ULARGE_INTEGER free{};
    if (GetDiskFreeSpaceExW(CompilerDir().c_str(), &free, nullptr, nullptr) && free.QuadPart < kDiskNeeded)
        return L"Preparing the game needs 1 GB of free space on the drive it is on.";
    return {};
}

const wchar_t* StageText() {
    switch (g_progress.stage.load()) {
        case Stage::Translating: return L"Translating your Xbox game's code...";
        case Stage::Compiling: return L"Compiling the game for this PC...";
        case Stage::Linking: return L"Finishing preparation...";
        default: return L"Ready. Starting the game...";
    }
}

LRESULT CALLBACK PrepareWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: PrepareFont(hwnd); return 0;
        case WM_DPICHANGED: {
            PrepareFont(hwnd);
            const auto* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_DESTROY:
            if (g_prepare_font) DeleteObject(g_prepare_font);
            g_prepare_font = nullptr;
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT area;
            GetClientRect(hwnd, &area);
            FillRect(dc, &area, GetSysColorBrush(COLOR_WINDOW));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            auto old_font = SelectObject(dc, g_prepare_font ? reinterpret_cast<HGDIOBJ>(g_prepare_font) : GetStockObject(DEFAULT_GUI_FONT));
            const auto pixel = [dpi = GetDpiForWindow(hwnd)](int value) { return MulDiv(value, int(dpi), 96); };
            RECT heading{pixel(20), pixel(15), area.right - pixel(20), pixel(45)};
            DrawTextW(dc, L"Preparing Marvel Nemesis for this PC", -1, &heading, DT_SINGLELINE | DT_VCENTER);
            RECT stage{pixel(20), pixel(50), area.right - pixel(20), pixel(80)};
            DrawTextW(dc, StageText(), -1, &stage, DT_SINGLELINE | DT_VCENTER);
            float fraction = 0;
            if (g_progress.stage == Stage::Compiling && g_progress.bytes_total)
                fraction = float(g_progress.bytes_done) / float(g_progress.bytes_total);
            if (g_progress.stage == Stage::Linking || g_progress.stage == Stage::Ready) fraction = 1;
            RECT track{pixel(20), pixel(85), area.right - pixel(20), pixel(100)};
            FillRect(dc, &track, GetSysColorBrush(COLOR_BTNFACE));
            track.right = track.left + LONG(float(track.right - track.left) * fraction);
            FillRect(dc, &track, GetSysColorBrush(COLOR_HIGHLIGHT));
            RECT detail{pixel(20), pixel(110), area.right - pixel(20), pixel(160)};
            std::wstring text = L"First launch may take several minutes. No Visual Studio installation is needed.";
            if (g_progress.stage == Stage::Compiling)
                text = L"Files completed: " + std::to_wstring(g_progress.files_done) + L" / " +
                       std::to_wstring(g_progress.files_total) + L". Closing keeps completed work for next time.";
            DrawTextW(dc, text.c_str(), -1, &detail, DT_WORDBREAK);
            SelectObject(dc, old_font);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER: InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_CLOSE:
            if (MessageBoxW(hwnd, L"Stop preparing the game? Completed files are kept for the next launch.",
                            kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) g_progress.cancel = true;
            return 0;
        case kBuildDone: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RunBuild(std::vector<Target>& targets) {
    try { return Build(targets); }
    catch (const std::exception& error) {
        LOGE("Game code: preparation exception: %s", error.what());
        return RecordFailure(L"The preparation files could not be accessed. See logs/compile.log.");
    }
}

bool BuildWithWindow(std::vector<Target>& targets, bool prepare_only) {
    if (std::wstring blocker = Blocker(targets); !blocker.empty()) {
        RecordFailure(blocker);
        if (!prepare_only) MessageBoxW(nullptr, blocker.c_str(), kTitle, MB_ICONERROR);
        return false;
    }
    if (prepare_only) return RunBuild(targets);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PrepareWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"MarvelNemesisGameCode";
    RegisterClassExW(&wc);
    const auto dpi = GetDpiForSystem();
    RECT area{0, 0, MulDiv(600, int(dpi), 96), MulDiv(180, int(dpi), 96)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&area, style, FALSE, 0, dpi);
    g_window = CreateWindowExW(0, wc.lpszClassName, kTitle, style | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left, area.bottom - area.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_window) return RecordFailure(L"The preparation window could not be opened.");
    SetTimer(g_window, 1, 200, nullptr);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    bool ok = false;
    std::thread worker([&] {
        ok = RunBuild(targets);
        if (ok) g_progress.stage = Stage::Ready;
        PostMessageW(g_window, kBuildDone, ok, 0);
    });
    MSG msg;
    while (IsWindow(g_window) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    worker.join();
    SetThreadExecutionState(ES_CONTINUOUS);
    if (!ok && !g_progress.cancel) {
        std::wstring error = g_failure + L"\n\nSee logs\\compile.log for details.";
        MessageBoxW(nullptr, error.c_str(), kTitle, MB_ICONERROR);
    }
    return ok;
}

}  // namespace

bool PrepareGameCode(const GameFiles& game) {
    bool prepare_only = std::wcsstr(GetCommandLineW(), L"--prepare-only") != nullptr;
    HANDLE lock = CreateMutexW(nullptr, FALSE, L"MarvelNemesis-GameCode");
    if (!lock) return false;
    DWORD wait = WaitForSingleObject(lock, INFINITE);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(lock); return false; }
    bool ok = false;
    try {
        std::vector<Target> stale = StaleTargets(game);
        LOGR("Game code: %s", stale.empty() ? "using cached DLL" : "first-launch preparation required");
        bool built = stale.empty() || BuildWithWindow(stale, prepare_only);
        if (built) ok = dispatch::Load(CodeDll(kImages[0]));
    } catch (const std::exception& error) {
        LOGE("Game code: %s", error.what());
    }
    ReleaseMutex(lock);
    CloseHandle(lock);
    logx::Flush();
    if (!ok && !prepare_only && !g_progress.cancel && g_failure.empty())
        MessageBoxW(nullptr, L"The prepared game code could not be loaded. See the log in the logs folder.", kTitle, MB_ICONERROR);
    return ok;
}

}  // namespace app

