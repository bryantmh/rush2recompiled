// Race camera options: Chase Camera and LOOK BACK (docs/camera.md).
//
// Chase Camera (option, off by default). Each player's camera mode is car state +0x348. In a race the VIEW button
// cycles 0-3 (cam_update_all_players_8009D9F4 at 0x8009DD6C: bumper, hood, the chase camera 2 and the closer chase
// camera 3) and wraps at 4. The chase cameras (cam_update_chase_main_8009C228) sit at the view's chase height
// 0x800E7CC0[view] and distance 0x800E7D10[view] (mode 3 at 5 lower and 4 nearer), which cam_set_chase_mode_8009B480
// sets: 6 / 16 in a race. In the attract demo race_game_mode_800FAE6C is set and the race reset
// (race_reset_hud_and_globals_8009FD68) has cleared race_attract_mode_800E7BB0, so it sets 12 / 30 instead: the chase
// camera high above and far behind the car. With the option on, VIEW goes 0, 1, 2, 3, then that high chase camera
// (mode 2 at 12 / 30) and back to 0. The high chase camera is told apart from mode 2 by those two values, so it needs
// no state of its own: anything that sets the chase camera again (VIEW to 2 or 3, a new race) puts back the race's.
//
// LOOK BACK (a binding, D-pad left by default; src/controls.cpp). Every camera mode leaves its camera in the view's
// block 0x800E79D0 + view * 0x40 (3x3 matrix whose rows are the camera's axes in world space, position at +0x24),
// which the frame's view setup, visibility and sky read. While a player holds LOOK BACK in a driving camera (0-3) the
// block is turned 180 degrees about the car's up axis (row 1 of its matrix, car state +0x24) through the car's
// position: the chase cameras then sit ahead of the car looking back at it, the bumper and hood cameras at its rear
// facing backward. The camera update only notes it (hook at 0x8009DE58); the block is turned when its view is drawn
// (view_fog_setup_8007C624 entry, from src/interpolation.cpp) and put back after the frame's views and HUD (from
// src/hud.cpp), so no game code between frames sees a turned camera: the camera modes build on the block they left,
// and a camera left turned made the chase cameras jump between two places every frame. The chase cameras are turned
// from their own state (0x800E7B38 per view, moved to 0x80240390), which no drawing affects. Frame interpolation
// (src/interpolation.cpp) sees the turn as a camera cut and snaps.

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

#include "recomp.h"

#include "online.h"
#include "rush2.h"
#include "rush2_hooks.h"

extern "C" void cam_set_chase_mode_8009B480(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t race_game_mode = 0x800FAE6C; // Nonzero while the attract demo runs.

    // Per view (moved there for four players, src/players4.cpp): chase height (from 0x800E7CC0) and distance
    // (from 0x800E7D10), f32 each.
    constexpr uint32_t chase_height = 0x802404B0;
    constexpr uint32_t chase_distance = 0x802404C0;
    // The attract demo's values for them (cam_set_chase_mode_8009B480 with race_game_mode set and
    // race_attract_mode clear).
    constexpr float high_height = 12.0f;
    constexpr float high_distance = 30.0f;
    // The chase camera's state, 0x30 per view (from 0x800E7B38): the point it follows at +0x24.
    constexpr uint32_t chase_state = 0x80240390;
    constexpr uint32_t chase_state_size = 0x30;

    // Camera blocks, 0x40 per view: matrix rows +0x0, +0xC, +0x18; position +0x24; ground-checked position +0x34.
    constexpr uint32_t cameras = 0x800E79D0;
    constexpr uint32_t camera_size = 0x40;
    constexpr int32_t cam_pos = 0x24;
    constexpr int32_t cam_ground = 0x34;
    constexpr int max_views = 4;

    // Car state (0x354 per car).
    constexpr int32_t car_pos = 0x0;
    constexpr int32_t car_matrix = 0x24;
    constexpr int32_t car_mode = 0x348;

    constexpr int mode_chase = 2;
    constexpr int mode_close_chase = 3;

    std::atomic<bool> attract_camera = false;
    std::atomic<bool> look_back = true;

    // What LOOK BACK changes in a view's camera block: the axes (+0x0-0x23), the position (+0x24-0x2F) and the
    // ground-checked position (+0x34-0x3F). Signed: an unsigned MEM_W offset makes the address unsigned, so it isn't
    // sign-extended and lands 4 GB past RDRAM (issue 33: a crash wherever that isn't mapped, as with Vulkan).
    constexpr int32_t kept_words[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15 };
    // Per view: whether its player looks back this frame (set by the camera update, for the car state and camera mode
    // it updated), and while the frame is drawn, the block as the game left it.
    struct LookBack {
        bool wanted = false;
        uint32_t car = 0;
        int mode = 0;
        bool turned = false;
        std::array<uint32_t, std::size(kept_words)> game{};
    };
    std::array<LookBack, max_views> look_backs{};

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

    // Mode 2 at the attract demo's height and distance: the high chase camera.
    bool high_chase(uint8_t* rdram, int view) {
        return get_f32(rdram, chase_height + view * 4) == high_height &&
               get_f32(rdram, chase_distance + view * 4) == high_distance;
    }
}


void rush2::set_look_back(bool enabled) {
    look_back.store(enabled, std::memory_order_relaxed);
}

bool rush2::look_back_enabled() {
    return look_back.load(std::memory_order_relaxed);
}

void rush2::set_attract_camera(bool enabled) {
    attract_camera.store(enabled, std::memory_order_relaxed);
}

// cam_update_all_players_8009D9F4 at 0x8009DD6C, where a VIEW press adds 1 to the mode in $s0 (car state $s1, view
// $s3). From the high chase camera VIEW goes on to 0: 3 here becomes 4, which wraps. This runs with the option off
// too, so turning it off mid-race still leaves the high chase camera at the next press.
extern "C" void rush2_attract_camera_view(uint8_t* rdram, recomp_context* ctx) {
    int view = (int8_t)ctx->r19;
    if (MEM_BU(0, (int32_t)race_game_mode) != 0 || view < 0 || view >= max_views ||
        (int32_t)ctx->r16 != mode_chase || !high_chase(rdram, view)) {
        return;
    }
    ctx->r16 = mode_close_chase;
}

// cam_update_all_players_8009D9F4 at 0x8009DE30, where every path has the view's new camera mode in $s0 ($a0 = the
// old one, $s1 = the car state, $s3 = the view). Outside the attract demo, new 0 with old 3 is only VIEW wrapping
// from the close chase camera; with Chase Camera on it goes to the high chase camera instead, set up as VIEW sets
// up mode 2 (0x8009DDC0: cam_set_chase_mode_8009B480, the followed point back on the car) at the attract demo's
// height and distance.
extern "C" void rush2_attract_camera_cycle(uint8_t* rdram, recomp_context* ctx) {
    if (!attract_camera.load(std::memory_order_relaxed) || MEM_BU(0, (int32_t)race_game_mode) != 0 ||
        (int32_t)ctx->r16 != 0 || (int8_t)ctx->r4 != mode_close_chase) {
        return;
    }
    uint32_t car = (uint32_t)ctx->r17;
    int view = (int8_t)ctx->r19;
    if (view < 0 || view >= max_views) {
        return;
    }
    recomp_context call = *ctx;
    call.r4 = sx(car);
    cam_set_chase_mode_8009B480(rdram, &call);
    Vec pos = get_vec(rdram, car + car_pos);
    put_vec(rdram, chase_state + view * chase_state_size + 0x24, { pos.x, pos.y + 1.0f, pos.z });
    put_f32(rdram, chase_height + view * 4, high_height);
    put_f32(rdram, chase_distance + view * 4, high_distance);
    ctx->r16 = mode_chase;
}

// cam_update_all_players_8009D9F4 at 0x8009DE58, after cam_update_dispatch_8009D02C placed view $s3's camera for car
// state $s1 ($s4 = the view's player record, +1 its controller port; online, view i is this machine's port i). Only
// notes whether the view looks back: the camera is turned just for drawing the view (look_back_draw_view), so nothing
// the game runs between frames sees it turned. Some of the camera modes build on the block they left last frame, and a
// camera left turned there made the chase cameras alternate between two places every frame.
extern "C" void rush2_look_back(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r17;
    int view = (int8_t)ctx->r19;
    if (view < 0 || view >= max_views) {
        return;
    }
    LookBack& look = look_backs[view];
    look.wanted = false;
    if (!look_back.load(std::memory_order_relaxed) || MEM_BU(0, (int32_t)race_game_mode) != 0) {
        return;
    }
    int mode = (int8_t)MEM_B(car_mode, (int32_t)car);
    int port = rush2::online::game::in_race() ? view : MEM_BU(1, (int32_t)ctx->r20);
    if (mode < 0 || mode > mode_close_chase || !rush2::controls::look_back_held(port)) {
        return;
    }
    look.wanted = true;
    look.car = car;
    look.mode = mode;
}

// view_fog_setup_8007C624 entry, before it reads view $a0's camera block to draw it.
void rush2::look_back_draw_view(uint8_t* rdram, int view) {
    if (view < 0 || view >= max_views || !look_backs[view].wanted || look_backs[view].turned) {
        return;
    }
    LookBack& look = look_backs[view];
    uint32_t cam = cameras + view * camera_size;
    Vec pivot = get_vec(rdram, look.car + car_pos);
    Vec up = get_vec(rdram, look.car + car_matrix + 0xC);
    float len = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
    if (len < 1e-4f) {
        return;
    }
    up = { up.x / len, up.y / len, up.z / len };
    for (size_t i = 0; i < std::size(kept_words); i++) {
        look.game[i] = (uint32_t)MEM_W(kept_words[i] * 4, (int32_t)cam);
    }
    look.turned = true;
    // The chase cameras start from their own state (axes and position, which cam_update_dispatch_8009D02C copies
    // into the block at 0x8009D474), not from the block: while the car is off its path the block then goes through
    // collision_ground_query_basis_8008BDC0 (0x8009D4F4), and after a frame drawn turned that query put the camera
    // next to the car every other frame. The turned camera skips the query.
    bool chase = look.mode == mode_chase || look.mode == mode_close_chase;
    uint32_t from = chase ? chase_state + view * chase_state_size : cam;
    for (int row = 0; row < 3; row++) {
        put_vec(rdram, cam + row * 0xC, half_turn(get_vec(rdram, from + row * 0xC), up));
    }
    Vec p = get_vec(rdram, from + cam_pos);
    Vec d = half_turn({ p.x - pivot.x, p.y - pivot.y, p.z - pivot.z }, up);
    Vec turned = { pivot.x + d.x, pivot.y + d.y, pivot.z + d.z };
    put_vec(rdram, cam + cam_pos, turned);
    if (chase) {
        put_vec(rdram, cam + cam_ground, turned);
    }
}

// gfx_draw_frame_views_8007D9DC, after the frame's views and HUD are drawn: the turned cameras go back to the game's.
void rush2::look_back_draw_end(uint8_t* rdram) {
    for (int view = 0; view < max_views; view++) {
        LookBack& look = look_backs[view];
        if (!look.turned) {
            continue;
        }
        look.turned = false;
        uint32_t cam = cameras + view * camera_size;
        for (size_t i = 0; i < std::size(kept_words); i++) {
            MEM_W(kept_words[i] * 4, (int32_t)cam) = (int32_t)look.game[i];
        }
    }
}
