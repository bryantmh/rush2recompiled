// The Rush 2049 wings' contrails: smoke puffs streaming from each wing tip while the wings are out.
//
// Rush 2049 (N64) makes them in its wing callback (func_800924F4): every frame a car's wing node is out (node flag
// 0x10, which stays set while the wings slide back in) in a one-player race (0x8014A108 == 1), it calls its particle
// emitter func_8008EA10(car, 6 left / 7 right, 0, &d), d = (side * the car's slide distance 0x8011B4B4 [type], its
// wing height, 0), with the slide scaled by the step while the wings slide. Rush 2's particle emitter
// (particle_emitter_spawn_8008270C) only has emitters 0-5, so this file ports emitters 6 and 7 and Rush 2049's
// particle update (func_800CADA4) for them, and draws the puffs through src/rush2049/battle_render.cpp.
//
// func_8008EA10, emitters 6 and 7 (car state offsets are Rush 2049's; Rush 2's are 8 less, speed +0xF8 is Rush 2's +0xE0):
// - Only with fewer than 3 players. Puffs this frame: by speed s = |state +0xF8 >> 2|: 4 at s >= 115, 3 at >= 100,
//   2 at >= 85, else 1; then 1 if the side has no tip position from the last frame (0x801392D8 [car] bit 0x4000 left /
//   0x8000 right), else + 5, and + 3 more at s >= 150.
// - Each puff: life (+0x3C) 0.08 + rand * 0.07 s, frame timer (+0x40) 1/30 s, velocity half the car's, scale (node
//   +0xC) 0.075 + rand * 0.1, color (node +0x3C) by the car's wing style (the W table 0x8011128C [car + 1][type]):
//   0xB4FFB4A0, 0xB9B4FFA0, 0xFFC0BBA0. Model: with the Expansion Pak (0x80156994) a random one of SMOKEG1-8
//   (handles 0x80142948, file 61), without it on tracks 0-5 SMOKERSMOKE (0x80142946, file 62). Every other puff of a
//   side (bits 0x1000 / 0x2000 toggled) steps its frames forward, the others backward (+0x50 bit 0).
// - Place: car position + up * (d.y + 0.5) + forward * (d.z - 1.15) + right * (d.x -+ 2.6). The first puff of a frame
//   is at the tip, and the tip is kept as the side's last position; the others go back along the line to last frame's
//   tip, puff i at i / n of its length, so the trail has no gaps at speed.
// - Hidden in the car's own view in its close cameras (state +0x35C view, +0x35D camera 0 or 1).
// func_800CADA4, each frame for each puff: life -= dt, freed at 0; alpha (node +0x3F) - 9 while life < 1 (always,
// here) and alpha >= 9; the frame timer counts down by dt and at 0 (back to 1/30 s) shows the current SMOKEG frame and
// steps it; scale + 0.03; position += velocity * dt. The pool (0x8013F1E0) holds 100 particles shared with the wheel
// smoke; here the contrails have 100 of their own.
//
// Mod cars whose wings aren't drawn ("no_wings") leave no contrails either. Puffs are visual only (no online sync).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "battle_render.h"
#include "car2049.h"
#include "recomp.h"
#include "rush2_hooks.h"
#include "wings_internal.h"

namespace {
    constexpr uint32_t cars = 0x800F5470, car_size = 0x81C;
    constexpr uint32_t car_type = 0x7EA;                  // u8
    constexpr uint32_t car_states = 0x801124A0, car_state_size = 0x354;
    constexpr uint32_t state_velocity = 0xC;              // f32[3], ft/s
    constexpr uint32_t state_axes = 0x24;                 // f32[9]: rows right, up, forward
    constexpr uint32_t state_speed = 0xE0;                // s16, >> 2 for the particles' speed (Rush 2049 +0xF8)
    constexpr uint32_t state_view = 0x347;                // s8: the view the car is seen from, -1 none
    constexpr uint32_t state_camera = 0x348;              // s8: that view's camera, 0 and 1 the close ones
    constexpr uint32_t num_players = 0x8010C3E2;          // s16
    constexpr uint32_t physics_clock = 0x8010C0E4;        // f32

    constexpr int first_slot = 224;                       // battle_render slots 224-323
    constexpr int pool_size = 100;                        // Rush 2049's particle pool (0x8013F1E0) holds 100
    constexpr float frame_time = 1.0f / 30.0f;            // 0x80123960 / 0x80123FE0 (0.0333333)
    constexpr float tip_back = -1.15f;                    // 0x80123994
    constexpr float tip_out = 2.6f;                       // 0x8012399C (left: 0x80123998, -2.6)
    constexpr float tip_up = 0.5f;
    constexpr float life_base = 0.08f, life_rand = 0.07f; // 0x8012396C, 0x80123968
    constexpr float scale_base = 0.075f, scale_rand = 0.1f;   // 0x801239A4, 0x801239A0
    constexpr float scale_step = 0.03f;                   // 0x80123FC0, per frame
    constexpr int alpha_step = 9;
    constexpr uint32_t style_colors[3] = { 0xB4FFB4A0, 0xB9B4FFA0, 0xFFC0BBA0 };   // 0x801174A0-0x801174A8
    constexpr int smoke_frames = 8;                       // SMOKEG1-8

    struct Puff {
        bool on = false;
        float pos[3] = {}, vel[3] = {};
        float life = 0.0f, timer = 0.0f, scale = 0.0f;
        uint32_t rgba = 0;
        int frame = 0, shown = 0;
        bool forward = false;
        int view = -1;
        bool fresh = false;       // emitted this tick: drawn at the wing tip, tiny (show)
        float tip[3] = {};
    };
    std::array<Puff, pool_size> puffs;

    struct Side {
        bool has_tip = false;     // 0x801392D8 bit 0x4000 / 0x8000
        bool toggle = false;      // bit 0x1000 / 0x2000
        float tip[3] = {};        // 0x8013C098 + car * 0x18 (+0 left, +0xC right)
    };
    std::array<std::array<Side, 2>, rush2::wings::max_cars> sides;

    uint32_t rng_state = 0;       // Rush 2049's LCG (0x8011735C), kept apart from Rush 2's
    float last_clock = 0.0f;

    float rand01() {
        rng_state = rng_state * 0x41C64E6D + 0x3039;
        return float((rng_state >> 16) & 0x7FFF) / 32768.0f;
    }

    float read_f(uint8_t* rdram, uint32_t addr) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    void model_name(int frame, char out[16]) {
        snprintf(out, 16, "SMOKEG%d", frame + 1);
    }

    void show(int i) {
        const Puff& p = puffs[i];
        char name[16];
        model_name(p.shown, name);
        // Frame interpolation draws the car between the last tick and this one, and a puff new this tick has no last
        // position, so drawn where it belongs it would sit ahead of the drawn wing (it fills the stretch the wing
        // covers up to this tick). On its first tick it is drawn tiny at the wing tip instead, and from there it
        // streams out to its place over the next tick. The SMOKEG frames are one puff changing (same_object).
        float scale = p.fresh ? 0.001f : p.scale;
        float m[9] = { scale, 0, 0, 0, scale, 0, 0, 0, scale };
        rush2::battle_render::place(first_slot + i, name, m, p.fresh ? p.tip : p.pos, p.rgba, -1, false, true, 0.0f, false, true);
        rush2::battle_render::hide_in_view(first_slot + i, p.view);
    }

    void clear_all() {
        for (int i = 0; i < pool_size; i++) {
            if (puffs[i].on) rush2::battle_render::hide(first_slot + i);
            puffs[i].on = false;
        }
        for (auto& car : sides) car = {};
    }

    // func_800CADA4 for emitters 6 and 7.
    void update(float dt) {
        for (int i = 0; i < pool_size; i++) {
            Puff& p = puffs[i];
            if (!p.on) continue;
            p.fresh = false;
            p.life -= dt;
            if (p.life <= 0.0f) {
                p.on = false;
                rush2::battle_render::hide(first_slot + i);
                continue;
            }
            if (p.life < 1.0f && (p.rgba & 0xFF) >= alpha_step) p.rgba -= alpha_step;
            p.timer -= dt;
            if (p.timer <= 0.0f) {
                p.shown = p.frame;
                p.frame = p.forward ? (p.frame + 1) % smoke_frames : (p.frame + smoke_frames - 1) % smoke_frames;
                p.timer = frame_time;
            }
            p.scale += scale_step;
            for (int k = 0; k < 3; k++) p.pos[k] += p.vel[k] * dt;
            show(i);
        }
    }

    // func_8008EA10(car, 6 + side, 0, d) for emitters 6 and 7.
    void emit(uint8_t* rdram, int car, int side, const float d[3], uint32_t rgba) {
        uint32_t state = car_states + uint32_t(car) * car_state_size;
        int speed = std::abs((int)(int16_t)((int16_t)MEM_H(0, (int32_t)(state + state_speed)) >> 2));
        int count = speed >= 115 ? 4 : speed >= 100 ? 3 : speed >= 85 ? 2 : 1;
        Side& s = sides[car][side];
        count = s.has_tip ? count + 5 : 1;
        if (speed >= 150 && count != 1) count += 3;

        float pos[3], vel[3], ax[9];
        for (int k = 0; k < 3; k++) {
            pos[k] = read_f(rdram, state + uint32_t(k) * 4);
            vel[k] = read_f(rdram, state + state_velocity + uint32_t(k) * 4) * 0.5f;
        }
        for (int k = 0; k < 9; k++) ax[k] = read_f(rdram, state + state_axes + uint32_t(k) * 4);
        const float* right = ax;
        const float* up = ax + 3;
        const float* fwd = ax + 6;
        float out = side ? tip_out : -tip_out;
        float tip[3];
        for (int k = 0; k < 3; k++) {
            tip[k] = pos[k] + up[k] * (d[1] + tip_up) + fwd[k] * (d[2] + tip_back) + right[k] * (d[0] + out);
        }
        int8_t view = MEM_B(0, (int32_t)(state + state_view));
        int8_t camera = MEM_B(0, (int32_t)(state + state_camera));
        int hidden_view = (view >= 0 && (camera == 0 || camera == 1)) ? view : -1;

        float back[3] = {};
        for (int n = 0; n < count; n++) {
            int slot = -1;
            for (int i = 0; i < pool_size; i++) {
                if (!puffs[i].on) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) return;   // the pool is full (func_8008E3C0 returns null)
            Puff& p = puffs[slot];
            p = {};
            p.on = true;
            p.timer = frame_time;
            p.frame = p.shown = int(rand01() * smoke_frames) % smoke_frames;
            p.life = rand01() * life_rand + life_base;
            for (int k = 0; k < 3; k++) p.vel[k] = vel[k];
            s.toggle = !s.toggle;
            p.forward = s.toggle;
            memcpy(p.pos, tip, sizeof(tip));
            memcpy(p.tip, tip, sizeof(tip));
            p.fresh = true;
            if (n == 0) {
                float last[3];
                memcpy(last, s.tip, sizeof(last));
                bool had = s.has_tip;
                s.has_tip = true;
                memcpy(s.tip, tip, sizeof(tip));
                if (had) {
                    for (int k = 0; k < 3; k++) back[k] = last[k] - tip[k];
                }
            }
            else {
                float len = std::sqrt(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
                if (len != 0.0f) {
                    float f = float(n) * (len / float(count)) / len;
                    for (int k = 0; k < 3; k++) p.pos[k] += back[k] * f;
                }
            }
            p.scale = rand01() * scale_rand + scale_base;
            p.rgba = rgba;
            p.view = hidden_view;
            show(slot);
        }
    }
}

// physics_tick_80076578 at 0x8007660C, the end of a physics tick: $sp + 0x18 = the tick's dt.
extern "C" void rush2_wings_contrails_tick(uint8_t* rdram, recomp_context* ctx) {
    float now = read_f(rdram, physics_clock);
    bool new_race = now < last_clock;
    last_clock = now;
    if (new_race) clear_all();
    if (!rush2::wings::enabled() || !rush2::battle_render::ready(rdram) || !rush2::battle_render::has_model("SMOKEG1")) {
        clear_all();
        return;
    }
    float dt = read_f(rdram, (uint32_t)((int32_t)ctx->r29 + 0x18));
    update(dt);

    bool one_player = (int16_t)MEM_H(0, (int32_t)num_players) == 1;
    for (int car = 0; car < rush2::wings::max_cars; car++) {
        rush2::wings::Pose pose;
        uint8_t type = MEM_BU(0, (int32_t)(cars + uint32_t(car) * car_size + car_type));
        if (!rush2::wings::get_pose(rdram, car, pose) || rush2::car2049::mod_car_wings_hidden(type)) {
            for (Side& s : sides[car]) s.has_tip = false;   // func_80092484: the wings are away, so the tips start over
            continue;
        }
        if (!one_player) continue;
        uint32_t rgba = style_colors[std::clamp(pose.style, 0, 2)];
        for (int side = 0; side < 2; side++) {
            float d[3] = { (side ? 1.0f : -1.0f) * pose.emit_slide, pose.height, 0.0f };
            emit(rdram, car, side, d, rgba);
        }
    }
}
