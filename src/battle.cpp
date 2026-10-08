// Rush 2049's battle mode in Rush 2 (docs/rush2049_research/battle.md).
//
// The battle arenas (track select ids 30-37, src/track2049_menu.cpp) are hosted by Rush 2's stunt track, so the race
// runs as Rush 2's stunt mode (free roam, a clock, no checkpoints). Rush 2049's battle was multiplayer only, so there
// are no computer cars yet (writing their AI is future work). Rush 2049 runs its battle in a mode overlay (ROM 0xB6FEC4, loaded at 0x8038A400)
// on top of its race code; this file does what that overlay does, on Rush 2's cars:
// - Health: 800 per car (the HEAL pickup sets 0x320). A car at 0 is wrecked (car +0x648, which Rush 2's own wreck and
//   respawn code then handles) and its last attacker scores a kill; the respawned car starts at full health with no
//   weapon.
// - Pickups: each arena places 8 weapon pickups (WEPICON_CANN, GATT, GREN, MINE, MISS, RAM, ROCK, SONC) and several
//   POWUP pickups (a random one of HEAL, INVS, SHLD). The converter keeps their records (PickupRecord) and this file
//   hides a pickup that a car drove into and shows it again after a while. A weapon pickup gives the weapon and its
//   ammo (2049's table 0x80121D60: 20, 100, 20, 3, 3, 5, 20, 5); the same weapon again adds the ammo. INVS makes the
//   car invisible to homing for 30 s (2049: flag 0x38C & 1, 30 s) and SHLD scales the damage taken by 0.2 (2049
//   0x80394DC4).
// - Weapons: the FIRE button is HORN (C-right in the game's layout) in battle arenas. Projectiles and effects are
//   records of a pool in the converted placement (battle_pool_size, hidden at the origin) whose model is swapped and
//   pose written each tick, like the props (src/track2049_props.cpp). Damage falls off with the square of the distance
//   from an explosion, (1 - d^2 / r^2)^2 x damage (2049 func_8038D798).
// Values 2049 keeps in code or tables not read yet (speeds, damages, cooldowns, durations) are marked [I].

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "recomp.h"
#include "battle.h"
#include "ghost.h"
#include "rush2.h"
#include "track2049.h"

extern "C" void hud_widget_alloc_800541F8(uint8_t* rdram, recomp_context* ctx);   // Allocates a 2D widget.
extern "C" void widgets_create_800604FC(uint8_t* rdram, recomp_context* ctx);   // Builds HUD elements from layout entries.
extern "C" void text_select_style_800737E4(uint8_t* rdram, recomp_context* ctx);   // Selects a text style.
extern "C" void text_center_x_800734AC(uint8_t* rdram, recomp_context* ctx);   // x of a string centered on $a1.
extern "C" void text_print_string_800734E0(uint8_t* rdram, recomp_context* ctx);   // Prints a string at (x, y).

namespace {
    using rush2::track2049::PickupRecord;
    void update_hud(uint8_t* rdram);

    // Game addresses (Rush 2).
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t cars = 0x800F5470;
    constexpr uint32_t car_size = 0x81C;
    constexpr int max_cars = 8;
    constexpr uint32_t player_slots = 0x800C2140;    // 0x28 per player: +0 car index, +2 buttons pressed, +4 held.
    constexpr uint32_t player_size = 0x28;
    constexpr uint32_t num_players = 0x8010C3E2;     // s16
    constexpr uint32_t record_base = 0x8010C15C;     // Pointer to the placement file's first record.
    constexpr uint32_t record_size = 0x64;
    constexpr uint32_t nodes = 0x800D9E90;           // Scene nodes, 0x38 bytes each.
    constexpr uint32_t node_size = 0x38;
    constexpr uint32_t node_count = 0x800FAE58;
    constexpr uint32_t node_hidden = 0x400;
    constexpr uint32_t time_allowed = 0x8010C204;    // f32: the race's clock in seconds.
    constexpr uint32_t widget_array = 0x802F6C00;    // 2D widgets, 0x20 each (src/hud.cpp).
    constexpr uint32_t widget_size = 0x20;

    // Car fields (docs/rush2049_research/movers.md).
    constexpr uint32_t car_force = 0x11C;            // f32[3] external force, car-local, cleared each step
    constexpr uint32_t car_velocity = 0x218;         // f32[3] world
    constexpr uint32_t car_position = 0x224;         // f32[3] world
    constexpr uint32_t car_matrix = 0x2CC;           // f32[9]: rows right, up, forward
    constexpr uint32_t car_body_mass = 0x5B0;
    constexpr uint32_t car_wrecked = 0x648;          // s8
    constexpr uint32_t car_radius = 0x658;
    constexpr uint32_t car_active = 0x7E4;           // s16
    constexpr uint32_t car_kind = 0x7E8;             // u8: 1 drone, 2 human

    constexpr uint16_t button_fire = 0x0001;         // C-right: HORN in the game's layout (src/controls.cpp)

    // 2049 values.
    constexpr float max_health = 800.0f;                    // 0x320 [V]
    constexpr int weapon_ammo[8] = { 20, 100, 20, 3, 3, 5, 20, 5 };   // 0x80121D60 [V]
    constexpr float pickup_radius = 6.0f;                   // type table param [V]
    constexpr float invisible_seconds = 30.0f;              // [V]
    constexpr float shield_factor = 0.2f;                   // 0x80394DC4 [V]

    enum Weapon { cannon, gatling, grenade, mine, missile, ram, rocket, sonic, heal, invisibility, shield, powerup };
    const char* const weapon_names[8] = { "CANNON", "GATLING", "GRENADE", "MINES", "MISSILES", "RAM", "ROCKETS", "SONIC" };

    struct WeaponInfo {
        float speed;        // ft/s (muzzle speed)
        float life;         // seconds before it explodes or vanishes
        float cooldown;     // seconds between shots
        float radius;       // explosion radius, 0 = hit only
        float damage;       // at the center of an explosion, or per hit
        float hit;          // collision radius against a car
        const char* model;
    };
    // [I] approximate.
    const WeaponInfo weapons[8] = {
        { 380.0f, 2.0f, 0.7f, 10.0f, 300.0f, 5.0f, "WPR_CANNG1" },
        { 520.0f, 0.9f, 0.08f, 0.0f, 22.0f, 4.0f, "WFX_TRACERG1" },
        { 110.0f, 2.2f, 1.0f, 20.0f, 450.0f, 5.0f, "WPR_GRENG1" },
        { 0.0f, 60.0f, 1.0f, 16.0f, 600.0f, 9.0f, "WPR_MINEBASE" },
        { 230.0f, 4.0f, 1.2f, 12.0f, 450.0f, 5.0f, "WPR_MISSG1" },
        { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, "" },
        { 300.0f, 2.5f, 0.5f, 8.0f, 260.0f, 5.0f, "WPR_ROCKG1" },
        { 0.0f, 0.4f, 1.5f, 70.0f, 220.0f, 0.0f, "WFX_SNCBLSTG1" },
    };
    constexpr float pickup_respawn = 20.0f;      // [I]
    constexpr float shield_seconds = 20.0f;      // [I]
    constexpr float gravity = -70.0f;            // [I] grenade ft/s^2
    constexpr float mine_arm = 1.0f;
    constexpr float homing_range = 450.0f;
    constexpr float homing_turn = 2.4f;          // rad/s [I]
    constexpr float ram_damage_base = 160.0f;    // 2049 func_800CE358: 160 + 7 x speed [V]; speed scaled here [I]

    struct Fighter {
        bool present = false;
        float health = max_health;
        int kills = 0, deaths = 0;
        int weapon = -1, ammo = 0;
        float cooldown = 0.0f;
        float invisible = 0.0f, shield = 0.0f;
        int last_attacker = -1;
        bool was_wrecked = false;
        float push[3] = {};     // World velocity change still to apply
        float push_time = 0.0f;
    };

    struct Pickup {
        PickupRecord rec;
        uint32_t record = 0, node = 0;
        float respawn = 0.0f;   // > 0: hidden until it runs out
    };

    struct Shot {
        bool live = false;
        int owner = -1, weapon = 0, slot = -1;
        float pos[3] = {}, vel[3] = {};
        float age = 0.0f;
        int target = -1;
    };

    struct Effect {
        bool live = false;
        int slot = -1;
        float pos[3] = {};
        float age = 0.0f, duration = 0.3f, scale0 = 1.0f, scale1 = 4.0f;
    };

    struct PoolSlot {
        uint32_t record = 0, node = 0;
        bool used = false;
    };

    std::mutex battle_mutex;
    std::vector<PickupRecord> pickup_records;
    std::vector<int> pool_records;
    std::map<std::string, uint16_t> models;     // Converted geometry: name -> handle index.
    bool setup_pending = false, ready = false;
    Fighter fighters[max_cars];
    std::vector<Pickup> pickups;
    std::vector<PoolSlot> pool;
    std::vector<Shot> shots;
    std::vector<Effect> effects;
    std::mt19937 rng{ 2049 };
    float clock_seconds = 0.0f;
    bool clock_set = false;
    uint32_t built_head = 0;     // The HUD element list the battle HUD was built for.
    std::atomic_int limit_option = 3;

    // ------------------------------------------------------------------------------------------------------------
    // Memory helpers

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

    uint32_t be32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    float bef(const uint8_t* p) {
        uint32_t w = be32(p);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Vectors

    float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    float length(const float a[3]) { return std::sqrt(dot(a, a)); }

    void normalize(float a[3]) {
        float l = length(a);
        if (l > 1e-6f) {
            for (int i = 0; i < 3; i++) a[i] /= l;
        }
    }

    uint32_t car_at(int i) { return cars + (uint32_t)i * car_size; }

    void car_pos(uint8_t* rdram, int i, float out[3]) {
        for (int k = 0; k < 3; k++) out[k] = read_f(rdram, car_at(i) + car_position + k * 4);
    }

    void car_vel(uint8_t* rdram, int i, float out[3]) {
        for (int k = 0; k < 3; k++) out[k] = read_f(rdram, car_at(i) + car_velocity + k * 4);
    }

    // The car's orientation rows: right, up, forward.
    void car_axes(uint8_t* rdram, int i, float m[9]) {
        for (int k = 0; k < 9; k++) m[k] = read_f(rdram, car_at(i) + car_matrix + k * 4);
    }

    bool is_wrecked(uint8_t* rdram, int i) { return MEM_B(0, (int32_t)(car_at(i) + car_wrecked)) != 0; }

    // ------------------------------------------------------------------------------------------------------------
    // Placement records

    uint32_t record_pointer(uint8_t* rdram, int record) {
        return (uint32_t)MEM_W(0, (int32_t)record_base) + (uint32_t)record * record_size;
    }

    void hide_node(uint8_t* rdram, uint32_t node) {
        if (node != 0) MEM_W(0, (int32_t)node) = (uint32_t)MEM_W(0, (int32_t)node) | node_hidden;
    }

    void show_node(uint8_t* rdram, uint32_t node) {
        if (node != 0) MEM_W(0, (int32_t)node) = (uint32_t)MEM_W(0, (int32_t)node) & ~node_hidden;
    }

    void set_model(uint8_t* rdram, uint32_t node, const std::string& name) {
        auto it = models.find(name.substr(0, 15));
        if (node == 0 || it == models.end()) return;
        uint16_t slot_bits = uint16_t(MEM_HU(0, (int32_t)(node + 0xC)) & 0xFC00);
        MEM_H(0, (int32_t)(node + 0xC)) = uint16_t(slot_bits | it->second);
    }

    // A top-level record's world pose: rows of m are the object's axes.
    void write_pose(uint8_t* rdram, uint32_t record, const float m[9], const float pos[3]) {
        for (int k = 0; k < 9; k++) write_f(rdram, record + 0x10 + k * 4, m[k]);
        for (int k = 0; k < 3; k++) write_f(rdram, record + 0x34 + k * 4, pos[k]);
    }

    // An orientation with its z row along `forward` and a level x row.
    void facing(const float forward[3], float m[9]) {
        float z[3] = { forward[0], forward[1], forward[2] };
        normalize(z);
        float x[3] = { z[2], 0.0f, -z[0] };
        if (length(x) < 0.01f) {
            x[0] = 1.0f; x[1] = 0.0f; x[2] = 0.0f;
        }
        normalize(x);
        float y[3] = { z[1] * x[2] - x[1] * z[2], z[2] * x[0] - x[2] * z[0], z[0] * x[1] - x[0] * z[1] };
        for (int i = 0; i < 3; i++) {
            m[i] = x[i];
            m[3 + i] = y[i];
            m[6 + i] = z[i];
        }
    }

    void read_models(const std::vector<uint8_t>& g) {
        models.clear();
        if (g.size() < 0x14) return;
        uint32_t names = be32(&g[4]), count = be32(&g[16]);
        for (uint32_t i = 0; i < count && names + (i + 1) * 0x18 <= g.size(); i++) {
            const char* c = (const char*)&g[names + i * 0x18];
            models[std::string(c, strnlen(c, 16))] = (uint16_t)i;
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Pool

    // Pool slots 0-15 are for projectiles and effects, 16-23 for the weapons mounted on the cars (one per car) and 24-27
    // for the HUD's rotating weapon (one per local player).
    constexpr int shot_slots = 16;
    constexpr int hud_slot_first = 24;

    int alloc_slot() {
        for (size_t i = 0; i < pool.size() && i < (size_t)shot_slots; i++) {
            if (!pool[i].used) {
                pool[i].used = true;
                return (int)i;
            }
        }
        return -1;
    }

    void free_slot(uint8_t* rdram, int slot) {
        if (slot < 0 || slot >= (int)pool.size()) return;
        pool[slot].used = false;
        hide_node(rdram, pool[slot].node);
    }

    void place_slot(uint8_t* rdram, int slot, const char* model, const float m[9], const float pos[3]) {
        if (slot < 0 || slot >= (int)pool.size()) return;
        set_model(rdram, pool[slot].node, model);
        write_pose(rdram, pool[slot].record, m, pos);
        show_node(rdram, pool[slot].node);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Setup

    bool battle_race(uint8_t* rdram) {
        int slot = rush2::track2049::loaded_slot();
        return slot >= 0 && MEM_B(0, (int32_t)track_id) == slot && rush2::track2049::battle_arena() > 0;
    }

    void setup(uint8_t* rdram) {
        pickups.clear();
        pool.clear();
        shots.clear();
        effects.clear();
        for (Fighter& f : fighters) f = Fighter{};
        int count = (int)MEM_W(0, (int32_t)node_count);
        std::map<uint32_t, uint32_t> node_of_matrix;
        for (int i = 0; i < count; i++) {
            uint32_t node = nodes + i * node_size;
            node_of_matrix[(uint32_t)MEM_W(0, (int32_t)(node + 4))] = node;
        }
        for (const PickupRecord& rec : pickup_records) {
            Pickup p;
            p.rec = rec;
            p.record = record_pointer(rdram, rec.record);
            auto n = node_of_matrix.find(p.record + 0x10);
            p.node = n == node_of_matrix.end() ? 0 : n->second;
            pickups.push_back(p);
        }
        for (int record : pool_records) {
            PoolSlot s;
            s.record = record_pointer(rdram, record);
            auto n = node_of_matrix.find(s.record + 0x10);
            s.node = n == node_of_matrix.end() ? 0 : n->second;
            hide_node(rdram, s.node);
            pool.push_back(s);
        }
        int missing = 0;
        for (const Pickup& p : pickups) missing += p.node == 0;
        for (const PoolSlot& s : pool) missing += s.node == 0;
        fprintf(stderr, "[Battle] %zu pickups, %zu pool records (%d without a scene node)\n", pickups.size(), pool.size(), missing);
        clock_seconds = 0.0f;
        clock_set = false;
        ready = true;
        if (const char* give = getenv("R2_BATTLE_GIVE")) {
            // Test aid: the first car starts with weapon R2_BATTLE_GIVE.
            fighters[0].present = true;
            fighters[0].weapon = atoi(give);
            fighters[0].ammo = 99;
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Damage

    // The 2D HUD reads these.
    int player_car(uint8_t* rdram, int player) {
        return (int8_t)MEM_B(0, (int32_t)(player_slots + player * player_size));
    }

    void damage(uint8_t* rdram, int victim, int attacker, float amount) {
        Fighter& f = fighters[victim];
        if (!f.present || is_wrecked(rdram, victim) || amount <= 0.0f) return;
        if (f.shield > 0.0f) amount *= shield_factor;
        f.health -= amount;
        f.last_attacker = attacker;
        if (f.health <= 0.0f) {
            f.health = 0.0f;
            MEM_B(0, (int32_t)(car_at(victim) + car_wrecked)) = 1;
            f.deaths++;
            if (attacker >= 0 && attacker != victim && fighters[attacker].present) fighters[attacker].kills++;
            fprintf(stderr, "[Battle] car %d destroyed by car %d\n", victim, attacker);
        }
    }

    // Velocity change spread over the next fifth of a second.
    void push(uint8_t* rdram, int car, const float dv[3]) {
        Fighter& f = fighters[car];
        for (int i = 0; i < 3; i++) f.push[i] += dv[i];
        f.push_time = 0.2f;
    }

    void apply_pushes(uint8_t* rdram, float dt) {
        for (int i = 0; i < max_cars; i++) {
            Fighter& f = fighters[i];
            if (!f.present || f.push_time <= 0.0f) continue;
            float step = std::min(dt, f.push_time);
            float share = step / f.push_time;
            float m[9];
            car_axes(rdram, i, m);
            float mass = read_f(rdram, car_at(i) + car_body_mass);
            float world[3];
            for (int k = 0; k < 3; k++) {
                world[k] = f.push[k] * share * mass / std::max(dt, 1e-3f);
                f.push[k] -= f.push[k] * share;
            }
            for (int k = 0; k < 3; k++) {
                float local = m[k * 3] * world[0] + m[k * 3 + 1] * world[1] + m[k * 3 + 2] * world[2];
                uint32_t at = car_at(i) + car_force + k * 4;
                write_f(rdram, at, read_f(rdram, at) + local);
            }
            f.push_time -= step;
        }
    }

    void explode(uint8_t* rdram, const float pos[3], float radius, float amount, int owner, float knock) {
        for (int i = 0; i < max_cars; i++) {
            if (!fighters[i].present || is_wrecked(rdram, i) || i == owner) continue;
            float c[3];
            car_pos(rdram, i, c);
            float d[3] = { c[0] - pos[0], c[1] - pos[1], c[2] - pos[2] };
            float d2 = dot(d, d);
            if (d2 >= radius * radius) continue;
            float f = 1.0f - d2 / (radius * radius);
            damage(rdram, i, owner, f * f * amount);
            float away[3] = { d[0], d[1] + 0.3f * length(d), d[2] };
            normalize(away);
            float dv[3] = { away[0] * knock * f, away[1] * knock * f, away[2] * knock * f };
            push(rdram, i, dv);
        }
        int slot = alloc_slot();
        if (slot >= 0) {
            Effect e;
            e.live = true;
            e.slot = slot;
            memcpy(e.pos, pos, sizeof(e.pos));
            e.duration = 0.35f;
            e.scale0 = 0.5f;
            e.scale1 = std::max(2.0f, radius * 0.35f);
            effects.push_back(e);
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Weapons

    void spawn_shot(uint8_t* rdram, int owner, int weapon) {
        const WeaponInfo& w = weapons[weapon];
        float c[3], v[3], m[9];
        car_pos(rdram, owner, c);
        car_vel(rdram, owner, v);
        car_axes(rdram, owner, m);
        float forward[3] = { m[6], m[7], m[8] };
        Shot s;
        s.owner = owner;
        s.weapon = weapon;
        s.slot = alloc_slot();
        if (s.slot < 0) return;
        float muzzle = weapon == mine ? -8.0f : 8.0f;
        for (int i = 0; i < 3; i++) s.pos[i] = c[i] + forward[i] * muzzle + m[3 + i] * 1.5f;
        if (weapon == mine) {
            for (int i = 0; i < 3; i++) s.vel[i] = 0.0f;
        }
        else {
            for (int i = 0; i < 3; i++) s.vel[i] = forward[i] * w.speed + v[i] * 0.8f;
            if (weapon == grenade) s.vel[1] += 45.0f;
        }
        s.live = true;
        shots.push_back(s);
    }

    int nearest_enemy(uint8_t* rdram, int owner, const float from[3], const float dir[3], float range, float min_dot) {
        int best = -1;
        float best_d = range;
        for (int i = 0; i < max_cars; i++) {
            if (i == owner || !fighters[i].present || is_wrecked(rdram, i) || fighters[i].invisible > 0.0f) continue;
            float c[3];
            car_pos(rdram, i, c);
            float d[3] = { c[0] - from[0], c[1] - from[1], c[2] - from[2] };
            float dist = length(d);
            if (dist < 1.0f || dist >= best_d) continue;
            if (dot(d, dir) / dist < min_dot) continue;
            best = i;
            best_d = dist;
        }
        return best;
    }

    bool fire(uint8_t* rdram, int car) {
        Fighter& f = fighters[car];
        if (f.weapon < 0 || f.ammo <= 0 || f.cooldown > 0.0f || is_wrecked(rdram, car) || f.weapon == ram) return false;
        const WeaponInfo& w = weapons[f.weapon];
        fprintf(stderr, "[Battle] car %d fires %s (%d left)\n", car, weapon_names[f.weapon], f.ammo - 1);
        f.cooldown = w.cooldown;
        f.ammo--;
        if (f.weapon == sonic) {
            float c[3];
            car_pos(rdram, car, c);
            explode(rdram, c, w.radius, w.damage, car, 45.0f);
        }
        else {
            spawn_shot(rdram, car, f.weapon);
        }
        if (f.ammo <= 0) f.weapon = -1;
        return true;
    }

    void update_shots(uint8_t* rdram, float dt) {
        for (Shot& s : shots) {
            if (!s.live) continue;
            const WeaponInfo& w = weapons[s.weapon];
            s.age += dt;
            float old[3] = { s.pos[0], s.pos[1], s.pos[2] };
            bool detonate = false;
            if (s.weapon == mine) {
                if (s.age > mine_arm) {
                    for (int i = 0; i < max_cars && !detonate; i++) {
                        if (i == s.owner && s.age < 4.0f) continue;
                        if (!fighters[i].present || is_wrecked(rdram, i)) continue;
                        float c[3];
                        car_pos(rdram, i, c);
                        float d[3] = { c[0] - s.pos[0], c[1] - s.pos[1], c[2] - s.pos[2] };
                        if (length(d) < w.hit) detonate = true;
                    }
                }
            }
            else {
                if (s.weapon == missile) {
                    float dir[3] = { s.vel[0], s.vel[1], s.vel[2] };
                    normalize(dir);
                    if (s.target < 0 || is_wrecked(rdram, s.target) || fighters[s.target].invisible > 0.0f) {
                        s.target = nearest_enemy(rdram, s.owner, s.pos, dir, homing_range, 0.5f);
                    }
                    if (s.target >= 0) {
                        float c[3];
                        car_pos(rdram, s.target, c);
                        float want[3] = { c[0] - s.pos[0], c[1] - s.pos[1], c[2] - s.pos[2] };
                        normalize(want);
                        float speed = length(s.vel);
                        float k = std::min(1.0f, homing_turn * dt);
                        for (int i = 0; i < 3; i++) dir[i] += (want[i] - dir[i]) * k;
                        normalize(dir);
                        for (int i = 0; i < 3; i++) s.vel[i] = dir[i] * speed;
                    }
                }
                if (s.weapon == grenade) s.vel[1] += gravity * dt;
                for (int i = 0; i < 3; i++) s.pos[i] += s.vel[i] * dt;
                // A hit: the segment passes within the shot's radius of an enemy car.
                float seg[3] = { s.pos[0] - old[0], s.pos[1] - old[1], s.pos[2] - old[2] };
                float seg_len2 = std::max(dot(seg, seg), 1e-6f);
                for (int i = 0; i < max_cars && !detonate; i++) {
                    if (!fighters[i].present || is_wrecked(rdram, i)) continue;
                    if (i == s.owner && s.age < 0.4f) continue;
                    float c[3];
                    car_pos(rdram, i, c);
                    float rel[3] = { c[0] - old[0], c[1] - old[1], c[2] - old[2] };
                    float t = std::clamp(dot(rel, seg) / seg_len2, 0.0f, 1.0f);
                    float closest[3] = { old[0] + seg[0] * t - c[0], old[1] + seg[1] * t - c[1], old[2] + seg[2] * t - c[2] };
                    float r = w.hit + std::max(read_f(rdram, car_at(i) + car_radius), 3.0f) * 0.5f;
                    if (length(closest) < r) {
                        if (w.radius <= 0.0f) {
                            damage(rdram, i, s.owner, w.damage);
                            s.live = false;
                            free_slot(rdram, s.slot);
                            break;
                        }
                        detonate = true;
                    }
                }
            }
            if (s.live && (s.age >= w.life || detonate)) {
                if (w.radius > 0.0f) explode(rdram, s.pos, w.radius, w.damage, s.owner, 30.0f);
                s.live = false;
                free_slot(rdram, s.slot);
            }
            if (s.live) {
                float m[9];
                float dir[3] = { s.vel[0], s.vel[1], s.vel[2] };
                if (length(dir) < 1e-3f) {
                    dir[0] = 0.0f; dir[1] = 0.0f; dir[2] = 1.0f;
                }
                facing(dir, m);
                place_slot(rdram, s.slot, w.model, m, s.pos);
            }
        }
        shots.erase(std::remove_if(shots.begin(), shots.end(), [](const Shot& s) { return !s.live; }), shots.end());
    }

    // The weapon a car holds is mounted above its roof (2049's WEP_* models) and turns toward the nearest enemy in
    // range, or along the car when there is none.
    const char* const mount_models[8] = { "WEP_CANNG1", "WEP_GATTG1", "WEP_GRENG1", "WEP_MINEG1", "WEP_MISSG1",
                                          "WEP_RAMG1", "WEP_ROCKG1", "WEP_SONCG1" };
    constexpr float mount_height = 2.4f;     // [I]
    constexpr float mount_range = 200.0f;    // [I]

    void update_mounts(uint8_t* rdram) {
        for (int i = 0; i < max_cars && shot_slots + i < (int)pool.size(); i++) {
            const Fighter& f = fighters[i];
            int slot = shot_slots + i;
            if (!f.present || f.weapon < 0 || f.weapon > 7 || is_wrecked(rdram, i) || f.invisible > 0.0f) {
                hide_node(rdram, pool[slot].node);
                continue;
            }
            float c[3], m[9];
            car_pos(rdram, i, c);
            car_axes(rdram, i, m);
            float pos[3] = { c[0] + m[3] * mount_height, c[1] + m[4] * mount_height, c[2] + m[5] * mount_height };
            float dir[3] = { m[6], m[7], m[8] };
            int target = nearest_enemy(rdram, i, c, dir, mount_range, -1.0f);
            if (target >= 0) {
                float t[3];
                car_pos(rdram, target, t);
                dir[0] = t[0] - pos[0];
                dir[1] = 0.0f;
                dir[2] = t[2] - pos[2];
            }
            float pose[9];
            facing(dir, pose);
            place_slot(rdram, slot, mount_models[f.weapon], pose, pos);
        }
    }

    // The HUD's weapon: Rush 2049 shows the weapon held as a rotating 3D model at the bottom left of the view. It is a
    // pool record in front of the camera (position +0x24, rows right +0, up +0xC and, although car_engines.cpp calls it
    // back, forward +0x18 of the view's 0x40 byte camera), so every view of a split screen draws it: with more than one player the HUD shows the weapon's icon
    // instead.
    constexpr uint32_t cameras = 0x800E79D0;
    constexpr float hud_distance = 13.0f, hud_x = -5.6f, hud_y = -3.9f;   // [I]
    float hud_spin = 0.0f;

    void update_hud_models(uint8_t* rdram, float dt) {
        int players = std::clamp<int>((int16_t)MEM_H(0, (int32_t)num_players), 1, 4);
        hud_spin += dt * 1.6f;
        for (int p = 0; p < 4 && hud_slot_first + p < (int)pool.size(); p++) {
            int slot = hud_slot_first + p;
            int car = p < players ? player_car(rdram, p) : -1;
            if (players != 1 || car < 0 || car >= max_cars || fighters[car].weapon < 0 || fighters[car].weapon > 7 ||
                is_wrecked(rdram, car)) {
                hide_node(rdram, pool[slot].node);
                continue;
            }
            uint32_t cam = cameras + (uint32_t)p * 0x40;
            float pos[3], right[3], up[3], back[3];
            for (int i = 0; i < 3; i++) {
                right[i] = read_f(rdram, cam + i * 4);
                up[i] = read_f(rdram, cam + 0xC + i * 4);
                back[i] = read_f(rdram, cam + 0x18 + i * 4);
                pos[i] = read_f(rdram, cam + 0x24 + i * 4) + back[i] * hud_distance + right[i] * hud_x + up[i] * hud_y;
            }
            // Turns about the camera's up axis.
            float c = std::cos(hud_spin), s = std::sin(hud_spin);
            float m[9];
            for (int i = 0; i < 3; i++) {
                m[i] = right[i] * c + back[i] * s;          // x row
                m[3 + i] = up[i];
                m[6 + i] = back[i] * c - right[i] * s;      // z row
            }
            place_slot(rdram, slot, mount_models[fighters[car].weapon], m, pos);
        }
    }

    void update_effects(uint8_t* rdram, float dt) {
        for (Effect& e : effects) {
            if (!e.live) continue;
            e.age += dt;
            if (e.age >= e.duration) {
                e.live = false;
                free_slot(rdram, e.slot);
                continue;
            }
            float k = e.age / e.duration;
            float scale = e.scale0 + (e.scale1 - e.scale0) * k;
            float m[9] = { scale, 0, 0, 0, scale, 0, 0, 0, scale };
            place_slot(rdram, e.slot, "WFX_MFLSHG11", m, e.pos);
        }
        effects.erase(std::remove_if(effects.begin(), effects.end(), [](const Effect& e) { return !e.live; }), effects.end());
    }

    // ------------------------------------------------------------------------------------------------------------
    // Pickups

    void give(uint8_t* rdram, int car, int kind) {
        Fighter& f = fighters[car];
        if (kind == powerup) {
            static const int options[3] = { heal, invisibility, shield };
            kind = options[std::uniform_int_distribution<int>(0, 2)(rng)];
        }
        switch (kind) {
            case heal: f.health = max_health; break;
            case invisibility: f.invisible = invisible_seconds; break;
            case shield: f.shield = shield_seconds; break;
            default:
                if (kind >= 0 && kind < 8) {
                    if (f.weapon == kind) f.ammo += weapon_ammo[kind];
                    else {
                        f.weapon = kind;
                        f.ammo = weapon_ammo[kind];
                    }
                    f.cooldown = std::max(f.cooldown, 0.3f);
                }
                break;
        }
    }

    void update_pickups(uint8_t* rdram, float dt) {
        for (Pickup& p : pickups) {
            if (p.respawn > 0.0f) {
                p.respawn -= dt;
                if (p.respawn <= 0.0f) show_node(rdram, p.node);
                continue;
            }
            for (int i = 0; i < max_cars; i++) {
                if (!fighters[i].present || is_wrecked(rdram, i)) continue;
                float c[3];
                car_pos(rdram, i, c);
                float d[3] = { c[0] - p.rec.pos[0], c[1] - p.rec.pos[1], c[2] - p.rec.pos[2] };
                float r = pickup_radius + 3.0f;
                if (dot(d, d) <= r * r) {
                    give(rdram, i, p.rec.kind);
                    p.respawn = pickup_respawn;
                    hide_node(rdram, p.node);
                    break;
                }
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Cars

    void update_cars(uint8_t* rdram, float dt) {
        int players = (int16_t)MEM_H(0, (int32_t)num_players);
        for (int i = 0; i < max_cars; i++) {
            Fighter& f = fighters[i];
            bool active = MEM_H(0, (int32_t)(car_at(i) + car_active)) != 0 && !rush2::ghost::is_ghost_car(i);
            if (active != f.present) {
                f.present = active;
                f.health = max_health;
            }
            if (!f.present) continue;
            bool wrecked = is_wrecked(rdram, i);
            if (f.was_wrecked && !wrecked) {
                // Respawned: full health, nothing in hand.
                f.health = max_health;
                f.weapon = -1;
                f.ammo = 0;
                f.invisible = f.shield = 0.0f;
            }
            if (wrecked && !f.was_wrecked) {
                f.weapon = -1;
                f.ammo = 0;
            }
            f.was_wrecked = wrecked;
            f.cooldown = std::max(0.0f, f.cooldown - dt);
            f.invisible = std::max(0.0f, f.invisible - dt);
            f.shield = std::max(0.0f, f.shield - dt);
            if (wrecked) continue;

            // Firing: players with the HORN button, computer cars when an enemy is in front of them.
            int player = -1;
            for (int p = 0; p < players && p < 4; p++) {
                if (player_car(rdram, p) == i) player = p;
            }
            if (player >= 0) {
                uint32_t rec = player_slots + player * player_size;
                uint16_t held = MEM_HU(0, (int32_t)(rec + 4));
                uint16_t pressed = MEM_HU(0, (int32_t)(rec + 2));
                bool auto_fire = f.weapon == gatling;
                if (((auto_fire ? held : pressed) & button_fire) != 0) fire(rdram, i);
            }
            // (Computer cars don't fight: Rush 2049's battle was multiplayer only.)

            // RAM: a car holding it damages the cars it runs into, using an ammo each time.
            if (f.weapon == ram && f.ammo > 0) {
                float c[3], v[3];
                car_pos(rdram, i, c);
                car_vel(rdram, i, v);
                for (int j = 0; j < max_cars; j++) {
                    if (j == i || !fighters[j].present || is_wrecked(rdram, j)) continue;
                    float o[3], ov[3];
                    car_pos(rdram, j, o);
                    car_vel(rdram, j, ov);
                    float d[3] = { o[0] - c[0], o[1] - c[1], o[2] - c[2] };
                    float reach = read_f(rdram, car_at(i) + car_radius) + read_f(rdram, car_at(j) + car_radius) + 1.0f;
                    if (dot(d, d) > reach * reach || f.cooldown > 0.0f) continue;
                    float rel[3] = { v[0] - ov[0], v[1] - ov[1], v[2] - ov[2] };
                    float closing = dot(rel, d) / std::max(length(d), 1e-3f);
                    if (closing < 20.0f) continue;
                    float mph = closing / 1.4667f;
                    damage(rdram, j, i, ram_damage_base + 4.0f * mph);
                    f.ammo--;
                    f.cooldown = 1.0f;
                    if (f.ammo <= 0) f.weapon = -1;
                    break;
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------------------------------------------
// Interface

void rush2::battle::set_time_limit(TimeLimit limit) {
    limit_option = (int)limit;
}

int rush2::battle::time_limit_seconds() {
    return limit_option * 60;
}

void rush2::battle::set_data(const std::vector<PickupRecord>& pickup_list, const std::vector<int>& pool_list,
                             const std::vector<uint8_t>& converted_geometry) {
    std::lock_guard lock{ battle_mutex };
    pickup_records = pickup_list;
    pool_records = pool_list;
    read_models(converted_geometry);
}

void rush2::battle::reset() {
    std::lock_guard lock{ battle_mutex };
    built_head = 0;
    setup_pending = true;
    ready = false;
}

bool rush2::battle::active(uint8_t* rdram) {
    return ready && battle_race(rdram);
}

void rush2::battle::tick(uint8_t* rdram, float dt) {
    std::lock_guard lock{ battle_mutex };
    if (!battle_race(rdram)) {
        ready = false;
        return;
    }
    if (setup_pending) {
        setup_pending = false;
        setup(rdram);
    }
    if (!ready) return;
    // The stunt clock: 0x8010C204 holds the time allowed, which the HUD counts down from. It is the start countdown's
    // 3.5 s until the race begins (writing it early would stretch the countdown) and then 300, which becomes the limit.
    if (!clock_set && read_f(rdram, time_allowed) >= 60.0f) {
        write_f(rdram, time_allowed, (float)time_limit_seconds());
        clock_set = true;
    }
    clock_seconds += dt;
    if (getenv("R2_BATTLE_TEST") && clock_seconds > 6.0f && clock_seconds < 6.0f + dt) {
        // Test aid: car 0 is destroyed (as if by car 1) six seconds in.
        fighters[1].present = true;
        damage(rdram, 0, 1, 900.0f);
    }
    update_cars(rdram, dt);
    update_pickups(rdram, dt);
    update_shots(rdram, dt);
    update_effects(rdram, dt);
    update_mounts(rdram);
    update_hud_models(rdram, dt);
    apply_pushes(rdram, dt);
    update_hud(rdram);
}

// ----------------------------------------------------------------------------------------------------------------
// HUD
//
// The battle HUD is built from Rush 2049's own images, which the converter merges into the arena's geometry
// (files 63 and 76, so Rush 2's HUD finds them by name like its own images): HEALTHBG (a 64 x 8 frame) with HEALTHBAR
// (64 x 4, drawn cropped to the health left) inside it, WEAPONHUD (128 x 16: the 8 weapons' 16 x 16 icons side by
// side, cropped to the weapon held), and BCOIN_<color> (a 16 x 16 coin: 2049 counts a battle's kills as coins, "X n").
// They are elements of Rush 2's HUD layout system (func_800604FC builds an element, and its 2D widget, from a 0x28
// byte layout entry whose +0 is the image's name): static ones, whose widgets this file crops and hides each tick.
// Text (the kill count and ammo) is printed after the widget loop with Rush 2's font. The stunt score panel is hidden
// (rush2_battle_hide_stunt_panel).
namespace {
    constexpr uint32_t speedometer_callback = 0x800BA2A8;
    constexpr uint32_t element_pool = 0x802F6400;     // Pointers to the HUD elements (func_80060418; moved by src/players4.cpp), 0xC8 at most
    constexpr uint32_t element_count = 0x80125A58;
    constexpr uint32_t hud_element_list = 0x8010C030; // Head of the HUD's element list (+0x3C next).
    constexpr uint32_t hud_entries = 0x8024F600;     // Layout entries, 0x28 bytes each, 4 per player (free RDRAM)
    constexpr uint32_t hud_names = 0x8024F900;       // The images' names, 16 bytes each
    constexpr uint32_t hud_text = 0x8024F500;        // A string
    constexpr int text_style = 0xA;                  // Text style the menus print with (func_800737E4)
    constexpr int health_width = 64;                 // HEALTHBG / HEALTHBAR
    constexpr int entry_size = 0x28;

    enum Image { image_frame, image_weapon, image_coin, image_count };
    const char* const image_names[image_count] = { "HEALTHBG", "WEAPONHUD", "BCOIN_BLUE" };
    // The icon of each weapon in WEAPONHUD's strip (cannon, gatling, grenade, mine, missile, ram, rocket, sonic); the
    // strip also has the stealth (4) and shield (5) icons. [I] for the grenade, mine, ram and sonic icons.
    constexpr int weapon_icons[8] = { 1, 0, 6, 6, 3, 5, 2, 6 };
    constexpr uint32_t hud_colors = 0x8024F400;      // RGBA of the health bars' fills, 4 bytes per player
    // Coin colors by player (2049's team colors).
    const char* const coin_names[4] = { "BCOIN_BLUE", "BCOIN_RED", "BCOIN_YELLOW", "BCOIN_GREEN" };

    struct Area {
        int x0, y0, x1, y1;
    };

    struct HudWidgets {
        int slot[image_count] = { -1, -1, -1 };
        int fill = -1;       // The health bar's fill: a rectangle in the frame's inner area
    };
    HudWidgets hud_widgets[4];
    int hud_players = 0;

    Area view_area(uint8_t* rdram, int player, int players) {
        constexpr int inset_x = 12, inset_y = 10;
        if (players <= 1) return { inset_x, inset_y, 320 - inset_x, 240 - inset_y };
        // The layout the game builds its HUD in: player 1 in the top half, the other players (copies of player 2's
        // elements) in the bottom half. src/hud.cpp moves them to the side by side or quadrant views.
        return player == 0 ? Area{ inset_x, inset_y, 320 - inset_x, 119 } : Area{ inset_x, 121, 320 - inset_x, 240 - inset_y };
    }

    // Rush 2049's battle HUD (docs/rush2049_research/battle.md): the health bar centered at the bottom of the view,
    // the weapon's icon with its ammo at the bottom left and the kill coin with its count at the bottom right.
    constexpr int bar_scale = 1, icon_scale = 1;   // Image widgets clip rather than stretch: the elements keep their size

    void bar_position(const Area& a, int& x, int& y) {
        x = (a.x0 + a.x1) / 2 - health_width * bar_scale / 2;
        y = a.y1 - 26;
    }

    void weapon_position(const Area& a, int& x, int& y) {
        x = a.x0 + 6;
        y = a.y1 - 36;
    }

    void coin_position(const Area& a, int& x, int& y) {
        x = a.x1 - 16 * icon_scale - 6;
        y = a.y1 - 36;
    }

    void write_h(uint8_t* rdram, uint32_t at, int16_t v) { MEM_H(0, (int32_t)at) = v; }

    // A layout entry for a static image `name` at (x, y), whole image, no callback.
    void make_entry(uint8_t* rdram, uint32_t entry, uint32_t name, int x, int y, uint32_t alpha = 0xFE) {
        for (uint32_t i = 0; i < entry_size; i += 4) MEM_W(0, (int32_t)(entry + i)) = 0;
        MEM_W(0, (int32_t)entry) = (int32_t)name;
        write_h(rdram, entry + 4, (int16_t)x);
        write_h(rdram, entry + 6, (int16_t)y);
        for (uint32_t off = 8; off <= 0x12; off += 2) write_h(rdram, entry + off, -1);
        MEM_W(0, (int32_t)(entry + 0x1C)) = alpha;   // 0xFF (opaque) or less (blended with the image's own alpha)
    }

    // func_800604FC: builds an element for the layout entry; returns its widget slot (the element's +0x34), or -1.
    int build_element(uint8_t* rdram, recomp_context* ctx, uint32_t entry) {
        recomp_context saved = *ctx;
        ctx->r4 = 0;
        ctx->r5 = 0;
        ctx->r6 = (int32_t)entry;
        ctx->r7 = 1;
        widgets_create_800604FC(rdram, ctx);
        uint32_t element = (uint32_t)ctx->r2;
        *ctx = saved;
        return element == 0 ? -1 : (int)MEM_HU(0, (int32_t)(element + 0x34));
    }

    // func_800541F8: a widget without an image, which draws a rectangle in the color at `color`; returns its slot.
    int create_fill(uint8_t* rdram, recomp_context* ctx, uint32_t color) {
        recomp_context saved = *ctx;
        ctx->r4 = 0;
        ctx->r5 = 0;
        ctx->r6 = (int32_t)color;
        ctx->r7 = 0;
        uint32_t sp = (uint32_t)ctx->r29;
        MEM_W(0x10, (int32_t)sp) = 0;
        MEM_W(0x14, (int32_t)sp) = 0;
        MEM_W(0x18, (int32_t)sp) = 0;
        hud_widget_alloc_800541F8(rdram, ctx);
        int slot = (int32_t)ctx->r2;
        *ctx = saved;
        return slot;
    }

    uint32_t widget_at(int slot) { return widget_array + (uint32_t)slot * widget_size; }

    // Moves a widget, crops it to the source rectangle (x0, 0)-(x1, h - 1) of its image, and shows or hides it.
    void place_widget(uint8_t* rdram, int slot, int x, int y, int x0, int x1, int h, bool visible, int scale = 1) {
        if (slot < 0 || slot >= 400) return;
        uint32_t widget = widget_at(slot);
        MEM_H(0xA, (int32_t)widget) = (int16_t)x;
        MEM_H(0xC, (int32_t)widget) = (int16_t)y;
        MEM_H(0x10, (int32_t)widget) = (int16_t)((x1 - x0 + 1) * scale);
        MEM_H(0x12, (int32_t)widget) = (int16_t)(h * scale);
        MEM_H(0x1A, (int32_t)widget) = (int16_t)x0;
        MEM_H(0x1E, (int32_t)widget) = (int16_t)x1;
        MEM_B(0x16, (int32_t)widget) = visible ? 0 : 1;
    }

    // Whether the widget is still the one built for this HUD (a slot is reused once the HUD is torn down): it must
    // still draw an image.
    bool widget_valid(uint8_t* rdram, int slot) {
        return slot >= 0 && slot < 400 && MEM_W(0, (int32_t)(widget_at(slot) + 4)) != 0;
    }

    void update_hud(uint8_t* rdram) {
        for (int p = 0; p < hud_players; p++) {
            const HudWidgets& w = hud_widgets[p];
            int car = player_car(rdram, p);
            if (car < 0 || car >= max_cars || !widget_valid(rdram, w.slot[image_frame])) continue;
            const Fighter& f = fighters[car];
            Area a = view_area(rdram, p, hud_players);
            int x, y;
            bar_position(a, x, y);
            float fraction = std::clamp(f.health / max_health, 0.0f, 1.0f);
            int fill = (int)std::lround(fraction * (health_width - 8));
            place_widget(rdram, w.slot[image_frame], x, y, 0, health_width - 1, 8, true, bar_scale);
            // The fill is a flat rectangle (2049 tints HEALTHBAR, a grey gradient, with its primitive color): green, as 2049's
            // bar, going to yellow and red as the health runs out.
            if (w.fill >= 0 && w.fill < 400) {
                uint32_t widget = widget_at(w.fill);
                uint32_t color = (uint32_t)MEM_W(0, (int32_t)widget);
                if (color == hud_colors + (uint32_t)p * 4) {
                    uint8_t r = fraction > 0.5f ? 0 : 200, g = fraction > 0.25f ? 128 : 0, b = 0;
                    if (fraction > 0.25f && fraction <= 0.5f) r = 200;
                    MEM_B(0, (int32_t)color) = (int8_t)r;
                    MEM_B(1, (int32_t)color) = (int8_t)g;
                    MEM_B(2, (int32_t)color) = (int8_t)b;
                    MEM_B(3, (int32_t)color) = (int8_t)0xFF;
                    MEM_H(0xA, (int32_t)widget) = (int16_t)(x + 4 * bar_scale);
                    MEM_H(0xC, (int32_t)widget) = (int16_t)(y + 2 * bar_scale);
                    MEM_H(0x10, (int32_t)widget) = (int16_t)(fill * bar_scale);
                    MEM_H(0x12, (int32_t)widget) = (int16_t)(4 * bar_scale);
                    MEM_B(0x16, (int32_t)widget) = fill > 0 ? 0 : 1;
                }
            }
            // The weapon held: its 16 x 16 icon of the strip.
            bool armed = f.weapon >= 0 && f.weapon < 8;
            int wx, wy, cx, cy;
            weapon_position(a, wx, wy);
            coin_position(a, cx, cy);
            int icon = armed ? weapon_icons[f.weapon] * 16 : 0;
            place_widget(rdram, w.slot[image_weapon], wx, wy, icon, icon + 15, 16,
                         armed && hud_players > 1, icon_scale);
            place_widget(rdram, w.slot[image_coin], cx, cy, 0, 15, 16, true, icon_scale);
        }
    }

    void print_text(uint8_t* rdram, recomp_context* ctx, int x, int y, const char* text) {
        int32_t px = x, py = y;
        rush2::hud::anchor_text(rdram, px, py);
        x = px;
        y = py;
        for (size_t i = 0; i <= strlen(text); i++) MEM_B((int32_t)i, (int32_t)hud_text) = (int8_t)text[i];
        recomp_context saved = *ctx;
        ctx->r4 = text_style;
        text_select_style_800737E4(rdram, ctx);
        *ctx = saved;
        ctx->r4 = x;
        ctx->r5 = y;
        ctx->r6 = (int32_t)hud_text;
        text_print_string_800734E0(rdram, ctx);
        *ctx = saved;
    }
}

void rush2::battle::hud_built(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ battle_mutex };
    // The hook runs on every call of the HUD setup function, which returns early once the HUD exists: build once per
    // element list (players4::hud_built does the same).
    uint32_t head = (uint32_t)MEM_W(0, (int32_t)hud_element_list);
    if (head == 0 || head == built_head) return;
    built_head = head;
    for (HudWidgets& w : hud_widgets) w = HudWidgets{};
    hud_players = 0;
    int slot = rush2::track2049::loaded_slot();
    if (slot < 0 || MEM_B(0, (int32_t)track_id) != slot || rush2::track2049::battle_arena() <= 0) return;
    int players = std::clamp<int>((int16_t)MEM_H(0, (int32_t)num_players), 1, 4);
    uint32_t name_at = hud_names;
    for (int p = 0; p < players; p++) {
        for (int i = 0; i < image_count; i++) {
            const char* name = i == image_coin ? coin_names[p] : image_names[i];
            for (size_t c = 0; c <= strlen(name); c++) MEM_B((int32_t)c, (int32_t)name_at) = (int8_t)name[c];
            uint32_t entry = hud_entries + (uint32_t)(p * image_count + i) * entry_size;
            make_entry(rdram, entry, name_at, 0, 0);
            hud_widgets[p].slot[i] = build_element(rdram, ctx, entry);
            fprintf(stderr, "[Battle] HUD %s -> widget %d, image %08X\n", name, hud_widgets[p].slot[i], hud_widgets[p].slot[i] < 0 ? 0u : (uint32_t)MEM_W(0, (int32_t)(widget_at(hud_widgets[p].slot[i]) + 4)));
            name_at += 16;
        }
        for (int c = 0; c < 4; c++) MEM_B(c, (int32_t)(hud_colors + p * 4)) = 0;
        hud_widgets[p].fill = create_fill(rdram, ctx, hud_colors + (uint32_t)p * 4);
    }
    hud_players = players;
    // Rush 2's own HUD is hidden except the speedometer: every element with a callback but the speedometer's loses its
    // callback (which would show it again) and is hidden with its widget. The elements are in a pool (func_80060418).
    int element_total = (int)MEM_W(0, (int32_t)element_count);
    for (int i = 0; i < element_total && i < 0xC8; i++) {
        uint32_t e = (uint32_t)MEM_W(0, (int32_t)(element_pool + (uint32_t)i * 4));
        uint32_t callback = e == 0 ? 0 : (uint32_t)MEM_W(0x28, (int32_t)e);
        if (callback == 0 || callback == speedometer_callback) continue;
        MEM_W(0x28, (int32_t)e) = 0;
        MEM_B(0x1A, (int32_t)e) = 1;
        int widget_slot = (int16_t)MEM_H(0x34, (int32_t)e);
        if (widget_slot >= 0 && widget_slot < 400) MEM_B(0x16, (int32_t)widget_at(widget_slot)) = 1;
    }
}

void rush2::battle::hud_draw(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ battle_mutex };
    if (!ready || !battle_race(rdram) || hud_players == 0) return;
    for (int p = 0; p < hud_players; p++) {
        int car = player_car(rdram, p);
        if (car < 0 || car >= max_cars) continue;
        const Fighter& f = fighters[car];
        Area a = view_area(rdram, p, hud_players);
        int x, y;
        bar_position(a, x, y);
        int wx, wy, cx, cy;
        weapon_position(a, wx, wy);
        coin_position(a, cx, cy);
        char text[48];
        // The kill count sits in the coin, the ammo right of the weapon icon.
        snprintf(text, sizeof(text), "%d", f.kills);
        print_text(rdram, ctx, cx + 16 * icon_scale / 2 - 4 * (int)strlen(text), cy + 16 * icon_scale / 2 - 5, text);
        if (f.weapon >= 0) {
            snprintf(text, sizeof(text), "%d", f.ammo);
            print_text(rdram, ctx, wx + 16 * icon_scale + 4, wy + 16 * icon_scale / 2 - 5, text);
        }
        if (f.shield > 0.0f || f.invisible > 0.0f) {
            snprintf(text, sizeof(text), "%s%s", f.shield > 0.0f ? "SHIELD " : "", f.invisible > 0.0f ? "INVISIBLE" : "");
            print_text(rdram, ctx, x, y - 12, text);
        }
    }
}

// func_800B9CC0 (the stunt score panel's visibility callback) at 0x800B9D2C: $a1 = whether to hide it. A battle arena is
// played in stunt mode but has no stunt score.
extern "C" void rush2_battle_hide_stunt_panel(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::battle_race(rdram)) ctx->r5 = 1;
}
