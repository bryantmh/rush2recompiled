// California Speed's own physics on its tracks: the Games tab's CS Physics option, a test of the other way round from
// the CS Road Hold port (docs/calspeed_research.md#cs-physics-california-speeds-world).
//
// California Speed's car code is Rush 2's, function for function (its car struct is Rush 2's with fields inserted:
// +0x24 from +0x224, +0x58 from +0x2CC, +0x5C/+0x64 further on), so the port runs Rush 2's own functions and changes
// only where California Speed's differ (found with tools/calspeed/physdiff.py and tools/rush2049/decomp.py cs):
// - ground contact (func_800A58A4): its wall collision in place of Rush 2's body probe (calspeed_walls.cpp), a missed
//   wheel keeps its compression while three hit, the per-wheel road-hold factor, a wheel's material 6 (the subway
//   tunnel loop) kept while it is off the road;
// - wheel forces (func_8008F268, func_800B8CE8, func_800B8F68): the tire's load capped at 1100 lb, its two forces
//   scaled by the descriptor's two grip factors on every wheel (Rush 2: the rear lateral one only), the drag of dirt
//   and grass under the rear wheels -1 x speed (Rush 2 -10 and -15);
// - force sum (func_8008F7E4): the rear wheels' drive forces always averaged, the road hold (weight x the factor per
//   wheel, along the car's down axis, world down once a wheel is a foot out), and the hill push (+1200 lb forward while
//   climbing steeply under throttle below 25 mph off the walls);
// - torque (func_8008FC1C): the lifted-wheel torque;
// - velocities (func_8008FFFC): a creep to 6.25 ft/s off the brake, 400 ft/s cap (its forward-only clamp left out:
//   the cars reverse as Rush 2's do);
// - orientation (func_80090540, func_800901E0): pitch and roll rates zeroed every step, the car lifted so its wheels'
//   mean compression is at most 0.35 ft (Rush 2: any one past 0.5) and a human's car pitched level with its wheels
//   (California Speed's own cars only: converted cars keep Rush 2's lift, see orient_end), the body points pushed out on the low side once it rolls past 0.3, and on the tunnel loop (both rear wheels on
//   material 6) the car stood square to the road under it;
// - gearbox (func_8008E5C4): shifts held back by road speed.
// The AI driver, the steering terms, the drivetrain, the tire model and the wheel probe are the same code in both. The
// computer cars drive the lanes' true speeds. California Speed has no catch-up (its speed factor +0x858 is 1.0 all
// race); here the Drone Cheating option decides, as on every other track.
// Every car runs California Speed's values (native): a California Speed car its own (descriptor, tire curves and
// per-car tables, unscaled), a Rush 2, 2049 or mod car its own mapped back from Rush 2's roster
// (rush2::calspeed::native_values), so all cars drive alike whoever drives them. Converted (the values kept, springs and
// anti-roll x 2.28, dampers x sqrt(2.28), tire load / 2.28) is left only as the fallback when the California Speed cars
// aren't loaded; on a player's car it fishtailed on hills and landings. Rush 2's wrecks, respawns and car-to-car
// collision stay.
// Roles are set at car init, so the option takes effect from the next race.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "rush2_hooks.h"
#include "calspeed.h"
#include "online_sync.h"
#include "car_types.h"
#include "surface_fx.h"

namespace {
    namespace ct = rush2::car_types;
    using rush2::calspeed::physics::Mode;

    std::atomic<int> mode{ int(Mode::Rush2World) };

    constexpr uint32_t physics_cars = 0x800F5470, physics_car_size = 0x81C;
    constexpr int max_cars = 8;

    // Rush 2's car struct (California Speed's offsets in the comments where they differ).
    constexpr uint32_t car_desc = 0x0;            // descriptor pointer
    constexpr uint32_t car_force = 0x4;           // f32[3], the car's frame
    constexpr uint32_t car_torque = 0x10;         // f32[3]
    constexpr uint32_t car_accel = 0x1C;          // f32[3]
    constexpr uint32_t car_alpha = 0x28;          // f32[3]
    constexpr uint32_t car_velocity = 0x34;       // f32[3]: x sideways, y up, z forward
    constexpr uint32_t car_angular = 0x40;        // f32[3]: pitch, yaw, roll rates
    constexpr uint32_t car_wheel_force = 0x58;    // f32[4][3], each wheel's force in the car's frame
    constexpr uint32_t car_body_offset = 0xE8;    // f32[4][3], the body points in the car's frame
    constexpr uint32_t car_gravity = 0x200;       // f32[3]: gravity in the car's frame (car_force_sum)
    constexpr uint32_t car_position = 0x224;      // f32[3] (CS +0x248)
    constexpr uint32_t car_wheel_point = 0x230;   // f32[4][3] world (CS +0x254)
    constexpr uint32_t car_body_point = 0x260;    // f32[4][3] world (CS +0x284)
    constexpr uint32_t car_matrix = 0x2CC;        // f32[3][3] world -> car, rows right, up, forward (CS +0x324)
    constexpr uint32_t car_wheel_poly = 0x2F0;    // f32[4][9] world -> each wheel's road polygon, 0x24 apart (CS +0x348)
    constexpr uint32_t car_throttle = 0x3B4;      // f32 (CS +0x418)
    constexpr uint32_t car_brake = 0x3B8;         // f32 (CS +0x41C)
    constexpr uint32_t car_speed = 0x3D4;         // f32 |v| (CS +0x438)
    constexpr uint32_t car_gear = 0x3DE;          // s16 (CS +0x442)
    constexpr uint32_t car_gear_request = 0x3E0;  // s16 (CS +0x444)
    constexpr uint32_t car_engine_speed = 0x3F8;  // f32 (CS +0x45C)
    constexpr uint32_t car_wheel_spin = 0x410;    // f32: the driven wheels' mean spin (CS +0x474)
    constexpr uint32_t car_tires = 0x424;         // tire states, 0x5C each: +0 radius, +0x48 spin (CS +0x488)
    constexpr uint32_t tire_size = 0x5C;
    constexpr uint32_t car_offroad = 0x5A8;       // f32 the TIRES off-road factor (CS +0x60C)
    constexpr uint32_t car_weight = 0x5AC;        // f32 lb (CS +0x610)
    constexpr uint32_t car_mass = 0x5B0;          // f32 slugs (CS +0x614)
    constexpr uint32_t car_compression = 0x5BC;   // f32[4] (CS +0x620)
    constexpr uint32_t car_rest = 0x5CC;          // f32[4]: each wheel's suspension offset (CS +0x630)
    constexpr uint32_t car_travel = 0x5DC;        // f32[4] = -compression (CS +0x640)
    constexpr uint32_t car_surface = 0x5FC;       // s32[4] surface type, 8 = air (CS +0x660, 10 = air)
    constexpr uint32_t car_material = 0x60C;      // u16[4] material (CS +0x670)
    constexpr uint32_t car_clock = 0x630;         // f32: the car's own time (CS +0x68C)
    constexpr uint32_t car_dt = 0x638;            // f32 (CS +0x694)
    constexpr uint32_t car_shift_time = 0x65C;    // f32: no shift before this (CS +0x6B4)
    constexpr uint32_t car_human = 0x7E8;         // s8: 2 = a player's car
    constexpr uint32_t car_type = 0x7EA;          // u8
    constexpr uint32_t car_flags = 0x7F4;         // s32: bit 0x10 Rush 2's no-drag mode
    // The descriptor (Rush 2's layout).
    constexpr uint32_t desc_springs = 0x18;       // f32[4] springs, then +0x28 anti-roll, +0x2C
    constexpr uint32_t desc_dampers = 0x30;       // f32[4] rebound, then [4] bump
    constexpr uint32_t desc_drag = 0x60;          // f32 drag, +0x64 rolling resistance
    constexpr uint32_t desc_wheels = 0x7C;        // f32[4][3] wheel positions
    constexpr uint32_t desc_final_drive = 0xB0;   // f32
    constexpr uint32_t desc_gears = 0xCC;         // f32[5] neutral, 1-4
    constexpr uint32_t desc_top_gear = 0xE0;      // s16
    constexpr uint32_t desc_shift_up = 0xE4, desc_shift_down = 0xE8;

    constexpr uint32_t race_cars_slots = 0x801124A0, slot_size = 0x354;   // car slots: +0x344 wrecked
    constexpr uint32_t gravity_sign = 0x8010D388;                          // s8, the drag's slope term (CS 0x8014156F)

    // The heavier world a converted car is built for: California Speed's 1 + 4 x 0.32 g on the road.
    constexpr float world_g = 1.0f + 4 * 0.32f;

    enum class Role : uint8_t { Off, Native, Converted };
    struct CarState {
        Role role = Role::Off;
        float grip_lateral = 1.0f, grip_long = 1.0f;   // California Speed's descriptor +0x6C, +0x70 (natives)
        float hold[4] = { 0.32f, 0.32f, 0.32f, 0.32f };// the road hold's factor per wheel (CS +0x314)
        uint16_t probe_material[4] = {};               // each wheel's material as California Speed keeps it (+0x670):
                                                       // the track's, 10 off the road unless it was 6 (the tunnel loop)
        float normal[3] = { 0.0f, 1.0f, 0.0f };        // the last hit wheel's road normal (CS +0x3D8)
        float rest[4] = {};                            // +0x5CC before the orientation step
        int16_t mph4 = 0;                              // the rear wheels' road speed in mph x 4 (CS +0x7C6)
        bool wall[4] = {};                             // a wall hit a body point this step (CS +0x862)
    };
    CarState cars[max_cars];

    // The per-type tables a native car's init reads, with California Speed's values put in for the init.
    struct Swap {
        uint32_t at = 0;
        int32_t old = 0;
    };
    Swap swaps[8];
    int swap_count = 0;

    constexpr uint32_t native_descs = 0x80225000;   // per race car descriptor copies (src/rush2049/car2049.cpp)
    constexpr uint32_t native_curves = 0x80225C00;  // 8 x two tire curves (6 floats each)

    float f32(uint8_t* rdram, uint32_t at) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
    void put_f32(uint8_t* rdram, uint32_t at, float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        MEM_W(0, (int32_t)at) = (int32_t)bits;
    }
    uint32_t fbits(float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        return bits;
    }
    void get3(uint8_t* rdram, uint32_t at, float v[3]) {
        for (int k = 0; k < 3; k++) v[k] = f32(rdram, at + 4 * k);
    }
    void put3(uint8_t* rdram, uint32_t at, const float v[3]) {
        for (int k = 0; k < 3; k++) put_f32(rdram, at + 4 * k, v[k]);
    }
    // out = M v (world -> car) and M^T v (car -> world), M row-major at `m`.
    void to_car(uint8_t* rdram, uint32_t m, const float v[3], float out[3]) {
        for (int r = 0; r < 3; r++) {
            out[r] = f32(rdram, m + 12 * r) * v[0] + f32(rdram, m + 12 * r + 4) * v[1] + f32(rdram, m + 12 * r + 8) * v[2];
        }
    }
    void to_world(uint8_t* rdram, uint32_t m, const float v[3], float out[3]) {
        for (int c = 0; c < 3; c++) {
            out[c] = f32(rdram, m + 4 * c) * v[0] + f32(rdram, m + 12 + 4 * c) * v[1] + f32(rdram, m + 24 + 4 * c) * v[2];
        }
    }

    int slot_of(uint32_t car) {
        if (car < physics_cars || (car - physics_cars) % physics_car_size != 0) return -1;
        uint32_t i = (car - physics_cars) / physics_car_size;
        return i < uint32_t(max_cars) ? int(i) : -1;
    }

    // The car's role in this race, Off unless California Speed's physics run on this track.
    Role role(uint8_t* rdram, uint32_t car) {
        int s = slot_of(car);
        if (s < 0 || cars[s].role == Role::Off || !rush2::calspeed::racing(rdram)) return Role::Off;
        return cars[s].role;
    }

    // Test aid, RUSH2_CSPHYS_SKIP=<letters>: California Speed's rules left out for converted cars, to find which one
    // a behavior comes from: r the pitch and roll rates' zeroing (p the pitch rate's alone: on a player's car its
    // pitch momentum flipped the car on San Francisco's landings, 3 times in 40 s against none), a the rear drive forces' averaging, h the road hold,
    // u the hill push, t the lifted-wheel torque, d the drag's drive term, l the lift by the wheels' mean offset past
    // 0.35 ft (orient_end). With * in the letters they apply to every car, native ones too.
    bool skip(uint8_t* rdram, uint32_t car, char rule) {
        static const char* env = getenv("RUSH2_CSPHYS_SKIP");
        return env != nullptr && strchr(env, rule) != nullptr &&
               (role(rdram, car) == Role::Converted || strchr(env, '*') != nullptr);
    }

    // Test aid, RUSH2_CSPHYS_SLIDES=<file>: a player's car's physics steps kept for the last 2 s, written to
    // the file with the next second whenever the car starts to slide (its velocity more than 15 degrees off its heading
    // above 40 ft/s, after under 8): steering, throttle, brake, yaw rate, slip, and per wheel the compression, surface
    // type, material, road polygon (+0x594), the tire's load and its forces along and across, and the car's position
    // (slide_step, tire_load, tire_grip).
    struct SlideStep {
        float t, speed, slip, yaw, steer, throttle, brake, pos[3];
        float comp[4], load[4], along[4], across[4];
        int surface[4];
        int material[4];
        int poly[4];
    };
    constexpr int slide_keep = 240, slide_after = 120;
    struct SlideLog {
        FILE* file = nullptr;
        SlideStep ring[slide_keep] = {};
        SlideStep cur = {};
        int count = 0, after = 0, events = 0;
        bool sliding = false;
    };
    SlideLog* slide_log(uint8_t* rdram, uint32_t car) {
        static const char* path = getenv("RUSH2_CSPHYS_SLIDES");
        static SlideLog log;
        if (path == nullptr || slot_of(car) < 0 || role(rdram, car) == Role::Off ||
            (int8_t)MEM_B(0, (int32_t)(car + car_human)) != 2) {
            return nullptr;
        }
        if (log.file == nullptr) {
            log.file = fopen(path, "a");
            if (log.file == nullptr) return nullptr;
            fprintf(log.file, "# t speed slip(deg, + = sliding right) yaw steer thr brk | per wheel FL FR RL RR: "
                              "comp surface/material load along across poly | x y z\n");
        }
        return &log;
    }
    int tire_wheel(uint32_t car, uint32_t tire) {
        constexpr uint32_t car_tires_at = 0x424, tire_bytes = 0x5C;
        int i = int((tire - car - car_tires_at) / tire_bytes);
        return i >= 0 && i < 4 ? i : -1;
    }
    void slide_write(SlideLog& log, const SlideStep& s) {
        fprintf(log.file, "%.3f %.0f %+.1f %+.2f %+.2f %.2f %.2f |", s.t, s.speed, s.slip, s.yaw, s.steer, s.throttle,
                s.brake);
        for (int i = 0; i < 4; i++) {
            fprintf(log.file, " %.2f %d/%d %.0f %.0f %.0f %d", s.comp[i], s.surface[i], s.material[i], s.load[i],
                    s.along[i], s.across[i], s.poly[i]);
        }
        fprintf(log.file, " | %.1f %.1f %.1f", s.pos[0], s.pos[1], s.pos[2]);
        fputc('\n', log.file);
    }

    void restore_swaps(uint8_t* rdram) {
        for (int i = swap_count - 1; i >= 0; i--) MEM_W(0, (int32_t)swaps[i].at) = swaps[i].old;
        swap_count = 0;
    }
    void swap_word(uint8_t* rdram, uint32_t at, uint32_t value) {
        if (swap_count >= int(sizeof(swaps) / sizeof(swaps[0]))) return;
        swaps[swap_count++] = { at, MEM_W(0, (int32_t)at) };
        MEM_W(0, (int32_t)at) = (int32_t)value;
    }
    void swap_byte(uint8_t* rdram, uint32_t at, int value) {
        uint32_t word = at & ~3u;
        if (swap_count >= int(sizeof(swaps) / sizeof(swaps[0]))) return;
        swaps[swap_count++] = { word, MEM_W(0, (int32_t)word) };
        MEM_B(0, (int32_t)at) = (int8_t)value;
    }
}

void rush2::calspeed::physics::set_mode(Mode m) {
    mode = int(m);
}

Mode rush2::calspeed::physics::get_mode() {
    return Mode(mode.load());
}

bool rush2::calspeed::physics::active(uint8_t* rdram, uint32_t car) {
    return role(rdram, car) != Role::Off;
}

bool rush2::calspeed::physics::native(uint8_t* rdram, uint32_t car) {
    return role(rdram, car) == Role::Native;
}

bool rush2::calspeed::physics::converted(uint8_t* rdram, uint32_t car) {
    return role(rdram, car) == Role::Converted;
}

bool rush2::calspeed::physics::race_mode() {
    return mode.load() == int(Mode::CalSpeed) || mode.load() == int(Mode::Rushified);
}

// A converted car's descriptor copy (rush2_car49_setup_desc): its springs and anti-roll x world_g, its dampers x the
// square root, so under California Speed's 2.28 g on the road it sits and rolls as it does at Rush 2's 1 g, with Rush
// 2's damping ratio.
void rush2::calspeed::physics::convert_descriptor(uint8_t* rdram, uint32_t desc) {
    for (uint32_t o = desc_springs; o < desc_springs + 0x18; o += 4) put_f32(rdram, desc + o, f32(rdram, desc + o) * world_g);
    float damp = std::sqrt(world_g);
    for (uint32_t o = desc_dampers; o < desc_dampers + 0x20; o += 4) put_f32(rdram, desc + o, f32(rdram, desc + o) * damp);
}

// car_init_setup_8008DBA0's entry ($a0 = the car, its type at +0x7EA and driver at +0x7E8 already set): the car's role,
// and its California Speed values put into the tables the init reads (restored at
// its end, rush2_csphys_init_end): its descriptor (built unscaled into the car's descriptor copy, with its tire curves
// as California Speed has them), steering and yaw bases, preload, mass, inertia and TIRES (4 for every car).
extern "C" void rush2_csphys_init_begin(uint8_t* rdram, recomp_context* ctx) {
    restore_swaps(rdram);
    uint32_t car = (uint32_t)ctx->r4;
    int s = slot_of(car);
    if (s < 0) return;
    CarState& st = cars[s];
    st = CarState{};
    if (!rush2::calspeed::physics::race_mode() || !rush2::calspeed::racing(rdram)) return;
    int type = MEM_BU(0, (int32_t)(car + car_type));
    st.role = Role::Converted;
    // Every car runs native, whoever drives it and whatever game it comes from: a California Speed car on its own
    // values, any other on its values mapped back from Rush 2's roster (native_values). Converted (Rush 2's tires on
    // springs x world_g), a player's car fishtailed on every hill and landing of San Francisco; native it doesn't
    // (user, 2026-10-21/22).
    rush2::calspeed::NativeCar v;
    uint32_t desc = native_descs + uint32_t(s) * ct::desc_size, curves = native_curves + uint32_t(s) * 0x30;
    bool got = ct::is_calspeed(type) ? rush2::calspeed::native_car(rdram, type - ct::first_calspeed, desc, curves, v)
                                     : rush2::calspeed::native_values(rdram, type, desc, curves, v);
    if (!got) return;
    st.role = Role::Native;
    st.grip_lateral = v.grip_lateral;
    st.grip_long = v.grip_long;
    int index = MEM_BU(0, (int32_t)ct::entry(ct::desc_index, 0, type));
    // CS Physics Partial: a player's native car keeps these of its Rush 2 values (the type's own tables and descriptor:
    // a Rush 2 car's own, a California Speed car's rebalanced onto Rush 2's roster) in place of California Speed's;
    // Partial keeps "yepmidct", all but the slide term, which with the faster levelling (orient_end) drove without a
    // fishtail where California Speed's values spun (user, 2026-10-22). RUSH2_CSPHYS_RUSH=<letters> overrides the set
    // in either mode, a test aid:
    // y the yaw gain (the steering term, func_80069E74 / CS func_8008F0B4: -steering x speed x yaw gain, +0x5A4 / CS
    //   +0x608, the turn-in Rush 2's steering has and California Speed's lacks),
    // s the steer base (the slide term +0x5A0: California Speed's 9800 against Rush 2's 0-1200),
    // c the tire curves (and no grip factors), d the springs, anti-roll and dampers, e the torque scales and gears,
    // p the preload, m the mass, i the inertia, t the TIRES byte (California Speed's 4 otherwise).
    // The stick's curve is close in both (CS 80081C7C: squared with its sign, small inputs x 100 / speed above
    // 100 ft/s; Rush 2 func_80076694: cubed).
    static const char* rush_env = getenv("RUSH2_CSPHYS_RUSH");
    const char* keep = rush_env != nullptr ? rush_env : mode.load() == int(Mode::Rushified) ? "yepmidct" : "";
    bool human = (int8_t)MEM_B(0, (int32_t)(car + car_human)) == 2;
    auto rush = [&](char k) { return human && strchr(keep, k) != nullptr; };
    uint32_t own = (uint32_t)MEM_W(0, (int32_t)(ct::desc_ptrs + uint32_t(index) * 4));
    auto keep_desc = [&](uint32_t lo, uint32_t hi) {
        for (uint32_t o = lo; o < hi; o += 4) MEM_W(0, (int32_t)(desc + o)) = MEM_W(0, (int32_t)(own + o));
    };
    if (rush('c')) {
        keep_desc(0x68, 0x78);
        st.grip_lateral = st.grip_long = 1.0f;
    }
    if (rush('d')) keep_desc(desc_springs, 0x50);
    if (rush('e')) {
        keep_desc(0xB8, 0xC8);
        keep_desc(0xD0, 0xE0);
    }
    swap_word(rdram, ct::desc_ptrs + uint32_t(index) * 4, desc);
    if (!rush('s')) swap_word(rdram, ct::entry(ct::steer_base, 0, type), fbits(v.steer));
    if (!rush('y')) swap_word(rdram, ct::entry(ct::yaw_damping, 0, type), fbits(v.yaw));
    if (!rush('p')) swap_word(rdram, ct::entry(ct::preload, 0, type), fbits(v.preload));
    if (!rush('m')) swap_word(rdram, ct::entry(ct::mass, 0, type), fbits(v.mass));
    if (!rush('i')) swap_word(rdram, ct::entry(ct::inertia, 0, type), fbits(v.inertia));
    if (!rush('t')) swap_byte(rdram, ct::entry(ct::tires, 0, type), v.tires);
    if (human && keep[0] != '\0') printf("[csphys] player car type %d keeps Rush 2's: %s\n", type, keep);
}

// car_init_setup_8008DBA0 at 0x8008E01C, its end ($s2 = the car): the tables as they were, and the car's crash
// threshold (+0x644: Rush 2's func_80071724 wrecks a car whose total force +0x4 is above it; California Speed has no such
// check) x world_g, the world's own scale: every car is held to the road with 2.28 g (4.6 g in the air), so the forces
// of a landing or a hit are that much larger than Rush 2's for the same event. At 390000 lb every computer car wrecked
// landing California's crest at 240 ft/s (400000-585000 lb, upright, on its lane), a jump California Speed's lanes
// take at full speed and its cars land.
extern "C" void rush2_csphys_init_end(uint8_t* rdram, recomp_context* ctx) {
    restore_swaps(rdram);
    uint32_t car = (uint32_t)ctx->r18;
    int s = slot_of(car);
    if (s < 0 || cars[s].role == Role::Off) return;
    put_f32(rdram, car + 0x644, f32(rdram, car + 0x644) * world_g);
}

// car_ground_contact_800703EC's entry: this step's wall hits cleared.
void rush2::calspeed::physics::ground_contact_begin(uint8_t* rdram, uint32_t car) {
    int s = slot_of(car);
    if (s < 0) return;
    for (bool& w : cars[s].wall) w = false;
    // RUSH2_CSPHYS_WRECKS: each car's wreck (its race slot's wreck state +0x342 going up) with the crash check's inputs
    // (func_80071724, its flag +0x648): the total force it took (+0x654) against the threshold (+0x644), the speed
    // (past 400 wrecks), the yaw rate (past 10 wrecks) and the body points' forces (+0xB8, y of each: two below -1000
    // with the car tipped past 45 degrees under 100 ft/s wreck). A wreck with the flag clear is a reset
    // (race_stuck_wrongway_update_8008CDA4) or a car_wreck_trigger_8006E1A8 call (rush2_csphys_wreck_log).
    static const bool log_wrecks = getenv("RUSH2_CSPHYS_WRECKS") != nullptr;
    static bool was[max_cars] = {};
    bool crash = MEM_B(0, (int32_t)(race_cars_slots + uint32_t(s) * slot_size + 0x342)) != 0;   // the wreck state
    if (log_wrecks && crash && !was[s]) {
        printf("[cswreck] car %d t %.2f flag %d at %.0f %.0f %.0f force %.0f / %.0f speed %.0f yaw %.2f up %.2f body "
               "%.0f %.0f %.0f %.0f\n", s, f32(rdram, car + car_clock), MEM_B(0, (int32_t)(car + 0x648)),
               f32(rdram, car + car_position),
               f32(rdram, car + car_position + 4), f32(rdram, car + car_position + 8), f32(rdram, car + 0x654),
               f32(rdram, car + 0x644), f32(rdram, car + car_speed), f32(rdram, car + car_angular + 4),
               f32(rdram, car + car_matrix + 16), f32(rdram, car + 0xBC), f32(rdram, car + 0xC8),
               f32(rdram, car + 0xD4), f32(rdram, car + 0xE0));
        // The drone's lane state (drone_driver_80074990: +0 along, +4 across, +0xC the segment, +0x20 the time of its
        // last step to the next point, +0x24 the point) and +0x810, its lost-the-lane flag.
        uint32_t lane = race_cars_slots + uint32_t(s) * slot_size + 0x30C;
        printf("[cswreck]   lane along %.1f across %.1f segment %.1f stepped %.2f s ago point %d lost %d\n",
               f32(rdram, lane), f32(rdram, lane + 4), f32(rdram, lane + 0xC),
               f32(rdram, 0x80117488) - f32(rdram, lane + 0x20), (int16_t)MEM_H(0, (int32_t)(lane + 0x24)),
               (int16_t)MEM_H(0, (int32_t)(car + 0x810)));
    }
    was[s] = crash;
    // Test aid, RUSH2_CS_PLACE="x,y,z,dx,dz,speed,at": the player's car (physics car 0) put at (x, y, z) heading
    // (dx, dz) level at speed ft/s once its clock passes `at`, then its position, attitude, velocity and wheels logged
    // ([csplace]) each step for 8 s (docs/calspeed_research.md#wall-climbing-a-missed-wheel-reads-as-fully-compressed-fixed-2026-10-27).
    static const char* place = getenv("RUSH2_CS_PLACE");
    static bool placed = false;
    static int logged = 0;
    float p[7] = {};
    if (place == nullptr || s != 0 || logged >= 480 ||
        sscanf(place, "%f,%f,%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6]) != 7) {
        return;
    }
    if (!placed) {
        if (f32(rdram, car + car_clock) <= p[6]) return;
        placed = true;
        float len = std::sqrt(p[3] * p[3] + p[4] * p[4]), dx = p[3] / len, dz = p[4] / len;
        float m[9] = { dz, 0.0f, -dx, 0.0f, 1.0f, 0.0f, dx, 0.0f, dz };
        for (int k = 0; k < 9; k++) put_f32(rdram, car + car_matrix + 4 * k, m[k]);
        float pos[3] = { p[0], p[1], p[2] }, v[3] = { 0.0f, 0.0f, p[5] }, zero[3] = {};
        put3(rdram, car + car_position, pos);
        put3(rdram, car + 0x7B0, pos);
        put3(rdram, car + car_velocity, v);
        put3(rdram, car + car_angular, zero);
        return;
    }
    logged++;
    printf("[csplace] t %.3f pos %.1f %.1f %.1f up %.2f %.2f %.2f v %.1f %.1f %.1f comp %.2f %.2f %.2f %.2f surf %d %d %d "
           "%d hold %.2f %.2f %.2f %.2f\n", f32(rdram, car + car_clock), f32(rdram, car + car_position),
           f32(rdram, car + car_position + 4), f32(rdram, car + car_position + 8), f32(rdram, car + car_matrix + 12),
           f32(rdram, car + car_matrix + 16), f32(rdram, car + car_matrix + 20), f32(rdram, car + car_velocity),
           f32(rdram, car + car_velocity + 4), f32(rdram, car + car_velocity + 8), f32(rdram, car + car_compression),
           f32(rdram, car + car_compression + 4), f32(rdram, car + car_compression + 8),
           f32(rdram, car + car_compression + 12), (int)MEM_W(0, (int32_t)(car + car_surface)),
           (int)MEM_W(0, (int32_t)(car + car_surface + 4)), (int)MEM_W(0, (int32_t)(car + car_surface + 8)),
           (int)MEM_W(0, (int32_t)(car + car_surface + 12)), cars[s].hold[0], cars[s].hold[1], cars[s].hold[2],
           cars[s].hold[3]);
    fflush(stdout);
}

// A wall hit on body point `point` (calspeed_walls.cpp, func_800B0A04 records wall types 4-9 at +0x862).
void rush2::calspeed::physics::wall_hit(uint32_t car, int point, int type) {
    int s = slot_of(car);
    if (s < 0 || point < 0 || point > 3 || type < 4 || type > 9) return;
    cars[s].wall[point] = true;
}

// car_ground_contact_800703EC at 0x8007057C, after wheel $s3's probe ($s1 = the car, $fp its road polygon matrix):
// the track's material it stands on (Rush 2's +0x60C holds the material its effects treat it as, src/surface_fx.cpp; a
// miss keeps the last, as California Speed's probe leaves +0x670 alone) and, for a hit, its road normal (California
// Speed keeps the last hit wheel's in +0x3D8).
extern "C" void rush2_csphys_probe(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r17;
    if (role(rdram, car) == Role::Off) return;
    CarState& st = cars[slot_of(car)];
    int i = (int)ctx->r19;
    if (i < 0 || i > 3) return;
    constexpr int32_t miss_matrix = (int32_t)0x800CA27C;   // the matrix a missed wheel gets
    uint32_t m = car + car_wheel_poly + 0x24 * uint32_t(i);
    bool miss = true;
    for (int k = 0; miss && k < 9; k++) miss = MEM_W(0, (int32_t)(m + 4 * k)) == MEM_W(4 * k, miss_matrix);
    if (miss) return;
    int source = rush2::surface_fx::source_material(car, i);
    st.probe_material[i] = uint16_t(source >= 0 ? source : MEM_HU(0, (int32_t)(car + car_material + 2 * i)));
    float n[3] = { f32(rdram, m + 12), f32(rdram, m + 16), f32(rdram, m + 20) };
    float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0.00005f) {
        for (int k = 0; k < 3; k++) st.normal[k] = n[k] / len;
    }
}

// car_ground_contact_800703EC at 0x80070680, its wheels done (after the missed-wheel rule, calspeed_walls.cpp): the
// road-hold factor per wheel from its compression (func_800A58A4): off the road 0.5, 1.5 from 0.5 to 0.75 ft out, 0.9
// further; on it 0.32, past 0.5 ft of compression 0.82 - compression, first raising wheel (i + 2) % 4's to at least 0.2
// (wheels 2 and 3 then replace what 0 and 1 gave them).
void rush2::calspeed::physics::ground_contact_end(uint8_t* rdram, uint32_t car) {
    if (role(rdram, car) == Role::Off) return;
    CarState& st = cars[slot_of(car)];
    for (int i = 0; i < 4; i++) {
        // A wheel off the road (Rush 2's type 8) gets California Speed's air material 10 unless it was on 6.
        if (MEM_W(0, (int32_t)(car + car_surface + 4 * i)) == 8 && st.probe_material[i] != 6) st.probe_material[i] = 10;
    }
    for (int i = 0; i < 4; i++) {
        float c = f32(rdram, car + car_compression + 4 * i);
        if (c < 0.0f) {
            st.hold[i] = c < -0.75f ? 0.9f : c < -0.5f ? 1.5f : 0.5f;
        }
        else if (c > 0.5f) {
            float& other = st.hold[(i + 2) % 4];
            if (other < 0.2f) other += 0.2f;
            st.hold[i] = (float)((double)(0.5f - c) + 0.32);
        }
        else {
            st.hold[i] = 0.32f;
        }
    }
}

// car_physics_forces_800706D0 at 0x800706EC, after car_wheel_forces_drag ($s0 = the car): the drag's drive term as
// California Speed has it. Rush 2's adds k x v to the force +0x54 with k = (1 - off-road) x -10 when both rear wheels
// are on surface 1, x -15 on surface 2 (its constant when the track's 0x8010D3A6 is 2), else -0.7 x off-road;
// California Speed's -1, -1 and -0.7 (0x800D7350-58), with no track byte. v is the drag's speed (+0x3C less the
// slope's part, Rush 2's own unless the car is wrecked or in its no-drag mode).
void rush2::calspeed::physics::wheel_forces_end(uint8_t* rdram, uint32_t car) {
    if (role(rdram, car) == Role::Off || skip(rdram, car, 'd')) return;
    constexpr uint32_t track_byte = 0x8010D3A6;
    constexpr uint32_t k_rear1 = 0x800CF784, k_rear2 = 0x800CF788;   // Rush 2's constants for track byte 2
    int slot = MEM_H(0, (int32_t)(car + 0x7E0));
    bool wrecked = slot >= 0 && slot < max_cars && MEM_B(0, (int32_t)(race_cars_slots + slot * slot_size + 0x344)) != 0;
    float v = f32(rdram, car + car_velocity + 8);
    if (!wrecked && !(MEM_W(0, (int32_t)(car + car_flags)) & 0x10)) {
        v -= f32(rdram, car + car_matrix + 0x18) * (float)(int8_t)MEM_B(0, (int32_t)gravity_sign) * 20.0f;
    }
    int t2 = MEM_W(0, (int32_t)(car + car_surface + 8)), t3 = MEM_W(0, (int32_t)(car + car_surface + 12));
    bool track2 = (int8_t)MEM_B(0, (int32_t)track_byte) == 2;
    float offroad = f32(rdram, car + car_offroad);
    float k_r2, k_cs;
    if (t3 == 1 && t2 == 1) {
        k_r2 = (track2 ? f32(rdram, k_rear1) : 1.0f - offroad) * -10.0f;
        k_cs = (1.0f - offroad) * -1.0f;
    }
    else if (t3 == 2 && t2 == 2) {
        k_r2 = (track2 ? f32(rdram, k_rear2) : 1.0f - offroad) * -15.0f;
        k_cs = (1.0f - offroad) * -1.0f;
    }
    else {
        k_r2 = -0.7f * (track2 ? 0.0f : offroad);
        k_cs = -0.7f * offroad;
    }
    put_f32(rdram, car + car_force + 0x50, f32(rdram, car + car_force + 0x50) + (k_cs - k_r2) * v);
}

// car_wheel_force_loop_8006ABB0 at 0x8006AD4C ($a2 = the wheel's load for car_tire_model, $a0 = the car): a native car's
// capped at 1100 lb (func_800B8F68's first step), a converted car's / world_g (Rush 2's grip under California Speed's
// heavier hold). Rush 2 has just capped the load at the car's weight (+0x5AC, 0x8006AD30; the uncapped load stays at
// 0x34($sp)); a converted car's is capped at weight x world_g instead, so that after / world_g its tires top out at the
// car's weight as Rush 2's do. Capped at the weight itself they topped out at 0.44 of it a wheel (0.25 at rest), so
// every load spike of a quick swerve or a polygon edge's damper kick cost grip. Returns true when it ran.
bool rush2::calspeed::physics::tire_load(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r4;
    Role r = role(rdram, car);
    if (r == Role::Off) return false;
    uint32_t bits = (uint32_t)ctx->r6;
    float load;
    memcpy(&load, &bits, 4);
    if (r == Role::Converted) {
        float weight = f32(rdram, car + car_weight);
        if (load >= weight) load = std::min(f32(rdram, (uint32_t)ctx->r29 + 0x34), weight * world_g);
    }
    load = r == Role::Native ? std::min(load, 1100.0f) : load / world_g;
    bits = fbits(load);
    ctx->r6 = (uint64_t)(int64_t)(int32_t)bits;
    if (SlideLog* log = slide_log(rdram, car)) {
        int i = tire_wheel(car, (uint32_t)ctx->r8);
        if (i >= 0) log->cur.load[i] = load;
    }
    return true;
}

// car_wheel_force_loop_8006ABB0 at 0x8006ADFC, the tire's forces at 0x40($sp) (along, in $f18) and 0x44($sp) (across,
// in $f8) before they go to the car's frame ($a0 = the car, $t0 = car + 0x424 + 0x5C x the wheel): a native car's
// scaled by its descriptor's grip factors on every wheel (func_800B8CE8: +0x6C across, +0x70 along). Returns true when
// it ran.
bool rush2::calspeed::physics::tire_grip(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r4;
    Role r = role(rdram, car);
    if (r == Role::Off) return false;
    if (r != Role::Native) {
        if (SlideLog* log = slide_log(rdram, car)) {
            int i = tire_wheel(car, (uint32_t)ctx->r8);
            if (i >= 0) {
                log->cur.along[i] = f32(rdram, (uint32_t)ctx->r29 + 0x40);
                log->cur.across[i] = f32(rdram, (uint32_t)ctx->r29 + 0x44);
            }
        }
        return true;
    }
    const CarState& st = cars[slot_of(car)];
    uint32_t sp = (uint32_t)ctx->r29;
    float along = f32(rdram, sp + 0x40) * st.grip_long, across = f32(rdram, sp + 0x44) * st.grip_lateral;
    put_f32(rdram, sp + 0x40, along);
    put_f32(rdram, sp + 0x44, across);
    ctx->f18.fl = along;
    ctx->f8.fl = across;
    if (SlideLog* log = slide_log(rdram, car)) {
        int i = tire_wheel(car, (uint32_t)ctx->r8);
        if (i >= 0) {
            log->cur.along[i] = along;
            log->cur.across[i] = across;
        }
    }
    return true;
}

// car_physics_forces_800706D0 at 0x800706F4, after car_force_sum ($s0 = the car): California Speed's func_8008F7E4.
// The rear wheels' drive forces averaged (Rush 2 only for drive flag 1: the sum is the same, the torque from them
// isn't); the road hold, weight x each wheel's factor along the car's down axis, or world down turned into the car's
// frame once any wheel is more than 1 ft out; and the hill push: +1200 lb forward while the car's nose points up
// steeply (forward axis y > 0.55) under throttle below 25 mph with no wall on its front body points. Returns true when
// it ran (the CS Road Hold port then doesn't).
bool rush2::calspeed::physics::force_sum(uint8_t* rdram, uint32_t car) {
    if (role(rdram, car) == Role::Off) return false;
    const CarState& st = cars[slot_of(car)];
    if (!skip(rdram, car, 'a')) {
        float rear = (f32(rdram, car + car_wheel_force + 0x18 + 8) + f32(rdram, car + car_wheel_force + 0x24 + 8)) * 0.5f;
        put_f32(rdram, car + car_wheel_force + 0x18 + 8, rear);
        put_f32(rdram, car + car_wheel_force + 0x24 + 8, rear);
    }
    bool lifted = false;
    for (int i = 0; i < 4; i++) lifted |= f32(rdram, car + car_compression + 4 * i) < -1.0f;
    float weight = f32(rdram, car + car_weight), sum = 0.0f;
    for (int i = 0; i < 4; i++) sum += st.hold[i];
    if (skip(rdram, car, 'h')) sum = 0.0f;
    float push[3] = { 0.0f, -weight * sum, 0.0f };
    if (lifted) {
        float world[3] = { 0.0f, -weight * sum, 0.0f };
        to_car(rdram, car + car_matrix, world, push);
    }
    float f[3];
    get3(rdram, car + car_force, f);
    for (int k = 0; k < 3; k++) f[k] += push[k];
    if (f32(rdram, car + car_matrix + 0x1C) > 0.55f && f32(rdram, car + car_throttle) != 0.0f && (st.mph4 >> 2) < 25 &&
        !st.wall[0] && !st.wall[1]) {
        if (!skip(rdram, car, 'u')) f[2] += 1200.0f;
    }
    put3(rdram, car + car_force, f);
    return true;
}

// car_physics_forces_800706D0 at 0x80070704, after car_torque_from_forces ($s0 = the car): the lifted-wheel torque
// (func_8008FC1C), unless all four wheels are in the air: for each wheel at -0.1 ft or more out, F = gravity (+0x200)
// x -2 x (compression + 0.1) at its descriptor position with y zeroed, the torque F x r as the game takes its torques.
void rush2::calspeed::physics::torque_end(uint8_t* rdram, uint32_t car) {
    if (role(rdram, car) == Role::Off || skip(rdram, car, 't')) return;
    bool air = true;
    for (int i = 0; i < 4; i++) air &= MEM_W(0, (int32_t)(car + car_surface + 4 * i)) == 8;
    if (air) return;
    uint32_t desc = (uint32_t)MEM_W(0, (int32_t)(car + car_desc));
    float g[3], t[3];
    get3(rdram, car + car_gravity, g);
    get3(rdram, car + car_torque, t);
    for (int i = 0; i < 4; i++) {
        float c = f32(rdram, car + car_compression + 4 * i);
        if (c > -0.1f) continue;
        float k = (c + 0.1f) * -2.0f;
        float F[3] = { g[0] * k, g[1] * k, g[2] * k };
        float rx = f32(rdram, desc + desc_wheels + 12 * i), rz = f32(rdram, desc + desc_wheels + 12 * i + 8);
        t[0] += F[1] * rz - F[2] * 0.0f;
        t[1] += F[2] * rx - F[0] * rz;
        t[2] += F[0] * 0.0f - F[1] * rx;
    }
    put3(rdram, car + car_torque, t);
}

// car_integrate_velocities_800697D0's entry ($a0 = the car): a native car's as California Speed's func_8008FFFC
// (a creep up to 6.25 ft/s off the brake, 400 ft/s at most; the spin rates are integrated unless capped, and below
// 3 ft/s on the brake the car stops). Its v.z >= 0 clamp is left out, so the cars reverse with Rush 2's reverse gear
// (rush2_csphys_shift). Returns 1 to skip Rush 2's.
extern "C" int rush2_csphys_velocities(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r4;
    if (role(rdram, car) != Role::Native) return 0;
    float dt = f32(rdram, car + car_dt), v[3], a[3];
    get3(rdram, car + car_velocity, v);
    get3(rdram, car + car_accel, a);
    for (int k = 0; k < 3; k++) v[k] += a[k] * dt;
    float brake = f32(rdram, car + car_brake);
    if (brake == 0.0f && v[2] < 6.25f) v[2] += (6.25f - v[2]) * 0.5f;
    float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    put_f32(rdram, car + car_speed, speed);
    if (speed > 400.0f) {
        float k = 400.0f / speed;
        for (float& c : v) c *= k;
        put3(rdram, car + car_velocity, v);
        // California Speed stores the speed before the cap; here the capped one, since Rush 2's crash check
        // (func_80071724) wrecks a car past 400 ft/s, and under the cap that is a car at its top speed, not a blowup
        // (California Speed has no such check). Its steering terms read it a step later, a percent or so apart.
        put_f32(rdram, car + car_speed, 400.0f);
        return 1;
    }
    float w[3], al[3];
    get3(rdram, car + car_angular, w);
    get3(rdram, car + car_alpha, al);
    for (int k = 0; k < 3; k++) w[k] += al[k] * dt;
    put3(rdram, car + car_angular, w);
    if (speed < 3.0f && brake > 0.5f) {
        v[0] = 0.0f;
        v[2] = 0.0f;
    }
    put3(rdram, car + car_velocity, v);
    return 1;
}

// car_physics_forces_800706D0 at 0x80070714, before car_integrate_position_orientation ($s0 = the car). On the tunnel
// loop (both rear wheels on material 6, func_80090540): the car is lowered by its smallest gap when all four wheels
// are off the road, and stood square to the road under its last hit wheel. Then Rush 2's own lift (any wheel's offset
// past 0.5 ft) is kept out of the step: the offsets are held at 0.5 for it and put back after
// (rush2_csphys_orient_end).
void rush2::calspeed::physics::orient_begin(uint8_t* rdram, uint32_t car) {
    if (role(rdram, car) == Role::Off) return;
    CarState& st = cars[slot_of(car)];
    // RUSH2_CSPHYS_LOG=<physics car>: that car's wheels each step while any wheel is on material 6 or off the road
    // (the tunnel loop's inputs).
    static const int log_car = getenv("RUSH2_CSPHYS_LOG") ? atoi(getenv("RUSH2_CSPHYS_LOG")) : -1;
    if (slot_of(car) == log_car) {
        bool show = false;
        for (int i = 0; i < 4; i++) {
            show |= MEM_HU(0, (int32_t)(car + car_material + 2 * i)) == 6 || st.probe_material[i] == 6 ||
                    MEM_W(0, (int32_t)(car + car_surface + 4 * i)) == 8;
        }
        if (show) {
            printf("[csphys] t %.2f pos %.0f %.0f %.0f", f32(rdram, car + car_clock), f32(rdram, car + car_position),
                   f32(rdram, car + car_position + 4), f32(rdram, car + car_position + 8));
            for (int i = 0; i < 4; i++) {
                printf(" | w%d m%u/%u s%d c%.2f tr%.2f h%.2f", i, MEM_HU(0, (int32_t)(car + car_material + 2 * i)),
                       st.probe_material[i], (int)MEM_W(0, (int32_t)(car + car_surface + 4 * i)),
                       f32(rdram, car + car_compression + 4 * i), f32(rdram, car + car_travel + 4 * i), st.hold[i]);
            }
            printf(" | n %.2f %.2f %.2f\n", st.normal[0], st.normal[1], st.normal[2]);
        }
    }
    if (st.probe_material[2] == 6 && st.probe_material[3] == 6) {
        float gap = 5000.0f;
        for (int i = 0; i < 4; i++) gap = std::min(gap, f32(rdram, car + car_travel + 4 * i));
        if (gap > 0.0f) put_f32(rdram, car + car_position + 4, f32(rdram, car + car_position + 4) - gap);
        float r0[3], r2[3];
        const float* n = st.normal;
        get3(rdram, car + car_matrix, r0);
        float len = std::sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
        if (len > 0.00001f) {
            for (float& c : r0) c /= len;
        }
        r2[2] = r0[0] * n[1] - r0[1] * n[0];
        r2[1] = r0[2] * n[0] - r0[0] * n[2];
        r2[0] = r0[1] * n[2] - r0[2] * n[1];
        r0[0] = n[1] * r2[2] - n[2] * r2[1];
        r0[1] = n[2] * r2[0] - n[0] * r2[2];
        r0[2] = n[0] * r2[1] - n[1] * r2[0];
        put3(rdram, car + car_matrix, r0);
        put3(rdram, car + car_matrix + 12, n);
        put3(rdram, car + car_matrix + 24, r2);
    }
    for (int i = 0; i < 4; i++) {
        st.rest[i] = f32(rdram, car + car_rest + 4 * i);
        if (st.rest[i] > 0.5f && st.role == Role::Native) put_f32(rdram, car + car_rest + 4 * i, 0.5f);
    }
}

// car_physics_forces_800706D0 at 0x8007071C, its end ($s0 = the car): the rest of func_80090540 and func_800901E0.
// The pitch and roll rates zeroed (only the yaw rate carries from step to step); the wheels' offsets back, then the car
// lifted by their mean past 0.35 ft (each offset lowered by it), a human's car first pitched toward level with its
// axles (at most 0.15 rad a second); the wheel and body points again from the moved car, the body points on the low
// side pushed out by 3 x (|roll| - 0.3) once the car rolls past 0.3; and the rear wheels' road speed for the next step.
// A converted car keeps Rush 2's own lift (any offset past 0.5 ft) in place of func_800901E0's (the lift and a human's
// pitch levelling): its tires' load is the springs' / world_g, uncapped, so each lift by the mean offset took grip off
// all four wheels at once, and the light rear let go (San Francisco in a sim: 16 slides past 25 degrees a race on
// converted computer cars with it, 5 without, 4 on Rush 2's physics; California Speed's own cars, their load capped at
// 1100 lb of about 2580, never felt it: 0).
void rush2::calspeed::physics::orient_end(uint8_t* rdram, uint32_t car) {
    if (role(rdram, car) == Role::Off) return;
    CarState& st = cars[slot_of(car)];
    if (!skip(rdram, car, 'r') && !skip(rdram, car, 'p')) put_f32(rdram, car + car_angular, 0.0f);
    if (!skip(rdram, car, 'r')) put_f32(rdram, car + car_angular + 8, 0.0f);
    float d[4], total = 0.0f, front = 0.0f, rear = 0.0f;
    for (int i = 0; i < 4; i++) {
        put_f32(rdram, car + car_rest + 4 * i, st.rest[i]);
        d[i] = st.rest[i] - 0.35f;
        total += d[i];
        (i < 2 ? front : rear) += d[i];
    }
    uint32_t desc = (uint32_t)MEM_W(0, (int32_t)(car + car_desc));
    uint32_t m = car + car_matrix;
    bool own_lift = st.role == Role::Native;
    // The levelling's rate: California Speed's 0.15 rad/s, Partial's 2 (RUSH2_CSPHYS_LEVEL=<rad/s> overrides it, a test
    // aid). With the pitch rate zeroed every step, this is all a player's car pitches by: at 0.15, on San Francisco's
    // hill the front bottomed out on a slope change (1.3 ft), the levelling took a second to follow, the lift held the
    // rear wheels 0.5 ft off the road and the car swung around its front tires (most of a drive's slides, 2026-10-22).
    // The levelling stops at level with the wheels, so the faster rate doesn't overshoot.
    static const char* level_env = getenv("RUSH2_CSPHYS_LEVEL");
    float level_rate = level_env != nullptr ? (float)atof(level_env) : mode.load() == int(Mode::Rushified) ? 2.0f : 0.15f;
    if (own_lift && (int8_t)MEM_B(0, (int32_t)(car + car_human)) == 2) {
        float f = front * 0.5f, r = rear * 0.5f, most = f32(rdram, car + car_dt) * level_rate;
        float zf = f32(rdram, desc + desc_wheels + 8), zr = f32(rdram, desc + desc_wheels + 0x18 + 8);
        float diff = 0.0f, a = 0.0f;
        if (f > 0.0f && r < f) {
            diff = f - r;
            a = f / zf;
        }
        else if (r > 0.0f && f < r) {
            diff = r - f;
            a = -r / zr;
        }
        if (diff != 0.0f) {
            if (most < a) {
                a = most;
            }
            else {
                float b = diff / (zf - zr);
                if (b < a) a = b;
            }
            if (r < f) a = -a;
            float c = std::sqrt(1.0f - a * a);
            total += 2.0f * a * zf + a * zr + a * zr;
            for (int k = 0; k < 3; k++) {
                float up = f32(rdram, m + 12 + 4 * k), fwd = f32(rdram, m + 24 + 4 * k);
                put_f32(rdram, m + 12 + 4 * k, c * up + a * fwd);
                put_f32(rdram, m + 24 + 4 * k, c * fwd - a * up);
            }
        }
    }
    float pos[3];
    get3(rdram, car + car_position, pos);
    if (total > 0.0f && own_lift && !skip(rdram, car, 'l')) {
        float lift = total * 0.25f;
        for (int i = 0; i < 4; i++) put_f32(rdram, car + car_rest + 4 * i, f32(rdram, car + car_rest + 4 * i) - lift);
        float up[3] = { 0.0f, lift, 0.0f }, w[3];
        to_world(rdram, m, up, w);
        for (int k = 0; k < 3; k++) pos[k] += w[k];
        put3(rdram, car + car_position, pos);
    }
    for (int i = 0; i < 4; i++) {
        float p[3] = { f32(rdram, desc + desc_wheels + 12 * i),
                       f32(rdram, desc + desc_wheels + 12 * i + 4) + f32(rdram, car + car_rest + 4 * i),
                       f32(rdram, desc + desc_wheels + 12 * i + 8) }, w[3];
        to_world(rdram, m, p, w);
        for (int k = 0; k < 3; k++) w[k] += pos[k];
        put3(rdram, car + car_wheel_point + 12 * i, w);
    }
    float roll = f32(rdram, m + 4), widen = 0.0f;
    if (std::fabs(roll) > 0.3f) widen = (roll > 0.0f ? std::fabs(roll) - 0.3f : 0.3f - std::fabs(roll)) * 3.0f;
    for (int i = 0; i < 4; i++) {
        float p[3], w[3];
        get3(rdram, car + car_body_offset + 12 * i, p);
        if (widen != 0.0f && widen * p[0] > 0.0f) p[0] += widen;
        to_world(rdram, m, p, w);
        for (int k = 0; k < 3; k++) w[k] += pos[k];
        put3(rdram, car + car_body_point + 12 * i, w);
    }
    // func_800A3E20: (spin x radius of rear wheels 2 and 3) x 0.5 x 2.7272 = mph x 4.
    uint32_t t2 = car + car_tires + 2 * tire_size, t3 = car + car_tires + 3 * tire_size;
    double mph4 = (double)(f32(rdram, t2 + 0x48) * f32(rdram, t2) + f32(rdram, t3 + 0x48) * f32(rdram, t3)) * 0.5 *
                  2.727272727272727;
    st.mph4 = (int16_t)(int32_t)mph4;
    if (SlideLog* log = slide_log(rdram, car)) {
        SlideStep& c = log->cur;
        float vx = f32(rdram, car + car_velocity), vz = f32(rdram, car + car_velocity + 8);
        c.t = f32(rdram, car + car_clock);
        c.speed = f32(rdram, car + car_speed);
        c.slip = std::atan2(vx, std::fabs(vz)) * 57.29578f;
        c.yaw = f32(rdram, car + car_angular + 4);
        c.steer = f32(rdram, car + 0x728);
        c.throttle = f32(rdram, car + car_throttle);
        c.brake = f32(rdram, car + car_brake);
        for (int i = 0; i < 4; i++) {
            c.comp[i] = f32(rdram, car + car_compression + 4 * i);
            c.surface[i] = MEM_W(0, (int32_t)(car + car_surface + 4 * i));
            c.material[i] = st.probe_material[i];
            c.poly[i] = (int16_t)MEM_H(0, (int32_t)(car + 0x594 + 2 * i));
        }
        get3(rdram, car + car_position, c.pos);
        if (log->after > 0) {
            slide_write(*log, c);
            if (--log->after == 0) fflush(log->file);
        }
        bool start = !log->sliding && c.speed > 40.0f && std::fabs(c.slip) > 15.0f;
        if (std::fabs(c.slip) < 8.0f) log->sliding = false;
        if (start) {
            log->sliding = true;
            float pos[3];
            get3(rdram, car + car_position, pos);
            fprintf(log->file, "## slide %d at (%.0f %.0f %.0f), %d steps before:\n", ++log->events, pos[0], pos[1],
                    pos[2], std::min(log->count, slide_keep));
            for (int k = std::max(0, log->count - slide_keep); k < log->count; k++) slide_write(*log, log->ring[k % slide_keep]);
            fprintf(log->file, "## the slide's start, then %d steps after:\n", slide_after);
            slide_write(*log, c);
            log->after = slide_after;
        }
        log->ring[log->count++ % slide_keep] = c;
        c = SlideStep{};
    }
    // RUSH2_CSPHYS_LOG=<physics car>: its drivetrain every 15 steps.
    static const int log_car = getenv("RUSH2_CSPHYS_LOG") ? atoi(getenv("RUSH2_CSPHYS_LOG")) : -1;
    static int log_step = 0;
    if (slot_of(car) == log_car && ++log_step % 15 == 0) {
        printf("[csphys] t %.2f gear %d req %d auto %d rpm %.0f mph %d spin %.1f thr %.2f br %.2f v %.1f %.1f %.1f "
               "shift %.2f\n", f32(rdram, car + car_clock), (int16_t)MEM_H(0, (int32_t)(car + car_gear)),
               (int16_t)MEM_H(0, (int32_t)(car + car_gear_request)), (int16_t)MEM_H(0, (int32_t)(car + 0x3D8)),
               f32(rdram, car + car_engine_speed), st.mph4 >> 2, f32(rdram, car + car_wheel_spin),
               f32(rdram, car + car_throttle), f32(rdram, car + car_brake), f32(rdram, car + car_velocity),
               f32(rdram, car + car_velocity + 4), f32(rdram, car + car_velocity + 8),
               f32(rdram, car + car_shift_time));
    }
}

// The gearbox's automatic shift (Rush 2's func_800711F4, California Speed's func_8008E5C4) for a native car ($a0):
// as Rush 2's (thresholds descriptor +0xE4 / +0xE8 x (throttle + 3) / 4, one shift a second), but a shift up also needs
// the rear wheels' road speed past 26, 66 and 106 mph out of gears 1-3, and below 30, 70 and 110 mph in gears 2-4 the
// box shifts down whatever the engine. Returns 1 to skip Rush 2's.
extern "C" int rush2_csphys_shift(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r4;
    if (role(rdram, car) != Role::Native) return 0;
    const CarState& st = cars[slot_of(car)];
    uint32_t desc = (uint32_t)MEM_W(0, (int32_t)(car + car_desc));
    float scale = (f32(rdram, car + car_throttle) + 3.0f) * 0.25f;
    float up = f32(rdram, desc + desc_shift_up) * scale, down = f32(rdram, desc + desc_shift_down) * scale;
    int spd = st.mph4 >> 2;
    int16_t request = (int16_t)MEM_H(0, (int32_t)(car + car_gear_request));
    if (request == 0 || request == -1) {   // neutral, or Rush 2's reverse (California Speed has none)
        MEM_H(0, (int32_t)(car + car_gear)) = request;
        return 1;
    }
    int top = (int16_t)MEM_H(0, (int32_t)(desc + desc_top_gear));
    float now = f32(rdram, car + car_clock);
    auto gear = [&]() { return (int)(int16_t)MEM_H(0, (int32_t)(car + car_gear)); };
    auto set_gear = [&](int g) { MEM_H(0, (int32_t)(car + car_gear)) = (int16_t)g; };
    if (gear() == 0 || gear() == -1) {
        // func_8008E780: the first gear whose wheel speed keeps the engine under the up threshold.
        set_gear(1);
        while (gear() < top) {
            float rpm = f32(rdram, car + car_wheel_spin) *
                        (f32(rdram, desc + desc_final_drive) * f32(rdram, desc + desc_gears + 4 * uint32_t(gear())));
            if (rpm < up) {
                put_f32(rdram, car + car_shift_time, now + 1.0f);
                break;
            }
            set_gear(gear() + 1);
        }
    }
    int g = gear();
    float rpm = f32(rdram, car + car_engine_speed);
    bool low = (g == 2 && spd < 30) || (g == 3 && spd < 70) || (g == 4 && spd < 110);
    if (!low) {
        if (up < rpm && ((g == 1 && spd >= 26) || (g == 2 && spd >= 66) || (g == 3 && spd >= 106))) {
            if (g < top && f32(rdram, car + car_shift_time) < now) {
                set_gear(g + 1);
                put_f32(rdram, car + car_shift_time, now + 1.0f);
            }
            return 1;
        }
        if (!(rpm < down)) return 1;
    }
    if (g >= 2 && f32(rdram, car + car_shift_time) < now) {
        set_gear(g - 1);
        put_f32(rdram, car + car_shift_time, now + 1.0f);
    }
    return 1;
}

// The physics step's hooks (car_physics_forces_800706D0, $s0 = the car): after car_wheel_forces_drag (0x800706EC),
// after car_torque_from_forces (0x80070704), before car_integrate_position_orientation (0x80070714) and after it
// (0x8007071C).
extern "C" void rush2_csphys_wheel_forces(uint8_t* rdram, recomp_context* ctx) {
    rush2::calspeed::physics::wheel_forces_end(rdram, (uint32_t)ctx->r16);
}

extern "C" void rush2_csphys_torque(uint8_t* rdram, recomp_context* ctx) {
    rush2::calspeed::physics::torque_end(rdram, (uint32_t)ctx->r16);
}

extern "C" void rush2_csphys_orient_begin(uint8_t* rdram, recomp_context* ctx) {
    rush2::calspeed::physics::orient_begin(rdram, (uint32_t)ctx->r16);
}

extern "C" void rush2_csphys_orient_end(uint8_t* rdram, recomp_context* ctx) {
    rush2::calspeed::physics::orient_end(rdram, (uint32_t)ctx->r16);
}

// car_wreck_trigger_8006E1A8's entry ($a0 = the race car, $ra = the caller): with RUSH2_CSPHYS_WRECKS, each wreck and
// where it came from (0x8006E4DC the crash flag, 0x8006FB90 / 0x8006FBA4 the ground under a wheel, 0x800701DC Rush 2's
// body probe, 0x800AB154, 0x800AEAA8 the race start; called from the recompiled code, the under-the-course reset of
// calspeed_rom.cpp shows its own caller).
extern "C" void rush2_csphys_wreck_log(uint8_t* rdram, recomp_context* ctx) {
    static const bool log_wrecks = getenv("RUSH2_CSPHYS_WRECKS") != nullptr;
    if (!log_wrecks || !rush2::calspeed::racing(rdram)) return;
    int s = (int)(int16_t)ctx->r4;
    if (s < 0 || s >= max_cars) return;
    uint32_t car = physics_cars + uint32_t(s) * physics_car_size;
    printf("[cswreck] trigger car %d a1 %d from %08X at %.0f %.0f %.0f speed %.0f\n", s, (int)ctx->r5,
           (uint32_t)ctx->r31, f32(rdram, car + car_position), f32(rdram, car + car_position + 4),
           f32(rdram, car + car_position + 8), f32(rdram, car + car_speed));
}

// Online play's checks and resyncs (include/online_sync.h): each car's California Speed physics state.
void rush2::calspeed::physics::online_sync(rush2::online::sync::Walker& w) {
    namespace s = rush2::online::sync;
    for (CarState& c : cars) {
        s::field(w, c.role);
        s::field(w, c.grip_lateral);
        s::field(w, c.grip_long);
        s::field(w, c.hold);
        s::field(w, c.probe_material);
        s::field(w, c.normal);
        s::field(w, c.rest);
        s::field(w, c.mph4);
        s::field(w, c.wall);
    }
}
