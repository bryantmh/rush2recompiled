#ifndef __BATTLE_H__
#define __BATTLE_H__

#include <cstdint>
#include <vector>

#include "recomp.h"
#include "track2049_convert.h"

// Rush 2049's battle mode in Rush 2 (src/rush2049/battle.cpp, docs/rush2049_research/battle.md): the battle arenas (track select
// ids 30-37) are played in stunt mode (Rush 2049's battle was multiplayer only; the computer opponents are
// src/rush2049/battle_ai.cpp), and this adds what 2049's battle overlay does on top of it: health, weapon and power-up pickups, weapons and their projectiles, kills, and a battle HUD.
namespace rush2::battle {
    // The old Games tab time limit option, in minutes. It is hidden now and only read once: a limit changed from its
    // default (3) becomes the BATTLE select's TIME LIMIT.
    enum class TimeLimit : uint32_t { One = 1, Two = 2, Three = 3, Five = 5, Ten = 10 };
    void set_time_limit(TimeLimit limit);
    int legacy_time_limit_minutes();

    // Rush 2049's battle rules (its setup overlay, ROM 0xB5C534, left / right at 0x8038EC80): the points (kills) that
    // win the round, 5-50 in steps of 5 (default 10), and the time limit, off by default or 1-20 minutes (2049 keeps
    // on / off and the minutes apart; its minutes default to 8). A round ends when a car reaches the points or the
    // clock runs out, which without a time limit is after 1200 s. The BATTLE track select's POINTS and TIME LIMIT rows
    // (src/rush2049/track2049_menu.cpp).
    constexpr int min_points = 5, max_points = 50, points_step = 5, default_points = 10;
    constexpr int max_minutes = 20, default_minutes = 0;    // 0: no time limit
    constexpr int untimed_seconds = 1200;
    void set_points_to_win(int points);
    int points_to_win();
    void set_time_limit_minutes(int minutes);
    int time_limit_minutes();
    // The round's clock in seconds: the time limit, or untimed_seconds without one.
    int time_limit_seconds();

    // The converted arena's pickups, projectile pool and models (called when a battle arena is converted).
    void set_data(const std::vector<rush2::track2049::PickupRecord>& pickups, const std::vector<int>& pool,
                  const std::vector<uint8_t>& converted_geometry, const std::vector<float>& solid_triangles);
    // Race setup: the next physics tick sets everything up again.
    void reset();
    // Once per physics tick of a race (src/rush2049/track2049_movers.cpp); dt in seconds.
    void tick(uint8_t* rdram, float dt, recomp_context* ctx = nullptr);
    // Whether the race in progress is a battle (its objects are set up).
    bool active(uint8_t* rdram);
    // The Weapons cheat (Cheats tab): gives the players weapons in the other races. 0 off, 1-8 a weapon (cannon,
    // gatling, grenade, mine, missile, ram, rocket, sonic), 9 invisibility, 10 a random weapon each time.
    void set_weapons_cheat(int option);
    // The race track select's BATTLE row (src/rush2049/track2049_menu.cpp): a race with weapons. Every car has the
    // battle's health but no default gun, a row of pickups (a random weapon or power-up each) lies on each checkpoint
    // line, and the
    // computer cars leave their racing line a little for a pickup ahead and fire at the cars around them. Read when a
    // race starts.
    void set_race_battle(bool on);
    bool race_battle();
    // Whether the race being set up or run is a battle race: the row is on, the Rush 2049 tracks are there, and it
    // isn't stunt mode (the arenas, 0x8010C3E8 == 2), a battle arena or a ghost race. Such a race has half as much
    // time again at its start and checkpoints (rush2_track49_race_time).
    bool race_battle_applies(uint8_t* rdram);
    // Settings: holding the steering stick back while firing shoots behind the car (rush2::controls::battle_back),
    // with every weapon that shoots ahead (not the mine, ram or sonic blast). On by default.
    void set_fire_backward(bool on);
    // A player's team (0-3: blue, red, yellow, green; Games tab). Cars of a team don't damage each other.
    void set_team(int player, int team);
    int team_of(int player);
    // For the arrows over the other cars (src/arrows.cpp): whether car is a live car of the battle in progress, and
    // whether it is invisible.
    bool arrow_target(uint8_t* rdram, int car, bool& faded);

    // After the race HUD is built (func_800A06F8): adds the battle HUD's widgets. After the widget draw loop
    // (func_8007D9DC): draws the battle HUD's text.
    void hud_built(uint8_t* rdram, recomp_context* ctx);
    void hud_draw(uint8_t* rdram, recomp_context* ctx);
}

#endif
