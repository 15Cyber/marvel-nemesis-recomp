#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct X86Context;

namespace game::pc_development {
enum class Kind { Mission, TestLevel };
struct Destination {
    Kind kind = Kind::Mission;
    uint32_t index = 0;
    std::string name, level, scene;
    bool available = false;
    std::string unavailable_reason;
    bool operator==(const Destination&) const = default;
};
struct Catalog {
    std::vector<Destination> missions, tests;
    bool frontend = false;
    bool launch_ready = false;
};
struct LaunchRequest {
    Destination destination;
    uint32_t controller = 0; // Logical input port that confirmed Load.
};
// UI gets owned copies. No guest pointers or calls cross into the UI thread.
Catalog Read();
std::string Status();
bool Request(const Destination& destination, uint32_t controller = 0);
// Session policy, not a saved setting. A fresh native campaign load/new game
// clears it; returning from a test alone must not save its in-memory progress.
bool AutosaveBlocked();
void SetAutosaveBlocked(bool blocked); // Guest-thread lifecycle only.

// Guest-thread bridge, called by the existing APT integration after page restore.
void Publish(Catalog catalog);
std::optional<LaunchRequest> TakeRequest();
void Complete(const std::string& status);
void Tick(X86Context& ctx, bool frontend, bool can_launch);
}  // namespace game::pc_development
