// Falling snow (docs/custom_tracks.md#snowfall): on a custom track whose track.toml asks for it ([look] snowfall: the
// SF Rush PlayStation mod's SNOW versions; rush2::track2049::mod_snowfall), flakes fall round every player's camera,
// none at FOG 1 (no fog) and thicker and faster with each step after (FOG 2-4, 0x800D1FA8). Not in any of the original games: the PlayStation
// SF Rush's snow has no falling snow (docs/sfrush_ps1.md#snow-and-night).
//
// The flakes: per view, a field of flakes in a box round the camera (`half` ft each side across, `below` / `above`
// it up and down). Each has a world position; every game frame (the view's pvs_apply_8007C27C, before it is drawn)
// they fall and drift by the game clock's step (0x80117488, so they hold still while it does), and a flake that
// leaves the box comes back in on the opposite side (a box in world space, wrapped round the camera, so the field
// moves past the camera as the car does and never runs out). Each is a small diamond facing the camera (the view's
// axes, rush2::views::axes; none within 6 ft of it), white with a little blue, opaque and fogged like the track, so far flakes fade into the
// fog.
//
// Drawing: one rush2::battle_render slot a view (194-197), placed at a fixed origin for the race with the view's
// camera far from it moved only past 1500 ft (vertices are s16 in 1/16 ft), so battle_render's frame interpolation
// carries it with nothing to smooth. The model is a trampoline below 16 MB (0x80E1F300, 32 bytes a view: RT64's
// extended RDRAM on, a call to the list, off) into a list above 16 MB (recomp::alloc), written each game frame into the
// next of three buffers so one the renderer may still read is never rewritten.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"

#include "arrows.h"
#include "battle_render.h"
#include "snowfall.h"
#include "track2049.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

namespace {
    constexpr uint32_t game_state = 0x8010C0D0;   // 3, 6, 9, 10: a race (src/sky_fog.cpp in_race)
    constexpr uint32_t game_clock = 0x80117488;   // f32 s (timer_game_time)
    constexpr uint32_t fog_option = 0x800D1FA8;   // FOG, 1-4
    constexpr uint32_t trampolines = 0x80E1F300;  // to 0x80E1F380 (src/controls_menu.cpp lists the ranges)
    constexpr int first_slot = 194;               // to 197 (include/battle_render.h)
    constexpr int max_views = 4;
    constexpr int buffers = 3;

    // The field round the camera (ft) and the flakes for each FOG step (at snowfall 1).
    // (A larger field at fewer flakes a cubic foot read as a few stray squares: 2026-11-12's first in-game look.)
    constexpr float half = 40.0f, below = 15.0f, above = 30.0f;
    // FOG 1 has no fog, so no snow; 2 a light fall drifting down, 3 a steady one, 4 a blizzard driven sideways (user,
    // 2026-11-12: the steps looked alike, the flakes too big and the heavy falls too lazy).
    constexpr int flakes_for_fog[5] = { 0, 0, 1100, 3000, 6000 };
    constexpr float near_cut = 6.0f;              // ft: a flake nearer the camera isn't drawn (it would fill the screen)
    constexpr float fall_for_fog[5] = { 0.0f, 0.0f, 3.5f, 9.0f, 20.0f };   // ft/s
    constexpr float drift_for_fog[5] = { 0.0f, 0.0f, 0.5f, 2.5f, 11.0f };  // ft/s sideways, x then z
    constexpr float sway_for_fog[5] = { 0.0f, 0.0f, 0.6f, 0.4f, 0.2f };    // ft/s, a light flake's flutter
    constexpr int max_flakes = 9000;
    constexpr int per_load = 15;                  // quads a G_VTX (60 vertices)
    constexpr uint32_t list_bytes = 64 + (max_flakes / per_load + 1) * (8 + per_load * 8) + 64;
    constexpr uint32_t buffer_bytes = ((list_bytes + max_flakes * 4 * 16) + 15) & ~15u;

    struct Flake {
        float p[3];
        float size;      // ft, half the square's side
        float phase;     // sway
        uint8_t shade;
    };

    struct ViewSnow {
        std::vector<Flake> flakes;
        float origin[3] = {};
        bool placed = false, have_origin = false;
        float last_clock = -1.0f;
        int buffer = 0;
        uint32_t seed = 0;
    };

    std::atomic<bool> on_flag = true;            // Graphics > Falling Snow
    std::array<ViewSnow, max_views> views;
    uint32_t block = 0;          // 3 buffers a view, above 16 MB
    bool models_added = false;

    float read_f(uint8_t* rdram, uint32_t at) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    void write_cmd(uint8_t* rdram, uint32_t& at, uint32_t w0, uint32_t w1) {
        MEM_W(0, (int32_t)at) = (int32_t)w0;
        MEM_W(4, (int32_t)at) = (int32_t)w1;
        at += 8;
    }

    float rnd(uint32_t& s) {
        s = s * 1664525u + 1013904223u;
        return float(s >> 8) / float(1 << 24);
    }

    std::string model_name(int v) { return "SNOWFALL" + std::to_string(v); }

    void hide(int v) {
        if (!views[size_t(v)].placed) return;
        rush2::battle_render::hide(first_slot + v);
        views[size_t(v)].placed = false;
    }

    void reset(int v) {
        hide(v);
        views[size_t(v)].flakes.clear();
        views[size_t(v)].have_origin = false;
        views[size_t(v)].last_clock = -1.0f;
    }

    // Brings a coordinate back into [-h, h) about c.
    float wrap(float x, float c, float lo, float hi) {
        float span = hi - lo, r = x - c;
        if (r < lo || r >= hi) r = lo + std::fmod(std::fmod(r - lo, span) + span, span);
        return c + r;
    }

    void update(uint8_t* rdram, int v, float amount) {
        ViewSnow& vs = views[size_t(v)];
        const int fog = std::clamp((int)MEM_BU(0, (int32_t)fog_option), 1, 4);
        const int want = std::min(max_flakes, int(std::lround(flakes_for_fog[fog] * amount)));
        if (want == 0) {
            reset(v);
            return;
        }
        float cam[3], axes[9];
        rush2::views::eye(rdram, v, cam);
        rush2::views::axes(rdram, v, axes);
        if (!std::isfinite(cam[0]) || !std::isfinite(cam[1]) || !std::isfinite(cam[2])) return;

        // The field: made (or grown, shrunk) round the camera.
        if ((int)vs.flakes.size() != want) {
            if (vs.seed == 0) vs.seed = 0x5A0Bu + uint32_t(v) * 7919u;
            while ((int)vs.flakes.size() < want) {
                Flake f;
                f.p[0] = cam[0] + (rnd(vs.seed) * 2 - 1) * half;
                f.p[1] = cam[1] - below + rnd(vs.seed) * (below + above);
                f.p[2] = cam[2] + (rnd(vs.seed) * 2 - 1) * half;
                f.size = 0.05f + 0.05f * rnd(vs.seed);   // ft, half the diamond's width: 1-2 inches across
                f.phase = rnd(vs.seed) * 6.2831853f;
                f.shade = uint8_t(225 + int(rnd(vs.seed) * 30));
                vs.flakes.push_back(f);
            }
            vs.flakes.resize(size_t(want));
        }
        // The fall since the last game frame (none while the game clock stands still: paused).
        const float clock = read_f(rdram, game_clock);
        float dt = vs.last_clock < 0.0f ? 0.0f : clock - vs.last_clock;
        if (!(dt >= 0.0f && dt <= 0.25f)) dt = 0.0f;
        vs.last_clock = clock;
        const float fall = fall_for_fog[fog], drift = drift_for_fog[fog];
        for (Flake& f : vs.flakes) {
            float sway = std::sin(clock * 1.3f + f.phase) * sway_for_fog[fog];
            f.p[0] += (drift + sway) * dt;
            f.p[1] -= fall * dt;
            f.p[2] += (drift * 0.6f + std::cos(clock * 1.1f + f.phase) * sway_for_fog[fog]) * dt;
            f.p[0] = wrap(f.p[0], cam[0], -half, half);
            f.p[1] = wrap(f.p[1], cam[1], -below, above);
            f.p[2] = wrap(f.p[2], cam[2], -half, half);
        }
        // The model's origin: kept for the race; moved (a jump past battle_render's teleport distance, so nothing is
        // smoothed across it) only when the camera is far enough for the vertices' s16 range.
        if (!vs.have_origin || std::fabs(cam[0] - vs.origin[0]) > 1500.0f || std::fabs(cam[1] - vs.origin[1]) > 1500.0f ||
            std::fabs(cam[2] - vs.origin[2]) > 1500.0f) {
            for (int k = 0; k < 3; k++) vs.origin[k] = cam[k];
            vs.have_origin = true;
        }

        // The list: render state, then per 15 flakes a G_VTX of their 60 vertices and their squares (two triangles
        // each, culling off). Vertices after the commands.
        vs.buffer = (vs.buffer + 1) % buffers;
        const uint32_t start = block + uint32_t(v * buffers + vs.buffer) * buffer_bytes;
        const uint32_t count = (uint32_t)vs.flakes.size();
        const uint32_t loads = (count + per_load - 1) / per_load;
        const uint32_t verts = start + ((64 + loads * (8 + per_load * 8) + 64 + 15) & ~15u);
        uint32_t at = start;
        GfxCommand push[3], pop[3];
        gEXPushOtherMode(&push[0]);
        gEXPushCombineMode(&push[1]);
        gEXPushGeometryMode(&push[2]);
        gEXPopOtherMode(&pop[0]);
        gEXPopCombineMode(&pop[1]);
        gEXPopGeometryMode(&pop[2]);
        for (const GfxCommand& c : push) write_cmd(rdram, at, c.values.word0, c.values.word1);
        write_cmd(rdram, at, 0xE7000000, 0);                 // G_RDPPIPESYNC
        write_cmd(rdram, at, 0xD7000000, 0xFFFFFFFF);        // G_TEXTURE off
        write_cmd(rdram, at, 0xD9F9F9FF, 0x00210005);        // no culling or lighting; z-buffer, shade, smooth, fog
        write_cmd(rdram, at, 0xE3000A01, 0x00100000);        // cycle type 2-cycle
        write_cmd(rdram, at, 0xE200001C, 0xC8112078);        // fog by shade alpha, then opaque z-buffered (the game's)
        write_cmd(rdram, at, 0xFCFFFFFF, 0xFFFE7838);        // combiner: shade, passed on
        const float* right = axes;
        const float* up = axes + 3;
        for (uint32_t q0 = 0; q0 < count; q0 += per_load) {
            const uint32_t n = std::min<uint32_t>(per_load, count - q0);
            const uint32_t vat = verts + q0 * 4 * 16;
            write_cmd(rdram, at, 0x01000000 | ((n * 4) << 12) | ((n * 4) << 1), vat);   // G_VTX
            for (uint32_t k = 0; k < n; k++) {
                const uint32_t b = 4 * k * 2;
                write_cmd(rdram, at, 0x06000000 | (b << 16) | ((b + 2) << 8) | (b + 4), ((b) << 16) | ((b + 4) << 8) | (b + 6));
                const Flake& f = vs.flakes[q0 + k];
                const float dx = f.p[0] - cam[0], dy = f.p[1] - cam[1], dz = f.p[2] - cam[2];
                const float size = dx * dx + dy * dy + dz * dz < near_cut * near_cut ? 0.0f : f.size;
                for (int c = 0; c < 4; c++) {
                    // A diamond (corners below, right, above, left) facing the camera.
                    // Stretched up and down by its speed (a fast fall reads as streaks, as a camera's shutter sees it).
                    const float su = c == 1 ? size : c == 3 ? -size : 0.0f;
                    const float sv = (c == 0 ? -size : c == 2 ? size : 0.0f) * (1.0f + fall * 0.15f);
                    int16_t p[3];
                    for (int a = 0; a < 3; a++) {
                        float w = f.p[a] + right[a] * su + up[a] * sv - vs.origin[a];
                        p[a] = (int16_t)std::clamp(int(std::lround(w * 16.0f)), -32768, 32767);
                    }
                    const uint32_t va = vat + (k * 4 + uint32_t(c)) * 16;
                    MEM_W(0, (int32_t)va) = (int32_t)((uint32_t(uint16_t(p[0])) << 16) | uint16_t(p[1]));
                    MEM_W(4, (int32_t)va) = (int32_t)(uint32_t(uint16_t(p[2])) << 16);
                    MEM_W(8, (int32_t)va) = 0;
                    const uint32_t s = f.shade;
                    MEM_W(12, (int32_t)va) = (int32_t)((s << 24) | (s << 16) | (std::min(255u, s + 14) << 8) | 0xFF);
                }
            }
        }
        write_cmd(rdram, at, 0xE7000000, 0);
        for (int c = 2; c >= 0; c--) write_cmd(rdram, at, pop[c].values.word0, pop[c].values.word1);
        write_cmd(rdram, at, 0xD7000002, 0xFFFFFFFF);        // G_TEXTURE on, as battle_render leaves it
        write_cmd(rdram, at, 0xDF000000, 0);

        // The trampoline: into the list above 16 MB and back.
        uint32_t t = trampolines + uint32_t(v) * 32;
        GfxCommand ext;
        gEXSetRDRAMExtended(&ext, 1);
        write_cmd(rdram, t, ext.values.word0, ext.values.word1);
        write_cmd(rdram, t, 0xDE000000, start);
        gEXSetRDRAMExtended(&ext, 0);
        write_cmd(rdram, t, ext.values.word0, ext.values.word1);
        write_cmd(rdram, t, 0xDF000000, 0);

        const float m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        rush2::battle_render::place(first_slot + v, model_name(v).c_str(), m, vs.origin, 0xFFFFFFFFu, v);
        vs.placed = true;
    }
}

void rush2::snowfall::set_enabled(bool on) { on_flag = on; }
bool rush2::snowfall::enabled() { return on_flag.load(); }

// pvs_apply_8007C27C, before each view: the view's snow moved and its list written (us.toml).
extern "C" void rush2_snowfall_view(uint8_t* rdram, recomp_context* ctx) {
    const int state = (int)MEM_W(0, (int32_t)game_state);
    const bool racing = state == 3 || state == 6 || state == 9 || state == 10;
    const float amount = racing && on_flag.load() ? rush2::track2049::mod_snowfall(rdram) : 0.0f;
    if (amount <= 0.0f) {
        for (int v = 0; v < max_views; v++) {
            if (views[size_t(v)].placed || !views[size_t(v)].flakes.empty()) reset(v);
        }
        return;
    }
    const int v = rush2::views::view_of_camera(rdram, (uint32_t)ctx->r5);
    if (v < 0 || v >= max_views) return;
    if (block == 0) {
        uint8_t* mem = (uint8_t*)recomp::alloc(rdram, size_t(buffer_bytes) * buffers * max_views + 16);
        if (mem == nullptr) return;
        block = (uint32_t(mem - rdram) + 0x80000000u + 15) & ~15u;
    }
    if (!models_added) {
        for (int k = 0; k < max_views; k++) {
            uint32_t t = trampolines + uint32_t(k) * 32;
            write_cmd(rdram, t, 0xDF000000, 0);
            rush2::battle_render::add_model(model_name(k).c_str(), trampolines + uint32_t(k) * 32);
        }
        models_added = true;
    }
    update(rdram, v, amount);
}
