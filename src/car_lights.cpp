// Car Lights (Graphics > Effects, include/car_lights.h, docs/graphics_effects.md#car-lights): on a night track
// (rush2::track2049::night_race: a converted course whose fog is near black, Rush 2049's night tracks and the
// PlayStation SF Rush's night versions), each race car's brake lights shine red on the road and whatever else is round
// them while Rush 2 lights them (braking, the car's flags 0x1000 set and 0x10 clear: func_800599A0). A side's light goes
// out once that rear corner's panel is damaged at all (level 1, car state +0xE4), and a wrecked car shows none.
//
// Where: the middle of what the game itself lights on that side of the car. The car's body display lists (frame,
// corners, top: rush2::car_types::part_record) are walked with their texture state, and every texel the game lights is
// put back on its triangle (barycentric from the texture coordinates) and averaged per side:
//   - Rush 2's palette lighting (its cars, Rush 2049's on the N64 ROM): CI8 texels at entries 161-175, the tail light
//     ramp func_800599A0 turns bright red;
//   - a lit copy (California Speed and Dreamcast cars, rush2::car2049::brake_light_sources): the texels that differ;
//   - a mod car's or livery's brake light lists (the lit lights drawn over the body): every triangle, by area.
// A car's lights are found again when its type, parts or damage stage change, one car a frame.
//
// How: RT64's car light pass (gEXSetCarLight; lib/rt64 CarLightPS.hlsl, FramebufferRenderer::submitCarLights). Each lit
// side is a marker, a model of the port's own placed through rush2::battle_render (slots 186-201) at the light, so it
// moves with the car through frame interpolation; RT64 doesn't draw the marker, it lights the view's 3D round it.
//
// The lenses glow too: on a converted track every car part is multiplied by the track's car light color
// (rush2::car2049::light_model), which on a night track is dark, so Rush 2's lit tail light texels (0xF801) came out
// dark red. While a side's light shines, each body part holding its lit texels is followed (model_draw, in the model
// draw hook after light_model, so with the part's own matrix) by those triangles again: an IA8 mask of the lit texels
// over the part's own texture coordinates, Rush 2's lit red as PRIM, unshaded and unfogged, blended over the lens as a
// decal. A mod car's or livery's brake light lists already draw unshaded (src/car_mods.cpp brake_render_mode).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"

#include "battle.h"
#include "battle_render.h"
#include "car2049.h"
#include "car_lights.h"
#include "car_types.h"
#include "ghost.h"
#include "rush2.h"
#include "rush2_hooks.h"
#include "track2049.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

extern "C" uint32_t rush2_interp_side_dl(uint8_t* rdram, const GfxCommand* cmds, uint32_t count);

namespace {
    constexpr int max_cars = 8;
    constexpr int first_slot = 186;               // to 201 (battle_render.h): car c's side s at first_slot + 2c + s
    constexpr const char* model_name = "CAR_LIGHT";

    // The marker model (0x80E1F200-0x80E1F300, src/controls_menu.cpp): three vertices and its display list.
    constexpr uint32_t vertices_at = 0x80E1F200;  // 3 x 16 bytes
    constexpr uint32_t dl_at = 0x80E1F240;

    // The light: red (31, 2, 1 of 31), strength 48 / 16 (a surface at the light 4 times its own red), reaching 12 ft
    // (in world units, 1/16 ft), from just behind the lens, in a cone straight back from the car (the marker's axes are
    // the car's: CarLightPS.hlsl). The pass multiplies what is drawn, so a dark night road needs the strength: 24 / 16
    // over 10 ft left it unchanged. A light shining every way lit its own car's body and roof.
    constexpr uint32_t light_red = 31, light_green = 2, light_blue = 1, light_strength = 48;
    constexpr uint32_t light_radius = 12 * 16;
    constexpr float light_behind = 0.3f;          // ft

    constexpr uint32_t game_state = 0x8010C0D0;   // s32: 3 and up a race
    constexpr uint32_t cars = 0x800F5470;         // physics cars
    constexpr uint32_t car_size = 0x81C;
    constexpr uint32_t car_wrecked = 0x648;       // s8
    constexpr uint32_t car_active = 0x7E4;        // s16, nonzero for a car in the race
    constexpr uint32_t car_type = 0x7EA;          // u8
    constexpr uint32_t car_corners = 0xE8;        // f32[4][3]: the collision box's corners (panel order)
    constexpr uint32_t car_states = 0x801124A0;
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t state_damage = 0xE4;       // u32: each panel's 2-bit damage level
    constexpr uint32_t panel_masks = 0x800CE188;  // u32 per panel (0 front right, 1 front left, 2 rear right, 3 rear left)
    constexpr uint32_t panel_shifts = 0x800CE180; // u8 per panel
    constexpr int tail_first = 161, tail_last = 175;   // Rush 2's tail light ramp (func_800599A0)

    std::atomic_bool enabled = false;
    std::atomic_bool braking[max_cars] = {};
    // Test aid: RUSH2_CAR_LIGHTS_LOG=1 prints each car's lights as they are found and each light as it comes on or goes off.
    const bool log_on = std::getenv("RUSH2_CAR_LIGHTS_LOG") != nullptr;
    // Test aid: RUSH2_CAR_LIGHTS_TEST=1 shines every car's lights as if it braked (the game's own lenses stay as they
    // are), to check where they land in any frame.
    const bool test_on = std::getenv("RUSH2_CAR_LIGHTS_TEST") != nullptr;

    struct Side {
        bool has = false;
        float pos[3] = {};   // ft, the body's frame
    };
    // A body part's glowing lenses: the display list drawing its lit triangles on each side (0: none).
    struct Emission {
        uint32_t list = 0;   // the part's own list (KSEG0), as the model draw hook gets it
        uint32_t dl[2] = {};
    };
    struct Lights {
        uint64_t key = 0;
        bool found = false;
        Side side[2];        // 0 x > 0, 1 x < 0
        std::vector<Emission> emission;
        uint32_t lists[6] = {};                          // the body parts' lists (KSEG0)
        std::vector<uint32_t> texture_commands[6];       // Finder::texture_commands
    };
    // The body parts (index into Lights::lists) each car has drawn so far in the view being drawn, in order: after the
    // lens glow, all their texture commands are replayed in that order, so a part that reuses what an earlier one
    // loaded (a corner using the frame's texture) finds it again.
    std::vector<int> drawn_parts[max_cars];

    // The lens glow's masks and display lists, above 16 MB (recomp::alloc; RT64 reads them with gEXSetRDRAMExtended):
    // two buffers a car, written in turn, so a rebuild never rewrites what the renderer may still read.
    constexpr uint32_t emission_buffer = 0x8000;
    uint32_t emission_base = 0;
    int emission_half[max_cars] = {};
    // The lit red (Rush 2's 0xF801), as PRIM.
    constexpr uint32_t lens_color = 0xFF0808FFu;
    Lights lights[max_cars];
    int next_car = 0;
    bool shown[max_cars][2] = {};
    bool model_added = false;

    float read_f(uint8_t* rdram, uint32_t at) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    bool plain(uint32_t w) { return (w >> 24) == 0x00 || (w >> 24) == 0x80; }   // KSEG0 or physical
    uint32_t kseg0(uint32_t w) { return 0x80000000u | (w & 0xFFFFFF); }

    // The marker: a sliver of a triangle at the model's origin between the car light command and its end.
    void write_model(uint8_t* rdram) {
        const int16_t v[3][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
        for (int i = 0; i < 3; i++) {
            uint32_t at = vertices_at + uint32_t(i) * 16;
            MEM_W(0, (int32_t)at) = (int32_t)((uint32_t(uint16_t(v[i][0])) << 16) | uint16_t(v[i][1]));
            MEM_W(4, (int32_t)at) = (int32_t)(uint32_t(uint16_t(v[i][2])) << 16);
            MEM_W(8, (int32_t)at) = 0;
            MEM_W(12, (int32_t)at) = (int32_t)0xFFFFFFFFu;
        }
        const uint32_t light = (light_red << 27) | (light_green << 22) | (light_blue << 17) | (light_strength << 11) | light_radius;
        const uint32_t ex = 0x64000000u | 0x39u;   // RT64_EXTENDED_OPCODE, G_EX_SETCARLIGHT_V1
        const uint32_t dl[][2] = {
            { ex, light },
            { 0x01003006u, vertices_at },          // G_VTX: 3 vertices to slots 0-2
            { 0x05000204u, 0 },                    // G_TRI1 (0 1 2)
            { ex, 0 },                             // the light ends
            { 0xDF000000u, 0 },                    // G_ENDDL
        };
        for (size_t i = 0; i < std::size(dl); i++) {
            MEM_W(0, (int32_t)(dl_at + i * 8)) = (int32_t)dl[i][0];
            MEM_W(4, (int32_t)(dl_at + i * 8)) = (int32_t)dl[i][1];
        }
    }

    // ---- finding a car's lights -------------------------------------------------------------------------------------

    struct Finder {
        uint8_t* rdram;
        const rush2::car2049::BrakeLightSources& sources;
        double sum[2][3] = {};
        double weight[2] = {};
        // Texture state.
        struct Tile { int fmt = 0, siz = 0, line = 0, tmem = 0; float uls = 0, ult = 0, lrt = 0; uint32_t wrap = 0, size0 = 0, size1 = 0; };
        Tile tiles[8];
        uint32_t timg = 0;
        int timg_siz = 0, timg_width = 0;
        std::map<int, uint32_t> tmem_image;   // TMEM word -> the image loaded there
        int tex_tile = 0;
        float sscale = 1.0f, tscale = 1.0f;
        uint32_t texture_w1 = 0;
        std::array<uint32_t, 64> slots{};
        // Each texture's lit texels (u, v), by image, row bytes and rows.
        using MaskKey = std::tuple<uint32_t, int, int, int>;
        std::map<MaskKey, std::vector<std::array<int, 2>>> masks;
        // The triangles holding lit texels, for the lens glow: their part, side, vertices and texture state.
        struct LitTriangle {
            int list = 0, side = 0;
            uint32_t v[3] = {};
            MaskKey mask;
            int width = 0, rows = 0;
            uint32_t texture_w1 = 0, wrap = 0, size0 = 0, size1 = 0;
        };
        std::vector<LitTriangle> lit_triangles;
        int list_index = 0;
        // Each part's texture commands (loads, tiles, G_TEXTURE, syncs), by address in drawing order: the lens glow
        // replays them after itself, so the next part finds TMEM and the tiles as this part left them (a corner reuses
        // what the one before it loaded: the overlay's mask load blacked out the next corner's lens).
        std::vector<uint32_t> texture_commands[6];

        Finder(uint8_t* r, const rush2::car2049::BrakeLightSources& s) : rdram(r), sources(s) {}

        void add(const float p[3], double w) {
            int s = p[0] >= 0.0f ? 0 : 1;
            for (int k = 0; k < 3; k++) sum[s][k] += p[k] * w;
            weight[s] += w;
        }

        // The lit texels of the texture at `image`, `row` bytes a row, `rows` rows, of texel size siz (0 4b ... 3 32b).
        const std::vector<std::array<int, 2>>& mask(uint32_t image, int row, int rows, int siz, int fmt) {
            auto key = std::make_tuple(image, row, rows, (siz << 4) | fmt);
            auto it = masks.find(key);
            if (it != masks.end()) return it->second;
            std::vector<std::array<int, 2>>& m = masks[key];
            if (row <= 0 || rows <= 0 || row > 4096 || rows > 1024 || !rush2::in_game_ram(image + uint32_t(row * rows))) return m;
            const int bytes = siz == 0 ? 1 : (1 << (siz - 1));   // 4b texels are read a byte (two) at a time
            const int per_row = siz == 0 ? row * 2 : row / bytes;
            // A lit copy of this texture (either way round: the car may be drawn lit right now).
            uint32_t lit = 0;
            const std::vector<uint8_t>* lit_bytes = nullptr;
            for (const auto& [a, b] : sources.lit_copies) {
                if (a == image) lit = b;
                else if (b == image) lit = a;
            }
            for (const auto& [a, bytes_of] : sources.lit_bytes) {
                if (a == image) lit_bytes = &bytes_of;
            }
            const bool palette = sources.palette && fmt == 2 && siz == 1;
            if (lit == 0 && lit_bytes == nullptr && !palette) return m;
            for (int v = 0; v < rows; v++) {
                for (int u = 0; u < per_row; u++) {
                    uint32_t off = uint32_t(v * row + (siz == 0 ? u / 2 : u * bytes));
                    bool hit = false;
                    if (palette) {
                        int index = MEM_BU(0, (int32_t)(image + off));
                        hit = index >= tail_first && index <= tail_last;
                    }
                    for (int b = 0; b < bytes && !hit; b++) {
                        uint8_t a = MEM_BU(0, (int32_t)(image + off + uint32_t(b)));
                        if (lit != 0) hit = a != MEM_BU(0, (int32_t)(lit + off + uint32_t(b)));
                        else if (lit_bytes != nullptr && off + uint32_t(b) < lit_bytes->size()) hit = a != (*lit_bytes)[off + uint32_t(b)];
                    }
                    if (hit) m.push_back({ u, v });
                }
            }
            return m;
        }

        // A textured triangle: each lit texel of its texture inside its texture coordinates, put back on it.
        void triangle(uint32_t a, uint32_t b, uint32_t c, bool whole) {
            uint32_t v[3] = { slots[(a / 2) & 63], slots[(b / 2) & 63], slots[(c / 2) & 63] };
            float p[3][3], uv[3][2];
            for (int k = 0; k < 3; k++) {
                if (v[k] == 0) return;
                for (int j = 0; j < 3; j++) p[k][j] = (float)(int16_t)MEM_H(0, (int32_t)(v[k] + j * 2)) / 16.0f;
                uv[k][0] = (float)(int16_t)MEM_H(0, (int32_t)(v[k] + 8)) * sscale / 32.0f;
                uv[k][1] = (float)(int16_t)MEM_H(0, (int32_t)(v[k] + 10)) * tscale / 32.0f;
            }
            if (whole) {
                // A brake light list's triangle: all of it, by its area.
                float e1[3], e2[3], n[3], mid[3];
                for (int j = 0; j < 3; j++) e1[j] = p[1][j] - p[0][j], e2[j] = p[2][j] - p[0][j], mid[j] = (p[0][j] + p[1][j] + p[2][j]) / 3.0f;
                n[0] = e1[1] * e2[2] - e1[2] * e2[1];
                n[1] = e1[2] * e2[0] - e1[0] * e2[2];
                n[2] = e1[0] * e2[1] - e1[1] * e2[0];
                double area = 0.5 * std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]);
                if (area > 0.0) add(mid, area);
                return;
            }
            const Tile& t = tiles[tex_tile & 7];
            auto img = tmem_image.find(t.tmem);
            if (img == tmem_image.end()) return;
            const int row = t.line * 8, rows = int(t.lrt - t.ult) + 1;
            const std::vector<std::array<int, 2>>& m = mask(img->second, row, rows, t.siz, t.fmt);
            if (m.empty()) return;
            const int width = t.siz == 0 ? row * 2 : row / (1 << (t.siz - 1));
            for (int k = 0; k < 3; k++) uv[k][0] -= t.uls, uv[k][1] -= t.ult;
            int hits = 0;
            double hit_x = 0.0;
            float lo[2] = { std::min({ uv[0][0], uv[1][0], uv[2][0] }), std::min({ uv[0][1], uv[1][1], uv[2][1] }) };
            float hi[2] = { std::max({ uv[0][0], uv[1][0], uv[2][0] }), std::max({ uv[0][1], uv[1][1], uv[2][1] }) };
            const float d = (uv[1][1] - uv[2][1]) * (uv[0][0] - uv[2][0]) + (uv[2][0] - uv[1][0]) * (uv[0][1] - uv[2][1]);
            if (std::fabs(d) < 1e-6f || width <= 0 || rows <= 0 || hi[0] - lo[0] > 4096 || hi[1] - lo[1] > 4096) return;
            for (const auto& texel : m) {
                // The texel and its repeats across the coordinates' span (wrapped textures).
                for (float ty = texel[1] + 0.5f + rows * std::floor((lo[1] - texel[1]) / rows); ty <= hi[1]; ty += rows) {
                    for (float tx = texel[0] + 0.5f + width * std::floor((lo[0] - texel[0]) / width); tx <= hi[0]; tx += width) {
                        float l0 = ((uv[1][1] - uv[2][1]) * (tx - uv[2][0]) + (uv[2][0] - uv[1][0]) * (ty - uv[2][1])) / d;
                        float l1 = ((uv[2][1] - uv[0][1]) * (tx - uv[2][0]) + (uv[0][0] - uv[2][0]) * (ty - uv[2][1])) / d;
                        float l2 = 1.0f - l0 - l1;
                        if (l0 < -1e-3f || l1 < -1e-3f || l2 < -1e-3f) continue;
                        float q[3];
                        for (int j = 0; j < 3; j++) q[j] = l0 * p[0][j] + l1 * p[1][j] + l2 * p[2][j];
                        add(q, 1.0);
                        hits++;
                        hit_x += q[0];
                    }
                }
            }
            if (hits > 0) {
                LitTriangle lt;
                lt.list = list_index;
                lt.side = hit_x >= 0.0 ? 0 : 1;
                for (int k = 0; k < 3; k++) lt.v[k] = v[k];
                lt.mask = std::make_tuple(img->second, row, rows, (t.siz << 4) | t.fmt);
                lt.width = width;
                lt.rows = rows;
                lt.texture_w1 = texture_w1;
                lt.wrap = t.wrap;
                lt.size0 = t.size0;
                lt.size1 = t.size1;
                lit_triangles.push_back(lt);
            }
        }

        // F3DEX2 commands (and the 2049 conversion's call, as battle.cpp's body_triangles reads them).
        void walk(uint32_t dl, int depth, bool whole) {
            if (depth > 8 || !plain(dl)) return;
            uint32_t at = kseg0(dl);
            for (int n = 0; n < 8192; n++, at += 8) {
                if (!rush2::in_game_ram(at + 8)) return;
                uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)at), w1 = (uint32_t)MEM_W(4, (int32_t)at);
                uint32_t op = w0 >> 24;
                if (!whole && (op == 0xD7 || op == 0xFD || op == 0xF5 || op == 0xF2 || op == 0xF3 || op == 0xF4 || op == 0xF0 ||
                               op == 0xE6 || op == 0xE7 || op == 0xE8)) {
                    texture_commands[list_index].push_back(at);
                }
                switch (op) {
                case 0x01: {   // G_VTX
                    uint32_t count = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F;
                    uint32_t a = kseg0(w1);
                    bool ok = plain(w1) && rush2::in_game_ram(a + count * 16);
                    for (uint32_t k = 0; k < count; k++) slots[(end - count + k) & 63] = ok ? a + k * 16 : 0;
                    break;
                }
                case 0x05: triangle((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF, whole); break;
                case 0x06:
                case 0x07:
                    triangle((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF, whole);
                    triangle((w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF, whole);
                    break;
                case 0xD7:     // G_TEXTURE
                    tex_tile = (w0 >> 8) & 7;
                    texture_w1 = w1;
                    sscale = float(w1 >> 16) / 65536.0f;
                    tscale = float(w1 & 0xFFFF) / 65536.0f;
                    break;
                case 0xFD:     // G_SETTIMG
                    timg = plain(w1) ? kseg0(w1) : 0;
                    timg_siz = (w0 >> 19) & 3;
                    timg_width = int(w0 & 0xFFF) + 1;
                    break;
                case 0xF5: {   // G_SETTILE
                    Tile& t = tiles[(w1 >> 24) & 7];
                    t.fmt = (w0 >> 21) & 7;
                    t.siz = (w0 >> 19) & 3;
                    t.line = (w0 >> 9) & 0x1FF;
                    t.tmem = w0 & 0x1FF;
                    t.wrap = w1 & 0x000FFFFF;   // cmt, maskt, shiftt, cms, masks, shifts
                    break;
                }
                case 0xF2: {   // G_SETTILESIZE
                    Tile& t = tiles[(w1 >> 24) & 7];
                    t.uls = float((w0 >> 12) & 0xFFF) / 4.0f;
                    t.ult = float(w0 & 0xFFF) / 4.0f;
                    t.lrt = float(w1 & 0xFFF) / 4.0f;
                    t.size0 = w0 & 0x00FFFFFF;
                    t.size1 = w1 & 0x00FFFFFF;
                    break;
                }
                case 0xF3:     // G_LOADBLOCK: the image from its start
                    if (timg != 0) tmem_image[tiles[(w1 >> 24) & 7].tmem] = timg;
                    break;
                case 0xF4: {   // G_LOADTILE: from its corner
                    if (timg == 0) break;
                    uint32_t uls = ((w0 >> 12) & 0xFFF) / 4, ult = (w0 & 0xFFF) / 4;
                    uint32_t bytes = timg_siz == 0 ? 0 : (1u << (timg_siz - 1));
                    tmem_image[tiles[(w1 >> 24) & 7].tmem] = timg + ult * uint32_t(timg_width) * bytes + uls * bytes;
                    break;
                }
                case 0xDE:     // G_DL
                    walk(w1, depth + 1, whole);
                    if (((w0 >> 16) & 0xFF) != 0) return;
                    break;
                case 0xE0:     // the 2049 conversion's call
                    if (((w0 >> 16) & 0xFF) == 1) walk(w1, depth + 1, whole);
                    break;
                case 0xDF: return;
                default: break;
                }
            }
        }
    };

    // Car i's body lists (frame, the four corners, the top) and a key of them, its type and its copy.
    uint64_t body_key(uint8_t* rdram, int i, uint32_t (&lists)[6], uint32_t base) {
        static constexpr int parts[] = { rush2::car_types::part_frame, rush2::car_types::part_front_right, rush2::car_types::part_front_left,
                                         rush2::car_types::part_rear_right, rush2::car_types::part_rear_left, rush2::car_types::part_top };
        int type = MEM_BU(0, (int32_t)(cars + uint32_t(i) * car_size + car_type));
        uint64_t key = (0xCBF29CE484222325ull ^ uint64_t(type + 1)) * 0x100000001B3ull;
        key = (key ^ base) * 0x100000001B3ull;
        for (size_t k = 0; k < std::size(parts); k++) {
            uint32_t record = rush2::car_types::part_record(rdram, i, parts[k]);
            lists[k] = record != 0 && rush2::in_game_ram(record) ? (uint32_t)MEM_W(0xC, (int32_t)record) : 0;
            key = (key ^ lists[k]) * 0x100000001B3ull;
        }
        return key;
    }

    void write_cmd(uint8_t* rdram, uint32_t& at, uint32_t w0, uint32_t w1) {
        MEM_W(0, (int32_t)at) = (int32_t)w0;
        MEM_W(4, (int32_t)at) = (int32_t)w1;
        at += 8;
    }

    // Car i's lens glow lists (see the top): the lit texels' masks, then a list per part and side, into the car's next
    // buffer. Each triangle goes out as three one-vertex G_VTX (the part's own vertices) and a G_TRI1, a mask load
    // before each change of texture.
    void build_emission(uint8_t* rdram, int i, const Finder& f, const uint32_t (&lists)[6], Lights& l) {
        if (f.lit_triangles.empty()) return;
        if (emission_base == 0) {
            uint8_t* block = (uint8_t*)recomp::alloc(rdram, emission_buffer * 2 * max_cars + 16);
            if (block == nullptr) return;
            emission_base = (uint32_t(block - rdram) + 0x80000000u + 15) & ~15u;
        }
        emission_half[i] ^= 1;
        const uint32_t start = emission_base + uint32_t(i * 2 + emission_half[i]) * emission_buffer;
        const uint32_t end = start + emission_buffer;
        uint32_t at = start;
        // The masks: IA8, the source tile's texels a row (padded to 8), lit 0xFF, else 0.
        std::map<Finder::MaskKey, uint32_t> mask_at;
        std::map<Finder::MaskKey, int> mask_pitch;
        for (const Finder::LitTriangle& t : f.lit_triangles) {
            if (mask_at.contains(t.mask)) continue;
            const int pitch = (t.width + 7) & ~7;
            const uint32_t bytes = uint32_t(pitch * t.rows);
            if (bytes > 4096 || at + bytes > end) {
                mask_at[t.mask] = 0;
                continue;
            }
            for (uint32_t b = 0; b < bytes; b += 4) MEM_W(0, (int32_t)(at + b)) = 0;
            auto m = f.masks.find(t.mask);
            if (m != f.masks.end()) {
                for (const auto& texel : m->second) {
                    if (texel[0] < t.width && texel[1] < t.rows) {
                        MEM_B(0, (int32_t)(at + uint32_t(texel[1] * pitch + texel[0]))) = (int8_t)0xFF;
                    }
                }
            }
            if (log_on) {
                std::printf("[car_lights] car %d mask: image %08X fmt %d siz %d, %d x %d texels (pitch %d), %zu lit\n", i,
                            std::get<0>(t.mask), std::get<3>(t.mask) & 15, std::get<3>(t.mask) >> 4, t.width, t.rows, pitch,
                            m != f.masks.end() ? m->second.size() : size_t(0));
            }
            mask_at[t.mask] = at;
            mask_pitch[t.mask] = pitch;
            at += (bytes + 7) & ~7u;
        }
        GfxCommand push[3], pop[3];
        gEXPushOtherMode(&push[0]);
        gEXPushCombineMode(&push[1]);
        gEXPushPrimColor(&push[2]);
        gEXPopOtherMode(&pop[0]);
        gEXPopCombineMode(&pop[1]);
        gEXPopPrimColor(&pop[2]);
        size_t lit_count = 0;
        for (int k = 0; k < 6; k++) {
            if (lists[k] == 0) continue;
            Emission e;
            e.list = kseg0(lists[k]);
            for (int s = 0; s < 2; s++) {
                size_t count = 0;
                for (const Finder::LitTriangle& t : f.lit_triangles) {
                    if (t.list == k && t.side == s && mask_at[t.mask] != 0) count++;
                }
                // At most 14 commands round it, a load of 9 per triangle and 4 per triangle.
                if (count == 0 || at + (14 + count * 13) * 8 > end) continue;
                lit_count += count;
                e.dl[s] = at;
                for (const GfxCommand& c : push) write_cmd(rdram, at, c.values.word0, c.values.word1);
                write_cmd(rdram, at, 0xE7000000, 0);               // G_RDPPIPESYNC
                write_cmd(rdram, at, 0xE3000A01, 0x00100000);      // cycle type 2-cycle
                write_cmd(rdram, at, 0xE3001001, 0);               // TEXTLUT = NONE
                write_cmd(rdram, at, 0xE3000F00, 0);               // TEXTLOD = TILE
                // Render mode: no fog (G_RM_PASS), then G_RM_ZB_XLU_DECAL2: blended by the mask's alpha over the lens's
                // own depth, the depth not written.
                write_cmd(rdram, at, 0xE200001C, 0x0C184E50);
                write_cmd(rdram, at, 0xFCFFFFFF, 0xFFFDF238);      // combiner: PRIM, alpha TEXEL0; then passed on
                write_cmd(rdram, at, 0xFA000000, lens_color);
                const Finder::LitTriangle* loaded = nullptr;
                for (const Finder::LitTriangle& t : f.lit_triangles) {
                    if (t.list != k || t.side != s || mask_at[t.mask] == 0) continue;
                    static const bool diag_notex = std::getenv("RUSH2_CAR_LIGHTS_DIAG_NOTEX") != nullptr;
                    if (diag_notex) {
                        if (loaded == nullptr) write_cmd(rdram, at, 0xFCFFFFFF, 0xFFFDFC38);   // PRIM, alpha 1
                        loaded = &t;
                    }
                    else if (loaded == nullptr || t.mask != loaded->mask || t.texture_w1 != loaded->texture_w1 || t.wrap != loaded->wrap ||
                        t.size0 != loaded->size0 || t.size1 != loaded->size1) {
                        const uint32_t image = mask_at[t.mask];
                        const int pitch = mask_pitch[t.mask];
                        const uint32_t words = uint32_t(pitch / 8);
                        const uint32_t texels = uint32_t(pitch * t.rows + 1) / 2;   // loaded as 16-bit texels
                        const uint32_t dxt = (2048 + words - 1) / words;
                        write_cmd(rdram, at, 0xFD000000 | (3u << 21) | (2u << 19), image);        // G_SETTIMG IA 16b
                        write_cmd(rdram, at, 0xF5000000 | (3u << 21) | (2u << 19), 0x07000000);  // load tile 7
                        write_cmd(rdram, at, 0xE6000000, 0);                                       // G_RDPLOADSYNC
                        write_cmd(rdram, at, 0xF3000000, 0x07000000 | ((texels - 1) << 12) | (dxt & 0xFFF));
                        write_cmd(rdram, at, 0xE7000000, 0);
                        write_cmd(rdram, at, 0xF5000000 | (3u << 21) | (1u << 19) | (words << 9), t.wrap);  // tile 0, IA8
                        write_cmd(rdram, at, 0xF2000000 | t.size0, t.size1);
                        write_cmd(rdram, at, 0xD7000002, t.texture_w1);    // G_TEXTURE on, tile 0, the part's scale
                        write_cmd(rdram, at, 0xE7000000, 0);
                        loaded = &t;
                    }
                    for (uint32_t v = 0; v < 3; v++) write_cmd(rdram, at, 0x01001000 | ((v + 1) << 1), t.v[v]);   // G_VTX 1 to slot v
                    write_cmd(rdram, at, 0x05000204, 0);           // G_TRI1 (0 1 2)
                }
                write_cmd(rdram, at, 0xE7000000, 0);
                for (int c = 2; c >= 0; c--) write_cmd(rdram, at, pop[c].values.word0, pop[c].values.word1);
                write_cmd(rdram, at, 0xDF000000, 0);               // G_ENDDL
            }
            if (e.dl[0] != 0 || e.dl[1] != 0) l.emission.push_back(e);
        }
        for (int k = 0; k < 6; k++) {
            l.lists[k] = lists[k] != 0 ? kseg0(lists[k]) : 0;
            l.texture_commands[k] = f.texture_commands[k];
            if (log_on) {
                int tluts = 0, loads = 0;
                for (uint32_t a : f.texture_commands[k]) {
                    uint32_t op = (uint32_t)MEM_W(0, (int32_t)a) >> 24;
                    tluts += op == 0xF0;
                    loads += op == 0xF3 || op == 0xF4;
                }
                std::printf("[car_lights] car %d part %d: list %08X, %d loads, %d TLUT loads\n", i, k, l.lists[k], loads, tluts);
            }
        }
        if (log_on) {
            std::printf("[car_lights] car %d: %zu of %zu lit triangles glow, %zu parts, %u bytes\n", i, lit_count,
                        f.lit_triangles.size(), l.emission.size(), at - start);
            std::fflush(stdout);
        }
    }

    void find(uint8_t* rdram, int i) {
        rush2::car2049::BrakeLightSources sources;
        rush2::car2049::brake_light_sources(rdram, i, sources);
        uint32_t lists[6];
        uint64_t key = body_key(rdram, i, lists, sources.base);
        Lights& l = lights[i];
        if (l.key == key && key != 0) return;
        l = Lights{};
        l.key = key;
        Finder f(rdram, sources);
        // A mod car's or livery's brake light lists are its lights, where it has them; otherwise its lit texels.
        for (uint32_t list : sources.lists) f.walk(list, 0, true);
        if (f.weight[0] + f.weight[1] <= 0.0) {
            for (int k = 0; k < 6; k++) {
                f.list_index = k;
                if (lists[k] != 0) f.walk(lists[k], 0, false);
            }
            build_emission(rdram, i, f, lists, l);
        }
        for (int s = 0; s < 2; s++) {
            if (f.weight[s] <= 0.0) continue;
            l.side[s].has = true;
            for (int k = 0; k < 3; k++) l.side[s].pos[k] = float(f.sum[s][k] / f.weight[s]);
        }
        l.found = true;
        if (log_on) {
            std::printf("[car_lights] car %d type %d: %zu lit copies, %zu lists, palette %d; +x %s (%.2f, %.2f, %.2f) w %.0f, "
                        "-x %s (%.2f, %.2f, %.2f) w %.0f\n", i, (int)MEM_BU(0, (int32_t)(cars + uint32_t(i) * car_size + car_type)),
                        sources.lit_copies.size() + sources.lit_bytes.size(), sources.lists.size(), (int)sources.palette,
                        l.side[0].has ? "at" : "none", l.side[0].pos[0], l.side[0].pos[1], l.side[0].pos[2], f.weight[0],
                        l.side[1].has ? "at" : "none", l.side[1].pos[0], l.side[1].pos[1], l.side[1].pos[2], f.weight[1]);
            std::fflush(stdout);
        }
    }

    int panel_level(uint8_t* rdram, int car, int panel) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)(car_states + uint32_t(car) * car_state_size + state_damage));
        uint32_t mask = (uint32_t)MEM_W(0, (int32_t)(panel_masks + panel * 4));
        return int((bits & mask) >> MEM_BU(0, (int32_t)(panel_shifts + panel)));
    }

    void hide(int i, int s) {
        if (!shown[i][s]) return;
        rush2::battle_render::hide(first_slot + i * 2 + s);
        shown[i][s] = false;
        if (log_on) std::printf("[car_lights] car %d side %d off\n", i, s), std::fflush(stdout);
    }

    void hide_all() {
        for (int i = 0; i < max_cars; i++) hide(i, 0), hide(i, 1);
    }
}

void rush2::car_lights::set_enabled(bool on) {
    enabled = on;
}

void rush2::car_lights::set_braking(int car, bool lit) {
    if (car >= 0 && car < max_cars) braking[car] = lit;
}

// pvs_apply_8007C27C, before each view: the lit lights' markers placed for the views about to be drawn.
extern "C" void rush2_car_lights_view(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (!enabled || (int32_t)MEM_W(0, (int32_t)game_state) < 3 || !rush2::track2049::night_race(rdram)) {
        hide_all();
        for (Lights& l : lights) l = Lights{};
        return;
    }
    for (std::vector<int>& d : drawn_parts) d.clear();
    write_model(rdram);
    if (!model_added) {
        rush2::battle_render::add_model(model_name, dl_at);
        model_added = true;
    }
    // One car's lights looked for (again) a call, if its body changed.
    find(rdram, next_car);
    next_car = (next_car + 1) % max_cars;

    for (int i = 0; i < max_cars; i++) {
        const uint32_t car = cars + uint32_t(i) * car_size;
        const Lights& l = lights[i];
        if (!(braking[i] || test_on) || !l.found || MEM_H(0, (int32_t)(car + car_active)) == 0 || MEM_B(0, (int32_t)(car + car_wrecked)) != 0 ||
            rush2::ghost::is_ghost_car(i)) {
            hide(i, 0), hide(i, 1);
            continue;
        }
        float pos[3], m[9];
        rush2::battle::car_body_pose(rdram, i, pos, m);
        // Which side is the right: the rear right corner's (panel 2) side of the car.
        const bool right_positive = read_f(rdram, car + car_corners + 2 * 12) >= 0.0f;
        for (int s = 0; s < 2; s++) {
            const int panel = (s == 0) == right_positive ? 2 : 3;
            // Out once that corner is damaged at all: collisions only ever raise a panel to level 1 (func_8006B730,
            // func_8009A264 and func_800715E8 stop at slti 2), the level that swaps in its damaged model.
            const int level = panel_level(rdram, i, panel);
            if (!l.side[s].has || level >= 1) {
                if (log_on && shown[i][s] && level >= 1) {
                    std::printf("[car_lights] car %d side %d: panel %d damaged (level %d)\n", i, s, panel, level);
                }
                hide(i, s);
                continue;
            }
            float local[3] = { l.side[s].pos[0], l.side[s].pos[1], l.side[s].pos[2] - light_behind };
            float at[3];
            for (int k = 0; k < 3; k++) at[k] = pos[k] + m[0 + k] * local[0] + m[3 + k] * local[1] + m[6 + k] * local[2];
            rush2::battle_render::place(first_slot + i * 2 + s, model_name, m, at, 0xFFFFFFFFu);
            if (log_on && !shown[i][s]) {
                std::printf("[car_lights] car %d side %d on at (%.1f, %.1f, %.1f)\n", i, s, at[0], at[1], at[2]);
                std::fflush(stdout);
            }
            shown[i][s] = true;
        }
    }
}

// The model draw hook (src/widescreen.cpp), after rush2::car2049::light_model: a lit car's body part with its lens glow
// after it, for each side whose light shines (rush2_car_lights_view's markers, this frame's).
void rush2::car_lights::model_draw(uint8_t* rdram, recomp_context* ctx, uint32_t dl) {
    if (!enabled || emission_base == 0) return;
    const int car = rush2::car2049::model_car((uint32_t)ctx->r23);
    if (car < 0 || car >= max_cars) return;
    const Lights& l = lights[car];
    const uint32_t list = kseg0(dl);
    int part = -1;
    for (int k = 0; k < 6 && part < 0; k++) {
        if (l.lists[k] == list) part = k;
    }
    if (part < 0) return;
    std::vector<int>& drawn = drawn_parts[car];
    if (std::find(drawn.begin(), drawn.end(), part) == drawn.end()) drawn.push_back(part);
    if (!(shown[car][0] || shown[car][1])) return;
    for (const Emission& e : l.emission) {
        if (e.list != list) continue;
        std::vector<GfxCommand> cmds;
        auto add = [&](uint32_t w0, uint32_t w1) {
            GfxCommand c;
            c.values.word0 = w0;
            c.values.word1 = w1;
            cmds.push_back(c);
        };
        add(0xDE000000, (uint32_t)ctx->r30);   // G_DL, pushed: the part as it was going to be drawn
        GfxCommand c;
        gEXSetRDRAMExtended(&c, 1);
        cmds.push_back(c);
        bool any = false;
        static const bool diag_skip = std::getenv("RUSH2_CAR_LIGHTS_DIAG_SKIP") != nullptr;
        for (int s = 0; s < 2; s++) {
            if (!shown[car][s] || e.dl[s] == 0) continue;
            if (!diag_skip) add(0xDE000000, e.dl[s]);
            any = true;
        }
        if (!any) return;
        gEXSetRDRAMExtended(&c, 0);
        cmds.push_back(c);
        // The texture commands of the car's parts drawn so far, this one last, as they are now (a lit or damaged copy
        // may have been swapped in).
        size_t replayed = 0;
        for (int k : drawn) {
            for (uint32_t at : l.texture_commands[k]) add((uint32_t)MEM_W(0, (int32_t)at), (uint32_t)MEM_W(4, (int32_t)at));
            replayed += l.texture_commands[k].size();
        }
        ctx->r30 = (uint64_t)(int64_t)(int32_t)rush2_interp_side_dl(rdram, cmds.data(), uint32_t(cmds.size()));
        static bool logged[max_cars] = {};
        if (log_on && !logged[car]) {
            logged[car] = true;
            std::printf("[car_lights] car %d: lens glow after part %d (%zu parts drawn, %zu texture commands replayed)\n", car, part,
                        drawn.size(), replayed);
            std::fflush(stdout);
        }
        return;
    }
}
