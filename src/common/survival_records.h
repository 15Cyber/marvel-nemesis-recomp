#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include "common/survival_arena.h"
#include "common/survival_difficulty.h"

namespace game::pc_survival {
enum class RecordStatus { None, Saved, TestRun, Unavailable };
struct RecordStats {
    uint64_t runs = 0, wins = 0, enemies = 0, deaths = 0;
    uint64_t best_waves = 0, best_enemies = 0, fastest_win_seconds = 0;
};
using RecordBook = std::array<std::array<RecordStats, 3>, Arenas.size()>;
// Missing files are an empty book; unreadable or invalid files return no snapshot.
std::optional<RecordBook> ReadRecords(unsigned difficulty = 0, bool endless = false);
struct RecordSummary {
    RecordStatus status = RecordStatus::None;
    RecordStats stats;
    bool new_best = false;
};
struct RecordAttempt {
    unsigned venue = 0, mode = 0, start_wave = 1; // Modes: solo, solo with AI, two humans.
    unsigned waves = 0, enemies = 0, deaths = 0, seconds = 0;
    bool won = false;
    unsigned difficulty = 0;
    bool endless = false;
};
// Guest-thread terminal results only; test starts never write the records file.
RecordSummary SaveRecord(const RecordAttempt& attempt);
std::string SurvivalTime(uint64_t seconds);
std::array<std::string, 2> RecordLabels(const RecordSummary& record, bool endless = false);
} // namespace game::pc_survival
