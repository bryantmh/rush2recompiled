// Rush 2049's battle mode in Rush 2 (docs/rush2049_research/battle.md).
//
// The battle arenas (track select ids 30-37, src/track2049_menu.cpp) are hosted by Rush 2's stunt track, so the race
// runs as Rush 2's stunt mode (free roam, a clock, no checkpoints). Rush 2049's battle was multiplayer only, so there
// are no computer cars (their AI is future work). Rush 2049 runs its battle in a mode overlay (ROM 0xB6FEC4, raw
// deflate, loaded at 0x8038A400) on top of its race code; this file is that overlay's logic on Rush 2's cars. Function
// names below are the overlay's (or 2049's main code's); values are 2049's unless marked [I] (inferred).
//
// - Health (func_8038D3A4): 800 per car. Damage from a car to itself is ignored; a shield scales damage by 0.2. A car
//   at 0 is wrecked (car +0x648, which Rush 2's own wreck and respawn code then handles) and its attacker scores a
//   kill, shown as a coin. The respawned car has full health and the default gun.
// - Weapons (func_8038FCE0 fires, func_8038E114 moves the shots): every car always has a gun with endless ammo
//   (weapon 8). A pickup replaces it with one of the 8 weapons and its ammo (table 0x80121D60); the same weapon again
//   adds the ammo, DROP WEAPON goes back to the gun, and so does running out. FIRE shoots on a press (the gatling
//   while held). The guns aim themselves at the nearest car ahead (func_8038F568), the missile homes
//   (func_8038DDDC), grenades and mines fall and bounce, the sonic blast is a growing ring (func_8038D498), and the
//   ram damages what the car hits. A shot hits a car inside a cylinder around it (func_8038DA78) and explodes or
//   stops on the arena's collision polygons (2049 func_800ADD58; here the converted arena's triangles).
// - Pickups (2049 main func_8010D3C0 / func_8010D680): each arena places the 8 weapons and several POWUPs (a random
//   one of heal, invisibility, shield). They turn in place; a weapon comes back once no car holds it, a power-up
//   after its type's 60 s.
// - The weapon a car holds is mounted on it at 2049's offset for that weapon and car model (table 0x803943A4,
//   func_8038AA8C); Rush 2's own cars use offsets worked out from their body height [I].
// - HUD (func_80391B00, func_80391864, func_80391650): the health bar (HEALTHBG, 2D), and 3D models in front of each
//   view's camera: the weapon held, turning, with its ammo; the power-up in effect; and the player's coin with the
//   kill count. Rush 2's HUD is removed except the speedometer.
// Projectiles, mounted weapons, shields and the HUD's models are records of a pool in the converted placement
// (battle_pool_size, hidden at the origin) whose model is swapped and pose written each tick, like the props
// (src/track2049_props.cpp).

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
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "assets.h"
#include "audio2049.h"
#include "battle.h"
#include "car2049.h"
#include "ghost.h"
#include "rush2.h"
#include "rush2_hooks.h"
#include "track2049.h"
#include "wings.h"

extern "C" void hud_widget_alloc_800541F8(uint8_t* rdram, recomp_context* ctx);   // Allocates a 2D widget.
extern "C" void widgets_create_800604FC(uint8_t* rdram, recomp_context* ctx);   // Builds HUD elements from layout entries.

namespace {
    using rush2::track2049::PickupRecord;
    void update_hud(uint8_t* rdram);

    // Game addresses (Rush 2).
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t cars = 0x800F5470;
    constexpr uint32_t car_size = 0x81C;
    constexpr int max_cars = 8;
    // The cars' draw states, 0x354 each, with the pose a car is drawn with: +0 position, +0xC velocity, +0x24
    // orientation rows (right, up, forward). Rush 2049's are at 0x80152818 (0x3B8 each, its battle fields at the end)
    // with these 8 bytes further in (+8, +0x14, +0x2C); its battle code takes the cars' poses from them.
    constexpr uint32_t car_states = 0x801124A0;
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t state_position = 0x0;
    constexpr uint32_t state_velocity = 0xC;
    constexpr uint32_t state_matrix = 0x24;
    constexpr uint32_t state_body_node = 0xF0;       // s32: the index of the scene node of the car's body
    constexpr uint32_t player_slots = 0x800C2140;    // 0x28 per player: +0 car index, +1 controller port, +2 pressed, +4 held.
    constexpr uint32_t player_size = 0x28;
    constexpr uint32_t num_players = 0x8010C3E2;     // s16
    constexpr uint32_t record_base = 0x8010C15C;     // Pointer to the placement file's first record.
    constexpr uint32_t record_size = 0x64;
    constexpr uint32_t nodes = 0x800D9E90;           // Scene nodes, 0x38 bytes each.
    constexpr uint32_t node_size = 0x38;
    constexpr uint32_t node_count = 0x800FAE58;
    constexpr uint32_t node_hidden = 0x400;
    constexpr uint32_t time_allowed = 0x8010C204;    // f32: the race's clock in seconds.
    constexpr uint32_t race_clock_start = 0x8010C034; // f32: the game timer when the race's clock started
    constexpr uint32_t game_timer = 0x80117488;      // f32: seconds
    constexpr uint32_t out_of_time = 0x800FAE98;     // s8: the race is out of time (func_800AE670 ends it)
    constexpr uint32_t widget_array = 0x802F6C00;    // 2D widgets, 0x20 each (src/hud.cpp).
    constexpr uint32_t widget_size = 0x20;
    constexpr uint32_t cameras = 0x800E79D0;         // Per view, 0x40 bytes: rows +0 right, +0xC up, +0x18 forward; +0x24 position.
    constexpr uint32_t game_state = 0x8010C0D0;      // 0-2 the menus, track select and car select; 3 and up a race

    // Car fields.
    constexpr uint32_t car_force = 0x11C;            // f32[3] external force, car-local, cleared each step
    constexpr uint32_t car_velocity = 0x218;         // f32[3] world
    constexpr uint32_t car_position = 0x224;         // f32[3] world
    constexpr uint32_t car_matrix = 0x2CC;           // f32[9]: rows right, up, forward
    constexpr uint32_t car_body_mass = 0x5B0;
    constexpr uint32_t car_wrecked = 0x648;          // s8
    constexpr uint32_t car_radius = 0x658;
    constexpr uint32_t car_active = 0x7E4;           // s16
    constexpr uint32_t car_type = 0x7EA;             // u8: its car type (0-21 Rush 2's, 23-35 Rush 2049's cars)

    // ------------------------------------------------------------------------------------------------------------
    // Rush 2049's values.

    constexpr float tick_rate = 30.0f;                      // 2049's battle steps per second (its per-step constants)
    constexpr float max_health = 800.0f;                    // 0x320
    constexpr int weapon_ammo[8] = { 20, 100, 20, 3, 3, 5, 20, 5 };   // 0x80121D60
    constexpr float pickup_radius = 6.0f, powerup_radius = 4.0f;      // type table 0x80117530 +0x18
    constexpr float touch_margin = 3.5f;                    // a car touches an object within its radius + 3.5
    constexpr float powerup_respawn = 60.0f;                // type table +0x1C
    constexpr float invisible_seconds = 30.0f;
    constexpr float shield_seconds = 30.0f;                 // func_8038F938
    constexpr float shield_factor = 0.2f;                   // 0x80394DC4
    constexpr float ram_front_factor = 0.35f;               // 0x80394E58 / 0x80394DD4: a ram's front takes less
    constexpr float ram_front_angle = 1.35f;                // 0x80394DE0 / 0x80394DD8 (radians)
    constexpr float explosion_radius2 = 900.0f;             // func_8038D798's radius squared
    constexpr float gravity = 32.2f;                        // 0x80142764 [I: the value is set at run time]
    constexpr float car_hit_radius = 6.25f, car_hit_height = 3.5f;   // func_8038DA78
    constexpr float aim_step = 0.01f;                       // 0x80394E8C..94: radians per step
    constexpr float homing_yaw = 0.04f, homing_pitch = 0.03f;         // 0x80394DE4..F0: radians per step
    constexpr float mine_life = 45.0f, mine_radius = 12.5f;           // WPR_MINE's type row
    constexpr int mines_per_car = 3;                        // func_8010C974
    constexpr float sonic_step = 1.0f / 30.0f;              // 0x80394E60: the ring grows by 1 each
    constexpr float sonic_push = 330000.0f, sonic_lift = 66000.0f;    // 0x80394DD0, 0x80394DCC (force)
    constexpr float ram_damage_base = 160.0f;               // func_800CE358: 160 + 7 x speed; speed scaled here [I]
    constexpr int explosion_sound = 0x45;                   // func_800AF06C: 0x2D at size 1 and up, 0x45 from 0.5, else 0x2F
    constexpr float pickup_spin = 3.0f;                     // rad/s [I]: 2049 turns a pickup by its object's own rate

    // Weapons: 0-7 are the pickups' (WEPICON_CANN, GATT, GREN, MINE, MISS, RAM, ROCK, SONC), 8 is the default gun.
    enum Weapon { cannon, gatling, grenade, mine, missile, ram, rocket, sonic, gun, weapon_count };
    // PickupRecord kinds past the weapons.
    enum Kind { kind_heal = 8, kind_invisibility = 9, kind_shield = 10, kind_powerup = 11 };

    struct WeaponInfo {
        float cooldown;     // seconds before the car fires again (+0x3AC)
        float life;         // the shot's seconds (+8)
        float radius;       // the shot's radius against cars (+0x10)
        float damage;       // a hit's, or an explosion's at its center
        float flash;        // the size of a hit's flash
        bool explodes;
        int sound;          // 2049 sound effect (table 0x803942C0)
        const char* model;
    };
    const WeaponInfo weapons[weapon_count] = {
        { 1.0f / 6.0f, 4.0f, 0.5f, 400.0f, 10.0f, false, 0x5C, "WPR_CANNG1" },
        { 1.0f / 15.0f, 4.0f, 0.25f, 114.0f, 3.0f, false, 0x57, "WFX_TRACERG1" },
        { 1.0f / 6.0f, 19.0f / 6.0f, 0.5f, 800.0f, 10.0f, true, 0x5A, "WPR_GRENG1" },
        { 1.0f, 5.0f, 0.5f, 800.0f, 10.0f, false, 0x58, "WPR_MINEBASE" },
        { 1.0f, 15.0f, 2.0f, 960.0f, 12.0f, true, 0x5B, "WPR_MISSG1" },
        { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0x5D, "" },
        { 1.0f / 6.0f, 10.0f, 1.0f, 520.0f, 8.0f, true, 0x5E, "WPR_ROCKG1" },
        { 1.0f, 1.0f, 2.0f, 0.0f, 0.0f, false, 0x5F, "WFX_SNCBLSTG1" },
        { 1.0f / 6.0f, 4.0f, 0.5f, 80.0f, 3.0f, false, 0x59, "WFX_TRACERG1" },
    };
    const char* const mount_models[8] = { "WEP_CANNG1", "WEP_GATTG1", "WEP_GRENG1", "WEP_MINEG1", "WEP_MISSG1",
                                          "WEP_RAMG1", "WEP_ROCKG1", "WEP_SONCG1" };

    // Tables of the battle overlay, read from the player's Rush 2049 ROM.
    constexpr uint32_t overlay_rom = 0xB6FEC4, overlay_vram = 0x8038A400, overlay_size = 0xAB70;
    constexpr int car_types_2049 = 13;
    struct Tuning {
        bool loaded = false;
        float mount[8][car_types_2049][3] = {};  // 0x803943A4: where a car model carries each weapon (x right, y up, z forward)
        float muzzle[9][3] = {};                 // 0x80394B08: where a weapon's shot starts, from its mount
        float shield_size[car_types_2049] = {};  // 0x80394358: the shield globe's scale
    };
    Tuning tuning;

    // ------------------------------------------------------------------------------------------------------------
    // State

    struct Fighter {
        bool present = false;
        float health = max_health;
        int kills = 0, deaths = 0;
        int weapon = gun, ammo = -1;
        float cooldown = 0.0f;          // +0x3AC
        float aim_yaw = 0.0f, aim_pitch = 0.0f;   // +0x3B4, +0x3B0
        float invisible = 0.0f;         // +0x390
        float shield = 0.0f;            // seconds left of the hold
        float shield_scale = 0.0f, shield_spin = 0.0f;
        bool was_wrecked = false;
        uint8_t buttons = 0;            // the weapon buttons held last tick
        bool hidden = false;            // its car state's invisible flag was set here
    };

    struct Pickup {
        PickupRecord rec;
        uint32_t record = 0, node = 0;
        float m[9] = {}, local[3] = {};   // The record's own pose, relative to its section
        float angle = 0.0f;
        bool taken = false;
        float respawn = 0.0f;
    };

    struct Shot {
        bool live = false;
        int owner = -1, weapon = 0, slot = -1;
        float pos[3] = {}, prev[3] = {}, vel[3] = {};
        float m[9] = {};                // rows right, up, forward
        float life = 0.0f, age = 0.0f, radius = 0.0f, speed = 0.0f;
        bool bounced = false, ram_front = false, placed = false;
        float ring = 1.0f, ring_clock = 0.0f;   // sonic
        uint32_t ring_hits = 0;
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

    // Pool slots (rush2::track2049::battle_pool_size).
    constexpr int shot_slots = 16;               // 0-15: projectiles and effects
    constexpr int mount_slot_first = 16;         // 16-23: the weapon mounted on each car
    constexpr int hud_weapon_slot_first = 24;    // 24-27: the HUD's weapon, per view
    constexpr int shield_slot_first = 28;        // 28-35: each car's shield
    constexpr int hud_powerup_slot_first = 36;   // 36-39: the HUD's power-up, per view
    constexpr int hud_coin_slot_first = 40;      // 40-43: the HUD's coin, per view

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
    float hud_spin = 0.0f;

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
    // Vectors and matrices (a matrix's rows are an object's axes: right, up, forward)

    float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    float length(const float a[3]) { return std::sqrt(dot(a, a)); }

    void normalize(float a[3]) {
        float l = length(a);
        if (l > 1e-6f) {
            for (int i = 0; i < 3; i++) a[i] /= l;
        }
    }

    void cross(const float a[3], const float b[3], float out[3]) {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    // The vector v in the frame m (2049 func_800A61B0).
    void to_local(const float m[9], const float v[3], float out[3]) {
        for (int k = 0; k < 3; k++) out[k] = dot(&m[k * 3], v);
    }

    // origin + the frame m's axes scaled by a local offset.
    void offset_point(const float origin[3], const float m[9], const float local[3], float out[3]) {
        for (int i = 0; i < 3; i++) out[i] = origin[i] + m[i] * local[0] + m[3 + i] * local[1] + m[6 + i] * local[2];
    }

    // Turns the frame about its own up axis (2049 func_80090E9C; positive turns forward toward right) or about its own
    // right axis (func_80090F44; positive lifts forward).
    void yaw(float m[9], float a) {
        float c = std::cos(a), s = std::sin(a);
        for (int i = 0; i < 3; i++) {
            float x = m[i], z = m[6 + i];
            m[i] = x * c - z * s;
            m[6 + i] = z * c + x * s;
        }
    }

    void pitch(float m[9], float a) {
        float c = std::cos(a), s = std::sin(a);
        for (int i = 0; i < 3; i++) {
            float y = m[3 + i], z = m[6 + i];
            m[3 + i] = y * c - z * s;
            m[6 + i] = z * c + y * s;
        }
    }

    void scale_matrix(float m[9], float s) {
        for (int i = 0; i < 9; i++) m[i] *= s;
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
        float y[3];
        cross(z, x, y);
        for (int i = 0; i < 3; i++) {
            m[i] = x[i];
            m[3 + i] = y[i];
            m[6 + i] = z[i];
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Cars

    uint32_t car_at(int i) { return cars + (uint32_t)i * car_size; }
    uint32_t state_at(int i) { return car_states + (uint32_t)i * car_state_size; }

    // The pose a car is drawn with, which 2049's battle code uses: its model's origin and axes.
    void car_pos(uint8_t* rdram, int i, float out[3]) {
        for (int k = 0; k < 3; k++) out[k] = read_f(rdram, state_at(i) + state_position + k * 4);
    }

    void car_vel(uint8_t* rdram, int i, float out[3]) {
        for (int k = 0; k < 3; k++) out[k] = read_f(rdram, car_at(i) + car_velocity + k * 4);
    }

    void car_axes(uint8_t* rdram, int i, float m[9]) {
        for (int k = 0; k < 9; k++) m[k] = read_f(rdram, state_at(i) + state_matrix + k * 4);
        // A state that isn't posed yet: the physics orientation.
        if (std::fabs(dot(m, m) - 1.0f) > 0.2f) {
            for (int k = 0; k < 9; k++) m[k] = read_f(rdram, car_at(i) + car_matrix + k * 4);
        }
    }

    bool is_wrecked(uint8_t* rdram, int i) { return MEM_B(0, (int32_t)(car_at(i) + car_wrecked)) != 0; }

    bool alive(uint8_t* rdram, int i) { return fighters[i].present && !is_wrecked(rdram, i); }

    int player_car(uint8_t* rdram, int player) {
        return (int8_t)MEM_B(0, (int32_t)(player_slots + player * player_size));
    }

    int local_players(uint8_t* rdram) {
        return std::clamp<int>((int16_t)MEM_H(0, (int32_t)num_players), 1, 4);
    }

    // Where car i carries `weapon` (0-7), in its own frame. Rush 2049's cars (types 23-35) use 2049's table; Rush 2's
    // cars carry the roof weapons where 2049's second car does, at their own roof's height [I].
    void mount_offset(uint8_t* rdram, int i, int weapon, float out[3]) {
        int type = MEM_BU(0, (int32_t)(car_at(i) + car_type));
        int type49 = type - rush2::car2049::first_type;
        if (tuning.loaded && type49 >= 0 && type49 < car_types_2049) {
            memcpy(out, tuning.mount[weapon][type49], sizeof(float) * 3);
            return;
        }
        constexpr int like = 1;
        constexpr float roof_sink = 0.45f;
        const float* base = tuning.mount[weapon][like];
        float fallback[3] = { 0.0f, 3.0f, 0.0f };
        if (!tuning.loaded) base = fallback;
        out[0] = base[0];
        out[1] = base[1];
        out[2] = base[2];
        if (weapon != mine && weapon != ram) {
            float cannon_y = tuning.loaded ? tuning.mount[cannon][like][1] : fallback[1];
            out[1] = rush2::wings::body_height(type) - roof_sink + (base[1] - cannon_y);
        }
    }

    int type_2049(uint8_t* rdram, int i) {
        int type49 = MEM_BU(0, (int32_t)(car_at(i) + car_type)) - rush2::car2049::first_type;
        return type49 >= 0 && type49 < car_types_2049 ? type49 : 1;
    }

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

    void read_models(const std::vector<uint8_t>& g) {
        models.clear();
        if (g.size() < 0x14) return;
        uint32_t names = be32(&g[4]), count = be32(&g[16]);
        for (uint32_t i = 0; i < count && names + (i + 1) * 0x18 <= g.size(); i++) {
            const char* c = (const char*)&g[names + i * 0x18];
            models[std::string(c, strnlen(c, 16))] = (uint16_t)i;
        }
        if (const char* test = getenv("R2_BATTLE_TEST"); test != nullptr && strstr(test, "models") != nullptr) {
            for (auto& [name, index] : models) fprintf(stderr, "[Battle] model %s" "\n", name.c_str());
        }
    }

    void load_tuning() {
        if (tuning.loaded) return;
        auto rom = rush2::wings::get_rom();
        std::vector<uint8_t> overlay;
        if (rom == nullptr || rom->size() <= overlay_rom ||
            !rush2::assets::inflate_raw(rom->data() + overlay_rom, rom->size() - overlay_rom, overlay) ||
            overlay.size() < overlay_size) {
            fprintf(stderr, "[Battle] Couldn't read Rush 2049's battle overlay\n");
            return;
        }
        auto f = [&](uint32_t vram) { return bef(&overlay[vram - overlay_vram]); };
        for (int w = 0; w < 8; w++) {
            for (int c = 0; c < car_types_2049; c++) {
                for (int k = 0; k < 3; k++) tuning.mount[w][c][k] = f(0x803943A4 + w * 0x9C + c * 0xC + k * 4);
            }
        }
        for (int w = 0; w < 9; w++) {
            for (int k = 0; k < 3; k++) tuning.muzzle[w][k] = f(0x80394B08 + w * 0xC + k * 4);
        }
        for (int c = 0; c < car_types_2049; c++) tuning.shield_size[c] = f(0x80394358 + c * 4);
        tuning.loaded = true;
    }

    // ------------------------------------------------------------------------------------------------------------
    // The arena's collision: its solid polygons as triangles, in a grid over x and z.

    struct Triangle {
        float a[3], b[3], c[3];
    };
    std::vector<Triangle> triangles;
    std::unordered_map<int64_t, std::vector<int>> triangle_grid;
    constexpr float grid_cell = 48.0f;

    int64_t grid_key(int x, int z) { return ((int64_t)x << 32) ^ (uint32_t)z; }

    void build_collision(const std::vector<float>& t) {
        triangles.clear();
        triangle_grid.clear();
        for (size_t i = 0; i + 9 <= t.size(); i += 9) {
            Triangle tri;
            memcpy(tri.a, &t[i], 12);
            memcpy(tri.b, &t[i + 3], 12);
            memcpy(tri.c, &t[i + 6], 12);
            int x0 = (int)std::floor(std::min({ tri.a[0], tri.b[0], tri.c[0] }) / grid_cell);
            int x1 = (int)std::floor(std::max({ tri.a[0], tri.b[0], tri.c[0] }) / grid_cell);
            int z0 = (int)std::floor(std::min({ tri.a[2], tri.b[2], tri.c[2] }) / grid_cell);
            int z1 = (int)std::floor(std::max({ tri.a[2], tri.b[2], tri.c[2] }) / grid_cell);
            if (x1 - x0 > 64 || z1 - z0 > 64) continue;
            int index = (int)triangles.size();
            triangles.push_back(tri);
            for (int x = x0; x <= x1; x++) {
                for (int z = z0; z <= z1; z++) triangle_grid[grid_key(x, z)].push_back(index);
            }
        }
    }

    // The first triangle the segment p0-p1 crosses: its point and unit normal (facing p0).
    bool sweep(const float p0[3], const float p1[3], float hit[3], float normal[3]) {
        float d[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        float best = 2.0f;
        int x0 = (int)std::floor(std::min(p0[0], p1[0]) / grid_cell), x1 = (int)std::floor(std::max(p0[0], p1[0]) / grid_cell);
        int z0 = (int)std::floor(std::min(p0[2], p1[2]) / grid_cell), z1 = (int)std::floor(std::max(p0[2], p1[2]) / grid_cell);
        if (x1 - x0 > 16 || z1 - z0 > 16) return false;
        for (int x = x0; x <= x1; x++) {
            for (int z = z0; z <= z1; z++) {
                auto cell = triangle_grid.find(grid_key(x, z));
                if (cell == triangle_grid.end()) continue;
                for (int index : cell->second) {
                    const Triangle& t = triangles[index];
                    float e1[3] = { t.b[0] - t.a[0], t.b[1] - t.a[1], t.b[2] - t.a[2] };
                    float e2[3] = { t.c[0] - t.a[0], t.c[1] - t.a[1], t.c[2] - t.a[2] };
                    float p[3], q[3];
                    cross(d, e2, p);
                    float det = dot(e1, p);
                    if (std::fabs(det) < 1e-7f) continue;
                    float s[3] = { p0[0] - t.a[0], p0[1] - t.a[1], p0[2] - t.a[2] };
                    float u = dot(s, p) / det;
                    if (u < 0.0f || u > 1.0f) continue;
                    cross(s, e1, q);
                    float v = dot(d, q) / det;
                    if (v < 0.0f || u + v > 1.0f) continue;
                    float k = dot(e2, q) / det;
                    if (k < 0.0f || k > 1.0f || k >= best) continue;
                    best = k;
                    cross(e1, e2, normal);
                    normalize(normal);
                    if (dot(normal, d) > 0.0f) {
                        for (int i = 0; i < 3; i++) normal[i] = -normal[i];
                    }
                }
            }
        }
        if (best > 1.0f) return false;
        for (int i = 0; i < 3; i++) hit[i] = p0[i] + d[i] * best + normal[i] * 0.05f;
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Pool

    int alloc_slot() {
        for (size_t i = 0; i < pool.size() && i < (size_t)shot_slots; i++) {
            if (!pool[i].used) {
                pool[i].used = true;
                return (int)i;
            }
        }
        return -1;
    }

    void hide_slot(uint8_t* rdram, int slot) {
        if (slot >= 0 && slot < (int)pool.size()) hide_node(rdram, pool[slot].node);
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

    // Colors. 2049 draws a car's weapon models in the car's color (its car state's +0x34C, func_8008E06C); here they
    // take the player's color, the one of its coin [I]. The other models' colors are not known from 2049's code [I].
    constexpr uint32_t player_colors[4] = { 0x4060FFFF, 0xFF3838FF, 0xFFE030FF, 0x38D048FF };
    constexpr uint32_t neutral_color = 0x7090B0FF, tracer_color = 0xFFF0A0FF, shell_color = 0xB0B0B0FF,
                       flash_color = 0xFFB040FF, ring_color = 0x80C0FFFF;

    void color_slot(int slot, uint32_t rgba) {
        if (slot >= 0 && slot < (int)pool.size()) rush2::interpolation_node_color(pool[slot].node, rgba);
    }

    uint32_t car_color(uint8_t* rdram, int car) {
        int players = local_players(rdram);
        for (int p = 0; p < players; p++) {
            if (player_car(rdram, p) == car) return player_colors[p];
        }
        return neutral_color;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Sounds: 2049 plays a weapon's sound from its car (func_800B61A8), with its 3D emitter law for each local player.

    void play_sound(uint8_t* rdram, int id, const float pos[3]) {
        if (id < 0 || !rush2::audio2049::loaded()) return;
        constexpr float range = 400.0f;
        float volume = 0.0f, pan = 0.0f, surround = 0.0f;
        int players = local_players(rdram);
        for (int p = 0; p < players; p++) {
            int car = player_car(rdram, p);
            if (car < 0 || car >= max_cars) continue;
            float listener[3], back[3], up[3];
            car_pos(rdram, car, listener);
            uint32_t cam = cameras + (uint32_t)p * 0x40;
            for (int i = 0; i < 3; i++) {
                up[i] = read_f(rdram, cam + 0xC + i * 4);
                back[i] = -read_f(rdram, cam + 0x18 + i * 4);
            }
            auto mix = rush2::audio2049::emitter_mix(pos, listener, back, up, range);
            volume += mix.volume;
            pan += mix.pan * mix.volume;
            surround += mix.surround * mix.volume;
        }
        if (volume <= 0.0f) return;
        pan /= volume;
        surround /= volume;
        rush2::audio2049::sfx_start(id, std::min(volume, 1.0f), pan, 1.0f, surround);
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
        load_tuning();
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
            for (int k = 0; k < 9; k++) p.m[k] = read_f(rdram, p.record + 0x10 + k * 4);
            for (int k = 0; k < 3; k++) p.local[k] = read_f(rdram, p.record + 0x34 + k * 4);
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
        rush2::interpolation_clear_view_attached();
        for (const Pickup& p : pickups) rush2::interpolation_node_color(p.node, neutral_color);
        for (int slot = 0; slot < (int)pool.size(); slot++) color_slot(slot, neutral_color);
        for (int slot = hud_weapon_slot_first; slot < (int)pool.size(); slot++) {
            if (slot < shield_slot_first || slot >= hud_powerup_slot_first) rush2::interpolation_view_attached(pool[slot].node);
        }
        clock_seconds = 0.0f;
        clock_set = false;
        ready = true;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Damage

    // func_8038D3A4: a car doesn't damage itself; a shield takes 0.2 of it.
    void damage(uint8_t* rdram, int victim, int attacker, float amount) {
        if (victim < 0 || victim >= max_cars || victim == attacker || !alive(rdram, victim) || amount <= 0.0f) return;
        Fighter& f = fighters[victim];
        if (f.shield > 0.0f) amount *= shield_factor;
        f.health -= std::floor(amount);
        if (f.health <= 0.0f) {
            f.health = 0.0f;
            MEM_B(0, (int32_t)(car_at(victim) + car_wrecked)) = 1;
            f.deaths++;
            if (attacker >= 0 && attacker < max_cars && fighters[attacker].present) fighters[attacker].kills++;
        }
    }

    // A force on a car for one step, in world axes (2049 adds it to the car's force accumulators).
    void add_force(uint8_t* rdram, int car, const float world[3]) {
        float m[9];
        for (int k = 0; k < 9; k++) m[k] = read_f(rdram, car_at(car) + car_matrix + k * 4);
        for (int k = 0; k < 3; k++) {
            uint32_t at = car_at(car) + car_force + k * 4;
            write_f(rdram, at, read_f(rdram, at) + dot(&m[k * 3], world));
        }
    }

    // Whether car `victim` holds the ram and `from` (a world direction toward what hit it) is within the ram's angle
    // of its nose.
    bool ram_blocks(uint8_t* rdram, int victim, const float from[3]) {
        if (fighters[victim].weapon != ram) return false;
        float m[9], local[3];
        car_axes(rdram, victim, m);
        to_local(m, from, local);
        return std::fabs(std::atan2(local[0], local[2])) < ram_front_angle;
    }

    void add_effect(const float pos[3], float duration, float scale0, float scale1) {
        int slot = alloc_slot();
        if (slot < 0) return;
        Effect e;
        e.live = true;
        e.slot = slot;
        memcpy(e.pos, pos, sizeof(e.pos));
        e.duration = duration;
        e.scale0 = scale0;
        e.scale1 = scale1;
        color_slot(slot, flash_color);
        effects.push_back(e);
    }

    // func_8038D798: every car within the radius takes (1 - d^2 / r^2)^2 of the damage (a ram facing the shot's path
    // 0.35 of that).
    void explode(uint8_t* rdram, const float pos[3], const float from[3], int owner, float amount) {
        for (int i = 0; i < max_cars; i++) {
            if (!alive(rdram, i)) continue;
            float c[3];
            car_pos(rdram, i, c);
            float d[3] = { pos[0] - c[0], pos[1] - c[1], pos[2] - c[2] };
            float d2 = dot(d, d);
            if (d2 >= explosion_radius2) continue;
            float k = (explosion_radius2 - d2) / explosion_radius2;
            float back[3] = { from[0] - pos[0], from[1] - pos[1], from[2] - pos[2] };
            float share = k * k * amount;
            if (ram_blocks(rdram, i, back)) share *= ram_front_factor;
            damage(rdram, i, owner, share);
        }
        // 2049 main func_800AF06C(pos, 0, 0.5, 1): the game's explosion at half size, with sound 0x45 heard within 400.
        play_sound(rdram, explosion_sound, pos);
        add_effect(pos, 0.35f, 0.5f, 10.0f);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Weapons

    // func_8038F568: the guns turn toward the nearest car in a cone ahead, a hundredth of a radian per step.
    void update_aim(uint8_t* rdram, int car, float steps) {
        Fighter& f = fighters[car];
        float c[3], m[9];
        car_pos(rdram, car, c);
        car_axes(rdram, car, m);
        float height = tuning.muzzle[std::min<int>(f.weapon, 8)][1];
        if (f.weapon < 8) {
            float mount[3];
            mount_offset(rdram, car, f.weapon, mount);
            height += mount[1];
        }
        float best = 2000.0f, want_yaw = 0.0f, want_pitch = 0.0f;
        for (int i = 0; i < max_cars; i++) {
            if (i == car || !alive(rdram, i)) continue;
            float t[3], local[3];
            car_pos(rdram, i, t);
            float d[3] = { t[0] - c[0] - m[3] * height, t[1] - c[1] - m[4] * height, t[2] - c[2] - m[5] * height };
            to_local(m, d, local);
            if (local[2] < 0.0f || std::fabs(local[0]) > local[2] * 0.5f || std::fabs(local[1]) > local[2] || local[2] > best) {
                continue;
            }
            best = local[2];
            want_yaw = std::atan2(local[0], local[2]);
            want_pitch = std::atan2(local[1], local[2]);
        }
        float step = aim_step * steps;
        f.aim_yaw += std::clamp(want_yaw - f.aim_yaw, -step, step);
        f.aim_pitch += std::clamp(want_pitch - f.aim_pitch, -step, step);
    }

    float random_unit() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); }

    // func_8038FCE0: car fires its weapon.
    void fire(uint8_t* rdram, int car) {
        Fighter& f = fighters[car];
        const WeaponInfo& w = weapons[f.weapon];
        float c[3], v[3], m[9];
        car_pos(rdram, car, c);
        car_vel(rdram, car, v);
        car_axes(rdram, car, m);
        Shot s;
        s.live = true;
        s.owner = car;
        s.weapon = f.weapon;
        memcpy(s.m, m, sizeof(s.m));
        s.life = w.life;
        s.radius = w.radius;
        f.cooldown = w.cooldown;
        switch (f.weapon) {
            case grenade:
                pitch(s.m, 0.5f);
                for (int i = 0; i < 3; i++) s.vel[i] = s.m[6 + i] * 75.0f + v[i];
                break;
            case missile:
                s.speed = dot(v, &s.m[6]) + 165.0f;
                break;
            case rocket:
                pitch(s.m, f.aim_pitch);
                for (int i = 0; i < 3; i++) s.vel[i] = s.m[6 + i] * 50.0f + v[i];
                break;
            case gun:
                yaw(s.m, f.aim_yaw);
                pitch(s.m, f.aim_pitch);
                break;
            case cannon:
                pitch(s.m, f.aim_pitch);
                break;
            case gatling:
                // Each shot strays by up to 0.0125 radians.
                yaw(s.m, f.aim_yaw + random_unit() * 0.025f - 0.0125f);
                pitch(s.m, f.aim_pitch + random_unit() * 0.025f - 0.0125f);
                break;
            case mine:
                // Let go behind the car, tipped back, with a toss upward.
                pitch(s.m, 2.0735f);
                for (int i = 0; i < 3; i++) s.vel[i] = m[3 + i] * 15.0f + v[i];
                break;
            default:
                break;
        }
        // The shot starts at the weapon's mount plus its muzzle (the gun's is on the car itself).
        float local[3] = { tuning.muzzle[f.weapon][0], tuning.muzzle[f.weapon][1], tuning.muzzle[f.weapon][2] };
        if (f.weapon < 8) {
            float mount[3];
            mount_offset(rdram, car, f.weapon, mount);
            for (int i = 0; i < 3; i++) local[i] += mount[i];
        }
        offset_point(c, m, local, s.pos);
        memcpy(s.prev, s.pos, sizeof(s.prev));
        if (f.weapon == sonic) {
            memcpy(s.pos, c, sizeof(s.pos));
            s.pos[1] += 1.25f;
        }
        s.slot = alloc_slot();
        color_slot(s.slot, f.weapon == gun || f.weapon == gatling ? tracer_color : f.weapon == sonic ? ring_color : shell_color);
        play_sound(rdram, w.sound, s.pos);
        if (s.slot < 0 && f.weapon != sonic) {
            // No record to draw it with: the shot is lost (the ammo is still used).
            s.live = false;
        }
        if (s.live) shots.push_back(s);
        if (f.ammo > 0) {
            f.ammo--;
            if (f.ammo == 0) {
                f.weapon = gun;
                f.ammo = -1;
            }
        }
    }

    // func_8038DA78: the first car (not the shot's owner) whose cylinder the shot's step crosses; the shot stops there.
    int car_hit(uint8_t* rdram, Shot& s) {
        float d[3] = { s.pos[0] - s.prev[0], s.pos[1] - s.prev[1], s.pos[2] - s.prev[2] };
        float flat2 = d[0] * d[0] + d[2] * d[2];
        int hit = -1;
        float best = 1.0f;
        for (int i = 0; i < max_cars; i++) {
            if (i == s.owner || !alive(rdram, i)) continue;
            float c[3];
            car_pos(rdram, i, c);
            float t = flat2 < 1e-6f ? 0.0f : ((c[0] - s.prev[0]) * d[0] + (c[2] - s.prev[2]) * d[2]) / flat2;
            if (t < 0.0f || t > best) continue;
            float at[3] = { s.prev[0] + d[0] * t, s.prev[1] + d[1] * t, s.prev[2] + d[2] * t };
            float dy = at[1] - c[1];
            float side = std::sqrt((at[0] - c[0]) * (at[0] - c[0]) + (at[2] - c[2]) * (at[2] - c[2]));
            if (dy < -s.radius || dy > car_hit_height + s.radius || side > s.radius + car_hit_radius) continue;
            float back[3] = { -d[0], -d[1], -d[2] };
            if (ram_blocks(rdram, i, back)) s.ram_front = true;
            hit = i;
            best = t;
            memcpy(s.pos, at, sizeof(at));
        }
        return hit;
    }

    // The velocity after bouncing off a surface: its part along the normal is turned back and scaled.
    void bounce(float vel[3], const float normal[3], float keep) {
        float along = dot(vel, normal);
        for (int i = 0; i < 3; i++) vel[i] -= normal[i] * along * (1.0f + keep);
    }

    void end_shot(uint8_t* rdram, Shot& s) {
        s.live = false;
        free_slot(rdram, s.slot);
        s.slot = -1;
    }

    // func_8038D498: the sonic blast's ring. It grows by 1 every thirtieth of a second and reaches 4 times its size;
    // a car it reaches (once each) takes 800, 400 or 200 as the ring grows, and is thrown outward.
    void update_sonic(uint8_t* rdram, Shot& s, float dt) {
        s.ring_clock += dt;
        while (s.ring_clock >= sonic_step) {
            s.ring_clock -= sonic_step;
            s.ring += 1.0f;
        }
        float amount = s.ring <= 8.0f ? 800.0f : s.ring <= 20.0f ? 400.0f : 200.0f;
        float force = s.ring <= 8.0f ? 1.0f : s.ring <= 20.0f ? 0.6f : 0.5f;
        float reach = s.ring * 4.0f;
        if (alive(rdram, s.owner)) {
            car_pos(rdram, s.owner, s.pos);
        }
        for (int i = 0; i < max_cars; i++) {
            if (i == s.owner || !alive(rdram, i) || (s.ring_hits & (1u << i)) != 0) continue;
            float c[3];
            car_pos(rdram, i, c);
            float d[3] = { c[0] - s.pos[0], c[1] - s.pos[1], c[2] - s.pos[2] };
            float dist = length(d);
            if (dist > reach || dist < 1e-3f) continue;
            s.ring_hits |= 1u << i;
            damage(rdram, i, s.owner, amount);
            float world[3] = { d[0] / dist * sonic_push * force, (d[1] / dist * sonic_push + sonic_lift) * force,
                               d[2] / dist * sonic_push * force };
            add_force(rdram, i, world);
        }
        if (s.slot >= 0) {
            float m[9] = { s.ring, 0, 0, 0, s.ring, 0, 0, 0, s.ring };
            float at[3] = { s.pos[0], s.pos[1] + 1.25f, s.pos[2] };
            place_slot(rdram, s.slot, weapons[sonic].model, m, at);
        }
    }

    // A mine at rest (2049's WPR_MINE object, func_8010C7F4): a car that comes within its radius sets it off and takes
    // 800 (a ram that meets it nose first 280); the car that laid it sets it off unharmed.
    void update_placed_mine(uint8_t* rdram, Shot& s) {
        for (int i = 0; i < max_cars; i++) {
            if (!alive(rdram, i)) continue;
            float c[3];
            car_pos(rdram, i, c);
            float d[3] = { s.pos[0] - c[0], s.pos[1] - c[1], s.pos[2] - c[2] };
            if (length(d) > mine_radius + touch_margin) continue;
            float m[9], local[3];
            car_axes(rdram, i, m);
            to_local(m, d, local);
            damage(rdram, i, s.owner, fighters[i].weapon == ram && local[2] >= 0.0f ? 280.0f : 800.0f);
            add_effect(s.pos, 0.35f, 0.5f, 8.0f);
            play_sound(rdram, weapons[mine].sound, s.pos);
            end_shot(rdram, s);
            return;
        }
    }

    // func_8038E114: one step of every shot.
    void update_shots(uint8_t* rdram, float dt) {
        float steps = dt * tick_rate;
        for (size_t n = 0; n < shots.size(); n++) {
            Shot& s = shots[n];
            if (!s.live) continue;
            const WeaponInfo& w = weapons[s.weapon];
            s.life -= dt;
            s.age += dt;
            if (s.life <= 0.0f) {
                if (w.explodes) explode(rdram, s.pos, s.prev, s.owner, w.damage);
                end_shot(rdram, s);
                continue;
            }
            if (s.weapon == sonic) {
                update_sonic(rdram, s, dt);
                continue;
            }
            if (s.placed) {
                update_placed_mine(rdram, s);
                if (s.live) place_slot(rdram, s.slot, w.model, s.m, s.pos);
                continue;
            }
            memcpy(s.prev, s.pos, sizeof(s.prev));
            float forward_speed = 0.0f;
            switch (s.weapon) {
                case mine:
                    s.vel[1] -= gravity * dt;
                    s.vel[0] *= std::pow(0.9f, steps);
                    s.vel[2] *= std::pow(0.9f, steps);
                    break;
                case grenade:
                    s.vel[1] -= gravity * 2.5f * dt;
                    break;
                case missile: {
                    // func_8038DDDC: turns toward the nearest car in a cone ahead.
                    float best = 2000.0f, target[3] = {};
                    bool found = false;
                    for (int i = 0; i < max_cars; i++) {
                        if (i == s.owner || !alive(rdram, i)) continue;
                        float c[3], local[3];
                        car_pos(rdram, i, c);
                        float d[3] = { c[0] - s.pos[0], c[1] - s.pos[1], c[2] - s.pos[2] };
                        to_local(s.m, d, local);
                        if (local[2] < 0.0f || std::fabs(local[0]) > local[2] || std::fabs(local[1]) > local[2]) continue;
                        float dist = length(local);
                        if (dist >= best) continue;
                        best = dist;
                        memcpy(target, local, sizeof(target));
                        found = true;
                    }
                    if (found) {
                        yaw(s.m, (target[0] > 0.0f ? homing_yaw : -homing_yaw) * steps);
                        pitch(s.m, (target[1] > 0.0f ? homing_pitch : -homing_pitch) * steps);
                    }
                    s.speed += (100.0f - s.speed) * dt;
                    forward_speed = s.speed;
                    break;
                }
                case rocket:
                    for (int i = 0; i < 3; i++) s.vel[i] += s.m[6 + i] * 800.0f * dt;
                    break;
                case cannon:
                    forward_speed = 1000.0f;
                    break;
                default:
                    forward_speed = 2000.0f;
                    break;
            }
            for (int i = 0; i < 3; i++) s.pos[i] += (forward_speed != 0.0f ? s.m[6 + i] * forward_speed : s.vel[i]) * dt;

            int victim = s.weapon == mine ? -1 : car_hit(rdram, s);
            float wall[3], normal[3];
            bool walled = sweep(s.prev, s.pos, wall, normal);
            if (walled) {
                memcpy(s.pos, wall, sizeof(wall));
                if (s.weapon == mine) {
                    if (s.bounced && std::fabs(s.vel[0]) < 10.0f && std::fabs(s.vel[2]) < 10.0f) {
                        // At rest: it becomes a mine on the ground. A car's fourth mine removes its oldest.
                        s.placed = true;
                        s.life = mine_life;
                        float up[3] = { 0.0f, 0.0f, 1.0f };
                        facing(up, s.m);
                        float y[3], x[3] = { s.m[0], s.m[1], s.m[2] };
                        cross(normal, x, y);
                        for (int i = 0; i < 3; i++) {
                            s.m[3 + i] = normal[i];
                            s.m[6 + i] = y[i];
                        }
                        int placed = 0, oldest = -1;
                        for (size_t k = 0; k < shots.size(); k++) {
                            if (k == n || !shots[k].live || !shots[k].placed || shots[k].owner != s.owner) continue;
                            placed++;
                            if (oldest < 0 || shots[k].life < shots[oldest].life) oldest = (int)k;
                        }
                        if (placed >= mines_per_car && oldest >= 0) end_shot(rdram, shots[oldest]);
                    }
                    else if (s.bounced) {
                        bounce(s.vel, normal, 0.4f);
                    }
                    else {
                        // The first bounce leaves the surface at 15.
                        s.bounced = true;
                        float along = dot(s.vel, normal);
                        for (int i = 0; i < 3; i++) s.vel[i] += normal[i] * (15.0f - along);
                    }
                }
                else if (s.weapon == grenade && victim < 0 && s.life > dt) {
                    bounce(s.vel, normal, 0.9f);
                    add_effect(s.pos, 0.15f, 0.3f, 1.5f);
                }
                else if (w.explodes) {
                    explode(rdram, s.pos, s.prev, s.owner, w.damage);
                    end_shot(rdram, s);
                    continue;
                }
                else {
                    // A bullet or shell stops at the wall with a flash (2049 leaves it there until its time is up).
                    add_effect(s.pos, 0.12f, 0.2f, w.flash * 0.2f);
                    end_shot(rdram, s);
                    continue;
                }
            }
            else if (victim >= 0) {
                if (w.explodes) {
                    explode(rdram, s.pos, s.prev, s.owner, w.damage);
                }
                else {
                    damage(rdram, victim, s.owner, s.ram_front ? w.damage * ram_front_factor : w.damage);
                    add_effect(s.pos, 0.15f, 0.2f, w.flash * 0.3f);
                }
                end_shot(rdram, s);
                continue;
            }
            if (s.weapon == grenade || s.weapon == rocket) {
                // They point where they go.
                if (length(s.vel) > 1.0f) facing(s.vel, s.m);
            }
            place_slot(rdram, s.slot, w.model, s.m, s.pos);
        }
        shots.erase(std::remove_if(shots.begin(), shots.end(), [](const Shot& s) { return !s.live; }), shots.end());
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

    // func_8038AA8C: the weapon a car holds sits on it at the weapon's offset for its car model, turned with the car.
    // The gun shows nothing. A mine is held behind the car until it is let go.
    void update_mounts(uint8_t* rdram) {
        for (int i = 0; i < max_cars; i++) {
            const Fighter& f = fighters[i];
            int slot = mount_slot_first + i;
            if (!alive(rdram, i) || f.weapon >= 8 || f.invisible > 0.0f) {
                hide_slot(rdram, slot);
            }
            else {
                float c[3], m[9], local[3], pos[3];
                car_pos(rdram, i, c);
                car_axes(rdram, i, m);
                mount_offset(rdram, i, f.weapon, local);
                offset_point(c, m, local, pos);
                color_slot(slot, car_color(rdram, i));
                place_slot(rdram, slot, mount_models[f.weapon], m, pos);
            }
            // func_8038F938: the shield's globe grows to the car model's size, holds, then shrinks.
            int shield_slot = shield_slot_first + i;
            if (!alive(rdram, i) || f.shield_scale <= 0.0f) {
                hide_slot(rdram, shield_slot);
            }
            else {
                float c[3], m[9];
                car_pos(rdram, i, c);
                car_axes(rdram, i, m);
                pitch(m, f.shield_spin);
                scale_matrix(m, f.shield_scale);
                place_slot(rdram, shield_slot, "WFX_SHIELDG1", m, c);
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Pickups

    // func_8010D3C0: what a pickup gives.
    void give(uint8_t* rdram, int car, int kind) {
        Fighter& f = fighters[car];
        if (kind == kind_powerup) {
            static const int options[3] = { kind_heal, kind_invisibility, kind_shield };
            kind = options[std::uniform_int_distribution<int>(0, 2)(rng)];
        }
        switch (kind) {
            case kind_heal: f.health = max_health; break;
            case kind_invisibility: f.invisible = invisible_seconds; break;
            case kind_shield:
                f.shield = shield_seconds;
                f.shield_scale = std::max(f.shield_scale, 0.05f);
                break;
            default:
                if (kind >= 0 && kind < 8) {
                    if (f.weapon == kind) {
                        f.ammo += weapon_ammo[kind];
                    }
                    else {
                        f.weapon = kind;
                        f.ammo = weapon_ammo[kind];
                    }
                }
                break;
        }
    }

    // func_8010D680: a pickup turns in place. A weapon's comes back once no car holds that weapon, a power-up's after a
    // minute.
    void update_pickups(uint8_t* rdram, float dt) {
        for (Pickup& p : pickups) {
            bool weapon_pickup = p.rec.kind >= 0 && p.rec.kind < 8;
            if (p.taken) {
                bool back;
                if (weapon_pickup) {
                    back = true;
                    for (int i = 0; i < max_cars; i++) back = back && !(fighters[i].present && fighters[i].weapon == p.rec.kind);
                }
                else {
                    p.respawn -= dt;
                    back = p.respawn <= 0.0f;
                }
                if (!back) continue;
                p.taken = false;
                show_node(rdram, p.node);
            }
            p.angle = std::fmod(p.angle + pickup_spin * dt, 6.2831853f);
            float m[9];
            memcpy(m, p.m, sizeof(m));
            yaw(m, p.angle);
            write_pose(rdram, p.record, m, p.local);
            float reach = (p.rec.kind == kind_powerup ? powerup_radius : pickup_radius) + touch_margin;
            for (int i = 0; i < max_cars; i++) {
                if (!alive(rdram, i)) continue;
                float c[3];
                car_pos(rdram, i, c);
                float d[3] = { c[0] - p.rec.pos[0], c[1] - p.rec.pos[1], c[2] - p.rec.pos[2] };
                if (dot(d, d) > reach * reach) continue;
                give(rdram, i, p.rec.kind);
                p.taken = true;
                p.respawn = powerup_respawn;
                hide_node(rdram, p.node);
                break;
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Cars

    // The weapon buttons a player holds (rush2::controls::battle_buttons). Test aid: R2_BATTLE_TEST=fire holds
    // player 1's FIRE.
    uint8_t weapon_buttons(uint8_t* rdram, int player) {
        static const bool test_fire = getenv("R2_BATTLE_TEST") != nullptr && strstr(getenv("R2_BATTLE_TEST"), "fire") != nullptr;
        uint8_t bits = rush2::controls::battle_buttons(MEM_BU(0, (int32_t)(player_slots + player * player_size + 1)));
        if (test_fire && std::fmod(clock_seconds, 0.5f) < 0.25f) bits |= rush2::controls::battle_fire;
        return bits;
    }

    void update_cars(uint8_t* rdram, float dt) {
        float steps = dt * tick_rate;
        int players = local_players(rdram);
        for (int i = 0; i < max_cars; i++) {
            Fighter& f = fighters[i];
            bool active = MEM_H(0, (int32_t)(car_at(i) + car_active)) != 0 && !rush2::ghost::is_ghost_car(i);
            if (active != f.present) {
                int kills = f.kills, deaths = f.deaths;
                f = Fighter{};
                f.present = active;
                f.kills = kills;
                f.deaths = deaths;
            }
            if (!f.present) continue;
            bool wrecked = is_wrecked(rdram, i);
            if (wrecked != f.was_wrecked) {
                // Wrecked, or back from it (func_8038CA24): the gun, no power-ups, and full health once it is back.
                f.weapon = gun;
                f.ammo = -1;
                f.invisible = f.shield = f.shield_scale = 0.0f;
                f.aim_yaw = f.aim_pitch = 0.0f;
                if (!wrecked) f.health = max_health;
            }
            f.was_wrecked = wrecked;
            f.cooldown = std::max(0.0f, f.cooldown - dt);
            f.invisible = std::max(0.0f, f.invisible - dt);
            // The shield: grows by 0.05 a step to its size, holds for its time, shrinks.
            if (f.shield > 0.0f) {
                float full = tuning.loaded ? tuning.shield_size[type_2049(rdram, i)] : 1.0f;
                f.shield_scale = std::min(full, f.shield_scale + 0.05f * steps);
                if (f.shield_scale >= full) f.shield -= dt;
            }
            else if (f.shield_scale > 0.0f) {
                f.shield_scale -= 0.05f * steps;
                if (f.shield_scale <= 0.06f) f.shield_scale = 0.0f;
            }
            f.shield_spin -= 0.005f * steps;
            if (wrecked) continue;

            int player = -1;
            for (int p = 0; p < players; p++) {
                if (player_car(rdram, p) == i) player = p;
            }
            uint8_t held = player >= 0 ? weapon_buttons(rdram, player) : 0;
            uint8_t pressed = held & ~f.buttons;
            f.buttons = held;

            if (f.weapon == cannon || f.weapon == gatling || f.weapon == rocket || f.weapon == gun) {
                update_aim(rdram, i, steps);
            }
            if ((pressed & rush2::controls::battle_drop) != 0) {
                f.weapon = gun;
                f.ammo = -1;
            }
            // func_8038FCE0: FIRE on a press, or held with the gatling, once the weapon is ready.
            bool trigger = (pressed & rush2::controls::battle_fire) != 0 ||
                           (f.weapon == gatling && (held & rush2::controls::battle_fire) != 0);
            if (trigger && f.cooldown <= 0.0f && f.ammo != 0 && f.weapon != ram) {
                fire(rdram, i);
            }

            // The ram (2049 main func_800CE358): its car damages the cars it runs into, using an ammo each time.
            if (f.weapon == ram && f.ammo > 0 && f.cooldown <= 0.0f) {
                float c[3], v[3];
                car_pos(rdram, i, c);
                car_vel(rdram, i, v);
                for (int j = 0; j < max_cars; j++) {
                    if (j == i || !alive(rdram, j)) continue;
                    float o[3], ov[3];
                    car_pos(rdram, j, o);
                    car_vel(rdram, j, ov);
                    float d[3] = { o[0] - c[0], o[1] - c[1], o[2] - c[2] };
                    float reach = read_f(rdram, car_at(i) + car_radius) + read_f(rdram, car_at(j) + car_radius) + 1.0f;
                    if (dot(d, d) > reach * reach) continue;
                    float rel[3] = { v[0] - ov[0], v[1] - ov[1], v[2] - ov[2] };
                    float closing = dot(rel, d) / std::max(length(d), 1e-3f);
                    if (closing < 20.0f) continue;
                    damage(rdram, j, i, ram_damage_base + 4.0f * closing / 1.4667f);
                    play_sound(rdram, weapons[ram].sound, c);
                    f.cooldown = 1.0f;
                    if (--f.ammo <= 0) {
                        f.weapon = gun;
                        f.ammo = -1;
                    }
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
                             const std::vector<uint8_t>& converted_geometry, const std::vector<float>& solid_triangles) {
    std::lock_guard lock{ battle_mutex };
    pickup_records = pickup_list;
    pool_records = pool_list;
    read_models(converted_geometry);
    build_collision(solid_triangles);
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
        const char* test_env = getenv("R2_BATTLE_TEST");
        bool test_short = test_env != nullptr && strstr(test_env, "short") != nullptr;   // Test aid: a 25 s battle.
        write_f(rdram, time_allowed, test_short ? 25.0f : (float)time_limit_seconds());
        clock_set = true;
    }
    if (const char* t = getenv("R2_BATTLE_TEST"); t != nullptr && strstr(t, "log") != nullptr && std::fmod(clock_seconds, 3.0f) < dt) {
        fprintf(stderr, "[Battle] t=%.1f allowed=%.2f state=%d clock=%.2f timer=%.2f set=%d\n", clock_seconds, read_f(rdram, time_allowed),
                (int)MEM_W(0, (int32_t)game_state), read_f(rdram, 0x8010C034), read_f(rdram, 0x80117488), (int)clock_set);
    }
    // The battle's time limit. The game's own test of the clock (func_800AE670, state 3: the time allowed less the
    // time raced is under half a second) waits for a far later time in a race without checkpoints (0x800D9E88), as
    // the arenas are; its out of time flag (0x800FAE98) is set here instead, and the game ends the race from it.
    if (clock_set && MEM_W(0, (int32_t)game_state) == 3) {
        float raced = read_f(rdram, game_timer) - read_f(rdram, race_clock_start);
        if (read_f(rdram, time_allowed) - raced <= 0.5f) MEM_B(0, (int32_t)out_of_time) = 1;
    }
    clock_seconds += dt;
    hud_spin = std::fmod(hud_spin + dt * 2.0f, 6.2831853f);
    if (const char* test = getenv("R2_BATTLE_TEST")) {
        // Test aids: "give<n>" gives car 0 weapon n at 2 s; "kill" destroys car 0 (as if by car 1) at 8 s.
        if (const char* g = strstr(test, "give"); g != nullptr && clock_seconds > 2.0f && clock_seconds <= 2.0f + dt) {
            for (int i = 0; i < max_cars; i++) {
                if (fighters[i].present) give(rdram, i, std::clamp(atoi(g + 4), 0, 11));
            }
        }
        if (strstr(test, "kill") != nullptr && clock_seconds > 8.0f && clock_seconds <= 8.0f + dt) {
            fighters[1].present = true;
            damage(rdram, 0, 1, 900.0f);
        }
    }
    update_cars(rdram, dt);
    update_pickups(rdram, dt);
    update_shots(rdram, dt);
    update_effects(rdram, dt);
    update_mounts(rdram);
    update_hud(rdram);
}

// ----------------------------------------------------------------------------------------------------------------
// HUD
//
// Rush 2049's battle HUD (func_80391B00 and the functions it calls): the health bar is 2D (HEALTHBG, a 64 x 8 frame,
// with HEALTHBAR inside it; here the fill is a rectangle, as a widget can't tint HEALTHBAR), and the weapon held, the
// power-up in effect and the player's coin are 3D models put in front of each view's camera at a screen position
// (func_800A6094), shown in that view only. The ammo is printed on the weapon and the kill count on the coin. With two
// players 2049 draws all of it a third wider than with one, three or four; the layout here follows that. Every element
// keeps its distance from an anchor of its view (an edge or the middle), so HUD Placement (Original, 16:9, the window's
// edges) moves the battle HUD as it moves Rush 2's; the digits are drawn by rush2::hud::draw_number, as the game's own
// text is queued and can't be anchored.
//
// The bar is an element of Rush 2's HUD layout system (func_800604FC builds an element, and its 2D widget, from a
// 0x28 byte layout entry whose +0 is the image's name; the converter merges 2049's HUD files 63 and 76 into the arena's
// geometry, where Rush 2 finds the images by name), placed in final screen coordinates and scaled by src/hud.cpp
// (rush2::hud::set_widget_scale). The models are pool records posed for each view just before it is drawn
// (rush2_battle_view). The stunt score panels are hidden (rush2_battle_hide_stunt_panel).
namespace {
    constexpr uint32_t speedometer_callback = 0x800BA2A8;
    constexpr uint32_t element_pool = 0x802F6400;     // Pointers to the HUD elements (func_80060418; moved by src/players4.cpp), 0xC8 at most
    constexpr uint32_t element_count = 0x80125A58;
    constexpr uint32_t hud_element_list = 0x8010C030; // Head of the HUD's element list (+0x3C next).
    constexpr uint32_t hud_entries = 0x8024F600;      // Layout entries, 0x28 bytes each (free RDRAM)
    constexpr uint32_t hud_names = 0x8024F900;        // The images' names, 16 bytes each
    constexpr uint32_t hud_colors = 0x8024F400;       // RGBA of the health bars' fills, 4 bytes per player
    constexpr int health_width = 64, health_height = 8;   // HEALTHBG
    constexpr int entry_size = 0x28;

    // 2049 puts the models 2 units ahead of the camera, scaled 0.069 (weapons; the sonic 0.175 at 5.75), 0.025 (the
    // power-up) and 0.012 (the coin; 0.009 with 3 or 4 views). Here they are hud_distance ahead, scaled to look the
    // same: close enough to stay above the road at the bottom of a view, clear of Rush 2's near plane.
    constexpr float hud_distance = 3.0f;
    constexpr float weapon_scale = 0.069f / 2.0f, sonic_scale = 0.175f / 5.75f, powerup_scale = 0.025f / 2.0f;
    // The coin's model is far bigger than the others in the converted geometry: its scales are set by its size on
    // screen in 2049 (a tenth of the screen's height; three quarters of that with 3 or 4 views) [I].
    constexpr float coin_scale = 0.0036f;
    // The tangent of half a view's vertical field of view comes from the view itself (rush2::splitscreen::view_tan_v:
    // 0.75, or 0.375 for the game's own stacked views); the horizontal one follows from the view's shape, which in
    // widescreen runs out to the window's edges.
    const char* const coin_models[4] = { "BCOIN_BLUEG1", "BCOIN_REDG1", "BCOIN_YELLOWG1", "BCOIN_GREENG1" };

    struct View {
        int x0, y0, x1, y1;
        bool right, bottom, wide;   // in the right column or bottom row of a split; drawn 2049's two player way
        // Where the view is drawn, in the same 4:3 screen pixels: in widescreen its outer side reaches the window's edge.
        float left = 0.0f, right_edge = 320.0f;
    };

    // Positions on the 4:3 screen. Each element keeps its distance from an anchor, a fraction of the screen's width
    // (the view's left edge, middle or right edge), which HUD Placement moves (rush2::hud::set_widget_scale).
    struct Layout {
        float bar_x, bar_y, bar_sx, bar_sy, bar_anchor;
        float weapon_x, weapon_y, weapon_anchor;
        float coin_x, coin_y, coin_anchor;
        float powerup_x, powerup_y;
        bool small;     // a quadrant: smaller digits
    };

    // Where a point of the 4:3 screen anchored at `anchor` shows, in 4:3 screen pixels from the 4:3 area's left edge.
    float anchored_x(float x, float anchor) {
        float width = rush2::splitscreen::hud_width();
        return (160.0f - width * 0.5f + anchor * width) + (x - anchor * 320.0f);
    }

    struct HudWidgets {
        int frame = -1;
        int fill = -1;       // The health bar's fill: a rectangle in the frame's inner area
    };
    HudWidgets hud_widgets[4];
    int hud_players = 0;

    // The part of the 320 x 240 screen a player's view covers.
    View view_of(uint8_t* rdram, int player, int players) {
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

    // Where a view's elements go: the bar centered at the bottom; the weapon at the bottom on the view's outer side;
    // the coin at the corner toward the screen's middle (2049's tables 0x80394150, 0x803941D0, 0x803940D0, 0x80394210).
    Layout layout_of(const View& v) {
        Layout l{};
        float w = (float)(v.x1 - v.x0), h = (float)(v.y1 - v.y0);
        bool tall = h > 120.0f;
        l.bar_sx = v.wide ? 4.0f / 3.0f : 1.0f;
        l.bar_sy = v.wide ? 8.0f / 7.0f : 1.0f;
        float bar_center_y = (float)v.y1 - (tall ? 32.0f : 13.0f);
        l.bar_x = (float)v.x0 + w * 0.5f - health_width * l.bar_sx * 0.5f;
        l.bar_y = bar_center_y - health_height * l.bar_sy * 0.5f;
        float side = v.wide ? 43.0f : w > 160.0f ? 32.0f : 40.0f;
        l.weapon_x = v.right ? (float)v.x1 - side : (float)v.x0 + side;
        l.weapon_y = bar_center_y;
        if (w <= 160.0f && h <= 120.0f) {
            // Quadrants: the coins meet at the middle of the screen.
            l.coin_x = v.right ? (float)v.x0 + 16.0f : (float)v.x1 - 16.0f;
            l.coin_y = v.bottom ? (float)v.y0 + 12.0f : (float)v.y1 - 14.0f;
        }
        else {
            // Stacked views: toward the line between them, as 2049. Otherwise at the bottom right, on the bar's line.
            l.coin_x = (float)v.x1 - 23.0f;
            l.coin_y = v.wide ? (v.bottom ? (float)v.y0 + 14.0f : (float)v.y1 - 14.0f) : bar_center_y;
        }
        l.small = w <= 160.0f && h <= 120.0f;
        l.powerup_x = l.bar_x + health_width * l.bar_sx + 14.0f;
        l.powerup_y = bar_center_y;
        if (v.right && w <= 160.0f) l.powerup_x = l.bar_x - 14.0f;
        l.bar_anchor = (float)(v.x0 + v.x1) / 640.0f;
        l.weapon_anchor = (float)(v.right ? v.x1 : v.x0) / 320.0f;
        l.coin_anchor = (float)(l.coin_x > (float)(v.x0 + v.x1) * 0.5f ? v.x1 : v.x0) / 320.0f;
        return l;
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

    // Whether the widget is still the one built for this HUD (a slot is reused once the HUD is torn down): it must
    // still draw an image.
    bool widget_valid(uint8_t* rdram, int slot) {
        return slot >= 0 && slot < 400 && MEM_W(0, (int32_t)(widget_at(slot) + 4)) != 0;
    }

    void update_hud(uint8_t* rdram) {
        for (int p = 0; p < hud_players; p++) {
            const HudWidgets& w = hud_widgets[p];
            int car = player_car(rdram, p);
            if (car < 0 || car >= max_cars || !widget_valid(rdram, w.frame)) continue;
            const Fighter& f = fighters[car];
            Layout l = layout_of(view_of(rdram, p, hud_players));
            int x = (int)std::lround(l.bar_x), y = (int)std::lround(l.bar_y);
            uint32_t frame = widget_at(w.frame);
            MEM_H(0xA, (int32_t)frame) = (int16_t)x;
            MEM_H(0xC, (int32_t)frame) = (int16_t)y;
            MEM_B(0x16, (int32_t)frame) = 0;
            rush2::hud::set_widget_scale(w.frame, l.bar_sx, l.bar_sy, l.bar_anchor);
            // The fill: green, as 2049's bar, inside the frame's 56 x 4 window.
            if (w.fill >= 0 && w.fill < 400) {
                uint32_t fill = widget_at(w.fill);
                uint32_t color = (uint32_t)MEM_W(0, (int32_t)fill);
                if (color == hud_colors + (uint32_t)p * 4) {
                    float fraction = std::clamp(f.health / max_health, 0.0f, 1.0f);
                    int width = (int)std::lround(fraction * 56.0f * l.bar_sx);
                    MEM_B(0, (int32_t)color) = 0;
                    MEM_B(1, (int32_t)color) = (int8_t)0x80;
                    MEM_B(2, (int32_t)color) = 0;
                    MEM_B(3, (int32_t)color) = (int8_t)0xFF;
                    MEM_H(0xA, (int32_t)fill) = (int16_t)(x + (int)std::lround(4.0f * l.bar_sx));
                    MEM_H(0xC, (int32_t)fill) = (int16_t)(y + (int)std::lround(2.0f * l.bar_sy));
                    MEM_H(0x10, (int32_t)fill) = (int16_t)width;
                    MEM_H(0x12, (int32_t)fill) = (int16_t)std::lround(4.0f * l.bar_sy);
                    MEM_B(0x16, (int32_t)fill) = width > 0 ? 0 : 1;
                    rush2::hud::set_widget_scale(w.fill, 1.0f, 1.0f, l.bar_anchor);
                }
            }
        }
    }

    // How much to scale a HUD model in a view so it has its size in screen pixels of a full-height view (the scales
    // above), times 0.7 in a quadrant: a quadrant keeps the full field of view at half the size.
    float model_gain(uint8_t* rdram, int p, const View& v) {
        float tan_y = rush2::splitscreen::view_tan_v(rdram, p);
        if (!(tan_y > 0.05f && tan_y < 4.0f)) tan_y = v.wide ? 0.375f : 0.75f;
        float h = (float)(v.y1 - v.y0);
        float gain = (120.0f / 0.75f) / (h * 0.5f / tan_y);
        return v.x1 - v.x0 <= 160 && h <= 120.0f ? gain * 0.7f : gain;
    }

    // The point `distance` ahead of view p's camera that shows at screen position (x, y) (2049 func_800A6094), and the
    // camera's axes.
    void view_point(uint8_t* rdram, int p, const View& v, float x, float y, float distance, float out[3], float axes[9]) {
        uint32_t cam = cameras + (uint32_t)p * 0x40;
        for (int k = 0; k < 9; k++) axes[k] = read_f(rdram, cam + k * 4);
        float w = v.right_edge - v.left, h = (float)(v.y1 - v.y0);
        float tan_y = rush2::splitscreen::view_tan_v(rdram, p);
        if (!(tan_y > 0.05f && tan_y < 4.0f)) tan_y = v.wide ? 0.375f : 0.75f;
        float tan_x = tan_y * w / h;
        float nx = (x - (v.left + w * 0.5f)) / (w * 0.5f), ny = (((float)v.y0 + h * 0.5f) - y) / (h * 0.5f);
        for (int i = 0; i < 3; i++) {
            out[i] = read_f(rdram, cam + 0x24 + i * 4) + axes[6 + i] * distance + axes[i] * nx * tan_x * distance +
                     axes[3 + i] * ny * tan_y * distance;
        }
    }
}

void rush2::battle::hud_built(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ battle_mutex };
    // The hook runs on every call of the HUD setup function, which returns early once the HUD exists: build once per
    // element list (players4::hud_built does the same).
    uint32_t head = (uint32_t)MEM_W(0, (int32_t)hud_element_list);
    if (head == 0 || head == built_head) return;
    built_head = head;
    rush2::hud::clear_widget_scales();
    for (HudWidgets& w : hud_widgets) w = HudWidgets{};
    hud_players = 0;
    int slot = rush2::track2049::loaded_slot();
    if (slot < 0 || MEM_B(0, (int32_t)track_id) != slot || rush2::track2049::battle_arena() <= 0) return;
    int players = local_players(rdram);
    static const char frame_name[] = "HEALTHBG";
    for (size_t c = 0; c < sizeof(frame_name); c++) MEM_B((int32_t)c, (int32_t)hud_names) = (int8_t)frame_name[c];
    for (int p = 0; p < players; p++) {
        uint32_t entry = hud_entries + (uint32_t)p * entry_size;
        make_entry(rdram, entry, hud_names, 0, 0);
        hud_widgets[p].frame = build_element(rdram, ctx, entry);
        for (int c = 0; c < 4; c++) MEM_B(c, (int32_t)(hud_colors + p * 4)) = 0;
        hud_widgets[p].fill = create_fill(rdram, ctx, hud_colors + (uint32_t)p * 4);
        rush2::hud::set_widget_scale(hud_widgets[p].frame, 1.0f, 1.0f);
        rush2::hud::set_widget_scale(hud_widgets[p].fill, 1.0f, 1.0f);
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

// After the widget draw loop: the ammo on each view's weapon and the kills on its coin.
void rush2::battle::hud_draw(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ battle_mutex };
    if (MEM_W(0, (int32_t)game_state) < 3) {
        // Back in the menus (the track stays loaded): the race's widget slots are theirs again.
        if (hud_players != 0 || built_head != 0) {
            rush2::hud::clear_widget_scales();
            for (HudWidgets& w : hud_widgets) w = HudWidgets{};
            hud_players = 0;
            built_head = 0;
        }
        return;
    }
    if (!ready || !battle_race(rdram) || hud_players == 0) return;
    // Not over the pause menu's screens (0x8002305C is set while one is open).
    if (MEM_B(0, (int32_t)0x8002305C) != 0) return;
    for (int p = 0; p < hud_players; p++) {
        int car = player_car(rdram, p);
        if (car < 0 || car >= max_cars) continue;
        const Fighter& f = fighters[car];
        Layout l = layout_of(view_of(rdram, p, hud_players));
        char text[16];
        // Whole texels in a quadrant (the digits are 10 tall): a smaller size loses their top row.
        float digits = l.small ? 10.0f : 12.0f;
        snprintf(text, sizeof(text), "%d", std::min(f.kills, 999));
        rush2::hud::draw_number(rdram, text, l.coin_x, l.coin_y, digits, l.coin_anchor);
        if (f.weapon < 8 && f.ammo > 0 && !is_wrecked(rdram, car)) {
            snprintf(text, sizeof(text), "%d", f.ammo);
            rush2::hud::draw_number(rdram, text, l.weapon_x, l.weapon_y, digits, l.weapon_anchor);
        }
    }
    // The weapons' models are drawn in the primitive color (2049 sets each to its car's color, func_8008E06C with the
    // car state's +0x34C; Rush 2's nodes carry no color), which is whatever was set last: leave a fixed steel blue.
    rush2::hud::set_prim_color(rdram, 0x7090B0FF);
}

// func_8007C27C entry ($a1 = the camera position of the view about to be drawn): shows that view's HUD models and
// hides the other views', as 2049 shows each of its HUD models in one view (func_8008B0D8 with the view's bit).
extern "C" void rush2_battle_view(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ battle_mutex };
    if (!ready || pool.size() < (size_t)hud_coin_slot_first + 4 || MEM_W(0, (int32_t)game_state) < 3) return;
    uint32_t at = (uint32_t)ctx->r5;
    int view = -1;
    if (at >= cameras && at < cameras + 4 * 0x40) {
        view = (int)((at - cameras) / 0x40);
    }
    else {
        // A copy of the position: the view whose camera is there.
        for (int p = 0; p < 4 && view < 0; p++) {
            bool same = true;
            for (int i = 0; i < 3; i++) {
                same = same && MEM_W(i * 4, (int32_t)at) == MEM_W(0x24 + i * 4, (int32_t)(cameras + (uint32_t)p * 0x40));
            }
            if (same) view = p;
        }
    }
    int players = local_players(rdram);
    // Invisible cars: the body's scene node (car state +0xF0, a node index) is hidden in the other players' views
    // (its wheels and shadow still show) and drawn in its own. 2049 fades the car out, to nothing in the other views.
    for (int i = 0; i < max_cars; i++) {
        Fighter& f = fighters[i];
        bool invisible = f.present && f.invisible > 0.0f && !is_wrecked(rdram, i);
        if (!invisible && !f.hidden) continue;
        int32_t body = MEM_W(0, (int32_t)(state_at(i) + state_body_node));
        if (body < 0 || body >= (int32_t)MEM_W(0, (int32_t)node_count)) continue;
        bool own = view >= 0 && view < players && player_car(rdram, view) == i;
        if (invisible && !own) hide_node(rdram, nodes + (uint32_t)body * node_size);
        else show_node(rdram, nodes + (uint32_t)body * node_size);
        f.hidden = invisible;
    }
    for (int p = 0; p < 4; p++) {
        int weapon_slot = hud_weapon_slot_first + p, powerup_slot = hud_powerup_slot_first + p, coin_slot = hud_coin_slot_first + p;
        int car = p < players ? player_car(rdram, p) : -1;
        if (p != view || car < 0 || car >= max_cars || !battle_race(rdram)) {
            hide_slot(rdram, weapon_slot);
            hide_slot(rdram, powerup_slot);
            hide_slot(rdram, coin_slot);
            continue;
        }
        const Fighter& f = fighters[car];
        View v = view_of(rdram, p, players);
        Layout l = layout_of(v);
        float pos[3], axes[9], m[9];
        float gain = model_gain(rdram, p, v) * hud_distance;
        // The weapon held, turning about the camera's up axis.
        if (f.weapon < 8 && !is_wrecked(rdram, car)) {
            view_point(rdram, p, v, anchored_x(l.weapon_x, l.weapon_anchor), l.weapon_y, hud_distance, pos, axes);
            memcpy(m, axes, sizeof(m));
            yaw(m, hud_spin);
            scale_matrix(m, (f.weapon == sonic ? sonic_scale : weapon_scale) * gain);
            color_slot(weapon_slot, player_colors[p]);
            place_slot(rdram, weapon_slot, mount_models[f.weapon], m, pos);
        }
        else {
            hide_slot(rdram, weapon_slot);
        }
        // The power-up in effect (invisibility before the shield), turning.
        if ((f.invisible > 0.0f || f.shield > 0.0f) && !is_wrecked(rdram, car)) {
            view_point(rdram, p, v, anchored_x(l.powerup_x, l.bar_anchor), l.powerup_y, hud_distance, pos, axes);
            memcpy(m, axes, sizeof(m));
            yaw(m, hud_spin);
            scale_matrix(m, powerup_scale * gain);
            place_slot(rdram, powerup_slot, f.invisible > 0.0f ? "WEPICON_INVSG1" : "WEPICON_SHLDG1", m, pos);
        }
        else {
            hide_slot(rdram, powerup_slot);
        }
        // The player's coin, face on (2049 tips its model a quarter turn back).
        view_point(rdram, p, v, anchored_x(l.coin_x, l.coin_anchor), l.coin_y, hud_distance, pos, axes);
        memcpy(m, axes, sizeof(m));
        pitch(m, -1.5707964f);
        scale_matrix(m, coin_scale * gain);
        place_slot(rdram, coin_slot, coin_models[p], m, pos);
    }
}

// func_800B9CC0 (the stunt score panel's visibility callback) at 0x800B9D2C: $a1 = whether to hide it. A battle arena is
// played in stunt mode but has no stunt score.
extern "C" void rush2_battle_hide_stunt_panel(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::battle_race(rdram)) ctx->r5 = 1;
}
