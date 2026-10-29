#pragma once
#include <array>
#include <span>

namespace game::pc_survival {
// Native NSD start IDs may repeat; position and scene ownership identify each anchor.
struct ArenaPoint { unsigned id; std::array<float, 3> position; };
struct Arena {
    const char* key;
    const char* label;
    const char* option;
    const char* level;
    const char* scene;
    const char* world;
    std::array<const char*, 3> sound_banks;
    std::array<ArenaPoint, 2> players;
    std::span<const ArenaPoint> enemies;
};
inline constexpr unsigned DefaultArena = 2;
// Original Versus NSD ground anchors; leave balcony/perimeter/lower-floor points unused.
inline constexpr std::array<ArenaPoint, 5> kArenaEnemies0{{
    {4, {11.2042532f, -64.7725067f, 0.999999821f}},
    {4, {11.2042532f, -77.3294449f, 0.999999821f}},
    {4, {18.9772015f, -69.3597107f, 0.999999821f}},
    {4, {-4.73160934f, -67.47509f, 0.999999821f}},
    {4, {-18.8092155f, -63.4321404f, 0.999999821f}}
}};
inline constexpr std::array<ArenaPoint, 6> kArenaEnemies1{{
    {2, {2.0f, 21.125f, 0.224653989f}},
    {3, {23.0f, 21.125f, 0.224653989f}},
    {4, {-1.0f, 10.0f, 0.0f}},
    {4, {25.0f, 9.81245518f, 0.0f}},
    {4, {17.5f, 22.4246578f, 0.0f}},
    {5, {10.9375f, -1.25f, 0.0f}}
}};
inline constexpr std::array<ArenaPoint, 4> kArenaEnemies2{{
    {1, {3.16200018f, 25.0f, 1.00012851f}},
    {3, {-0.661999941f, 33.0f, 1.00012851f}},
    {2, {15.1619997f, 33.0f, 1.00012851f}},
    {0, {11.1619997f, 25.0f, 1.00012851f}}
}};
inline constexpr std::array<ArenaPoint, 6> kArenaEnemies3{{
    {2, {12.9814854f, 33.3389282f, 20.0157967f}},
    {2, {17.6508293f, 27.2313499f, 20.0157967f}},
    {3, {-11.0146599f, 24.8498249f, 20.0423088f}},
    {3, {-7.53032732f, 34.548481f, 20.0423088f}},
    {4, {2.02688622f, 16.9948063f, 20.0169811f}},
    {4, {3.63666773f, 34.7621002f, 20.0169811f}}
}};
inline constexpr std::array<ArenaPoint, 8> kArenaEnemies4{{
    {2, {-16.0f, 8.0f, 0.0f}},
    {3, {-16.0f, -6.0f, 0.0f}},
    {4, {-13.0f, -10.0f, 0.0f}},
    {4, {-13.0f, 12.0f, 0.0f}},
    {4, {-4.0f, 0.0f, 0.0f}},
    {4, {-7.0f, 13.0f, 0.0f}},
    {4, {-4.0f, -13.0f, 0.0f}},
    {4, {-20.0f, -5.0f, 0.0f}}
}};
inline constexpr std::array<ArenaPoint, 8> kArenaEnemies5{{
    {2, {14.0f, -8.0f, 0.0f}},
    {2, {5.0f, 18.0f, 0.0f}},
    {3, {-14.0f, -8.0f, 0.0f}},
    {3, {-5.0f, 18.0f, 0.0f}},
    {4, {-13.0f, 1.0f, 0.0f}},
    {4, {13.0f, 1.0f, 0.0f}},
    {4, {4.0f, 15.0f, 0.0f}},
    {4, {-4.0f, 15.0f, 0.0f}}
}};
inline constexpr std::array<ArenaPoint, 5> kArenaEnemies6{{
    {2, {-24.8586044f, 4.70842552f, -19.9999981f}},
    {3, {-24.5005779f, -0.709076703f, -19.9999981f}},
    {4, {-30.0181999f, 7.4210248f, -20.0f}},
    {0, {-28.2860641f, -1.85105693f, -20.0f}},
    {1, {-18.8854294f, 5.50284052f, -19.9999981f}}
}};

// IDs and selection keys follow the original seven-entry Versus venue table.
inline constexpr std::array<Arena, 7> Arenas{{
    {"BrooklynBridge", "BROOKLYN BRIDGE", "SURVIVAL - BROOKLYN BRIDGE",
     "BrooklynBridge", "BrooklynBridge_MP", "BrooklynBridge.nil",
     {"SFX_ambiance_bridge", "BrooklynBridge_MP", "BrooklynBridge_MP_amb"},
     {{{0, {8.24064922f, -69.0151062f, 0.999999821f}}, {1, {-9.84866905f, -67.2570953f, 0.999999821f}}}}, kArenaEnemies0},
    {"NYCStreets", "NYC STREETS", "SURVIVAL - NYC STREETS",
     "NYCStreets2", "NYCStreets_MP", "NYCStreets2.nil",
     {"SFX_ambiance_NYC", "NYCStreets_MP", "NYCStreets_MP_amb"},
     {{{0, {3.0f, 10.0f, 0.0f}}, {1, {20.0f, 10.0f, 0.0f}}}}, kArenaEnemies1},
    {"AvengersMansion", "AVENGERS MANSION", "SURVIVAL - AVENGERS MANSION",
     "AvengersMansion", "AvengersMansion_MP", "AvengersMansion.nil",
     {"SFX_ambiance_mansion", "AvengersMansion_MP", "AvengersMansion_MP_amb"},
     {{{0, {11.1619997f, 25.0f, 1.00012851f}}, {1, {3.16200018f, 25.0f, 1.00012851f}}}}, kArenaEnemies2},
    {"DailyBugle", "DAILY BUGLE", "SURVIVAL - DAILY BUGLE",
     "DailyBugle", "DailyBugle_MP", "DailyBugle.nil",
     {"SFX_ambiance_Bugle", "DailyBugle_MP", "DailyBugle_MP_amb"},
     {{{0, {7.66057634f, 26.2379589f, 20.0083256f}}, {1, {-4.57150888f, 25.2542477f, 20.0130215f}}}}, kArenaEnemies3},
    {"GrandCentralStation", "GRAND CENTRAL STATION", "SURVIVAL - GRAND CENTRAL STATION",
     "GrandCentral", "GrandCentral_MP", "GrandCentral.nil",
     {"SFX_ambiance_GrandCentral", "GrandCentral_MP", "GrandCentral_MP_amb"},
     {{{0, {-10.3622408f, 6.0f, 0.0f}}, {1, {-10.3622408f, -4.0f, 0.0f}}}}, kArenaEnemies4},
    {"PowerPlant", "POWER PLANT", "SURVIVAL - POWER PLANT",
     "PowerPlant", "PowerPlant_MP", "PowerPlant.nil",
     {"SFX_ambiance_power_plant", "PowerPlant_MP", "PowerPlant_MP_amb"},
     {{{0, {-6.0f, 1.0f, 0.0f}}, {1, {6.0f, 1.0f, 0.0f}}}}, kArenaEnemies5},
    {"RoekelFacility", "ROEKEL FACILITY", "SURVIVAL - ROEKEL FACILITY",
     "RoekelFacility", "RoekelFacility_MP", "RoekelFacility.nil",
     {"SFX_ambiance_Roekel", "RoekelFacility_MP", "RoekelFacility_MP_amb"},
     {{{0, {-28.2860641f, -1.85105693f, -20.0f}}, {1, {-18.8854294f, 5.50284052f, -19.9999981f}}}}, kArenaEnemies6}
}};
constexpr const Arena* ArenaFor(unsigned venue) { return venue < Arenas.size() ? &Arenas[venue] : nullptr; }
static_assert(ArenaFor(DefaultArena) && !ArenaFor(7));
static_assert([] {
    for (const auto& arena : Arenas) {
        if (arena.players[0].id != 0 || arena.players[1].id != 1 || arena.enemies.size() < 3) return false;
        for (const auto& point : arena.enemies) for (const auto v : point.position)
            if (!(v >= -10000.0f && v <= 10000.0f)) return false;
    }
    return true;
}());
}  // namespace game::pc_survival
