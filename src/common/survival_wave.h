#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include "common/survival_difficulty.h"

namespace game::pc_survival {
// Handles are identities, not retained guest pointers.
struct Wave {
    static constexpr unsigned Capacity = 26, MaxActive = 8;
    std::array<uint32_t, Capacity> handles{};
    std::array<bool, Capacity> dead{};
    unsigned target = 3, spawned = 0, defeated = 0;
    unsigned active_limit = 3;

    constexpr unsigned Alive() const { return spawned - defeated; }
    constexpr unsigned Pending() const { return target - spawned; }

    constexpr bool Add(uint32_t handle) {
        if (!handle || target > Capacity || !active_limit || active_limit > MaxActive ||
            spawned >= target || Alive() >= active_limit) return false;
        for (auto known : handles) if (known == handle) return false;
        handles[spawned++] = handle;
        return true;
    }
    constexpr bool Defeat(uint32_t handle) {
        for (unsigned i = 0; i < spawned; ++i) {
            if (handles[i] != handle || dead[i]) continue;
            dead[i] = true;
            ++defeated;
            return true;
        }
        return false;
    }
    constexpr bool Complete() const { return spawned == target && defeated == target; }
};

struct Lives {
    static constexpr unsigned MaxIcons = 9;
    unsigned initial = 3, remaining = 3, deaths = 0;
    bool pending = false;

    static constexpr bool Allowed(unsigned count) { return count == 1 || count == 3 || count == 5; }
    constexpr unsigned Icons(unsigned player) const {
        return player == 0 ? (remaining < MaxIcons ? remaining : MaxIcons) : 0;
    }
    constexpr bool Reset(unsigned count) {
        if (!Allowed(count)) return false;
        initial = remaining = count;
        deaths = 0;
        pending = false;
        return true;
    }
    constexpr bool Die() {
        if (!remaining || pending) return false;
        --remaining;
        ++deaths;
        pending = remaining != 0;
        return true;
    }
    constexpr bool Respawn() {
        if (!pending || !remaining) return false;
        pending = false;
        return true;
    }
    constexpr unsigned RewardBoss(unsigned amount = 3) {
        if (!remaining || pending) return 0; // A solo defeat or pending respawn cannot be rescued by a reward.
        const auto room = remaining < MaxIcons ? MaxIcons - remaining : 0u;
        const auto bonus = room < amount ? room : amount;
        remaining += bonus;
        return bonus;
    }
};

struct Run {
    enum class Enemy { Human1, Human3, Brute1, HeroBoss, Human2, Brute2, Brute3, Flyer1, FinalBoss, Flyer2, Flyer3 };
    static constexpr std::array<unsigned, 20> WaveSizes{3, 4, 5, 6, 3, 8, 9, 10, 12, 5, 12, 14, 16, 18, 7, 20, 22, 24, 26, 9};
    static constexpr unsigned TotalWaves = unsigned(WaveSizes.size());
    static constexpr unsigned TotalEnemies = [] {
        unsigned total = 0;
        for (auto size : WaveSizes) total += size;
        return total;
    }();
    Wave wave;
    Lives lives;
    unsigned number = 1, start_wave = 1, cleared = 0, total_defeated = 0, boss_bonus = 0;
    unsigned difficulty = 0;
    bool endless = false;

    static constexpr bool AllowedStart(unsigned first) { return first >= 1 && first <= TotalWaves; }
    constexpr unsigned WavesRequired() const { return endless ? 0u : TotalWaves - start_wave + 1; }
    constexpr unsigned EnemiesRequired() const {
        if (endless) return 0;
        unsigned total = 0;
        for (unsigned n = start_wave; n <= TotalWaves; ++n) total += WaveTarget(n);
        return total;
    }
    constexpr unsigned WaveTarget(unsigned n) const {
        const auto pattern = endless && n > TotalWaves ? 16u + (n - 21u) % 5u : n;
        const auto extra = endless && n > TotalWaves ? ((n - 1) / 20 > 3 ? 6u : 2u * ((n - 1) / 20)) : 0u;
        const auto target = WaveSizes[pattern - 1] + (n % 5 == 0 ? DifficultyFor(difficulty).boss_escorts : 0u) + extra;
        return target > Wave::Capacity ? Wave::Capacity : target;
    }
    constexpr bool Start(unsigned first, unsigned initial_lives, unsigned level = 0, bool infinite = false) {
        if (!AllowedStart(first) || !Lives::Allowed(initial_lives) || !AllowedDifficulty(level)) return false;
        *this = {};
        difficulty = level;
        endless = infinite;
        start_wave = number = first;
        lives.Reset(initial_lives);
        wave.target = WaveTarget(first);
        wave.active_limit = ActiveLimit();
        return true;
    }

    constexpr bool BossWave() const { return number % 5 == 0; }
    constexpr unsigned Pattern() const { return endless && number > TotalWaves ? 16u + (number - 21u) % 5u : number; }
    constexpr unsigned Pressure() const { return endless ? ((number - 1) / 20 > 3 ? 3u : (number - 1) / 20) : 0u; }
    constexpr bool FinalBossWave() const { return number == 20 || (endless && number % 20 == 0); }
    constexpr bool HeroWave() const { return number == 10 || number == 15 || (endless && number > 20 && BossWave() && !FinalBossWave()); }
    constexpr unsigned ReinforcementDelay() const {
        const auto base = DifficultyFor(difficulty).reinforcement_ms;
        const auto reduction = endless ? ((number - 1) / 20 > 6 ? 1500u : 250u * ((number - 1) / 20)) : 0u;
        return base > reduction + 500u ? base - reduction : 500u;
    }
    constexpr unsigned ActiveLimit(bool cooperative = false) const {
        const auto limit = (cooperative ? (number <= 5 ? 5u : number <= 10 ? 6u : 8u) :
                                         (number <= 5 ? 3u : number <= 10 ? 4u : 5u)) + DifficultyFor(difficulty).extra_active + Pressure();
        return limit > Wave::MaxActive ? Wave::MaxActive : limit;
    }
    constexpr unsigned InitialEnemies() const { return BossWave() ? 1u : 3u; }
    constexpr Enemy NextEnemy() const {
        // Extra boss escorts stay terrestrial; the approved two-drone quota never grows.
        if (endless && number > TotalWaves && BossWave()) {
            if (!wave.spawned) return FinalBossWave() ? Enemy::FinalBoss : Enemy::HeroBoss;
            return wave.spawned == 3 || wave.spawned == 6 ? Enemy::Flyer3 : Enemy::Brute1;
        }
        if (wave.spawned >= WaveSizes[Pattern() - 1]) return Enemy::Brute1;
        const auto enemy = BaseEnemy();
        const bool human = enemy == Enemy::Human1 || enemy == Enemy::Human2 || enemy == Enemy::Human3;
        if (difficulty == 2 && human) return wave.spawned % 2 ? Enemy::Brute1 : Enemy::Brute2;
        if (difficulty == 1 && enemy == Enemy::Human1) return number >= 3 ? Enemy::Brute1 : Enemy::Human3;
        if (difficulty == 1 && number >= 6 && human) return Enemy::Brute2;
        return enemy;
    }
    constexpr Enemy BaseEnemy() const {
        const auto index = wave.spawned;
        const auto n = Pattern();
        // Two flyer entries per selected wave bound aerial pressure without changing native actors.
        if (n >= 16) {
            if (n == 20) {
                if (!index) return Enemy::FinalBoss;
                return index % 2 ? Enemy::Brute1 : Enemy::Brute3;
            }
            if (n == 18) return index % 3 == 0 ? Enemy::Brute1 : index % 3 == 1 ? Enemy::Brute2 : Enemy::Brute3;
            if (index == 4 || index == 9) return Enemy::Flyer3;
            return index % 2 == 0 ? Enemy::Brute1 : Enemy::Brute3;
        }
        if (n == 15) {
            if (!index) return Enemy::HeroBoss;
            return index == 3 || index == 5 ? Enemy::Flyer3 : Enemy::Brute1;
        }
        if (n >= 11) {
            if (n == 14) return index % 3 == 0 ? Enemy::Brute1 : index % 3 == 1 ? Enemy::Brute2 : Enemy::Brute3;
            if (n != 11 && (index == 4 || index == 9)) return Enemy::Flyer2;
            return index % 2 == 0 ? Enemy::Brute1 : n == 12 ? Enemy::Brute2 : Enemy::Brute3;
        }
        if (n == 10) {
            if (!index) return Enemy::HeroBoss;
            return index % 2 == 0 ? Enemy::Flyer2 : Enemy::Brute1;
        }
        if (n == 9) {
            if (index < 2) return Enemy::Human3;
            return index % 2 == 0 ? Enemy::Brute1 : Enemy::Brute2;
        }
        if (n == 8) {
            if (index == 3 || index == 6) return Enemy::Flyer1;
            return index < 2 ? Enemy::Human2 : Enemy::Brute1;
        }
        if (n == 7) {
            if (index == 4 || index == 7) return Enemy::Flyer1;
            return index < 3 ? Enemy::Human3 : Enemy::Brute1;
        }
        if (n == 6) return index < 2 ? Enemy::Human1 : index < 5 ? Enemy::Brute1 : Enemy::Human3;
        if (n == 5) return index == 2 ? Enemy::Flyer1 : Enemy::Brute1;
        if (n == 4) return index < 2 ? Enemy::Human1 : index < 4 ? Enemy::Brute1 : Enemy::Human3;
        if (n == 3) return index < 3 ? Enemy::Human1 : index == 3 ? Enemy::Brute1 : Enemy::Human3;
        if (n == 2) return index < 3 ? Enemy::Human1 : Enemy::Human3;
        return Enemy::Human1;
    }

    constexpr bool Defeat(uint32_t handle) {
        if (!wave.Defeat(handle)) return false;
        if (total_defeated < std::numeric_limits<unsigned>::max()) ++total_defeated;
        return true;
    }
    // Shared wave ordering; solo and cooperative callers own their life checks and rewards.
    constexpr bool ClearWave() {
        if (!wave.Complete() || start_wave + cleared != number) return false;
        ++cleared;
        return true;
    }
    constexpr bool Clear() {
        if (!lives.remaining || lives.pending || !ClearWave()) return false;
        boss_bonus = BossWave() ? lives.RewardBoss(DifficultyFor(difficulty).boss_lives) : 0;
        return true;
    }
    constexpr bool Advance() {
        return lives.remaining && !lives.pending && AdvanceWave();
    }
    constexpr bool AdvanceWave() {
        if (number == std::numeric_limits<unsigned>::max() || start_wave + cleared != number + 1 || (!endless && number == TotalWaves)) return false;
        ++number;
        wave = {};
        wave.target = WaveTarget(number);
        wave.active_limit = ActiveLimit();
        boss_bonus = 0;
        return true;
    }
    constexpr bool Complete() const { return !endless && cleared == WavesRequired(); }
};

// No game or test runner is launched: the user's build checks these invariants.
static_assert([] {
    Wave wave;
    if (wave.Add(0) || wave.Defeat(0) || wave.Complete()) return false;
    if (!wave.Add(10) || wave.Add(10) || !wave.Defeat(10)) return false;
    if (wave.Defeat(10) || wave.Defeat(99) || wave.Complete()) return false;
    if (!wave.Add(20) || !wave.Add(30) || wave.Add(40)) return false;
    if (!wave.Defeat(30) || wave.Complete()) return false;
    return wave.Defeat(20) && wave.Complete() && wave.defeated == 3;
}());

static_assert([] {
    for (unsigned limit : std::array{3u, 4u, 5u, 6u, 8u}) {
        Wave wave;
        wave.target = Wave::Capacity;
        wave.active_limit = limit;
        uint32_t next = 1;
        for (unsigned i = 0; i < limit; ++i) if (!wave.Add(next++)) return false;
        if (wave.Add(99) || wave.Alive() != limit) return false;
        for (uint32_t handle = 1; handle <= Wave::Capacity; ++handle) {
            if (!wave.Defeat(handle) || wave.Defeat(handle) || wave.Add(handle)) return false;
            if (handle < Wave::Capacity && wave.Complete()) return false;
            if (wave.Pending() && !wave.Add(next++)) return false;
            if (wave.Alive() > limit) return false;
        }
        if (!wave.Complete() || wave.Pending() || wave.Alive() || wave.Add(99)) return false;
    }
    Wave invalid;
    invalid.active_limit = Wave::MaxActive + 1;
    return !invalid.Add(1);
}());

static_assert([] {
    Run run;
    for (unsigned number = 1; number <= Run::TotalWaves; ++number) {
        run.number = number;
        if (run.ActiveLimit() != (number <= 5 ? 3u : number <= 10 ? 4u : 5u) ||
            run.ActiveLimit(true) != (number <= 5 ? 5u : number <= 10 ? 6u : 8u) ||
            run.ActiveLimit(true) > Wave::MaxActive) return false;
    }
    return true;
}());

static_assert([] {
    Lives lives;
    if (lives.Reset(0) || lives.Reset(2) || lives.Reset(9)) return false;
    for (unsigned count : std::array{1u, 3u, 5u}) {
        if (!lives.Reset(count) || lives.remaining != count || lives.Icons(0) != count ||
            lives.Icons(1) || lives.Icons(2) || lives.Respawn()) return false;
        for (unsigned death = 1; death <= count; ++death) {
            if (!lives.Die() || lives.remaining != count - death || lives.Icons(0) != count - death ||
                lives.Icons(1) || lives.Die() || lives.deaths != death) return false;
            if (death < count) {
                if (!lives.pending || !lives.Respawn() || lives.Respawn()) return false;
            } else if (lives.pending || lives.Respawn()) return false;
        }
        if (lives.Die()) return false;
    }
    return lives.Reset(3) && lives.remaining == 3 && lives.deaths == 0 && !lives.pending;
}());

static_assert([] {
    Lives lives;
    lives.remaining = 20; // Never let a malformed count overrun the native nine-icon arrays.
    return lives.Icons(0) == Lives::MaxIcons && lives.Icons(1) == 0;
}());

static_assert([] {
    for (unsigned count = 0; count <= Lives::MaxIcons; ++count) {
        Lives lives;
        lives.remaining = count;
        const auto expected = !count ? 0u : count < 6 ? count + 3 : 9u;
        if (lives.RewardBoss() != expected - count || lives.remaining != expected ||
            lives.Icons(0) != expected || lives.deaths) return false;
        lives.pending = true;
        if (lives.RewardBoss() || lives.remaining != expected) return false;
    }
    return true;
}());

static_assert([] {
    Run run;
    for (uint32_t handle = 1; handle <= run.wave.target; ++handle)
        if (!run.wave.Add(handle) || !run.Defeat(handle)) return false;
    if (!run.lives.Die() || run.Clear() || run.Advance()) return false;
    if (!run.lives.Respawn() || !run.Clear() || !run.Advance()) return false;
    if (run.lives.remaining != 2 || run.lives.deaths != 1 || run.total_defeated != 3) return false;
    if (!run.lives.Die() || !run.lives.Respawn() || !run.lives.Die()) return false;
    for (uint32_t handle = 4; handle <= 7; ++handle)
        if (!run.wave.Add(handle) || !run.Defeat(handle)) return false;
    return !run.Clear() && !run.Advance() && !run.Complete();
}());

// Each wave gets a separate constant-expression budget, including its exact archetype mix.
constexpr bool CheckWaveComposition(unsigned number) {
    constexpr std::array<std::array<unsigned, 11>, 20> expected{{
        {3,0,0,0,0,0,0,0,0,0,0},
        {3,1,0,0,0,0,0,0,0,0,0},
        {3,1,1,0,0,0,0,0,0,0,0},
        {2,2,2,0,0,0,0,0,0,0,0},
        {0,0,2,0,0,0,0,1,0,0,0},
        {2,3,3,0,0,0,0,0,0,0,0},
        {0,3,4,0,0,0,0,2,0,0,0},
        {0,0,6,0,2,0,0,2,0,0,0},
        {0,2,5,0,0,5,0,0,0,0,0},
        {0,0,2,1,0,0,0,0,0,2,0},
        {0,0,6,0,0,0,6,0,0,0,0},
        {0,0,6,0,0,6,0,0,0,2,0},
        {0,0,7,0,0,0,7,0,0,2,0},
        {0,0,6,0,0,6,6,0,0,0,0},
        {0,0,4,1,0,0,0,0,0,0,2},
        {0,0,9,0,0,0,9,0,0,0,2},
        {0,0,10,0,0,0,10,0,0,0,2},
        {0,0,8,0,0,8,8,0,0,0,0},
        {0,0,12,0,0,0,12,0,0,0,2},
        {0,0,4,0,0,0,4,0,1,0,0}
    }};
    Run run;
    if (!run.Start(number, 3)) return false;
    const bool boss = number % 5 == 0;
    if (run.BossWave() != boss || run.InitialEnemies() != (boss ? 1u : 3u) ||
        run.wave.active_limit != (number <= 5 ? 3u : number <= 10 ? 4u : 5u)) return false;
    std::array<unsigned, 11> counts{};
    for (unsigned i = 0; i < run.wave.target; ++i) {
        ++counts[unsigned(run.NextEnemy())];
        if (!run.wave.Add(i + 1) || !run.Defeat(i + 1) || run.Defeat(i + 1)) return false;
        if (i + 1 < run.wave.target && (run.Clear() || run.Advance())) return false;
    }
    const auto flyers = counts[7] + counts[9] + counts[10];
    if (counts != expected[number - 1] || flyers > 2 || !run.Clear() || run.Clear()) return false;
    if ((number < 5 && flyers) || (number < 10 && counts[9]) || (number < 15 && counts[10])) return false;
    if (number >= 16 && (counts[0] || counts[1] || counts[4])) return false;
    return run.total_defeated == Run::WaveSizes[number - 1] && run.lives.remaining == (boss ? 6u : 3u);
}
static_assert(CheckWaveComposition(1));
static_assert(CheckWaveComposition(2));
static_assert(CheckWaveComposition(3));
static_assert(CheckWaveComposition(4));
static_assert(CheckWaveComposition(5));
static_assert(CheckWaveComposition(6));
static_assert(CheckWaveComposition(7));
static_assert(CheckWaveComposition(8));
static_assert(CheckWaveComposition(9));
static_assert(CheckWaveComposition(10));
static_assert(CheckWaveComposition(11));
static_assert(CheckWaveComposition(12));
static_assert(CheckWaveComposition(13));
static_assert(CheckWaveComposition(14));
static_assert(CheckWaveComposition(15));
static_assert(CheckWaveComposition(16));
static_assert(CheckWaveComposition(17));
static_assert(CheckWaveComposition(18));
static_assert(CheckWaveComposition(19));
static_assert(CheckWaveComposition(20));

static_assert([] {
    Run run;
    run.number = 5;
    run.cleared = 4;
    if (!run.lives.Reset(1) || !run.wave.Add(1) || !run.Defeat(1) || run.Clear()) return false;
    if (!run.wave.Add(2) || !run.Defeat(2) || run.Clear()) return false;
    if (!run.wave.Add(3) || !run.Defeat(3) || !run.lives.Die()) return false;
    return !run.Clear() && !run.Complete() && !run.boss_bonus && !run.lives.remaining;
}());
// Starting at a later wave earns only real kills and rewards in the selected suffix.
static_assert([] {
    Run invalid;
    if (invalid.Start(0, 3) || invalid.Start(21, 3) || invalid.Start(20, 2) ||
        invalid.number != 1 || invalid.start_wave != 1 || invalid.lives.remaining != 3) return false;
    return true;
}());

// Evaluate each start/lives pair separately; larger suffixes must not share Clang's constexpr step budget.
constexpr bool CheckStartingWave(unsigned first, unsigned initial) {
    constexpr std::array<unsigned, 20> targets{233,230,226,221,215,212,204,195,185,173,168,156,142,126,108,101,81,59,35,9};
    Run run;
    if (!run.Start(first, initial) || run.start_wave != first || run.number != first ||
        run.cleared || run.total_defeated || run.boss_bonus || run.lives.deaths || run.lives.pending ||
        run.lives.remaining != initial || run.EnemiesRequired() != targets[first - 1] ||
        run.Clear() || run.Advance() || run.Complete()) return false;
    unsigned kills = 0, waves = 0, stock = initial;
    for (unsigned n = first; n <= Run::TotalWaves; ++n) {
        if (run.number != n || run.wave.target != Run::WaveSizes[n - 1] ||
            run.wave.active_limit != (n <= 5 ? 3u : n <= 10 ? 4u : 5u) || run.boss_bonus) return false;
        for (unsigned i = 0; i < Run::WaveSizes[n - 1]; ++i) {
            const auto handle = ++kills;
            if (!run.wave.Add(handle) || !run.Defeat(handle)) return false;
            if (i + 1 < run.wave.target && (run.Clear() || run.Advance())) return false;
        }
        const auto bonus = n % 5 == 0 ? (stock <= 6 ? 3u : 9u - stock) : 0u;
        stock += bonus;
        if (!run.Clear() || run.Clear() || run.cleared != ++waves || run.total_defeated != kills ||
            run.boss_bonus != bonus || run.lives.remaining != stock) return false;
        if (n < Run::TotalWaves && !run.Advance()) return false;
    }
    return run.Complete() && !run.Advance() && kills == targets[first - 1] &&
        waves == run.WavesRequired() && !run.lives.deaths;
}
static_assert(Run::TotalWaves == 20, "Extend the starting-wave assertions when adding waves.");
static_assert(Run::TotalEnemies == 233);
static_assert(CheckStartingWave(1, 1));
static_assert(CheckStartingWave(1, 3));
static_assert(CheckStartingWave(1, 5));
static_assert(CheckStartingWave(2, 1));
static_assert(CheckStartingWave(2, 3));
static_assert(CheckStartingWave(2, 5));
static_assert(CheckStartingWave(3, 1));
static_assert(CheckStartingWave(3, 3));
static_assert(CheckStartingWave(3, 5));
static_assert(CheckStartingWave(4, 1));
static_assert(CheckStartingWave(4, 3));
static_assert(CheckStartingWave(4, 5));
static_assert(CheckStartingWave(5, 1));
static_assert(CheckStartingWave(5, 3));
static_assert(CheckStartingWave(5, 5));
static_assert(CheckStartingWave(6, 1));
static_assert(CheckStartingWave(6, 3));
static_assert(CheckStartingWave(6, 5));
static_assert(CheckStartingWave(7, 1));
static_assert(CheckStartingWave(7, 3));
static_assert(CheckStartingWave(7, 5));
static_assert(CheckStartingWave(8, 1));
static_assert(CheckStartingWave(8, 3));
static_assert(CheckStartingWave(8, 5));
static_assert(CheckStartingWave(9, 1));
static_assert(CheckStartingWave(9, 3));
static_assert(CheckStartingWave(9, 5));
static_assert(CheckStartingWave(10, 1));
static_assert(CheckStartingWave(10, 3));
static_assert(CheckStartingWave(10, 5));
static_assert(CheckStartingWave(11, 1));
static_assert(CheckStartingWave(11, 3));
static_assert(CheckStartingWave(11, 5));
static_assert(CheckStartingWave(12, 1));
static_assert(CheckStartingWave(12, 3));
static_assert(CheckStartingWave(12, 5));
static_assert(CheckStartingWave(13, 1));
static_assert(CheckStartingWave(13, 3));
static_assert(CheckStartingWave(13, 5));
static_assert(CheckStartingWave(14, 1));
static_assert(CheckStartingWave(14, 3));
static_assert(CheckStartingWave(14, 5));
static_assert(CheckStartingWave(15, 1));
static_assert(CheckStartingWave(15, 3));
static_assert(CheckStartingWave(15, 5));
static_assert(CheckStartingWave(16, 1));
static_assert(CheckStartingWave(16, 3));
static_assert(CheckStartingWave(16, 5));
static_assert(CheckStartingWave(17, 1));
static_assert(CheckStartingWave(17, 3));
static_assert(CheckStartingWave(17, 5));
static_assert(CheckStartingWave(18, 1));
static_assert(CheckStartingWave(18, 3));
static_assert(CheckStartingWave(18, 5));
static_assert(CheckStartingWave(19, 1));
static_assert(CheckStartingWave(19, 3));
static_assert(CheckStartingWave(19, 5));
static_assert(CheckStartingWave(20, 1));
static_assert(CheckStartingWave(20, 3));
static_assert(CheckStartingWave(20, 5));

// A full wave must refill real free slots, including five live actors and twenty-six unique defeats.
static_assert([] {
    Run run;
    if (!run.Start(16, 3)) return false;
    uint32_t next = 1;
    for (unsigned number = 16; number <= Run::TotalWaves; ++number) {
        uint32_t oldest = next;
        while (!run.wave.Complete()) {
            while (run.wave.Pending() && run.wave.Alive() < run.wave.active_limit)
                if (!run.wave.Add(next++)) return false;
            if (run.Clear() || run.Advance() || run.wave.Alive() > 5) return false;
            if (run.wave.Pending() && (run.wave.Alive() != 5 || run.wave.Add(next))) return false;
            if (!run.Defeat(oldest++)) return false;
        }
        if (!run.Clear() || run.Clear()) return false;
        if (number < Run::TotalWaves && !run.Advance()) return false;
    }
    return run.Complete() && run.total_defeated == 101 && run.lives.remaining == 6;
}());

// Pending respawn and the last solo life cannot claim the new boss reward.
static_assert([] {
    for (unsigned initial : std::array{1u, 3u}) {
        Run run;
        if (!run.Start(20, initial)) return false;
        for (uint32_t handle = 1; handle <= run.wave.target; ++handle)
            if (!run.wave.Add(handle) || !run.Defeat(handle)) return false;
        if (!run.lives.Die() || run.Clear() || run.Complete() || run.boss_bonus) return false;
        if (initial == 1) {
            if (run.lives.Respawn() || run.Advance()) return false;
        } else {
            if (!run.lives.Respawn() || !run.Clear() || run.Clear() ||
                !run.Complete() || run.boss_bonus != 3 || run.lives.remaining != 5) return false;
        }
    }
    return true;
}());
}  // namespace game::pc_survival
