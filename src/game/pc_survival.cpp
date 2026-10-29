// Opt-in solo proof for the SHA-1-pinned retail XBE. Original disc data stay read-only.
#include "common/pc_survival.h"
#include "common/survival_coop.h"
#include "common/pc_development.h"
#include "common/settings_panel.h"
#include "common/survival_wave.h"
#include "core/log.h"
#include "xdk/hle.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace game::pc_survival {
namespace {
constexpr uint32_t kWorld = 0x4C33F0, kManager = 0x4F3B30, kPlayer = 0x4F3B40;
constexpr uint32_t kCount = 0x4F40C0, kMemcard = 0x5155E8;
constexpr uint32_t kNativeNis = 0x4DA200;
constexpr uint32_t kPartner = kPlayer + 0xB0, kPartnerName = 448, kSharedCamera = 0x4F416C;
const Arena* g_arena = &Arenas[DefaultArena];
constexpr std::array<uint32_t, 3> kCampaignScalars{0x4F0718, 0x4F0B94, 0x4F0A7C};
constexpr std::array<const char*, 31> kSoundBanks{
    "SFX_Wolv1",
    "SFX_Peon1",
    "SFX_Peonhuman1",
    "SFX_Peonhuman3",
    "SFX_PeonBrute1",
    "SFX_Spidey",
    "SFX_Torch",
    "SFX_Peonhuman2",
    "SFX_PeonBrute2",
    "SFX_PeonBrute3",
    "SFX_PeonFlyer1",
    "SFX_Roekel",
    "SFX_Elektra",
    "SFX_Venom",
    "SFX_Thing",
    "SFX_PeonFlyer2",
    "SFX_PeonFlyer3",
    "SFX_ambiance_mansion",
    "AvengersMansion_MP",
    "AvengersMansion_MP_amb",
    "SFX_Wink",
    "SFX_Ohm",
    "SFX_Devil",
    "SFX_Storm",
    "SFX_AfterShk",
    "SFX_Heatwave",
    "SFX_Iron2",
    "SFX_Brigade",
    "SFX_Magneto",
    "SFX_Hazmat",
    "SFX_DefCon"};
const char* SoundBankName(unsigned index, const Arena& arena) {
    return index >= 17 && index <= 19 ? arena.sound_banks[index - 17] : kSoundBanks[index];
}
using SelectedParty = std::array<Character, 2>;
constexpr SelectedParty kDefaultSelection{Characters[0], Characters[1]};
struct NativeCharacter { unsigned internal; std::array<Character, 2> costumes; };
// Original GetCharList order; UI IDs differ from actor, AI and sound IDs.
constexpr std::array<NativeCharacter, 18> kNativeCharacters{{
    {10, {{{"Thing", "plr_thing", 14}, {"Thing", "plr_altThing", 14}}}},
    {2, {{{"Wolverine", "plr_wolverine", 0}, {"Wolverine", "plr_altWolverine", 0}}}},
    {15, {{{"The Wink", "plr_paradox", 20}, {"The Wink", "plr_altParadox", 20}}}},
    {6, {{{"Elektra", "plr_elektra", 12}, {"Elektra", "plr_altElektra", 12}}}},
    {14, {{{"Johnny Ohm", "plr_johnnyohm", 21}, {"Johnny Ohm", "plr_altJohnnyohm", 21}}}},
    {3, {{{"Spider-Man", "plr_spiderman", 5}, {"Spider-Man", "plr_altSpiderman", 5}}}},
    {9, {{{"Daredevil", "plr_daredevil", 22}, {"Daredevil", "plr_altDaredevil", 22}}}},
    {7, {{{"Storm", "plr_storm", 23}, {"Storm", "plr_altStorm", 23}}}},
    {13, {{{"Fault Zone", "plr_aftershock", 24}, {"Fault Zone", "plr_altAftershock", 24}}}},
    {4, {{{"Venom", "plr_venom", 13}, {"Venom", "plr_altVenom", 13}}}},
    {16, {{{"Solara", "plr_heatwave", 25}, {"Solara", "plr_altHeatwave", 25}}}},
    {8, {{{"Human Torch", "plr_humantorch", 6}, {"Human Torch", "plr_altHumantorch", 6}}}},
    {1, {{{"Iron Man", "plr_ironman", 26}, {"Iron Man", "plr_altIronman", 26}}}},
    {17, {{{"Brigade", "plr_megalith", 27}, {"Brigade", "plr_altMegalith", 27}}}},
    {5, {{{"Magneto", "plr_magneto", 28}, {"Magneto", "plr_altMagneto", 28}}}},
    {12, {{{"HazMat", "plr_biohazard", 29}, {"HazMat", "plr_altBiohazard", 29}}}},
    {11, {{{"Paragon", "plr_defcon", 30}, {"Paragon", "plr_altDefcon", 30}}}},
    {18, {{{"Roekel", "plr_roekel", 11}, {"Roekel", "plr_altRoekel", 11}}}}
}};
static_assert([] {
    for (const auto& hero : kNativeCharacters) for (const auto& costume : hero.costumes) {
        unsigned length = 0;
        while (length < 32 && costume.actor[length]) ++length;
        if (!length || length == 32 || costume.sound_bank >= kSoundBanks.size()) return false;
    }
    return true;
}());
constexpr Lives InitialStock(unsigned count) {
    Lives lives;
    lives.initial = lives.remaining = count;
    return lives;
}
static_assert([] {
    auto first = InitialStock(2), second = InitialStock(7);
    if (!first.Die() || first.remaining != 1 || !first.Respawn() || second.remaining != 7) return false;
    return first.RewardBoss() == 3 && first.remaining == 4 && second.RewardBoss() == 2 &&
        second.remaining == 9 && InitialStock(9).remaining == 9 && !InitialStock(4).pending;
}());

constexpr const char* kEnemyAnimations[]{
    "Humanoid_Stand", "Humanoid_Run", "Humanoid_BladeWalk", "Humanoid_Jump2", "Humanoid_BladeAttack1", "Humanoid_Create",
    "Spiderman_walk", "Humanoid_GunRangedAttack", "Humanoid_GunAttack1",
    "Wolverine_ThrowMale", "Male_WolvThrowVictim",
    "Brute_SharedWalk", "Brute_BladeRun", "Brute_SharedStand", "Brute_BladeJump", "Brute_bladeAttack1", "Brute_Create",
    "Spiderman_Stand", "Spiderman_Run", "Spiderman_Attack1", "Spiderman_HugeReactFloorRec",
    "Spiderman_ThrowMale", "Male_SpiderThrowVictim2",
    "Johnnyohm_walk", "Torch_Stand", "Torch_Attack1",
    "Female_WolvThrowVictim", "Brute_WolvThrowVictim",
    "Elektra_stand", "Elektra_Walk", "Elektra_Run2", "Elektra_Attack1", "Elektra_ThrowLikeDefcon", "Male_defconThrowVictim",
    "Venom_Stand", "Venom_Walk", "Venom_Run2", "Venom_Attack1", "Venom_Throw", "Male_VenomThrowVictim",
    "Thing_Stand", "Thing_Walk2", "Thing_Run", "Thing_Attack1", "Thing_ThrowMale", "Male_ThingThrowVictim",
    "Humanoid_Walk", "Humanoid_GrapplerRun", "Humanoid_GrapplerStand",
    "Brute_ClubRun", "Brute_ClubAttackFar", "Brute_GunAttack1", "Brute_GunRangedAtk",
    "AlienFlier_stand", "AlienFlier_StrafeAttack", "AlienFlier_AirRecover", "AlienFlier_GroundReact",
    "AlienFlier_Attack1", "AlienFlier_StrafeLeft", "AlienFlier_StrafeRight",
    "AlienFlier_2_StrafeAttack", "AlienFlier_3_StrafeAttack",
    "Roekel_Walk", "Roekel_Run", "Roekel_Stand", "Roekel_Jump", "Roekel_Attack1", "Roekel_SuperRanged",
    "Roekel_ThrowMale", "Male_RoekelThrowVictim", "Roekel_SuperThrowMale", "Male_RoekelSThrowVictim"};
constexpr const char* kEnemyFiles[]{
    "D:\\Models\\HumanoidGun.nod", "D:\\Models\\HumanoidGun_phys.nod", "D:\\Motions\\HumanoidGun.nms",
    "D:\\Motions\\humanpeon.nms", "D:\\Misc\\PLAYER_Human3.nvb",
    "D:\\Models\\Brute_Blade.nod", "D:\\Models\\Brute_Blade_phys.nod", "D:\\Motions\\Brute_Blade.nms",
    "D:\\Motions\\brutepeon.nms", "D:\\Misc\\PLAYER_Brute1.nvb",
    "D:\\Models\\Spiderman_alt.nod", "D:\\Models\\spiderman_alt_shadow.nod", "D:\\Models\\Spiderman_alt_phys.nod",
    "D:\\Motions\\SpiderMan.nms", "D:\\Misc\\PLAYER_Spiderman.nvb",
    "D:\\Models\\Torch_alt.nod", "D:\\Models\\Torch_alt_shadow.nod", "D:\\Models\\Torch_alt_phys.nod",
    "D:\\Motions\\Torch.nms", "D:\\Misc\\PLAYER_HumanTorch.nvb",
    "D:\\Models\\Humanoid_Club.nod", "D:\\Models\\humanoid_phys.nod", "D:\\Motions\\Humanoid.nms", "D:\\Misc\\PLAYER_Human2.nvb",
    "D:\\Models\\Brute.nod", "D:\\Models\\Brute_phys.nod", "D:\\Motions\\BruteClub.nms", "D:\\Misc\\PLAYER_Brute2.nvb",
    "D:\\Models\\BruteGun.nod", "D:\\Models\\BruteGun_phys.nod", "D:\\Motions\\BruteGun.nms", "D:\\Misc\\PLAYER_Brute3.nvb",
    "D:\\Models\\AlienFlier.nod", "D:\\Motions\\Flyer1.nms", "D:\\Misc\\PLAYER_Flyer1.nvb", "D:\\Models\\PlasmaBomb.nod",
    "D:\\Models\\Roekel.nod", "D:\\Models\\Roekel_shadow.nod", "D:\\Models\\Roekel_phys.nod",
    "D:\\Motions\\Roekel.nms", "D:\\Motions\\Brute.nms", "D:\\Misc\\PLAYER_Roekel.nvb",
    "D:\\Models\\Elektra_alt.nod", "D:\\Models\\Elektra_alt_shadow.nod", "D:\\Models\\Elektra_alt_phys.nod",
    "D:\\Motions\\Elektra.nms", "D:\\Motions\\Female.nms", "D:\\Misc\\PLAYER_Elektra.nvb",
    "D:\\Models\\Venom_alt.nod", "D:\\Models\\Venom_alt_shadow.nod", "D:\\Models\\Venom_alt_phys.nod",
    "D:\\Motions\\Venom.nms", "D:\\Misc\\PLAYER_Venom.nvb",
    "D:\\Models\\Thing_alt.nod", "D:\\Models\\Thing_alt_shadow.nod", "D:\\Models\\Thing_alt_phys.nod",
    "D:\\Motions\\Thing.nms", "D:\\Misc\\PLAYER_Thing.nvb",
    "D:\\Models\\AlienFlier_Silver.nod", "D:\\Motions\\Flyer2.nms", "D:\\Misc\\PLAYER_Flyer2.nvb",
    "D:\\Models\\AlienFlier_Gold.nod", "D:\\Motions\\Flyer3.nms", "D:\\Misc\\PLAYER_Flyer3.nvb", "D:\\Misc\\game.nfx"};
struct EnemyDefinition { const char* actor; const char* label; unsigned sound_bank; };
// Use the original roster's alternate presets; bosses remain NPCs without participant records.
constexpr auto kHeroBosses = [] {
    std::array<EnemyDefinition, kNativeCharacters.size()> bosses{};
    for (size_t i = 0; i < bosses.size(); ++i) {
        const auto& hero = kNativeCharacters[i].costumes[1];
        bosses[i] = {hero.actor, hero.label, hero.sound_bank};
        // Preserve the two campaign NPC presets already exercised in Survival.
        if (hero.sound_bank == 5) bosses[i].actor = "act_altSpiderMan";
        if (hero.sound_bank == 6) bosses[i].actor = "act_altHumanTorch";
    }
    return bosses;
}();
struct HeroResources {
    const char* model; const char* motion; const char* base_motion; const char* nvb;
    unsigned ai;
    std::array<const char*, 4> animations; // Stand, walk, run and first attack from the effective native motion set.
};
constexpr std::array<HeroResources, kNativeCharacters.size()> kHeroResources{{
    {"Thing", "Thing.nms", "Brute.nms", "PLAYER_THING.nvb", 16,
     {{"Thing_Stand", "Thing_Walk2", "Thing_Run", "Thing_Attack1"}}},
    {"Wolverine", "wolverine.nms", "Male.nms", "PLAYER_WOLVERINE.nvb", 18,
     {{"Wolverine_Stand", "Wolverine_Walk", "Wolverine_Run", "Wolverine_Ncombo_H1"}}},
    {"Paradox", "Paradox.nms", "Female.nms", "PLAYER_Paradox.nvb", 12,
     {{"Wink_Stand", "Wink_walk", "Storm_Run", "Wink_Attack1"}}},
    {"Elektra", "Elektra.nms", "Female.nms", "PLAYER_ELEKTRA.nvb", 5,
     {{"Elektra_stand", "Elektra_Walk", "Elektra_Run2", "Elektra_Attack1"}}},
    {"Johnnyohm", "JohnnyOhm.nms", "Male.nms", "PLAYER_JohnnyOhm.nvb", 9,
     {{"Johnnyohm_Idle", "Johnnyohm_walk", "Johnnyohm_run", "Johnnyohm_NCombo_H1"}}},
    {"Spiderman", "SpiderMan.nms", "Male.nms", "PLAYER_Spiderman.nvb", 14,
     {{"Spiderman_Stand", "Spiderman_walk", "Spiderman_Run", "Spiderman_Attack1"}}},
    {"Daredevil", "Daredevil.nms", "Male.nms", "PLAYER_Daredevil.nvb", 3,
     {{"Daredevil_Stand", "Spiderman_walk", "Spiderman_Run", "Daredevil_NCombo_H1"}}},
    {"Storm", "Storm.nms", "Female.nms", "PLAYER_Storm.nvb", 15,
     {{"Storm_Stand", "Storm_Walk", "Storm_Run", "Storm_Attack1"}}},
    {"Shock", "Shock.nms", "Female2.nms", "PLAYER_Aftershock.nvb", 1,
     {{"Shock_Stand", "Wink_walk", "Shock_Run2", "Shock_NCombo_H1"}}},
    {"Venom", "Venom.nms", "Brute.nms", "PLAYER_Venom.nvb", 17,
     {{"Venom_Stand", "Venom_Walk", "Venom_Run2", "Venom_Attack1"}}},
    {"Heatwave", "Heatwave.nms", "Female2.nms", "PLAYER_Heatwave.nvb", 6,
     {{"Heatwave_Stand", "Heatwave_Walk", "Heatwave_Run", "Heatwave_Attack1"}}},
    {"Torch", "Torch.nms", "Male.nms", "PLAYER_HumanTorch.nvb", 7,
     {{"Torch_Stand", "Johnnyohm_walk", "Spiderman_Run", "Torch_Attack1"}}},
    {"Ironman", "ironman.nms", "Male.nms", "PLAYER_IRONMAN.nvb", 8,
     {{"IronMan_attack_idle", "Ironman_Walk", "Ironman_run", "Ironman_melee_H1"}}},
    {"Megalith", "Megalith.nms", "Brute.nms", "PLAYER_Megalith.nvb", 11,
     {{"Megalith_Stand", "Megalith_Walk", "Megalith_Run", "Megalith_Attack1"}}},
    {"Magneto", "Magneto.nms", "Male.nms", "PLAYER_Magneto.nvb", 10,
     {{"Magneto_Stand", "Magneto_Walk", "Magneto_Run", "Magneto_Attack1"}}},
    {"Biohazard", "Biohazard.nms", "Male.nms", "PLAYER_Biohazard.nvb", 2,
     {{"Biohazard_Stand", "Biohazard_Walk", "Biohazard_Run", "Biohazard_NCombo_H1"}}},
    {"Defcon", "DefCon.nms", "Female2.nms", "PLAYER_DefCon.nvb", 4,
     {{"Defcon_Stand", "Defcon_Walk", "Defcon_Run", "Defcon_NCombo_H1"}}},
    {"Roekel", "Roekel.nms", "Brute.nms", "PLAYER_Roekel.nvb", 13,
     {{"Roekel_Stand", "Roekel_Walk", "Roekel_Run", "Roekel_Attack1"}}},
}};
static_assert(kHeroBosses.size() == 18 && kHeroResources.size() == kHeroBosses.size());
static_assert([] {
    for (size_t i = 0; i < kHeroResources.size(); ++i) {
        const auto& resource = kHeroResources[i];
        if (!resource.model || !resource.motion || !resource.base_motion || !resource.nvb ||
            resource.ai < 1 || resource.ai > 18) return false;
        for (const auto* animation : resource.animations) if (!animation) return false;
        for (size_t j = 0; j < i; ++j) if (kHeroResources[j].ai == resource.ai) return false;
    }
    return true;
}());
constexpr unsigned OtherHero(unsigned first, unsigned draw) { return draw >= first ? draw + 1 : draw; }
constexpr unsigned HeroForWave(unsigned wave, const std::array<unsigned, 2>& order) {
    // Freeze two distinct native presets per attempt; death cannot reroll either encounter.
    return order[wave > 20 ? (wave / 5) % 2 : wave >= 15 ? 1 : 0];
}
static_assert([] {
    for (unsigned first = 0; first < kHeroBosses.size(); ++first)
        for (unsigned draw = 0; draw + 1 < kHeroBosses.size(); ++draw) {
            const std::array<unsigned, 2> order{first, OtherHero(first, draw)};
            if (order[1] >= kHeroBosses.size() || order[0] == order[1] ||
                HeroForWave(10, order) != first || HeroForWave(15, order) != order[1]) return false;
        }
    return true;
}());
constexpr EnemyDefinition EnemyFor(Run::Enemy type, unsigned hero) {
    switch (type) {
    case Run::Enemy::Human1: return {"act_human1", "Human1", 2};
    case Run::Enemy::Human3: return {"act_human3", "Human3", 3};
    case Run::Enemy::Brute1: return {"act_brute1", "Brute1", 4};
    case Run::Enemy::HeroBoss: return kHeroBosses[hero];
    case Run::Enemy::Human2: return {"act_human2", "Human2", 7};
    case Run::Enemy::Brute2: return {"act_brute2", "Brute2", 8};
    case Run::Enemy::Brute3: return {"act_brute3", "Brute3", 9};
    case Run::Enemy::Flyer1: return {"act_flyer1", "Flyer1", 10};
    case Run::Enemy::FinalBoss: return {"act_Roekel", "Roekel", 11};
    case Run::Enemy::Flyer2: return {"act_flyer2", "Flyer2", 15};
    case Run::Enemy::Flyer3: return {"act_flyer3", "Flyer3", 16};
    }
    return {"", "", 0};
}
constexpr std::array<bool, kSoundBanks.size()> RequiredSounds(Run plan, unsigned hero, bool cooperative = false, SelectedParty characters = kDefaultSelection, bool managed_audio = false) {
    std::array<bool, kSoundBanks.size()> required{};
    required[1] = true; // Shared peon sounds remain necessary for every roster.
    required[characters[0].sound_bank] = true;
    if (cooperative) required[characters[1].sound_bank] = true;
    for (plan.wave.spawned = 0; plan.wave.spawned < plan.wave.target; ++plan.wave.spawned)
        required[EnemyFor(plan.NextEnemy(), hero).sound_bank] = true;
    // Keep the base banks only before the first drone wave; later rosters need their own five-bank budget.
    if (plan.number < 5 && plan.difficulty == 0) for (unsigned i = 1; i < 5; ++i) required[i] = true;
    if (cooperative || managed_audio) for (unsigned i : {17u, 18u, 19u}) required[i] = true;
    return required;
}
constexpr bool CheckWaveSounds(unsigned hero, bool cooperative = false) {
    const auto hero_mask = 1u << kHeroBosses[hero].sound_bank;
    const std::array<unsigned, 20> expected{31,31,31,31,1043,31,1051,1171,283,
        32787u | hero_mask,531,33043,33299,787,65555u | hero_mask,66067,66067,787,66067,2579};
    for (unsigned n = 1; n <= Run::TotalWaves; ++n) {
        Run plan;
        if (!plan.Start(n, 3)) return false;
        const auto required = RequiredSounds(plan, hero, cooperative);
        unsigned mask = 0, count = 0;
        for (unsigned i = 0; i < required.size(); ++i) if (required[i]) { mask |= 1u << i; ++count; }
        const auto coop_mask = cooperative ? ((1u << 5) | (1u << 17) | (1u << 18) | (1u << 19)) : 0u;
        if (mask != (expected[n - 1] | coop_mask) || count > (cooperative ? 9u : 5u)) return false;
    }
    return true;
}
// Coop reserves one native generic bank plus at most nine arena/character banks.
constexpr bool CheckNativeAudioBudget(unsigned hero, unsigned difficulty = 0) {
    // Four NPC banks plus two participants, three arena banks and generic fit the native ten slots.
    for (unsigned number = 1; number <= Run::TotalWaves; ++number) {
        Run plan;
        if (!plan.Start(number, 3, difficulty)) return false;
        const Character peon{"", "", 1};
        const auto required = RequiredSounds(plan, hero, false, {peon, peon});
        unsigned count = 0;
        for (const auto present : required) count += present;
        if (count > 4) return false;
    }
    return true;
}
// Separate constant expressions keep the complete roster within compiler evaluation limits.
template<size_t Hero> struct HeroAudioChecks {
    static_assert(CheckWaveSounds(Hero));
    static_assert(CheckWaveSounds(Hero, true));
    static_assert(CheckNativeAudioBudget(Hero));
    static_assert(CheckNativeAudioBudget(Hero, 1));
    static_assert(CheckNativeAudioBudget(Hero, 2));
};
template<size_t... Hero> constexpr bool CheckHeroAudio(std::index_sequence<Hero...>) {
    return ((sizeof(HeroAudioChecks<Hero>) > 0) && ...);
}
static_assert(CheckHeroAudio(std::make_index_sequence<kHeroBosses.size()>{}));
std::array<unsigned, 2> g_hero_order{0, 1};
uint32_t g_sound_owner = 0;
std::array<bool, kSoundBanks.size()> g_owned_sounds{};
constexpr uint32_t kAudioPoolTable = 0x5185E0, kAudioPoolSize = 16u << 20;
uint32_t g_audio_pool = 0, g_audio_pool_id = 64;
thread_local bool g_audio_allocating = false;
constexpr uint32_t kEnemyResources = 0x8950, kEnemyKey = 0x89B0, kEnemyPosition = 0x89F0, kEnemyHudSize = 0x8A00;
static_assert(kEnemyResources + 3 * 0x1C <= kEnemyKey && kEnemyKey + 64 <= kEnemyPosition && kEnemyPosition + 8 <= kEnemyHudSize);
uint32_t g_enemy_hud = 0, g_enemy_hud_owner = 0, g_enemy_hud_target = 0;
bool g_enemy_hud_constructed = false, g_enemy_hud_visible = false;
bool g_armed = false, g_target_load = false, g_save_fence = false;
bool g_native_player_death = false;
thread_local bool g_ai_target_query = false;
thread_local uint32_t g_ai_melee_attacker = 0;
thread_local uint32_t g_ai_melee_choice_brain = 0;
unsigned g_ai_target_reports = 0;
unsigned g_ai_repair_reports = 0;
unsigned g_ai_melee_reports = 0;
unsigned g_ai_melee_choices = 0, g_ai_basic_choices = 0, g_ai_throw_choices = 0, g_ai_melee_gated = 0;
unsigned g_ai_melee_tasks = 0;
unsigned g_ai_melee_unblocked = 0;
std::chrono::steady_clock::time_point g_ai_last_melee_tick{};
uint32_t g_ai_reported_target = ~0u;
bool g_native_lives_query = false;
bool g_coop = false, g_ai_partner = false, g_coop_hud_setup = false, g_coop_reviving = false, g_managed_audio = false;
SelectedParty g_characters = kDefaultSelection;
CoopLives g_coop_lives;
std::array<bool, 2> g_coop_down{};
unsigned g_camera_excluded_mask = ~0u;
std::array<uint32_t, 2> g_coop_actors{}, g_coop_handles{}, g_coop_controllers{}, g_coop_cameras{};
std::array<std::chrono::steady_clock::time_point, 2> g_coop_respawn{}, g_coop_protection{};
std::array<uint8_t, 36> g_partner_name{};
uint32_t g_partner_flags = 0, g_shared_camera_before = 0;
Phase g_phase = Phase::Off;
Phase g_resume_phase = Phase::Fighting;
Run g_run;
std::array<unsigned, 2> g_player_kills{};
unsigned g_shared_kills = 0;
uint32_t g_world = 0, g_scene = 0, g_scratch = 0;
uint32_t g_respawn_actor = 0, g_respawn_handle = 0;
bool g_respawn_early = false;
uint32_t g_hero_actor = 0, g_hero_handle = 0;
struct FinisherActor {
    uint32_t actor = 0, handle = 0, controller = 0;
    bool protected_before = false;
};
struct NativeFinisher {
    uint32_t world = 0, definition = 0;
    unsigned player_slot = 0;
    std::array<FinisherActor, 2> actors{}; // P1, then the tracked NPC hero.
};
NativeFinisher g_finisher;
uint32_t g_lives_hud = 0;
constexpr uint32_t kScratchSize = 512, kTextStart = 96, kTextStride = 0x80, kTextInput = 352, kTextCapacity = 64;
constexpr uint32_t kEnemyName = kTextInput + kTextCapacity;
static_assert(kTextStart + 2 * kTextStride == kTextInput && kEnemyName + 32 <= kScratchSize);
static_assert([] {
    for (unsigned hero = 0; hero < kHeroBosses.size(); ++hero)
        for (unsigned type = 0; type <= unsigned(Run::Enemy::Flyer3); ++type) {
            const auto definition = EnemyFor(Run::Enemy(type), hero);
            unsigned length = 0;
            while (length < 32 && definition.actor[length]) ++length;
            if (!length || length == 32 || definition.sound_bank >= kSoundBanks.size()) return false;
        }
    return true;
}());
static_assert(sizeof("WAVE COMPLETE - Next wave in ") + 10 <= kTextCapacity);
static_assert(sizeof("BOSS COMPLETE +3 LIVES - Next wave in ") + 10 <= kTextCapacity);
static_assert(sizeof("SURVIVAL COMPLETE - Start: pause menu") <= kTextCapacity);
static_assert(sizeof("SURVIVAL COMPLETE +3 LIVES - Start: pause menu") <= kTextCapacity);
static_assert(sizeof("BOSS COMPLETE P1 +3 P2 +3 - Next wave in 10") <= kTextCapacity);
static_assert(sizeof("SURVIVAL COMPLETE P1 +3 P2 +3 - Start: pause menu") <= kTextCapacity);
uint32_t g_text_hud = 0;
unsigned g_text_count = 0, g_counter_value = ~0u, g_counter_target = 0;
std::array<std::string, 2> g_text_labels;
std::chrono::steady_clock::time_point g_text_next{};
std::array<uint8_t, 36> g_player_name{};
std::array<uint32_t, kCampaignScalars.size()> g_campaign{};
uint32_t g_player_flags = 0;
std::chrono::steady_clock::time_point g_started{}, g_next{}, g_report{}, g_intermission_end{};
std::chrono::steady_clock::time_point g_ai_retarget{};
std::chrono::steady_clock::time_point g_ai_still_since{}, g_ai_pursuit_next{};
std::array<float, 2> g_ai_still_position{};
uint32_t g_ai_still_target = 0;
unsigned g_ai_pursuit_reports = 0;
std::chrono::steady_clock::time_point g_respawn_end{}, g_protected_until{};
std::chrono::steady_clock::time_point g_reinforcement_end{};
std::chrono::steady_clock::time_point g_grab_pickup_report{}, g_grab_throw_report{};
std::chrono::steady_clock::duration g_preparation_left{};
constexpr auto kPreparation = std::chrono::seconds(10);
// Use the native simulation clock so APT pause freezes gameplay deadlines without freezing the UI.
uint32_t g_clock_ticks = 0;
std::chrono::milliseconds g_clock_elapsed{};
bool g_clock_ready = false;
bool g_result_clock_frozen = false;
unsigned g_result_seconds = 0;
bool g_native_exit_requested = false;
std::optional<ReplaySettings> g_replay_settings;
std::chrono::steady_clock::time_point RunNow() {
    if (!g_clock_ready) return kernel::GuestNow();
    const auto ticks = kernel::R32(0x49E924);
    g_clock_elapsed += std::chrono::milliseconds(uint32_t(ticks - g_clock_ticks));
    g_clock_ticks = ticks;
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(g_clock_elapsed));
}


unsigned ElapsedSeconds() {
    if (!g_clock_ready) return 0;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(RunNow().time_since_epoch()).count();
    return unsigned(std::clamp<int64_t>(seconds, 0, std::numeric_limits<unsigned>::max()));
}

struct GrabTrace;
GrabTrace* g_grab_trace = nullptr;
struct GrabTrace {
    GrabTrace* previous = g_grab_trace;
    uint32_t controller;
    bool querying = false;
    explicit GrabTrace(uint32_t value) : controller(value) { g_grab_trace = this; }
    ~GrabTrace() { g_grab_trace = previous; }
}; // Diagnostic state exists only during this native call, including exception unwinding.

bool Address(uint32_t p, uint32_t size) {
    if (p < 0x10000 || !size || uint64_t(p) + size > 0x100000000ull) return false;
    for (uint64_t page = p & ~uint64_t(0xFFF); page < uint64_t(p) + size; page += 0x1000)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}
bool NameIs(uint32_t p, const char* expected) {
    if (!Address(p, 32)) return false;
    const auto* text = static_cast<const unsigned char*>(kernel::HostPtr(p));
    for (unsigned i = 0; i < 32; ++i) {
        auto a = text[i];
        auto b = static_cast<unsigned char>(expected[i]);
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
        if (!a) return true;
    }
    return false;
}
bool AudioPoolReady() {
    return g_audio_pool_id >= 2 && g_audio_pool_id < 64 && Address(g_audio_pool, 0x80) &&
        kernel::R32(kAudioPoolTable + g_audio_pool_id * 4) == g_audio_pool + 0x10 &&
        kernel::R32(g_audio_pool + 0x18) == g_audio_pool;
}
bool EnsureAudioPool(X86Context& ctx) {
    if (g_audio_pool) return AudioPoolReady();
    if (!Address(kAudioPoolTable, 64 * 4)) return false;
    const X86Context saved = ctx;
    const auto id = kernel::CallGuest(ctx, 0x11CD5D, {});
    ctx = saved;
    if (id < 2 || id >= 64 || kernel::R32(kAudioPoolTable + id * 4)) return false;
    uint32_t memory = 0, bytes = kAudioPoolSize;
    if (mem::AllocateVirtual(memory, bytes, mem::kMemReserve | mem::kMemCommit, 4) != 0) return false;
    constexpr char name[] = "SurvivalAudio";
    std::memcpy(kernel::HostPtr(g_scratch + 64), name, sizeof(name));
    // Native pools retain their own block headers; normal bank cleanup can free these blocks after the session.
    kernel::CallGuest(ctx, 0x11CB18, {id, g_scratch + 64, memory, bytes, 16, 16, 0, 0x800});
    ctx = saved;
    g_audio_pool = memory;
    g_audio_pool_id = id;
    const bool ready = AudioPoolReady();
    LOGR("survival-coop: audio pool id=%u base=%08X bytes=%u ready=%u", id, memory, bytes, unsigned(ready));
    return ready;
}
struct AudioAllocationScope {
    const bool previous = g_audio_allocating;
    explicit AudioAllocationScope(bool managed) { g_audio_allocating = managed; }
    ~AudioAllocationScope() { g_audio_allocating = previous; }
};
void Present(const char* message) {
    View view{g_phase, g_run.wave.defeated, message};
    view.cooperative = g_coop;
    view.ai_partner = g_ai_partner;
    view.wave = g_run.number;
    view.waves_cleared = g_run.cleared;
    view.total_defeated = g_run.total_defeated;
    view.kills = g_player_kills[0];
    view.partner_kills = g_player_kills[1];
    view.shared_kills = g_shared_kills;
    view.target = g_run.wave.target;
    view.lives = g_run.lives.remaining;
    view.initial_lives = g_run.lives.initial;
    view.start_wave = g_run.start_wave;
    view.venue = unsigned(g_arena - Arenas.data());
    view.difficulty = g_run.difficulty;
    view.endless = g_run.endless;
    view.elapsed_seconds = g_result_clock_frozen ? g_result_seconds : ElapsedSeconds();
    view.waves_required = g_run.WavesRequired();
    view.enemies_required = g_run.EnemiesRequired();
    view.deaths = g_run.lives.deaths;
    view.boss_wave = g_run.BossWave();
    view.boss_bonus = g_run.boss_bonus;
    view.native_text = g_text_count == 2;
    if (g_coop) {
        view.lives = g_coop_lives.players[0].remaining;
        view.deaths = g_coop_lives.players[0].deaths;
        view.partner_lives = g_coop_lives.players[1].remaining;
        view.partner_deaths = g_coop_lives.players[1].deaths;
        view.boss_bonus = g_coop_lives.boss_bonus[0];
        view.partner_boss_bonus = g_coop_lives.boss_bonus[1];
    }
    if (g_phase == Phase::Intermission || g_phase == Phase::Respawning) {
        const auto end = g_phase == Phase::Respawning ? g_respawn_end : g_intermission_end;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - RunNow()).count();
        view.seconds = SecondsLeft(ms);
    }
    Publish(std::move(view));
}
void State(Phase phase, const char* message) {
    if ((phase == Phase::Won || phase == Phase::Lost) && !g_result_clock_frozen) {
        g_result_seconds = ElapsedSeconds();
        g_result_clock_frozen = true;
    }
    g_phase = phase;
    Present(message);
    if (g_coop) {
        LOGR("survival-coop: phase=%u wave=%u/%u cleared=%u spawned=%u defeated=%u target=%u p1_lives=%u p2_lives=%u p1_deaths=%u p2_deaths=%u message=%s",
             unsigned(phase), g_run.number, Run::TotalWaves, g_run.cleared, g_run.wave.spawned, g_run.wave.defeated, g_run.wave.target,
             g_coop_lives.players[0].remaining, g_coop_lives.players[1].remaining,
             g_coop_lives.players[0].deaths, g_coop_lives.players[1].deaths, message);
        return;
    }
    LOGR("survival: stage=9 phase=%u wave=%u/%u cleared=%u spawned=%u defeated=%u total=%u lives=%u/%u deaths=%u message=%s",
         unsigned(phase), g_run.number, Run::TotalWaves, g_run.cleared,
         g_run.wave.spawned, g_run.wave.defeated, g_run.total_defeated,
         g_run.lives.remaining, g_run.lives.initial, g_run.lives.deaths, message);
}
uint32_t Player(unsigned slot = 0) {
    if (slot >= (g_coop ? 2u : 1u)) return 0;
    const auto record = kPlayer + slot * 0xB0;
    const uint32_t actor = kernel::R32(record + 0x28);
    return (kernel::R32(kCount) == (g_coop ? 2u : 1u) ||
        (g_ai_target_query && g_ai_partner && g_coop && kernel::R32(kCount) == 1)) && Address(actor, 0x408) &&
        kernel::G<uint8_t>(actor + 0x10) == 7 && kernel::R32(actor + 0x30C) == record ? actor : 0;
}
unsigned CoopSlot(uint32_t actor) {
    if (!g_armed || !g_coop || g_phase == Phase::Leaving || g_phase == Phase::Failed ||
        !actor || !g_world || kernel::R32(kWorld) != g_world) return 2;
    for (unsigned slot = 0; slot < 2; ++slot)
        if (actor == g_coop_actors[slot] && actor == Player(slot) &&
            kernel::R32(actor + 0x34) == g_coop_handles[slot] && kernel::R32(actor + 0x14) == g_world &&
            kernel::R32(actor + 0x3EC) == g_coop_controllers[slot]) return slot;
    return 2;
}
bool Allies(uint32_t first, uint32_t second) {
    return first != second && CoopSlot(first) < 2 && CoopSlot(second) < 2;
}
void CoopDeath(unsigned slot) {
    if (slot >= 2 || (g_phase != Phase::Fighting && g_phase != Phase::Intermission) || g_coop_down[slot]) return;
    if (!g_coop_lives.players[slot].Die()) return;
    g_coop_down[slot] = true;
    g_coop_respawn[slot] = RunNow() + std::chrono::seconds(5);
    g_coop_protection[slot] = {};
    LOGR("survival-coop: death player=%u lives=%u pending=%u deaths=%u", slot + 1,
         g_coop_lives.players[slot].remaining, unsigned(g_coop_lives.players[slot].pending), g_coop_lives.players[slot].deaths);
}
bool Dead(uint32_t actor) {
    return kernel::G<float>(actor + 0x32C) <= 0 || (kernel::R32(actor + 0x314) & 0x100) != 0;
}
bool FinisherActive() {
    const auto player = Player(g_finisher.player_slot);
    return g_armed && g_phase != Phase::Leaving && g_finisher.world && g_finisher.world == g_world &&
        kernel::R32(kWorld) == g_world && player && player == g_finisher.actors[0].actor &&
        kernel::R32(player + 0x34) == g_finisher.actors[0].handle;
}
bool WorldMatches();
bool Tracked(uint32_t actor);
bool NativeResultBlocked() {
    // Arena hazards can publish story results before or after the ordinary death callback.
    return g_native_player_death || FinisherActive() ||
        (g_armed && g_phase != Phase::Loading && g_phase != Phase::Leaving && WorldMatches());
}
void FinishNativeFinisher() {
    const auto finished = std::exchange(g_finisher, NativeFinisher{});
    if (!g_armed || !finished.world || g_world != finished.world || kernel::R32(kWorld) != finished.world) return;
    for (const auto& saved : finished.actors) {
        if (saved.protected_before) continue;
        const auto actor = saved.actor;
        if (!Address(actor, 0x408) || kernel::G<uint8_t>(actor + 0x10) != 7 ||
            kernel::R32(actor + 0x34) != saved.handle || kernel::R32(actor + 0x14) != finished.world ||
            kernel::R32(actor + 0x3EC) != saved.controller || !Address(saved.controller, 0xF8) ||
            kernel::R32(saved.controller + 0xF4) != actor) {
            LOGR("survival-finisher: protection restore skipped actor=%08X handle=%08X reason=identity", actor, saved.handle);
            continue;
        }
        const auto flags = kernel::R32(actor + 0x314);
        // C9080 adds bit 1 to both participants; native cleanup clears only the victim's bit.
        kernel::W32(actor + 0x314, flags & ~1u);
        LOGR("survival-finisher: protection released actor=%08X handle=%08X flags=%08X->%08X",
             actor, saved.handle, flags, kernel::R32(actor + 0x314));
    }
    LOGR("survival-finisher: native cleanup complete definition=%08X", finished.definition);
}
bool GrabReport(uint32_t controller, std::chrono::steady_clock::time_point& next) {
    if (!g_armed || g_phase != Phase::Fighting) return false;
    const auto player = Player();
    if (!player || kernel::R32(player + 0x3EC) != controller || !Address(controller, 0x1C8)) return false;
    const auto now = RunNow();
    if (now < next) return false;
    next = now + std::chrono::milliseconds(250);
    return true;
}
void ReportGrabActor(const char* label, uint32_t actor) {
    if (!Address(actor, 0x408)) {
        LOGR("survival-grab: %s actor=%08X unavailable", label, actor);
        return;
    }
    const auto record = kernel::R32(actor + 0x30C);
    const bool linked = Address(record, 0x38);
    LOGR("survival-grab: %s actor=%08X kind=%u handle=%08X flags=%08X controller=%08X record=%08X record_valid=%u record_flags=%08X position=%.3f,%.3f,%.3f",
         label, actor, kernel::G<uint8_t>(actor + 0x10), kernel::R32(actor + 0x34), kernel::R32(actor + 0x314),
         kernel::R32(actor + 0x3EC), record, unsigned(linked), linked ? kernel::R32(record + 0x34) : 0,
         kernel::G<float>(actor + 0xC0), kernel::G<float>(actor + 0xC4), kernel::G<float>(actor + 0xC8));
    LOGR("survival-grab: actor-details actor=%08X entity_flags=%08X entity_owner=%08X flags310=%08X flags318=%08X body_variant=%u",
         actor, kernel::R32(actor + 0x3C), kernel::R32(actor + 0xD0), kernel::R32(actor + 0x310),
         kernel::R32(actor + 0x318), kernel::R32(actor + 0x210));
    const auto controller = kernel::R32(actor + 0x3EC);
    if (Address(controller, 0x1C8)) LOGR("survival-grab: actor-state actor=%08X state=%u flags=%08X carry_handle=%08X input=%08X",
        actor, kernel::G<uint8_t>(controller + 0x18), kernel::R32(controller + 0x28),
        kernel::R32(controller + 0x100), kernel::R32(controller + 0x1C4));
}
uint32_t AiBrainOwner(uint32_t brain) {
    if (!g_ai_partner || !WorldMatches() || !Address(brain, 0x5F0)) return 0;
    const auto actor = kernel::R32(brain + 0x530);
    if (!actor || actor != Player(1) || CoopSlot(actor) != 1 ||
        kernel::R32(actor + 0x390) != brain || (kernel::R32(kPartner + 0x34) & 6) != 4) return 0;
    const auto controller = g_coop_controllers[1];
    return Address(controller, 0x444) && kernel::R32(controller + 0xF4) == actor ? actor : 0;
}
void ReportAiPartner() {
    if (!g_ai_partner || !WorldMatches() || g_phase != Phase::Fighting) return;
    const auto actor = Player(1);
    if (!actor || CoopSlot(actor) != 1) return;
    const auto controller = kernel::R32(actor + 0x3EC), brain = kernel::R32(actor + 0x390);
    if (!Address(controller, 0x444) || kernel::R32(controller + 0xF4) != actor) return;
    const auto input = kernel::R32(controller + 0x1C4);
    const bool brain_owned = Address(brain, 0x5F0) && kernel::R32(brain + 0x530) == actor;
    LOGR("survival-ai: sample wave=%u actor=%08X position=%.3f,%.3f,%.3f hp=%.2f down=%u state=%u flags=%08X carry=%08X input=%08X input_type=%08X brain=%08X brain_owned=%u cached_target=%08X brain_nearest=%08X brain_priority=%08X melee_tasks=%u melee_choices=%u basic_choices=%u throw_choices=%u melee_gated=%u melee_unblocked=%u",
         g_run.number, actor, kernel::G<float>(actor + 0xC0), kernel::G<float>(actor + 0xC4),
         kernel::G<float>(actor + 0xC8), kernel::G<float>(actor + 0x32C), unsigned(g_coop_down[1]),
         kernel::G<uint8_t>(controller + 0x18), kernel::R32(controller + 0x28), kernel::R32(controller + 0x100),
         input, Address(input, 4) ? kernel::R32(input) : 0, brain, unsigned(brain_owned), kernel::R32(controller + 0x440),
         brain_owned ? kernel::R32(brain + 0x19C) : 0, brain_owned ? kernel::R32(brain + 0x1B4) : 0,
         g_ai_melee_tasks, g_ai_melee_choices, g_ai_basic_choices, g_ai_throw_choices, g_ai_melee_gated,
         g_ai_melee_unblocked);
}
void RefreshAiTarget(X86Context& ctx) {
    if (!g_ai_partner || !WorldMatches() || g_phase != Phase::Fighting || g_coop_down[1] || FinisherActive()) return;
    const auto actor = Player(1);
    if (!actor || CoopSlot(actor) != 1 || Dead(actor)) return;
    const auto controller = g_coop_controllers[1], brain = kernel::R32(actor + 0x390);
    if (!Address(controller, 0x444) || kernel::R32(controller + 0xF4) != actor ||
        !Address(brain, 0x5F0) || kernel::R32(brain + 0x530) != actor ||
        (kernel::R32(kPartner + 0x34) & 6) != 4 || kernel::R32(controller + 0x100)) return;
    const auto now = RunNow();
    if (now < g_ai_retarget) return;
    g_ai_retarget = now + std::chrono::milliseconds(250);
    const X86Context saved = ctx;
    ctx.ecx = brain;
    auto target = kernel::CallGuest(ctx, 0x6FC40, {});
    // Keep native prop/pickup intentions. A character target must still belong to this live wave.
    if (Address(target, 0x40) && kernel::R32(target + 0x14) == g_world &&
        kernel::G<uint8_t>(target + 0x10) != 7) { g_ai_still_since = {}; ctx = saved; return; }
    if (!Tracked(target) || Dead(target)) {
        const auto previous = kernel::R32(brain + 0x1B4);
        {
            struct RestoreQuery {
                const bool previous = g_ai_target_query;
                ~RestoreQuery() { g_ai_target_query = previous; }
            } query;
            g_ai_target_query = true;
            ctx.ecx = brain + 0xD0;
            target = kernel::CallGuest(ctx, 0x82350, {}); // Native spatial search and nearest-hostile ranking.
        }
        const auto handle = Tracked(target) && !Dead(target) ? kernel::R32(target + 0x34) : 0;
        // 81ED0 only rejects unresolved handles: a still-allocated corpse otherwise wins over nearest.
        kernel::W32(brain + 0x1B4, 0);
        kernel::W32(brain + 0x19C, handle);
        if (handle && g_ai_repair_reports < 16) {
            ++g_ai_repair_reports;
            LOGR("survival-ai: brain target repaired wave=%u actor=%08X previous_priority=%08X target=%08X",
                 g_run.number, actor, previous, handle);
        }
    }
    if (Tracked(target) && !Dead(target) && kernel::G<uint8_t>(controller + 0x18) == 2) {
        if (g_ai_last_melee_tick == std::chrono::steady_clock::time_point{})
            g_ai_last_melee_tick = now;
        const float x = kernel::G<float>(actor + 0xC0), y = kernel::G<float>(actor + 0xC4);
        const float dx = x - kernel::G<float>(target + 0xC0);
        const float dy = y - kernel::G<float>(target + 0xC4);
        const float dz = kernel::G<float>(actor + 0xC8) - kernel::G<float>(target + 0xC8);
        const float moved_x = x - g_ai_still_position[0], moved_y = y - g_ai_still_position[1];
        const auto handle = kernel::R32(target + 0x34);
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) ||
            handle != g_ai_still_target || g_ai_still_since == std::chrono::steady_clock::time_point{} ||
            moved_x * moved_x + moved_y * moved_y > 0.49f) {
            g_ai_still_position = {x, y};
            g_ai_still_target = handle;
            g_ai_still_since = now;
        } else if (dx * dx + dy * dy > 25.0f && dx * dx + dy * dy < 1600.0f &&
                   std::abs(dz) < 4.0f && now - g_ai_still_since >= std::chrono::seconds(2) &&
                   now >= g_ai_pursuit_next) {
            const auto tasks = kernel::R32(brain + 0x204);
            if (Address(tasks, 0x2C)) {
                ctx.ecx = tasks;
                kernel::CallGuest(ctx, 0x845D0, {2}); // Native TASK_MELEE pursues the current hostile.
                g_ai_pursuit_next = now + std::chrono::seconds(5);
                if (g_ai_pursuit_reports++ < 16)
                    LOGR("survival-ai: restart melee pursuit wave=%u actor=%08X target=%08X distance=%.1f",
                         g_run.number, actor, handle, std::sqrt(dx * dx + dy * dy));
            }
            g_ai_still_since = now;
        }
        const auto tasks = kernel::R32(brain + 0x204);
        if (dx * dx + dy * dy <= 400.0f && std::abs(dz) < 2.5f && now >= g_ai_pursuit_next &&
            g_ai_last_melee_tick != std::chrono::steady_clock::time_point{} &&
            now - g_ai_last_melee_tick >= std::chrono::seconds(3) && Address(tasks, 0x2C) &&
            kernel::R32(tasks + 0x28) < 4) {
            const auto active = kernel::R32(tasks + 0x14);
            if (!Address(active, 4) || kernel::R32(active) != 0x40E368) { // Native TASK_MELEE vtable.
                ctx.ecx = tasks;
                kernel::CallGuest(ctx, 0x845D0, {2});
                g_ai_pursuit_next = now + std::chrono::seconds(5);
                if (g_ai_pursuit_reports++ < 16)
                    LOGR("survival-ai: resume melee task wave=%u actor=%08X target=%08X previous=%08X depth=%u",
                         g_run.number, actor, handle, active, kernel::R32(tasks + 0x28));
            }
        }
    } else {
        g_ai_still_since = {};
    }
    const auto cached = kernel::R32(controller + 0x440);
    if (Tracked(target) && !Dead(target)) {
        const auto handle = kernel::R32(target + 0x34);
        if (cached != handle) kernel::W32(controller + 0x440, handle);
        ctx = saved;
        return;
    }
    for (unsigned i = 0; i < g_run.wave.spawned; ++i)
        if (cached && !g_run.wave.dead[i] && cached == g_run.wave.handles[i]) { ctx = saved; return; }
    // Retire a previous wave/prop target, then use native spatial filtering and ranking. No camera notification.
    kernel::W32(controller + 0x440, 0);
    ctx.ecx = controller;
    kernel::CallGuest(ctx, 0xBADC0, {0});
    ctx = saved;
}
bool NativeLivesHud(uint32_t hud) {
    return g_armed && g_phase != Phase::Loading && g_phase != Phase::Leaving &&
        hud == g_lives_hud && Address(hud, 0x418C) && hud == kernel::R32(0x4A2E50) &&
        g_world && kernel::R32(kWorld) == g_world;
}
struct NativeLivesQuery {
    const bool previous = g_native_lives_query;
    NativeLivesQuery() { g_native_lives_query = true; }
    ~NativeLivesQuery() { g_native_lives_query = previous; }
};
void DestroyNativeText(X86Context& ctx) {
    const X86Context saved = ctx;
    while (g_text_count) {
        ctx.ecx = g_scratch + kTextStart + --g_text_count * kTextStride;
        kernel::CallGuest(ctx, 0x1F6C0, {}); // Unregister before the native HUD/font owners are destroyed.
    }
    g_text_hud = 0;
    g_text_labels = {};
    ctx = saved;
}
bool CreateNativeText(X86Context& ctx, uint32_t hud) {
    if (g_text_count) return false;
    const X86Context saved = ctx;
    const auto font = kernel::CallGuest(ctx, 0xDD7C0, {1});
    const float screen_height = kernel::G<float>(hud + 0x4184), screen_width = kernel::G<float>(hud + 0x4180);
    if (!Address(font, 8) || !std::isfinite(screen_width) || !std::isfinite(screen_height) ||
        screen_width < 100 || screen_height < 100 || screen_width > 8192 || screen_height > 8192) { ctx = saved; return false; }
    for (unsigned i = 0; i < 2; ++i) {
        const auto text = g_scratch + kTextStart + i * kTextStride;
        ctx.ecx = text;
        kernel::CallGuest(ctx, 0x14C10, {}); // Constructor owns a 64-byte buffer and its font-registry entry.
        ++g_text_count;
        if (!Address(kernel::R32(text + 0xC), kTextCapacity) || kernel::R32(text + 0x24) < kTextCapacity) {
            DestroyNativeText(ctx); ctx = saved; return false;
        }
        kernel::W32(text + 0x18, font);
        ctx.ecx = text;
        kernel::CallGuest(ctx, 0xDE3C0, {0x41980000}); // Native counter's 19-pixel font size.
        ctx.ecx = text;
        kernel::CallGuest(ctx, 0xDDC20, {2}); // Native center alignment.
        kernel::G<float>(text + 0x74) = 320.0f; // 2093C0/DDD40 render CText on a logical 640-wide canvas.
        kernel::G<float>(text + 0x78) = screen_width * (i ? 0.12f : 0.06f);
    }
    g_text_hud = hud;
    g_counter_value = ~0u;
    g_counter_target = 0;
    g_text_next = {};
    g_text_labels = {};
    LOGR("survival: native text ready hud=%08X font=%08X screen=%.0fx%.0f center_x=320 count=%u",
         hud, font, screen_width, screen_height, g_text_count);
    ctx = saved;
    return true;
}
const char* ParticipantLabel(unsigned slot) { return slot ? (g_ai_partner ? "AI" : "P2") : "P1"; }
void UpdateNativeText(X86Context& ctx, uint32_t hud) {
    if (g_text_count != 2 || g_text_hud != hud || g_phase == Phase::Failed ||
        static_cast<int32_t>(kernel::R32(0x49E93C)) > 0) return;
    const auto now = RunNow();
    if (now < g_text_next) return;
    g_text_next = now + std::chrono::milliseconds(100);
    const auto current = g_phase == Phase::Won ? g_run.total_defeated : g_run.wave.defeated;
    const auto target = g_phase == Phase::Won ? g_run.EnemiesRequired() : g_run.wave.target;
    if (g_counter_value != current || g_counter_target != target) {
        ctx.ecx = hud;
        kernel::CallGuest(ctx, 0x124B0, {current, target});
        // Retail spaces the centered fields once for "00"; measure again for three-digit run totals.
        ctx.ecx = hud + 0x88;
        const float slash_width = kernel::CallGuest(ctx, 0xDE150, {0});
        const float gap = slash_width * 0.5f + 4.0f;
        ctx.ecx = hud + 0x8;
        const float current_width = kernel::CallGuest(ctx, 0xDE150, {0});
        ctx.ecx = hud + 0x108;
        const float target_width = kernel::CallGuest(ctx, 0xDE150, {0});
        const float center = kernel::G<float>(hud + 0xFC);
        kernel::G<float>(hud + 0x7C) = center - gap - current_width * 0.5f;
        kernel::G<float>(hud + 0x17C) = center + gap + target_width * 0.5f;
        ctx.ecx = hud;
        kernel::CallGuest(ctx, 0x12580, {1});
        g_counter_value = current;
        g_counter_target = target;
        LOGR("survival: native counter wave=%u current=%u target=%u", g_run.number, current, target);
    }
    std::array<std::array<char, kTextCapacity>, 2> labels{};
    if (g_run.endless) std::snprintf(labels[0].data(), kTextCapacity,
                                    g_run.BossWave() ? "ENDLESS - BOSS WAVE %u" : "ENDLESS - WAVE %u", g_run.number);
    else std::snprintf(labels[0].data(), kTextCapacity,
                       g_run.BossWave() ? "BOSS WAVE %u / %u" : "WAVE %u / %u", g_run.number, Run::TotalWaves);
    if (g_coop && g_coop_lives.Pending() && (g_phase == Phase::Fighting || g_phase == Phase::Intermission)) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            if (!g_coop_lives.players[slot].pending) continue;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(g_coop_respawn[slot] - now).count();
            if (FinisherActive()) std::snprintf(labels[1].data(), kTextCapacity, "%s RESPAWN AFTER FINISHING MOVE", ParticipantLabel(slot));
            else std::snprintf(labels[1].data(), kTextCapacity, "%s RESPAWN IN %u", ParticipantLabel(slot), SecondsLeft(ms));
            break;
        }
    } else if (g_phase == Phase::Intermission || g_phase == Phase::Respawning) {
        const auto end = g_phase == Phase::Intermission ? g_intermission_end : g_respawn_end;
        const auto seconds = SecondsLeft(std::chrono::duration_cast<std::chrono::milliseconds>(end - now).count());
        if (g_phase == Phase::Respawning && FinisherActive())
            std::snprintf(labels[1].data(), kTextCapacity, "RESPAWN AFTER FINISHING MOVE");
        else if (g_phase == Phase::Intermission && g_coop && g_run.BossWave())
            std::snprintf(labels[1].data(), kTextCapacity, "BOSS COMPLETE P1 +%u %s +%u - Next wave in %u",
                          g_coop_lives.boss_bonus[0], ParticipantLabel(1), g_coop_lives.boss_bonus[1], seconds);
        else if (g_phase == Phase::Intermission && g_run.BossWave() && g_run.boss_bonus)
            std::snprintf(labels[1].data(), kTextCapacity, "BOSS COMPLETE +%u LIVES - Next wave in %u", g_run.boss_bonus, seconds);
        else std::snprintf(labels[1].data(), kTextCapacity, g_phase == Phase::Intermission ?
                              "WAVE COMPLETE - Next wave in %u" : "RESPAWN IN %u", seconds);
    } else if (g_phase == Phase::Won || g_phase == Phase::Lost) {
        if (g_phase == Phase::Won && g_coop && g_run.BossWave())
            std::snprintf(labels[1].data(), kTextCapacity, "SURVIVAL COMPLETE P1 +%u %s +%u - Start: pause menu",
                          g_coop_lives.boss_bonus[0], ParticipantLabel(1), g_coop_lives.boss_bonus[1]);
        else if (g_phase == Phase::Won && g_run.boss_bonus)
            std::snprintf(labels[1].data(), kTextCapacity, "SURVIVAL COMPLETE +%u LIFE%s - Start: pause menu",
                          g_run.boss_bonus, g_run.boss_bonus == 1 ? "" : "S");
        else std::snprintf(labels[1].data(), kTextCapacity, "%s - Start: pause menu",
                           g_phase == Phase::Won ? "SURVIVAL COMPLETE" : "SURVIVAL DEFEAT");
    } else if (g_phase == Phase::Fighting && g_reinforcement_end != std::chrono::steady_clock::time_point{}) {
        const auto seconds = SecondsLeft(std::chrono::duration_cast<std::chrono::milliseconds>(g_reinforcement_end - now).count());
        if (seconds) std::snprintf(labels[1].data(), kTextCapacity, "REINFORCEMENTS IN %u", seconds);
        else std::snprintf(labels[1].data(), kTextCapacity, "REINFORCEMENTS INCOMING");
    } else if (g_phase == Phase::Fighting && g_coop) {
        for (unsigned slot = 0; slot < 2; ++slot)
            if (!g_coop_lives.players[slot].remaining)
                std::snprintf(labels[1].data(), kTextCapacity, "%s OUT - PARTNER MUST FINISH THE WAVE", ParticipantLabel(slot));
        if (!labels[1][0] && g_run.BossWave())
            std::snprintf(labels[1].data(), kTextCapacity, "DEFEAT THE BOSS AND ALL ENEMIES");
    } else if (g_phase == Phase::Fighting && g_run.BossWave()) {
        std::snprintf(labels[1].data(), kTextCapacity, "DEFEAT THE BOSS AND ALL ENEMIES");
    }
    for (unsigned i = 0; i < 2; ++i) {
        const auto text = g_scratch + kTextStart + i * kTextStride;
        if (g_text_labels[i] != labels[i].data()) {
            std::memcpy(kernel::HostPtr(g_scratch + kTextInput), labels[i].data(), kTextCapacity);
            ctx.ecx = text;
            kernel::CallGuest(ctx, 0xDDC80, {g_scratch + kTextInput});
            g_text_labels[i] = labels[i].data();
        }
        const bool result = g_phase == Phase::Won || g_phase == Phase::Lost;
        kernel::G<float>(text + 0x78) = kernel::G<float>(hud + 0x4180) * (i ? (result ? 0.4f : 0.12f) : 0.06f);
    }
}
void DrawNativeText(X86Context& ctx, uint32_t hud) {
    if (g_text_count != 2 || g_text_hud != hud || g_phase == Phase::Failed ||
        static_cast<int32_t>(kernel::R32(0x49E93C)) > 0) return;
    const auto color = kernel::R32(hud + 0x3E24);
    if (!Address(color, 4)) return;
    for (unsigned i = 0; i < 2; ++i) {
        if (g_text_labels[i].empty()) continue;
        ctx.ecx = g_scratch + kTextStart + i * kTextStride;
        kernel::CallGuest(ctx, 0x16AB0, {color});
    }
}
struct LooseAnimations {
    const uint32_t bundled_only = kernel::R32(0x53C1D8);
    LooseAnimations() { kernel::W32(0x53C1D8, 0); }
    ~LooseAnimations() { kernel::W32(0x53C1D8, bundled_only); }
}; // 0x20CE80 otherwise rejects NAD cache misses in an arena's animation bundle.
bool Tracked(uint32_t actor) {
    if (!g_armed || (g_phase != Phase::Fighting && g_phase != Phase::Respawning) || !Address(actor, 0x408) ||
        kernel::G<uint8_t>(actor + 0x10) != 7 || kernel::R32(actor + 0x14) != g_world) return false;
    const auto handle = kernel::R32(actor + 0x34);
    for (unsigned i = 0; i < g_run.wave.spawned; ++i)
        if (g_run.wave.handles[i] == handle && !g_run.wave.dead[i]) return true;
    return false;
}
int AiHostility(uint32_t first, uint32_t second) {
    if (!g_ai_partner || !WorldMatches()) return -1;
    if (Allies(first, second)) return 0;
    if (g_coop_down[1]) return -1;
    if ((CoopSlot(first) == 1 && !Dead(first) && Tracked(second) && !Dead(second)) ||
        (CoopSlot(second) == 1 && !Dead(second) && Tracked(first) && !Dead(first))) return 1;
    return -1;
}
void Defeated(uint32_t handle, const char* source, unsigned killer = 2) {
    if (g_run.Defeat(handle)) {
        if (killer < 2) ++g_player_kills[killer];
        else ++g_shared_kills;
        LOGR("survival-kills: handle=%08X credit=%u p1=%u p2=%u shared=%u total=%u",
             handle, killer < 2 ? killer + 1 : 0, g_player_kills[0], g_player_kills[1], g_shared_kills, g_run.total_defeated);
        LOGR("survival: defeated handle=%08X wave=%u total=%u/%u run_total=%u source=%s",
             handle, g_run.number, g_run.wave.defeated, g_run.wave.target, g_run.total_defeated, source);
        Present("Defeat the remaining enemies.");
    }
}
bool EnemyHudVisible(uint32_t actor);
void DestroyEnemyHud(X86Context& ctx) {
    const X86Context saved = ctx;
    if (g_enemy_hud_constructed) {
        ctx.ecx = g_enemy_hud;
        kernel::CallGuest(ctx, 0x1A5D0, {});
        ctx.ecx = g_enemy_hud;
        kernel::CallGuest(ctx, 0x1A260, {});
    }
    if (g_enemy_hud) for (unsigned i = 0; i < 3; ++i) {
        const auto descriptor = g_enemy_hud + kEnemyResources + i * 0x1C;
        const auto resource = kernel::R32(descriptor + 0x18);
        if (resource) {
            ctx.ecx = resource;
            kernel::CallGuest(ctx, kernel::R32(kernel::R32(resource)), {1});
            kernel::W32(descriptor + 0x18, 0);
        }
    }
    g_enemy_hud_constructed = g_enemy_hud_visible = false;
    g_enemy_hud_owner = g_enemy_hud_target = 0;
    ctx = saved;
}
bool CreateEnemyHud(X86Context& ctx, uint32_t hud) {
    if (!g_coop || g_enemy_hud_constructed) return false;
    if (!g_enemy_hud) g_enemy_hud = mem::AllocSystem(kEnemyHudSize);
    if (!Address(g_enemy_hud, kEnemyHudSize) || !Address(kernel::R32(hud + 0x3E24), 4)) return false;
    std::memset(kernel::HostPtr(g_enemy_hud), 0, kEnemyHudSize);
    const X86Context saved = ctx;
    ctx.ecx = g_enemy_hud;
    kernel::CallGuest(ctx, 0x1B650, {});
    g_enemy_hud_constructed = true;
    g_enemy_hud_owner = hud;
    const auto initialize = [&]() -> bool {
        ctx.ecx = g_enemy_hud;
        kernel::CallGuest(ctx, 0x1B8B0, {3, 0x40ABDC, kernel::R32(hud + 0x3E24)});
        const auto key = g_enemy_hud + kEnemyKey, position = g_enemy_hud + kEnemyPosition;
        const float wide = (kernel::G<uint8_t>(0x53BCDC) & 4) ? float(int32_t(kernel::R32(0x40AD54))) : 0.0f;
        const auto read_position = [&](const char* name, bool required) -> bool {
            const auto length = std::snprintf(reinterpret_cast<char*>(kernel::HostPtr(key)), 64, "%s", name);
            if (length < 0 || length >= 64) return false;
            ctx.ecx = hud + 0x340;
            if (required && !kernel::CallGuest(ctx, 0x1E7D0, {key})) {
                LOGR("survival-coop: missing enemy HUD layout=%s", name); return false;
            }
            kernel::CallGuest(ctx, 0x17F00, {position, hud + 0x340, key, 0, 0, 0});
            return std::isfinite(kernel::G<float>(position)) && std::isfinite(kernel::G<float>(position + 4));
        };
        // Follow 11BA0's p3 layout, including the native widescreen offsets; P1/P2 arrays stay unchanged.
        for (unsigned i = 0; i < 18; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "pos_p3_%s", reinterpret_cast<const char*>(kernel::HostPtr(kernel::R32(0x40ACC0 + i * 8))));
            if (!read_position(name, i < 2 || i == 3)) return false;
            ctx.ecx = g_enemy_hud;
            kernel::CallGuest(ctx, 0x1A4B0, {i, std::bit_cast<uint32_t>(kernel::G<float>(position) + wide), kernel::R32(position + 4), 0, 0});
        }
        constexpr const char* textures[]{"tex_mainBarContainer_2", "tex_mainBarBackground_2", "tex_healthBar_2"};
        for (unsigned i = 0; i < 3; ++i) {
            const auto descriptor = g_enemy_hud + kEnemyResources + i * 0x1C;
            std::strcpy(reinterpret_cast<char*>(kernel::HostPtr(key)), textures[i]);
            ctx.ecx = hud + 0x340;
            if (!kernel::CallGuest(ctx, 0x1E7D0, {key})) {
                LOGR("survival-coop: missing enemy HUD texture=%s", textures[i]); return false;
            }
            ctx.ecx = hud;
            kernel::CallGuest(ctx, 0x11AC0, {key, descriptor});
            if (!Address(kernel::R32(descriptor + 0x18), 8)) return false;
            ctx.ecx = g_enemy_hud;
            kernel::CallGuest(ctx, 0x1A600, {position, i < 2 ? i : 3});
            ctx.ecx = g_enemy_hud;
            kernel::CallGuest(ctx, i < 2 ? 0x1A440 : 0x1A4F0, {
                i < 2 ? i : 1, kernel::R32(descriptor + 0x18), kernel::R32(position), kernel::R32(position + 4),
                kernel::R32(descriptor + 8), kernel::R32(descriptor + 0xC), kernel::R32(descriptor + 0x10), kernel::R32(descriptor + 0x14)});
        }
        if (!read_position("pos_p3_name", true)) return false;
        ctx.ecx = g_enemy_hud;
        kernel::CallGuest(ctx, 0x1A3E0, {0, std::bit_cast<uint32_t>(kernel::G<float>(position) + wide * 0.5f),
            kernel::R32(position + 4), kernel::R32(0x472B7C), 0x40ABDC});
        ctx.ecx = g_enemy_hud;
        kernel::CallGuest(ctx, 0x1A2F0, {});
        return kernel::R32(g_enemy_hud) == 0x1A660 && kernel::R32(g_enemy_hud + 4) == 0x1B630 &&
            kernel::G<uint8_t>(g_enemy_hud + 0x430 + 0x40C);
    };
    const bool initialized = initialize();
    if (!initialized) DestroyEnemyHud(ctx);
    ctx = saved;
    LOGR("survival-coop: campaign enemy HUD=%08X ready=%u", g_enemy_hud, unsigned(initialized));
    return initialized;
}
void UpdateEnemyHud(X86Context& ctx, uint32_t hud, uint32_t delta) {
    if (!g_enemy_hud_constructed || g_enemy_hud_owner != hud) return;
    if (g_phase != Phase::Fighting) { g_enemy_hud_visible = false; return; }
    const X86Context saved = ctx;
    const auto target = kernel::R32(0x4A2E88);
    const auto actor = target ? kernel::CallGuest(ctx, 0xAB770, {target}) : 0;
    g_enemy_hud_visible = Tracked(actor) && kernel::R32(actor + 0x34) == target && EnemyHudVisible(actor) &&
        std::isfinite(kernel::G<float>(actor + 0x330)) && kernel::G<float>(actor + 0x330) > 0 &&
        (kernel::CallGuest(ctx, 0x10F900, {}) & 0xFF);
    if (g_enemy_hud_target != (g_enemy_hud_visible ? target : 0u)) {
        g_enemy_hud_target = g_enemy_hud_visible ? target : 0u;
        if (g_enemy_hud_visible) {
            const auto name = kernel::CallGuest(ctx, 0x10FA80, {});
            ctx.ecx = g_enemy_hud;
            kernel::CallGuest(ctx, 0x1A630, {name});
        }
        LOGR("survival-coop: campaign enemy HUD target=%08X", g_enemy_hud_target);
    }
    if (g_enemy_hud_visible) {
        const auto ratio = std::clamp(kernel::G<float>(actor + 0x32C) / kernel::G<float>(actor + 0x330), 0.0f, 1.0f);
        ctx.ecx = g_enemy_hud + 0x430;
        kernel::CallGuest(ctx, 0x178D0, {std::bit_cast<uint32_t>(ratio), delta});
        const unsigned tier = ratio <= kernel::G<float>(0x40A994) ? 2 : ratio <= kernel::G<float>(0x40A990) ? 1 : 0;
        if (kernel::G<uint8_t>(g_enemy_hud + 0x8944) != tier) {
            ctx.ecx = g_enemy_hud;
            kernel::CallGuest(ctx, 0x1A980, {tier});
        }
        ctx.ecx = g_enemy_hud;
        kernel::CallGuest(ctx, 0x1A890, {delta});
    }
    ctx = saved;
}
void DrawEnemyHud(X86Context& ctx, uint32_t hud) {
    if (!g_enemy_hud_constructed || !g_enemy_hud_visible || g_enemy_hud_owner != hud || g_phase != Phase::Fighting) return;
    const X86Context saved = ctx;
    ctx.ecx = g_enemy_hud;
    kernel::CallGuest(ctx, 0x1A8E0, {});
    ctx = saved;
}
void ConfigurePlayer(X86Context& ctx) {
    const X86Context saved = ctx;
    ctx.ecx = kManager;
    kernel::CallGuest(ctx, 0x97860, {g_scratch, 0});
    kernel::W32(kPlayer + 0x24, 0); // Resolve the chosen template again, never reuse a previous character's cache.
    kernel::W32(kPlayer + 0x34, kernel::R32(kPlayer + 0x34) & ~6u);
    if (g_coop) {
        ctx.ecx = kManager;
        kernel::CallGuest(ctx, 0x97860, {g_scratch + kPartnerName, 1});
        kernel::W32(kPartner + 0x24, 0);
        kernel::W32(kPartner + 0x34, (kernel::R32(kPartner + 0x34) & ~6u) | (g_ai_partner ? 4u : 0u));
    }
    kernel::W32(kCount, g_coop ? 2 : 1);
    ctx = saved;
}
void FinishedTeardown() {
    g_replay_settings.reset();
    // Native teardown owns actor/controller/HUD destruction. Never resurrect old guest pointers.
    std::memcpy(kernel::HostPtr(kPlayer), g_player_name.data(), g_player_name.size());
    kernel::W32(kPlayer + 0x24, 0);
    kernel::W32(kPlayer + 0x34, g_player_flags);
    if (g_coop) {
        std::memcpy(kernel::HostPtr(kPartner), g_partner_name.data(), g_partner_name.size());
        kernel::W32(kPartner + 0x24, 0);
        kernel::W32(kPartner + 0x34, g_partner_flags);
        kernel::W32(kSharedCamera, g_shared_camera_before);
        g_coop = false;
        g_coop_lives = {};
        g_coop_down = {};
    g_camera_excluded_mask = ~0u;
        g_coop_actors = g_coop_handles = g_coop_controllers = g_coop_cameras = {};
    }
    for (size_t i = 0; i < kCampaignScalars.size(); ++i) kernel::W32(kCampaignScalars[i], g_campaign[i]);
    g_armed = false;
    g_ai_partner = false;
    g_arena = &Arenas[DefaultArena];
    g_characters = kDefaultSelection;
    g_managed_audio = false;
    g_native_player_death = false;
    g_lives_hud = 0;
    g_protected_until = {};
    g_reinforcement_end = {};
    g_world = g_scene = 0;
    g_clock_ready = false;
    g_result_clock_frozen = false;
    g_result_seconds = 0;
    g_native_exit_requested = false;
    g_hero_actor = g_hero_handle = 0;
    g_finisher = {};
    g_sound_owner = 0;
    g_owned_sounds = {};
    g_run = {};
    g_player_kills = {};
    g_shared_kills = 0;
    State(Phase::Off, "");
    pc_development::Complete("SURVIVAL ENDED; LOAD A CAMPAIGN SAVE BEFORE STORY");
}
bool WorldMatches() {
    return Address(g_world, 0x140) && kernel::R32(kWorld) == g_world &&
        NameIs(g_world + 0x30, g_arena->world) && NameIs(0x4F40EC, g_arena->scene) &&
        !kernel::R32(0x4F42A0) && !kernel::R32(0x4F42A4) && !kernel::R32(0x4F42A8);
}
unsigned CameraSurvivor() {
    if (!g_armed || !g_coop || !WorldMatches() || g_coop_cameras[0] == g_coop_cameras[1] ||
        CoopSlot(Player(0)) != 0 || CoopSlot(Player(1)) != 1) return 2;
    for (unsigned slot = 0; slot < 2; ++slot)
        if (kernel::R32(kPlayer + slot * 0xB0 + 0x2C) != g_coop_cameras[slot] ||
            !Address(g_coop_cameras[slot], 0x400)) return 2;
    const bool first = !g_coop_down[0] && !Dead(Player(0));
    const bool second = !g_coop_down[1] && !Dead(Player(1));
    return first == second ? 2u : first ? 0u : 1u;
}
struct NativeCameraScope {
    bool active = false, input_selected = false;
    unsigned slot = 2;
    uint32_t index = 0, record = 0, shared = 0, input = 0;
    explicit NativeCameraScope(const X86Context& ctx) {
        if (ctx.ecx != kManager) return;
        slot = CameraSurvivor();
        if (slot >= 2) return;
        index = kernel::R32(kManager + 0x598);
        record = kernel::R32(kManager + 0x744);
        shared = kernel::R32(kSharedCamera);
        active = true;
        kernel::W32(kManager + 0x598, slot);
        kernel::W32(kManager + 0x744, kPlayer + slot * 0xB0);
        kernel::W32(kSharedCamera, 0);
    }
    void SelectInput(X86Context& ctx) {
        if (!active) return;
        input = kernel::R32(0x5590E4);
        input_selected = true;
        const X86Context saved = ctx;
        kernel::CallGuest(ctx, 0x27AE10, {kernel::R16(kPlayer + (g_ai_partner ? 0u : slot) * 0xB0 + 0x3C)});
        ctx = saved;
    }
    ~NativeCameraScope() {
        if (!active) return;
        if (input_selected) kernel::W32(0x5590E4, input);
        kernel::W32(kManager + 0x598, index);
        kernel::W32(kManager + 0x744, record);
        kernel::W32(kSharedCamera, shared);
    }
};
bool OpenNativePause(X86Context& ctx, unsigned slot) {
    if (!WorldMatches() || kernel::R32(0x4C3444) != 3 || slot >= (g_coop ? 2u : 1u) || (g_ai_partner && slot == 1)) return false;
    const auto port = kernel::R32(kPlayer + slot * 0xB0 + 0x3C);
    if (port > 1) return false;
    const X86Context saved = ctx;
    kernel::W32(0x483960, port);
    kernel::W32(0x483964, slot);
    kernel::W32(0x48393C, 0);
    kernel::W32(0x483814, 1);
    // Match the native pause prologue: text uses bit 4, deferred vectors need bit 8.
    if (kernel::R32(0x53BCDC) & 4) kernel::CallGuest(ctx, 0x1CEEB0, {1});
    kernel::W32(0x48397C, 0); // Offline pause freezes gameplay while APT keeps running.
    ctx.ecx = 0x483800;
    kernel::CallGuest(ctx, 0xB2130, {});
    ctx = saved;
    LOGR("survival: native pause player=%u port=%u mode=%u clock_paused=%u", slot + 1,
         port, kernel::R32(0x4C3444), kernel::R32(0x49E93C));
    return true;
}
void Leave(X86Context& ctx) {
    const auto native_state = kernel::R32(0x4C3444);
    LOGR("survival: exit requested native_state=%u next=%u world=%08X", native_state,
         kernel::R32(0x4C343C), kernel::R32(kWorld));
    if (kernel::R32(kWorld) && native_state != 3 && native_state != 4 && native_state != 5) {
        State(Phase::Failed, "Still transitioning. Try Return to main menu again.");
        return;
    }
    if (native_state == 5) {
        if (!WorldMatches()) { State(Phase::Failed, "Pause session changed. Try Return to main menu again."); return; }
        const X86Context saved = ctx;
        kernel::W32(0x483814, 0);
        kernel::CallGuest(ctx, 0x1CEEB0, {0});
        ctx.ecx = 0x483800;
        kernel::CallGuest(ctx, 0xB21A0, {});
        ctx = saved;
    }
    State(Phase::Leaving, "Returning to menu...");
    // Native state 1 tears down gameplay and opens the frontend. State 6 waits for APT's network/menu fade.
    kernel::W32(0x4C343C, 1);
}
uint32_t ArenaStartPoint(const ArenaPoint& anchor) {
    if (!WorldMatches() || !Address(g_scene, 4)) return 0;
    const auto count = kernel::R32(g_world + 0x10), points = kernel::R32(g_world + 0x13C);
    if (!count || count > 128 || !Address(points, count * 0x50)) return 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto point = points + i * 0x50;
        if (kernel::R32(point) != g_scene || kernel::R32(point + 8) != anchor.id) continue;
        bool matches = true;
        for (unsigned n = 0; n < 3; ++n) {
            const auto value = kernel::G<float>(point + 0x40 + n * 4);
            matches &= std::isfinite(value) && std::abs(value - anchor.position[n]) <= 0.02f;
        }
        if (!matches) continue;
        for (unsigned n = 0; n < 16; ++n) {
            const auto value = kernel::G<float>(point + 0x10 + n * 4);
            if (!std::isfinite(value) || std::abs(value) > 10000.0f) return 0;
        }
        return point;
    }
    return 0;
}
bool HeroEnemy(uint32_t actor) {
    return g_armed && g_phase != Phase::Leaving && g_hero_handle && actor == g_hero_actor &&
        Address(actor, 0x408) && kernel::G<uint8_t>(actor + 0x10) == 7 &&
        kernel::R32(actor + 0x34) == g_hero_handle && kernel::R32(actor + 0x14) == g_world &&
        kernel::R32(kWorld) == g_world && !kernel::R32(actor + 0x30C);
}
bool EnemyHudVisible(uint32_t actor) {
    return !Dead(actor) && std::isfinite(kernel::G<float>(actor + 0x32C)) &&
        !(kernel::R32(actor + 0x3C) & 0x20000);
}
uint32_t SoundBankEntry(const char* name) {
    if (!g_sound_owner || kernel::R32(0x512094) != g_sound_owner || !Address(g_sound_owner, 8)) return 0;
    const auto count = kernel::R32(g_sound_owner), entries = kernel::R32(g_sound_owner + 4);
    if (!count || count > 128 || !Address(entries, count * 0x3C)) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto entry = entries + i * 0x3C;
        if (kernel::G<uint8_t>(entry) && NameIs(entry + 0x1C, name)) return entry;
    }
    return 0;
}
bool LoadBank(X86Context& ctx, unsigned index) {
    const auto* name = SoundBankName(index, *g_arena);
    const auto banks = kernel::R32(0x512094); // Same character-bank owner used by 0xEF190.
    if (banks != g_sound_owner || !Address(banks, 8) || (g_managed_audio && !AudioPoolReady())) return false;
    const auto count = kernel::R32(banks), entries = kernel::R32(banks + 4);
    if (!count || count > 128 || !Address(entries, count * 0x3C)) return false;
    if (SoundBankEntry(name)) return true;
    unsigned occupied = 0;
    for (uint32_t i = 0; i < count; ++i) occupied += kernel::G<uint8_t>(entries + i * 0x3C) != 0;
    if (occupied == count) {
        LOGR("survival: sound_bank=%s loaded=0 reason=no_free_slot slots=%u/%u", name, occupied, count);
        return false;
    }
    std::memset(kernel::HostPtr(g_scratch + 64), 0, 32);
    std::memcpy(kernel::HostPtr(g_scratch + 64), name, std::strlen(name));
    const X86Context saved = ctx;
    ctx.ecx = banks;
    {
        const AudioAllocationScope allocation(g_managed_audio);
        kernel::CallGuest(ctx, 0xF2F90, {g_scratch + 64, 1});
    }
    ctx = saved;
    const bool loaded = SoundBankEntry(name) != 0;
    if (loaded) g_owned_sounds[index] = true;
    LOGR("survival: sound_bank=%s loaded=%u slots_before=%u/%u", name, unsigned(loaded), occupied, count);
    return loaded;
}
bool CheckHeroResources(unsigned hero, std::string& error) {
    const auto& resource = kHeroResources[hero];
    const auto require = [&error](const std::string& path) {
        std::vector<uint8_t> data;
        if (kernel::ReadGameFile(path, data) && !data.empty()) return true;
        error = "SURVIVAL HERO RESOURCE MISSING: " + path;
        return false;
    };
    for (const auto* suffix : {"_alt.nod", "_alt_phys.nod", "_alt_shadow.nod"})
        if (!require(std::string("D:\\Models\\") + resource.model + suffix)) return false;
    for (const auto* motion : {resource.motion, resource.base_motion})
        if (!require(std::string("D:\\Motions\\") + motion)) return false;
    if (!require(std::string("D:\\Misc\\") + resource.nvb)) return false;
    for (const auto* animation : resource.animations)
        if (!require(std::string("D:\\Anims\\") + animation + ".nad")) return false;
    return true;
}
bool WaveSounds(X86Context& ctx) {
    if (!g_armed || g_run.wave.spawned || !WorldMatches() || !Player()) return false;
    const auto banks = kernel::R32(0x512094);
    if (!Address(banks, 8) || (g_sound_owner && g_sound_owner != banks)) return false;
    const auto count = kernel::R32(banks), entries = kernel::R32(banks + 4);
    if (!count || count > 128 || !Address(entries, count * 0x3C)) return false;
    g_sound_owner = banks;
    const auto hero = HeroForWave(g_run.number, g_hero_order);
    const auto required = RequiredSounds(g_run, hero, g_coop, g_characters, g_managed_audio);
    for (unsigned i = 0; i < required.size(); ++i) {
        if (required[i] || !g_owned_sounds[i]) continue;
        const auto* name = SoundBankName(i, *g_arena);
        if (const auto entry = SoundBankEntry(name)) {
            // F3210 reads only count/table; a one-entry owner view confines native cleanup to our unused bank.
            kernel::W32(g_scratch + 64, 1);
            kernel::W32(g_scratch + 68, entry);
            const X86Context saved = ctx;
            ctx.ecx = g_scratch + 64;
            kernel::CallGuest(ctx, 0xF3210, {});
            ctx = saved;
            if (SoundBankEntry(name)) return false;
            LOGR("survival: sound_bank=%s released=1 wave=%u", name, g_run.number);
        }
        g_owned_sounds[i] = false;
    }
    for (unsigned i = 0; i < required.size(); ++i)
        if (required[i] && !LoadBank(ctx, i)) return false;
    unsigned occupied = 0;
    for (uint32_t i = 0; i < count; ++i) occupied += kernel::G<uint8_t>(entries + i * 0x3C) != 0;
    LOGR("survival: wave_audio ready=1 wave=%u boss=%s slots=%u used=%u coop=%u", g_run.number,
         g_run.FinalBossWave() ? "Roekel" : g_run.HeroWave() ? kHeroBosses[hero].label : "none",
         count, occupied, unsigned(g_coop));
    return true;
}
bool SpawnEnemies(X86Context& ctx, unsigned amount) {
    const auto player = Player();
    if (!WorldMatches() || !player || kernel::R32(player + 0x14) != g_world) {
        State(Phase::Failed, "Wave session validation failed. Return to menu."); return false;
    }
    if (!amount || g_run.wave.Alive() >= g_run.wave.active_limit ||
        amount > g_run.wave.active_limit - g_run.wave.Alive() || amount > g_run.wave.Pending()) {
        State(Phase::Failed, "Invalid reinforcement quota. Return to menu."); return false;
    }
    const X86Context saved = ctx;
    std::array<std::array<float, 2>, Wave::MaxActive> occupied{};
    unsigned alive = 0;
    for (unsigned i = 0; i < g_run.wave.spawned; ++i) {
        if (g_run.wave.dead[i]) continue;
        const auto enemy = kernel::CallGuest(ctx, 0xAB770, {g_run.wave.handles[i]});
        if (!Address(enemy, 0x408) || kernel::R32(enemy + 0x34) != g_run.wave.handles[i] ||
            kernel::R32(enemy + 0x14) != g_world || kernel::R32(enemy + 0x30C) || !std::isfinite(kernel::G<float>(enemy + 0xC0)) ||
            !std::isfinite(kernel::G<float>(enemy + 0xC4))) {
            ctx = saved;
            State(Phase::Failed, "Enemy position unavailable for reinforcement. Return to menu."); return false;
        }
        occupied[alive++] = {kernel::G<float>(enemy + 0xC0), kernel::G<float>(enemy + 0xC4)};
    }
    ctx = saved;
    std::array<uint32_t, Wave::MaxActive> matrices{};
    std::array<uint32_t, Wave::MaxActive> chosen{};
    unsigned selected = 0;
    const auto px = kernel::G<float>(player + 0xC0), py = kernel::G<float>(player + 0xC4);
    if (!std::isfinite(px) || !std::isfinite(py)) {
        State(Phase::Failed, "Invalid player position. Return to menu."); return false;
    }
    for (const auto& anchor : g_arena->enemies) {
        const auto point = ArenaStartPoint(anchor);
        if (!point) {
            State(Phase::Failed, "Arena spawn layout mismatch. Return to menu."); return false;
        }
        const auto dx = anchor.position[0] - px, dy = anchor.position[1] - py;
        bool available = dx * dx + dy * dy >= 9.0f;
        if (g_coop) {
            const auto partner = Player(1);
            if (!partner || !std::isfinite(kernel::G<float>(partner + 0xC0)) ||
                !std::isfinite(kernel::G<float>(partner + 0xC4))) {
                State(Phase::Failed, "Coop partner position unavailable. Return to menu."); return false;
            }
            const auto dx2 = anchor.position[0] - kernel::G<float>(partner + 0xC0);
            const auto dy2 = anchor.position[1] - kernel::G<float>(partner + 0xC4);
            available &= dx2 * dx2 + dy2 * dy2 >= 9.0f;
        }
        for (unsigned n = 0; n < alive; ++n) {
            const auto ex = anchor.position[0] - occupied[n][0], ey = anchor.position[1] - occupied[n][1];
            if (ex * ex + ey * ey < 4.0f) available = false;
        }
        if (selected < amount && available) {
            matrices[selected] = point + 0x10;
            chosen[selected++] = (point - kernel::R32(g_world + 0x13C)) / 0x50;
        }
    }
    if (selected != amount) return false; // Keep the quota pending while every validated point is occupied.
    const LooseAnimations animation_source;
    LOGR("survival: spawn animation source allows loose NAD (previous bundled_only=%u)", animation_source.bundled_only);
    for (unsigned i = 0; i < amount; ++i) {
        const auto type = g_run.NextEnemy();
        const auto definition = EnemyFor(type, HeroForWave(g_run.number, g_hero_order));
        const auto* name = definition.label;
        std::memset(kernel::HostPtr(g_scratch + kEnemyName), 0, 32);
        std::memcpy(kernel::HostPtr(g_scratch + kEnemyName), definition.actor, std::strlen(definition.actor) + 1);
        const auto ordinal = g_run.wave.spawned + 1;
        const auto enemy_template = kernel::CallGuest(ctx, 0xDCFC0, {g_scratch + kEnemyName, 0});
        if (!Address(enemy_template, 0x30)) {
            ctx = saved;
            State(Phase::Failed, "Enemy template unavailable. Return to menu."); return false;
        }
        // Use the scene's original transform for ground enemies, drones and bosses.
        const auto enemy = kernel::CallGuest(ctx, 0xAD800, {enemy_template, g_world, g_scene, matrices[i], 0});
        if (Player() != player || !Address(enemy, 0x408) || kernel::G<uint8_t>(enemy + 0x10) != 7 ||
            kernel::R32(enemy + 0x14) != g_world || kernel::R32(enemy + 0x30C) || !std::isfinite(kernel::G<float>(enemy + 0x32C)) ||
            kernel::G<float>(enemy + 0x32C) <= 0 || !g_run.wave.Add(kernel::R32(enemy + 0x34))) {
            ctx = saved;
            State(Phase::Failed, "Enemy creation failed; wave not completed. Return to menu."); return false;
        }
        if (!Address(kernel::R32(enemy + 0x3EC), 4) || !Address(kernel::R32(enemy + 0x390), 4) ||
            !Address(kernel::R32(enemy + 0x1D0), 4)) {
            ctx = saved;
            State(Phase::Failed, "Enemy controller, AI or motions unavailable. Return to menu."); return false;
        }
        if (type == Run::Enemy::HeroBoss) {
            const auto hero = HeroForWave(g_run.number, g_hero_order);
            if (kernel::R16(enemy + 0x394) != kHeroResources[hero].ai) {
                ctx = saved;
                State(Phase::Failed, "Hero native AI does not match the roster. Return to menu."); return false;
            }
            LOGR("survival: hero native AI=%u roster=%s npc_record=%08X",
                 kernel::R16(enemy + 0x394), definition.actor, kernel::R32(enemy + 0x30C));
        }
        LOGR("survival: spawn=%u type=%s handle=%08X actor=%08X startpoint=%u position=%.3f,%.3f,%.3f",
             ordinal, name, kernel::R32(enemy + 0x34), enemy, chosen[i],
             kernel::G<float>(enemy + 0xC0), kernel::G<float>(enemy + 0xC4), kernel::G<float>(enemy + 0xC8));
        LOGR("survival: initialized enemy=%08X controller=%08X ai=%08X motion_set=%08X",
             kernel::R32(enemy + 0x34), kernel::R32(enemy + 0x3EC), kernel::R32(enemy + 0x390), kernel::R32(enemy + 0x1D0));
        LOGR("survival: population wave=%u alive=%u cap=%u spawned=%u target=%u", g_run.number,
             g_run.wave.Alive(), g_run.wave.active_limit, g_run.wave.spawned, g_run.wave.target);
        if (type == Run::Enemy::HeroBoss || type == Run::Enemy::FinalBoss) {
            g_hero_actor = enemy;
            g_hero_handle = kernel::R32(enemy + 0x34);
            LOGR("survival: hero enemy HUD actor=%08X handle=%08X record=%08X flags310=%08X",
                 enemy, g_hero_handle, kernel::R32(enemy + 0x30C), kernel::R32(enemy + 0x310));
            kernel::CallGuest(ctx, 0x10FDF0, {kPlayer, enemy});
        }
    }
    ctx = saved;
    return true;
}
void SpawnWave(X86Context& ctx) {
    g_run.wave.active_limit = g_run.ActiveLimit(g_coop);
    g_reinforcement_end = {};
    g_hero_actor = g_hero_handle = 0;
    const auto& hero = kHeroBosses[HeroForWave(g_run.number, g_hero_order)];
    if (g_run.HeroWave())
        LOGR("survival: hero_boss encounter wave=%u selected=%s template=%s", g_run.number, hero.label, hero.actor);
    if (g_run.FinalBossWave()) LOGR("survival: final_boss encounter wave=%u template=act_Roekel", g_run.number);
    if (!WaveSounds(ctx)) {
        State(Phase::Failed, "Wave sound resources unavailable. Return to menu."); return;
    }
    // Preserve the Mansion cadence on every arena: three initial enemies and later reinforcements.
    // Reuse free anchors one at a time, after earlier actors move away.
    // Co-op starts with two on ordinary waves, leaving room for both participants in the Mansion.
    const auto initial = g_coop && !g_run.BossWave() ? 2u : g_run.InitialEnemies();
    LOGR("survival: encounter wave=%u target=%u boss=%u initial=%u active_limit=%u", g_run.number,
         g_run.wave.target, unsigned(g_run.BossWave()), initial, g_run.wave.active_limit);
    if (!SpawnEnemies(ctx, initial)) {
        if (g_phase != Phase::Failed) State(Phase::Failed, "Not enough safe spawn points. Return to menu.");
        return;
    }
    State(Phase::Fighting, g_run.BossWave() ? "Boss wave: defeat the boss and all reinforcements." :
          "Defeat all enemies to clear this wave.");
    const auto music = kernel::R32(0x5148C4);
    if (kernel::R32(0x47F364) && Address(music, 0x2F8)) {
        const X86Context saved = ctx;
        // Reuse the native Versus project pool and music controller, including its volume and pause handling.
        auto project = kernel::R32(0x4890BC);
        if (project < 1 || project > 8) {
            project = (kernel::R32(0x53BC5C) * 0x19660Du + 0x3C6EF35Fu);
            kernel::W32(0x53BC5C, project);
            project = (project & 7u) + 1u;
            kernel::CallGuest(ctx, 0xF5D90, {project});
        }
        ctx.ecx = music;
        kernel::CallGuest(ctx, 0xF7F00, {g_run.BossWave() ? 14u : 13u});
        ctx = saved;
        LOGR("survival-music: wave=%u project=%u event=%s", g_run.number, project,
             g_run.BossWave() ? "GME_boss_fight" : "GME_peon_fight");
    }
}
void EndBossMusic(X86Context& ctx) {
    if (!g_run.BossWave()) return;
    const auto music = kernel::R32(0x5148C4);
    if (!kernel::R32(0x47F364) || !Address(music, 0x2F8)) return;
    const X86Context saved = ctx;
    const auto previous = kernel::R32(music + 0x2D0);
    // The project has a separate boss-fight-end event; peon-fight alone leaves the boss stream playing.
    ctx.ecx = music;
    kernel::CallGuest(ctx, 0xF7F00, {16u});
    ctx = saved;
    LOGR("survival-music: wave=%u event=GME_boss_fight_end previous=%u current=%u",
         g_run.number, previous, kernel::R32(music + 0x2D0));
}
void Loaded(X86Context& ctx) {
    g_world = kernel::R32(kWorld);
    const auto player = Player();
    if (!WorldMatches() || !player || kernel::R32(player + 0x14) != g_world) {
        State(Phase::Failed, "Load validation failed. Return to menu."); return;
    }
    if (g_replay_settings)
        for (unsigned slot = 0; slot < (g_coop ? 2u : 1u); ++slot)
            g_replay_settings->controllers[slot] = kernel::R32(kPlayer + slot * 0xB0 + 0x3C);
    g_clock_ticks = kernel::R32(0x49E924);
    g_clock_elapsed = {};
    g_clock_ready = true;
    g_scene = kernel::R32(player + 0x24);
    const auto hud = kernel::R32(0x4A2E50);
    if (g_lives_hud != hud || !Address(hud, 0x418C) || !kernel::G<uint8_t>(hud + 0x4188) ||
        kernel::G<uint8_t>(hud + 0x360)) {
        State(Phase::Failed, "Native lives HUD unavailable. Return to menu."); return;
    }
    if (g_coop) {
        if (!kernel::G<uint8_t>(hud + 0x4189) || kernel::R32(hud + 0x350) != 2) {
            State(Phase::Failed, "Coop native HUD layout unavailable. Return to menu."); return;
        }
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto actor = Player(slot), record = kPlayer + slot * 0xB0;
            if (!actor || kernel::R32(actor + 0x14) != g_world || kernel::R32(actor + 0x24) != g_scene ||
                !std::isfinite(kernel::G<float>(actor + 0x32C)) || Dead(actor) ||
                !Address(kernel::R32(actor + 0x3EC), 0x444) || !Address(kernel::R32(record + 0x2C), 4) ||
                kernel::R32(kernel::R32(actor + 0x3EC) + 0xF4) != actor ||
                (kernel::R32(record + 0x34) & 6) != (g_ai_partner && slot == 1 ? 4u : 0u) ||
                kernel::R32(record + 0x3C) > 1) {
                State(Phase::Failed, "Coop participant initialization failed. Return to menu."); return;
            }
            g_coop_actors[slot] = actor;
            g_coop_handles[slot] = kernel::R32(actor + 0x34);
            g_coop_controllers[slot] = kernel::R32(actor + 0x3EC);
            g_coop_cameras[slot] = kernel::R32(record + 0x2C);
            LOGR("survival-coop: player=%u actor=%08X handle=%08X record=%08X controller=%08X input_port=%u camera=%08X",
                 slot + 1, actor, g_coop_handles[slot], record, g_coop_controllers[slot], kernel::R32(record + 0x3C), g_coop_cameras[slot]);
        }
        if (g_coop_actors[0] == g_coop_actors[1] || g_coop_handles[0] == g_coop_handles[1] ||
            g_coop_controllers[0] == g_coop_controllers[1] || kernel::R32(kPlayer + 0x3C) == kernel::R32(kPartner + 0x3C)) {
            State(Phase::Failed, "Coop participants share an identity or input. Return to menu."); return;
        }
        if (g_ai_partner) {
            const auto actor = g_coop_actors[1], ai = kernel::R32(actor + 0x390);
            kernel::W32(actor + 0x188, 2);
            if (ai) {
                if (!Address(ai, 0x5F0) || kernel::R32(ai + 0x530) != actor) {
                    State(Phase::Failed, "Coop partner AI identity mismatch. Return to menu."); return;
                }
                const X86Context saved = ctx;
                ctx.ecx = ai;
                kernel::CallGuest(ctx, 0x6F860, {}); // Refresh parameters if activation created AI before Loaded.
                ctx = saved;
            }
            LOGR("survival-ai: partner ready actor=%08X ai=%08X column=2 flags=%08X", actor, ai, kernel::R32(kPartner + 0x34));
        }
        // Native BA180 selects shared framing through this script flag, without activating versus rules.
        kernel::W32(kSharedCamera, 1);
        LOGR("survival-coop: shared_camera=1 participants=2 start_wave=%u total_waves=%u target=%u",
             g_run.number, Run::TotalWaves, g_run.wave.target);
    }
    LOGR("survival: native lives HUD=%08X icons=%u", hud, g_run.lives.Icons(0));
    ReportGrabActor("loaded-player", player);
    if (!CreateNativeText(ctx, hud)) {
        State(Phase::Failed, "Native Survival text unavailable. Return to menu."); return;
    }
    if (g_coop) {
        if (!g_enemy_hud_constructed || g_enemy_hud_owner != hud || !Address(g_enemy_hud, kEnemyHudSize)) {
            State(Phase::Failed, "Native enemy HUD unavailable. Return to menu."); return;
        }
        // The two-panel HUD listens for versus start; Survival keeps the campaign bootstrap.
        const X86Context saved = ctx;
        const auto hidden = !kernel::G<uint8_t>(hud + 0x3C91);
        ctx.ecx = hud;
        kernel::CallGuest(ctx, 0x11570, {});
        ctx = saved;
        LOGR("survival-coop: native HUD activated hud=%08X was_hidden=%u visible=%u requested=%u update=%08X draw=%08X",
             hud, unsigned(hidden), unsigned(kernel::G<uint8_t>(hud + 0x3C91)),
             unsigned(kernel::G<uint8_t>(hud + 0x3C90)), kernel::R32(hud), kernel::R32(hud + 4));
        if (!kernel::G<uint8_t>(hud + 0x3C90) || !kernel::G<uint8_t>(hud + 0x3C91)) {
            State(Phase::Failed, "Coop native HUD activation failed. Return to menu."); return;
        }
    }
    LOGR("survival: loaded world=%08X scene=%08X player=%08X position=%.3f,%.3f,%.3f",
         g_world, g_scene, player, kernel::G<float>(player + 0xC0),
         kernel::G<float>(player + 0xC4), kernel::G<float>(player + 0xC8));
    SpawnWave(ctx);
}
bool Respawn(X86Context& ctx, uint32_t player, unsigned slot = 0) {
    if (slot >= (g_coop ? 2u : 1u)) return false;
    const auto record = kPlayer + slot * 0xB0;
    const auto& anchor = g_arena->players[slot];
    const auto point = ArenaStartPoint(anchor);
    const auto controller = kernel::R32(player + 0x3EC), camera = kernel::R32(record + 0x2C);
    if (!point ||
        !Address(controller, 0x1C8) || !Address(camera, 4) ||
        kernel::R32(controller + 0xF4) != player || kernel::R32(player + 0x24) != g_scene) return false;
    const auto handle = kernel::R32(player + 0x34);
    const auto render_before = kernel::R32(player + 0x40), effects_before = kernel::R32(player + 0x16C);
    const X86Context saved = ctx;
    g_protected_until = RunNow() + std::chrono::seconds(2);
    // Native cleanup keeps persistent hero FX (0x4000) and detaches transient death effects.
    ctx.ecx = player;
    kernel::CallGuest(ctx, 0x213140, {1});
    ctx.ecx = player;
    const auto revived = kernel::CallGuest(ctx, 0x2F3B0, {0x3F800000, 0x3F800000, 0, 1});
    if (revived && Player(slot) == player) {
        struct RestoreSlot {
            uint32_t record, previous;
            ~RestoreSlot() { kernel::W32(record + 0x38, previous); }
        } restore_slot{record, kernel::R32(record + 0x38)};
        kernel::W32(record + 0x38, anchor.id);
        ctx.ecx = record;
        kernel::CallGuest(ctx, 0x98550, {g_world, 0});
    }
    ctx = saved;
    if (!revived || Player(slot) != player || !WorldMatches() || Dead(player) ||
        kernel::R32(player + 0x34) != handle || kernel::R32(player + 0x14) != g_world ||
        kernel::R32(player + 0x3EC) != controller || kernel::R32(record + 0x2C) != camera ||
        kernel::R32(controller + 0xF4) != player || !std::isfinite(kernel::G<float>(player + 0x32C))) return false;
    for (unsigned n = 0; n < 3; ++n)
        if (!std::isfinite(kernel::G<float>(player + 0xC0 + n * 4))) return false;
    // Match native round reset (0x9DAFC): remove the body-hide bit after death FX cleanup.
    kernel::W32(player + 0x40, kernel::R32(player + 0x40) & ~0x200u);
    LOGR("survival: respawn actor=%08X handle=%08X hp=%.1f flags=%08X controller=%08X state=%u position=%.3f,%.3f,%.3f protection=2s render=%08X->%08X effects=%08X->%08X",
         player, handle, kernel::G<float>(player + 0x32C), kernel::R32(player + 0x314), controller,
         kernel::G<uint8_t>(controller + 0x18), kernel::G<float>(player + 0xC0),
         kernel::G<float>(player + 0xC4), kernel::G<float>(player + 0xC8), render_before,
         kernel::R32(player + 0x40), effects_before, kernel::R32(player + 0x16C));
    return true;
}
}

static bool PrepareSelected(X86Context& ctx, std::array<unsigned, 2> initial_lives, std::string& error, unsigned start_wave,
                            bool cooperative, SelectedParty characters, bool managed_audio, unsigned venue, bool ai_partner = false,
                            unsigned difficulty = 0, bool endless = false) {
    if (!AllowedDifficulty(difficulty)) { error = "SURVIVAL DIFFICULTY INVALID"; return false; }
    if (ai_partner && !cooperative) { error = "SURVIVAL AI PARTNER REQUIRES TWO PARTICIPANTS"; return false; }
    const auto* arena = ArenaFor(venue);
    if (!arena) { error = "SURVIVAL ARENA SELECTION INVALID"; return false; }
    if (!Run::AllowedStart(start_wave)) {
        error = "SURVIVAL STARTING WAVE MUST BE 1 TO " + std::to_string(Run::TotalWaves); return false;
    }
    if (g_armed || !Address(kManager, 0x790) || kernel::R32(0x4F42A0) ||
        kernel::R32(0x4F42A4) || kernel::R32(0x4F42A8)) {
        error = "RETURN TO MAIN MENU BEFORE SURVIVAL"; return false;
    }
    const auto card = kernel::R32(kMemcard);
    if (!Address(card, 0x740) || kernel::G<uint8_t>(card + 0x6E7) ||
        kernel::R32(card + 0x6CC) != 0xFFFFFFFFu) {
        error = "WAIT FOR THE CURRENT STORAGE OPERATION"; return false;
    }
    for (unsigned i = 0; i < kSoundBanks.size(); ++i) {
        const auto* bank = SoundBankName(i, *arena);
        std::vector<uint8_t> data;
        if (!kernel::ReadGameFile(std::string("D:\\Sounds\\") + bank + ".abk", data) || data.empty()) {
            error = std::string("SURVIVAL SOUND BANK MISSING: ") + bank; return false;
        }
    }
    for (const auto* animation : kEnemyAnimations) {
        std::vector<uint8_t> data;
        if (!kernel::ReadGameFile(std::string("D:\\Anims\\") + animation + ".nad", data) || data.empty()) {
            error = std::string("SURVIVAL ANIMATION MISSING: ") + animation; return false;
        }
    }
    for (const auto* path : kEnemyFiles) {
        std::vector<uint8_t> data;
        if (!kernel::ReadGameFile(path, data) || data.empty()) {
            error = std::string("SURVIVAL ENEMY RESOURCE MISSING: ") + path; return false;
        }
    }
    if (g_text_count) { error = "RETURN TO MAIN MENU BEFORE SURVIVAL"; return false; }
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchSize);
    if (!Address(g_scratch, kScratchSize)) { error = "SURVIVAL MEMORY UNAVAILABLE"; return false; }
    if (managed_audio && !EnsureAudioPool(ctx)) { error = "COOP AUDIO MEMORY UNAVAILABLE"; return false; }
    // Select the hero order once per attempt; respawns and reinforcements retain each encounter's template.
    static std::mt19937 boss_random(std::random_device{}());
    g_hero_order[0] = std::uniform_int_distribution<unsigned>(0, unsigned(kHeroBosses.size()) - 1)(boss_random);
    const auto other = std::uniform_int_distribution<unsigned>(0, unsigned(kHeroBosses.size()) - 2)(boss_random);
    g_hero_order[1] = OtherHero(g_hero_order[0], other);
    for (const auto hero : g_hero_order)
        if (!CheckHeroResources(hero, error)) return false;
    std::memset(kernel::HostPtr(g_scratch), 0, kScratchSize);
    const auto first_character = characters[0].actor;
    const auto second_character = characters[1].actor;
    std::memcpy(kernel::HostPtr(g_scratch), first_character, std::strlen(first_character) + 1);
    const X86Context saved = ctx;
    static_assert(kPartnerName + 32 <= kScratchSize);
    std::memcpy(kernel::HostPtr(g_scratch + kPartnerName), second_character, std::strlen(second_character) + 1);
    const auto player_template = kernel::CallGuest(ctx, 0xDCFC0, {g_scratch, 0});
    bool templates_ready = Address(player_template, 0x30);
    if (cooperative)
        templates_ready &= Address(kernel::CallGuest(ctx, 0xDCFC0, {g_scratch + kPartnerName, 0}), 0x30);
    for (unsigned type = 0; type <= unsigned(Run::Enemy::Flyer3); ++type) {
        for (unsigned slot = 0; slot < (type == unsigned(Run::Enemy::HeroBoss) ? 2u : 1u); ++slot) {
            const auto definition = EnemyFor(Run::Enemy(type), g_hero_order[slot]);
            std::memset(kernel::HostPtr(g_scratch + kEnemyName), 0, 32);
            std::memcpy(kernel::HostPtr(g_scratch + kEnemyName), definition.actor, std::strlen(definition.actor) + 1);
            const auto enemy_template = kernel::CallGuest(ctx, 0xDCFC0, {g_scratch + kEnemyName, 0});
            templates_ready = templates_ready && Address(enemy_template, 0x30);
            if (!Address(enemy_template, 0x30)) LOGR("survival: template unavailable name=%s", definition.actor);
        }
    }
    ctx = saved;
    if (!templates_ready) {
        error = "SURVIVAL CHARACTER TEMPLATES UNAVAILABLE"; return false;
    }
    LOGR("survival: hero_boss order wave10=%s wave15=%s pool=%u", kHeroBosses[g_hero_order[0]].label,
         kHeroBosses[g_hero_order[1]].label, unsigned(kHeroBosses.size()));
    std::memcpy(g_player_name.data(), kernel::HostPtr(kPlayer), g_player_name.size());
    g_player_flags = kernel::R32(kPlayer + 0x34);
    if (cooperative) {
        std::memcpy(g_partner_name.data(), kernel::HostPtr(kPartner), g_partner_name.size());
        g_partner_flags = kernel::R32(kPartner + 0x34);
        g_shared_camera_before = kernel::R32(kSharedCamera);
    }
    for (size_t i = 0; i < kCampaignScalars.size(); ++i) g_campaign[i] = kernel::R32(kCampaignScalars[i]);
    if (!g_run.Start(start_wave, 3, difficulty, endless)) { error = "SURVIVAL START PARAMETERS INVALID"; return false; }
    g_run.lives = InitialStock(initial_lives[0]);
    g_player_kills = {};
    g_shared_kills = 0;
    LOGR("survival: test start_wave=%u waves_required=%u enemies_required=%u initial_lives=%u",
         start_wave, g_run.WavesRequired(), g_run.EnemiesRequired(), initial_lives[0]);
    ReplaySettings replay;
    replay.lives = initial_lives;
    replay.start_wave = start_wave;
    replay.difficulty = difficulty;
    replay.endless = endless;
    replay.cooperative = cooperative;
    replay.ai_partner = ai_partner;
    replay.venue = venue;
    bool replay_valid = true;
    for (unsigned slot = 0; slot < (cooperative ? 2u : 1u); ++slot) {
        bool found = false;
        for (unsigned id = 0; id < kNativeCharacters.size(); ++id)
            for (unsigned costume = 0; costume < 2; ++costume)
                if (NameIs(g_scratch + (slot ? kPartnerName : 0), kNativeCharacters[id].costumes[costume].actor)) {
                    replay.characters[slot] = id;
                    replay.costumes[slot] = costume;
                    found = true;
                }
        replay_valid &= found;
    }
    g_replay_settings = replay_valid ? std::optional<ReplaySettings>(replay) : std::nullopt;
    g_characters = characters;
    g_arena = arena;
    LOGR("survival: difficulty=%s reinforcement_ms=%u extra_active=%u boss_escorts=%u",
         DifficultyFor(difficulty).key, DifficultyFor(difficulty).reinforcement_ms,
         DifficultyFor(difficulty).extra_active, DifficultyFor(difficulty).boss_escorts);
    LOGR("survival: format=%s boss_lives=%u", endless ? "endless" : "classic", DifficultyFor(difficulty).boss_lives);
    LOGR("survival: arena venue=%u level=%s scene=%s spawn_candidates=%zu", venue,
         arena->level, arena->scene, arena->enemies.size());
    g_managed_audio = managed_audio;
    LOGR("survival: party p1=%s p2=%s players=%u managed_audio=%u", first_character, cooperative ? second_character : "none",
         cooperative ? 2u : 1u, unsigned(g_managed_audio));
    g_coop = cooperative;
    g_ai_partner = ai_partner;
    g_ai_target_reports = 0;
    g_ai_repair_reports = 0;
    g_ai_melee_reports = 0;
    g_ai_reported_target = ~0u;
    g_coop_lives.Reset(3);
    for (unsigned slot = 0; slot < 2; ++slot) g_coop_lives.players[slot] = InitialStock(initial_lives[slot]);
    g_coop_down = {};
    g_camera_excluded_mask = ~0u;
    g_coop_actors = g_coop_handles = g_coop_controllers = g_coop_cameras = {};
    g_coop_respawn = g_coop_protection = {};
    g_sound_owner = 0;
    g_owned_sounds = {};
    g_native_player_death = false;
    g_respawn_end = g_protected_until = {};
    g_reinforcement_end = {};
    g_grab_pickup_report = g_grab_throw_report = {};
    g_respawn_actor = g_respawn_handle = 0;
    g_respawn_early = false;
    g_hero_actor = g_hero_handle = 0;
    g_finisher = {};
    g_lives_hud = 0;
    g_world = g_scene = 0;
    g_clock_ready = false;
    g_result_clock_frozen = false;
    g_result_seconds = 0;
    g_native_exit_requested = false;
    TakeExit();
    TakeReplay();
    g_armed = g_save_fence = true;
    g_started = kernel::GuestNow();
    g_next = g_report = g_intermission_end = g_ai_retarget = g_ai_still_since = g_ai_pursuit_next = {};
    g_ai_still_target = g_ai_pursuit_reports = 0;
    g_ai_melee_tasks = g_ai_melee_choices = g_ai_basic_choices = g_ai_throw_choices = g_ai_melee_gated =
        g_ai_melee_unblocked = 0;
    g_ai_last_melee_tick = {};
    State(Phase::Loading, (std::string("Loading ") + arena->label + "...").c_str());
    return true;
}
bool Prepare(X86Context& ctx, unsigned initial_lives, std::string& error, unsigned start_wave, bool cooperative, Party characters,
             unsigned venue, unsigned difficulty, bool endless) {
    if (!AllowedParty(characters)) { error = "SURVIVAL CHARACTER SELECTION INVALID"; return false; }
    if (!Lives::Allowed(initial_lives)) { error = "SURVIVAL REQUIRES 1, 3 OR 5 STARTING LIVES"; return false; }
    return PrepareSelected(ctx, {initial_lives, initial_lives}, error, start_wave, cooperative,
                           {Characters[characters[0]], Characters[characters[1]]}, ManagedAudio(cooperative, characters) || venue != DefaultArena, venue,
                           false, difficulty, endless);
}
bool PrepareNative(X86Context& ctx, std::array<unsigned, 2> char_ids, std::array<unsigned, 2> costumes,
                   std::array<unsigned, 2> lives, bool cooperative, unsigned venue, std::string& error, unsigned start_wave, bool ai_partner,
                   unsigned difficulty, bool endless) {
    const auto* arena = ArenaFor(venue);
    if (!arena || !NameIs(0x481AF0u + venue * 0x41, arena->key)) {
        error = "SURVIVAL NATIVE ARENA DOES NOT MATCH THIS BUILD"; return false;
    }
    SelectedParty selected = kDefaultSelection;
    for (unsigned slot = 0; slot < (cooperative ? 2u : 1u); ++slot) {
        if (char_ids[slot] >= kNativeCharacters.size() || costumes[slot] > 1 || !lives[slot] || lives[slot] > Lives::MaxIcons) {
            error = "SURVIVAL NATIVE SELECTION INVALID"; return false;
        }
        const auto& hero = kNativeCharacters[char_ids[slot]];
        const auto table = 0x481AA4u + char_ids[slot] * 4;
        const auto preset = (costumes[slot] == 1 ? 0x481510u : 0x480F80u) + hero.internal * 0x47;
        selected[slot] = hero.costumes[costumes[slot]];
        if (!Address(table, 4) || kernel::R32(table) != hero.internal || !NameIs(preset, selected[slot].actor)) {
            error = "SURVIVAL NATIVE ROSTER DOES NOT MATCH THIS BUILD"; return false;
        }
    }
    if (!cooperative) lives[1] = lives[0];
    LOGR("survival: native selection ids=%u,%u costumes=%u,%u lives=%u,%u players=%u",
         char_ids[0], char_ids[1], costumes[0], costumes[1], lives[0], lives[1], cooperative ? 2u : 1u);
    return PrepareSelected(ctx, lives, error, start_wave, cooperative, selected, true, venue, ai_partner, difficulty, endless);
}
bool Protected(uint32_t actor) {
    const auto slot = CoopSlot(actor);
    if (slot < 2) return g_phase == Phase::Won || g_coop_down[slot] || RunNow() < g_coop_protection[slot];
    return g_armed && Player() == actor && kernel::R32(actor + 0x14) == g_world &&
        (g_phase == Phase::Won || ((g_phase == Phase::Fighting || g_phase == Phase::Intermission || g_phase == Phase::Respawning) &&
                                 RunNow() < g_protected_until)); // Ignore ordinary projectile damage after victory.
}
uint32_t DamageOwner(X86Context& ctx, uint32_t source) {
    if (!Address(source, 0x1C4) || kernel::R32(source + 0x14) != g_world) return 0;
    const auto kind = kernel::G<uint8_t>(source + 0x10);
    if (kind != 2 && kind != 4) return 0;
    // Native thrown props (99D36/A03D5) and projectiles attribute damage through this owner handle.
    const X86Context saved = ctx;
    const auto owner = kernel::CallGuest(ctx, 0xAB770, {kernel::R32(source + 0x1C0)});
    ctx = saved;
    return owner;
}
bool FriendlyDamage(X86Context& ctx, uint32_t victim, uint32_t source) {
    if (CoopSlot(victim) >= 2) return false;
    return Allies(victim, source) || Allies(victim, DamageOwner(ctx, source));
}
unsigned KillerSlot(X86Context& ctx, uint32_t source) {
    if (!WorldMatches()) return 2;
    const auto direct = g_coop ? CoopSlot(source) : source && source == Player() ? 0u : 2u;
    if (direct < 2) return direct;
    if (!Address(source, 0x1C4) || kernel::G<uint8_t>(source + 0x10) != 4) return 2;
    const auto owner = DamageOwner(ctx, source);
    return g_coop ? CoopSlot(owner) : owner && owner == Player() ? 0u : 2u;
}
bool CoopRevive(X86Context& ctx, unsigned slot) {
    struct Reviving {
        const bool previous = std::exchange(g_coop_reviving, true);
        ~Reviving() { g_coop_reviving = previous; }
    } reviving;
    if (!Respawn(ctx, Player(slot), slot) || !g_coop_lives.players[slot].Respawn()) return false;
    g_coop_down[slot] = false;
    g_coop_protection[slot] = RunNow() + std::chrono::seconds(2);
    LOGR("survival-coop: respawn player=%u lives=%u actor=%08X handle=%08X", slot + 1,
         g_coop_lives.players[slot].remaining, g_coop_actors[slot], g_coop_handles[slot]);
    return true;
}
bool UpdateCoopPlayers(X86Context& ctx) {
    const auto now = RunNow();
    for (unsigned slot = 0; slot < 2; ++slot) {
        const auto actor = Player(slot), record = kPlayer + slot * 0xB0;
        if (CoopSlot(actor) != slot || kernel::R32(record + 0x2C) != g_coop_cameras[slot] ||
            !std::isfinite(kernel::G<float>(actor + 0x32C))) {
            State(Phase::Failed, "Coop participant identity changed. Return to menu."); return false;
        }
        if (Dead(actor)) CoopDeath(slot);
        if (g_coop_lives.players[slot].pending && now >= g_coop_respawn[slot] && !FinisherActive()) {
            if (!CoopRevive(ctx, slot)) {
                State(Phase::Failed, "Coop native respawn failed. Return to menu."); return false;
            }
        }
    }
    if (!g_coop_lives.CanContinue()) {
        State(Phase::Lost, "Both players are out of lives. Return to menu."); return false;
    }
    return true;
}
void Tick(X86Context& ctx) {
    if (!g_armed) return;
    if (g_phase == Phase::Leaving && !kernel::R32(kWorld) && !kernel::R32(0x4C33E4)) {
        FinishedTeardown();
        return;
    }
    const X86Context saved = ctx;
    const bool native_exit = std::exchange(g_native_exit_requested, false);
    if ((TakeExit() || native_exit) && g_phase != Phase::Leaving) {
        Leave(ctx);
        ctx = saved;
        return;
    }
    const bool replay = TakeReplay();
    if (replay && (g_phase == Phase::Won || g_phase == Phase::Lost) && WorldMatches() && g_replay_settings) {
        const auto settings = *g_replay_settings;
        Leave(ctx);
        if (g_phase == Phase::Leaving) {
            pc_development::QueueSurvivalReplay(settings);
            LOGR("survival-results: replay queued after teardown start_wave=%u", settings.start_wave);
        }
        ctx = saved;
        return;
    }
    // Open only after any finishing move releases its actors; actions run before the paused-clock gate.
    if ((g_phase == Phase::Won || g_phase == Phase::Lost) && !FinisherActive()) OpenNativePause(ctx, 0);
    if (g_phase == Phase::Loading && kernel::GuestNow() - g_started > std::chrono::seconds(60))
        State(Phase::Failed, "Loading did not complete. Return to menu.");
    if (static_cast<int32_t>(kernel::R32(0x49E93C)) > 0) return;
    const auto now = RunNow();
    if ((g_phase != Phase::Fighting && g_phase != Phase::Intermission && g_phase != Phase::Respawning) || now < g_next) return;
    g_next = now + std::chrono::milliseconds(100);
    const auto player = Player();
    if (!WorldMatches() || !player) { State(Phase::Failed, "Session changed. Return to menu."); return; }
    if (!std::isfinite(kernel::G<float>(player + 0x32C))) {
        State(Phase::Failed, "Invalid player health. Return to menu."); return;
    }
    if (g_coop) {
        if (!UpdateCoopPlayers(ctx)) { ctx = saved; return; }
    } else {
    if (g_phase == Phase::Respawning) {
        if (player != g_respawn_actor || kernel::R32(player + 0x34) != g_respawn_handle) {
            LOGR("survival: respawn identity mismatch expected=%08X/%08X actual=%08X/%08X hp=%.1f flags=%08X",
                 g_respawn_actor, g_respawn_handle, player, kernel::R32(player + 0x34),
                 kernel::G<float>(player + 0x32C), kernel::R32(player + 0x314));
            State(Phase::Failed, "Player changed during respawn. Return to menu."); return;
        }
        if (FinisherActive() && g_finisher.definition && kernel::R32(kNativeNis + 0x78) == g_finisher.definition) {
            Present("Life lost. Waiting for the finishing move to end..."); return;
        }
        // Native actor recovery can restore this same actor before our five-second countdown ends.
        if (!Dead(player) && !g_respawn_early) {
            g_respawn_early = true;
            g_protected_until = g_respawn_end + std::chrono::seconds(2);
            LOGR("survival: early native recovery actor=%08X handle=%08X hp=%.1f flags=%08X; keeping respawn deadline",
                 player, g_respawn_handle, kernel::G<float>(player + 0x32C), kernel::R32(player + 0x314));
        }
        if (now < g_respawn_end) { Present("Life lost. Waiting to respawn..."); return; }
        if (!Respawn(ctx, player) || !g_run.lives.Respawn()) {
            g_protected_until = {};
            State(Phase::Failed, "Native respawn failed. Return to menu."); return;
        }
        if (g_resume_phase == Phase::Intermission) g_intermission_end = now + g_preparation_left;
        State(g_resume_phase, "Respawned. Continue the Survival run.");
        return;
    }
    if (Dead(player)) {
        if (!g_run.lives.Die()) { State(Phase::Failed, "Invalid life transition. Return to menu."); return; }
        g_protected_until = {};
        g_reinforcement_end = {}; // A returning player sees a fresh reinforcement warning.
        if (!g_run.lives.remaining) { State(Phase::Lost, "No lives remaining. Return to menu to try again."); return; }
        g_resume_phase = g_phase;
        g_preparation_left = g_phase == Phase::Intermission && g_intermission_end > now ? g_intermission_end - now :
            std::chrono::steady_clock::duration::zero();
        g_respawn_end = now + std::chrono::seconds(5);
        g_respawn_actor = player;
        g_respawn_handle = kernel::R32(player + 0x34);
        g_respawn_early = false;
        State(Phase::Respawning, "Life lost. Waiting to respawn...");
        return;
    }
    }
    if (g_phase == Phase::Intermission) {
        if (now < g_intermission_end) { Present("Wave cleared. Prepare for the next wave."); return; }
        if (g_coop && !g_coop_lives.ReadyToFinish()) { Present("Waiting for cooperative respawn..."); return; }
        if (!(g_coop ? g_coop_lives.Advance(g_run) : g_run.Advance())) {
            State(Phase::Failed, "Invalid wave transition. Return to menu."); return;
        }
        SpawnWave(ctx);
        ctx = saved;
        return;
    }
    RefreshAiTarget(ctx);
    const bool report = now >= g_report;
    if (report) {
        g_report = now + std::chrono::seconds(5);
        LOGR("survival: tick wave=%u player=%08X hp=%.1f defeated=%u/%u", g_run.number, player,
             kernel::G<float>(player + 0x32C), g_run.wave.defeated, g_run.wave.target);
        ReportAiPartner();
    }
    for (unsigned i = 0; i < g_run.wave.spawned; ++i) {
        if (g_run.wave.dead[i]) continue;
        const auto handle = g_run.wave.handles[i];
        const auto enemy = kernel::CallGuest(ctx, 0xAB770, {handle});
        if (!Address(enemy, 0x408) || kernel::R32(enemy + 0x34) != handle ||
            kernel::R32(enemy + 0x14) != g_world || kernel::G<uint8_t>(enemy + 0x10) != 7 ||
            !std::isfinite(kernel::G<float>(enemy + 0x32C))) {
            ctx = saved;
            State(Phase::Failed, "Enemy lost before confirmed defeat. Return to menu."); return;
        }
        if (Dead(enemy)) Defeated(handle, "frame-poll");
        if (report) LOGR("survival: enemy=%08X hp=%.1f flags=%08X controller=%08X ai=%08X z=%.3f x=%.3f y=%.3f",
                        handle, kernel::G<float>(enemy + 0x32C), kernel::R32(enemy + 0x314),
                        kernel::R32(enemy + 0x3EC), kernel::R32(enemy + 0x390), kernel::G<float>(enemy + 0xC8),
                        kernel::G<float>(enemy + 0xC0), kernel::G<float>(enemy + 0xC4));
    }
    ctx = saved;
    if (g_run.wave.Pending() && g_run.wave.Alive() < g_run.wave.active_limit) {
        if (g_reinforcement_end == std::chrono::steady_clock::time_point{}) {
            const auto delay = g_run.ReinforcementDelay();
            g_reinforcement_end = now + std::chrono::milliseconds(delay);
            LOGR("survival: reinforcement queued wave=%u ordinal=%u alive=%u pending=%u warning_ms=%u",
                 g_run.number, g_run.wave.spawned + 1, g_run.wave.Alive(), g_run.wave.Pending(), delay);
            Present("Reinforcements incoming.");
        } else if (now >= g_reinforcement_end) {
            if (SpawnEnemies(ctx, 1)) {
                g_reinforcement_end = {};
                Present("Reinforcement arrived. Defeat all remaining enemies.");
            } else if (g_phase != Phase::Failed && now - g_reinforcement_end >= std::chrono::seconds(10)) {
                State(Phase::Failed, "Reinforcement blocked: no safe spawn point. Return to menu.");
            }
        }
        ctx = saved;
        return;
    }
    if (g_coop) {
        if (!g_run.wave.Complete() || !g_coop_lives.ReadyToFinish() || FinisherActive()) return;
        g_coop_lives.RecoverEliminated();
        for (unsigned slot = 0; slot < 2; ++slot)
            if (g_coop_lives.players[slot].pending && !CoopRevive(ctx, slot)) {
                State(Phase::Failed, "Coop partner return failed. Return to menu."); ctx = saved; return;
            }
        if (!g_coop_lives.Clear(g_run)) {
            State(Phase::Failed, "Coop wave clear failed. Return to menu."); ctx = saved; return;
        }
        EndBossMusic(ctx);
        LOGR("survival-coop: wave cleared wave=%u cleared=%u total_defeated=%u p1_lives=%u p2_lives=%u p1_bonus=%u p2_bonus=%u",
             g_run.number, g_run.cleared, g_run.total_defeated,
             g_coop_lives.players[0].remaining, g_coop_lives.players[1].remaining,
             g_coop_lives.boss_bonus[0], g_coop_lives.boss_bonus[1]);
        if (g_run.Complete()) State(Phase::Won, "Selected cooperative waves cleared!");
        else {
            g_intermission_end = now + kPreparation;
            State(Phase::Intermission, "Wave cleared. Prepare for the next wave.");
        }
        ctx = saved;
    } else if (g_run.Clear()) {
        EndBossMusic(ctx);
        if (g_run.BossWave()) LOGR("survival: boss reward wave=%u bonus=%u lives=%u cap=%u defeated=%u/%u",
                                  g_run.number, g_run.boss_bonus, g_run.lives.remaining, Lives::MaxIcons,
                                  g_run.wave.defeated, g_run.wave.target);
        if (g_run.Complete()) State(Phase::Won, "Selected waves cleared!");
        else {
            g_intermission_end = now + kPreparation;
            State(Phase::Intermission, "Wave cleared. Prepare for the next wave.");
        }
    }
}
}  // namespace game::pc_survival

X86_FUNC(__imp__sub_00097E50);
X86_FUNC(sub_00097E50) {
    game::pc_survival::NativeCameraScope focus(ctx);
    focus.SelectInput(ctx);
    __imp__sub_00097E50(ctx, base);
}
X86_FUNC(__imp__sub_00097EB0);
X86_FUNC(sub_00097EB0) {
    game::pc_survival::NativeCameraScope focus(ctx);
    __imp__sub_00097EB0(ctx, base);
}
X86_FUNC(__imp__sub_00097F10);
X86_FUNC(sub_00097F10) {
    game::pc_survival::NativeCameraScope focus(ctx);
    __imp__sub_00097F10(ctx, base);
}
X86_FUNC(__imp__sub_000F0150);
X86_FUNC(sub_000F0150) {
    using namespace game::pc_survival;
    const auto survivor = CameraSurvivor();
    if (survivor >= 2) { __imp__sub_000F0150(ctx, base); return; }
    struct RestoreListenerIndex {
        uint32_t previous;
        ~RestoreListenerIndex() { kernel::W32(0x4F40C8, previous); }
    } restore{kernel::R32(0x4F40C8)};
    // Native audio otherwise reads P1's camera even while the shared view follows P2.
    kernel::W32(0x4F40C8, survivor);
    __imp__sub_000F0150(ctx, base);
}

X86_FUNC(__imp__sub_000978E0);
X86_FUNC(sub_000978E0) {
    using namespace game::pc_survival;
    const auto survivor = ctx.ecx == kManager ? CameraSurvivor() : 2u;
    if (survivor < 2) {
        kernel::Return(ctx, g_coop_cameras[survivor], 0);
        return;
    }
    __imp__sub_000978E0(ctx, base);
}
X86_FUNC(__imp__sub_000365D0);
X86_FUNC(sub_000365D0) {
    using namespace game::pc_survival;
    const auto camera = ctx.ecx;
    if (!g_armed || !g_coop || !WorldMatches() ||
        (camera != g_coop_cameras[0] && camera != g_coop_cameras[1]) ||
        CoopSlot(Player(0)) != 0 || CoopSlot(Player(1)) != 1) {
        __imp__sub_000365D0(ctx, base);
        return;
    }
    struct Exclusions {
        std::array<uint32_t, 2> actors;
        std::array<uint32_t, 2> previous;
        unsigned mask;
        ~Exclusions() {
            for (unsigned slot = 0; slot < 2; ++slot) {
                if (!(mask & (1u << slot)) || CoopSlot(actors[slot]) != slot) continue;
                const auto flags = actors[slot] + 0x314;
                kernel::W32(flags, (kernel::R32(flags) & ~0x08000000u) | previous[slot]);
            }
        }
    } targets{{Player(0), Player(1)}, {}, 0};
    for (unsigned slot = 0; slot < 2; ++slot) {
        const auto actor = targets.actors[slot];
        if (!g_coop_down[slot] && !Dead(actor)) continue;
        targets.mask |= 1u << slot;
        targets.previous[slot] = kernel::R32(actor + 0x314) & 0x08000000u;
        // The native camera substitutes the other target, or holds its last view if both are excluded.
        kernel::W32(actor + 0x314, kernel::R32(actor + 0x314) | 0x08000000u);
    }
    if (g_camera_excluded_mask != targets.mask) {
        g_camera_excluded_mask = targets.mask;
        LOGR("survival-coop: camera excluded_mask=%u (P1=1 P2=2) targets=%u", targets.mask,
             2u - unsigned(std::popcount(targets.mask)));
    }
    __imp__sub_000365D0(ctx, base);
}
X86_FUNC(__imp__sub_000C9080);
X86_FUNC(sub_000C9080) {
    using namespace game::pc_survival;
    const auto controller = ctx.ecx, victim = kernel::R32(ctx.esp + 4);
    const auto attacker = Address(controller, 0xF8) ? kernel::R32(controller + 0xF4) : 0;
    if (Allies(attacker, victim)) { kernel::Return(ctx, 0, 1); return; }
    const auto slot = g_coop ? (CoopSlot(attacker) < 2 ? CoopSlot(attacker) : CoopSlot(victim)) : 0u;
    const auto player = Player(slot);
    const bool pair = player && ((attacker == player && (g_coop ? Tracked(victim) : HeroEnemy(victim))) ||
                                (victim == player && (g_coop ? Tracked(attacker) : HeroEnemy(attacker))));
    if (!g_armed || g_phase != Phase::Fighting || g_finisher.world || !WorldMatches() || !pair) {
        __imp__sub_000C9080(ctx, base); return;
    }
    NativeFinisher pending;
    pending.world = g_world;
    pending.player_slot = slot;
    const std::array<uint32_t, 2> actors{player, attacker == player ? victim : attacker};
    for (unsigned i = 0; i < actors.size(); ++i) {
        const auto actor = actors[i], owner = kernel::R32(actor + 0x3EC);
        if (!Address(owner, 0xF8) || kernel::R32(owner + 0xF4) != actor) {
            __imp__sub_000C9080(ctx, base); return;
        }
        pending.actors[i] = {actor, kernel::R32(actor + 0x34), owner, (kernel::R32(actor + 0x314) & 1) != 0};
    }
    // The finisher can publish a campaign result before the ordinary death callback begins.
    g_finisher = pending;
    __imp__sub_000C9080(ctx, base);
    if (!ctx.eax || !FinisherActive()) { g_finisher = {}; return; }
    g_finisher.definition = kernel::R32(kNativeNis + 0x78);
    LOGR("survival-finisher: started attacker=%08X victim=%08X definition=%08X p1_handle=%08X hero_handle=%08X",
         attacker, victim, g_finisher.definition, pending.actors[0].handle, pending.actors[1].handle);
    if (!g_finisher.definition) FinishNativeFinisher();
}
X86_FUNC(__imp__sub_0025DA86);
X86_FUNC(sub_0025DA86) {
    using namespace game::pc_survival;
    const auto region = ctx.ecx;
    if (g_armed && WorldMatches() && Address(region, 0x58) &&
        kernel::R32(region) == 0x44A458 && kernel::R32(region + 0x28)) {
        const X86Context saved = ctx;
        const auto handle = kernel::R32(ctx.esp + 8);
        const auto actor = kernel::CallGuest(ctx, 0xAB770, {handle});
        ctx = saved;
        const auto slot = g_coop ? CoopSlot(actor) : actor && actor == Player() ? 0u : 2u;
        if (Address(actor, 0x408) && kernel::R32(actor + 0x14) == g_world &&
            kernel::R32(actor + 0x34) == handle &&
            (slot < 2 || (Tracked(actor) && !kernel::R32(actor + 0x30C)))) {
            ctx.ecx = region;
            const auto mode = static_cast<int32_t>(kernel::CallGuest(ctx, 0x66070, {8}));
            ctx = saved;
            if (mode > 0) {
                // Survival owns deaths and respawns; native ring-out cinematics also end campaign audio.
                ctx.ecx = actor;
                const auto killed = kernel::CallGuest(ctx, 0x2FF30, {0, 0, 0, 0, 0, 0, 0, 1, 0});
                ctx = saved;
                if (slot < 2)
                    LOGR("survival: ring-out player=%u handle=%08X region=%08X mode=%d native_death=%u",
                         slot + 1, handle, region, mode, killed);
                else
                    LOGR("survival: ring-out enemy=%08X region=%08X mode=%d native_death=%u",
                         handle, region, mode, killed);
                kernel::Return(ctx, 0, 3); // Region-entry callback uses RET12; existing native-death hook owns counting.
                return;
            }
        }
    }
    __imp__sub_0025DA86(ctx, base);
}
X86_FUNC(__imp__sub_0025DFC8);
X86_FUNC(sub_0025DFC8) {
    using namespace game::pc_survival;
    if (NativeResultBlocked()) {
        LOGR("survival: suppressed campaign result publication phase=%u", unsigned(g_phase));
        kernel::Return(ctx, 0, 3); // This actor callback only publishes a campaign result; native RET12.
        return;
    }
    __imp__sub_0025DFC8(ctx, base);
}
X86_FUNC(__imp__sub_00049CC0);
X86_FUNC(sub_00049CC0) {
    using namespace game::pc_survival;
    const bool target = g_finisher.definition && ctx.ecx == kNativeNis &&
        kernel::R32(kNativeNis + 0x78) == g_finisher.definition;
    __imp__sub_00049CC0(ctx, base);
    // A guarded native cleanup can return without ending the script; retain ownership in that case.
    if (target && !kernel::R32(kNativeNis + 0x78)) FinishNativeFinisher();
}

X86_FUNC(__imp__sub_0010F8B0);
X86_FUNC(sub_0010F8B0) {
    using namespace game::pc_survival;
    // This query selects the native hero-opponent HUD, not a player record or AI role.
    if (NativeLivesHud(kernel::R32(0x4A2E50)) && HeroEnemy(g_hero_actor) &&
        kernel::R32(0x4A2E84) == g_hero_handle) {
        kernel::Return(ctx, 0, 0); return;
    }
    __imp__sub_0010F8B0(ctx, base);
}
X86_FUNC(__imp__sub_0010F900);
X86_FUNC(sub_0010F900) {
    using namespace game::pc_survival;
    // Accept only our NPC hero in the ordinary enemy-health slot; keep its actor flags intact.
    if (NativeLivesHud(kernel::R32(0x4A2E50)) && HeroEnemy(g_hero_actor) &&
        kernel::R32(0x4A2E88) == g_hero_handle && EnemyHudVisible(g_hero_actor)) {
        kernel::Return(ctx, 1, 0); return;
    }
    __imp__sub_0010F900(ctx, base);
}
X86_FUNC(__imp__sub_0010FDF0);
X86_FUNC(sub_0010FDF0) {
    using namespace game::pc_survival;
    const auto record = kernel::R32(ctx.esp + 4), actor = kernel::R32(ctx.esp + 8);
    const auto player = record == kPlayer ? Player() : g_coop && record == kPartner ? Player(1) : 0;
    if (!NativeLivesHud(kernel::R32(0x4A2E50)) || !player ||
        (kernel::R32(record + 0x34) & 2) || !HeroEnemy(actor)) {
        __imp__sub_0010FDF0(ctx, base); return;
    }
    const auto target = EnemyHudVisible(actor) ? g_hero_handle : 0u;
    const auto previous = kernel::R32(0x4A2E88);
    kernel::W32(0x4A2E88, target);
    if (target != previous) {
        const X86Context saved = ctx;
        ctx.ecx = 0x516028; // Native enemy-target event updates the ordinary HUD's label/visibility.
        kernel::CallGuest(ctx, 0x15980, {target});
        ctx = saved;
        LOGR("survival: enemy HUD target=%08X previous=%08X source=hero", target, previous);
    }
    kernel::Return(ctx, 0, 0); // cdecl; combat callers retain ownership of both arguments.
}
X86_FUNC(__imp__sub_0002F3B0);
X86_FUNC(sub_0002F3B0) {
    using namespace game::pc_survival;
    const auto actor = ctx.ecx;
    const auto slot = CoopSlot(actor);
    if (slot < 2 && g_coop_down[slot] && !g_coop_reviving) { kernel::Return(ctx, 0, 4); return; }
    const bool report = g_armed && g_phase == Phase::Respawning && actor == g_respawn_actor &&
        actor == Player() && kernel::R32(actor + 0x34) == g_respawn_handle && Dead(actor);
    const auto caller = report ? kernel::R32(ctx.esp) : 0;
    __imp__sub_0002F3B0(ctx, base);
    if (report) LOGR("survival: native revive caller=%08X actor=%08X handle=%08X hp=%.1f flags=%08X result=%u",
                     caller, actor, kernel::R32(actor + 0x34), kernel::G<float>(actor + 0x32C),
                     kernel::R32(actor + 0x314), ctx.eax);
}

X86_FUNC(__imp__sub_000B5280);
X86_FUNC(sub_000B5280) {
    using namespace game::pc_survival;
    const bool report = GrabReport(ctx.ecx, g_grab_pickup_report);
    __imp__sub_000B5280(ctx, base);
    if (report) LOGR("survival-grab: prop-pickup result=%u", ctx.eax);
}
X86_FUNC(__imp__sub_000CEBA0);
X86_FUNC(sub_000CEBA0) {
    using namespace game::pc_survival;
    if (!GrabReport(ctx.ecx, g_grab_throw_report)) { __imp__sub_000CEBA0(ctx, base); return; }
    GrabTrace trace(ctx.ecx);
    LOGR("survival-grab: body-throw begin wave=%u controller=%08X controller_flags=%08X args=%u,%u",
         g_run.number, ctx.ecx, kernel::R32(ctx.ecx + 0x28), kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp + 8));
    ReportGrabActor("attacker", Player());
    __imp__sub_000CEBA0(ctx, base);
    LOGR("survival-grab: body-throw result=%u", ctx.eax);
}
X86_FUNC(__imp__sub_000CA3B0);
X86_FUNC(sub_000CA3B0) {
    using namespace game::pc_survival;
    if (!g_grab_trace || g_grab_trace->controller != ctx.ecx) { __imp__sub_000CA3B0(ctx, base); return; }
    const auto direction = kernel::R32(ctx.esp + 0x10);
    LOGR("survival-grab: query min_dot=%.3f radius=%.3f prefer=%u actor_only=%u direction=%08X",
         kernel::G<float>(ctx.esp + 4), kernel::G<float>(ctx.esp + 8), kernel::R32(ctx.esp + 0xC),
         kernel::R32(ctx.esp + 0x14), direction);
    if (Address(direction, 12)) LOGR("survival-grab: direction=%.3f,%.3f,%.3f",
        kernel::G<float>(direction), kernel::G<float>(direction + 4), kernel::G<float>(direction + 8));
    const bool previous = std::exchange(g_grab_trace->querying, true);
    __imp__sub_000CA3B0(ctx, base); // This query changes the selected target; never run it twice for diagnostics.
    g_grab_trace->querying = previous;
    ReportGrabActor("query-result", ctx.eax);
}
X86_FUNC(__imp__sub_00045B00);
X86_FUNC(sub_00045B00) {
    using namespace game::pc_survival;
    if (g_ai_target_query && Address(ctx.ecx, 0x34) && CoopSlot(kernel::R32(ctx.ecx + 0x30)) == 1) {
        const auto candidate = kernel::R32(ctx.esp + 4);
        if (!Tracked(candidate) || Dead(candidate)) { kernel::Return(ctx, 1, 1); return; }
    }
    if (g_coop && Address(ctx.ecx, 0x34) && Allies(kernel::R32(ctx.ecx + 0x30), kernel::R32(ctx.esp + 4))) {
        kernel::Return(ctx, 1, 1); return;
    }
    struct RestoreRoles {
        uint32_t query = 0, flags = 0;
        ~RestoreRoles() { if (query) kernel::W32(query + 0x28, flags); }
    } roles;
    if (g_ai_partner && Address(ctx.ecx, 0x34) &&
        AiHostility(kernel::R32(ctx.ecx + 0x30), kernel::R32(ctx.esp + 4)) == 1) {
        roles.query = ctx.ecx;
        roles.flags = kernel::R32(ctx.ecx + 0x28);
        // Keep native geometry/death checks; only the CPU-role exclusion conflicts with our teams.
        kernel::W32(roles.query + 0x28, roles.flags & ~0xA00u);
    }
    if (!g_grab_trace || !g_grab_trace->querying) { __imp__sub_00045B00(ctx, base); return; }
    const auto candidate = kernel::R32(ctx.esp + 4);
    const bool report = Tracked(candidate) && Address(ctx.ecx, 0x184C);
    const auto mask = report ? kernel::R32(ctx.ecx + 0x1848) : 0;
    const auto flags = report ? kernel::R32(ctx.ecx + 0x28) : 0;
    __imp__sub_00045B00(ctx, base);
    if (report) {
        LOGR("survival-grab: candidate-filter excluded=%u type_mask=%08X query_flags=%08X", ctx.eax, mask, flags);
        ReportGrabActor("candidate", candidate);
    }
}
X86_FUNC(__imp__sub_00090090);
X86_FUNC(sub_00090090) {
    using namespace game::pc_survival;
    const auto actor = ctx.ecx;
    const bool report = g_grab_trace && g_grab_trace->querying && (actor == Player() || Tracked(actor));
    __imp__sub_00090090(ctx, base);
    if (report) LOGR("survival-grab: native-role actor=%08X result=%u", actor, ctx.eax);
}
X86_FUNC(__imp__sub_0009F910);
X86_FUNC(sub_0009F910) {
    using namespace game::pc_survival;
    const bool report = g_grab_trace && kernel::R32(ctx.esp) == 0xCF49D;
    if (report && Address(ctx.ecx, 0x408)) {
        const auto motions = kernel::R32(ctx.ecx + 0x1D0), animator = kernel::R32(ctx.ecx + 0x1CC);
        LOGR("survival-grab: throw-animation begin actor=%08X motion=%u motions=%08X variants=%u variant=%u animator=%08X rig=%08X",
             ctx.ecx, kernel::R32(ctx.esp + 4), motions, Address(motions, 0x28) ? kernel::R32(motions + 0x24) : 0,
             kernel::G<uint8_t>(ctx.ecx + 0x214), animator, Address(animator, 0xBC) ? kernel::R32(animator + 0xB8) : 0);
    }
    __imp__sub_0009F910(ctx, base);
    if (report) LOGR("survival-grab: throw-animation result=%d", int32_t(ctx.eax));
}
X86_FUNC(__imp__sub_0020CE80);
X86_FUNC(sub_0020CE80) {
    using namespace game::pc_survival;
    if (!g_armed || !g_target_load) { __imp__sub_0020CE80(ctx, base); return; }
    // NMS parsing omits unavailable tracks; enabling loose NAD only at enemy spawn is too late for Wolverine.
    const LooseAnimations animation_source;
    const auto name = kernel::R32(ctx.esp + 4);
    const bool grab = NameIs(name, "Wolverine_ThrowMale.nad") || NameIs(name, "Wolverine_ThrowMale") ||
        NameIs(name, "Male_WolvThrowVictim.nad") || NameIs(name, "Male_WolvThrowVictim");
    __imp__sub_0020CE80(ctx, base);
    if (grab) LOGR("survival-grab: load-track name=%08X track=%08X previous_bundled_only=%u",
                   name, ctx.eax, animation_source.bundled_only);
}
X86_FUNC(__imp__sub_00044500);
X86_FUNC(sub_00044500) {
    using namespace game::pc_survival;
    const auto caller = g_grab_trace ? kernel::R32(ctx.esp) : 0;
    __imp__sub_00044500(ctx, base);
    if (caller == 0xCEFAB || caller == 0xCEFE1)
        LOGR("survival-grab: visibility caller=%08X blocked=%u", caller, ctx.eax);
}
X86_FUNC(__imp__sub_00027180);
X86_FUNC(sub_00027180) {
    const bool report = game::pc_survival::g_grab_trace != nullptr;
    __imp__sub_00027180(ctx, base);
    if (report) LOGR("survival-grab: victim-recovery rejected=%u", ctx.eax);
}
X86_FUNC(__imp__sub_00030C00);
X86_FUNC(sub_00030C00) {
    const bool report = game::pc_survival::g_grab_trace != nullptr;
    __imp__sub_00030C00(ctx, base);
    if (report) LOGR("survival-grab: victim-throw-state rejected=%u", ctx.eax);
}

X86_FUNC(__imp__sub_0010FF20);
X86_FUNC(sub_0010FF20) {
    using namespace game::pc_survival;
    if (g_armed && g_scratch && kernel::R32(ctx.esp) == 0x194E2) {
        constexpr char title[] = "SURVIVAL";
        std::memcpy(kernel::HostPtr(g_scratch + kTextInput), title, sizeof(title));
        LOGR("survival: native intro title=SURVIVAL");
        kernel::Return(ctx, g_scratch + kTextInput, 0); // 19B20 copies it before text input is reused.
        return;
    }
    __imp__sub_0010FF20(ctx, base);
}

X86_FUNC(__imp__sub_0001BFB0);
X86_FUNC(sub_0001BFB0) {
    using namespace game::pc_survival;
    const auto hud = kernel::R32(0x4A2E50);
    const bool target = g_armed && g_target_load && Address(hud, 0x418C) && ctx.ecx == hud + 0x360;
    bool resources = false;
    if (target) {
        const X86Context saved = ctx;
        const auto pack = kernel::R32(ctx.esp + 4);
        // The pack is released after native HUD setup; resolve while its owner is still alive.
        if (Address(pack, 0xC) && Address(kernel::R32(pack), 4)) {
            ctx.ecx = pack;
            const auto icon = kernel::CallGuest(ctx, 0x1E7D0, {0x40B2DC}); // tex_lifeIcon
            ctx.ecx = pack;
            const auto flash = kernel::CallGuest(ctx, 0x1E7D0, {0x40B2C8}); // tex_lifeIconFlash
            resources = Address(icon, 4) && Address(flash, 4);
            LOGR("survival: native life icon resources=%u icon=%08X flash=%08X", unsigned(resources), icon, flash);
        }
        ctx = saved;
    }
    __imp__sub_0001BFB0(ctx, base);
    if (target) g_lives_hud = resources ? hud : 0;
}
X86_FUNC(__imp__sub_0010F070);
X86_FUNC(sub_0010F070) {
    using namespace game::pc_survival;
    // Only the life-widget setup query: choose icons without changing versus rules or HUD mode.
    if (g_armed && g_target_load && kernel::R32(ctx.esp) == 0x1BFF8) {
        kernel::Return(ctx, 0, 0); return;
    }
    __imp__sub_0010F070(ctx, base);
}
X86_FUNC(__imp__sub_0010F7D0);
X86_FUNC(sub_0010F7D0) {
    using namespace game::pc_survival;
    if (g_armed && g_target_load && kernel::R32(ctx.esp) == 0x1BFFF) {
        kernel::Return(ctx, 0, 0); return;
    }
    __imp__sub_0010F7D0(ctx, base);
}
X86_FUNC(__imp__sub_0010FB80);
X86_FUNC(sub_0010FB80) {
    using namespace game::pc_survival;
    if (g_armed && g_native_lives_query) {
        const auto slot = kernel::R32(ctx.esp + 4);
        const auto icons = g_coop ? (slot < 2 ? g_coop_lives.players[slot].Icons(0) : 0u) : g_run.lives.Icons(slot);
        kernel::Return(ctx, icons, 0); return;
    }
    __imp__sub_0010FB80(ctx, base);
}
X86_FUNC(__imp__sub_00012650);
X86_FUNC(sub_00012650) {
    const auto hud = ctx.ecx, delta = kernel::R32(ctx.esp + 4);
    __imp__sub_00012650(ctx, base);
    using namespace game::pc_survival;
    if (!NativeLivesHud(hud)) return;
    const X86Context saved = ctx;
    {
        const NativeLivesQuery query;
        ctx.ecx = hud + 0x360;
        kernel::CallGuest(ctx, 0x1BCD0, {delta});
    }
    UpdateNativeText(ctx, hud);
    ctx = saved;
}
X86_FUNC(__imp__sub_00013860);
X86_FUNC(sub_00013860) {
    const auto hud = ctx.ecx;
    __imp__sub_00013860(ctx, base);
    using namespace game::pc_survival;
    if (!NativeLivesHud(hud)) return;
    const X86Context saved = ctx;
    {
        const NativeLivesQuery query;
        ctx.ecx = hud + 0x360;
        kernel::CallGuest(ctx, 0x1BDA0, {});
    }
    DrawNativeText(ctx, hud);
    ctx = saved;
}

X86_FUNC(__imp__sub_0002FF30);
X86_FUNC(sub_0002FF30) {
    using namespace game::pc_survival;
    const auto actor = ctx.ecx;
    const auto handle = Tracked(actor) ? kernel::R32(actor + 0x34) : 0;
    const auto world = g_world;
    const auto killer = handle ? KillerSlot(ctx, kernel::R32(ctx.esp + 4)) : 2u;
    const bool previous = g_native_player_death;
    g_native_player_death = previous || (g_armed && (actor == Player() || CoopSlot(actor) < 2) &&
        kernel::R32(actor + 0x14) == g_world);
    __imp__sub_0002FF30(ctx, base);
    g_native_player_death = previous;
    if (ctx.eax == 1) CoopDeath(CoopSlot(actor));
    // Native death succeeds before corpse removal. Publish only; the frame tick resolves victory/player defeat.
    if (handle && ctx.eax == 1 && g_armed && world == g_world)
        Defeated(handle, "native-death", killer);
}
X86_FUNC(__imp__sub_000AF220);
X86_FUNC(__imp__sub_000118D0);
X86_FUNC(sub_000118D0) {
    using namespace game::pc_survival;
    if (g_armed && NativeResultBlocked() && ctx.ecx == g_lives_hud) {
        LOGR("survival: skipped campaign HUD result during Survival death/finisher");
        kernel::Return(ctx, 0, 1); // This HUD listener uses RET4, unlike the cdecl audio listeners.
        return;
    }
    __imp__sub_000118D0(ctx, base);
}
X86_FUNC(__imp__sub_00014500);
X86_FUNC(sub_00014500) {
    if (ctx.ecx == game::pc_survival::g_enemy_hud_owner) game::pc_survival::DestroyEnemyHud(ctx);
    if (ctx.ecx == game::pc_survival::g_text_hud) game::pc_survival::DestroyNativeText(ctx);
    __imp__sub_00014500(ctx, base);
}
X86_FUNC(__imp__sub_000F0130);
X86_FUNC(sub_000F0130) {
    using namespace game::pc_survival;
    if (g_armed && NativeResultBlocked()) {
        LOGR("survival: skipped campaign sound shutdown during Survival death/finisher");
        kernel::Return(ctx, 0, 0); // cdecl event listener: its caller owns the boolean argument.
        return;
    }
    __imp__sub_000F0130(ctx, base);
}
X86_FUNC(__imp__sub_000F69D0);
X86_FUNC(sub_000F69D0) {
    using namespace game::pc_survival;
    if (g_armed && NativeResultBlocked()) {
        LOGR("survival: skipped campaign result music during Survival death/finisher");
        kernel::Return(ctx, 0, 0);
        return;
    }
    __imp__sub_000F69D0(ctx, base);
}
X86_FUNC(sub_000AF220) {
    using namespace game::pc_survival;
    if (g_armed && NativeResultBlocked() && ctx.ecx == 0x483800) {
        kernel::Return(ctx, 1, 1); // Skip this death's delayed story-loss event and native result state.
        return;
    }
    __imp__sub_000AF220(ctx, base);
}
X86_FUNC(__imp__sub_000B1300);
X86_FUNC(sub_000B1300) {
    using namespace game::pc_survival;
    if (!g_armed) { __imp__sub_000B1300(ctx, base); return; }
    const X86Context saved = ctx;
    if (g_phase != Phase::Loading && g_phase != Phase::Leaving && WorldMatches() &&
        ctx.ecx == 0x483800 && kernel::R32(0x4C3444) == 3 && Address(g_scratch + 32, 4)) {
        // This helper scans every controller; argument 2 is an optional uint16_t output pointer.
        kernel::W32(g_scratch + 32, 0xFFFFFFFFu);
        const bool start = kernel::CallGuest(ctx, 0x2E970, {0xB, g_scratch + 32}) != 0;
        ctx = saved;
        const auto port = kernel::G<uint16_t>(g_scratch + 32);
        for (unsigned slot = 0; start && slot < (g_coop ? 2u : 1u); ++slot) {
            if (!Player(slot) || kernel::R32(kPlayer + slot * 0xB0 + 0x3C) != port) continue;
            OpenNativePause(ctx, slot);
            kernel::Return(ctx, 1, 0);
            return;
        }
    }
    ctx = saved;
    kernel::Return(ctx, 0, 0); // Survival uses the native pause initializer without the sign-in branch.
}
X86_FUNC(__imp__sub_000E3BA0);
X86_FUNC(sub_000E3BA0) {
    using namespace game::pc_survival;
    if (g_armed && WorldMatches()) {
        // Finish the APT callback before closing its screen or destroying the arena.
        g_native_exit_requested = true;
        LOGR("survival: native pause quit queued");
        kernel::Return(ctx, 0, 0); // cdecl APT callback; caller owns its arguments.
        return;
    }
    __imp__sub_000E3BA0(ctx, base);
}
X86_FUNC(__imp__sub_000E3550);
X86_FUNC(sub_000E3550) {
    using namespace game::pc_survival;
    if (g_armed && WorldMatches() && kernel::R32(ctx.esp + 4) == 2) {
        g_native_exit_requested = true;
        LOGR("survival: native pause legacy quit queued");
        kernel::Return(ctx, 0, 0); // Preserve the native resume/options actions and cdecl stack ownership.
        return;
    }
    __imp__sub_000E3550(ctx, base);
}
X86_FUNC(__imp__sub_0002E710);
X86_FUNC(sub_0002E710) {
    const bool main_frame = ctx.ecx == 0x4C33E0 && kernel::R32(ctx.esp + 4) == 0;
    __imp__sub_0002E710(ctx, base);
    if (!main_frame || game::pc_survival::g_target_load) return;
    const X86Context saved = ctx;
    game::pc_survival::Tick(ctx);
    ctx = saved;
}
X86_FUNC(__imp__sub_0002DFC0);
X86_FUNC(sub_0002DFC0) {
    using namespace game::pc_survival;
    const bool target = g_armed && g_phase == Phase::Loading &&
        NameIs(kernel::R32(ctx.esp + 4), g_arena->level) && NameIs(kernel::R32(ctx.esp + 8), g_arena->scene);
    g_target_load = target;
    if (target) LOGR("survival-grab: target-load loose NAD fallback enabled");
    __imp__sub_0002DFC0(ctx, base);
    if (target) {
        const X86Context saved = ctx;
        Loaded(ctx);
        ctx = saved;
    }
    g_target_load = false;
}
X86_FUNC(__imp__sub_0002DBA0);
X86_FUNC(sub_0002DBA0) {
    if (game::pc_survival::g_enemy_hud_constructed) game::pc_survival::DestroyEnemyHud(ctx);
    if (game::pc_survival::g_text_count) game::pc_survival::DestroyNativeText(ctx);
    __imp__sub_0002DBA0(ctx, base);
    using namespace game::pc_survival;
    if (!g_armed) return;
    if (!g_target_load) FinishedTeardown();
}
X86_FUNC(__imp__sub_0002DA60);
X86_FUNC(sub_0002DA60) {
    // 0x2DFC0 has finished destroying any previous session before this point.
    // Changing the player count earlier would truncate the old teardown loop.
    if (game::pc_survival::g_target_load) game::pc_survival::ConfigurePlayer(ctx);
    __imp__sub_0002DA60(ctx, base);
}
X86_FUNC(__imp__sub_0002C7E0);
X86_FUNC(sub_0002C7E0) {
    using namespace game::pc_survival;
    if (!g_armed) { __imp__sub_0002C7E0(ctx, base); return; }
    // The frame checks current P1 death before charging a life; delayed callbacks cannot charge twice.
    kernel::Return(ctx, 0, 0); // cdecl callback: the caller owns its one boolean argument.
}
X86_FUNC(__imp__sub_00107E70);
X86_FUNC(sub_00107E70) {
    using namespace game::pc_survival;
    if (!game::pc_development::AutosaveBlocked()) g_save_fence = false;
    const auto card = kernel::R32(kMemcard);
    if (g_save_fence && ctx.ecx == card && Address(card, 0x740) && kernel::R32(card + 0x6D4) == 1) {
        const auto operation = kernel::R32(card + 0x6CC);
        if (operation == 4 || operation == 5 || operation == 0x10 || operation == 0x13) {
            kernel::W32(card + 0x6CC, 0xFFFFFFFFu);
            LOGR("survival: campaign save refused operation=%u; load a campaign save to resume", operation);
            const X86Context saved = ctx;
            kernel::CallGuest(ctx, 0xE08A0, {}); // Native failure, never claim that a blocked save succeeded.
            ctx = saved;
            kernel::Return(ctx, 0, 0);
            return;
        }
    }
    __imp__sub_00107E70(ctx, base);
}
X86_FUNC(__imp__sub_000E7C30);
X86_FUNC(sub_000E7C30) {
    __imp__sub_000E7C30(ctx, base);
    // Only the APT IsStoryMode callback: keep the native simulation predicate untouched.
    if (game::pc_survival::g_armed) ctx.eax = 0;
}

X86_FUNC(__imp__sub_001139EF);
X86_FUNC(sub_001139EF) {
    using namespace game::pc_survival;
    if (!g_armed) { __imp__sub_001139EF(ctx, base); return; }
    const auto pointer = kernel::R32(ctx.esp + 4);
    std::string message;
    bool terminated = false;
    for (unsigned i = 0; i < 0x800; ++i) {
        const auto address = uint64_t(pointer) + i;
        if (address > 0xFFFFFFFFull || !Address(uint32_t(address), 1)) {
            LOGR("survival-native-debug: unreadable message address=%08X", pointer);
            __imp__sub_001139EF(ctx, base);
            return;
        }
        const auto byte = kernel::G<uint8_t>(uint32_t(address));
        if (!byte) { terminated = true; break; }
        message += char(byte);
    }
    // This helper only emits debugger text; INT2D is otherwise ignored before its fatal INT3 in the recompiler.
    LOGR("survival-native-debug: %s%s", message.c_str(), terminated ? "" : " [truncated]");
    kernel::Return(ctx, 0, 1); // Native 113A1D continuation is LEAVE / RET4; the sink ignores EAX.
}

X86_FUNC(__imp__sub_000139D0);
X86_FUNC(sub_000139D0) {
    using namespace game::pc_survival;
    const auto hud = ctx.ecx;
    struct HudSetup {
        const bool previous = g_coop_hud_setup;
        HudSetup() { g_coop_hud_setup = g_armed && g_coop && g_target_load; }
        ~HudSetup() { g_coop_hud_setup = previous; }
    } setup;
    __imp__sub_000139D0(ctx, base);
    // The native loader releases hud+0x340 before Loaded; consume the campaign layout while it is still alive.
    if (g_coop_hud_setup && !CreateEnemyHud(ctx, hud))
        LOGR("survival-coop: campaign enemy HUD setup unavailable");
}
X86_FUNC(__imp__sub_0010F890);
X86_FUNC(sub_0010F890) {
    if (game::pc_survival::g_coop_hud_setup) { kernel::Return(ctx, 1, 0); return; }
    __imp__sub_0010F890(ctx, base);
}
X86_FUNC(__imp__sub_0010FE50);
X86_FUNC(sub_0010FE50) {
    using namespace game::pc_survival;
    const auto slot = kernel::R32(ctx.esp + 4);
    if (g_armed && g_target_load && slot < (g_coop ? 2u : 1u)) {
        const auto name = kPlayer + slot * 0xB0 + 4;
        if (NameIs(name, g_characters[slot].actor)) { kernel::Return(ctx, name, 0); return; }
    }
    __imp__sub_0010FE50(ctx, base);
}
X86_FUNC(__imp__sub_000114A0);
X86_FUNC(sub_000114A0) {
    using namespace game::pc_survival;
    const auto hud = ctx.ecx, delta = kernel::R32(ctx.esp + 4);
    if (!g_coop || !NativeLivesHud(hud)) { __imp__sub_000114A0(ctx, base); return; }
    { const NativeLivesQuery query; __imp__sub_000114A0(ctx, base); }
    const X86Context saved = ctx;
    UpdateNativeText(ctx, hud);
    UpdateEnemyHud(ctx, hud, delta);
    ctx.ecx = hud + 0x190;
    kernel::CallGuest(ctx, 0x177F0, {delta});
    for (uint32_t offset : {0x8u, 0x88u, 0x108u}) {
        ctx.ecx = hud + offset;
        kernel::CallGuest(ctx, 0x178B0, {delta});
    }
    ctx = saved;
}
X86_FUNC(__imp__sub_00013950);
X86_FUNC(sub_00013950) {
    using namespace game::pc_survival;
    const auto hud = ctx.ecx;
    if (!g_coop || !NativeLivesHud(hud)) { __imp__sub_00013950(ctx, base); return; }
    { const NativeLivesQuery query; __imp__sub_00013950(ctx, base); }
    const X86Context saved = ctx;
    const auto color = kernel::R32(hud + 0x3E24);
    if (Address(color, 4)) {
        ctx.ecx = hud + 0x190;
        kernel::CallGuest(ctx, 0x168A0, {color});
        for (uint32_t offset : {0x8u, 0x88u, 0x108u}) {
            ctx.ecx = hud + offset;
            kernel::CallGuest(ctx, 0x16AB0, {color});
        }
    }
    DrawNativeText(ctx, hud);
    DrawEnemyHud(ctx, hud);
    ctx = saved;
}
X86_FUNC(__imp__sub_0009C1B0);
X86_FUNC(sub_0009C1B0) {
    using namespace game::pc_survival;
    if (!g_armed || !g_ai_partner || !g_target_load || ctx.ecx != kPartner ||
        (kernel::R32(kPartner + 0x34) & 6) != 4 ||
        !NameIs(kPartner + 4, g_characters[1].actor) ||
        kernel::R32(ctx.esp + 8) != 0 || kernel::R32(ctx.esp + 12) != 1) {
        __imp__sub_0009C1B0(ctx, base); return;
    }
    struct RestoreSetup {
        uint32_t argument, value, human_count, selected, input;
        ~RestoreSetup() {
            kernel::W32(argument, value);
            kernel::W32(0x4F40C4, human_count);
            kernel::W32(kManager + 0x744, selected);
            kernel::W32(0x5590E4, input);
        }
    } restore{ctx.esp + 8, kernel::R32(ctx.esp + 8), kernel::R32(0x4F40C4),
              kernel::R32(kManager + 0x744), kernel::R32(0x5590E4)};
    // Reuse native camera ownership; the CPU flag still selects its AI controller.
    kernel::W32(restore.argument, 1);
    __imp__sub_0009C1B0(ctx, base);
    const auto human_port = kernel::R32(kPlayer + 0x3C);
    if (human_port <= 1) kernel::W32(kPartner + 0x3C, 1u - human_port);
    LOGR("survival-ai: native camera=%08X reserved_port=%u cpu_flags=%08X",
         kernel::R32(kPartner + 0x2C), kernel::R32(kPartner + 0x3C), kernel::R32(kPartner + 0x34));
}

X86_FUNC(__imp__sub_000B9100);
X86_FUNC(sub_000B9100) {
    using namespace game::pc_survival;
    const auto controller = ctx.ecx;
    const auto actor = Address(controller, 0x1C8) ? kernel::R32(controller + 0xF4) : 0;
    const bool initializing = g_armed && g_ai_partner && (g_target_load || WorldMatches()) &&
        actor && actor == Player(1) && kernel::R32(actor + 0x3EC) == controller &&
        kernel::R32(actor + 0x14) == kernel::R32(kWorld) &&
        (kernel::R32(kPartner + 0x34) & 6) == 4 && !kernel::R32(controller + 0x1C4);
    if (initializing) {
        // Native Versus CPU normal difficulty uses column 2; its AI caches this at creation.
        kernel::W32(actor + 0x188, 2);
    }
    __imp__sub_000B9100(ctx, base);
    if (initializing) {
        const auto state = kernel::R32(controller + 0x1C4);
        LOGR("survival-ai: initialized actor=%08X controller=%08X state=%08X vtable=%08X ai=%u column=%u",
             actor, controller, state, Address(state, 4) ? kernel::R32(state) : 0,
             kernel::R16(actor + 0x394), kernel::R32(actor + 0x188));
    }
}

X86_FUNC(__imp__sub_0006F860);
X86_FUNC(sub_0006F860) {
    using namespace game::pc_survival;
    const auto brain = ctx.ecx, actor = AiBrainOwner(brain);
    if (!actor) { __imp__sub_0006F860(ctx, base); return; }
    struct RestoreColumn {
        uint32_t actor, previous;
        ~RestoreColumn() { kernel::W32(actor + 0x188, previous); }
    } restore{actor, kernel::R32(actor + 0x188)};
    // Cache the native hard CPU decision profile only. Actor stats continue using their original column.
    kernel::W32(actor + 0x188, 3);
    __imp__sub_0006F860(ctx, base);
    // 87580 weights melee through 5BC, dodge/strafe through 5B8, and block through 5B0.
    // Keep native block strength for projectile defense; reduce only excess evasive movement.
    kernel::G<float>(brain + 0x5B8) = std::min(kernel::G<float>(brain + 0x5B8), 35.0f);
    if (kernel::G<float>(brain + 0x5BC) > 0.0f) {
        kernel::G<float>(brain + 0x5BC) = 100.0f;
        // 6F720 chooses the whole task: melee must beat charge, ranged and object hunting first.
        kernel::G<float>(brain + 0x5E0) = std::min(kernel::G<float>(brain + 0x5E0), 6.0f);
        kernel::G<float>(brain + 0x5C8) = std::min(kernel::G<float>(brain + 0x5C8), 6.0f);
        kernel::G<float>(brain + 0x5DC) = std::min(kernel::G<float>(brain + 0x5DC), 3.0f);
    }
    LOGR("survival-ai: decision profile=3 actor=%08X brain=%08X actor_column=%u melee=%.1f dodge=%.1f block=%.1f charge=%.1f ranged=%.1f objects=%.1f",
         actor, brain, restore.previous, kernel::G<float>(brain + 0x5BC),
         kernel::G<float>(brain + 0x5B8), kernel::G<float>(brain + 0x5B0),
         kernel::G<float>(brain + 0x5E0), kernel::G<float>(brain + 0x5C8), kernel::G<float>(brain + 0x5DC));
}

X86_FUNC(__imp__sub_0006F720);
X86_FUNC(sub_0006F720) {
    using namespace game::pc_survival;
    const auto brain = ctx.ecx, actor = AiBrainOwner(brain);
    if (!actor || g_phase != Phase::Fighting || g_coop_down[1] || Dead(actor) ||
        FinisherActive() || kernel::R32(brain + 0xE4) ||
        kernel::R32(g_coop_controllers[1] + 0x100) ||
        !(kernel::R32(actor + 0x310) & 0x200) ||
        !(kernel::G<float>(brain + 0x5BC) > 0.0f) ||
        !(kernel::G<float>(brain + 0x59C) > 0.0f)) {
        __imp__sub_0006F720(ctx, base);
        return;
    }
    const auto handle = kernel::R32(brain + 0x19C);
    const X86Context saved = ctx;
    const auto target = handle ? kernel::CallGuest(ctx, 0xAB770, {handle}) : 0;
    ctx = saved;
    if (!Tracked(target) || Dead(target)) {
        __imp__sub_0006F720(ctx, base);
        return;
    }
    const float dx = kernel::G<float>(actor + 0xC0) - kernel::G<float>(target + 0xC0);
    const float dy = kernel::G<float>(actor + 0xC4) - kernel::G<float>(target + 0xC4);
    const float dz = kernel::G<float>(actor + 0xC8) - kernel::G<float>(target + 0xC8);
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) ||
        dx * dx + dy * dy > 400.0f || std::abs(dz) > 2.5f) {
        __imp__sub_0006F720(ctx, base);
        return;
    }
    struct RestoreTasks {
        uint32_t brain;
        std::array<float, 3> weights;
        ~RestoreTasks() {
            kernel::G<float>(brain + 0x5E0) = weights[0];
            kernel::G<float>(brain + 0x5C8) = weights[1];
            kernel::G<float>(brain + 0x5DC) = weights[2];
        }
    } restore{brain, {kernel::G<float>(brain + 0x5E0), kernel::G<float>(brain + 0x5C8),
                       kernel::G<float>(brain + 0x5DC)}};
    // Nearby ground enemies enter the native melee task; airborne and distant targets retain native choices.
    kernel::G<float>(brain + 0x5E0) = 0.0f;
    kernel::G<float>(brain + 0x5C8) = 0.0f;
    kernel::G<float>(brain + 0x5DC) = 0.0f;
    ++g_ai_melee_tasks;
    __imp__sub_0006F720(ctx, base);
}

X86_FUNC(__imp__sub_00087580);
X86_FUNC(sub_00087580) {
    using namespace game::pc_survival;
    const auto task = ctx.ecx;
    const auto brain = Address(task, 0x24) ? kernel::R32(task + 4) : 0;
    const auto tasks = brain ? kernel::R32(task + 8) : 0;
    const bool active_melee = Address(tasks, 0x18) && kernel::R32(tasks + 0x14) == task &&
                              !kernel::R32(task + 0x20);
    const auto actor = AiBrainOwner(brain);
    bool live_target = false;
    if (actor && g_phase == Phase::Fighting && !g_coop_down[1] && !Dead(actor) &&
        !FinisherActive() && !kernel::R32(g_coop_controllers[1] + 0x100) &&
        !kernel::R32(brain + 0xE4)) {
        const auto target = kernel::R32(brain + 0x19C);
        for (unsigned i = 0; i < g_run.wave.spawned; ++i)
            if (target && !g_run.wave.dead[i] && target == g_run.wave.handles[i]) {
                live_target = true;
                break;
            }
    }
    struct RestoreChoice {
        uint32_t previous = g_ai_melee_choice_brain;
        ~RestoreChoice() { g_ai_melee_choice_brain = previous; }
    } restore;
    if (live_target && active_melee) {
        g_ai_melee_choice_brain = brain;
        g_ai_last_melee_tick = RunNow();
    }
    __imp__sub_00087580(ctx, base);
}

X86_FUNC(__imp__sub_00082A90);
X86_FUNC(sub_00082A90) {
    using namespace game::pc_survival;
    const auto brain = Address(ctx.ecx, 4) ? kernel::R32(ctx.ecx) : 0;
    const auto actor = AiBrainOwner(brain);
    __imp__sub_00082A90(ctx, base);
    if (!actor || g_phase != Phase::Fighting || g_coop_down[1] || Dead(actor) || FinisherActive()) return;
    const auto flags = kernel::R32(brain + 0xC);
    if (!flags || !Address(flags + 4, 4)) return;
    const X86Context saved = ctx;
    ctx.ecx = brain;
    const auto target = kernel::CallGuest(ctx, 0x6FC40, {});
    ctx = saved;
    if (!Tracked(target) || Dead(target)) return;
    const auto previous = kernel::R32(flags + 4);
    // Native perception sets bit 0x35 for player targets, but omits campaign NPCs (kind 7).
    kernel::W32(flags + 4, previous | 0x200000u);
    if (!(previous & 0x200000u)) ++g_ai_melee_unblocked;
}

X86_FUNC(__imp__sub_0008C230);
X86_FUNC(sub_0008C230) {
    using namespace game::pc_survival;
    if (g_ai_melee_choice_brain && kernel::R32(ctx.esp + 4) == 10) {
        const auto weights = kernel::R32(ctx.esp + 8);
        if (Address(weights, 10 * sizeof(float))) {
            const float basic = kernel::G<float>(weights);
            const float body_throw = kernel::G<float>(weights + 4 * sizeof(float));
            const bool can_basic = std::isfinite(basic) && basic > 0.0f;
            const bool can_throw = std::isfinite(body_throw) && body_throw > 0.0f;
            if (can_basic || can_throw) {
                ++g_ai_melee_choices;
                // Native cases 0/4 emit ATTACK_MELEE/BODY_THROW; keep other choices for defense and abilities.
                const unsigned turn = g_ai_melee_choices % 5;
                if (turn == 3 && can_throw) {
                    ++g_ai_throw_choices;
                    kernel::Return(ctx, 4, 0);
                    return;
                }
                if ((turn == 1 || turn == 2 || turn == 4) && can_basic) {
                    ++g_ai_basic_choices;
                    kernel::Return(ctx, 0, 0);
                    return;
                }
            } else {
                ++g_ai_melee_gated;
            }
        }
    }
    __imp__sub_0008C230(ctx, base);
}

X86_FUNC(__imp__sub_000BADC0);
X86_FUNC(sub_000BADC0) {
    using namespace game::pc_survival;
    const auto actor = Address(ctx.ecx, 0x444) ? kernel::R32(ctx.ecx + 0xF4) : 0;
    if (!g_ai_partner || !WorldMatches() || CoopSlot(actor) != 1 ||
        ctx.ecx != g_coop_controllers[1] || (kernel::R32(kPartner + 0x34) & 6) != 4 ||
        g_coop_down[1] || Dead(actor)) {
        __imp__sub_000BADC0(ctx, base); return;
    }
    struct TargetQuery {
        const uint32_t count = kernel::R32(kCount);
        const bool previous = g_ai_target_query;
        TargetQuery() { g_ai_target_query = true; kernel::W32(kCount, 1); }
        ~TargetQuery() { kernel::W32(kCount, count); g_ai_target_query = previous; }
    } query;
    // Skip only the native two-participant opponent shortcut; retain its NPC search and ranking.
    __imp__sub_000BADC0(ctx, base);
    const auto target = kernel::R32(g_coop_controllers[1] + 0x440);
    if (g_ai_target_reports < 16 && target != g_ai_reported_target) {
        g_ai_reported_target = target;
        ++g_ai_target_reports;
        LOGR("survival-ai: native target actor=%08X target_handle=%08X", actor, target);
    }
}
X86_FUNC(__imp__sub_000D1340);
X86_FUNC(sub_000D1340) {
    using namespace game::pc_survival;
    if (!g_ai_partner || !WorldMatches() || !Address(ctx.ecx, 0xF8)) {
        __imp__sub_000D1340(ctx, base); return;
    }
    struct RestoreMeleeAttacker {
        uint32_t previous = g_ai_melee_attacker;
        ~RestoreMeleeAttacker() { g_ai_melee_attacker = previous; }
    } restore;
    g_ai_melee_attacker = kernel::R32(ctx.ecx + 0xF4);
    const auto victim = kernel::R32(ctx.esp + 4);
    const bool report = g_ai_melee_reports < 8 && CoopSlot(victim) == 1 && AiHostility(g_ai_melee_attacker, victim) == 1;
    const float health = report ? kernel::G<float>(victim + 0x32C) : 0.0f;
    __imp__sub_000D1340(ctx, base);
    if (report && CoopSlot(victim) == 1) {
        ++g_ai_melee_reports;
        LOGR("survival-ai: melee-hit attacker=%08X ally=%08X health=%.2f->%.2f",
             g_ai_melee_attacker, victim, health, kernel::G<float>(victim + 0x32C));
    }
}
X86_FUNC(__imp__sub_0005D9A0);
X86_FUNC(sub_0005D9A0) {
    using namespace game::pc_survival;
    // D1340 rejects CPU-on-CPU melee before damage; only the owned hostile pair bypasses that veto.
    if (kernel::R32(ctx.esp) == 0xD13BD && Address(ctx.ecx, 0xF8) &&
        AiHostility(g_ai_melee_attacker, kernel::R32(ctx.ecx + 0xF4)) == 1) {
        kernel::Return(ctx, 0, 0); return;
    }
    __imp__sub_0005D9A0(ctx, base);
}

X86_FUNC(__imp__sub_000903D0);
X86_FUNC(sub_000903D0) {
    // Native melee treats every CPU as one team; the owned ally belongs to the Survival participants.
    const int hostile = game::pc_survival::AiHostility(ctx.ecx, kernel::R32(ctx.esp + 4));
    if (hostile >= 0) { kernel::Return(ctx, unsigned(!hostile), 1); return; }
    __imp__sub_000903D0(ctx, base);
}

X86_FUNC(__imp__sub_0008CAB0);
X86_FUNC(sub_0008CAB0) {
    const int hostile = game::pc_survival::AiHostility(kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp + 8));
    if (hostile >= 0) { kernel::Return(ctx, unsigned(hostile), 0); return; }
    __imp__sub_0008CAB0(ctx, base);
}
X86_FUNC(__imp__sub_0008D5D0);
X86_FUNC(sub_0008D5D0) {
    const int hostile = game::pc_survival::AiHostility(kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp + 8));
    if (hostile >= 0) { kernel::Return(ctx, unsigned(!hostile), 0); return; }
    __imp__sub_0008D5D0(ctx, base);
}

X86_FUNC(__imp__sub_000B83E0);
X86_FUNC(sub_000B83E0) {
    using namespace game::pc_survival;
    if (g_coop && Address(ctx.ecx, 0x444) && Allies(kernel::R32(ctx.ecx + 0xF4), kernel::R32(ctx.esp + 4))) {
        kernel::W32(ctx.ecx + 0x440, 0); // Native shared target cache must not retain the other human.
        kernel::Return(ctx, 0, 2); return;
    }
    __imp__sub_000B83E0(ctx, base);
}

X86_FUNC(__imp__sub_000F7D70);
X86_FUNC(sub_000F7D70) {
    using namespace game::pc_survival;
    if (g_armed && g_target_load && g_phase != Phase::Leaving) {
        // Native Versus chooses one of the eight music projects with this same RNG step.
        const auto random = kernel::R32(0x53BC5C) * 0x19660Du + 0x3C6EF35Fu;
        kernel::W32(0x53BC5C, random);
        kernel::Return(ctx, (random & 7u) + 1u, 0); return;
    }
    __imp__sub_000F7D70(ctx, base);
}
X86_FUNC(__imp__sub_000F6BB0);
X86_FUNC(sub_000F6BB0) {
    using namespace game::pc_survival;
    if (g_armed && g_target_load && g_phase != Phase::Leaving) {
        // Survival supplies combat events in SpawnWave, rather than the saved story mission's music script.
        const auto music = kernel::R32(0x5148C4);
        if (Address(music, 0x2F8)) {
            ctx.ecx = music;
            kernel::CallGuest(ctx, 0xF7AD0, {0});
        }
        kernel::Return(ctx, 0, 0); return;
    }
    __imp__sub_000F6BB0(ctx, base);
}
X86_FUNC(__imp__sub_000F6B50);
X86_FUNC(sub_000F6B50) {
    using namespace game::pc_survival;
    if (g_armed && (g_target_load || WorldMatches()) && g_phase != Phase::Leaving) { kernel::Return(ctx, 0, 0); return; }
    __imp__sub_000F6B50(ctx, base);
}

X86_FUNC(__imp__sub_000F2F90);
X86_FUNC(sub_000F2F90) {
    using namespace game::pc_survival;
    if (g_armed && g_managed_audio && g_target_load && Address(ctx.ecx, 8) &&
        ctx.ecx == kernel::R32(0x512094) && kernel::R32(ctx.esp) == 0xF08C9) {
        // This call preloads the saved campaign mission; WaveSounds supplies the selected Survival arena instead.
        LOGR("survival: skipped campaign mission sound preload managed_audio=1 coop=%u", unsigned(g_coop));
        kernel::Return(ctx, 0, 2); // Native RET8; F08C9 ignores EAX. Keep the separate SFX_generic call.
        return;
    }
    __imp__sub_000F2F90(ctx, base);
}

X86_FUNC(__imp__sub_0011CF82);
X86_FUNC(sub_0011CF82) {
    using namespace game::pc_survival;
    if (!g_audio_allocating || !g_armed || !g_managed_audio || !AudioPoolReady()) {
        __imp__sub_0011CF82(ctx, base); return;
    }
    // Preserve native alignment, headers and free logic; only the pool bits change during our bank load.
    const auto flags_address = ctx.esp + 12;
    struct RestoreFlags {
        uint32_t address, value;
        ~RestoreFlags() { kernel::W32(address, value); }
    } restore{flags_address, kernel::R32(flags_address)};
    kernel::W32(flags_address, (restore.value & ~0x3Fu) | g_audio_pool_id);
    __imp__sub_0011CF82(ctx, base);
}
