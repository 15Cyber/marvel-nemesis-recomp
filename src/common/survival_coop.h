#pragma once
#include "common/survival_wave.h"

namespace game::pc_survival {
struct CoopLives {
    std::array<Lives, 2> players;
    std::array<unsigned, 2> boss_bonus{};

    constexpr bool Reset(unsigned count) {
        if (!Lives::Allowed(count)) return false;
        for (auto& player : players) player.Reset(count);
        boss_bonus = {};
        return true;
    }
    constexpr bool CanContinue() const { return players[0].remaining || players[1].remaining; }
    constexpr bool Pending() const { return players[0].pending || players[1].pending; }
    constexpr bool ReadyToFinish() const { return CanContinue() && !Pending(); }
    constexpr bool Clear(Run& run) {
        if (!ReadyToFinish() || !players[0].remaining || !players[1].remaining || !run.ClearWave()) return false;
        boss_bonus = {};
        if (run.BossWave()) for (unsigned slot = 0; slot < players.size(); ++slot)
            boss_bonus[slot] = players[slot].RewardBoss(DifficultyFor(run.difficulty).boss_lives);
        return true;
    }
    constexpr bool Advance(Run& run) {
        if (!ReadyToFinish() || !run.AdvanceWave()) return false;
        boss_bonus = {};
        return true;
    }
    constexpr void RecoverEliminated() {
        if (!ReadyToFinish()) return;
        for (auto& player : players) if (!player.remaining) {
            player.remaining = 1;
            player.pending = true;
        }
    }
};

static_assert([] {
    CoopLives team;
    if (!team.Reset(3) || !team.players[0].Die() || !team.players[1].Die()) return false;
    return team.CanContinue() && team.Pending() && !team.ReadyToFinish();
}());
static_assert([] {
    CoopLives team;
    if (!team.Reset(1) || !team.players[0].Die()) return false;
    return team.CanContinue() && !team.Pending() && team.ReadyToFinish() &&
        team.players[0].remaining == 0 && team.players[1].remaining == 1;
}());
static_assert([] {
    CoopLives team;
    if (!team.Reset(1) || !team.players[0].Die() || !team.players[1].Die()) return false;
    team.RecoverEliminated();
    return !team.CanContinue() && !team.Pending() && !team.ReadyToFinish();
}());
static_assert([] {
    CoopLives team;
    if (!team.Reset(3)) return false;
    for (unsigned n = 0; n < 3; ++n) {
        if (!team.players[0].Die()) return false;
        if (n < 2 && !team.players[0].Respawn()) return false;
    }
    if (!team.CanContinue() || team.players[0].remaining || team.players[1].remaining != 3) return false;
    team.RecoverEliminated();
    return team.players[0].remaining == 1 && team.players[0].pending && team.players[0].deaths == 3 &&
        team.players[1].remaining == 3 && !team.players[1].pending && team.Pending() && !team.ReadyToFinish();
}());
static_assert([] {
    CoopLives team;
    if (!team.Reset(5) || !team.players[0].Die() || team.Reset(2)) return false;
    return team.players[0].initial == 5 && team.players[0].remaining == 4 &&
        team.players[0].deaths == 1 && team.players[0].pending &&
        team.players[1].initial == 5 && team.players[1].remaining == 5 &&
        !team.players[1].deaths && !team.players[1].pending;
}());

// Native return must settle before a cleared wave can grant either player's boss reward.
static_assert([] {
    Run run;
    CoopLives team;
    if (!run.Start(5, 1) || !team.Reset(1) || !team.players[0].Die()) return false;
    for (uint32_t handle = 1; handle <= run.wave.target; ++handle)
        if (!run.wave.Add(handle) || !run.Defeat(handle)) return false;
    if (team.Clear(run) || run.cleared) return false;
    team.RecoverEliminated();
    if (team.players[0].remaining != 1 || !team.players[0].pending || team.Clear(run)) return false;
    if (!team.players[0].Respawn() || !team.Clear(run) || team.Clear(run)) return false;
    return team.players[0].remaining == 4 && team.players[1].remaining == 4 &&
        team.boss_bonus[0] == 3 && team.boss_bonus[1] == 3 && run.cleared == 1;
}());
static_assert([] {
    Run run;
    CoopLives team;
    if (!run.Start(10, 3) || !team.Reset(3)) return false;
    team.players[0].remaining = 8;
    team.players[1].remaining = 2;
    for (uint32_t handle = 1; handle <= run.wave.target; ++handle)
        if (!run.wave.Add(handle) || !run.Defeat(handle)) return false;
    if (!team.Clear(run) || team.boss_bonus[0] != 1 || team.boss_bonus[1] != 3 ||
        team.players[0].remaining != 9 || team.players[1].remaining != 5) return false;
    return team.Advance(run) && run.number == 11 && !team.boss_bonus[0] && !team.boss_bonus[1];
}());
static_assert([] {
    Run run;
    CoopLives team;
    if (!run.Start(1, 3) || !team.Reset(3)) return false;
    for (uint32_t handle = 1; handle <= run.wave.target; ++handle)
        if (!run.wave.Add(handle) || !run.Defeat(handle)) return false;
    if (!team.players[1].Die() || team.Clear(run) || run.cleared || !team.players[1].Respawn() ||
        !team.Clear(run) || !team.players[0].Die() || team.Advance(run)) return false;
    return team.players[0].Respawn() && team.Advance(run) &&
        team.players[0].remaining == 2 && team.players[1].remaining == 2 && run.number == 2;
}());
static_assert([] {
    Run run;
    CoopLives team;
    if (!run.Start(1, 1) || !team.Reset(1)) return false;
    for (uint32_t handle = 1; handle <= run.wave.target; ++handle)
        if (!run.wave.Add(handle) || !run.Defeat(handle)) return false;
    if (!team.players[0].Die() || !team.players[1].Die()) return false;
    team.RecoverEliminated();
    return !team.Clear(run) && !team.Advance(run) && !run.cleared &&
        !team.players[0].remaining && !team.players[1].remaining;
}());
constexpr bool CheckCoopProgression(unsigned first, unsigned initial) {
    Run run;
    CoopLives team;
    if (!run.Start(first, initial) || !team.Reset(initial) || team.Clear(run) || team.Advance(run)) return false;
    unsigned kills = 0, stock = initial;
    for (unsigned number = first; number <= Run::TotalWaves; ++number) {
        if (run.number != number || team.boss_bonus[0] || team.boss_bonus[1]) return false;
        for (unsigned i = 0; i < run.wave.target; ++i) {
            const auto handle = ++kills;
            if (!run.wave.Add(handle) || !run.Defeat(handle)) return false;
            if (i + 1 < run.wave.target && (team.Clear(run) || team.Advance(run))) return false;
        }
        const auto bonus = number % 5 == 0 ? (stock <= 6 ? 3u : 9u - stock) : 0u;
        stock += bonus;
        if (!team.Clear(run) || team.Clear(run) || team.boss_bonus[0] != bonus || team.boss_bonus[1] != bonus ||
            team.players[0].remaining != stock || team.players[1].remaining != stock ||
            run.total_defeated != kills || run.lives.remaining != initial) return false;
        if (number < Run::TotalWaves && !team.Advance(run)) return false;
    }
    return run.Complete() && !team.Advance(run) && kills == run.EnemiesRequired() &&
        run.cleared == run.WavesRequired() && !team.players[0].deaths && !team.players[1].deaths;
}
// Separate each suffix so the compiler's constexpr budget remains bounded.
static_assert(CheckCoopProgression(1, 3));
static_assert(CheckCoopProgression(2, 3));
static_assert(CheckCoopProgression(3, 3));
static_assert(CheckCoopProgression(4, 3));
static_assert(CheckCoopProgression(5, 3));
static_assert(CheckCoopProgression(6, 3));
static_assert(CheckCoopProgression(7, 3));
static_assert(CheckCoopProgression(8, 3));
static_assert(CheckCoopProgression(9, 3));
static_assert(CheckCoopProgression(10, 3));
static_assert(CheckCoopProgression(11, 3));
static_assert(CheckCoopProgression(12, 3));
static_assert(CheckCoopProgression(13, 3));
static_assert(CheckCoopProgression(14, 3));
static_assert(CheckCoopProgression(15, 3));
static_assert(CheckCoopProgression(16, 3));
static_assert(CheckCoopProgression(17, 3));
static_assert(CheckCoopProgression(18, 3));
static_assert(CheckCoopProgression(19, 3));
static_assert(CheckCoopProgression(20, 3));
static_assert(CheckCoopProgression(1, 1));
static_assert(CheckCoopProgression(1, 5));
}  // namespace game::pc_survival
