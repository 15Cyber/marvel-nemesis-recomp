#include "common/pc_cheats.h"
#include "core/log.h"
#include <atomic>
#include <imgui.h>

namespace game::pc_cheats {
namespace {
std::atomic<uint32_t> g_flags{0}, g_refills{0};
std::atomic<Unlock> g_unlock{Unlock::None};
std::atomic<Result> g_result{Result::None};
std::array<std::atomic<uint32_t>, 2> g_actors{};
Unlock g_confirm = Unlock::None;  // UI thread / settings UiLock only.
}
Settings Read() {
    const auto bits = g_flags.load();
    Settings s;
    for (int p = 0; p < 2; ++p) {
        s.health[p] = (bits & (1u << p)) != 0;
        s.energy[p] = (bits & (1u << (p + 2))) != 0;
    }
    s.diagnostics = (bits & 16) != 0;
    return s;
}
void Set(const Settings& s) {
    uint32_t bits = s.diagnostics ? 16 : 0;
    for (int p = 0; p < 2; ++p) {
        if (s.health[p]) bits |= 1u << p;
        if (s.energy[p]) bits |= 1u << (p + 2);
    }
    if (g_flags.exchange(bits) != bits)
        LOGR("cheats: applied session settings health_p1=%u health_p2=%u energy_p1=%u energy_p2=%u diagnostics=%u",
             unsigned(s.health[0]), unsigned(s.health[1]), unsigned(s.energy[0]), unsigned(s.energy[1]), unsigned(s.diagnostics));
}
bool Enabled(int p, bool energy) {
    return p >= 0 && p < 2 && (g_flags.load() & (1u << (p + (energy ? 2 : 0)))) != 0;
}
bool Active() { return g_flags.load() != 0; }
const char* UnlockLabel(Unlock value) {
    switch (value) {
    case Unlock::All: return "Unlock all content";
    case Unlock::Characters: return "Unlock all characters";
    case Unlock::Arenas: return "Unlock all arenas";
    case Unlock::Cards: return "Unlock all cards";
    case Unlock::Comics: return "Unlock all comics";
    default: return "Unlock";
    }
}
bool RequestUnlock(Unlock value) {
    if (value < Unlock::All || value > Unlock::Comics) return false;
    // Publish status first, so the guest's completion cannot be overwritten by Queued.
    g_result = Result::Queued;
    auto expected = Unlock::None;
    return g_unlock.compare_exchange_strong(expected, value);
}
void RequestRefill(int player) {
    if (player < 0 || player > 1) return;
    g_result = Result::Queued;
    g_refills.fetch_or(1u << player);
}
Unlock TakeUnlock() { return g_unlock.exchange(Unlock::None); }
uint32_t TakeRefills() { return g_refills.exchange(0); }
void Complete(Result result) { g_result = result; }
void PublishPlayer(int p, uint32_t actor) { if (p >= 0 && p < 2) g_actors[p] = actor; }
bool PlayerPresent(int p) { return p >= 0 && p < 2 && g_actors[p].load() != 0; }
Result LastResult() { return g_result.load(); }
void CancelConfirmation() { g_confirm = Unlock::None; }
const char* Status() {
    switch (g_result.load()) {
    case Result::Queued: return "Request queued.";
    case Result::Unlocked: return "Unlocked. Reopen selection menus to refresh.";
    case Result::Refilled: return "Health and energy refilled.";
    case Result::NoPlayer: return "No living player available. Enter gameplay and try again.";
    default: return "Gameplay cheats are off at each launch.";
    }
}
void Draw(Settings& draft) {
    ImGui::TextWrapped("Experimental. Gameplay cheats apply to logical Player 1 / Player 2, shared by keyboard and controller. Apply changes below. They reset at launch.");
    for (int p = 0; p < 2; ++p) {
        ImGui::PushID(p);
        ImGui::Separator();
        ImGui::Text("Player %d", p + 1);
        ImGui::Checkbox("Infinite health", &draft.health[p]);
        ImGui::Checkbox("Infinite energy", &draft.energy[p]);
        if (ImGui::Button("Refill health and energy now")) RequestRefill(p);
        ImGui::PopID();
    }
    ImGui::TextWrapped("Health protects against normal damage; scripted defeats and ring-outs can still end a match. Energy does not include Rage.");
    if (ImGui::Button("Apply gameplay cheats")) Set(draft);
    if (ImGui::Button("Disable gameplay cheats now")) {
        draft = Settings{};
        Set(draft);
    }
    ImGui::Checkbox("Log player diagnostics", &draft.diagnostics);
    ImGui::Separator();
    ImGui::TextWrapped("Unlocks can be stored in your game save. They are immediate actions and cannot be undone here. Save normally in the game to keep them.");
    for (uint32_t i = uint32_t(Unlock::All); i <= uint32_t(Unlock::Comics); ++i) {
        const auto value = Unlock(i);
        if (ImGui::Button(UnlockLabel(value))) g_confirm = value;
    }
    if (g_confirm != Unlock::None) {
        ImGui::Separator();
        ImGui::TextUnformatted(UnlockLabel(g_confirm));
        ImGui::TextWrapped("This can change saved progress. It cannot be undone here.");
        if (ImGui::Button("Cancel unlock")) g_confirm = Unlock::None;
        ImGui::SameLine();
        if (ImGui::Button("Confirm unlock") && g_confirm != Unlock::None) {
            RequestUnlock(g_confirm); g_confirm = Unlock::None;
        }
    }
    ImGui::TextWrapped("%s", Status());
}
}  // namespace game::pc_cheats
