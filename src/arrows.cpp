// The arrows over the other players' cars, for every view of a race.
//
// Rush 2 (func_80086CA4, race setup) gives each player's car one world polygon with the ARROW image when there are two
// players: drawn in the other player's view only (flags 1 << (1 - car) | 0x200), in the car's paint color (table
// 0x800CE19C by the car's +0x7EB), and moved every frame by func_80059B9C: the corners (1, 1), (-1, 1), (-1, -1),
// (1, -1) turned to face the other view's camera, times s = the car's distance from that camera / 25, around the car's
// position + (0, s + 5, 0). One polygon can face and be sized for one camera only, so with three or four players
// (src/players4.cpp) those arrows are wrong in every view but one and missing for players 3 and 4.
//
// Rush 2049 has the same arrows (main func_800B1B48 and func_8008C884, the same code with the views generalized): a
// polygon per view for each other car. Here, with three or four players, every view gets one for every other
// player's car, placed as Rush 2 places its own, and Rush 2's are hidden. With two players Rush 2's arrows are left
// as they are.
//
// In a battle (src/rush2049/battle.cpp) the arrows are Rush 2049's battle ones, for any number of players: in the player
// colors (table 0x8011B558), drawn in front of everything (primitive depth 1), and with a = the angle from the view's
// direction to the car, seen from above:
// - |a| under 0.48 of the view's field of view (the car is in view): over the car, as above.
// - up to a quarter turn: at the view's left or right edge, pointing out, and lower the further round the car is
//   (from 0.75 of the vertical field of view, where it is level with the middle, down to the bottom).
// - behind: along the bottom edge, pointing down, at the middle for a car straight behind.
// The edge arrows are 2049's 2 units across, 10 pi / the field of view ahead of the camera: a twentieth of the
// view's width. docs/rush2049_research/battle.md section 6.1 has the arithmetic.
//
// Rush 2's world polygons are the same system as 2049's (0x800FAF00, 0x58 each: +0 vertex count, +2 flags, +4
// texture, +6 primitive depth, then 0x14 per vertex: position x 16, texture coordinates, color; func_80054010 makes
// one, func_8007C624 draws those with its view's bit). The image is Rush 2's own ARROW, in its effects container.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "arrows.h"
#include "battle.h"
#include "rush2.h"
#include "rush2_hooks.h"

namespace {
    constexpr uint32_t cars = 0x800F5470;                 // 0x81C each
    constexpr uint32_t car_size = 0x81C;
    constexpr uint32_t car_paint = 0x7EB;                 // u8: index of the car's paint color
    constexpr uint32_t car_states = 0x801124A0;           // 0x354 each: +0 the position a car is drawn at
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t player_slots = 0x800C2140;         // 0x28 per player: +0 car index
    constexpr uint32_t player_size = 0x28;
    constexpr uint32_t num_players = 0x8010C3E2;          // s16
    constexpr uint32_t cameras = 0x800E79D0;              // Per view, 0x40: rows +0 right, +0xC up, +0x18 forward; +0x24 position
    constexpr uint32_t game_state = 0x8010C0D0;           // 3 a race, 10 its countdown
    constexpr uint32_t paint_colors = 0x800CE19C;         // RGBA per paint index
    constexpr uint32_t view_tilt = 0x800D0171;            // s8: the view is turned about its direction
    constexpr uint32_t view_tilt_angle = 0x800CF9DC;      // f32
    constexpr int max_cars = 8;

    constexpr uint32_t polygons = 0x800FAF00;
    constexpr int polygon_size = 0x58;
    constexpr uint32_t polygon_count = 0x800FAEF0;        // s32: slots in use
    constexpr uint32_t polygon_high = 0x800FAEF4;         // s32: the most slots used
    constexpr int polygon_limit = 1000;
    constexpr uint32_t texture_tables = 0x80119220;       // per model container: its texture records (0x20 each), count
    constexpr uint32_t texture_table_count = 0x800D5788;  // u8
    constexpr uint16_t polygon_hidden = 0x8000;
    constexpr uint16_t race_flags = 0x200;                // Rush 2's arrows'
    constexpr uint16_t battle_flags = 0x3200;             // 2049's battle arrows'; 0x2000 draws at the primitive depth
    constexpr int16_t battle_depth = 1;
    constexpr int16_t race_mark = 0x4152;                 // +6 of a race's arrow (not drawn at a depth): marks it as one of these
    constexpr float arrow_in_view = 0.48f;                // 2049 0x8012390C
    constexpr float arrow_hover = 5.0f, arrow_hover_scale = 1.0f / 25.0f;
    constexpr float arrow_distance = 20.0f;               // 10 pi / 2049's field of view (a quarter turn)
    constexpr float arrow_size = 0.05f;                   // half an edge arrow, as a part of the view's width
    constexpr float arrow_side_start = 0.75f;             // of the vertical field of view (2 or more views)
    constexpr float arrow_bottom = 16.5f * 0.6366198f / 20.0f;   // an arrow at the bottom: this x the vertical field of view down
    constexpr uint32_t battle_colors[5] = { 0x0000E0FF, 0xE00000FF, 0xE0E000FF, 0x00E000FF, 0xE0E0E0FF };   // 2049 0x8011B558
    constexpr uint8_t arrow_alpha_invisible = 0x20;

    enum class Mode { off, race, battle };
    Mode mode = Mode::off;
    uint32_t arrow_polygons[4][max_cars] = {};   // [view][car]: the polygon's address, or 0
    int arrow_texture = -1;                      // texture handle (container << 10 | index) of ARROW; -2: there is none

    float read_f(uint8_t* rdram, uint32_t addr) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    void write_f(uint8_t* rdram, uint32_t addr, float f) {
        uint32_t w;
        memcpy(&w, &f, 4);
        MEM_W(0, (int32_t)addr) = (int32_t)w;
    }

    // The handle of the texture `name` (func_800601F8's), from the last container that has it.
    int find_texture(uint8_t* rdram, const char* name) {
        int tables = MEM_BU(0, (int32_t)texture_table_count);
        for (int t = std::min(tables, 32) - 1; t >= 0; t--) {
            uint32_t records = (uint32_t)MEM_W(0, (int32_t)(texture_tables + t * 8));
            int count = MEM_W(4, (int32_t)(texture_tables + t * 8));
            if (records == 0 || count <= 0 || count > 0x3FF) continue;
            for (int i = 0; i < count; i++) {
                bool same = true;
                for (int c = 0; c < 16 && same; c++) {
                    char have = (char)MEM_B(c, (int32_t)(records + i * 0x20));
                    same = have == name[c];
                    if (name[c] == 0) break;
                }
                if (same) return (t << 10) | i;
            }
        }
        return -2;
    }

    // Whether texture handle `texture` is an image called `name`.
    bool texture_named(uint8_t* rdram, int texture, const char* name) {
        int table = texture >> 10, tables = MEM_BU(0, (int32_t)texture_table_count);
        if (texture < 0 || table >= tables || table >= 32) return false;
        uint32_t records = (uint32_t)MEM_W(0, (int32_t)(texture_tables + table * 8));
        if (records == 0 || (texture & 0x3FF) >= MEM_W(4, (int32_t)(texture_tables + table * 8))) return false;
        for (int c = 0; c < 16; c++) {
            if ((char)MEM_B(c, (int32_t)(records + (texture & 0x3FF) * 0x20)) != name[c]) return false;
            if (name[c] == 0) break;
        }
        return true;
    }

    bool arrow_valid(uint8_t* rdram, uint32_t poly) {
        return poly != 0 && MEM_H(0, (int32_t)poly) == 4 && MEM_H(4, (int32_t)poly) == (int16_t)arrow_texture &&
               MEM_H(6, (int32_t)poly) == (mode == Mode::battle ? battle_depth : race_mark);
    }

    // Rush 2's own arrows: every polygon with an ARROW image that isn't one of these.
    void hide_game_arrows(uint8_t* rdram) {
        int used = std::min<int>(MEM_W(0, (int32_t)polygon_count), polygon_limit);
        for (int slot = 0; slot < used; slot++) {
            uint32_t poly = polygons + slot * polygon_size;
            uint16_t flags = MEM_HU(2, (int32_t)poly);
            if (MEM_H(0, (int32_t)poly) != 4 || (flags & polygon_hidden) != 0 || rush2::arrows::single_view(poly)) continue;
            if (texture_named(rdram, MEM_H(4, (int32_t)poly), "ARROW")) MEM_H(2, (int32_t)poly) = (int16_t)(flags | polygon_hidden);
        }
    }

    // func_80054010: the first free polygon (vertex count 0), hidden until it is placed.
    uint32_t make_arrow(uint8_t* rdram, int view) {
        int used = MEM_W(0, (int32_t)polygon_count), slot = 0;
        while (slot < used && MEM_H(0, (int32_t)(polygons + slot * polygon_size)) != 0) slot++;
        if (slot >= polygon_limit) return 0;
        if (slot >= used) MEM_W(0, (int32_t)polygon_count) = slot + 1;
        if (MEM_W(0, (int32_t)polygon_high) < slot + 1) MEM_W(0, (int32_t)polygon_high) = slot + 1;
        uint32_t poly = polygons + slot * polygon_size;
        for (int i = 0; i < polygon_size; i += 4) MEM_W(i, (int32_t)poly) = 0;
        bool battle = mode == Mode::battle;
        MEM_H(0, (int32_t)poly) = 4;
        MEM_H(2, (int32_t)poly) = (int16_t)((battle ? battle_flags : race_flags) | polygon_hidden | (1 << view));
        MEM_H(4, (int32_t)poly) = (int16_t)arrow_texture;
        MEM_H(6, (int32_t)poly) = battle ? battle_depth : race_mark;
        // func_80053D28's texture coordinates for the image: its last row at the first two vertices.
        uint32_t record = (uint32_t)MEM_W(0, (int32_t)(texture_tables + (arrow_texture >> 10) * 8)) + (arrow_texture & 0x3FF) * 0x20;
        int w = MEM_HU(0x10, (int32_t)record), h = MEM_HU(0x12, (int32_t)record);
        const int16_t u[4] = { int16_t((w << 6) - 16), int16_t((w << 5) - 16), int16_t((w << 5) - 16), int16_t((w << 6) - 16) };
        const int16_t t[4] = { int16_t((h << 6) - 16), int16_t((h << 6) - 16), int16_t((h << 5) - 16), int16_t((h << 5) - 16) };
        for (int i = 0; i < 4; i++) {
            MEM_H(0x14 + i * 0x14, (int32_t)poly) = u[i];
            MEM_H(0x16 + i * 0x14, (int32_t)poly) = t[i];
        }
        return poly;
    }

    void release(uint8_t* rdram) {
        for (auto& view : arrow_polygons) {
            for (uint32_t& poly : view) {
                if (arrow_texture >= 0 && arrow_valid(rdram, poly)) MEM_H(0, (int32_t)poly) = 0;
                poly = 0;
            }
        }
        arrow_texture = -1;
        mode = Mode::off;
    }

    // func_80059B9C / 2049 func_8008C884 for view p.
    void update(uint8_t* rdram, int p, int players) {
        using namespace rush2::views;
        if (arrow_texture == -1) {
            arrow_texture = find_texture(rdram, "ARROW");
            if (arrow_texture < 0) fprintf(stderr, "[Arrows] No ARROW image is loaded\n");
        }
        hide_game_arrows(rdram);
        if (arrow_texture < 0) return;
        bool battle = mode == Mode::battle;
        View v = view_of(rdram, p, players);
        int own = p < players ? player_car(rdram, p) : -1;
        float ax[9], cam[3], tan_x, tan_y;
        axes(rdram, p, ax);
        eye(rdram, p, cam);
        tangents(rdram, p, v, tan_x, tan_y);
        // 2049's views are a quarter turn wide; a view's 4:3 part here is as wide as its height makes it.
        float tan_x43 = tan_y * (float)(v.x1 - v.x0) / (float)(v.y1 - v.y0);
        float vfov = 2.0f * std::atan(tan_y);
        for (int car = 0; car < max_cars; car++) {
            uint32_t& poly = arrow_polygons[p][car];
            int player = -1;
            for (int q = 0; q < players; q++) {
                if (player_car(rdram, q) == car) player = q;
            }
            bool faded = false;
            // A battle: every live car. A race: the other players' cars, as Rush 2.
            bool show = own >= 0 && car != own && (battle ? rush2::battle::arrow_target(rdram, car, faded) : player >= 0);
            if (!arrow_valid(rdram, poly)) poly = show ? make_arrow(rdram, p) : 0;
            if (poly == 0) continue;
            uint16_t flags = uint16_t((battle ? battle_flags : race_flags) | (1 << p));
            if (!show) {
                MEM_H(2, (int32_t)poly) = (int16_t)(flags | polygon_hidden);
                continue;
            }
            float target[3], d[3], local[3];
            for (int i = 0; i < 3; i++) {
                target[i] = read_f(rdram, car_states + (uint32_t)car * car_state_size + i * 4);
                d[i] = target[i] - cam[i];
            }
            for (int k = 0; k < 3; k++) local[k] = ax[k * 3] * d[0] + ax[k * 3 + 1] * d[1] + ax[k * 3 + 2] * d[2];
            float a = std::atan2(local[0], local[2]);
            float center[3], half;
            int turn = 0;   // The image's quarter turns: 0 points down, 1 left, 3 right.
            if (!battle || std::fabs(a) < arrow_in_view * 2.0f * std::atan(tan_x)) {
                half = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * arrow_hover_scale;
                for (int i = 0; i < 3; i++) center[i] = target[i];
                center[1] += half + arrow_hover;
            }
            else {
                half = arrow_size * tan_x43 * arrow_distance;
                float edge_x = tan_x * arrow_distance - half;
                // Kept whole inside the view [I]: 2049's bottom arrows of two stacked views are half below it.
                float top = tan_y * arrow_distance - half;
                float edge_y = std::min(arrow_bottom * vfov * arrow_distance, top);
                float x, y;
                if (std::fabs(a) <= 1.5707964f) {
                    float start = arrow_side_start * vfov;
                    x = a > 0.0f ? edge_x : -edge_x;
                    y = std::clamp(-edge_y * (std::fabs(a) - start) / (1.5707964f - start), -edge_y, top);
                    turn = a > 0.0f ? 3 : 1;
                }
                else {
                    float behind = a > 0.0f ? a - 3.1415927f : a + 3.1415927f;
                    x = -edge_x * behind / 1.5707964f;
                    y = -edge_y;
                }
                for (int i = 0; i < 3; i++) center[i] = cam[i] + ax[6 + i] * arrow_distance + ax[i] * x + ax[3 + i] * y;
            }
            uint32_t color;
            if (battle) {
                color = battle_colors[player >= 0 ? rush2::battle::team_of(player) : 4];
                if (faded) color = (color & 0xFFFFFF00) | arrow_alpha_invisible;
            }
            else {
                int paint = MEM_BU(0, (int32_t)(cars + (uint32_t)car * car_size + car_paint));
                color = (uint32_t)MEM_W(0, (int32_t)(paint_colors + paint * 4));
            }
            // The corners (1, 1), (-1, 1), (-1, -1), (1, -1) across and up the view, turned with the image.
            static const float corner[4][2] = { { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 } };
            for (int i = 0; i < 4; i++) {
                const float* c = corner[(i - turn) & 3];
                uint32_t vertex = poly + 8 + i * 0x14;
                for (int k = 0; k < 3; k++) {
                    write_f(rdram, vertex + k * 4, (center[k] + (ax[k] * c[0] + ax[3 + k] * c[1]) * half) * 16.0f);
                }
                MEM_W(0x10, (int32_t)vertex) = (int32_t)color;
            }
            MEM_H(2, (int32_t)poly) = (int16_t)flags;
        }
    }
}

rush2::views::View rush2::views::view_of(uint8_t* rdram, int player, int players) {
    View v{ 0, 0, 320, 240, false, false, false };
    if (players <= 1) {
    }
    else if (int quad = rush2::splitscreen::quadrant_views(rdram); quad != 0 || players > 2) {
        int x = (player & 1) * 160, y = (player >> 1) * 120;
        v = { x, y, x + 160, y + 120, (player & 1) != 0, player >= 2, false };
    }
    else if (rush2::splitscreen::is_side_by_side(rdram)) {
        v = { player * 160, 0, player * 160 + 160, 240, player == 1, false, false };
    }
    else {
        v = { 0, player * 120, 320, player * 120 + 120, false, player == 1, true };
    }
    float margin = std::max(0.0f, (rush2::splitscreen::window_width() - 320.0f) * 0.5f);
    v.left = v.x0 == 0 ? -margin : (float)v.x0;
    v.right_edge = v.x1 == 320 ? 320.0f + margin : (float)v.x1;
    return v;
}

int rush2::views::local_players(uint8_t* rdram) {
    return std::clamp<int>((int16_t)MEM_H(0, (int32_t)num_players), 1, 4);
}

int rush2::views::player_car(uint8_t* rdram, int player) {
    return (int8_t)MEM_B(0, (int32_t)(player_slots + player * player_size));
}

int rush2::views::view_of_camera(uint8_t* rdram, uint32_t at) {
    if (at >= cameras && at < cameras + 4 * 0x40) {
        return (int)((at - cameras) / 0x40);
    }
    // A copy of the position: the view whose camera is there.
    for (int p = 0; p < 4; p++) {
        bool same = true;
        for (int i = 0; i < 3; i++) {
            same = same && MEM_W(i * 4, (int32_t)at) == MEM_W(0x24 + i * 4, (int32_t)(cameras + (uint32_t)p * 0x40));
        }
        if (same) return p;
    }
    return -1;
}

void rush2::views::axes(uint8_t* rdram, int p, float out[9]) {
    uint32_t cam = cameras + (uint32_t)p * 0x40;
    for (int k = 0; k < 9; k++) out[k] = read_f(rdram, cam + k * 4);
    if (MEM_B(0, (int32_t)view_tilt) == 0) return;
    float angle = read_f(rdram, view_tilt_angle), c = std::cos(angle), s = std::sin(angle);
    for (int i = 0; i < 3; i++) {
        float x = out[i], y = out[3 + i];
        out[i] = x * c - y * s;
        out[3 + i] = x * s + y * c;
    }
}

void rush2::views::eye(uint8_t* rdram, int p, float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = read_f(rdram, cameras + (uint32_t)p * 0x40 + 0x24 + i * 4);
}

void rush2::views::tangents(uint8_t* rdram, int p, const View& v, float& tan_x, float& tan_y) {
    tan_y = rush2::splitscreen::view_tan_v(rdram, p);
    if (!(tan_y > 0.05f && tan_y < 4.0f)) tan_y = v.wide ? 0.375f : 0.75f;
    tan_x = tan_y * (v.right_edge - v.left) / (float)(v.y1 - v.y0);
}

bool rush2::arrows::single_view(uint32_t polygon) {
    for (const auto& view : arrow_polygons) {
        for (uint32_t poly : view) {
            if (poly != 0 && poly == polygon) return true;
        }
    }
    return false;
}

// func_8007C27C entry ($a1 = the camera position of the view about to be drawn): places that view's arrows.
extern "C" void rush2_arrows_view(uint8_t* rdram, recomp_context* ctx) {
    int state = MEM_W(0, (int32_t)game_state);
    int players = rush2::views::local_players(rdram);
    // In a race or its countdown (state 10), as Rush 2 shows its own arrows.
    bool racing = state == 3 || state == 10;
    Mode want = !racing ? Mode::off : rush2::battle::active(rdram) ? Mode::battle : players >= 3 ? Mode::race : Mode::off;
    if (want != mode) {
        release(rdram);
        mode = want;
    }
    if (mode == Mode::off) return;
    int view = rush2::views::view_of_camera(rdram, (uint32_t)ctx->r5);
    if (view >= 0 && view < 4) update(rdram, view, players);
}
