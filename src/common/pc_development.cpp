#include "common/pc_development.h"
#include "common/survival_wave.h"
#include "core/log.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace game::pc_development {
namespace {
std::mutex g_lock;
Catalog g_catalog;
std::optional<LaunchRequest> g_request;
std::string g_status = "EXPERIMENTAL; AUTOSAVE DISABLED IN DEVELOPMENT";
std::atomic<bool> g_autosave_blocked{false};
}
bool AutosaveBlocked() { return g_autosave_blocked.load(std::memory_order_relaxed); }
void SetAutosaveBlocked(bool blocked) { g_autosave_blocked.store(blocked, std::memory_order_relaxed); }
Catalog Read() { std::lock_guard lock(g_lock); return g_catalog; }
std::string Status() { std::lock_guard lock(g_lock); return g_status; }
void Publish(Catalog catalog) { std::lock_guard lock(g_lock); g_catalog = std::move(catalog); }
bool Request(const Destination& destination, uint32_t controller, unsigned initial_lives, unsigned start_wave, unsigned players, pc_survival::Party characters,
             unsigned difficulty, bool endless) {
    std::lock_guard lock(g_lock);
    if (!pc_survival::AllowedDifficulty(difficulty) || (!IsSurvival(destination.kind) && (difficulty || endless))) {
        g_status = "SURVIVAL FORMAT OR DIFFICULTY INVALID"; return false;
    }
    if (IsSurvival(destination.kind) && !pc_survival::Lives::Allowed(initial_lives)) {
        g_status = "SURVIVAL REQUIRES 1, 3 OR 5 STARTING LIVES";
        return false;
    }
    if (destination.kind == Kind::Survival && !pc_survival::Run::AllowedStart(start_wave)) {
        g_status = "SURVIVAL STARTING WAVE MUST BE BETWEEN 1 AND " + std::to_string(pc_survival::Run::TotalWaves);
        return false;
    }
    if ((players != 1 && players != 2) || (!IsSurvival(destination.kind) && players != 1)) {
        g_status = "SURVIVAL REQUIRES 1 OR 2 PLAYERS";
        return false;
    }
    if (IsSurvival(destination.kind) && !pc_survival::AllowedParty(characters)) {
        g_status = "SURVIVAL CHARACTER SELECTION INVALID"; return false;
    }
    const auto& list = destination.kind == Kind::Mission ? g_catalog.missions : g_catalog.tests;
    if (g_catalog.frontend && !g_catalog.launch_ready) {
        g_status = "RETURN TO MAIN MENU BEFORE USING DEVELOPMENT";
        return false;
    }
    if (controller > 1 || !g_catalog.frontend || !destination.available || g_request ||
        std::find(list.begin(), list.end(), destination) == list.end()) {
        g_status = "DESTINATION UNAVAILABLE; RETURN TO OPTIONS";
        return false;
    }
    g_request = LaunchRequest{destination, controller, initial_lives, start_wave, players, characters, difficulty, endless};
    g_status = "LOAD QUEUED";
    LOGR("development: queued kind=%u index=%u level=%s scene=%s controller=%u",
         unsigned(destination.kind), destination.index, destination.level.c_str(), destination.scene.c_str(), controller);
    if (IsSurvival(destination.kind))
        LOGR("development: survival starting_lives=%u start_wave=%u players=%u characters=%u,%u", initial_lives, start_wave, players, characters[0], characters[1]);
    return true;
}
std::optional<LaunchRequest> TakeRequest() {
    std::lock_guard lock(g_lock);
    auto request = std::move(g_request);
    g_request.reset();
    return request;
}
void Complete(const std::string& status) {
    std::lock_guard lock(g_lock);
    g_status = status;
    LOGR("development: %s", status.c_str());
}
}  // namespace game::pc_development
