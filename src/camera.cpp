// Race camera options: Attract Camera and LOOK BACK (docs/camera.md).
//
// Attract Camera (option, off by default). Each player's camera mode is car state +0x348. In a race the VIEW button
// cycles 0-3 (cam_update_all_players_8009D9F4 at 0x8009DD6C: bumper, hood, and two chase cameras) and wraps at 4. The
// attract demo (race_game_mode_800FAE6C nonzero) instead rolls one of six every few seconds (0x8009DC38, jump table
// 0x800CFDC4): 0-3 as in a race, the trackside camera 6 (cam_update_random_orbit_attract_8009D858), and 10, the high
// chase camera: cam_set_chase_mode_8009B480 sets the view's chase distance 0x800E7D10[view] to 20 and height
// 0x800E7CC0[view] to 6 (attract values, 30 / 12 outside it, 16 / 6 in a race) and the mode becomes 10, which
// cam_update_dispatch_8009D02C runs through cam_update_mode_e_8009C050. With the option on, VIEW goes 0, 1, 2, 3, 10
// and back to 0; mode 10 is set up exactly as the attract demo does it.
//
// LOOK BACK (a binding, D-pad left by default; src/controls.cpp). Every camera mode leaves its camera in the view's
// block 0x800E79D0 + view * 0x40 (3x3 matrix whose rows are the camera's axes in world space, position at +0x24),
// which the frame's view setup, visibility, sky and sound read. While a player holds LOOK BACK in a driving camera
// (0-3, 10) the block is turned 180 degrees about the car's up axis (row 1 of its matrix, car state +0x24) through
// the car's position, after cam_update_dispatch_8009D02C returns: the chase cameras then sit ahead of the car looking
// back at it, the bumper and hood cameras at its rear facing backward. The chase cameras' own state (0x800E7B38 per
// view) is left alone, so letting go puts the camera back where it would have been. The 2 and 3 chase cameras keep
// out of the ground with collision_ground_query_basis_8008BDC0 when the car is off its path (0x8009D4A0); the turned
// camera does the same, with its own polygon cache so the chase camera's (+0x30) isn't moved. Frame interpolation
// (src/interpolation.cpp) sees the turn as a camera cut and snaps.

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "recomp.h"

#include "online.h"
#include "rush2.h"
#include "rush2_hooks.h"

extern "C" void cam_set_chase_mode_8009B480(uint8_t* rdram, recomp_context* ctx);
extern "C" void collision_ground_query_basis_8008BDC0(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t attract_running = 0x800FAE6C; // race_game_mode: nonzero while the attract demo runs.
    constexpr uint32_t race_type = 0x8010C3E8;
    constexpr uint32_t frame_retraces = 0x8002301C;
    constexpr uint32_t car_raise = 0x800CFD84;       // The camera's view of the car is raised by this (0.3).

    // Per view (moved there for four players, src/players4.cpp): chase height (from 0x800E7CC0) and distance
    // (from 0x800E7D10), f32 each.
    constexpr uint32_t chase_height = 0x802404B0;
    constexpr uint32_t chase_distance = 0x802404C0;
    // The attract demo's values for them (cam_set_chase_mode_8009B480 with race_attract_mode set).
    constexpr float attract_height = 6.0f;
    constexpr float attract_distance = 20.0f;

    // Camera blocks, 0x40 per view: matrix rows +0x0, +0xC, +0x18; position +0x24; ground polygon cache +0x30;
    // ground-checked position +0x34.
    constexpr uint32_t cameras = 0x800E79D0;
    constexpr uint32_t camera_size = 0x40;
    constexpr int32_t cam_pos = 0x24;
    constexpr int32_t cam_poly = 0x30;
    constexpr int32_t cam_ground = 0x34;
    constexpr int max_views = 4;

    // Car state (0x354 per car).
    constexpr int32_t car_pos = 0x0;
    constexpr int32_t car_matrix = 0x24;
    constexpr int32_t car_section = 0x309;   // s8: the chase cameras skip the ground check while 0 to 0x14.
    constexpr int32_t car_view = 0x347;
    constexpr int32_t car_mode = 0x348;
    // Physics car (0x81C per car, index car state +0x346): +0x6C8 >= 0 while it glides back after a wreck.
    constexpr uint32_t physics_cars = 0x800F5470;
    constexpr uint32_t physics_size = 0x81C;
    constexpr int32_t physics_glide = 0x6C8;
    constexpr int32_t car_index = 0x346;

    constexpr int mode_far_chase = 3;
    constexpr int mode_high_chase = 10;

    std::atomic<bool> attract_camera = false;
    // The turned camera's ground polygon cache, per view.
    std::array<uint32_t, max_views> look_poly{};

    float get_f32(uint8_t* rdram, uint32_t addr) {
        uint32_t v = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    void put_f32(uint8_t* rdram, uint32_t addr, float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        MEM_W(0, (int32_t)addr) = (int32_t)v;
    }
    gpr sx(uint32_t v) {
        return (gpr)(int64_t)(int32_t)v;
    }

    struct Vec {
        float x, y, z;
    };
    Vec get_vec(uint8_t* rdram, uint32_t addr) {
        return { get_f32(rdram, addr), get_f32(rdram, addr + 4), get_f32(rdram, addr + 8) };
    }
    void put_vec(uint8_t* rdram, uint32_t addr, Vec v) {
        put_f32(rdram, addr, v.x);
        put_f32(rdram, addr + 4, v.y);
        put_f32(rdram, addr + 8, v.z);
    }
    // v turned 180 degrees about the unit axis u: 2 (u . v) u - v.
    Vec half_turn(Vec v, Vec u) {
        float d = 2.0f * (u.x * v.x + u.y * v.y + u.z * v.z);
        return { d * u.x - v.x, d * u.y - v.y, d * u.z - v.z };
    }
}

void rush2::set_attract_camera(bool enabled) {
    attract_camera.store(enabled, std::memory_order_relaxed);
}

// cam_update_all_players_8009D9F4 at 0x8009DE30, where every path has the view's new camera mode in $s0 ($a0 = the
// old one, $s1 = the car state). Outside the attract demo, new 0 with old 3 is only VIEW wrapping from the far chase
// camera; with Attract Camera on it goes to the high chase camera instead, set up as the attract demo does
// (0x8009DCC0: cam_set_chase_mode_8009B480, then mode 10). From 10 VIEW wraps to 0 as usual.
extern "C" void rush2_attract_camera_cycle(uint8_t* rdram, recomp_context* ctx) {
    if (!attract_camera.load(std::memory_order_relaxed) || MEM_BU(0, (int32_t)attract_running) != 0 ||
        (int32_t)ctx->r16 != 0 || (int8_t)ctx->r4 != mode_far_chase) {
        return;
    }
    uint32_t car = (uint32_t)ctx->r17;
    int view = (int8_t)MEM_B(car_view, (int32_t)car);
    if (view < 0 || view >= max_views) {
        return;
    }
    recomp_context call = *ctx;
    call.r4 = sx(car);
    cam_set_chase_mode_8009B480(rdram, &call);
    put_f32(rdram, chase_height + view * 4, attract_height);
    put_f32(rdram, chase_distance + view * 4, attract_distance);
    ctx->r16 = mode_high_chase;
}

// cam_update_all_players_8009D9F4 at 0x8009DE58, after cam_update_dispatch_8009D02C placed view $s3's camera for car
// state $s1 ($s4 = the view's player record, +1 its controller port; online, view i is this machine's port i).
extern "C" void rush2_look_back(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r17;
    int view = (int8_t)ctx->r19;
    if (view < 0 || view >= max_views || MEM_BU(0, (int32_t)attract_running) != 0) {
        return;
    }
    int mode = (int8_t)MEM_B(car_mode, (int32_t)car);
    if ((mode < 0 || mode > mode_far_chase) && mode != mode_high_chase) {
        look_poly[view] = 0;
        return;
    }
    int port = rush2::online::game::in_race() ? view : MEM_BU(1, (int32_t)ctx->r20);
    if (!rush2::controls::look_back_held(port)) {
        look_poly[view] = 0;
        return;
    }

    uint32_t cam = cameras + view * camera_size;
    Vec pivot = get_vec(rdram, car + car_pos);
    Vec up = get_vec(rdram, car + car_matrix + 0xC);
    float len = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
    if (len < 1e-4f) {
        return;
    }
    up = { up.x / len, up.y / len, up.z / len };
    for (int row = 0; row < 3; row++) {
        put_vec(rdram, cam + row * 0xC, half_turn(get_vec(rdram, cam + row * 0xC), up));
    }
    Vec p = get_vec(rdram, cam + cam_pos);
    Vec d = half_turn({ p.x - pivot.x, p.y - pivot.y, p.z - pivot.z }, up);
    put_vec(rdram, cam + cam_pos, { pivot.x + d.x, pivot.y + d.y, pivot.z + d.z });

    // The 2 and 3 chase cameras' ground check (cam_update_dispatch_8009D02C at 0x8009D4A0-0x8009D51C).
    if (mode != 2 && mode != mode_far_chase) {
        return;
    }
    uint32_t physics = physics_cars + (uint32_t)(int8_t)MEM_B(car_index, (int32_t)car) * physics_size;
    int section = (int8_t)MEM_B(car_section, (int32_t)car);
    if ((int16_t)MEM_H(physics_glide, (int32_t)physics) >= 0 || (section >= 0 && section < 0x15) ||
        MEM_W(0, (int32_t)race_type) == 3) {
        put_vec(rdram, cam + cam_ground, get_vec(rdram, cam + cam_pos));
        return;
    }
    // As the game calls it: (polygon cache, camera position, ground-checked position out, the car's raised position,
    // the camera block, frame_retraces + 1). The call's frame goes below the hooked function's.
    uint32_t scratch = (uint32_t)ctx->r29 - 0x400;
    uint32_t from = scratch + 0x40;
    put_vec(rdram, from, { pivot.x, pivot.y + get_f32(rdram, car_raise), pivot.z });
    MEM_W(0x10, (int32_t)scratch) = (int32_t)cam;
    MEM_W(0x14, (int32_t)scratch) = MEM_W(0, (int32_t)frame_retraces) + 1;
    uint32_t saved_poly = (uint32_t)MEM_W(cam_poly, (int32_t)cam);
    recomp_context call = *ctx;
    call.r29 = sx(scratch);
    call.r4 = sx(look_poly[view]);
    call.r5 = sx(cam + cam_pos);
    call.r6 = sx(cam + cam_ground);
    call.r7 = sx(from);
    collision_ground_query_basis_8008BDC0(rdram, &call);
    look_poly[view] = (uint32_t)call.r2;
    MEM_W(cam_poly, (int32_t)cam) = (int32_t)saved_poly;
}
