#include "common/pc_survival.h"
#include "common/survival_wave.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace game::pc_survival {
namespace {
std::mutex g_lock;
View g_view;
bool g_recorded = false;
RecordSummary g_result_records;
std::atomic<bool> g_exit{false}, g_replay{false};
}
void Publish(View view) {
    std::lock_guard lock(g_lock);
    if (view.phase == Phase::Off || view.phase == Phase::Loading) {
        g_recorded = false;
        g_result_records = {};
    }
    if ((view.phase == Phase::Won || view.phase == Phase::Lost) && !g_recorded) {
        g_recorded = true;
        g_result_records = SaveRecord({view.venue, view.cooperative ? (view.ai_partner ? 1u : 2u) : 0u,
            view.start_wave, view.waves_cleared, view.total_defeated,
            view.deaths + (view.cooperative ? view.partner_deaths : 0u), view.elapsed_seconds, view.phase == Phase::Won, view.difficulty, view.endless});
    }
    view.records = g_result_records;
    g_view = std::move(view);
}
View ReadView() { std::lock_guard lock(g_lock); return g_view; }
void RequestExit() { g_exit.store(true); }
void RequestReplay() { g_replay.store(true); }
bool TakeExit() { return g_exit.exchange(false); }
bool TakeReplay() { return g_replay.exchange(false); }
void Draw() {
    View view;
    { std::lock_guard lock(g_lock); view = g_view; }
    if (view.phase == Phase::Off) return;
    if (view.native_text && view.phase != Phase::Failed) return;
    const auto* viewport = ImGui::GetMainViewport();
    const bool result = view.phase == Phase::Won || view.phase == Phase::Lost || view.phase == Phase::Failed;
    if (result) {
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::max(240.0f, std::min(580.0f, viewport->WorkSize.x - 24)), 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.97f);
        if (ImGui::Begin("Survival result", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::SetWindowFontScale(1.5f);
            ImGui::TextUnformatted(view.cooperative ?
                (view.phase == Phase::Won ? "SURVIVAL COOP COMPLETE" :
                 view.phase == Phase::Lost ? "SURVIVAL COOP DEFEAT" : "SURVIVAL COOP ERROR") :
                (view.phase == Phase::Won ? "SURVIVAL COMPLETE" :
                 view.phase == Phase::Lost ? "SURVIVAL DEFEAT" : "SURVIVAL ERROR"));
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Separator();
            if (view.start_wave > 1) ImGui::Text("Starting wave: %u (test)", view.start_wave);
            if (view.endless) {
                ImGui::Text("Endless - Waves cleared: %u", view.waves_cleared);
                ImGui::Text("Enemies defeated: %u", view.total_defeated);
            } else {
                ImGui::Text("Waves cleared: %u / %u", view.waves_cleared, view.waves_required);
                ImGui::Text("Enemies defeated: %u / %u", view.total_defeated, view.enemies_required);
            }
            ImGui::Text("Time: %s", SurvivalTime(view.elapsed_seconds).c_str());
            for (const auto& label : RecordLabels(view.records, view.endless))
                if (!label.empty()) ImGui::TextUnformatted(label.c_str());
            if (view.cooperative) {
                ImGui::Text("P1 kills: %u    Lives: %u    Deaths: %u", view.kills, view.lives, view.deaths);
                ImGui::Text("%s kills: %u    Lives: %u    Deaths: %u", view.ai_partner ? "AI" : "P2", view.partner_kills, view.partner_lives, view.partner_deaths);
            }
            if (view.cooperative && view.phase == Phase::Won && view.boss_wave)
                ImGui::Text("Boss reward: P1 +%u    P2 +%u lives", view.boss_bonus, view.partner_boss_bonus);
            else if (view.boss_bonus) ImGui::Text("Boss reward: +%u lives (now %u)", view.boss_bonus, view.lives);
            ImGui::TextWrapped("%s", view.message.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("Press Start to open the game pause menu and quit.");
        }
        ImGui::End();
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                 viewport->WorkPos.y + 12), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin(view.cooperative ? "Survival coop proof" : "Survival solo proof", nullptr, flags)) {
        ImGui::TextUnformatted(view.ai_partner ? "SURVIVAL SOLO + AI" : view.cooperative ? "SURVIVAL COOP" : "SURVIVAL - SOLO PROTOTYPE");
        if (view.endless) ImGui::Text("ENDLESS - Wave %u    Defeated %u / %u", view.wave, view.defeated, view.target);
        else ImGui::Text(view.boss_wave ? "Boss wave %u / %u    Defeated %u / %u" : "Wave %u / %u    Defeated %u / %u",
                         view.wave, Run::TotalWaves, view.defeated, view.target);
        if (view.cooperative) {
            ImGui::Text("P1 lives: %u    Deaths: %u", view.lives, view.deaths);
            ImGui::Text("%s lives: %u    Deaths: %u", view.ai_partner ? "AI" : "P2", view.partner_lives, view.partner_deaths);
        }
        if (view.phase == Phase::Intermission) {
            ImGui::TextColored(ImVec4(0.96f, 0.77f, 0.26f, 1), "WAVE CLEARED - Next wave in %u", view.seconds);
            if (view.cooperative && view.boss_wave)
                ImGui::Text("Boss reward: P1 +%u    P2 +%u lives", view.boss_bonus, view.partner_boss_bonus);
            else if (view.boss_bonus) ImGui::Text("Boss reward: +%u lives (now %u)", view.boss_bonus, view.lives);
        }
        if (view.phase == Phase::Respawning)
            ImGui::TextColored(ImVec4(0.96f, 0.77f, 0.26f, 1), "LIFE LOST - Respawn in %u", view.seconds);
        ImGui::TextUnformatted(view.message.c_str());
        ImGui::TextUnformatted("Campaign saving disabled. F10: Survival / Return to menu.");
    }
    ImGui::End();
}
bool DrawControls() {
    Phase phase;
    bool cooperative;
    { std::lock_guard lock(g_lock); phase = g_view.phase; cooperative = g_view.cooperative; }
    if (phase == Phase::Off) return false;
    ImGui::TextUnformatted(cooperative ? "SURVIVAL COOP" : "SURVIVAL - SOLO PROTOTYPE");
    ImGui::BeginDisabled(phase == Phase::Leaving);
    const bool leave = ImGui::Button("Return to main menu (Survival)");
    ImGui::EndDisabled();
    if (leave) g_exit.store(true);
    ImGui::Separator();
    return leave;
}
}  // namespace game::pc_survival
