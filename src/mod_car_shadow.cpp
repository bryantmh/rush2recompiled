// Round shadows for mod cars that ask for one ("round_shadow": a radius in feet, docs/car_mods.md#new-cars), such as
// the PlayStation SF Rush UFO: a saucer whose own shadow should be a disc its size, not the box between its wheels.
//
// Rush 2 gives each race car one shadow: a world polygon (car state +0x20C, include/world_polygons.h) with the
// SHADBLUR image, a white rounded rectangle with soft edges drawn in the shadow color. Every frame
// car_model_update_parts_80059D24 puts its four corners at the car's wheel points, dropped to the ground under each
// (polygon_set_80053D28, corner w at the car record's wheel point w; w 0 and 1 the front pair), so it lies on the
// road, slopes and all, between the wheels.
//
// After that (hook at 0x8005A270, its end) a round-shadow car's polygon is redrawn as a square reaching `radius` to
// each side of the corners' middle, in the plane they make, and two copies of it turned 30 and 60 degrees are drawn with it: three
// soft rounded squares whose union is a twelve-sided disc with a soft rim. Each is drawn at the opacity that makes
// the three together, where they all overlap, as dark as the game's single shadow. The square keeps the order of the
// game's corners (which corner is ahead and which to the right), so it is the same square however often it is
// redone, and the image maps onto it as onto the game's.
//
// The copies are world polygons of their own, taken as src/arrows.cpp takes its arrows (the first free slot, the
// count raised). Each frame they are checked: still in use, still the shadow image and never one of the cars' own
// shadows. A race's setup clears the table (scene_lights_reset_8007FAB4 sets its count to 0), which ends them; they
// are made again as needed.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "recomp.h"
#include "car2049.h"
#include "world_polygons.h"

namespace {
    constexpr uint32_t physics_cars = 0x800F5470;   // 0x81C each: +0x7EA the car type
    constexpr uint32_t physics_size = 0x81C;
    constexpr uint32_t car_states = 0x801124A0;     // 0x354 each: +0x20C the shadow polygon
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t state_shadow = 0x20C;
    constexpr int max_cars = 8;
    constexpr int copies = 2;                       // turned 30 and 60 degrees
    constexpr uint32_t polygons = rush2::world_polygons::table;
    constexpr uint32_t polygon_size = rush2::world_polygons::size;
    constexpr int vertex = 0x14;                    // +8 + i * 0x14: position (3 f32, feet x 16), u, t (s16), color

    uint32_t extra[max_cars][copies] = {};
    uint32_t written[max_cars][4] = {}, game_color[max_cars][4] = {};   // a car's corner colors: as written, the game's

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

    uint32_t shadow_of(uint8_t* rdram, int car) {
        uint32_t p = (uint32_t)MEM_W(0, (int32_t)(car_states + uint32_t(car) * car_state_size + state_shadow));
        return p >= polygons && p < rush2::world_polygons::table_end ? p : 0;
    }

    bool is_car_shadow(uint8_t* rdram, uint32_t poly) {
        for (int car = 0; car < max_cars; car++) {
            if (shadow_of(rdram, car) == poly) return true;
        }
        return false;
    }

    // A copy is still ours: in use, inside the table's count, the shadow image and not a car's own shadow.
    bool valid(uint8_t* rdram, uint32_t poly, int16_t texture) {
        if (poly < polygons || poly >= rush2::world_polygons::table_end) return false;
        int used = MEM_W(0, (int32_t)rush2::world_polygons::count);
        if (int((poly - polygons) / polygon_size) >= used) return false;
        return MEM_H(0, (int32_t)poly) == 4 && MEM_H(4, (int32_t)poly) == texture && !is_car_shadow(rdram, poly);
    }

    // The first free world polygon (polygon_create_80054010's way), 0 when the table is full.
    uint32_t take(uint8_t* rdram) {
        int used = MEM_W(0, (int32_t)rush2::world_polygons::count), slot = 0;
        while (slot < used && MEM_H(0, (int32_t)(polygons + uint32_t(slot) * polygon_size)) != 0) slot++;
        if (slot >= rush2::world_polygons::capacity) return 0;
        if (slot >= used) MEM_W(0, (int32_t)rush2::world_polygons::count) = slot + 1;
        if (MEM_W(0, (int32_t)rush2::world_polygons::high) < slot + 1) MEM_W(0, (int32_t)rush2::world_polygons::high) = slot + 1;
        uint32_t poly = polygons + uint32_t(slot) * polygon_size;
        for (uint32_t i = 0; i < polygon_size; i += 4) MEM_W(int32_t(i), (int32_t)poly) = 0;
        return poly;
    }

    void hide(uint8_t* rdram, int car, int16_t texture) {
        for (uint32_t& poly : extra[car]) {
            if (poly != 0 && valid(rdram, poly, texture)) MEM_H(2, (int32_t)poly) = (int16_t)(MEM_HU(2, (int32_t)poly) | 0x8000);
        }
    }

    void round_shadow(uint8_t* rdram, int car, float radius_ft) {
        uint32_t shadow = shadow_of(rdram, car);
        if (shadow == 0 || MEM_H(0, (int32_t)shadow) != 4) return;
        const int16_t texture = MEM_H(4, (int32_t)shadow);
        float p[4][3];
        for (int i = 0; i < 4; i++) {
            for (int k = 0; k < 3; k++) p[i][k] = read_f(rdram, shadow + 8 + uint32_t(i * vertex + k * 4));
        }
        // The corners' middle, ahead (the front pair, corners 0 and 1, from the rear pair) and across (corner 0's side).
        float c[3], f[3], r[3];
        for (int k = 0; k < 3; k++) {
            c[k] = (p[0][k] + p[1][k] + p[2][k] + p[3][k]) * 0.25f;
            f[k] = (p[0][k] + p[1][k]) - (p[2][k] + p[3][k]);
            r[k] = (p[0][k] + p[3][k]) - (p[1][k] + p[2][k]);
        }
        auto unit = [](float* v) {
            float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (n < 1e-6f) return false;
            for (int k = 0; k < 3; k++) v[k] /= n;
            return true;
        };
        if (!unit(f) || !unit(r)) return;
        // Each corner's side of the middle, ahead and across (+1 or -1), kept so the square's corners go round as the
        // game's do.
        float sf[4], sr[4];
        for (int i = 0; i < 4; i++) {
            float a = 0, b = 0;
            for (int k = 0; k < 3; k++) a += (p[i][k] - c[k]) * f[k], b += (p[i][k] - c[k]) * r[k];
            sf[i] = a >= 0 ? 1.0f : -1.0f;
            sr[i] = b >= 0 ? 1.0f : -1.0f;
        }
        // Half a side: the radius, so the disc's inner edge is the car's own (the rounded corners reach a little past).
        const float half = radius_ft * 16.0f;
        // Three layers at alpha a' with 1 - (1 - a')^3 = a, the game's alpha, where they all overlap. The hook runs
        // after every car's update, so a color still as written here last time stands for the game's color it came
        // from (taking a' of a' again would fade the shadow away within a few frames).
        uint32_t colors[4];
        for (int i = 0; i < 4; i++) {
            uint32_t col = (uint32_t)MEM_W(0x10, (int32_t)(shadow + 8 + uint32_t(i * vertex)));
            if (col == written[car][i]) col = game_color[car][i];
            game_color[car][i] = col;
            float a = float(col & 0xFF) / 255.0f;
            float layer = 1.0f - std::cbrt(std::max(0.0f, 1.0f - a));
            colors[i] = (col & 0xFFFFFF00u) | uint32_t(std::clamp(std::lround(layer * 255.0f), 0L, 255L));
            written[car][i] = colors[i];
        }
        const uint16_t flags = MEM_HU(2, (int32_t)shadow);
        for (int layer = 0; layer <= copies; layer++) {
            uint32_t poly = shadow;
            if (layer > 0) {
                uint32_t& slot = extra[car][layer - 1];
                if (slot == 0 || !valid(rdram, slot, texture)) slot = take(rdram);
                if (slot == 0) continue;
                poly = slot;
                // The game's polygon's header and image coordinates.
                MEM_H(0, (int32_t)poly) = 4;
                MEM_H(4, (int32_t)poly) = texture;
                MEM_H(6, (int32_t)poly) = MEM_H(6, (int32_t)shadow);
                for (int i = 0; i < 4; i++) {
                    uint32_t from = shadow + 8 + uint32_t(i * vertex), to = poly + 8 + uint32_t(i * vertex);
                    MEM_W(0xC, (int32_t)to) = MEM_W(0xC, (int32_t)from);
                }
            }
            const float turn = float(layer) * 3.14159265f / 6.0f, cs = std::cos(turn), sn = std::sin(turn);
            for (int i = 0; i < 4; i++) {
                float a = sf[i] * half, b = sr[i] * half;
                float ta = a * cs - b * sn, tb = a * sn + b * cs;
                uint32_t v = poly + 8 + uint32_t(i * vertex);
                for (int k = 0; k < 3; k++) write_f(rdram, v + uint32_t(k * 4), c[k] + f[k] * ta + r[k] * tb);
                MEM_W(0x10, (int32_t)v) = (int32_t)colors[i];
            }
            MEM_H(2, (int32_t)poly) = (int16_t)flags;   // the game's views, and hidden when it hides the shadow
        }
    }
}

// car_model_update_parts_80059D24 at 0x8005A270 (its end, after the shadow polygon is set): every race car with a
// round shadow gets it.
extern "C" void rush2_mod_car_shadows(uint8_t* rdram) {
    for (int car = 0; car < max_cars; car++) {
        int type = MEM_BU(0x7EA, (int32_t)(physics_cars + uint32_t(car) * physics_size));
        float radius = rush2::car2049::mod_car_shadow_radius(type);
        if (radius > 0.0f) {
            round_shadow(rdram, car, radius);
        }
        else {
            uint32_t shadow = shadow_of(rdram, car);
            if (shadow != 0) hide(rdram, car, MEM_H(4, (int32_t)shadow));
        }
    }
}
