#ifndef __EXPLOSIVE_H__
#define __EXPLOSIVE_H__

#include <cstdint>

#include "recomp.h"

// DEATHS' third value, EXPLOSIVE: the SF Rush PlayStation version's Explosive mode (docs/sfrush_ps1.md#explosive-mode)
// on any track. A race with it is a DEATHS race (one life: the race global 0x8010BC98 holds 2, which every game test
// of it reads as on) whose human players carry a speed bomb: it arms the first time their speedometer reaches 66 mph
// and wrecks the car the moment it then reads under 60, which with DEATHS puts the car out. The skull on the HUD
// blinks while a player's bomb is armed (src/explosive.cpp; the track select row in src/rush2049/track2049_menu.cpp).
namespace rush2::explosive {
    constexpr int deaths_value = 2;          // the track select's DEATHS (0x800D5760 + 0x11) and 0x8010BC98 with EXPLOSIVE
    constexpr int arm_mph = 66;              // PS1 0x800161C4: speed >= 0x42 arms the bomb
    constexpr int blow_mph = 60;             // PS1 0x800161E0: speed < 0x3C with the bomb armed wrecks the car

    // Game thread. Whether the race (or the menu's race settings) has DEATHS at EXPLOSIVE.
    bool on(uint8_t* rdram);
    // Game thread. Whether car `car`'s bomb is armed in this race.
    bool armed(int car);
}

#endif
