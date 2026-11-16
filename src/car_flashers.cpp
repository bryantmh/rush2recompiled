// Roof flashers on mod cars (include/car_flashers.h, docs/car_mods.md#flashers). A new car's .json "flashers" (the
// PlayStation SF Rush police car's two roof lights, src/sfrush_ps1/ps1_cars.cpp) are glows that pulse while the car
// races: each brightens from dark to full over the car's "flash_period" and then goes dark at once, offset by its
// "phase", as the PlayStation draws its police car's lights (docs/sfrush_ps1.md#police-lights: a glow sprite whose
// brightness is (frame & 31) x 8, the second light half a cycle behind).
//
// Each glow is a rush2::battle_render model of the port's own (FLASH_GLOW: a flare like the PlayStation's ART/LIGHT.GFX,
// a bright core with a ray across and a ray up, flat in the car's x-y plane as the PlayStation's sprite is (a square
// 3.2 ft across at x +-16, y +-16 round its point, func_80041C58), vertex alpha from the middle out, colored and dimmed
// by its slot's PRIM, unlit, unfogged, blended without writing depth), placed with the car's body pose before each
// view (pvs_apply 0x8007C27C, as src/car_lights.cpp does), pulled a little toward the camera so the roof it sits on
// doesn't cut it. Slots 420-451: car c's flasher k at first_slot + c x 4 + k.
// Hidden for a wrecked car, a car out of the race or not drawn (car +0x7F4 bit 0x10, after a wreck) and a ghost.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>

#include "battle.h"
#include "battle_render.h"
#include "car_flashers.h"
#include "car_types.h"
#include "ghost.h"
#include "own_model_dl.h"
#include "rush2_hooks.h"

namespace ct = rush2::car_types;

namespace {
    constexpr float pi = 3.14159265358979f;
    constexpr int max_cars = 8;
    constexpr int per_car = rush2::car_flashers::max_flashers;
    constexpr int first_slot = 420;   // to 451 (battle_render.h)
    static_assert(first_slot + max_cars * per_car <= rush2::battle_render::max_slots);

    // The glow's model, in spare RDRAM below 16 MB (the list in src/controls_menu.cpp): 0x80E1F480-0x80E1F800.
    constexpr uint32_t glow_vertices = 0x80E1F480;   // 25 x 16 = 0x190
    constexpr uint32_t glow_dl = 0x80E1F610;         // to 0x80E1F800
    constexpr const char* glow_model = "FLASH_GLOW";
    // How far the glow is pulled toward each view's camera (feet), off the roof it sits on.
    constexpr float glow_pull = 0.6f;

    constexpr uint32_t game_state = 0x8010C0D0;
    constexpr uint32_t cars = 0x800F5470, car_size = 0x81C;
    constexpr uint32_t car_type = 0x7EA;     // u8
    constexpr uint32_t car_wrecked = 0x648;  // s8
    constexpr uint32_t car_active = 0x7E4;   // s16, nonzero for a car in the race
    constexpr uint32_t car_flags = 0x7F4;    // 0x10: the car isn't drawn (after a wreck, until its reset)

    std::mutex specs_mutex;
    std::map<int, rush2::car_flashers::Spec> specs;   // by car type
    bool model_written = false;
    bool shown[max_cars][per_car] = {};

    // The flare, 16 (1 ft) to each side of the middle in its x-y plane, as ART/LIGHT.GFX draws it (32 x 32 texels: a
    // core of full brightness three texels round, fading out by eleven, and a ray each way across and up to the edge):
    // the middle at alpha 255, a ring of radius 4 at 170, one of radius 10 at 0 (8 segments), and two thin diamonds
    // from tip to tip (alpha 0 at the tips, 150 a texel and a half off the middle). Vertices: 0 the middle, 1-8 and
    // 9-16 the rings, 17-20 the ray across (left, top, right, bottom), 21-24 the ray up (bottom, right, top, left).
    void write_glow(uint8_t* rdram) {
        constexpr int segments = 8;
        auto vertex = [&](int i, float x, float y, uint8_t alpha) {
            rush2::own_model::put_vertex(rdram, glow_vertices + uint32_t(i) * 16, x, y, 0.0f, alpha);
        };
        vertex(0, 0.0f, 0.0f, 255);
        for (int i = 0; i < segments; i++) {
            float a = 2.0f * pi * (float(i) + 0.5f) / float(segments);
            vertex(1 + i, 4.0f * std::cos(a), 4.0f * std::sin(a), 170);
            vertex(1 + segments + i, 10.0f * std::cos(a), 10.0f * std::sin(a), 0);
        }
        vertex(17, -16.0f, 0.0f, 0);
        vertex(18, 0.0f, 1.5f, 150);
        vertex(19, 16.0f, 0.0f, 0);
        vertex(20, 0.0f, -1.5f, 150);
        vertex(21, 0.0f, -16.0f, 0);
        vertex(22, 1.5f, 0.0f, 150);
        vertex(23, 0.0f, 16.0f, 0);
        vertex(24, -1.5f, 0.0f, 150);
        rush2::own_model::Dl dl{ rdram, glow_dl };
        dl.begin();
        dl.vtx(glow_vertices, 25);
        for (int i = 0; i < segments; i += 2) {
            dl.tri2(0, 1 + i, 1 + (i + 1) % segments, 0, 1 + (i + 1) % segments, 1 + (i + 2) % segments);
        }
        for (int i = 0; i < segments; i++) {
            int j = (i + 1) % segments;
            dl.tri2(1 + i, 1 + segments + i, 1 + segments + j, 1 + i, 1 + segments + j, 1 + j);
        }
        dl.tri2(17, 18, 20, 19, 20, 18);
        dl.tri2(21, 22, 24, 23, 24, 22);
        dl.end();
    }

    void hide_car(int car) {
        for (int k = 0; k < per_car; k++) {
            if (!shown[car][k]) continue;
            shown[car][k] = false;
            rush2::battle_render::hide(first_slot + car * per_car + k);
        }
    }

    float seconds() {
        using clock = std::chrono::steady_clock;
        static const clock::time_point start = clock::now();
        return std::chrono::duration<float>(clock::now() - start).count();
    }
}

void rush2::car_flashers::set_mod_flashers(int type, const Spec& spec) {
    std::lock_guard lock{ specs_mutex };
    if (spec.flashers.empty()) specs.erase(type);
    else specs[type] = spec;
}

// pvs_apply 0x8007C27C, before each view: the flashers of every race car that has them.
extern "C" void rush2_car_flashers_view(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    bool racing = (int32_t)MEM_W(0, (int32_t)game_state) >= 3;
    if (!model_written) {
        write_glow(rdram);
        rush2::battle_render::add_model(glow_model, glow_dl);
        model_written = true;
    }
    const float now = seconds();
    std::lock_guard lock{ specs_mutex };
    for (int i = 0; i < max_cars; i++) {
        const uint32_t car = cars + uint32_t(i) * car_size;
        int type = MEM_BU(0, (int32_t)(car + car_type));
        auto it = specs.find(type);
        if (!racing || it == specs.end() || MEM_H(0, (int32_t)(car + car_active)) == 0 ||
            MEM_B(0, (int32_t)(car + car_wrecked)) != 0 || (MEM_W(0, (int32_t)(car + car_flags)) & 0x10) != 0 ||
            rush2::ghost::is_ghost_car(i)) {
            hide_car(i);
            continue;
        }
        const rush2::car_flashers::Spec& spec = it->second;
        float pos[3], m[9];
        rush2::battle::car_body_pose(rdram, i, pos, m);
        for (int k = 0; k < per_car; k++) {
            const int slot = first_slot + i * per_car + k;
            if (k >= (int)spec.flashers.size()) {
                if (shown[i][k]) rush2::battle_render::hide(slot);
                shown[i][k] = false;
                continue;
            }
            const rush2::car_flashers::Flasher& f = spec.flashers[size_t(k)];
            float at[3];
            for (int c = 0; c < 3; c++) at[c] = pos[c] + m[c] * f.at[0] + m[3 + c] * f.at[1] + m[6 + c] * f.at[2];
            // The cycle: dark to full, then dark at once (the PlayStation's (frame & 31) x 8 of 255).
            float level = std::fmod(now / std::max(spec.period, 0.05f) + f.phase, 1.0f) * (248.0f / 255.0f);
            uint32_t rgba = uint32_t(std::clamp(f.rgb[0], 0.0f, 255.0f)) << 24 | uint32_t(std::clamp(f.rgb[1], 0.0f, 255.0f)) << 16 |
                            uint32_t(std::clamp(f.rgb[2], 0.0f, 255.0f)) << 8 | uint32_t(std::clamp(level, 0.0f, 1.0f) * 255.0f);
            // Flat in the car's x-y plane, as the PlayStation's sprite.
            float gm[9];
            for (int c = 0; c < 9; c++) gm[c] = m[c] * f.radius;
            rush2::battle_render::place(slot, glow_model, gm, at, rgba, -1, false, false, -glow_pull);
            shown[i][k] = true;
        }
    }
}
