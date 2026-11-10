// A custom track's [handling] traction (include/track_mod.h, docs/custom_tracks.md#handling): steps of speed at which
// every car's push along the ground weakens, the PS1 SF Rush's snow (docs/sfrush_ps1.md#snow-and-night). The PS1's
// car_integrate (0x8005D190), in snow, halves every car's horizontal forces for the frame (its thrust, the tires'
// friction, the rolling friction: everything summed at 0x800ED0FC but the vertical part) from speed 151, and again from
// 301. Top speed stays, as thrust and friction weaken together; speeding up, braking and turning take two or four times
// as long.
//
// Rush 2 sums a car's forces in its own frame (car +0x4, +0x8, +0xC: x across, y up, z ahead; car_force_sum_8006A02C,
// gravity rotated in with them) and car_linear_accel_stage_80069A54 turns them into accelerations: +0x1C, +0x20, +0x24
// = force x 0x80110018 / the weight (+0x5AC; $f0 holds the quotient). rush2_track_traction, before its jal at
// 0x80069A9C (+0x1C stored, +0x24's value in $f6 for the delay slot), scales the across and ahead parts by the step the
// car's speed (+0x3D4, ft/s) has reached, gravity's share of them left whole (the PS1's gravity isn't one of the
// forces it scales): gravity there is the weight along -world y, the car's matrix +0x2CC (rows right, up, forward)
// turning it into the car's frame. The turn the tires give (car_torque_from_forces_80069AB4) is left as it is, as the
// PS1's (func_800608E0) is.

#include <cstdint>
#include <cstring>

#include "recomp.h"

#include "rush2_hooks.h"
#include "track2049.h"

namespace {
    constexpr uint32_t car_force = 0x4;       // f32[3], the car's frame
    constexpr uint32_t car_accel = 0x1C;      // f32[3]
    constexpr uint32_t car_speed = 0x3D4;     // f32 ft/s
    constexpr uint32_t car_weight = 0x5AC;    // f32 lb
    constexpr uint32_t car_matrix = 0x2CC;    // f32[9]: rows right, up, forward (world)
    constexpr float mph_per_fps = 3600.0f / 5280.0f;

    float read_f(uint8_t* rdram, uint32_t at) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    void write_f(uint8_t* rdram, uint32_t at, float f) {
        uint32_t w;
        memcpy(&w, &f, 4);
        MEM_W(0, (int32_t)at) = (int32_t)w;
    }
}

// car_linear_accel_stage at 0x80069A9C: $a0 = the car, $f0 = 0x80110018 / its weight, $f6 = its ahead acceleration.
extern "C" void rush2_track_traction(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r4;
    float factor = rush2::track2049::mod_traction(rdram, read_f(rdram, car + car_speed) * mph_per_fps);
    if (factor == 1.0f) return;
    float weight = read_f(rdram, car + car_weight), per = ctx->f0.fl;
    // Gravity in the car's frame: the weight along -world y, each axis its row's y.
    float g_across = -weight * read_f(rdram, car + car_matrix + 1 * 4);
    float g_ahead = -weight * read_f(rdram, car + car_matrix + 7 * 4);
    float across = read_f(rdram, car + car_force), ahead = read_f(rdram, car + car_force + 8);
    write_f(rdram, car + car_accel, (g_across + (across - g_across) * factor) * per);
    ctx->f6.fl = (g_ahead + (ahead - g_ahead) * factor) * per;
}
