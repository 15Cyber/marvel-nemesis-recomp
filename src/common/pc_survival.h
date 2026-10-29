#pragma once
#include "common/survival_arena.h"
#include "common/survival_records.h"
#include <array>
#include <cstdint>
#include <string>

struct X86Context;

namespace game::pc_survival {
struct Character { const char* label; const char* actor; unsigned sound_bank; };
inline constexpr std::array<Character, 6> Characters{{
    {"WOLVERINE", "plr_Wolverine", 0}, {"SPIDER-MAN (ALT)", "plr_altSpiderMan", 5},
    {"HUMAN TORCH (ALT)", "plr_altHumanTorch", 6}, {"ELEKTRA (ALT)", "plr_altElektra", 12},
    {"VENOM (ALT)", "plr_altVenom", 13}, {"THING (ALT)", "plr_altThing", 14}
}};
using Party = std::array<unsigned, 2>;
inline constexpr Party DefaultParty{0, 1};
constexpr bool AllowedParty(Party party) { return party[0] < Characters.size() && party[1] < Characters.size(); }
constexpr bool ManagedAudio(bool cooperative, Party party) { return cooperative || party[0] != DefaultParty[0]; }
static_assert(AllowedParty(DefaultParty) && AllowedParty({5, 5}) && !AllowedParty({6, 0}) && !AllowedParty({0, 6}));
static_assert(!ManagedAudio(false, DefaultParty) && ManagedAudio(true, DefaultParty) && ManagedAudio(false, {1, 0}));
enum class Phase { Off, Loading, Fighting, Won, Lost, Failed, Leaving, Intermission, Respawning };
struct View {
    Phase phase = Phase::Off;
    unsigned defeated = 0;
    std::string message;
    unsigned wave = 1, waves_cleared = 0, total_defeated = 0, seconds = 0;
    unsigned start_wave = 1, waves_required = 0, enemies_required = 0;
    unsigned venue = DefaultArena, elapsed_seconds = 0;
    unsigned difficulty = 0;
    bool endless = false;
    RecordSummary records;
    unsigned target = 3;
    unsigned kills = 0, partner_kills = 0, shared_kills = 0;
    unsigned lives = 3, initial_lives = 3, deaths = 0;
    unsigned boss_bonus = 0;
    bool boss_wave = false;
    bool native_text = false;
    bool cooperative = false;
    bool ai_partner = false;
    unsigned partner_lives = 0, partner_deaths = 0, partner_boss_bonus = 0;
};
constexpr unsigned SecondsLeft(int64_t milliseconds) {
    return milliseconds > 0 ? unsigned((milliseconds + 999) / 1000) : 0;
}
static_assert(SecondsLeft(-1) == 0 && SecondsLeft(0) == 0 && SecondsLeft(1) == 1 &&
              SecondsLeft(999) == 1 && SecondsLeft(1000) == 1 && SecondsLeft(1001) == 2 && SecondsLeft(5000) == 5);
// Only owned host data crosses the guest/UI boundary.
void Publish(View view);
View ReadView();
void RequestExit();
void RequestReplay();
bool TakeExit();
bool TakeReplay();
struct ReplaySettings {
    std::array<unsigned, 2> characters{}, costumes{}, lives{3, 3}, controllers{0, 1};
    unsigned venue = DefaultArena, start_wave = 1;
    unsigned difficulty = 0;
    bool endless = false;
    bool cooperative = false;
    bool ai_partner = false;
};
void Draw();
bool DrawControls(); // Embedded in F10; true asks the panel to close.

bool Prepare(X86Context& ctx, unsigned initial_lives, std::string& error, unsigned start_wave = 1,
             bool cooperative = false, Party characters = DefaultParty,
             unsigned venue = DefaultArena, unsigned difficulty = 0, bool endless = false); // guest thread, after launch checks
// Native Versus UI IDs/costumes; inactive P2 fields are ignored in solo.
bool PrepareNative(X86Context& ctx, std::array<unsigned, 2> char_ids, std::array<unsigned, 2> costumes,
                   std::array<unsigned, 2> lives, bool cooperative, unsigned venue, std::string& error, unsigned start_wave = 1, bool ai_partner = false,
                   unsigned difficulty = 0, bool endless = false);
bool Protected(uint32_t actor); // Guest thread only, shared with the existing damage hook.
bool FriendlyDamage(X86Context& ctx, uint32_t victim, uint32_t source); // Cooperative participants only.
void Tick(X86Context& ctx);
}  // namespace game::pc_survival
