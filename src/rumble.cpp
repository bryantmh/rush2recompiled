// Directional rumble: detects which side of a human player's car was hit, for Rumble Mode > Enhanced (control_profiles.h; called Directional before).
//
// How the game rumbles: physics_car_tick (func_80075880) sets a human car's rumble level at the end of each tick. It
// sums the absolute values of three floats at car+0x4, +0x8 and +0xC, counts how many of 40 ascending thresholds at
// 0x800C4010 (4000, 5000, ... 600000) the sum reaches, and writes that to the per-player level D_800D50E8[port]. The
// level ramps and decays in rumble_update (func_80063E4C), which switches the Rumble Pak's single motor on and off.
//
// What the three floats are: the net force on the car in its own frame, written by car_force_sum_8006A02C (the four
// wheels' forces +0x58.., gravity rotated into the car's frame +0x200.., and the external force accumulator
// +0x11C..). The collision code adds into that accumulator, but it's back to 0 by the time this hook runs, so the
// accumulator itself can't tell a hit from cornering.
//   +0x4  sideways, positive = toward the car's right. Verified 2026-10-11 (tools/rush1/shots.py on the first race):
//         steering right holds it at about +7000 and left at about -7000 (the tires' cornering force), and driving the
//         car's left side into the building after the start spiked it from -2900 to +22000 in one tick.
//   +0x8  vertical, spikes by 10000 to 26000 on bumps, landings and road level changes (not used yet).
//   +0xC  forward, 700 to 5000 while driving, down to about -150000 to -765000 in a head-on hit.
//
// This file turns those into two kinds of impact for src/input.cpp, which decides how long they rumble and on which
// side:
//   soft  the sideways force rose a little: cornering or the tires sliding. Its side is the outside of the turn (a
//         left turn pushes the car left, which reads as a hit on its right), like being thrown against the door.
//   hard  the sideways force jumped, or the forward one dropped sharply: a wall or another car.
// It also reports the cornering side every tick (set_rumble_cornering), which Rumble Effects > Distinct plays the tire
// screech on while the game rumbles without a recent hard impact, and wall grinding (set_rumble_grinding, found by
// rush2_rumble_wall), which the force sum misses after the first impact: the game pushes the car out of a wall by moving
// it, not with a force.
//
// All tuning values are in the constants below. Running with --debug-rumble --show-console prints every impact
// detected in Rumble Mode > Directional and the side levels, to help with tuning them.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "recomp.h"
#include "rush2_hooks.h"
#include "rush2.h"
#include "online.h"
#include "light_bar.h"

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

    // Acceleration of the previous tick, per port: only the rise since then marks an impact. Not valid on a race's first
    // tick (or after a pause), when there's nothing to compare with yet.
    float previous_lateral[num_ports] = {};
    float previous_longitudinal[num_ports] = {};
    bool have_previous[num_ports] = {};

    // Wall grinding (rush2_rumble_wall).
    constexpr uint32_t car_velocity = 0x218; // World velocity, 3 floats.
    constexpr uint32_t car_matrix = 0x2CC;   // World to car frame, row-major 3x3 (rows: right, up, forward).
    // Speed along the wall at which grinding rumbles fully, in the velocity's units (close to mph: a scrape logged
    // with --debug-rumble read 51 down to 36 while the speedometer showed 49 down to 35). Under grind_min_speed, a car
    // resting against a wall, it doesn't rumble.
    constexpr float grind_full_speed = 50.0f;
    constexpr float grind_min_speed = 3.0f;
    // Like side_gain, for the side of the wall: a wall square to the car's side is a side of 1.
    constexpr float grind_side_gain = 1.4f;

    // Game thread only. Each port's car (from the last rush2_rumble_impact), and the wall contact found by
    // rush2_rumble_wall since that car's last rumble block: the strongest one's side and intensity.
    uint32_t port_car[num_ports] = {};
    bool wall_contact[num_ports] = {};
    float wall_side[num_ports] = {};
    float wall_intensity[num_ports] = {};

    float get_f32(uint8_t* rdram, uint32_t addr) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)addr);
        float value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }
}

// In car_wall_collision_query_8006FC94, where a body probe is inside a wall polygon (surface types 5 and 6 and the
// rest of what the query finds): the game pushes the car out by moving its position (+0x224) along the wall's normal
// (sp+0xB0, world frame, pointing out of the wall) times the depth. That push never goes through the car's force sum
// (+0x4..), so a car grinding along a wall only shows in the force sum at the first impact; this catches every tick of
// it. $s1 = car. Records the contact's side and how fast the car slides along the wall for rush2_rumble_impact.
extern "C" void rush2_rumble_wall(uint8_t* rdram, recomp_context* ctx) {
    float normal[3];
    for (int i = 0; i < 3; i++) {
        normal[i] = get_f32(rdram, (uint32_t)ctx->r29 + 0xB0 + 4 * i);
    }
    rush2_rumble_wall_contact(rdram, (uint32_t)ctx->r17, normal);
}

// A wall contact of `car` with a wall whose normal (world frame, out of the wall) is `normal`: Rush 2's probe above,
// and California Speed's wall collision on its tracks (src/calspeed/calspeed_walls.cpp).
extern "C" void rush2_rumble_wall_contact(uint8_t* rdram, uint32_t car, const float* normal) {
    if (!rush2::input::rumble_directional()) {
        return;
    }
    int port = -1;
    for (int i = 0; i < num_ports; i++) {
        if (port_car[i] == car) {
            port = i;
        }
    }
    if (port < 0) {
        return;
    }
    float velocity[3], m[9];
    for (int i = 0; i < 3; i++) {
        velocity[i] = get_f32(rdram, car + car_velocity + 4 * i);
    }
    for (int i = 0; i < 9; i++) {
        m[i] = get_f32(rdram, car + car_matrix + 4 * i);
    }
    // The normal in the car's frame: x > 0 pushes the car to its right, so the wall is on its left.
    float side_x = m[0] * normal[0] + m[1] * normal[1] + m[2] * normal[2];
    float side = std::clamp(side_x * grind_side_gain, -1.0f, 1.0f);
    // Speed along the wall: the velocity less its part into the wall.
    float into = velocity[0] * normal[0] + velocity[1] * normal[1] + velocity[2] * normal[2];
    float along = 0.0f;
    for (int i = 0; i < 3; i++) {
        float v = velocity[i] - into * normal[i];
        along += v * v;
    }
    along = std::sqrt(along);
    float intensity = std::clamp((along - grind_min_speed) / (grind_full_speed - grind_min_speed), 0.0f, 1.0f);
    if (!wall_contact[port] || intensity > wall_intensity[port]) {
        wall_side[port] = side;
        wall_intensity[port] = intensity;
    }
    wall_contact[port] = true;
    if (rush2::input::rumble_debug_enabled()) {
        fprintf(stderr, "rumble port %d: wall contact side %+.2f, speed along it %.1f\n", port, side, along);
    }
}

// At the start of the rumble block of physics_car_tick, before the level is worked out: $a2 = car, $a3 = player.
// Reports an impact's side (see side_gain) and whether it's hard to rush2::input::set_rumble_impact. A head-on hit has
// a small lateral part next to a huge longitudinal one, so its side stays near 0 and it rumbles both motors.
// Does not change the game's own rumble level.
extern "C" void rush2_rumble_impact(uint8_t* rdram, recomp_context* ctx) {
    int port = rush2::online::game::rumble_port((uint32_t)ctx->r7, MEM_BU(1, ctx->r7));
    if (port >= num_ports) {
        return;
    }
    // The light bar shows the car's MAIN COLOR (type +0x7EA, MAIN +0x7EB; docs/npc_cars.md, src/light_bar.cpp).
    rush2::light_bar::set_car(port, MEM_BU(0x7EA, ctx->r6), MEM_BU(0x7EB, ctx->r6));
    if (rush2::input::take_rumble_impact_reset(port)) {
        have_previous[port] = false;
    }
    if (!rush2::input::rumble_directional()) {
        return;
    }

    uint32_t car = (uint32_t)ctx->r6;
    port_car[port] = car;
    // Wall grinding found this tick (rush2_rumble_wall), or none.
    rush2::input::set_rumble_grinding(port, wall_contact[port] ? wall_side[port] : 0.0f,
        wall_contact[port] ? wall_intensity[port] : 0.0f);
    wall_contact[port] = false;

    float lateral = get_f32(rdram, car + car_lateral);
    float longitudinal = get_f32(rdram, car + car_longitudinal);
    // The side the tires are loaded on right now, every tick (0 under the game's lowest rumble threshold): the outside
    // of the turn, where Rumble Effects > Distinct plays the tire screech.
    float cornering = 0.0f;
    if (std::fabs(lateral) >= min_lateral) {
        cornering = std::clamp(lateral / (std::fabs(lateral) + std::fabs(longitudinal)) * side_gain, -1.0f, 1.0f);
    }
    rush2::input::set_rumble_cornering(port, cornering);
    float rise = std::fabs(lateral) - std::fabs(previous_lateral[port]);
    float drop = previous_longitudinal[port] - longitudinal;
    bool had_previous = have_previous[port];
    previous_lateral[port] = lateral;
    previous_longitudinal[port] = longitudinal;
    have_previous[port] = true;
    if (!had_previous) {
        return;
    }

    bool hard = (rise >= hard_rise && std::fabs(lateral) >= min_lateral) ||
                (drop >= hard_rise && longitudinal <= -hard_rise);
    if (!hard && (std::fabs(lateral) < min_lateral || rise < soft_rise)) {
        return;
    }

    float side = lateral / (std::fabs(lateral) + std::fabs(longitudinal)) * side_gain;
    side = std::clamp(side, -1.0f, 1.0f);
    rush2::input::set_rumble_impact(port, side, hard);
}
