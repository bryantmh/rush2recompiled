#ifndef __BATTLE_H__
#define __BATTLE_H__

#include <cstdint>
#include <vector>

#include "recomp.h"
#include "track2049_convert.h"

// Rush 2049's battle mode in Rush 2 (src/battle.cpp, docs/rush2049_research/battle.md): the battle arenas (track select
// ids 30-37) are played in stunt mode without computer cars (Rush 2049's battle was multiplayer only), and this adds what 2049's battle overlay does on top of
// it: health, weapon and power-up pickups, weapons and their projectiles, kills, and a battle HUD.
namespace rush2::battle {
    // The time limit option (Games tab), in minutes.
    enum class TimeLimit : uint32_t { One = 1, Two = 2, Three = 3, Five = 5, Ten = 10 };
    void set_time_limit(TimeLimit limit);
    int time_limit_seconds();

    // The converted arena's pickups, projectile pool and models (called when a battle arena is converted).
    void set_data(const std::vector<rush2::track2049::PickupRecord>& pickups, const std::vector<int>& pool,
                  const std::vector<uint8_t>& converted_geometry, const std::vector<float>& solid_triangles);
    // Race setup: the next physics tick sets everything up again.
    void reset();
    // Once per physics tick of a race (src/track2049_movers.cpp); dt in seconds.
    void tick(uint8_t* rdram, float dt);
    // Whether the race in progress is a battle (its objects are set up).
    bool active(uint8_t* rdram);

    // After the race HUD is built (func_800A06F8): adds the battle HUD's widgets. After the widget draw loop
    // (func_8007D9DC): draws the battle HUD's text.
    void hud_built(uint8_t* rdram, recomp_context* ctx);
    void hud_draw(uint8_t* rdram, recomp_context* ctx);
}

#endif
