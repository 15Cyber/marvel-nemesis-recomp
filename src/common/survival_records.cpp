#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "common/survival_records.h"
#include "common/survival_arena.h"
#include "common/survival_wave.h"
#include "app/app.h"
#include "core/log.h"
#include <algorithm>
#include <fstream>
#include <utility>
#include <limits>
#include <toml++/toml.hpp>

namespace game::pc_survival {
namespace {
constexpr std::array<const char*, 3> kModes{"solo", "solo_ai", "coop"};
static_assert(RecordBook{}[0].size() == kModes.size());
constexpr uint64_t kCounterLimit = uint64_t(std::numeric_limits<int64_t>::max());

uint64_t AddRecordCounter(uint64_t value, uint64_t amount) {
    return amount > kCounterLimit - value ? kCounterLimit : value + amount;
}
RecordStats UpdatedRecord(RecordStats stats, const RecordAttempt& attempt) {
    stats.runs = AddRecordCounter(stats.runs, 1);
    stats.wins = AddRecordCounter(stats.wins, attempt.won ? 1 : 0);
    stats.enemies = AddRecordCounter(stats.enemies, attempt.enemies);
    stats.deaths = AddRecordCounter(stats.deaths, attempt.deaths);
    stats.best_waves = std::max(stats.best_waves, uint64_t(attempt.waves));
    stats.best_enemies = std::max(stats.best_enemies, uint64_t(attempt.enemies));
    if (attempt.won && (!stats.fastest_win_seconds || attempt.seconds < stats.fastest_win_seconds))
        stats.fastest_win_seconds = attempt.seconds;
    return stats;
}
bool ReadCounter(const toml::table& row, const char* key, uint64_t& value) {
    const auto number = row[key].as_integer();
    if (!number || number->get() < 0) return false;
    value = uint64_t(number->get());
    return true;
}
bool LoadRecords(const std::filesystem::path& target, RecordBook& book, bool endless = false) {
    std::error_code error;
    const bool exists = std::filesystem::exists(target, error);
    if (error) return false;
    if (!exists) return true;
    if (std::filesystem::file_size(target) > 65536) return false;
    std::ifstream input(target, std::ios::binary);
    if (!input) return false;
    const auto table = toml::parse(input);
    const auto version = table["version"].as_integer();
    if (!version || version->get() != 1) return false;
    const auto rows = table["records"].as_array();
    if (!rows || rows->size() > Arenas.size() * kModes.size()) return false;
    std::array<std::array<bool, kModes.size()>, Arenas.size()> seen{};
    for (const auto& item : *rows) {
        const auto row = item.as_table();
        if (!row) return false;
        const auto arena = (*row)["arena"].value<std::string>(), mode = (*row)["mode"].value<std::string>();
        if (!arena || !mode) return false;
        unsigned venue = 0, kind = 0;
        while (venue < Arenas.size() && *arena != Arenas[venue].key) ++venue;
        while (kind < kModes.size() && *mode != kModes[kind]) ++kind;
        if (venue == Arenas.size() || kind == kModes.size() || seen[venue][kind]) return false;
        seen[venue][kind] = true;
        auto& stats = book[venue][kind];
        if (!ReadCounter(*row, "runs", stats.runs) || !ReadCounter(*row, "wins", stats.wins) ||
            !ReadCounter(*row, "enemies", stats.enemies) || !ReadCounter(*row, "deaths", stats.deaths) ||
            !ReadCounter(*row, "best_waves", stats.best_waves) || !ReadCounter(*row, "best_enemies", stats.best_enemies) ||
            !ReadCounter(*row, "fastest_win_seconds", stats.fastest_win_seconds)) return false;
        if (!stats.runs || stats.wins > stats.runs || stats.best_waves > (endless ? std::numeric_limits<unsigned>::max() : Run::TotalWaves) ||
            (endless && stats.wins) ||
            stats.best_enemies > stats.enemies || stats.fastest_win_seconds > std::numeric_limits<unsigned>::max() ||
            (stats.wins == 0) != (stats.fastest_win_seconds == 0)) return false;
    }
    return !input.bad();
}
bool WriteRecords(const std::filesystem::path& target, const RecordBook& book) {
    toml::array rows;
    for (unsigned venue = 0; venue < Arenas.size(); ++venue)
        for (unsigned mode = 0; mode < kModes.size(); ++mode) {
            const auto& stats = book[venue][mode];
            if (!stats.runs) continue;
            rows.push_back(toml::table{
                {"arena", Arenas[venue].key}, {"mode", kModes[mode]},
                {"runs", int64_t(stats.runs)}, {"wins", int64_t(stats.wins)},
                {"enemies", int64_t(stats.enemies)}, {"deaths", int64_t(stats.deaths)},
                {"best_waves", int64_t(stats.best_waves)}, {"best_enemies", int64_t(stats.best_enemies)},
                {"fastest_win_seconds", int64_t(stats.fastest_win_seconds)}
            });
        }
    auto temporary = target;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << toml::table{{"version", int64_t(1)}, {"records", std::move(rows)}} << "\n";
        output.flush();
        bool ready = bool(output);
        output.close();
        ready = ready && !output.fail();
        if (ready && MoveFileExW(temporary.c_str(), target.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    } catch (...) {
        // Match the existing settings writer: a failed write never replaces the previous file.
    }
    DeleteFileW(temporary.c_str());
    return false;
}
} // namespace

std::optional<RecordBook> ReadRecords(unsigned difficulty, bool endless) {
    if (!AllowedDifficulty(difficulty)) return std::nullopt;
    try {
        RecordBook book{};
        if (LoadRecords(app::ExeDir() / RecordsFile(difficulty, endless), book, endless)) return book;
    } catch (...) {
        // A browser read must not create, repair or replace the records file.
    }
    LOGE("survival-records: browser cannot read records; existing file preserved");
    return std::nullopt;
}

RecordSummary SaveRecord(const RecordAttempt& attempt) {
    if (attempt.start_wave != 1) return {RecordStatus::TestRun};
    RecordSummary result{RecordStatus::Unavailable};
    if (!AllowedDifficulty(attempt.difficulty) || attempt.venue >= Arenas.size() || attempt.mode >= kModes.size() ||
        (!attempt.endless && attempt.waves > Run::TotalWaves) || (attempt.endless && attempt.won) ||
        (attempt.won && (attempt.waves != Run::TotalWaves || !attempt.seconds))) return result;
    try {
        const auto target = app::ExeDir() / RecordsFile(attempt.difficulty, attempt.endless);
        RecordBook book{};
        if (!LoadRecords(target, book, attempt.endless)) {
            LOGE("survival-records: cannot read records; existing file preserved");
            return result;
        }
        const auto previous = book[attempt.venue][attempt.mode];
        const auto updated = UpdatedRecord(previous, attempt);
        book[attempt.venue][attempt.mode] = updated;
        if (!WriteRecords(target, book)) {
            LOGE("survival-records: cannot save records; existing file preserved");
            return result;
        }
        result.status = RecordStatus::Saved;
        result.stats = updated;
        result.new_best = previous.runs == 0 || updated.best_waves > previous.best_waves ||
            updated.best_enemies > previous.best_enemies ||
            (updated.fastest_win_seconds && (!previous.fastest_win_seconds ||
                updated.fastest_win_seconds < previous.fastest_win_seconds));
        LOGR("survival-records: saved arena=%s mode=%s difficulty=%s format=%s runs=%llu wins=%llu waves=%llu enemies=%llu time=%llu",
             Arenas[attempt.venue].key, kModes[attempt.mode], DifficultyFor(attempt.difficulty).key, attempt.endless ? "endless" : "classic",
             static_cast<unsigned long long>(updated.runs), static_cast<unsigned long long>(updated.wins),
             static_cast<unsigned long long>(updated.best_waves), static_cast<unsigned long long>(updated.best_enemies),
             static_cast<unsigned long long>(updated.fastest_win_seconds));
    } catch (...) {
        LOGE("survival-records: file error; existing records preserved");
    }
    return result;
}
std::string SurvivalTime(uint64_t seconds) {
    const auto remainder = seconds % 60;
    return std::to_string(seconds / 60) + (remainder < 10 ? ":0" : ":") + std::to_string(remainder);
}
std::array<std::string, 2> RecordLabels(const RecordSummary& record, bool endless) {
    if (record.status == RecordStatus::None) return {"", ""};
    if (record.status == RecordStatus::TestRun) return {"TEST START - RECORDS NOT SAVED", ""};
    if (record.status != RecordStatus::Saved) return {"LOCAL RECORDS UNAVAILABLE", ""};
    const auto& stats = record.stats;
    return {
        std::string(record.new_best ? "NEW BEST: " : "BEST: ") + "W" + std::to_string(stats.best_waves) +
            "  K" + std::to_string(stats.best_enemies) + (endless ? "  ENDLESS" : "  WIN " +
            (stats.fastest_win_seconds ? SurvivalTime(stats.fastest_win_seconds) : "--:--")),
        "RUNS: " + std::to_string(stats.runs) + "  WINS: " + std::to_string(stats.wins) +
            "  KILLS: " + std::to_string(stats.enemies) + "  DEATHS: " + std::to_string(stats.deaths)
    };
}
} // namespace game::pc_survival
