// The cars' light glows on a custom track with [look] car_lamps (include/track_mod.h; the PS1 SF Rush's night,
// docs/sfrush_ps1.md#snow-and-night). After each car the PS1's track_draw_chunks (0x80042FF0) draws, with
// light_sprite_draw (0x80041C58), a 3.2 ft additive flare card in the car's own frame at each side's tail light
// (0x800A8FA8, day and night: its brightness the brake glow, car +0x90, +32 a frame while braking up to 128, -8 a frame
// down to 0, and 63 more at night) and, at night, each side's headlight (0x800A8EE8, brightness 0xC0), facing ahead and
// behind; a corner's light goes out when its damage reaches 3. Here every car on such a track gets the same:
//   - where: the car mod's "lamps" (the PS1 cars carry the PS1's own points, src/sfrush_ps1/ps1_cars.cpp), otherwise
//     measured from the body the car draws (rush2::battle::car_body_triangles): 70% of its half width out, 40% up its
//     height, on the body's nose and tail there
//   - drawn by rush2::battle_render as a model of the port's own: a 32 x 32 flare (IA16, made here), additive, on a
//     card facing out of the car (seen from ahead for a headlight, from behind for a tail light), 1.3 ft each way
//     (the PS1's 1.6 at the PS1 cars' size in Rush 2, 0.818)
//   - brightness: headlights 0xC0 / 128, tail lights (the brake glow + 63) / 128, the brake glow kept per car at the
//     PS1's rates at 25 frames a second (the car conversion's, ps1_track.cpp frames_per_second), both capped at full
//   - out: a car wrecked, or a corner's panel damaged to Rush 2's level 2 (car state +0xE4; the PS1's 3 of 7) [I]
//
// Spare RDRAM (the list in src/controls_menu.cpp): 0x80E1F200-0x80E1FB40.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "recomp.h"

#include "battle.h"
#include "battle_render.h"
#include "car2049.h"
#include "ghost.h"
#include "rush2_hooks.h"
#include "track2049.h"

namespace {
    constexpr int first_slot = 186;               // to 217 (battle_render.h): car c's lamp j at first_slot + 4c + j
    constexpr int max_cars = 8;
    constexpr const char* model_name = "CAR_LAMP";

    constexpr uint32_t texels_at = 0x80E1F200;    // 32 x 32 IA16, 2048 bytes
    constexpr uint32_t vertices_at = 0x80E1FA00;  // 4 x 16 bytes
    constexpr uint32_t dl_at = 0x80E1FA40;        // to 0x80E1FB40
    constexpr int texture_size = 32;

    constexpr uint32_t game_state = 0x8010C0D0;   // s32: 3 and up a race
    constexpr uint32_t cars = 0x800F5470;         // physics cars
    constexpr uint32_t car_size = 0x81C;
    constexpr uint32_t car_brake = 0x730;         // f32 0-1, the brake pedal
    constexpr uint32_t car_wrecked = 0x648;       // s8
    constexpr uint32_t car_active = 0x7E4;        // s16, nonzero for a car in the race
    constexpr uint32_t car_type = 0x7EA;          // u8
    constexpr uint32_t car_corners = 0xE8;        // f32[4][3]: the collision box's corners, car-local (0 and 1 the front)
    constexpr uint32_t car_states = 0x801124A0;
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t state_damage = 0xE4;       // u32: each panel's 2-bit damage level
    constexpr uint32_t panel_masks = 0x800CE188;  // u32 per panel (0 front right, 1 front left, 2 rear right, 3 rear left)
    constexpr uint32_t panel_shifts = 0x800CE180; // u8 per panel

    constexpr float card_half = 1.3f;             // ft
    constexpr float lift_off = 0.15f;             // ft out from the body's surface
    constexpr float ps1_fps = 239.7f / 9.6f;      // the PS1 car conversion's frames a second
    constexpr float head_level = 192.0f / 128.0f;
    constexpr float night_tail = 63.0f;
    // The PS1's sprites: ART/LIGHT2.GFX's yellow ramp for the headlights, ART/LIGHT.GFX's red for the tail lights [I].
    constexpr float head_rgb[3] = { 255.0f, 230.0f, 120.0f };
    constexpr float tail_rgb[3] = { 248.0f, 16.0f, 8.0f };

    struct Lamps {
        uint64_t key = 0;
        bool head = false, tail = false;
        float head_at[3] = {}, tail_at[3] = {};   // the right side's, body frame ft
    };
    Lamps lamps[max_cars];
    float brake_glow[max_cars] = {};
    bool shown = false;
    bool model_ready = false;
    std::chrono::steady_clock::time_point last_tick{};

    float read_f(uint8_t* rdram, uint32_t at) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    // The flare: a soft core and halo with four thin spikes, as the PS1's star sprites, clear at the edges.
    void write_model(uint8_t* rdram) {
        std::vector<uint8_t> texels(size_t(texture_size) * texture_size * 2);
        for (int y = 0; y < texture_size; y++) {
            for (int x = 0; x < texture_size; x++) {
                float dx = (x + 0.5f) / (texture_size / 2.0f) - 1.0f, dy = (y + 0.5f) / (texture_size / 2.0f) - 1.0f;
                float d = std::sqrt(dx * dx + dy * dy);
                float core = std::exp(-(d / 0.2f) * (d / 0.2f)), halo = 0.35f * std::exp(-(d / 0.5f) * (d / 0.5f));
                float fade = std::max(0.0f, 1.0f - d);
                float spikes = (std::exp(-(dx / 0.05f) * (dx / 0.05f)) + std::exp(-(dy / 0.05f) * (dy / 0.05f))) * 0.6f * fade * fade;
                float a = std::clamp(core + halo + spikes, 0.0f, 1.0f) * std::clamp((1.0f - d) * 4.0f, 0.0f, 1.0f);
                size_t i = (size_t(y) * texture_size + x) * 2;
                texels[i] = 255;
                texels[i + 1] = uint8_t(std::lround(a * 255.0f));
            }
        }
        for (size_t i = 0; i + 3 < texels.size(); i += 4) {
            MEM_W(0, (int32_t)(texels_at + i)) =
                (int32_t)((uint32_t(texels[i]) << 24) | (uint32_t(texels[i + 1]) << 16) | (uint32_t(texels[i + 2]) << 8) | texels[i + 3]);
        }
        // Vertices (1/16 ft): s16 x, y, z, u16 0, s16 s, t (10.5), u8 r, g, b, a. One face, seen from +z (wound
        // (0 2 1) (0 3 2): drawn the other way, as the arcade checkpoint banner's front face is, a headlight showed from
        // behind the car and the tail lights not at all).
        const int16_t h = 16, s1 = texture_size * 32;
        const int16_t v[4][5] = { { int16_t(-h), int16_t(-h), 0, 0, s1 }, { h, int16_t(-h), 0, s1, s1 }, { h, h, 0, s1, 0 }, { int16_t(-h), h, 0, 0, 0 } };
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t at = vertices_at + i * 16;
            MEM_W(0, (int32_t)at) = (int32_t)((uint32_t(uint16_t(v[i][0])) << 16) | uint16_t(v[i][1]));
            MEM_W(0, (int32_t)(at + 4)) = (int32_t)(uint32_t(uint16_t(v[i][2])) << 16);
            MEM_W(0, (int32_t)(at + 8)) = (int32_t)((uint32_t(uint16_t(v[i][3])) << 16) | uint16_t(v[i][4]));
            MEM_W(0, (int32_t)(at + 12)) = (int32_t)0xFFFFFFFF;
        }
        // Additive, as the PS1's semi-transparent sprites: G_RM_PASS, then the pixel x its alpha added to memory
        // (GBL_c2(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1)), z compared, not written. No fog: the fog in place when
        // rush2::battle_render draws isn't the scenery's (src/arcade2049/arcade_checkpoints.cpp).
        const uint32_t dl[][2] = {
            { 0xE7000000, 0x00000000 },        // G_RDPPIPESYNC
            { 0xE3001001, 0x00000000 },        // TEXTLUT = NONE
            { 0xE3000F00, 0x00000000 },        // TEXTLOD = TILE
            { 0xE200001C, 0x0C1A4A50 },        // render mode: pass, then additive z-compared
            { 0xFC1197FF, 0xFFFFFE38 },        // combiner: texel 0 x PRIM (color and alpha), then pass
            { 0xFD700000, texels_at },         // G_SETTIMG IA16
            { 0xF5700000, 0x07000000 },        // load tile 7
            { 0xE6000000, 0x00000000 },        // G_RDPLOADSYNC
            { 0xF3000000, 0x073FF100 },        // G_LOADBLOCK 1024 texels, dxt 256 (a 32-texel row is 8 words)
            { 0xE7000000, 0x00000000 },
            { 0xF5701000, 0x00080200 },        // tile 0: IA16, 8 words a row, clamped in s and t
            { 0xF2000000, 0x0007C07C },        // tile size 32 x 32
            { 0x01004008, vertices_at },       // G_VTX 4 from 0
            { 0x06000402, 0x00000604 },        // G_TRI2 (0 2 1) (0 3 2): seen from +z
            { 0xDF000000, 0x00000000 },        // G_ENDDL
        };
        for (uint32_t i = 0; i < sizeof(dl) / sizeof(dl[0]); i++) {
            MEM_W(0, (int32_t)(dl_at + i * 8)) = (int32_t)dl[i][0];
            MEM_W(0, (int32_t)(dl_at + i * 8 + 4)) = (int32_t)dl[i][1];
        }
        rush2::battle_render::add_model(model_name, dl_at);
    }

    // The highest (front) or lowest (back) z of the triangles where the line along z through (x, y) crosses them.
    bool body_end(const std::vector<std::array<float, 9>>& tris, float x, float y, bool front, float& z) {
        bool hit = false;
        for (const auto& t : tris) {
            float d = (t[4] - t[7]) * (t[0] - t[6]) + (t[6] - t[3]) * (t[1] - t[7]);
            if (std::fabs(d) < 1e-6f) continue;
            float a = ((t[4] - t[7]) * (x - t[6]) + (t[6] - t[3]) * (y - t[7])) / d;
            float b = ((t[7] - t[1]) * (x - t[6]) + (t[0] - t[6]) * (y - t[7])) / d;
            float c = 1.0f - a - b;
            if (a < -1e-4f || b < -1e-4f || c < -1e-4f) continue;
            float zz = a * t[2] + b * t[5] + c * t[8];
            if (!hit || (front ? zz > z : zz < z)) z = zz;
            hit = true;
        }
        return hit;
    }

    // Car i's lamp points: its mod's "lamps", else measured from its body (cached until its type or parts change).
    void update_lamps(uint8_t* rdram, int i) {
        Lamps& l = lamps[i];
        int type = MEM_BU(0, (int32_t)(cars + uint32_t(i) * car_size + car_type));
        uint64_t key = rush2::battle::car_body_triangles(rdram, i, nullptr);
        if (l.key == key) return;
        l = Lamps{};
        l.key = key;
        rush2::car2049::ModCar spec;
        if (rush2::car2049::mod_car_lamps(type, spec)) {
            l.head = spec.head_lamp;
            l.tail = spec.tail_lamp;
            memcpy(l.head_at, spec.head, sizeof(l.head_at));
            memcpy(l.tail_at, spec.tail, sizeof(l.tail_at));
            l.head_at[2] += lift_off;
            l.tail_at[2] -= lift_off;
            return;
        }
        std::vector<std::array<float, 9>> tris;
        rush2::battle::car_body_triangles(rdram, i, &tris);
        if (tris.empty()) {
            // No body parts to measure: the collision box's front and back (car +0xE8, 4 x f32[3], 0 and 1 the front),
            // 2 ft up.
            uint32_t c = cars + uint32_t(i) * car_size + car_corners;
            float half = std::fabs(read_f(rdram, c)), front = read_f(rdram, c + 8), back = read_f(rdram, c + 0x2C);
            if (!(half > 0.5f) || !(front > back)) {
                l.key = 0;   // nothing to place them by yet (a car still being set up): try again next view
                return;
            }
            l.head = l.tail = true;
            l.head_at[0] = l.tail_at[0] = half * 0.7f;
            l.head_at[1] = l.tail_at[1] = 2.0f;
            l.head_at[2] = front + lift_off;
            l.tail_at[2] = back - lift_off;
            return;
        }
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        for (const auto& t : tris) {
            for (int k = 0; k < 9; k++) lo[k % 3] = std::min(lo[k % 3], t[k]), hi[k % 3] = std::max(hi[k % 3], t[k]);
        }
        float half = std::max(std::fabs(lo[0]), std::fabs(hi[0]));
        float x = half * 0.7f, y = lo[1] + (hi[1] - lo[1]) * 0.4f;
        float zf = hi[2], zb = lo[2];
        body_end(tris, x, y, true, zf);
        body_end(tris, x, y, false, zb);
        l.head = l.tail = true;
        l.head_at[0] = l.tail_at[0] = x;
        l.head_at[1] = l.tail_at[1] = y;
        l.head_at[2] = zf + lift_off;
        l.tail_at[2] = zb - lift_off;
    }

    void hide_all() {
        for (int s = 0; s < max_cars * 4; s++) rush2::battle_render::hide(first_slot + s);
        shown = false;
    }

    int panel_level(uint8_t* rdram, int car, int panel) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)(car_states + uint32_t(car) * car_state_size + state_damage));
        uint32_t mask = (uint32_t)MEM_W(0, (int32_t)(panel_masks + panel * 4));
        return int((bits & mask) >> MEM_BU(0, (int32_t)(panel_shifts + panel)));
    }
}

// pvs_apply_8007C27C entry, before each view is drawn (the cars' bodies have the frame's poses).
extern "C" void rush2_car_lamps_view(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)MEM_W(0, (int32_t)game_state) < 3 || !rush2::track2049::mod_car_lamps(rdram)) {
        if (shown) hide_all();
        for (Lamps& l : lamps) l = Lamps{};
        for (float& b : brake_glow) b = 0.0f;
        model_ready = false;
        return;
    }
    if (!model_ready) {
        write_model(rdram);
        model_ready = true;
        last_tick = std::chrono::steady_clock::now();
    }
    auto now = std::chrono::steady_clock::now();
    float frames = std::min(std::chrono::duration<float>(now - last_tick).count(), 0.25f) * ps1_fps;
    last_tick = now;
    shown = true;
    for (int i = 0; i < max_cars; i++) {
        uint32_t car = cars + uint32_t(i) * car_size;
        int slot = first_slot + i * 4;
        if (MEM_H(0, (int32_t)(car + car_active)) == 0 || MEM_B(0, (int32_t)(car + car_wrecked)) != 0 || rush2::ghost::is_ghost_car(i)) {
            for (int j = 0; j < 4; j++) rush2::battle_render::hide(slot + j);
            brake_glow[i] = 0.0f;
            continue;
        }
        // The brake glow: +32 a PS1 frame while braking, to 128, -8 a frame to 0.
        bool braking = read_f(rdram, car + car_brake) > 0.1f;
        brake_glow[i] = std::clamp(brake_glow[i] + (braking ? 32.0f : -8.0f) * frames, 0.0f, 128.0f);
        update_lamps(rdram, i);
        const Lamps& l = lamps[i];
        float pos[3], m[9];
        rush2::battle::car_body_pose(rdram, i, pos, m);
        auto rgba = [](const float rgb[3], float level) {
            uint32_t c = 0xFF;
            for (int k = 0; k < 3; k++) c |= uint32_t(std::clamp(std::lround(rgb[k] * level), 0l, 255l)) << (24 - k * 8);
            return c;
        };
        float tail_level = (brake_glow[i] + night_tail) / 128.0f;
        // Lamps 0 right headlight, 1 left, 2 right tail light, 3 left; panels 0 front right, 1 front left, 2 rear right,
        // 3 rear left.
        for (int j = 0; j < 4; j++) {
            bool head = j < 2, right = (j % 2) == 0;
            if (!(head ? l.head : l.tail) || panel_level(rdram, i, j) >= 2) {
                rush2::battle_render::hide(slot + j);
                continue;
            }
            const float* at = head ? l.head_at : l.tail_at;
            float local[3] = { right ? at[0] : -at[0], at[1], at[2] };
            float world[3];
            for (int k = 0; k < 3; k++) world[k] = pos[k] + m[k] * local[0] + m[3 + k] * local[1] + m[6 + k] * local[2];
            // The card faces out of the car: ahead for a headlight, behind for a tail light (right and forward turned
            // round, so it is still seen from its +z).
            float s = head ? card_half : -card_half;
            float card[9] = { m[0] * s, m[1] * s, m[2] * s, m[3] * card_half, m[4] * card_half, m[5] * card_half, m[6] * s, m[7] * s, m[8] * s };
            rush2::battle_render::place(slot + j, model_name, card, world, head ? rgba(head_rgb, head_level) : rgba(tail_rgb, tail_level));
        }
    }
}
