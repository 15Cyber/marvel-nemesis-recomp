#include "common/pc_development.h"
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
bool Request(const Destination& destination, uint32_t controller) {
    std::lock_guard lock(g_lock);
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
    g_request = LaunchRequest{destination, controller};
    g_status = "LOAD QUEUED";
    LOGR("development: queued kind=%u index=%u level=%s scene=%s controller=%u",
         unsigned(destination.kind), destination.index, destination.level.c_str(), destination.scene.c_str(), controller);
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
