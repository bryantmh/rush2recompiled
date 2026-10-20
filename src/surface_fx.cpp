// The wheel surface effects of another game's tracks raced in Rush 2 (include/surface_fx.h,
// docs/calspeed_research.md#4-surface-types).
//
// Rush 2 keeps the wheel material of the polygon under each wheel at car +0x60C + 2 x wheel (the wheel probe
// collision_poly_extract_8006F704 stores it at 0x8006FA90) and acts on it in four places:
// - car_material_effects_update_800663CC: the dust bits (any of wheels 0, 2 on 1 or 2) and the skid sounds (off on 1,
//   2 and 8);
// - particle_emitter_spawn_8008270C: the particle a dusty wheel throws (0x800828EC: 1 DUST; 0x8008292C: 2 SPLASH; else
//   SMOKE or SMOKE_FAST by speed);
// - the tire marks (tire_mark_update_800823B0 decides, at 0x80082404: material 0 only; tire_mark_start_800821A4 starts a
//   mark polygon in color 0x800C5634 (black, alpha 0xC0) with SKID04; tire_mark_extend_80081FD4 extends it);
// - car_stunt_scoring_update_80065588 and car_sound_update_all_80067358 (8, airborne).
// While a converted track with its own game's rules is raced, the probe hook keeps the track's material per wheel
// and stores the Rush 2 material it acts like, and the hooks below give it its own game's particle, marks and spray.
//
// California Speed (its effects code: 0x8007ED84 the dust bits and skid sounds, 0x800BCF9C the wheel particles,
// 0x800BD4E4 the tire marks, 0x800BEF14 the water spray, 0x800BA8C4 the car's nodes):
// - dust: materials 1, 2, 3, 5, 8, 9 (Rush 2: 1, 2); skid sounds off on those and 10;
// - particles: 1 DUST, 2 none (the spray instead), anything else SMOKE / SMOKE_FAST by speed. Its DUST and SMOKE models
//   and textures are Rush 2's;
// - tire marks: on material 0 (black, as Rush 2), 5 and 8 (dirt: 0x80764C) and 3 and 9 (grass: 0x3A6332), alpha 0xC0,
//   with its own SKID04;
// - water spray: a node per wheel under the car's body (SPLASHRIGHT_ for wheels 0 and 2, SPLASHLEFT_ for 1 and 3,
//   primitive color 0xFFFFFFC8, 0xA0A0A0C8 on tracks 14 and 15), shown while the wheel is on material 2 and the
//   driven wheels turn forward (+0x438, Rush 2's +0x3D4): scaled by min(speed / 40 + 0.5, 2.5) and placed at the wheel
//   point (car state +0xC0, Rush 2's +0xA8) + (0, r - 0.835 x scale, 0.9 - r), r the car's front or rear wheel scale;
//   both models draw frame (game frame & 3) of SPLASH01-04 (the LOD's texture handle, flag 0x8000).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "recomp.h"
#include "calspeed.h"
#include "car_types.h"
#include "surface_fx.h"

extern "C" void scene_matrix_create_8007F91C(uint8_t* rdram, recomp_context* ctx);
extern "C" void scene_node_create_8007F6DC(uint8_t* rdram, recomp_context* ctx);
extern "C" void node_set_pos_matrix_8005A2D8(uint8_t* rdram, recomp_context* ctx);
extern "C" void model_find_by_name_8005BE3C(uint8_t* rdram, recomp_context* ctx);
extern "C" void texture_find_by_name_800601F8(uint8_t* rdram, recomp_context* ctx);

namespace {
    using rush2::surface_fx::Material;
    using rush2::surface_fx::Particle;
    using rush2::surface_fx::Rules;
    namespace ct = rush2::car_types;

    constexpr uint32_t physics_cars = 0x800F5470, physics_car_size = 0x81C;
    constexpr int32_t wheel_material = 0x60C;      // u16 per wheel (signed: an unsigned offset misaddresses MEM_*)
    constexpr uint32_t wheel_speed = 0x3D4;        // f32, the driven wheels' speed (rad/s)
    constexpr uint32_t car_type = 0x7EA;           // u8
    constexpr uint32_t draw_state_size = 0x134;
    constexpr uint32_t wheel_points = 0xA8;        // in a draw state: 4 x {x, y, z} feet, car frame
    constexpr uint32_t mark_color = 0x800C5634;    // RGBA of the tire marks' polygons
    constexpr uint32_t nodes = 0x800D9E90, node_size = 0x38;
    constexpr uint32_t node_count = 0x800FAE58;
    constexpr uint32_t node_hidden = 0x400, node_prim = 0x2000;
    constexpr uint32_t model_slots = 0x80118D78;   // per model slot: its records (8 bytes each)
    constexpr uint32_t model_record_size = 0x34;   // u32 LOD count, then {u16 texture, u16 flags, f32, u32 list}[4]
    constexpr uint32_t model_container_count = 0x800D5788;
    constexpr uint32_t game_frame_count = 0x8011962C;
    constexpr uint32_t race_car_count = 0x800E79B8;
    constexpr int max_cars = 8;

    Rules calspeed_rules(int t) {
        Rules r;
        for (Material& m : r.materials) {
            m.particle = Particle::Smoke;
        }
        r.materials[0] = { 0, Particle::Smoke, true, 0x000000, false };
        r.materials[1] = { 1, Particle::Dust, false, 0, false };
        r.materials[2] = { 2, Particle::None, false, 0, true };
        for (int m : { 5, 8 }) r.materials[m] = { 1, Particle::Smoke, true, 0x80764C, false };
        for (int m : { 3, 9 }) r.materials[m] = { 1, Particle::Smoke, true, 0x3A6332, false };
        r.mark_texture = "CSSKID04";
        r.spray_models[0] = "SPLASHRIGHT_";
        r.spray_models[1] = "SPLASHLEFT_";
        r.spray_frames[0] = "CSSPLASH01";
        r.spray_frames[1] = "CSSPLASH02";
        r.spray_frames[2] = "CSSPLASH03";
        r.spray_frames[3] = "CSSPLASH04";
        r.spray_rgba = (t == 14 || t == 15) ? 0xA0A0A0C8 : 0xFFFFFFC8;
        return r;
    }

    Rules rules_now;
    int rules_track = -1;

    // The rules of the track being raced, or null for Rush 2's own.
    const Rules* active(uint8_t* rdram) {
        if (!rush2::calspeed::racing(rdram)) {
            return nullptr;
        }
        int t = rush2::calspeed::race_track() - 1;
        if (t != rules_track) {
            rules_now = calspeed_rules(t);
            rules_track = t;
        }
        return &rules_now;
    }

    std::unordered_map<uint32_t, uint8_t> wheel_source;   // car + 2 x wheel -> the track's material under it
    std::unordered_map<uint32_t, uint32_t> mark_rgb;      // tire mark object -> its color
    uint32_t saved_mark_rgb = 0;

    // The race's spray nodes and the handles the rules name (-1: missing).
    int16_t spray_nodes[max_cars][4];
    int32_t spray_models[2] = { -1, -1 };
    int16_t spray_frames[4] = { -1, -1, -1, -1 };
    int16_t mark_texture = -1;
    bool ready = false;

    // The track's material under the wheel at `key` (car + 2 x wheel), or -1 when +0x60C no longer holds what the probe
    // stored (the no-polygon fallback and the airborne marker overwrite it).
    int source(uint8_t* rdram, const Rules& rules, uint32_t key) {
        auto it = wheel_source.find(key);
        if (it == wheel_source.end()) return -1;
        if ((MEM_HU(wheel_material, (int32_t)key) & 15) != rules.materials[it->second].like) return -1;
        return it->second;
    }

    float f32_at(uint8_t* rdram, uint32_t addr) {
        int32_t bits = MEM_W(0, (int32_t)addr);
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }

    void put_f32(uint8_t* rdram, uint32_t addr, float v) {
        int32_t bits;
        std::memcpy(&bits, &v, 4);
        MEM_W(0, (int32_t)addr) = bits;
    }

    int32_t call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0 = 0,
                 int32_t a1 = 0, int32_t a2 = 0, int32_t a3 = 0, int32_t a4 = 0) {
        recomp_context saved = *ctx;
        ctx->r29 = (int32_t)ctx->r29 - 0x20;
        MEM_W(0x10, (int32_t)ctx->r29) = (uint32_t)a4;
        ctx->r4 = a0;
        ctx->r5 = a1;
        ctx->r6 = a2;
        ctx->r7 = a3;
        func(rdram, ctx);
        int32_t ret = (int32_t)ctx->r2;
        *ctx = saved;
        return ret;
    }

    // A name for the lookups, on the stack below the callee's argument area.
    uint32_t put_name(uint8_t* rdram, recomp_context* ctx, const char* name) {
        uint32_t at = (uint32_t)ctx->r29 - 0x60;
        for (size_t i = 0; i <= strlen(name); i++) MEM_B(i, (int32_t)at) = name[i];
        return at;
    }

    int32_t find_model(uint8_t* rdram, recomp_context* ctx, const char* name) {
        int containers = MEM_BU(0, (int32_t)model_container_count);
        uint32_t at = put_name(rdram, ctx, name);
        ctx->r29 = (int32_t)ctx->r29 - 0x80;
        int32_t h = call(rdram, ctx, model_find_by_name_8005BE3C, (int32_t)at, 0, containers - 1, 1);
        ctx->r29 = (int32_t)ctx->r29 + 0x80;
        return h;
    }

    int16_t find_texture(uint8_t* rdram, recomp_context* ctx, const char* name) {
        int containers = MEM_BU(0, (int32_t)model_container_count);
        uint32_t at = put_name(rdram, ctx, name);
        uint32_t out = at - 0x10;
        MEM_H(0, (int32_t)out) = -1;
        ctx->r29 = (int32_t)ctx->r29 - 0x80;
        int32_t record = call(rdram, ctx, texture_find_by_name_800601F8, (int32_t)at, (int32_t)out, 0, containers - 1, 1);
        ctx->r29 = (int32_t)ctx->r29 + 0x80;
        return record != 0 ? MEM_H(0, (int32_t)out) : int16_t(-1);
    }

    uint32_t model_record(uint8_t* rdram, int32_t handle) {
        return (uint32_t)MEM_W(0, (int32_t)(model_slots + uint32_t(handle >> 10) * 8)) +
               uint32_t(handle & 0x3FF) * model_record_size;
    }

    uint32_t node_at(int node) {
        return nodes + uint32_t(node) * node_size;
    }

    // The spray node still holds the spray model (a new race's scene may have reused the index).
    bool spray_node_ok(uint8_t* rdram, int node, int32_t model) {
        return node >= 0 && node < MEM_W(0, (int32_t)node_count) && MEM_H(0xC, (int32_t)node_at(node)) == model;
    }
}

// collision_poly_extract_8006F704 at 0x8006FA90: $t6 = the material about to be stored at +0x60C of $a0 (car + 2 x
// wheel).
extern "C" void rush2_surface_wheel_material(uint8_t* rdram, recomp_context* ctx) {
    const Rules* rules = active(rdram);
    if (rules == nullptr) {
        if (!wheel_source.empty()) wheel_source.clear();
        return;
    }
    int m = (int)ctx->r14 & 15;
    wheel_source[(uint32_t)ctx->r4] = uint8_t(m);
    ctx->r14 = rules->materials[m].like;
}

int rush2::surface_fx::source_material(uint32_t car, int wheel) {
    auto it = wheel_source.find(car + 2 * uint32_t(wheel));
    return it == wheel_source.end() ? -1 : it->second;
}

// 0x8006FBE4: no polygon under the wheel ($v1 = car + 2 x wheel).
extern "C" void rush2_surface_wheel_fallback(uint8_t* rdram, recomp_context* ctx) {
    if (!wheel_source.empty()) wheel_source.erase((uint32_t)ctx->r3);
}

// particle_emitter_spawn_8008270C at 0x8008277C: $s0 = the emitter (wheels 0-3, 4 and 5 the car's own), $t6 = car + 2 x
// $s0, $s4 = its material, $s5 = the car's speed / 4 about to be taken (under 10 throws nothing from a wheel).
extern "C" void rush2_surface_particle(uint8_t* rdram, recomp_context* ctx) {
    int wheel = (int)(int32_t)ctx->r16;
    if (wheel < 0 || wheel > 3) return;
    const Rules* rules = active(rdram);
    if (rules == nullptr) return;
    int m = source(rdram, *rules, (uint32_t)ctx->r14);
    if (m < 0) return;
    switch (rules->materials[m].particle) {
    case Particle::Dust: ctx->r20 = 1; break;
    case Particle::Smoke: ctx->r20 = 0; break;
    case Particle::None: ctx->r21 = 0; break;
    case Particle::Rush2: break;
    }
}

// tire_mark_update_800823B0 at 0x80082408: $t9 = whether a skidding wheel lays a mark (Rush 2: material 0), $t8 = car +
// 2 x wheel.
extern "C" void rush2_surface_mark_rule(uint8_t* rdram, recomp_context* ctx) {
    const Rules* rules = active(rdram);
    if (rules == nullptr) return;
    int m = source(rdram, *rules, (uint32_t)ctx->r24);
    if (m >= 0) ctx->r25 = rules->materials[m].marks ? 1 : 0;
}

// tire_mark_start_800821A4 at 0x80082300, about to make a mark's polygon: $s0 = the mark, $s1 = car, $s4 = wheel, $a2 =
// SKID04's handle, $a3 = the color (0x800C5634).
extern "C" void rush2_surface_mark_start(uint8_t* rdram, recomp_context* ctx) {
    const Rules* rules = active(rdram);
    if (rules == nullptr) return;
    uint32_t key = physics_cars + uint32_t((int16_t)ctx->r17) * physics_car_size + uint32_t(ctx->r20 & 3) * 2;
    int m = source(rdram, *rules, key);
    uint32_t rgb = m >= 0 ? rules->materials[m].mark_rgb : 0;
    mark_rgb[(uint32_t)ctx->r16] = rgb;
    if (ready && mark_texture >= 0) ctx->r6 = mark_texture;
    saved_mark_rgb = (uint32_t)MEM_W(0, (int32_t)mark_color);
    MEM_W(0, (int32_t)mark_color) = int32_t((rgb << 8) | (saved_mark_rgb & 0xFF));
}

// Before extending a mark's polygon: 0x80082184 ($s3 = the mark) and 0x80082390 ($s6).
static void mark_color_of(uint8_t* rdram, uint32_t mark) {
    if (active(rdram) == nullptr) return;
    auto it = mark_rgb.find(mark);
    uint32_t rgb = it != mark_rgb.end() ? it->second : 0;
    saved_mark_rgb = (uint32_t)MEM_W(0, (int32_t)mark_color);
    MEM_W(0, (int32_t)mark_color) = int32_t((rgb << 8) | (saved_mark_rgb & 0xFF));
}

extern "C" void rush2_surface_mark_extend(uint8_t* rdram, recomp_context* ctx) {
    mark_color_of(rdram, (uint32_t)ctx->r19);
}

extern "C" void rush2_surface_mark_restart(uint8_t* rdram, recomp_context* ctx) {
    mark_color_of(rdram, (uint32_t)ctx->r22);
}

// effects_and_marks_update_80083228 at 0x80083404: fading the used marks, it sets each one's polygon again with the shared
// color word ($s0 = the mark), which would turn every mark black.
extern "C" void rush2_surface_mark_fade(uint8_t* rdram, recomp_context* ctx) {
    mark_color_of(rdram, (uint32_t)ctx->r16);
}

// After each of those: the color word back as it was (other polygons share it).
extern "C" void rush2_surface_mark_done(uint8_t* rdram, recomp_context* ctx) {
    if (active(rdram) == nullptr) return;
    MEM_W(0, (int32_t)mark_color) = int32_t((saved_mark_rgb & 0xFFFFFF00) | (MEM_W(0, (int32_t)mark_color) & 0xFF));
}

// race_load_car_assets_80086CA4 at 0x80087154, the cars' nodes made: the rules' handles and each car's spray nodes.
extern "C" void rush2_surface_race_cars(uint8_t* rdram, recomp_context* ctx) {
    ready = false;
    wheel_source.clear();
    mark_rgb.clear();
    for (auto& car : spray_nodes) for (int16_t& n : car) n = -1;
    const Rules* rules = active(rdram);
    if (rules == nullptr) return;
    mark_texture = rules->mark_texture ? find_texture(rdram, ctx, rules->mark_texture) : int16_t(-1);
    for (int i = 0; i < 2; i++) spray_models[i] = rules->spray_models[i] ? find_model(rdram, ctx, rules->spray_models[i]) : -1;
    for (int i = 0; i < 4; i++) spray_frames[i] = rules->spray_frames[i] ? find_texture(rdram, ctx, rules->spray_frames[i]) : int16_t(-1);
    if (rules->mark_texture && mark_texture < 0) fprintf(stderr, "[surface_fx] texture %s missing\n", rules->mark_texture);
    bool spray = spray_models[0] >= 0 && spray_models[1] >= 0 && spray_frames[0] >= 0;
    if (rules->spray_models[0] && !spray) fprintf(stderr, "[surface_fx] spray models or frames missing\n");
    int made = 0;
    if (spray) {
        int cars = std::min<int>(MEM_H(0, (int32_t)race_car_count), max_cars);
        for (int car = 0; car < cars; car++) {
            int32_t body = MEM_W(0, (int32_t)(ct::car_states + uint32_t(car) * draw_state_size));
            if (body < 0 || body >= MEM_W(0, (int32_t)node_count)) continue;
            for (int w = 0; w < 4; w++) {
                int32_t model = spray_models[w & 1];
                int32_t matrix = call(rdram, ctx, scene_matrix_create_8007F91C);
                int32_t node = call(rdram, ctx, scene_node_create_8007F6DC, model, matrix, 3, body, 0);
                if (node < 0) continue;
                uint32_t n = node_at(node);
                MEM_W(0, (int32_t)n) = (int32_t)((uint32_t)MEM_W(0, (int32_t)n) | node_hidden | node_prim);
                MEM_W(0x30, (int32_t)n) = (int32_t)rules->spray_rgba;
                spray_nodes[car][w] = int16_t(node);
                made++;
            }
        }
    }
    printf("[surface_fx] %d spray nodes, mark texture %d\n", made, mark_texture);
    ready = true;
}

// car_state_draw_objects_80082D14 at 0x80082D94: $a0 = car, $v0 = its draw state. The car's spray, as California Speed's
// 0x800BEF14 draws it.
extern "C" void rush2_surface_car_draw(uint8_t* rdram, recomp_context* ctx) {
    if (!ready) return;
    int car = (int)(int16_t)ctx->r4;
    if (car < 0 || car >= max_cars || spray_nodes[car][0] < 0) return;
    const Rules* rules = active(rdram);
    if (rules == nullptr) return;
    uint32_t phys = physics_cars + uint32_t(car) * physics_car_size;
    uint32_t state = (uint32_t)ctx->r2;
    float speed = f32_at(rdram, phys + wheel_speed);
    int type = MEM_BU(0, (int32_t)(phys + car_type));
    int16_t frame = spray_frames[MEM_W(0, (int32_t)game_frame_count) & 3];
    for (int w = 0; w < 4; w++) {
        int node = spray_nodes[car][w];
        int32_t model = spray_models[w & 1];
        if (!spray_node_ok(rdram, node, model)) continue;
        uint32_t n = node_at(node);
        int m = source(rdram, *rules, phys + uint32_t(w) * 2);
        bool on = m >= 0 && rules->materials[m].spray && speed > 0.0f;
        uint32_t flags = (uint32_t)MEM_W(0, (int32_t)n);
        if (!on) {
            MEM_W(0, (int32_t)n) = (int32_t)(flags | node_hidden);
            continue;
        }
        MEM_W(0, (int32_t)n) = (int32_t)(flags & ~node_hidden);
        if (frame >= 0) {
            uint32_t lod = model_record(rdram, model) + 4;
            MEM_H(0, (int32_t)lod) = frame;
            MEM_H(2, (int32_t)lod) = int16_t(MEM_HU(2, (int32_t)lod) | 0x8000);
        }
        float scale = std::min(speed / 40.0f + 0.5f, 2.5f);
        float r = f32_at(rdram, ct::entry(w < 2 ? ct::front_wheel_scale : ct::rear_wheel_scale, 0, type));
        uint32_t point = state + wheel_points + uint32_t(w) * 12;
        uint32_t sp = (uint32_t)ctx->r29 - 0x80;
        uint32_t pos = sp, mtx = sp + 0x10;
        put_f32(rdram, pos, f32_at(rdram, point));
        put_f32(rdram, pos + 4, f32_at(rdram, point + 4) + r - 0.835f * scale);
        put_f32(rdram, pos + 8, f32_at(rdram, point + 8) - r + 0.9f);
        for (int i = 0; i < 9; i++) put_f32(rdram, mtx + i * 4, (i % 4) == 0 ? scale : 0.0f);
        ctx->r29 = (int32_t)ctx->r29 - 0xA0;
        call(rdram, ctx, node_set_pos_matrix_8005A2D8, node, (int32_t)pos, (int32_t)mtx);
        ctx->r29 = (int32_t)ctx->r29 + 0xA0;
    }
}
