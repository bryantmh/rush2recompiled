// Rush 2049's knock-over props in Rush 2: cones, gas pumps, rats, rat cones, signs, parking meters and cacti.
//
// In 2049 these are dynamic objects (type table 0x80117530) with car callback slot 0 (0x8010C6C8): each frame, a car
// whose position is within the object's radius + 3.5 of it hits it (func_800BEAA0), which calls the type's init
// function once and plays the type's sound (+0x1C). The object never pushes the car back. The reactions:
// - Knock-over props, kind 2 (CONE1, GASPUMP, RAT, RATCONE; func_8010DCFC, update func_8010E4E4): the object takes
//   1/8 of the car's velocity, turns to face along it (func_8008B4C4), gets 2 up (1 for RAT and RATCONE, models 0xED
//   and 0x153) and spins at (0, 12, 15) rad/s. Each frame its velocity gains gravity (0, -0.25, 0) x 0.15 and its
//   position moves by the velocity plus that again; there is no collision. After 5 s it stops, and types with flag
//   0x2000 (CONE1, GASPUMP) are removed.
// - Signs and parking meters, kind 0 sub-kinds 1-2 (func_8010E72C, update func_8010E828): the sign turns to face the
//   car's travel direction and tips over about its x axis for 4 frames, by the sub-kind's angles per frame (table
//   0x80118D70: -0.384 rad, GETOFF -0.375 rad), and stays down.
// - CACTUS (func_8010D9CC, update func_80094888): it turns to face the car's travel direction and runs its 7-frame
//   flip-book (CACTUSG2-G8, from model index 263 of the handle table 0x8011AD68) at 1/16 s per frame after a 1/30 s
//   delay, and keeps the last frame (type flag 0x1000).
// 2049 runs these per rendered frame; the per-frame steps are scaled by the tick's length in 30 Hz frames here.
//
// The converter places each prop as a record drawing its 2049 model (X49<model>) and lists the records
// (PropRecord). Rush 2's placement walk gives each record a scene node whose matrix pointer points at the record's
// 12 floats, so writing a prop's pose there moves it, as for the moving objects (src/track2049_movers.cpp, whose
// hooks call this file). Props present in one race direction only (_FW, _BW) are hidden in the other.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "recomp.h"
#include "assets.h"
#include "ghost.h"
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
    constexpr uint32_t cars = 0x800F5470;
    constexpr uint32_t car_size = 0x81C;
    constexpr int max_cars = 8;
    constexpr uint32_t car_snap_vel = 0x7A4;        // f32[3] world
    constexpr uint32_t car_snap_pos = 0x7B0;        // f32[3] world
    constexpr uint32_t car_active = 0x7E4;          // s16

    constexpr uint32_t main_vram = 0x80086A50;
    constexpr size_t main_rom = 0xB0CB10;
    constexpr uint32_t model_names_2049 = 0x8011AD68;  // Name pointers of the model handle table.
    constexpr uint32_t handle_count = 0x200;
    constexpr uint32_t kind_params = 0x80118DDC;       // Per kind: pointer to per-sub-kind parameters.
    constexpr uint32_t kind_param_sizes = 0x80117510;  // Per kind: u8 entry size.

    // Type init functions: the reaction a hit starts.
    constexpr uint32_t init_knock = 0x8010DCFC;
    constexpr uint32_t init_sign = 0x8010E72C;
    constexpr uint32_t init_cactus = 0x8010D9CC;

    constexpr float frame_rate = 30.0f;              // 2049 frames per second
    constexpr float hit_margin = 3.5f;               // 0x8010C6C8
    constexpr float gravity[3] = { 0.0f, -0.25f, 0.0f }; // 0x80121DDC
    constexpr float gravity_scale = 0.15f;           // 0x801249CC
    constexpr float launch_scale = 0.125f;
    constexpr float knock_seconds = 5.0f;
    constexpr float knock_spin[3] = { 0.0f, 12.0f, 15.0f };
    constexpr float sign_frames = 4.0f;
    constexpr float flipbook_delay = 0.0333333f;     // 0x801249BC
    constexpr float flipbook_period = 0.0625f;
    constexpr int cactus_frames = 7;
    constexpr uint32_t type_remove = 0x2000;         // Type flag: remove the object when its reaction ends.
    constexpr uint32_t type_keep_last = 0x1000;      // Type flag: a flip-book keeps its last frame.

    struct Prop {
        rush2::track2049::PropRecord rec;
        uint32_t record = 0;
        uint32_t node = 0;
        float pinv[9];          // Inverse of the parent's matrix.
        float m[9], pos[3];     // World pose.
        float vel[3] = {}, spin[3] = {}, angles[3] = {};
        float radius = 0.0f;
        float timer = 0.0f, frames = 0.0f;
        int frame = 0;
        enum class State { idle, knocked, tipping, flipbook, done } state = State::idle;
    };

    std::mutex props_mutex;
    bool setup_pending = false;
    bool active = false;
    std::vector<Prop> props;
    std::vector<rush2::track2049::PropRecord> prop_records;
    std::map<std::string, std::pair<uint16_t, float>> models;   // Converted geometry: name -> (index, radius).
    // From 2049's main data, read once per ROM.
    std::shared_ptr<const std::vector<uint8_t>> types_rom;
    std::vector<movers::TypeInfo> types;
    std::vector<std::string> handle_names;   // 2049 model handle table names.
    float sign_angles[3][3] = {};            // Kind 0 parameters of sub-kinds 0-2.

    uint32_t be32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    float bef(const uint8_t* p) {
        uint32_t w = be32(p);
        float f;
        memcpy(&f, &w, 4);
        return f;
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

    std::string read_name(uint8_t* rdram, uint32_t addr) {
        std::string s;
        for (int i = 0; i < 16; i++) {
            char c = (char)MEM_B(0, (int32_t)(addr + i));
            if (c == 0) break;
            s.push_back(c);
        }
        return s;
    }

    bool load_types() {
        auto rom = rush2::wings::get_rom();
        if (rom == nullptr) {
            return false;
        }
        if (rom == types_rom && !types.empty()) {
            return true;
        }
        std::vector<uint8_t> main;
        if (rom->size() <= main_rom || !rush2::assets::inflate_raw(rom->data() + main_rom, rom->size() - main_rom, main) ||
            !movers::parse_types(main.data(), main.size(), main_vram, types)) {
            types.clear();
            return false;
        }
        auto in_main = [&](uint32_t addr, size_t n) { return addr >= main_vram && addr - main_vram + n <= main.size(); };
        handle_names.clear();
        for (uint32_t i = 0; i < handle_count && in_main(model_names_2049 + i * 4, 4); i++) {
            uint32_t s = be32(&main[model_names_2049 + i * 4 - main_vram]);
            if (!in_main(s, 1)) {
                handle_names.emplace_back();
                continue;
            }
            const char* c = (const char*)&main[s - main_vram];
            handle_names.emplace_back(c, strnlen(c, main.size() - (s - main_vram)));
        }
        uint32_t params = in_main(kind_params, 4) ? be32(&main[kind_params - main_vram]) : 0;
        uint8_t size = in_main(kind_param_sizes, 1) ? main[kind_param_sizes - main_vram] : 0;
        for (int sub = 0; sub < 3; sub++) {
            for (int a = 0; a < 3; a++) {
                uint32_t at = params + sub * size + a * 4;
                sign_angles[sub][a] = params != 0 && in_main(at, 4) ? bef(&main[at - main_vram]) : 0.0f;
            }
        }
        types_rom = rom;
        return true;
    }

    // Rush 2 model container: names 0x18 {name[16], f32 radius, u16 kind, u16 0}, in handle order.
    void read_models(const std::vector<uint8_t>& g) {
        models.clear();
        if (g.size() < 0x14) {
            return;
        }
        uint32_t names = be32(&g[4]), count = be32(&g[16]);
        for (uint32_t i = 0; i < count && names + (i + 1) * 0x18 <= g.size(); i++) {
            const char* c = (const char*)&g[names + i * 0x18];
            models[std::string(c, strnlen(c, 16))] = { (uint16_t)i, bef(&g[names + i * 0x18 + 16]) };
        }
    }

    void invert(const float* P, float* out) {
        float a = P[0], b = P[1], c = P[2], d = P[3], e = P[4], f = P[5], g = P[6], h = P[7], i = P[8];
        float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        if (std::fabs(det) < 1e-12f) {
            for (int k = 0; k < 9; k++) out[k] = k % 4 == 0 ? 1.0f : 0.0f;
            return;
        }
        float inv[9] = { (e * i - f * h), (c * h - b * i), (b * f - c * e),
                         (f * g - d * i), (a * i - c * g), (c * d - a * f),
                         (d * h - e * g), (b * g - a * h), (a * e - b * d) };
        for (int k = 0; k < 9; k++) out[k] = inv[k] / det;
    }

    // func_8008B4C4: an orientation whose z row points along v, with a level x row.
    void facing(const float v[3], float m[9]) {
        float len = std::sqrt(v[2] * v[2] + (v[0] * v[0] + v[1] * v[1]));
        float z[3] = { 0.0f, 0.0f, 1.0f };
        if (len > 0.0f) {
            for (int i = 0; i < 3; i++) z[i] = v[i] * (1.0f / len);
        }
        float x[3] = { v[2], 0.0f, -v[0] };
        float xl = std::sqrt(x[2] * x[2] + (x[0] * x[0] + x[1] * x[1]));
        if (xl <= 0.01f) {
            x[0] = 1.0f; x[1] = 0.0f; x[2] = 0.0f;
        }
        else {
            for (float& c : x) c *= 1.0f / xl;
        }
        float y[3] = { z[1] * x[2] - x[1] * z[2], z[2] * x[0] - x[2] * z[0], z[0] * x[1] - x[0] * z[1] };
        x[0] = y[1] * z[2] - z[1] * y[2];
        x[1] = y[2] * z[0] - z[2] * y[0];
        x[2] = y[0] * z[1] - z[0] * y[1];
        for (int i = 0; i < 3; i++) {
            m[i] = x[i];
            m[3 + i] = y[i];
            m[6 + i] = z[i];
        }
    }

    // The prop's world pose into its record, relative to the parent record (row vectors: world = local x P + p).
    void write_pose(uint8_t* rdram, const Prop& p) {
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = p.pos[i] - p.rec.parent_pos[i];
        for (int c = 0; c < 3; c++) {
            write_f(rdram, p.record + 0x34 + c * 4, d[0] * p.pinv[c] + d[1] * p.pinv[3 + c] + d[2] * p.pinv[6 + c]);
            for (int r = 0; r < 3; r++) {
                float v = p.m[r * 3] * p.pinv[c] + p.m[r * 3 + 1] * p.pinv[3 + c] + p.m[r * 3 + 2] * p.pinv[6 + c];
                write_f(rdram, p.record + 0x10 + (r * 3 + c) * 4, v);
            }
        }
    }

    void hide(uint8_t* rdram, const Prop& p) {
        if (p.node != 0) {
            MEM_W(0, (int32_t)p.node) = (uint32_t)MEM_W(0, (int32_t)p.node) | node_hidden;
        }
    }

    void set_model(uint8_t* rdram, const Prop& p, const std::string& name) {
        auto it = models.find(name.substr(0, 15));
        if (p.node == 0 || it == models.end()) {
            return;
        }
        uint16_t slot_bits = uint16_t(MEM_HU(0, (int32_t)(p.node + 0xC)) & 0xFC00);
        MEM_H(0, (int32_t)(p.node + 0xC)) = uint16_t(slot_bits | it->second.first);
    }

    bool setup(uint8_t* rdram) {
        props.clear();
        if (prop_records.empty()) {
            return false;
        }
        if (!load_types()) {
            fprintf(stderr, "[2049] Props: can't read Rush 2049's type table\n");
            return false;
        }
        bool backward = MEM_B(0, (int32_t)backward_flag) != 0;
        uint32_t base = (uint32_t)MEM_W(0, (int32_t)record_base);
        int count = (int)MEM_W(0, (int32_t)node_count);
        std::map<uint32_t, uint32_t> node_of_matrix;
        for (int i = 0; i < count; i++) {
            uint32_t node = nodes + i * node_size;
            node_of_matrix[(uint32_t)MEM_W(0, (int32_t)(node + 4))] = node;
        }
        for (const auto& rec : prop_records) {
            if (rec.type < 0 || rec.type >= (int)types.size()) {
                continue;
            }
            Prop p;
            p.rec = rec;
            p.record = base + rec.record * record_size;
            auto n = node_of_matrix.find(p.record + 0x10);
            p.node = n == node_of_matrix.end() ? 0 : n->second;
            invert(rec.parent_m, p.pinv);
            memcpy(p.m, rec.m, sizeof(p.m));
            memcpy(p.pos, rec.pos, sizeof(p.pos));
            // Radius: the type's, or the model's (func_800ABB58) when the type has -1.
            const movers::TypeInfo& t = types[rec.type];
            p.radius = t.param;
            if (t.param == -1.0f) {
                auto m = models.find(read_name(rdram, p.record));
                p.radius = m == models.end() ? 0.0f : m->second.second;
            }
            bool present = rec.direction == 0 || (rec.direction == 1) != backward;
            if (!present) {
                hide(rdram, p);
                p.state = Prop::State::done;
            }
            props.push_back(p);
        }
        fprintf(stderr, "[2049] %zu props\n", props.size());
        return !props.empty();
    }

    // A car hit the prop (the type's init function).
    void hit(uint8_t* rdram, Prop& p, uint32_t car) {
        const movers::TypeInfo& t = types[p.rec.type];
        float v[3];
        for (int i = 0; i < 3; i++) v[i] = read_f(rdram, car + car_snap_vel + i * 4);
        if (t.init == init_knock) {
            for (int i = 0; i < 3; i++) p.vel[i] = v[i] * launch_scale;
            facing(p.vel, p.m);
            p.vel[1] = p.vel[1] + ((t.anim == 0x153 || t.anim == 0xED) ? 1.0f : 2.0f);
            memcpy(p.spin, knock_spin, sizeof(p.spin));
            p.timer = knock_seconds;
            p.state = Prop::State::knocked;
        }
        else if (t.init == init_sign) {
            facing(v, p.m);
            int sub = t.sub < 3 ? t.sub : 0;
            memcpy(p.angles, sign_angles[sub], sizeof(p.angles));
            p.frames = sign_frames;
            p.state = Prop::State::tipping;
        }
        else if (t.init == init_cactus) {
            for (int i = 0; i < 3; i++) v[i] *= launch_scale;
            facing(v, p.m);
            p.frame = 0;
            p.timer = flipbook_delay;
            p.state = Prop::State::flipbook;
        }
        else {
            p.state = Prop::State::done;
            return;
        }
        write_pose(rdram, p);
        rush2::track2049::play_effect(rdram, t.sounds[0], p.pos, t.sound_range);
    }

    void update(uint8_t* rdram, Prop& p, float dt) {
        const movers::TypeInfo& t = types[p.rec.type];
        float frames = dt * frame_rate;
        switch (p.state) {
            case Prop::State::knocked: {
                for (int i = 0; i < 3; i++) {
                    float g = gravity[i] * gravity_scale;
                    p.vel[i] = p.vel[i] + g * frames;
                    p.pos[i] = p.pos[i] + (p.vel[i] + g) * frames;
                }
                float a[3] = { p.spin[0] * dt, p.spin[1] * dt, p.spin[2] * dt };
                movers::rotate_angles(p.m, a);
                write_pose(rdram, p);
                p.timer -= dt;
                if (p.timer <= 0.0f) {
                    if (t.flags & type_remove) {
                        hide(rdram, p);
                    }
                    p.state = Prop::State::done;
                }
                break;
            }
            case Prop::State::tipping: {
                float step = std::min(frames, p.frames);
                float a[3] = { p.angles[0] * step, p.angles[1] * step, p.angles[2] * step };
                movers::rotate_angles(p.m, a);
                write_pose(rdram, p);
                p.frames -= step;
                if (p.frames <= 0.0f) {
                    p.state = Prop::State::done;
                }
                break;
            }
            case Prop::State::flipbook: {
                p.timer -= dt;
                if (p.timer > 0.0f) {
                    break;
                }
                p.frame++;
                p.timer = flipbook_period;
                if (p.frame >= cactus_frames) {
                    if (t.flags & type_keep_last) {
                        p.state = Prop::State::done;
                        break;
                    }
                    if (t.flags & type_remove) {
                        hide(rdram, p);
                        p.state = Prop::State::done;
                        break;
                    }
                    p.frame = 0;
                }
                int index = t.anim + p.frame;
                if (t.anim >= 0 && index < (int)handle_names.size()) {
                    set_model(rdram, p, index == t.anim ? read_name(rdram, p.record) : handle_names[index]);
                }
                break;
            }
            default:
                break;
        }
    }

    bool racing(uint8_t* rdram) {
        int slot = rush2::track2049::loaded_slot();
        return slot >= 0 && MEM_B(0, (int32_t)track_id) == slot;
    }
}

void rush2::track2049::set_prop_data(const std::vector<PropRecord>& records, const std::vector<uint8_t>& converted_geometry) {
    std::lock_guard lock{ props_mutex };
    prop_records = records;
    read_models(converted_geometry);
}

void rush2::track2049::reset_props() {
    std::lock_guard lock{ props_mutex };
    setup_pending = true;
    active = false;
}

void rush2::track2049::props_tick(uint8_t* rdram, float dt) {
    std::lock_guard lock{ props_mutex };
    if (!racing(rdram)) {
        active = false;
        return;
    }
    if (setup_pending) {
        setup_pending = false;
        active = setup(rdram);
    }
    if (!active) {
        return;
    }
    for (Prop& p : props) {
        update(rdram, p, dt);
    }
}

// Car callback slot 0 (0x8010C6C8): the car's position within the prop's radius + 3.5 of the prop.
void rush2::track2049::props_car(uint8_t* rdram, uint32_t car) {
    std::lock_guard lock{ props_mutex };
    if (!active || !racing(rdram)) {
        return;
    }
    int index = (int)((car - cars) / car_size);
    if (index < 0 || index >= max_cars || MEM_H(0, (int32_t)(car + car_active)) == 0) {
        return;
    }
    // Ghost cars (src/ghost.cpp) pass through props, as through cars.
    if (rush2::ghost::is_ghost_car(index)) {
        return;
    }
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = read_f(rdram, car + car_snap_pos + i * 4);
    for (Prop& p : props) {
        if (p.state != Prop::State::idle) {
            continue;
        }
        float dx = c[0] - p.pos[0], dy = c[1] - p.pos[1], dz = c[2] - p.pos[2];
        float r = p.radius + hit_margin;
        if (dz * dz + (dx * dx + dy * dy) - r * r <= 0.0f) {
            hit(rdram, p, car);
        }
    }
}
