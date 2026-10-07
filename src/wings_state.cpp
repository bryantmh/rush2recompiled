// Wings: physics and deploy state.
//
// Rush 2 runs the same car physics as Rush 2049 (same integrators, constants and units: feet, ft/s, g = 32.2), so
// Rush 2049's wing code carries over directly. In Rush 2049 (func_800E15A0, func_800E1C30, func_800E23A4):
//
// - Each physics step, a car's wings are out when it isn't wrecked, all four wheels are more than 5 ft above the
//   ground, and its player holds the wings button. Only human players have wings.
// - While they're out, the stick adds torque: pitch -= stick Y * gain * speed, roll += stick X * gain * speed
//   (negated on mirrored tracks), with the stick as the raw normalized analog value (raw / calibration range,
//   clamped to +-1).
// - Gravity is scaled while the car isn't climbing (its height didn't rise during the last position update), and
//   the car's longitudinal resistance is scaled.
// - Drag and gravity are computed before the torque in each step, so they use the previous step's wing state.
//
// The three wing styles differ in gain {30, 40, 50}, gravity {1.0, 0.8, 0.5} and resistance {1.0, 1.5, 1.0}. Rush
// 2049 lets each player choose a style on the car setup screen; here it's a setting per player.
//
// The visual state follows Rush 2049's wing callback (func_800924F4): when the wings come out they slide out to
// the side in 6 steps of 15 ms, starting at 0 and stopping at 5/6 of the car's slide distance; when they go away
// they slide back in from the full distance and disappear. While out they tilt with the stick, and the flame on
// each side stretches with the stick's X (func_80091874).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

#include "recomp.h"
#include "rush2_hooks.h"
#include "wings_internal.h"

namespace {
    // Rush 2 car (physics) structs: 0x800F5470 + index * 0x81C.
    constexpr uint32_t car_index = 0x7E0;     // s16
    constexpr uint32_t car_type = 0x7EA;      // u8, index into the car list
    constexpr uint32_t car_flags = 0x7F4;     // 0x10 = wrecked
    constexpr uint32_t car_torque_x = 0x10;
    constexpr uint32_t car_torque_z = 0x18;
    constexpr uint32_t car_resistance = 0x54;
    constexpr uint32_t car_gravity_y = 0x2C4;   // World gravity force, set once at init in Rush 2.
    constexpr uint32_t car_position_y = 0x228;
    constexpr uint32_t car_speed = 0x3D4;
    constexpr uint32_t car_wheel_height = 0x5DC; // 4 floats: height of each wheel above the ground.
    constexpr uint32_t car_dt = 0x718;

    constexpr uint32_t car_states = 0x801124A0; // Per car: + index * 0x354; +0x350 = human player struct or 0.
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t car_state_player = 0x350;
    constexpr uint32_t players = 0x800C2140;
    constexpr uint32_t player_size = 0x28;
    constexpr uint32_t mirror_flag = 0x800D0190;

    constexpr float airborne_height = 5.0f;
    constexpr std::array<float, 3> style_gain = { 30.0f, 40.0f, 50.0f };
    constexpr std::array<float, 3> style_gravity = { 1.0f, 0.8f, 0.5f };
    constexpr std::array<float, 3> style_resistance = { 1.0f, 1.5f, 1.0f };

    constexpr float step_time = 0.015f;
    constexpr int open_frames = 6;
    constexpr float tilt_scale = 0.507f;
    constexpr float flame_scale = 8.0f;
    constexpr float min_flame_scale = 2.0f;

    // Wing placement per Rush 2 car: half the body's width and its height (world units, from the car models).
    // Rush 2049 slides its wings to rest about 0.72 units past each car's half width (5/6 of its per-car table value),
    // 1 unit up, or higher on its tall truck; the same rule is applied to Rush 2's cars.
    struct CarSize {
        float half_width;
        float height;
    };
    constexpr std::array<CarSize, 22> car_sizes = { {
        { 2.812f, 4.312f }, // PICKUP
        { 3.000f, 3.688f }, // INTEG
        { 2.875f, 3.375f }, // VETTE
        { 2.875f, 3.938f }, // SLED
        { 3.000f, 3.750f }, // BMW
        { 2.812f, 3.438f }, // CAMARO
        { 2.812f, 3.250f }, // SUPRA
        { 3.000f, 2.688f }, // BUGAT
        { 2.750f, 4.312f }, // VWBUS
        { 3.062f, 3.188f }, // VIPER
        { 3.125f, 3.875f }, // VWBUG
        { 3.000f, 3.125f }, // CONCPT
        { 2.938f, 3.625f }, // CIVIC
        { 3.312f, 4.000f }, // CADDY
        { 3.125f, 3.500f }, // MUST
        { 3.125f, 4.688f }, // SUV
        { 3.000f, 3.875f }, // TAXI
        { 3.188f, 3.562f }, // HOTROD
        { 2.875f, 2.688f }, // FORM1
        { 3.000f, 3.062f }, // GT90
        { 1.938f, 5.562f }, // ROCKET
        { 2.500f, 5.000f }, // DEW
    } };
    constexpr float rest_gap = 0.72f;
    // Rush 2049's cars are up to 3.44 units tall with the wings 1 unit up; its 5 unit truck has them 3 units up.
    constexpr float low_height = 3.44f;
    constexpr float tall_height = 5.0f;

    enum WingFlags : uint32_t {
        deployed = 0x10,
        opening = 0x100,
        closing = 0x200,
    };

    struct CarWings {
        bool flag = false;  // Rush 2049's per-step wing state (player + 0x3F8).
        float prev_y = 0.0f;
        bool gravity_saved = false;
        float saved_gravity = 0.0f;
        uint32_t flags = 0;
        int frame = 0;
        float clock = 0.0f;
        float timer = 0.0f;
        float slide = 0.0f;
        float stick_x = 0.0f;
        float stick_y = 0.0f;
        int style = 0;
        int player = -1;
    };
    std::array<CarWings, rush2::wings::max_cars> cars;

    // Ghost cars' recorded wing input (physics thread).
    struct GhostInput {
        bool active = false;
        rush2::wings::Input input;
    };
    std::array<GhostInput, rush2::wings::max_cars> ghost_inputs;

    std::array<std::atomic<int>, 2> player_style = { 0, 0 };

    float& f32(uint8_t* rdram, uint32_t addr) {
        return *reinterpret_cast<float*>(&MEM_W(0, (int32_t)addr));
    }

    // The human player struct driving a car, or 0 for drones.
    uint32_t car_player(uint8_t* rdram, int index) {
        return (uint32_t)MEM_W(0, (int32_t)((car_states + index * car_state_size) + car_state_player));
    }

    // Rush 2049's func_800C9590: raw stick over its auto-calibrated range, clamped. Rush 2 calibrates the same way.
    float stick_axis(uint8_t* rdram, uint32_t player, uint32_t axis) {
        int raw = MEM_B(0, (int32_t)(player + 8 + axis));
        int range = MEM_B(0, (int32_t)(player + 0xA + axis));
        if (range <= 0) {
            return 0.0f;
        }
        return std::clamp(float(raw) / float(range), -1.0f, 1.0f);
    }

    int car_index_of(uint8_t* rdram, uint32_t car) {
        int index = MEM_H(0, (int32_t)(car + car_index));
        return (index >= 0 && index < rush2::wings::max_cars) ? index : -1;
    }

    float rest_slide(uint8_t* rdram, uint32_t car) {
        uint8_t type = MEM_BU(0, (int32_t)(car + car_type));
        float half_width = type < car_sizes.size() ? car_sizes[type].half_width : 3.0f;
        return (half_width + rest_gap) * open_frames / (open_frames - 1);
    }

    float wing_height(uint8_t* rdram, uint32_t car) {
        uint8_t type = MEM_BU(0, (int32_t)(car + car_type));
        float height = type < car_sizes.size() ? car_sizes[type].height : low_height;
        return 1.0f + std::max(0.0f, height - low_height) * (2.0f / (tall_height - low_height));
    }

    // Rush 2049's wing callback, once per physics step.
    void update_visuals(uint8_t* rdram, uint32_t car, CarWings& w, float dt) {
        w.clock += dt;
        bool was_animating = (w.flags & (opening | closing)) != 0;

        if (w.flag && !(w.flags & deployed)) {
            w.flags = deployed | opening;
            w.frame = 0;
            w.timer = w.clock + step_time;
            w.slide = 0.0f;
        }
        else if (!w.flag && (w.flags & deployed) && !(w.flags & closing)) {
            w.flags = (w.flags & ~opening) | closing;
            w.timer = w.clock + step_time;
        }

        if ((w.flags & (opening | closing)) && w.timer < w.clock) {
            float full = rest_slide(rdram, car);
            if (w.flags & opening) {
                w.slide = full * w.frame / open_frames;
                w.frame++;
                if (w.frame == open_frames) {
                    w.flags &= ~opening;
                }
            }
            else {
                w.slide = full * w.frame / open_frames;
                w.frame--;
                if (w.frame < 0) {
                    w.flags = 0;
                }
            }
            w.timer = w.clock + step_time;
        }

        bool animating = (w.flags & (opening | closing)) != 0;
        if (animating != was_animating) {
            rush2::wings::set_sound(int(&w - cars.data()), animating);
        }
    }
}

void rush2::wings::set_player_style(int player, int style) {
    if (player >= 0 && player < 2) {
        player_style[player] = std::clamp(style, 0, 2);
    }
}

bool rush2::wings::get_pose(uint8_t* rdram, int index, Pose& out) {
    const CarWings& w = cars[index];
    if (!(w.flags & deployed)) {
        return false;
    }
    uint32_t car = car_struct(index);
    out.style = w.style;
    out.slide = w.slide;
    out.height = wing_height(rdram, car);
    out.tilt = w.stick_y * tilt_scale;
    out.flames = true;
    out.flame_scale[0] = std::max(-flame_scale * w.stick_x, min_flame_scale);
    out.flame_scale[1] = std::max(flame_scale * w.stick_x, min_flame_scale);
    return true;
}

bool rush2::wings::car_input(uint8_t* rdram, int car, Input& out) {
    if (car < 0 || car >= max_cars) {
        return false;
    }
    uint32_t player = car_player(rdram, car);
    if (player == 0) {
        return false;
    }
    int player_index = int((player - players) / player_size);
    out.held = player_index >= 0 && player_index < 2 && button_held(rdram, player_index);
    out.stick_x = stick_axis(rdram, player, 0);
    out.stick_y = stick_axis(rdram, player, 1);
    out.style = player_index >= 0 && player_index < 2 ? player_style[player_index].load() : 0;
    return true;
}

void rush2::wings::set_ghost_input(int car, bool active, const Input& in) {
    if (car >= 0 && car < max_cars) {
        ghost_inputs[car].active = active;
        ghost_inputs[car].input = in;
    }
}

uint32_t rush2::wings::car_struct(int index) {
    return 0x800F5470u + uint32_t(index) * 0x81Cu;
}

// func_800706D0 (physics step), after the torque sum and before it's used. $s0 = car.
extern "C" void rush2_wings_torque(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r16;
    int index = car_index_of(rdram, car);
    if (index < 0) {
        return;
    }
    CarWings& w = cars[index];

    uint32_t player = car_player(rdram, index);
    const GhostInput& ghost = ghost_inputs[index];
    bool flag = false;
    if (rush2::wings::enabled() && (player != 0 || ghost.active) && !(MEM_W(0, (int32_t)(car + car_flags)) & 0x10)) {
        flag = true;
        for (int i = 0; i < 4; i++) {
            if (!(f32(rdram, car + car_wheel_height + i * 4) > airborne_height)) {
                flag = false;
            }
        }
        if (player != 0) {
            int player_index = int((player - players) / player_size);
            flag = flag && player_index >= 0 && player_index < 2 && rush2::wings::button_held(rdram, player_index);
            w.player = player_index;
        }
        else {
            flag = flag && ghost.input.held;
        }
    }
    w.flag = flag;

    if (player != 0) {
        w.stick_x = stick_axis(rdram, player, 0);
        w.stick_y = stick_axis(rdram, player, 1);
        if (w.player >= 0 && w.player < 2) {
            w.style = player_style[w.player];
        }
    }
    else if (ghost.active) {
        w.stick_x = ghost.input.stick_x;
        w.stick_y = ghost.input.stick_y;
        w.style = std::clamp(ghost.input.style, 0, 2);
    }

    if (flag) {
        float gain = style_gain[w.style];
        float speed = f32(rdram, car + car_speed);
        float mirror = MEM_B(0, (int32_t)mirror_flag) == 1 ? -1.0f : 1.0f;
        f32(rdram, car + car_torque_x) -= w.stick_y * gain * speed;
        f32(rdram, car + car_torque_z) += w.stick_x * gain * speed * mirror;
    }

    update_visuals(rdram, car, w, f32(rdram, car + car_dt));
}

// func_800706D0, before position integration. $s0 = car.
extern "C" void rush2_wings_save_position(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r16;
    int index = car_index_of(rdram, car);
    if (index >= 0) {
        cars[index].prev_y = f32(rdram, car + car_position_y);
    }
}

// func_8006A02C (gravity), before world gravity is rotated into the car's frame. $a3 = car. Rush 2049 recomputes
// gravity every step and scales it while gliding. Rush 2 sets it once at car init (only this function reads it), so
// the game's value is kept aside while it's scaled and put back afterwards.
extern "C" void rush2_wings_gravity(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r7;
    int index = car_index_of(rdram, car);
    if (index < 0) {
        return;
    }
    CarWings& w = cars[index];
    float& gravity = f32(rdram, car + car_gravity_y);
    if (w.flag && w.prev_y >= f32(rdram, car + car_position_y)) {
        if (!w.gravity_saved) {
            w.saved_gravity = gravity;
            w.gravity_saved = true;
        }
        gravity = w.saved_gravity * style_gravity[w.style];
    }
    else if (w.gravity_saved) {
        gravity = w.saved_gravity;
        w.gravity_saved = false;
    }
}

// func_8006AFD8 (drag), after the longitudinal resistance is stored and before gear terms are added. $s0 = car.
extern "C" void rush2_wings_drag(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r16;
    int index = car_index_of(rdram, car);
    if (index >= 0 && cars[index].flag) {
        f32(rdram, car + car_resistance) *= style_resistance[cars[index].style];
    }
}

// func_8008DBA0 (car init). $s2 = car.
extern "C" void rush2_wings_car_init(uint8_t* rdram, recomp_context* ctx) {
    int index = car_index_of(rdram, (uint32_t)ctx->r18);
    if (index >= 0) {
        if (cars[index].flags & (opening | closing)) {
            rush2::wings::set_sound(index, false);
        }
        cars[index] = CarWings{};
    }
}
