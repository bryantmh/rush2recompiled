// Directional rumble: detects which side of a human player's car was hit, for the Rumble Motors > Directional setting.
//
// How the game rumbles: physics_car_tick (func_80075880) sets a human car's rumble level at the end of each tick. It
// sums the absolute values of three floats at car+0x4, +0x8 and +0xC, counts how many of 40 ascending thresholds at
// 0x800C4010 (4000, 5000, ... 600000) the sum reaches, and writes that to the per-player level D_800D50E8[port]. The
// level ramps and decays in rumble_update (func_80063E4C), which switches the Rumble Pak's single motor on and off.
//
// What the three floats are (measured by logging them while hitting the left wall, the right wall and a wall head on):
// the car's acceleration in its own frame.
//   +0x4  lateral, positive when the car is pushed to its right (a hit on its left side). Tire slides and cornering
//         hold it at 5000 to 9000; wall hits spike it to 36000 and up, in a single tick.
//   +0x8  vertical, spikes by 10000 to 26000 on bumps, landings and road level changes (not used yet).
//   +0xC  longitudinal, 700 to 2000 while driving, down to about -765000 in a head-on hit.
//
// This file turns those into two kinds of impact for src/input.cpp, which decides how long they rumble and on which
// motor:
//   soft  the lateral acceleration rose a little: the tires sliding sideways, as felt while drifting.
//   hard  the lateral acceleration jumped, or the longitudinal one dropped sharply: a wall or another car.
//
// All tuning values are in the constants below. Running with --debug-rumble --show-console prints every impact
// detected in Rumble Motors > Directional and the motor levels, to help with tuning them.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "recomp.h"
#include "rush2_hooks.h"
#include "rush2.h"

namespace {
    // Offsets in the car struct.
    constexpr uint32_t car_lateral = 0x4;
    constexpr uint32_t car_longitudinal = 0xC;

    constexpr int num_ports = 4;

    // ---- Tuning ----

    // Lateral acceleration (absolute) a soft impact needs. The game's own lowest rumble threshold is 4000
    // (0x800C4010[0]), under which it wouldn't rumble at all. Lower this for lighter slides to register.
    constexpr float min_lateral = 4000.0f;

    // How much the lateral acceleration must rise in one tick to count as a soft impact. Sliding and cornering change
    // it by hundreds a tick. Lower this to make slides register more often (and more of them feel like hits), raise
    // it to need sharper sideways movement.
    constexpr float soft_rise = 2500.0f;

    // The same rise, or a drop in the longitudinal acceleration of this much to under its negative (head on), for a
    // hard impact. Wall hits rise by 36000 and more. Lower this to catch lighter scrapes as hard impacts, raise it if
    // slides are being treated as hard impacts.
    constexpr float hard_rise = 8000.0f;

    // Scales the side of an impact (the lateral part of it, from -1 for the right to 1 for the left) before it is
    // clamped to that range. Hits are rarely square on to a wall; above 1, a glancing hit still favors one motor.
    // Lower this to even out the motors more, or raise it so more hits send the far motor to nothing.
    constexpr float side_gain = 1.6f;

    // Acceleration of the previous tick, per port: only the rise since then marks an impact.
    float previous_lateral[num_ports] = {};
    float previous_longitudinal[num_ports] = {};

    float get_f32(uint8_t* rdram, uint32_t addr) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)addr);
        float value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }
}

// At the start of the rumble block of physics_car_tick, before the level is worked out: $a2 = car, $a3 = player.
// Reports an impact's side (see side_gain) and whether it's hard to rush2::input::set_rumble_impact. A head-on hit has
// a small lateral part next to a huge longitudinal one, so its side stays near 0 and it rumbles both motors.
// Does not change the game's own rumble level.
extern "C" void rush2_rumble_impact(uint8_t* rdram, recomp_context* ctx) {
    int port = MEM_BU(1, ctx->r7);
    if (port >= num_ports) {
        return;
    }

    uint32_t car = (uint32_t)ctx->r6;
    float lateral = get_f32(rdram, car + car_lateral);
    float longitudinal = get_f32(rdram, car + car_longitudinal);
    float rise = std::fabs(lateral) - std::fabs(previous_lateral[port]);
    float drop = previous_longitudinal[port] - longitudinal;
    previous_lateral[port] = lateral;
    previous_longitudinal[port] = longitudinal;

    bool hard = (rise >= hard_rise && std::fabs(lateral) >= min_lateral) ||
                (drop >= hard_rise && longitudinal <= -hard_rise);
    if (!hard && (std::fabs(lateral) < min_lateral || rise < soft_rise)) {
        return;
    }

    float side = lateral / (std::fabs(lateral) + std::fabs(longitudinal)) * side_gain;
    side = std::clamp(side, -1.0f, 1.0f);
    // set_rumble_impact takes a side of exactly 0 to mean no impact.
    if (side == 0.0f) {
        side = 0.001f;
    }
    rush2::input::set_rumble_impact(port, side, hard);
}
