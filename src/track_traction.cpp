// A custom track's [handling] traction (include/track_mod.h, docs/custom_tracks.md#handling): steps of speed at which
// every car's push along the ground weakens, the PS1 SF Rush's snow (docs/sfrush_ps1.md#snow-and-night). The PS1's
// car_integrate (0x8005D190), in snow, halves every car's horizontal forces for the frame (its thrust, the tires'
// friction, the rolling friction: everything summed at 0x800ED0FC but the vertical part) from speed 151, and again from
// 301. Top speed stays, as thrust and friction weaken together; speeding up, braking and turning take two or four times
// as long. The PS1 snow conversion keeps only the first halving (the quarter played too slowly: ps1_track.cpp).
//
// Rush 2 sums a car's forces in its own frame (car +0x4, +0x8, +0xC: x across, y up, z ahead; car_force_sum_8006A02C,
// gravity rotated in with them) and car_linear_accel_stage_80069A54 turns them into accelerations: +0x1C, +0x20, +0x24
// = force x 0x80110018 / the weight (+0x5AC; $f0 holds the quotient). rush2_track_traction, before its jal at
// 0x80069A9C, does what the PS1 does to its sum: turns the whole force sum, gravity in it, into the world (the car's
// matrix +0x2CC, rows right, up, forward), scales its horizontal parts (world x and z) by the step the car's speed
// (+0x3D4, ft/s) has reached, keeps the vertical one, and turns it back. The PS1's physics differ: on the ground it
// has no gravity along a slope at all (func_8005E0CC adds gravity only while a wheel is off the ground, and with all
// four down takes the velocity's part along the car's up axis away), while Rush 2's slopes push through the ground
// forces' horizontal parts; scaling those with the thrust keeps a hill in snow what it is dry, slowed alike, where
// keeping them whole (the first version) left a car on a quarter of its thrust against all of a hill. In the air
// gravity is vertical and kept whole, as the PS1's. The turn the tires give (car_torque_from_forces_80069AB4) is
// left as it is, as the PS1's (func_800608E0) is.

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

// car_linear_accel_stage at 0x80069A9C: $a0 = the car, $f0 = 0x80110018 / its weight, $f6 = its ahead acceleration
// (+0x1C and +0x20 already stored, +0x24 stored from $f6 in the jal's delay slot).
extern "C" void rush2_track_traction(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r4;
    float factor = rush2::track2049::mod_traction(rdram, read_f(rdram, car + car_speed) * mph_per_fps);
    if (factor == 1.0f) return;
    float per = ctx->f0.fl;
    float m[9], f[3], w[3];
    for (int i = 0; i < 9; i++) m[i] = read_f(rdram, car + car_matrix + i * 4);
    for (int i = 0; i < 3; i++) f[i] = read_f(rdram, car + car_force + i * 4);
    // The force sum (gravity in it) in the world: each car axis's row times its part.
    for (int j = 0; j < 3; j++) w[j] = f[0] * m[j] + f[1] * m[3 + j] + f[2] * m[6 + j];
    w[0] *= factor;
    w[2] *= factor;
    // Back into the car's frame.
    for (int i = 0; i < 3; i++) f[i] = w[0] * m[i * 3] + w[1] * m[i * 3 + 1] + w[2] * m[i * 3 + 2];
    write_f(rdram, car + car_accel, f[0] * per);
    write_f(rdram, car + car_accel + 4, f[1] * per);
    ctx->f6.fl = f[2] * per;
}
