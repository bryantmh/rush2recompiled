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
// - Arrows (2049 main func_800B1B48 makes them, func_8008C884 moves them): each view has one for every other car, in
//   that car's player color. It floats over a car that is in view; for a car that isn't, it sits at the view's side
//   or bottom edge and points that way.
// - The round ends at the time limit; Rush 2049's results (main func_80105EA8) then name the winner in a box in the
//   middle of the screen ("%s WINS", "%d-WAY TIE") and give each view its player's name and points (kills).
// - Teams (0x8012E67C per player): cars of a team don't damage each other and share a color.
// Projectiles, mounted weapons, shields, explosions and the HUD's models are drawn by src/battle_render.cpp from
// Rush 2049's own model files, so they need nothing of the track. That is what lets the Weapons cheat (Cheats tab)
// give the players the same weapons in any other race: there the cars have the health, the weapons, the mounts and a
// small health bar and weapon on the HUD, and nothing else of a battle (no pickups, kills or time limit). Shots are
// stopped by the track's collision where it is a converted Rush 2049 track; on other tracks only the grenades and
// mines meet the ground, taken as level at the car that let them go.

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
#include "librecomp/addresses.hpp"
#include "assets.h"
#include "arrows.h"
#include "audio2049.h"
#include "battle.h"
#include "battle_render.h"
#include "car2049.h"
#include "ghost.h"
#include "rush2.h"
#include "rush2_hooks.h"
#include "track2049.h"
#include "wings.h"

extern "C" void hud_widget_alloc_800541F8(uint8_t* rdram, recomp_context* ctx);   // Allocates a 2D widget.
extern "C" void widgets_create_800604FC(uint8_t* rdram, recomp_context* ctx);   // Builds HUD elements from layout entries.
extern "C" void text_select_font_80088C24(uint8_t* rdram, recomp_context* ctx);   // Selects a font.
extern "C" void text_select_style_800737E4(uint8_t* rdram, recomp_context* ctx);  // Selects a text style.
extern "C" void text_measure_string_800732AC(uint8_t* rdram, recomp_context* ctx); // Width of a string.
extern "C" void text_print_string_800734E0(uint8_t* rdram, recomp_context* ctx);  // Prints a string at (x, y).

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
    constexpr uint32_t state_shadow = 0x20C;         // the car's shadow polygon (func_80086CA4)
    constexpr uint32_t player_slots = 0x800C2140;    // 0x28 per player: +0 car index, +1 controller port, +2 pressed, +4 held.
    constexpr uint32_t player_size = 0x28;
    constexpr uint32_t num_players = 0x8010C3E2;     // s16
    constexpr uint32_t record_base = 0x8010C15C;     // Pointer to the placement file's first record.
    constexpr uint32_t record_size = 0x64;
    constexpr uint32_t nodes = 0x800D9E90;           // Scene nodes, 0x38 bytes each.
    constexpr uint32_t node_size = 0x38;
    constexpr uint32_t node_count = 0x800FAE58;
    constexpr uint32_t node_hidden = 0x400;
    constexpr uint32_t body_nodes = 0x80219DD0;      // + car * 0x134: the car's body scene node index (src/ghost.cpp)
    constexpr uint32_t body_node_stride = 0x134;
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
    constexpr float explosion_scale = 0.5f;                 // func_800AF06C's size for a weapon's explosion
    constexpr int explosion_frames = 30;                    // NEXPLOSIONG1-30 (handles 0x80142908), one each 1/30 s (func_800908A0)
    constexpr float explosion_sound_seconds = 1.5f;         // [I] sound 0x45 holds until its key-off
    constexpr int pickup_sound = 0x60;                      // the WEPICON_* types' sound (type table +0x1C)
    constexpr float pickup_spin = 3.0f;                     // rad/s about its up axis: the type's rate, table 0x80118D70 row 0

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
        bool shadow_hidden = false;     // its shadow polygon was hidden here (invisible, in another player's view)
        float regive = 0.0f;            // the Weapons cheat: seconds until it is given its weapon again
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
        float ground = -1e9f;           // the level ground under the car that fired it (tracks without collision here)
    };

    struct Effect {
        bool live = false;
        int slot = -1;
        float pos[3] = {};
        float age = 0.0f, duration = 0.3f, scale0 = 1.0f, scale1 = 4.0f;
        bool explosion = false;         // 2049's explosion: its 30 frames
    };

    // The renderer's slots (src/battle_render.cpp).
    constexpr int shot_slots = 32;               // 0-31: projectiles and effects
    constexpr int mount_slot_first = 32;         // 32-39: the weapon mounted on each car
    constexpr int hud_weapon_slot_first = 40;    // 40-43: the HUD's weapon, per view
    constexpr int shield_slot_first = 44;        // 44-51: each car's shield
    constexpr int hud_powerup_slot_first = 52;   // 52-55: the HUD's power-up, per view
    constexpr int hud_coin_slot_first = 56;      // 56-59: the HUD's coin, per view
    constexpr int slot_count = 60;
    struct Slot {
        bool used = false;
        uint32_t color = 0xFFFFFFFF;
        int view = -1;          // the one view it is drawn in (the HUD's models)
        bool attached = false;  // it rides with that view's camera
    };

    // What is running: a battle arena, or the Weapons cheat in another race.
    enum class Mode { none, arena, cheat };

    std::mutex battle_mutex;
    std::vector<PickupRecord> pickup_records;
    std::vector<int> pool_records;
    std::map<std::string, uint16_t> models;     // Converted geometry: name -> handle index.
    bool setup_pending = false, ready = false;
    Fighter fighters[max_cars];
    std::vector<Pickup> pickups;
    Slot slots[slot_count];
    Mode mode = Mode::none;
    int last_state = 0;          // The game state at the last tick: a race starts with its countdown (10).
    std::atomic_int cheat_option = 0;            // The Weapons cheat: 0 off, 1-8 a weapon (+1), 9 invisibility, 10 random
    std::atomic_int team_option[4] = { 0, 1, 2, 3 };
    bool over = false;           // The round's time is up: the results show.
    bool flat_ground = false;    // No collision triangles for this track: grenades and mines meet level ground.
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

    // The pose a car's body is drawn with in this frame: its scene node's transform (rotation rows, then the position
    // at +0x24; src/interpolation.cpp). The draw state above is the pose of the physics tick, which the game moves on
    // from before it draws, so a model placed by it slides about on the car.
    void body_pose(uint8_t* rdram, int i, float pos[3], float m[9]) {
        int body = (int32_t)MEM_W(0, (int32_t)(body_nodes + (uint32_t)i * body_node_stride));
        uint32_t transform = body >= 0 && body < 0x400 ? (uint32_t)MEM_W(4, (int32_t)(nodes + (uint32_t)body * node_size)) : 0;
        if (transform >= 0x80000000u && transform < 0x80800000u) {
            for (int k = 0; k < 9; k++) m[k] = read_f(rdram, transform + k * 4);
            for (int k = 0; k < 3; k++) pos[k] = read_f(rdram, transform + 0x24 + k * 4);
            if (std::fabs(dot(m, m) - 1.0f) < 0.2f) return;
        }
        car_pos(rdram, i, pos);
        car_axes(rdram, i, m);
    }

    bool is_wrecked(uint8_t* rdram, int i) { return MEM_B(0, (int32_t)(car_at(i) + car_wrecked)) != 0; }

    bool alive(uint8_t* rdram, int i) { return fighters[i].present && !is_wrecked(rdram, i); }

    using rush2::views::local_players;
    using rush2::views::player_car;

    // The player who drives car i, or -1.
    int player_of(uint8_t* rdram, int car) {
        int players = local_players(rdram);
        for (int p = 0; p < players; p++) {
            if (player_car(rdram, p) == car) return p;
        }
        return -1;
    }

    int team_of_car(uint8_t* rdram, int car) {
        int player = player_of(rdram, car);
        return player < 0 ? -1 : std::clamp<int>(team_option[player], 0, 3);
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
        constexpr float roof_sink = -0.35f;  // negative: the weapon's base sits above the roof, not inside it
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
        for (int i = 0; i < shot_slots; i++) {
            if (!slots[i].used) {
                slots[i].used = true;
                return i;
            }
        }
        return -1;
    }

    void hide_slot(uint8_t*, int slot) {
        if (slot >= 0 && slot < slot_count) rush2::battle_render::hide(slot);
    }

    void free_slot(uint8_t*, int slot) {
        if (slot < 0 || slot >= slot_count) return;
        slots[slot].used = false;
        rush2::battle_render::hide(slot);
    }

    void place_slot(uint8_t*, int slot, const char* model, const float m[9], const float pos[3]) {
        if (slot < 0 || slot >= slot_count) return;
        rush2::battle_render::place(slot, model, m, pos, slots[slot].color, slots[slot].view, slots[slot].attached);
    }

    // Colors (the models' primitive color; most of them have a combiner that doesn't use it). 2049 gives the weapon
    // on a car the car's light level (its car state's +0x34C, func_8008E06C: white, from the table 0x8011AE58 that
    // darkens the car), the HUD's weapon white (0x803942A4) and a muzzle flash 0xFFFF2B (func_8038AA8C): the weapons
    // show in their own textures' colors, the same on every car. The mine's parts and the pickups (2049 sets no
    // color for a pickup's node) are by eye [I].
    constexpr uint32_t neutral_color = 0x7090B0FF, shell_color = 0xB0B0B0FF, flash_color = 0xFFFF2BFF, plain_color = 0xFFFFFFFF;

    void color_slot(int slot, uint32_t rgba) {
        if (slot >= 0 && slot < slot_count) slots[slot].color = rgba;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Sounds: 2049 plays a weapon's sound from its car (func_800B61A8), with its 3D emitter law for each local player.

    // Sounds that hold until their key-off (the explosion's), and when to send it.
    struct HeldSound {
        int handle;
        float stop_at;
    };
    std::vector<HeldSound> held_sounds;

    void play_sound(uint8_t* rdram, int id, const float pos[3], float hold_seconds = 0.0f) {
        // music_ready starts loading the sound banks if nothing has yet (a race with Rush 2's music and cars).
        if (id < 0 || !rush2::track2049::music_ready() || !rush2::audio2049::loaded()) return;
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
        int handle = rush2::audio2049::sfx_start(id, std::min(volume, 1.0f), pan, 1.0f, surround);
        if (handle >= 0 && hold_seconds > 0.0f) held_sounds.push_back({ handle, clock_seconds + hold_seconds });
        static const bool test_log = getenv("R2_BATTLE_TEST") != nullptr && strstr(getenv("R2_BATTLE_TEST"), "log") != nullptr;
        if (test_log) fprintf(stderr, "[Battle] sound 0x%X volume %.2f pan %.2f handle %d\n", id, volume, pan, handle);
    }

    // The 2049 sound effects are mixed only while something reports them in use each frame (they go quiet with the
    // race paused, src/track2049_audio.cpp); Rush 2049's cars' engines do, and so must a battle, or its sounds are
    // silent with a Rush 2 car.
    void update_sounds(uint8_t* rdram) {
        if (rush2::track2049::music_ready()) rush2::track2049::effects_running(rdram);
        for (HeldSound& h : held_sounds) {
            if (clock_seconds >= h.stop_at) {
                rush2::audio2049::sfx_stop(h.handle);
                h.handle = -1;
            }
        }
        held_sounds.erase(std::remove_if(held_sounds.begin(), held_sounds.end(), [](const HeldSound& h) { return h.handle < 0; }),
                          held_sounds.end());
    }

    // ------------------------------------------------------------------------------------------------------------
    // Setup

    bool battle_race(uint8_t* rdram) {
        int slot = rush2::track2049::loaded_slot();
        return slot >= 0 && MEM_B(0, (int32_t)track_id) == slot && rush2::track2049::battle_arena() > 0;
    }

    void set_faded(int car, int view) {
        rush2::ghost::set_faded(car, view);
    }

    // Puts back what a round changed outside this file's own state.
    void shutdown(uint8_t* rdram) {
        for (int i = 0; i < max_cars; i++) {
            set_faded(i, -1);
            if (fighters[i].shadow_hidden) {
                uint32_t shadow = (uint32_t)MEM_W(0, (int32_t)(state_at(i) + state_shadow));
                if (shadow >= 0x80000000u) MEM_H(2, (int32_t)shadow) = (int16_t)(MEM_HU(2, (int32_t)shadow) & ~0x8000);
                fighters[i].shadow_hidden = false;
            }
        }
        rush2::battle_render::clear();
        for (HeldSound& h : held_sounds) rush2::audio2049::sfx_stop(h.handle);
        held_sounds.clear();
        ready = false;
        mode = Mode::none;
        over = false;
    }

    void setup(uint8_t* rdram, Mode m) {
        shutdown(rdram);
        mode = m;
        pickups.clear();
        shots.clear();
        effects.clear();
        for (Fighter& f : fighters) f = Fighter{};
        for (Slot& slot : slots) slot = Slot{};
        load_tuning();
        if (!rush2::battle_render::ready(rdram) && m == Mode::cheat) {
            mode = Mode::none;
            return;
        }
        for (int p = 0; p < 4; p++) {
            for (int first : { hud_weapon_slot_first, hud_powerup_slot_first, hud_coin_slot_first }) {
                slots[first + p].view = p;
                slots[first + p].attached = true;
            }
        }
        // The track's collision is this file's only for a converted Rush 2049 track that is the one loaded.
        int slot49 = rush2::track2049::loaded_slot();
        flat_ground = triangles.empty() || slot49 < 0 || MEM_B(0, (int32_t)track_id) != slot49;
        if (m == Mode::arena) {
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
            // The converter's pool of spare records (the models were drawn from them before src/battle_render.cpp)
            // stays hidden.
            for (int record : pool_records) {
                auto n = node_of_matrix.find(record_pointer(rdram, record) + 0x10);
                if (n != node_of_matrix.end()) hide_node(rdram, n->second);
            }
            rush2::interpolation_clear_view_attached();
            for (const Pickup& p : pickups) rush2::interpolation_node_color(p.node, neutral_color);
            if (const char* test = getenv("R2_BATTLE_TEST"); test != nullptr && strstr(test, "log") != nullptr) {
                // Test aid: the pickups are world-space records with scene nodes, at the 2049 objects' positions.
                int with_node = 0;
                float off = 0.0f;
                for (const Pickup& p : pickups) {
                    with_node += p.node != 0;
                    for (int k = 0; k < 3; k++) off = std::max(off, std::fabs(p.local[k] - p.rec.pos[k]));
                }
                fprintf(stderr, "[Battle] %d pickups (%d with nodes, %.3f from their places), %d nodes\n", (int)pickups.size(), with_node, off, count);
            }
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
        // Nothing to a car of the attacker's team (0x8012E67C).
        if (attacker >= 0 && attacker < max_cars && team_of_car(rdram, attacker) >= 0 &&
            team_of_car(rdram, attacker) == team_of_car(rdram, victim)) {
            return;
        }
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

    // 2049 main func_800AF06C(pos, 0, 0.5, 1): the game's explosion, its 30 frames at half size.
    void add_explosion(const float pos[3]) {
        int slot = alloc_slot();
        if (slot < 0) return;
        Effect e;
        e.live = true;
        e.slot = slot;
        memcpy(e.pos, pos, sizeof(e.pos));
        e.duration = (float)explosion_frames / tick_rate;
        e.explosion = true;
        color_slot(slot, plain_color);
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
        play_sound(rdram, explosion_sound, pos, explosion_sound_seconds);
        add_explosion(pos);
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
        s.ground = c[1];
        s.slot = alloc_slot();
        color_slot(s.slot, f.weapon == mine ? shell_color : plain_color);
        play_sound(rdram, w.sound, s.pos);
        if (s.slot < 0 && f.weapon != sonic) {
            // No record to draw it with: the shot is lost (the ammo is still used).
            s.live = false;
        }
        if (s.live) shots.push_back(s);
        // The Weapons cheat's weapons never run out (their ammo isn't shown either).
        if (f.ammo > 0 && mode != Mode::cheat) {
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
            if (!walled && flat_ground && (s.weapon == mine || s.weapon == grenade) && s.pos[1] < s.ground && s.prev[1] >= s.pos[1]) {
                // A track without collision here: level ground at the height of the car that let the shot go.
                walled = true;
                wall[0] = s.pos[0];
                wall[1] = s.ground + 0.05f;
                wall[2] = s.pos[2];
                normal[0] = 0.0f;
                normal[1] = 1.0f;
                normal[2] = 0.0f;
            }
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
            if (e.explosion) {
                char frame[16];
                snprintf(frame, sizeof(frame), "NEXPLOSIONG%d", std::min(explosion_frames, 1 + (int)(e.age * tick_rate)));
                float m[9] = { explosion_scale, 0, 0, 0, explosion_scale, 0, 0, 0, explosion_scale };
                place_slot(rdram, e.slot, frame, m, e.pos);
                continue;
            }
            float scale = e.scale0 + (e.scale1 - e.scale0) * k;
            float m[9] = { scale, 0, 0, 0, scale, 0, 0, 0, scale };
            place_slot(rdram, e.slot, "WFX_MFLSHG11", m, e.pos);
        }
        effects.erase(std::remove_if(effects.begin(), effects.end(), [](const Effect& e) { return !e.live; }), effects.end());
    }

    // func_8038AA8C: the weapon a car holds sits on it at the weapon's offset for its car model, turned with the car.
    // The gun shows nothing. A mine is held behind the car until it is let go. Called before each view is drawn, when
    // the cars' bodies have the frame's poses.
    void update_mounts(uint8_t* rdram) {
        for (int i = 0; i < max_cars; i++) {
            const Fighter& f = fighters[i];
            int slot = mount_slot_first + i;
            if (!alive(rdram, i) || f.weapon >= 8 || f.invisible > 0.0f) {
                hide_slot(rdram, slot);
            }
            else {
                float c[3], m[9], local[3], pos[3];
                body_pose(rdram, i, c, m);
                mount_offset(rdram, i, f.weapon, local);
                offset_point(c, m, local, pos);
                color_slot(slot, plain_color);
                place_slot(rdram, slot, mount_models[f.weapon], m, pos);
            }
            // func_8038F938: the shield's globe grows to the car model's size, holds, then shrinks.
            int shield_slot = shield_slot_first + i;
            if (!alive(rdram, i) || f.shield_scale <= 0.0f) {
                hide_slot(rdram, shield_slot);
            }
            else {
                float c[3], m[9];
                body_pose(rdram, i, c, m);
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
                play_sound(rdram, pickup_sound, p.rec.pos);
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
                    if (mode != Mode::cheat && --f.ammo <= 0) {
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

void rush2::battle::set_weapons_cheat(int option) {
    cheat_option = std::clamp(option, 0, 10);
}

void rush2::battle::set_team(int player, int team) {
    if (player >= 0 && player < 4) team_option[player] = std::clamp(team, 0, 3);
}

int rush2::battle::team_of(int player) {
    return player >= 0 && player < 4 ? std::clamp<int>(team_option[player], 0, 3) : 0;
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
    return ready && mode == Mode::arena && battle_race(rdram);
}

bool rush2::battle::arrow_target(uint8_t* rdram, int car, bool& faded) {
    std::lock_guard lock{ battle_mutex };
    if (!ready || mode != Mode::arena || car < 0 || car >= max_cars || !alive(rdram, car)) return false;
    faded = fighters[car].invisible > 0.0f;
    return true;
}

namespace {
    // The Weapons cheat: a player's car without a weapon (or, for the invisibility, seen again) is given the chosen
    // one again a few seconds later; RANDOM picks one of the eight each time.
    constexpr float cheat_regive_seconds = 3.0f;

    void update_cheat(uint8_t* rdram, float dt) {
        int option = cheat_option;
        int players = local_players(rdram);
        for (int p = 0; p < players; p++) {
            int car = player_car(rdram, p);
            if (car < 0 || car >= max_cars || !alive(rdram, car)) continue;
            Fighter& f = fighters[car];
            bool has = option == 9 ? f.invisible > 0.0f : f.weapon != gun;
            if (has) {
                f.regive = cheat_regive_seconds;
                continue;
            }
            f.regive -= dt;
            if (f.regive > 0.0f) continue;
            f.regive = cheat_regive_seconds;
            int kind = option == 9 ? (int)kind_invisibility : option == 10 ? std::uniform_int_distribution<int>(0, 7)(rng) : option - 1;
            give(rdram, car, kind);
            float pos[3];
            car_pos(rdram, car, pos);
            play_sound(rdram, pickup_sound, pos);
        }
    }

    // The winner when the time is up (2049 main func_80105EA8): the player with the most kills, or how many tie.
    struct Results {
        int winner = -1, tied = 0;
        int kills[4] = {};
        int players = 0;
    };
    Results results;

    void end_round(uint8_t* rdram) {
        over = true;
        results = Results{};
        for (Shot& shot : shots) {
            if (shot.live) end_shot(rdram, shot);
        }
        shots.clear();
        results.players = local_players(rdram);
        int best = -1;
        for (int p = 0; p < results.players; p++) {
            int car = player_car(rdram, p);
            results.kills[p] = car >= 0 && car < max_cars ? fighters[car].kills : 0;
            if (results.kills[p] > best) {
                best = results.kills[p];
                results.winner = p;
                results.tied = 1;
            }
            else if (results.kills[p] == best) {
                results.tied++;
            }
        }
    }
}

void rush2::battle::tick(uint8_t* rdram, float dt) {
    std::lock_guard lock{ battle_mutex };
    int state = MEM_W(0, (int32_t)game_state);
    // Every race starts with its countdown (state 10): the Weapons cheat's races are set up there (an arena also by
    // reset, when its track is loaded).
    if (state == 10 && last_state != 10) setup_pending = true;
    last_state = state;
    Mode want = battle_race(rdram) ? Mode::arena : cheat_option != 0 && (state == 3 || state == 10) ? Mode::cheat : Mode::none;
    if (want == Mode::none) {
        if (ready || mode != Mode::none) shutdown(rdram);
        return;
    }
    if (setup_pending || want != mode) {
        setup_pending = false;
        setup(rdram, want);
    }
    if (!ready) return;
    const char* test_env = getenv("R2_BATTLE_TEST");
    if (mode == Mode::arena) {
        // The stunt clock: 0x8010C204 holds the time allowed, which the HUD counts down from. It is the start
        // countdown's 3.5 s until the race begins (writing it early would stretch the countdown) and then 300, which
        // becomes the limit.
        if (!clock_set && read_f(rdram, time_allowed) >= 60.0f) {
            bool test_short = test_env != nullptr && strstr(test_env, "short") != nullptr;   // Test aid: a 25 s battle.
            write_f(rdram, time_allowed, test_short ? 25.0f : (float)time_limit_seconds());
            clock_set = true;
        }
        // The battle's time limit. The game's own test of the clock (func_800AE670, state 3: the time allowed less the
        // time raced is under half a second) waits for a far later time in a race without checkpoints (0x800D9E88), as
        // the arenas are; its out of time flag (0x800FAE98) is set here instead, and the game ends the race from it.
        if (clock_set && state == 3) {
            float raced = read_f(rdram, game_timer) - read_f(rdram, race_clock_start);
            if (read_f(rdram, time_allowed) - raced <= 0.5f) {
                MEM_B(0, (int32_t)out_of_time) = 1;
                if (!over) end_round(rdram);
            }
        }
    }
    if (test_env != nullptr && strstr(test_env, "log") != nullptr && std::fmod(clock_seconds, 3.0f) < dt) {
        fprintf(stderr, "[Battle] t=%.1f mode=%d allowed=%.2f state=%d set=%d over=%d\n", clock_seconds, (int)mode, read_f(rdram, time_allowed),
                state, (int)clock_set, (int)over);
    }
    clock_seconds += dt;
    hud_spin = std::fmod(hud_spin + dt * 2.0f, 6.2831853f);
    if (test_env != nullptr) {
        // Test aids: "give<n>" gives every car pickup kind n at 2 s; "kill" destroys car 0 (as if by car 1) at 8 s.
        if (const char* g = strstr(test_env, "give"); g != nullptr && clock_seconds > 2.0f && clock_seconds <= 2.0f + dt) {
            for (int i = 0; i < max_cars; i++) {
                if (fighters[i].present) give(rdram, i, std::clamp(atoi(g + 4), 0, 11));
            }
        }
        if (strstr(test_env, "kill") != nullptr && clock_seconds > 8.0f && clock_seconds <= 8.0f + dt) {
            fighters[1].present = true;
            damage(rdram, 0, 1, 900.0f);
        }
    }
    update_sounds(rdram);
    if (over) {
        // The round is decided: what is in flight goes on, nothing new is fired or scored.
        update_effects(rdram, dt);
        update_hud(rdram);
        return;
    }
    update_cars(rdram, dt);
    if (mode == Mode::cheat) update_cheat(rdram, dt);
    update_pickups(rdram, dt);
    update_shots(rdram, dt);
    update_effects(rdram, dt);
    // An invisible car (2049 fades it out, to nothing in the other views): only its own player's view draws it, as a
    // ghost.
    for (int i = 0; i < max_cars; i++) {
        const Fighter& f = fighters[i];
        bool invisible = f.present && f.invisible > 0.0f && !is_wrecked(rdram, i);
        int own = player_of(rdram, i);
        set_faded(i, invisible ? (own >= 0 ? own : 7) : -1);
    }
    if (mode == Mode::arena) update_hud(rdram);
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
// (rush2::hud::set_widget_scale). The models are placed for each view just before it is drawn (rush2_battle_view)
// and drawn by src/battle_render.cpp. The stunt score panels are hidden (rush2_battle_hide_stunt_panel).
//
// With the Weapons cheat in another race Rush 2's HUD stays as it is and the battle's health bar is added at the
// bottom of each view (the cheat's weapons never run out, so the weapon held and its ammo aren't shown). Such a track has no HEALTHBG among its images, so the frame is
// drawn from Rush 2049's HUD file itself (rush2::battle_render::image), at the battle's size with the battle's fill.
namespace {
    using rush2::views::View;
    using rush2::views::view_of;

    constexpr uint32_t speedometer_callback = 0x800BA2A8;
    // The clock: Rush 2's stunt mode shows the time left, which is the round's (2049 shows a battle's time too).
    constexpr uint32_t race_time_callback = 0x800B7B1C, countdown_time_callback = 0x800B94C0;
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
            // Quadrants: the coins meet at the middle of the screen, either side of Rush 2's clock there.
            l.coin_x = v.right ? (float)v.x0 + 34.0f : (float)v.x1 - 34.0f;
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
        // The models keep their size on screen when the game changes the view's field of view (the start's camera).
        float h = (float)(v.y1 - v.y0);
        float gain = (120.0f / 0.75f) / (h * 0.5f / tan_y);
        return v.x1 - v.x0 <= 160 && h <= 120.0f ? gain * 0.7f : gain;
    }

    // The point `distance` ahead of view p's camera that shows at screen position (x, y) (2049 func_800A6094), and the
    // camera's axes.
    void view_point(uint8_t* rdram, int p, const View& v, float x, float y, float distance, float out[3], float axes[9]) {
        uint32_t cam = cameras + (uint32_t)p * 0x40;
        rush2::views::axes(rdram, p, axes);
        float w = v.right_edge - v.left, h = (float)(v.y1 - v.y0);
        float tan_x, tan_y;
        rush2::views::tangents(rdram, p, v, tan_x, tan_y);
        float nx = (x - (v.left + w * 0.5f)) / (w * 0.5f), ny = (((float)v.y0 + h * 0.5f) - y) / (h * 0.5f);
        for (int i = 0; i < 3; i++) {
            out[i] = read_f(rdram, cam + 0x24 + i * 4) + axes[6 + i] * distance + axes[i] * nx * tan_x * distance +
                     axes[3 + i] * ny * tan_y * distance;
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Text (the results), printed with the game's own fonts.

    constexpr int results_font = 0;
    constexpr int style_white = 1, style_highlight = 0x14;
    uint32_t text_scratch = 0, text_at = 0;
    constexpr uint32_t text_scratch_size = 0x200;

    int32_t call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0 = 0, int32_t a1 = 0,
                 int32_t a2 = 0, int32_t a3 = 0) {
        recomp_context saved = *ctx;
        ctx->r4 = a0;
        ctx->r5 = a1;
        ctx->r6 = a2;
        ctx->r7 = a3;
        func(rdram, ctx);
        int32_t ret = (int32_t)ctx->r2;
        *ctx = saved;
        return ret;
    }

    uint32_t game_string(uint8_t* rdram, const std::string& text) {
        if (text_scratch == 0) {
            text_scratch = (uint32_t)((uint8_t*)recomp::alloc(rdram, text_scratch_size) - rdram) + 0x80000000u;
        }
        if (text_at + text.size() + 1 > text_scratch_size) text_at = 0;
        uint32_t at = text_scratch + text_at;
        for (int i = 0; i <= (int)text.size(); i++) MEM_B(i, (int32_t)at) = i < (int)text.size() ? text[i] : 0;
        text_at += (uint32_t)((text.size() + 4) & ~3u);
        return at;
    }

    // Prints `text` centered on x.
    void print_centered(uint8_t* rdram, recomp_context* ctx, int style, int x, int y, const std::string& text) {
        uint32_t at = game_string(rdram, text);
        int width = call(rdram, ctx, text_measure_string_800732AC, (int32_t)at, -1);
        call(rdram, ctx, text_select_style_800737E4, style);
        call(rdram, ctx, text_print_string_800734E0, x - width / 2, y, (int32_t)at);
    }

    // 2049 main func_80105EA8: with two or more views a box in the middle of the screen names the winner ("%s WINS",
    // "%d-WAY TIE"), and each view shows its player's name over its points (the kills, "%s\n%d %s" with POINTS).
    // 2049 prints the players' profile names; here they are PLAYER 1-4 [I].
    void draw_results(uint8_t* rdram, recomp_context* ctx) {
        text_at = 0;
        call(rdram, ctx, text_select_font_80088C24, results_font);
        constexpr uint32_t box_color = 0x000000C0;
        for (int p = 0; p < results.players; p++) {
            View v = view_of(rdram, p, results.players);
            int cx = (v.x0 + v.x1) / 2, cy = (v.y0 + v.y1) / 2 + (results.players > 1 ? (v.bottom ? 24 : -30) : 0);
            if (results.players == 2 && !v.wide) cy = (v.y0 + v.y1) / 2 + 40;
            // The game's text is anchored by the third of the screen it is printed in (src/hud.cpp): the box too.
            float anchor = cx < 107 ? 0.0f : cx < 214 ? 0.5f : 1.0f;
            rush2::hud::draw_rect(rdram, (float)cx - 45.0f, (float)cy - 13.0f, (float)cx + 45.0f, (float)cy + 13.0f, box_color, anchor);
            print_centered(rdram, ctx, style_white, cx, cy - 10, "PLAYER " + std::to_string(p + 1));
            print_centered(rdram, ctx, style_white, cx, cy + 1, std::to_string(results.kills[p]) + " POINTS");
        }
        if (results.players >= 2) {
            // The box 2049 opens at (91, 106) - (229, 114), a text line taller.
            rush2::hud::draw_rect(rdram, 91.0f, 101.0f, 229.0f, 119.0f, box_color, 0.5f);
            std::string line = results.tied > 1 ? std::to_string(results.tied) + "-WAY TIE"
                                                : "PLAYER " + std::to_string(results.winner + 1) + " WINS";
            print_centered(rdram, ctx, style_highlight, 160, 105, line);
        }
    }

    // The Weapons cheat's bar, the battle's (update_hud): HEALTHBG with the green fill in its 56 x 4 window.
    void draw_cheat_bar(uint8_t* rdram, const Layout& l, float fraction) {
        uint32_t image = 0;
        int w = 0, h = 0;
        if (!rush2::battle_render::image(rdram, "HEALTHBG", &image, &w, &h)) return;
        float x = std::round(l.bar_x), y = std::round(l.bar_y);
        rush2::hud::draw_image(rdram, image, w, h, x, y, x + (float)w * l.bar_sx, y + (float)h * l.bar_sy, l.bar_anchor);
        float x0 = x + std::round(4.0f * l.bar_sx), y0 = y + std::round(2.0f * l.bar_sy);
        float width = std::round(std::clamp(fraction, 0.0f, 1.0f) * 56.0f * l.bar_sx);
        rush2::hud::draw_rect(rdram, x0, y0, x0 + width, y0 + std::round(4.0f * l.bar_sy), 0x008000FF, l.bar_anchor);
    }

    // The Weapons cheat shows only the bar (its weapons are endless, so neither the weapon held nor its ammo is shown),
    // centered at the bottom of the view. In split screen, where that runs into player p's time or position (the
    // bottom row of quadrants in 4:3), it goes above them.
    Layout cheat_layout(const View& v, int p) {
        Layout l = layout_of(v);
        rush2::hud::PanelBounds row;
        if (!rush2::hud::panel_row(p, row)) return l;
        constexpr float gap = 2.0f;
        float bar_y1 = l.bar_y + health_height * l.bar_sy;
        if (bar_y1 < row.y0 || l.bar_y > row.y1) return l;
        float bar_x0 = anchored_x(l.bar_x, l.bar_anchor), bar_x1 = anchored_x(l.bar_x + health_width * l.bar_sx, l.bar_anchor);
        if (bar_x0 < row.time_x1 + gap || bar_x1 > row.place_x0 - gap) {
            float dy = row.y0 - gap - bar_y1;
            l.bar_y += dy;
            l.powerup_y += dy;
        }
        return l;
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
    for (int c = 0; c < (int)sizeof(frame_name); c++) MEM_B(c, (int32_t)hud_names) = (int8_t)frame_name[c];
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
    // Rush 2's own HUD is hidden except the speedometer and the clock: every other element with a callback loses its
    // callback (which would show it again) and is hidden with its widget. The elements are in a pool (func_80060418).
    int element_total = (int)MEM_W(0, (int32_t)element_count);
    for (int i = 0; i < element_total && i < 0xC8; i++) {
        uint32_t e = (uint32_t)MEM_W(0, (int32_t)(element_pool + (uint32_t)i * 4));
        uint32_t callback = e == 0 ? 0 : (uint32_t)MEM_W(0x28, (int32_t)e);
        if (callback == 0 || callback == speedometer_callback || callback == race_time_callback || callback == countdown_time_callback) {
            continue;
        }
        MEM_W(0x28, (int32_t)e) = 0;
        MEM_B(0x1A, (int32_t)e) = 1;
        int widget_slot = (int16_t)MEM_H(0x34, (int32_t)e);
        if (widget_slot >= 0 && widget_slot < 400) MEM_B(0x16, (int32_t)widget_at(widget_slot)) = 1;
    }
}

// After the widget draw loop: the ammo on each view's weapon and the kills on its coin; the results once the time is
// up; the Weapons cheat's bar.
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
    if (!ready || mode == Mode::none) return;
    // Not over the pause menu's screens (0x8002305C is set while one is open).
    if (MEM_B(0, (int32_t)0x8002305C) != 0) return;
    int players = mode == Mode::arena ? hud_players : local_players(rdram);
    for (int p = 0; p < players; p++) {
        int car = player_car(rdram, p);
        if (car < 0 || car >= max_cars) continue;
        const Fighter& f = fighters[car];
        View v = view_of(rdram, p, players);
        Layout l = mode == Mode::arena ? layout_of(v) : cheat_layout(v, p);
        char text[16];
        // Whole texels in a quadrant (the digits are 10 tall): a smaller size loses their top row.
        float digits = l.small ? 10.0f : 12.0f;
        if (mode == Mode::arena && !over) {
            snprintf(text, sizeof(text), "%d", std::min(f.kills, 999));
            rush2::hud::draw_number(rdram, text, l.coin_x, l.coin_y, digits, l.coin_anchor);
        }
        else if (!is_wrecked(rdram, car)) {
            draw_cheat_bar(rdram, l, f.health / max_health);
        }
        if (mode == Mode::arena && f.weapon < 8 && f.ammo > 0 && !is_wrecked(rdram, car)) {
            snprintf(text, sizeof(text), "%d", f.ammo);
            rush2::hud::draw_number(rdram, text, l.weapon_x, l.weapon_y, digits, l.weapon_anchor);
        }
    }
    if (mode == Mode::arena && over) draw_results(rdram, ctx);
}

// func_8007C27C entry ($a1 = the camera position of the view about to be drawn): places that view's HUD models, as
// 2049 shows each of its HUD models in one view (func_8008B0D8 with the view's bit), and hides the shadows of the
// invisible cars in the other players' views.
extern "C" void rush2_battle_view(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ battle_mutex };
    if (!ready || mode == Mode::none || MEM_W(0, (int32_t)game_state) < 3) return;
    int view = rush2::views::view_of_camera(rdram, (uint32_t)ctx->r5);
    if (view < 0 || view >= 4) return;
    update_mounts(rdram);
    int players = local_players(rdram);
    for (int i = 0; i < max_cars; i++) {
        Fighter& f = fighters[i];
        bool invisible = f.present && f.invisible > 0.0f && !is_wrecked(rdram, i);
        if (!invisible && !f.shadow_hidden) continue;
        uint32_t shadow = (uint32_t)MEM_W(0, (int32_t)(state_at(i) + state_shadow));
        if (shadow < 0x80000000u) continue;
        bool own = view < players && player_car(rdram, view) == i;
        uint16_t flags = MEM_HU(2, (int32_t)shadow);
        if (invisible && !own) {
            if ((flags & 0x8000) == 0) {
                MEM_H(2, (int32_t)shadow) = (int16_t)(flags | 0x8000);
                f.shadow_hidden = true;
            }
        }
        else if (f.shadow_hidden) {
            MEM_H(2, (int32_t)shadow) = (int16_t)(flags & ~0x8000);
            f.shadow_hidden = false;
        }
    }
    int p = view;
    int weapon_slot = hud_weapon_slot_first + p, powerup_slot = hud_powerup_slot_first + p, coin_slot = hud_coin_slot_first + p;
    int car = p < players ? player_car(rdram, p) : -1;
    if (car < 0 || car >= max_cars) {
        hide_slot(rdram, weapon_slot);
        hide_slot(rdram, powerup_slot);
        hide_slot(rdram, coin_slot);
        return;
    }
    const Fighter& f = fighters[car];
    View v = view_of(rdram, p, players);
    Layout l = mode == Mode::arena ? layout_of(v) : cheat_layout(v, p);
    float pos[3], axes[9], m[9];
    float gain = model_gain(rdram, p, v) * hud_distance;
    // The weapon held, turning about the camera's up axis.
    if (mode == Mode::arena && f.weapon < 8 && !is_wrecked(rdram, car)) {
        view_point(rdram, p, v, anchored_x(l.weapon_x, l.weapon_anchor), l.weapon_y, hud_distance, pos, axes);
        memcpy(m, axes, sizeof(m));
        yaw(m, hud_spin);
        scale_matrix(m, (f.weapon == sonic ? sonic_scale : weapon_scale) * gain);
        color_slot(weapon_slot, plain_color);
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
        color_slot(powerup_slot, plain_color);
        place_slot(rdram, powerup_slot, f.invisible > 0.0f ? "WEPICON_INVSG1" : "WEPICON_SHLDG1", m, pos);
    }
    else {
        hide_slot(rdram, powerup_slot);
    }
    // The player's coin, face on (2049 tips its model a quarter turn back), in its team's color.
    if (mode == Mode::arena && !over) {
        view_point(rdram, p, v, anchored_x(l.coin_x, l.coin_anchor), l.coin_y, hud_distance, pos, axes);
        memcpy(m, axes, sizeof(m));
        pitch(m, -1.5707964f);
        scale_matrix(m, coin_scale * gain);
        color_slot(coin_slot, plain_color);
        place_slot(rdram, coin_slot, coin_models[rush2::battle::team_of(p)], m, pos);
    }
    else {
        hide_slot(rdram, coin_slot);
    }
}

// func_800B9CC0 (the stunt score panel's visibility callback) at 0x800B9D2C: $a1 = whether to hide it. A battle arena is
// played in stunt mode but has no stunt score.
extern "C" void rush2_battle_hide_stunt_panel(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::battle_race(rdram)) ctx->r5 = 1;
}
