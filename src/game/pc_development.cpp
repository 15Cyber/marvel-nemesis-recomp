// Development selector for the SHA-1-pinned retail XBE. No disc writes.
// Preserve the native mission/level load paths; never jump into the loader
// while the APT PC Settings page still owns its retained text clips.
#include "common/pc_development.h"
#include "core/log.h"
#include "kernel/disc.h"
#include "xdk/hle.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <utility>

namespace game::pc_development {
namespace {
constexpr uint32_t kChronicle = 0x4F08D8, kCount = kChronicle + 8, kMissions = kChronicle + 12;
constexpr uint32_t kLevelName = 0x4F40CC, kSceneName = 0x4F40EC;
constexpr uint32_t kDirectLevel = 0x4F414C, kNamedScene = 0x4F4150, kNextState = 0x4C343C;
constexpr uint32_t kMemcard = 0x5155E8;
std::atomic<int32_t> g_return_controller{-1};
uint8_t g_autosave_backup = 0; // Native Options backup, captured once per session.
struct TestLevel { const char* name; const char* level; };
constexpr std::array<TestLevel, 3> kTests{{
    {"COMBAT TEST", "0_Test_Clash"},
    {"CHARACTER TEST", "characterTest"},
    {"EMPTY TEST ARENA", "0_Test_Arena"},
}};

bool Address(uint32_t p, uint32_t size) {
    if (p < 0x10000 || !size || uint64_t(p) + size > 0x100000000ull) return false;
    for (uint64_t page = p & ~uint64_t(0xFFF); page < uint64_t(p) + size; page += 0x1000)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}
uint32_t Memcard() {
    const uint32_t card = kernel::R32(kMemcard);
    return Address(card, 0x706) ? card : 0;
}
void ClearAutosaveRequests(X86Context& ctx, uint32_t card) {
    const X86Context saved = ctx;
    // Native SetAutosaveCheck(false) and ResetAutosavePrompt. Do not change
    // the persisted preference (+0x705) or an operation in progress (+0x6E7).
    ctx.ecx = card;
    kernel::CallGuest(ctx, 0x1060B0, {0});
    ctx.ecx = card;
    kernel::CallGuest(ctx, 0x106340, {0});
    ctx = saved;
}
// Native level/scene names occupy fixed 32-byte arrays. Accept only basenames.
std::string Basename(uint32_t p) {
    if (!Address(p, 32)) return {};
    const auto* data = static_cast<const char*>(kernel::HostPtr(p));
    std::string text;
    for (size_t i = 0; i < 32; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (!c) return text;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return {};
        text += char(c);
    }
    return {};  // no terminator
}
bool File(const std::string& name, const char* extension) {
    if (name.empty()) return false;
    const auto relative = "Levels\\" + name + extension;
    // Resolve the shared overlay first, including when the base is an ISO.
    const auto host = kernel::ResolvePath("D:\\" + relative, 0);
    std::error_code error;
    if (!host.empty())
        return std::filesystem::is_regular_file(host, error) &&
            std::filesystem::file_size(host, error) > 0 && !error;
    if (const auto* disc = kernel::MountedDisc()) {
        const auto* entry = disc->Find(relative);
        return entry && !entry->dir && entry->size > 0;
    }
    return false;
}
bool Resources(const Destination& destination, bool refresh = false) {
    static std::map<std::pair<std::string, std::string>, bool> cache;
    const auto key = std::make_pair(destination.level, destination.scene);
    auto at = cache.find(key);
    if (refresh || at == cache.end())
        at = cache.insert_or_assign(key, File(destination.level, ".nil") && File(destination.scene, ".nsd")).first;
    return at->second;
}
// Existence alone is insufficient for the legacy test NILs. Follow only the
// bounded header reads used by 0xAE900 before it calls 0xADF60. That routine
// explicitly rejects a zero geometry-block count. Do not attempt to repair or
// convert the file here, and do not change the working campaign load path.
std::string TestWorldProblem(const std::string& level, bool refresh = false) {
    static std::map<std::string, std::string> cache;
    if (!refresh) {
        const auto found = cache.find(level);
        if (found != cache.end()) return found->second;
    }
    std::vector<uint8_t> data;
    auto inspect = [&]() -> std::string {
        if (!kernel::ReadGameFile("D:\\Levels\\" + level + ".nil", data) || data.size() < 0x7C)
            return "WORLD FILE UNAVAILABLE OR INCOMPLETE";
        auto u32 = [&](size_t p) {
            return uint32_t(data[p]) | (uint32_t(data[p + 1]) << 8) |
                (uint32_t(data[p + 2]) << 16) | (uint32_t(data[p + 3]) << 24);
        };
        if (u32(4) != 0x24) return "WORLD FORMAT UNSUPPORTED";
        const uint32_t textures = u32(0x78);
        uint64_t at = 0x7Cull + uint64_t(textures) * 32;
        if (at + 2 > data.size()) return "WORLD FILE INCOMPLETE";
        const auto models = uint32_t(data[size_t(at)]) | (uint32_t(data[size_t(at + 1)]) << 8);
        at += 2 + uint64_t(models) * 32;
        if (at + 4 > data.size()) return "WORLD FILE INCOMPLETE";
        const auto geometry = u32(size_t(at));
        LOGR("development: world preflight level=%s nil_bytes=%zu version=%u textures=%u models=%u geometry_offset=%llu geometry_blocks=%u",
             level.c_str(), data.size(), u32(4), textures, models,
             static_cast<unsigned long long>(at), geometry);
        if (!geometry) return "WORLD GEOMETRY UNAVAILABLE";
        return {};
    };
    auto problem = inspect();
    cache.insert_or_assign(level, problem);
    return problem;
}
std::optional<Destination> Mission(uint32_t index) {
    if (!Address(kCount, 4)) return {};
    const uint32_t count = kernel::R32(kCount);
    // Chronicle has an inline array of 100 pointers, NOT a pointer to an array.
    if (!count || count > 100 || index >= count || !Address(kMissions, count * 4)) return {};
    const uint32_t record = kernel::R32(kMissions + index * 4);
    if (!Address(record, 0x480) || kernel::R32(record + 0x460) != index) return {};
    Destination destination;
    destination.index = index;
    destination.level = Basename(record + 0x260);
    destination.scene = Basename(record + 0x280); // DebugMissionName returns this field.
    if (destination.level.empty() || destination.scene.empty()) return {};
    destination.name = std::to_string(index + 1) + " " + destination.scene;
    destination.available = Resources(destination);
    return destination;
}
Destination Test(uint32_t index) {
    const auto& test = kTests[index];
    Destination destination{Kind::TestLevel, index, test.name, test.level, test.level};
    destination.available = Resources(destination);
    if (destination.available) {
        destination.unavailable_reason = TestWorldProblem(destination.level);
        destination.available = destination.unavailable_reason.empty();
    }
    return destination;
}
Catalog Collect(bool frontend) {
    Catalog catalog;
    catalog.frontend = frontend;
    if (!frontend) return catalog;
    if (!Address(kChronicle, 0x1AC) || !Address(0x4F42A0, 8)) return catalog;
    catalog.launch_ready = !kernel::R32(0x4F42A0) && !kernel::R32(0x4F42A4);
    const auto count = kernel::R32(kCount);
    if (count > 0 && count <= 100)
        for (uint32_t i = 0; i < count; ++i)
            if (auto mission = Mission(i)) catalog.missions.push_back(std::move(*mission));
    for (uint32_t i = 0; i < kTests.size(); ++i) catalog.tests.push_back(Test(i));
    return catalog;
}
void CopyName(uint32_t address, const std::string& name) {
    std::memset(kernel::HostPtr(address), 0, 32);
    std::memcpy(kernel::HostPtr(address), name.data(), name.size());
}
}

void Tick(X86Context& ctx, bool frontend, bool can_launch) {
    static auto next = std::chrono::steady_clock::time_point{};
    static bool previous_frontend = false;
    const auto now = std::chrono::steady_clock::now();
    if (frontend != previous_frontend || now >= next) {
        Publish(Collect(frontend));
        previous_frontend = frontend;
        next = now + std::chrono::seconds(1);
    }
    // The same source page must still exist after restoring the PC menu.
    // A closed/changed frontend consumes and rejects, rather than deferring a
    // stale request until a later visit to Options.
    if (!can_launch && frontend) return;
    const auto request = TakeRequest();
    if (!request) return;
    if (!frontend || !can_launch || !Address(kLevelName, 0x90) ||
        !Address(kNextState, 4) || kernel::R32(kNextState) == 7) {
        Complete("LOAD CANCELLED; RETURN TO OPTIONS"); return;
    }
    const auto& destination = request->destination;
    const auto current = destination.kind == Kind::Mission ? Mission(destination.index) :
        destination.index < kTests.size() ? std::optional<Destination>(Test(destination.index)) : std::nullopt;
    if (request->controller > 1 || !current || *current != destination || !Resources(*current, true)) {
        Complete("LOAD CANCELLED; LEVEL OR SCENE UNAVAILABLE"); return;
    }
    if (current->kind == Kind::TestLevel) {
        const auto problem = TestWorldProblem(current->level, true);
        if (!problem.empty()) { Complete("LOAD CANCELLED; " + problem); return; }
    }
    // A Versus session can leave either of these flags set. Do not turn it into
    // a campaign by poking match state; require the normal frontend reset first.
    if (kernel::R32(0x4F42A0) || kernel::R32(0x4F42A4)) {
        Complete("RETURN TO MAIN MENU BEFORE USING DEVELOPMENT"); return;
    }
    const auto card = Memcard();
    if (!card) {
        Complete("SAVE SYSTEM UNAVAILABLE; RETURN TO MAIN MENU"); return;
    }
    if (kernel::G<uint8_t>(card + 0x6E7)) {
        Complete("WAIT FOR THE CURRENT SAVE TO FINISH; TRY AGAIN"); return;
    }
    LOGR("development: launch kind=%u index=%u level=%s scene=%s player_count=%u controller=%u previous_controller=%d",
         unsigned(current->kind), current->index, current->level.c_str(), current->scene.c_str(), kernel::R32(0x4F40C0),
         request->controller, int32_t(kernel::R32(0x515CFC)));
    const X86Context saved = ctx;
    // Arm only after all launch checks, before any mission setup can request
    // an autosave. Keep this policy across Quit/results/mission selection:
    // that RAM still contains test progress until a real save is loaded.
    if (!AutosaveBlocked()) g_autosave_backup = kernel::G<uint8_t>(card + 0x705);
    ClearAutosaveRequests(ctx, card);
    SetAutosaveBlocked(true);
    LOGR("development: autosave blocked for test session (stored_preference=%u)",
         unsigned(kernel::G<uint8_t>(card + 0x705)));
    // The stock mission screen reads nController0 from GetPrevCharList and
    // binds it after autosave. DebugChooseMission bypasses character selection,
    // so seed this cache and mapping using the native Control_BindController.
    g_return_controller.store(int32_t(request->controller), std::memory_order_relaxed);
    kernel::CallGuest(ctx, 0xE3490, {request->controller, 0});
    if (current->kind == Kind::Mission) {
        kernel::W32(kDirectLevel, 0);
        kernel::W32(kNamedScene, 0);
        // Original cdecl DebugChooseMission(index). This selects the native
        // chronicle record, prepares loading and requests state 7.
        kernel::CallGuest(ctx, 0xE7D00, {current->index});
    } else {
        // Venue_StartLevel's level request, with an explicit existing NSD.
        // The consumer at 0x2E186 passes this scene with flags 0x1018. Avoid
        // calling 0xE8DD0 directly: it owns a by-value guest string/destructor.
        CopyName(kLevelName, current->level);
        CopyName(kSceneName, current->scene);
        kernel::W32(kNamedScene, 1);
        kernel::CallGuest(ctx, 0x10AC70, {0});
        kernel::W32(kDirectLevel, 1);
        kernel::W32(kNextState, 7);
    }
    ctx = saved;
    Complete("LOAD REQUESTED; DEVELOPMENT AUTOSAVE DISABLED");
    Publish(Catalog{});
}
static int32_t ReturnMenuController() {
    return g_return_controller.load(std::memory_order_relaxed);
}
static void EndTestSession(X86Context& ctx, const char* reason) {
    if (!AutosaveBlocked()) return;
    if (const auto card = Memcard()) {
        ClearAutosaveRequests(ctx, card);
        const X86Context saved = ctx;
        ctx.ecx = card;
        kernel::CallGuest(ctx, 0x106320, {g_autosave_backup});
        ctx = saved;
    }
    SetAutosaveBlocked(false);
    g_return_controller.store(-1, std::memory_order_relaxed);
    Complete("DEVELOPMENT ENDED; NORMAL AUTOSAVE RESTORED");
    LOGR("development: normal autosave restored after %s", reason);
}
static void FilterAutosaveResult(X86Context& ctx, const char* query) {
    if (!AutosaveBlocked()) return;
    const auto original = uint8_t(ctx.eax);
    // These native getters return a byte in AL. Keep the original stack,
    // flags, upper EAX bits and all side effects of their ordinary wrappers.
    ctx.eax &= 0xFFFFFF00u;
    static std::atomic<uint32_t> reports{0};
    const auto count = reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 12)
        LOGR("development: autosave query=%s original=%u effective=0", query, unsigned(original));
}
}  // namespace game::pc_development

// The APT save flow consults these before starting its dialog/task. Returning
// false here follows the stock disabled-autosave branch instead of cancelling
// a disk operation or pretending an unfinished save has completed.
X86_FUNC(__imp__sub_00103210);
X86_FUNC(sub_00103210) {
    // GetSettings/GetSettings8 build oMemCardObj.autosave from this preference
    // OR GetAutosaveCheck. Both inputs must be false for the result screen.
    // Keep other native callers, including storage slot selection, unchanged.
    const uint32_t caller = kernel::R32(ctx.esp);
    __imp__sub_00103210(ctx, base);
    if (caller == 0xE632C || caller == 0xE64DC)
        game::pc_development::FilterAutosaveResult(ctx, "apt-settings-autosave");
}
X86_FUNC(__imp__sub_00106310);
X86_FUNC(sub_00106310) {
    __imp__sub_00106310(ctx, base);
    game::pc_development::FilterAutosaveResult(ctx, "settings-enabled");
}
X86_FUNC(__imp__sub_00103220);
X86_FUNC(sub_00103220) {
    // Native Options may reapply the effective OFF value from its APT object
    // when changing an unrelated option. Keep the real preference unchanged
    // until the test session ends, including its settings-file serialization.
    const uint32_t state = kernel::R32(0x5151D4);
    const bool preserve = game::pc_development::AutosaveBlocked() &&
        game::pc_development::Address(state, 0x29);
    const uint32_t argument = ctx.esp + 4;
    const uint32_t requested = preserve ? kernel::R32(argument) : 0;
    if (preserve) kernel::W32(argument, kernel::G<uint8_t>(state + 0x28));
    __imp__sub_00103220(ctx, base);
    if (preserve) kernel::W32(argument, requested);
}
X86_FUNC(__imp__sub_001060A0);
X86_FUNC(sub_001060A0) {
    __imp__sub_001060A0(ctx, base);
    game::pc_development::FilterAutosaveResult(ctx, "check-requested");
}
X86_FUNC(__imp__sub_00106330);
X86_FUNC(sub_00106330) {
    __imp__sub_00106330(ctx, base);
    game::pc_development::FilterAutosaveResult(ctx, "prompt-requested");
}

X86_FUNC(__imp__sub_00107D40);
X86_FUNC(sub_00107D40) {
    // Kind 1 applies a campaign file; param2 != 0 is only a slot preview.
    // Settings loads (kind 0), previews and failed reads must not lift the
    // protection. This function is reached when the native loader applies data.
    const uint32_t card = game::pc_development::Memcard();
    const bool campaign_load = card && ctx.ecx == card &&
        kernel::R32(card + 0x6D4) == 1 && kernel::G<uint8_t>(ctx.esp + 8) == 0;
    __imp__sub_00107D40(ctx, base);
    if (campaign_load) game::pc_development::EndTestSession(ctx, "native campaign load");
}
X86_FUNC(__imp__sub_000E5B00);
X86_FUNC(sub_000E5B00) {
    // clashMemcard_ResetStoryMode: the player explicitly starts a fresh story.
    __imp__sub_000E5B00(ctx, base);
    game::pc_development::EndTestSession(ctx, "native story reset");
}

X86_FUNC(__imp__sub_0010F2C0);
X86_FUNC(sub_0010F2C0) {
    // This exact caller serializes nController0 for the original APT mission
    // menu. Do not alter valid native ports, player 2, or other getter callers.
    const bool original_menu = kernel::R32(ctx.esp) == 0xE27C0 && kernel::R32(ctx.esp + 4) == 0;
    __imp__sub_0010F2C0(ctx, base);
    const int32_t fallback = game::pc_development::ReturnMenuController();
    if (!original_menu || fallback < 0 || fallback > 1) return;
    const uint32_t original = ctx.eax;
    if (original >= 4) ctx.eax = uint32_t(fallback);
    static std::atomic<uint32_t> reports{0};
    const auto count = reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 12 || original >= 4)
        LOGR("development: original menu controller original=%d effective=%u fallback=%u",
             int32_t(original), ctx.eax, unsigned(original >= 4));
}
