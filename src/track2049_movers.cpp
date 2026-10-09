// Rush 2049's moving track objects in Rush 2 (spec: docs/rush2049_research/movers.md).
//
// The objects are Rush 2049's path followers: trains, trolleys, gondolas, elevators, trap doors, trigger pads and the
// like, each moving along a PATH chunk of the 2049 track file. src/track2049_movers_logic.cpp ports 2049's follower
// code exactly; this file runs it inside Rush 2:
// - The converter places every path object at its spawn nodes as a top-level placement record (world-space, after the
//   track sections, so the visibility tables never hide it), and Rush 2's placement walk gives each record a scene
//   node whose matrix pointer points at the record's 12 floats. Writing an object's pose there moves its node.
//   Records of objects that don't spawn in this race (paths present only in the other direction) are hidden.
// - The followers advance once per physics tick (func_80076578, next to Rush 2's own subway trains), and their
//   collision groups move with them: the converted collision file keeps 2049's polygon and vertex numbering, so the
//   2049 file's MOVER rest records describe the same polygons in Rush 2's in-memory collision data. A group 2049
//   disables (an absent trap door's floor) has its origin vertex moved far below the world, which Rush 2's queries
//   never reach.
// - Per car and tick (func_800706D0, after the wheel probes): trigger pads see which collision polygons the wheels
//   are on, and moving bodies push cars with 2049's response (func_800FD9F8), on the Rush 2 car fields of the spec's
//   field map. 2049 applies that force for one physics tick per rendered frame, so it is applied every other tick.
//   Boost pads and ride-on platforms (func_800E1F80) act on the polygon a wheel probe found under the car.
// - Placement objects that turn in place (TROLLEY2, WINDMILL, WINDMILL2: func_8010E694) have their records' matrices
//   rotated each tick.
// - The tick and car hooks also run the knock-over props (src/track2049_props.cpp) and animated textures.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "rush2_hooks.h"
#include "assets.h"
#include "battle.h"
#include "rush2049_rom.h"
#include "track2049.h"
#include "track2049_movers_logic.h"
#include "wings.h"

namespace movers = rush2::track2049::movers;

namespace {
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t backward_flag = 0x80119848;
    constexpr uint32_t record_base = 0x8010C15C;   // Pointer to the placement file's first record.
    constexpr uint32_t record_size = 0x64;
    constexpr uint32_t nodes = 0x800D9E90;         // Scene nodes, 0x38 bytes each.
    constexpr uint32_t node_size = 0x38;
    constexpr uint32_t node_count = 0x800FAE58;
    constexpr uint32_t node_hidden = 0x400;
    constexpr uint32_t poly_table = 0x8011001C;    // Pointer to the collision file's POLY records (0x18 bytes).
    constexpr uint32_t vert_table = 0x80110050;    // Pointer to its VERT records (8 bytes).
    constexpr uint32_t cars = 0x800F5470;
    constexpr uint32_t car_size = 0x81C;
    constexpr int max_cars = 8;
    constexpr uint32_t player_slots = 0x800C2140; // Local player slots, 0x28 bytes: +0 = car index.
    constexpr uint32_t wreck_option = 0x80119628;  // s8: 1 = any contact wrecks.

    // Rush 2 car fields (movers.md §7).
    constexpr uint32_t car_force = 0x11C;          // f32[3] external force, car-local, cleared each tick
    constexpr uint32_t car_wheel_poly = 0x594;     // u16[4] polygon each wheel last found
    constexpr uint32_t car_mass = 0x5B4;           // f32 collision mass
    constexpr uint32_t car_wreck_threshold = 0x644;
    constexpr uint32_t car_wrecked = 0x648;        // s8
    constexpr uint32_t car_radius = 0x658;
    constexpr uint32_t car_snap_vel = 0x7A4;
    constexpr uint32_t car_snap_pos = 0x7B0;
    constexpr uint32_t car_snap_matrix = 0x7BC;
    constexpr uint32_t car_corners = 0xE8;          // 4 x f32[3], car-local
    constexpr uint32_t car_active = 0x7E4;          // s16
    constexpr uint32_t car_velocity = 0x218;        // f32[3] world
    constexpr uint32_t car_position = 0x224;        // f32[3] world
    constexpr uint32_t car_matrix = 0x2CC;          // f32[9] physics orientation
    constexpr uint32_t car_body_mass = 0x5B0;
    constexpr uint32_t car_step = 0x638;            // f32 dt of the physics step
    constexpr uint32_t car_wheel_height = 0x5DC;    // f32[4], <= 0 in contact
    constexpr uint32_t car_wheel_force = 0x58;      // f32[3] per wheel, 12 bytes apart

    // 2049's moving-body boxes (0x80118C10 + 32 * class): x min/max, z max/min, y max/min, radius, mass.
    struct BodyBox {
        float xmin, xmax, zmax, zmin, ymax, ymin, radius, mass;
    };
    constexpr BodyBox body_boxes[] = {
        { -7.0f, 7.0f, 30.0f, -30.0f, 0.0f, -14.6f, 45.0f, 4000.0f },      // GONDOLA1
        { -7.0f, 7.0f, 33.0f, -33.0f, 0.0f, -14.6f, 45.0f, 4000.0f },      // GONDOLA2
        { -4.0f, 4.5f, 10.0f, -10.0f, 28.0f, 0.0f, 45.0f, 4000.0f },       // ELEVATOR1
        { -10.0f, 8.0f, 142.0f, -144.0f, 12.0f, 0.0f, 155.0f, 5000.0f },   // TRAINORG
        { -6.5f, 6.5f, 17.7f, -18.5f, 18.5f, 0.0f, 40.0f, 4000.0f },       // TROLLEY(NT)
        { -5.4f, 5.4f, 22.0f, -22.0f, 14.5f, 0.0f, 40.0f, 4000.0f },       // MINITRAIN(N)
        { -20.0f, 20.0f, 30.0f, -30.0f, 10.0f, -10.0f, 70.0f, 6000.0f },   // PLANE
        { -5.0f, 5.0f, 10.0f, -10.0f, 15.0f, 0.0f, 50.0f, 6000.0f },       // PISTON
        { -10.0f, 10.0f, 5.0f, -5.0f, 10.0f, 0.0f, 30.0f, 6000.0f },       // TEETH
        { -40.0f, 40.0f, 40.0f, -40.0f, 40.0f, -40.0f, 120.0f, 6000.0f },  // BOULDER
        { -18.0f, 18.0f, 10.0f, -10.0f, 39.0f, 0.0f, 120.0f, 6000.0f },    // BLOCKNV
    };

    struct Mover {
        int16_t group;
        uint16_t poly;
        int16_t rest_matrix[9];
        uint16_t vert;
        int16_t rest_vert[4];
    };

    struct Placed {
        int object = -1;        // Index in the world's objects, or -1 if the record's object didn't spawn.
        uint32_t record = 0;    // Address of the placement record.
        uint32_t node = 0;      // Address of its scene node, or 0.
    };

    std::mutex movers_mutex;
    bool setup_pending = false;
    bool active = false;
    movers::World world;
    std::vector<Placed> placed;
    std::vector<Mover> mover_records;
    std::vector<uint16_t> poly_info;                    // 2049 info word of each collision polygon.
    std::set<int16_t> trigger_groups;
    std::map<std::string, uint16_t> model_handles;      // Model name -> Rush 2 handle in the track's slot.
    uint32_t tick = 0;
    int contact_poly[max_cars] = { -1, -1, -1, -1, -1, -1, -1, -1 }; // Boost pad / platform polygon under each car.

    // Inputs kept from the track conversion (src/track2049.cpp).
    std::vector<uint8_t> geometry_2049, collision_2049;
    std::vector<rush2::track2049::PathRecord> path_records;
    std::vector<rush2::track2049::SpinRecord> spin_records;
    std::vector<std::string> geometry_names;            // Sorted model names of the converted geometry.

    uint16_t be16(const std::vector<uint8_t>& d, size_t o) {
        return uint16_t((d[o] << 8) | d[o + 1]);
    }

    float read_f(uint8_t* rdram, uint32_t addr) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }
    void write_f(uint8_t* rdram, uint32_t addr, float f) {
        uint32_t w;
        memcpy(&w, &f, 4);
        MEM_W(0, (int32_t)addr) = w;
    }

    // The 2049 collision file's MOVER records and each polygon's info word (collision.md §2-3).
    bool parse_collision(const std::vector<uint8_t>& c) {
        mover_records.clear();
        poly_info.clear();
        if (c.size() < 0x10) {
            return false;
        }
        uint32_t n_seg = be16(c, 0), n_node = be16(c, 2), n_poly = be16(c, 4), n_vert = be16(c, 6), n_mover = be16(c, 8);
        size_t poly = 0x10 + n_seg * 0x84 + n_node * 0x14;
        size_t mover = poly + n_poly * 0x18 + n_vert * 8;
        if (mover + n_mover * 0x20 > c.size()) {
            return false;
        }
        for (uint32_t i = 0; i < n_poly; i++) {
            poly_info.push_back(be16(c, poly + i * 0x18 + 2));
        }
        for (uint32_t i = 0; i < n_mover; i++) {
            size_t o = mover + i * 0x20;
            Mover m;
            m.group = (int16_t)be16(c, o);
            m.poly = be16(c, o + 2);
            for (int j = 0; j < 9; j++) m.rest_matrix[j] = (int16_t)be16(c, o + 4 + j * 2);
            m.vert = be16(c, o + 0x16);
            for (int j = 0; j < 4; j++) m.rest_vert[j] = (int16_t)be16(c, o + 0x18 + j * 2);
            mover_records.push_back(m);
        }
        return true;
    }

    // Collision group edits on Rush 2's in-memory POLY/VERT records.
    void read_vert(uint8_t* rdram, uint32_t verts, uint16_t index, int16_t v[4]) {
        for (int j = 0; j < 4; j++) v[j] = (int16_t)MEM_H(0, (int32_t)(verts + index * 8 + j * 2));
    }
    void write_vert(uint8_t* rdram, uint32_t verts, uint16_t index, const int16_t v[4]) {
        for (int j = 0; j < 4; j++) MEM_H(0, (int32_t)(verts + index * 8 + j * 2)) = v[j];
    }
    void write_matrix(uint8_t* rdram, uint32_t polys, uint16_t index, const int16_t m[9]) {
        for (int j = 0; j < 9; j++) MEM_H(0, (int32_t)(polys + index * 0x18 + 4 + j * 2)) = m[j];
    }

    void apply_ops(uint8_t* rdram, const std::vector<movers::GroupOp>& ops) {
        uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table), verts = (uint32_t)MEM_W(0, (int32_t)vert_table);
        if (polys == 0 || verts == 0) {
            return;
        }
        for (const movers::GroupOp& op : ops) {
            for (const Mover& m : mover_records) {
                if (m.group != op.group) {
                    continue;
                }
                int16_t v[4], mat[9];
                switch (op.kind) {
                    case movers::GroupOp::translate:
                        read_vert(rdram, verts, m.vert, v);
                        movers::apply_translate(op.delta, v);
                        write_vert(rdram, verts, m.vert, v);
                        break;
                    case movers::GroupOp::rotate:
                        movers::apply_rotate(op, m.rest_matrix, m.rest_vert, mat, v);
                        write_matrix(rdram, polys, m.poly, mat);
                        write_vert(rdram, verts, m.vert, v);
                        break;
                    case movers::GroupOp::restore:
                        write_matrix(rdram, polys, m.poly, m.rest_matrix);
                        write_vert(rdram, verts, m.vert, m.rest_vert);
                        break;
                    case movers::GroupOp::disable:
                        // Rush 2 has no disabled polygon type: drop the polygon (whose other vertices are relative
                        // to its origin) far below the world.
                        memcpy(v, m.rest_vert, sizeof(v));
                        v[1] = -30000;
                        write_vert(rdram, verts, m.vert, v);
                        break;
                }
            }
        }
    }

    void restore_all_groups(uint8_t* rdram) {
        uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table), verts = (uint32_t)MEM_W(0, (int32_t)vert_table);
        if (polys == 0 || verts == 0) {
            return;
        }
        for (const Mover& m : mover_records) {
            write_matrix(rdram, polys, m.poly, m.rest_matrix);
            write_vert(rdram, verts, m.vert, m.rest_vert);
        }
    }

    uint16_t handle_of(const std::string& name, uint16_t slot_bits) {
        auto it = model_handles.find(name);
        if (it == model_handles.end()) {
            return 0xFFFF;
        }
        return uint16_t(slot_bits | it->second);
    }

    // Model a world object shows now (flip-books and trigger pads swap models).
    std::string model_name(const movers::Object& o) {
        const auto& types = world.types();
        if (o.model == movers::Model::trigger_on) return "TRIGGERON";
        if (o.model == movers::Model::trigger_off) return "TRIGGEROFF";
        if (o.anim_model >= 0 && o.type >= 0) {
            // Flip-book frames (movers.md §2): SHARK base 0x169, F1FLAG base 0x16D.
            int base = types[o.type].anim, frame = o.anim_model - base;
            static const char* shark[] = { "SHARKG2", "SHARKG4", "SHARKG5", "SHARKG16" };
            if (base == 0x169 && frame >= 0 && frame < 4) return shark[frame];
            if (base == 0x16D && frame >= 0 && frame < 20) return "F1FLAGG" + std::to_string(28 + frame);
        }
        return o.type >= 0 ? types[o.type].model.substr(0, 15) : std::string();
    }

    void write_pose(uint8_t* rdram, const Placed& p, const movers::Object& o, uint16_t slot_bits) {
        for (int i = 0; i < 9; i++) write_f(rdram, p.record + 0x10 + i * 4, o.m[i]);
        for (int i = 0; i < 3; i++) write_f(rdram, p.record + 0x34 + i * 4, o.pos[i]);
        if (p.node != 0) {
            uint16_t h = handle_of(model_name(o), slot_bits);
            if (h != 0xFFFF) {
                MEM_H(0, (int32_t)(p.node + 0xC)) = h;
            }
        }
    }

    bool setup(uint8_t* rdram) {
        active = false;
        placed.clear();
        trigger_groups.clear();
        if (!parse_collision(collision_2049)) {
            fprintf(stderr, "[2049] Moving objects: bad collision file\n");
            return false;
        }
        std::vector<movers::Path> paths;
        if (!movers::parse_paths(geometry_2049.data(), geometry_2049.size(), paths)) {
            fprintf(stderr, "[2049] Moving objects: bad paths\n");
            return false;
        }
        auto rom = rush2::wings::get_rom();
        std::vector<uint8_t> main_2049;
        if (rom == nullptr || rom->size() <= 0xB0CB10 ||
            !rush2::assets::inflate_raw(rom->data() + 0xB0CB10, rom->size() - 0xB0CB10, main_2049)) {
            fprintf(stderr, "[2049] Moving objects: can't read Rush 2049's main code\n");
            return false;
        }
        std::vector<movers::TypeInfo> types;
        if (!movers::parse_types(main_2049.data(), main_2049.size(), 0x80086A50, types)) {
            fprintf(stderr, "[2049] Moving objects: bad type table\n");
            return false;
        }
        movers::Options options;
        options.backward = MEM_B(0, (int32_t)backward_flag) != 0;
        options.mode = std::max(0, (int)rush2::track2049::game_type(rdram));
        world = movers::World();
        world.init(std::move(paths), types, options);

        // Records and their scene nodes.
        uint32_t base = (uint32_t)MEM_W(0, (int32_t)record_base);
        int count = (int)MEM_W(0, (int32_t)node_count);
        std::map<uint32_t, uint32_t> node_of_matrix;
        for (int i = 0; i < count; i++) {
            uint32_t node = nodes + i * node_size;
            node_of_matrix[(uint32_t)MEM_W(0, (int32_t)(node + 4))] = node;
        }
        const auto& objects = world.objects();
        std::vector<bool> used(objects.size(), false);
        uint16_t slot_bits = 0xFFFF;
        for (const auto& pr : path_records) {
            Placed p;
            p.record = base + pr.record * record_size;
            auto n = node_of_matrix.find(p.record + 0x10);
            p.node = n == node_of_matrix.end() ? 0 : n->second;
            if (p.node != 0 && slot_bits == 0xFFFF) {
                slot_bits = uint16_t(MEM_HU(0, (int32_t)(p.node + 0xC)) & 0xFC00);
            }
            // The object of this path closest to the record's spawn node (objects of a path take its spawn nodes
            // in reverse order).
            const auto& node = world.paths()[pr.path].nodes[pr.node];
            float best = 1e30f;
            for (size_t i = 0; i < objects.size(); i++) {
                if (used[i] || objects[i].path != pr.path) continue;
                float d = 0;
                for (int a = 0; a < 3; a++) d += (objects[i].pos[a] - node.pos[a]) * (objects[i].pos[a] - node.pos[a]);
                if (d < best) { best = d; p.object = (int)i; }
            }
            if (p.object >= 0) {
                used[p.object] = true;
            }
            else if (p.node != 0) {
                MEM_W(0, (int32_t)p.node) = (uint32_t)MEM_W(0, (int32_t)p.node) | node_hidden;
            }
            placed.push_back(p);
        }
        // Handles of every model by name, for flip-books and trigger pads (names sorted, as in the container).
        model_handles.clear();
        for (size_t i = 0; i < geometry_names.size(); i++) {
            model_handles[geometry_names[i]] = (uint16_t)i;
        }
        for (const auto& o : objects) {
            if (o.slot == 5 && o.cars && o.path >= 0) {
                trigger_groups.insert(world.paths()[o.path].group);
            }
        }
        restore_all_groups(rdram);
        apply_ops(rdram, world.take_group_ops());
        for (const Placed& p : placed) {
            if (p.object >= 0 && slot_bits != 0xFFFF) {
                write_pose(rdram, p, objects[p.object], slot_bits);
            }
        }
        active = slot_bits != 0xFFFF;
        size_t spin_nodes = 0;
        for (const auto& s : spin_records) {
            spin_nodes += node_of_matrix.contains(base + s.record * record_size + 0x10);
        }
        fprintf(stderr, "[2049] %zu moving objects, %zu placed, %zu collision movers, %zu trigger groups, %zu/%zu turning\n",
               objects.size(), placed.size(), mover_records.size(), trigger_groups.size(), spin_nodes,
               spin_records.size());
        return active;
    }

    // Row-vector transforms (movers.md §6): TMUL local -> world, MUL world -> local.
    void tmul(const float v[3], const float m[9], float out[3]) {
        for (int j = 0; j < 3; j++) out[j] = m[6 + j] * v[2] + (v[0] * m[j] + v[1] * m[3 + j]);
    }
    void mul(const float v[3], const float m[9], float out[3]) {
        for (int i = 0; i < 3; i++) out[i] = m[i * 3 + 2] * v[2] + (v[0] * m[i * 3] + v[1] * m[i * 3 + 1]);
    }

    // 0x8010C02C and func_800FD9F8 on Rush 2's car fields.
    void body_vs_car(uint8_t* rdram, uint32_t car, const movers::Object& o) {
        const auto& type = world.types()[o.type];
        int cls = (int)type.param;
        if (cls < 0 || cls >= (int)(sizeof(body_boxes) / sizeof(body_boxes[0]))) {
            return;
        }
        const BodyBox& b = body_boxes[cls];
        float cpos[3], cvel[3], cm[9];
        for (int i = 0; i < 3; i++) {
            cpos[i] = read_f(rdram, car + car_snap_pos + i * 4);
            cvel[i] = read_f(rdram, car + car_snap_vel + i * 4);
        }
        for (int i = 0; i < 9; i++) cm[i] = read_f(rdram, car + car_snap_matrix + i * 4);
        float d[3] = { cpos[0] - o.pos[0], cpos[1] - o.pos[1], cpos[2] - o.pos[2] };
        float s = ((0.0f + d[0] * d[0]) + d[1] * d[1]) + d[2] * d[2];
        float r = b.radius + read_f(rdram, car + car_radius);
        if (r * r < s) {
            return;
        }
        bool hit = false;
        for (int c = 0; c < 4 && !hit; c++) {
            float corner[3], w[3], p[3];
            for (int i = 0; i < 3; i++) corner[i] = read_f(rdram, car + car_corners + c * 12 + i * 4);
            tmul(corner, cm, w);
            for (int i = 0; i < 3; i++) w[i] = w[i] + cpos[i] - o.pos[i];
            mul(w, o.m, p);
            hit = p[0] >= b.xmin && p[0] <= b.xmax && p[1] >= b.ymin && p[1] <= b.ymax && p[2] >= b.zmin && p[2] <= b.zmax;
        }
        if (!hit) {
            return;
        }
        // func_800FD9F8.
        float l[3];
        mul(d, o.m, l);
        bool in_z = b.zmin < l[2] && l[2] < b.zmax, in_x = b.xmin < l[0] && l[0] < b.xmax;
        int8_t wrecked = (int8_t)MEM_B(0, (int32_t)(car + car_wrecked));
        if (in_z && in_x && wrecked) {
            // func_800FD8DC: a wreck inside the body is pushed out along d.
            float len = std::sqrt(d[2] * d[2] + (d[0] * d[0] + d[1] * d[1]));
            if (len > 0.0f) {
                float push[3] = { d[0] / len * 100000.0f, d[1] / len * 100000.0f, d[2] / len * 100000.0f }, local[3];
                mul(push, cm, local);
                for (int i = 0; i < 3; i++) write_f(rdram, car + car_force + i * 4, read_f(rdram, car + car_force + i * 4) + local[i]);
            }
            return;
        }
        float rel[3] = { cvel[0] - o.body_vel[0], cvel[1] - o.body_vel[1], cvel[2] - o.body_vel[2] }, rv[3];
        mul(rel, o.m, rv);
        float fz = (in_z && !in_x) ? 0.0f : rv[2] * 2500.0f;
        if (l[2] > 0.0f) {
            fz = std::min(fz, -4000.0f);
            if (in_x) { float t = (l[2] - b.zmax) * (10000.0f * std::fabs(l[2] - b.zmax)); fz = std::min(fz, t); }
        }
        else {
            fz = std::max(fz, 4000.0f);
            if (in_x) { float t = (l[2] - b.zmin) * (10000.0f * std::fabs(l[2] - b.zmin)); fz = std::max(fz, t); }
        }
        float fx = (in_x && !in_z) ? 0.0f : rv[0] * 20000.0f;
        if (l[0] > 0.0f) {
            fx = std::min(fx, -4000.0f);
            if (in_z) { float t = (l[0] - b.xmax) * (10000.0f * std::fabs(l[0] - b.xmax)); fx = std::min(fx, t); }
        }
        else {
            fx = std::max(fx, 4000.0f);
            if (in_z) { float t = (l[0] - b.xmin) * (10000.0f * std::fabs(l[0] - b.xmin)); fx = std::max(fx, t); }
        }
        float f[3] = { fx, rv[1] * 100.0f, fz }, world_f[3], c[3];
        tmul(f, o.m, world_f);
        mul(world_f, cm, c);
        float k = b.mass / ((read_f(rdram, car + car_mass) + b.mass) * 0.5f);
        for (int i = 0; i < 3; i++) {
            c[i] *= k;
            write_f(rdram, car + car_force + i * 4, read_f(rdram, car + car_force + i * 4) - c[i]);
        }
        float len = std::sqrt(c[2] * c[2] + (c[0] * c[0] + c[1] * c[1]));
        if ((MEM_B(0, (int32_t)wreck_option) == 1 || read_f(rdram, car + car_wreck_threshold) * 0.8f < len) && !wrecked) {
            MEM_B(0, (int32_t)(car + car_wrecked)) = 1;
        }
    }
}

// func_800E1F80 (2049): boost pads (POLY info 0x10) and ride-on platforms (0x20) under car `car`.
static void pad_or_platform(uint8_t* rdram, uint32_t car, int poly) {
    uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table);
    uint16_t info = poly_info[poly];
    int id = info >> 11;
    float spd = (float)id;
    float m[9], a[3];
    for (int i = 0; i < 9; i++) m[i] = read_f(rdram, car + car_matrix + i * 4);
    for (int i = 0; i < 3; i++) a[i] = (float)(int16_t)MEM_H(0, (int32_t)(polys + poly * 0x18 + 4 + i * 2)) * 6.1035156e-05f;
    float mass = read_f(rdram, car + car_body_mass);
    if (info & 0x10) {
        spd = (float)(id * 8);
        if (spd >= 51.0f) {
            // Strong mode: drive the car's speed along the pad toward its speed (mph).
            float v = a[2] * read_f(rdram, car + car_velocity + 8) +
                      (a[0] * read_f(rdram, car + car_velocity) + a[1] * read_f(rdram, car + car_velocity + 4));
            float k = (spd * 1.4666667f - v) * mass, push[3] = { a[0] * k, a[1] * k, a[2] * k }, local[3];
            mul(push, m, local);
            for (int i = 0; i < 3; i++) write_f(rdram, car + car_force + i * 4, read_f(rdram, car + car_force + i * 4) + local[i]);
            return;
        }
    }
    int wheels[4], n = 0;
    for (int w = 0; w < 4; w++) {
        uint16_t p = MEM_HU(0, (int32_t)(car + car_wheel_poly + w * 2));
        if (read_f(rdram, car + car_wheel_height + w * 4) <= 0.0f && p < poly_info.size() && (poly_info[p] & 0x30)) {
            wheels[n++] = w;
        }
    }
    float dt = read_f(rdram, car + car_step);
    if (n >= 3) {
        // Carried: move the car's x/z position.
        float dx, dz;
        if (info & 0x20) {
            float v[3];
            world.platform_velocity((int16_t)id, v);
            dx = v[0] * dt; dz = v[2] * dt;
        }
        else {
            float t = (spd * 1.4666667f) * dt;
            dx = (t * (float)(int16_t)MEM_H(0, (int32_t)(polys + poly * 0x18 + 4))) * 6.1035156e-05f;
            dz = (t * (float)(int16_t)MEM_H(0, (int32_t)(polys + poly * 0x18 + 8))) * 6.1035156e-05f;
        }
        write_f(rdram, car + car_position, read_f(rdram, car + car_position) + dx);
        write_f(rdram, car + car_position + 8, read_f(rdram, car + car_position + 8) + dz);
        return;
    }
    float v[3] = { a[0], a[1], a[2] };
    if (info & 0x20) {
        world.platform_velocity((int16_t)id, v);
        float len = std::sqrt(v[2] * v[2] + (v[0] * v[0] + v[1] * v[1]));
        if (len > 0.0f) {
            for (float& c : v) c /= len;
        }
    }
    for (float& c : v) c *= 30.0f * mass;
    float local[3];
    mul(v, m, local);
    for (int i = 0; i < n; i++) {
        uint32_t f = car + car_wheel_force + wheels[i] * 12;
        for (int j = 0; j < 3; j++) write_f(rdram, f + j * 4, read_f(rdram, f + j * 4) + local[j]);
    }
}

// The track conversion's inputs this file needs (src/track2049.cpp, when a 2049 track is loaded).
void rush2::track2049::set_mover_data(const std::vector<uint8_t>& geometry_2049_file, const std::vector<uint8_t>& collision_2049_file,
                                      const std::vector<PathRecord>& records, const std::vector<SpinRecord>& spins,
                                      const std::vector<std::string>& model_names) {
    std::lock_guard lock{ movers_mutex };
    geometry_2049 = geometry_2049_file;
    collision_2049 = collision_2049_file;
    path_records = records;
    spin_records = spins;
    geometry_names = model_names;
}

// Called at race setup: the objects are set up on the race's first physics tick.
void rush2::track2049::reset_movers() {
    std::lock_guard lock{ movers_mutex };
    setup_pending = true;
    active = false;
}

// func_80076578 at 0x800765F4, after Rush 2's subway trains moved: $sp + 0x18 = the tick's dt.
extern "C" void rush2_track49_movers_tick(uint8_t* rdram, recomp_context* ctx) {
    // Animated textures don't depend on the moving objects being set up.
    rush2::track2049::texanim_tick(rdram, read_f(rdram, (uint32_t)((int32_t)ctx->r29 + 0x18)));
    rush2::track2049::props_tick(rdram, read_f(rdram, (uint32_t)((int32_t)ctx->r29 + 0x18)));
    rush2::battle::tick(rdram, read_f(rdram, (uint32_t)((int32_t)ctx->r29 + 0x18)));
    std::lock_guard lock{ movers_mutex };
    int slot = rush2::track2049::loaded_slot();
    if (slot < 0 || MEM_B(0, (int32_t)track_id) != slot) {
        if (active) {
            rush2::track2049::stop_object_sounds();
        }
        active = false;
        return;
    }
    if (setup_pending) {
        setup_pending = false;
        setup(rdram);
    }
    if (!active) {
        return;
    }
    float dt = read_f(rdram, (uint32_t)((int32_t)ctx->r29 + 0x18));
    tick++;
    {
        // Player 1's car (local player slot 0 at 0x800C2140, +0 = car index), as 2049's car 0.
        uint32_t car = cars + MEM_BU(0, (int32_t)player_slots) * car_size;
        float focus[3];
        for (int i = 0; i < 3; i++) focus[i] = read_f(rdram, car + car_position + i * 4);
        world.set_focus(focus);
    }
    world.update(dt);
    // Objects that turn in place (func_8010E694), on their records' matrices.
    uint32_t base = (uint32_t)MEM_W(0, (int32_t)record_base);
    for (const auto& s : spin_records) {
        uint32_t m_addr = base + s.record * record_size + 0x10;
        float m[9];
        for (int i = 0; i < 9; i++) m[i] = read_f(rdram, m_addr + i * 4);
        movers::rotate_in_place(m, s.sub, dt);
        for (int i = 0; i < 9; i++) write_f(rdram, m_addr + i * 4, m[i]);
    }
    apply_ops(rdram, world.take_group_ops());
    uint16_t slot_bits = 0xFFFF;
    for (const Placed& p : placed) {
        if (p.node != 0) {
            slot_bits = uint16_t(MEM_HU(0, (int32_t)(p.node + 0xC)) & 0xFC00);
            break;
        }
    }
    const auto& objects = world.objects();
    for (const Placed& p : placed) {
        if (p.object >= 0) {
            write_pose(rdram, p, objects[p.object], slot_bits);
        }
    }
    // Sound requests (func_800BF394 start, func_800BF45C loop, func_800BF1C8 stop).
    std::vector<rush2::track2049::ObjectSound> sounds(objects.size());
    for (size_t i = 0; i < objects.size(); i++) {
        const movers::Object& o = objects[i];
        auto& s = sounds[i];
        if (o.type < 0) {
            continue;
        }
        const movers::TypeInfo& t = world.types()[o.type];
        s.request = o.sound == movers::Sound::start ? rush2::track2049::ObjectSound::start :
                    o.sound == movers::Sound::loop ? rush2::track2049::ObjectSound::loop :
                    o.sound == movers::Sound::stop ? rush2::track2049::ObjectSound::stop :
                    rush2::track2049::ObjectSound::none;
        for (int k = 0; k < 3; k++) s.ids[k] = t.sounds[k];
        for (int k = 0; k < 3; k++) s.pos[k] = o.pos[k];
        s.range = t.sound_range;
        // Loop sound 1 follows the object's speed (func_800BF45C).
        int max_speed = std::abs((int)o.f.max_speed);
        float f = max_speed > 0 ? (float)std::abs((int)o.f.speed) / (float)max_speed * 0.5f + 0.5f : 1.0f;
        s.speed_factor = std::min(1.0f, std::max(0.0f, f));
    }
    rush2::track2049::update_object_sounds(rdram, sounds);
}

// func_800706D0 at 0x800706E4, per car after its wheel probes: $s0 = the car.
extern "C" void rush2_track49_movers_car(uint8_t* rdram, recomp_context* ctx) {
    rush2::track2049::props_car(rdram, (uint32_t)ctx->r16);
    std::lock_guard lock{ movers_mutex };
    if (!active) {
        return;
    }
    uint32_t car = (uint32_t)ctx->r16;
    int index = (int)((car - cars) / car_size);
    if (index < 0 || index >= max_cars) {
        return;
    }
    bool wrecked = MEM_B(0, (int32_t)(car + car_wrecked)) != 0;
    // Trigger pads (0x8010C2E4): a wheel's polygon tagged 0x20 with the pad's group in bits 11-15.
    for (int16_t group : trigger_groups) {
        bool on = false;
        if (!wrecked) {
            for (int w = 0; w < 4; w++) {
                uint16_t poly = MEM_HU(0, (int32_t)(car + car_wheel_poly + w * 2));
                if (poly < poly_info.size()) {
                    uint16_t info = poly_info[poly];
                    on |= (info & 0x20) && (info & 0xF800) == (uint16_t)(group << 11);
                }
            }
        }
        world.car_on_group(index, group, on);
    }
    // Moving bodies, every other tick (2049 applies them once per rendered frame).
    if ((tick & 1) == 0 && MEM_H(0, (int32_t)(car + car_active)) != 0) {
        for (const auto& o : world.objects()) {
            if (o.body && o.cars && o.type >= 0) {
                body_vs_car(rdram, car, o);
            }
        }
    }
}

// func_800706D0 at 0x800706DC, before a car's wheel probes: $a0 = the car.
extern "C" void rush2_track49_movers_probe_begin(uint8_t* rdram, recomp_context* ctx) {
    int index = (int)(((uint32_t)ctx->r4 - cars) / car_size);
    if (index >= 0 && index < max_cars) {
        contact_poly[index] = -1;
    }
}

// func_8006F704 (wheel probe) at 0x8006F99C, with a polygon found: $t9 = its POLY record, $f24 = the wheel's
// distance from it, 0x138($sp) = the car. 2049 notes a boost pad or platform polygon within 1.0 of a wheel.
extern "C" void rush2_track49_movers_probe(uint8_t* rdram, recomp_context* ctx) {
    if (!active) {
        return;
    }
    uint32_t car = (uint32_t)MEM_W(0, (int32_t)ctx->r29 + 0x138);
    int index = (int)((car - cars) / car_size);
    uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table);
    int poly = (int)(((uint32_t)ctx->r25 - polys) / 0x18);
    if (index >= 0 && index < max_cars && poly >= 0 && poly < (int)poly_info.size() && (poly_info[poly] & 0x30) &&
        ctx->f24.fl < 1.0f) {
        contact_poly[index] = poly;
    }
}

// func_800706D0 at 0x800706EC, after the drag step and before the forces are summed: $s0 = the car.
extern "C" void rush2_track49_movers_pads(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ movers_mutex };
    if (!active) {
        return;
    }
    uint32_t car = (uint32_t)ctx->r16;
    int index = (int)((car - cars) / car_size);
    if (index >= 0 && index < max_cars && contact_poly[index] >= 0) {
        pad_or_platform(rdram, car, contact_poly[index]);
    }
}
