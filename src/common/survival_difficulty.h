#pragma once
#include <array>

namespace game::pc_survival {
struct Difficulty {
    const char* key;
    const char* label;
    const char* records_file;
    unsigned reinforcement_ms, extra_active, boss_escorts, boss_lives;
};
inline constexpr std::array<Difficulty, 3> Difficulties{{
    {"standard", "STANDARD", "survival_records.toml", 2000, 0, 0, 3},
    {"hard", "HARD", "survival_records_hard.toml", 1400, 1, 2, 1},
    {"extreme", "EXTREME", "survival_records_extreme.toml", 900, 2, 4, 0}
}};
constexpr bool AllowedDifficulty(unsigned difficulty) { return difficulty < Difficulties.size(); }
constexpr const Difficulty& DifficultyFor(unsigned difficulty) {
    return Difficulties[AllowedDifficulty(difficulty) ? difficulty : 0];
}
inline constexpr std::array<const char*, 3> EndlessRecordFiles{
    "survival_records_endless.toml", "survival_records_endless_hard.toml", "survival_records_endless_extreme.toml"};
constexpr const char* RecordsFile(unsigned difficulty, bool endless) {
    return endless ? EndlessRecordFiles[AllowedDifficulty(difficulty) ? difficulty : 0] : DifficultyFor(difficulty).records_file;
}
} // namespace game::pc_survival
