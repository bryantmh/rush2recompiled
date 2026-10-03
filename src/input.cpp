// Multi-controller input for Rush 2.
//
// The frontend's single player mode merges every controller into one input and its multiplayer mode needs players
// assigned by hand through a menu on every launch. Instead, each physical controller drives its own N64 port: a
// controller joins the lowest free port the first time it presses a button and keeps that port until it disconnects.
// Controllers that never press anything (idle pads, virtual duplicates from Steam Input or DS4Windows) never take a
// port. The keyboard always drives port 1. All controllers share the single controller binding profile.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "recompinput/recompinput.h"
#include "recompinput/input_state.h"
#include "recompinput/profiles.h"
#include "recompui/config.h"

#include "rush2.h"

namespace {
    constexpr int num_ports = 4;
    constexpr SDL_JoystickID no_controller = -1;

    // Instance ID of the SDL controller assigned to each port. Written on the game thread, read by the VI thread.
    std::atomic<SDL_JoystickID> port_controllers[num_ports] = { no_controller, no_controller, no_controller, no_controller };

    // Exponent applied to stick X before the game's steering. The game cubes the normalized stick X (func_80076694),
    // which was tuned for the stiff N64 stick; |x|^p with p < 1 cancels part (p = 1/3: all) of that curve.
    std::atomic<float> steering_exponent = 1.0f;

    std::atomic_bool rumble_active[num_ports] = {};
    std::array<float, num_ports> cur_rumble{}; // VI thread only.
    std::array<bool, num_ports> rumble_failed{}; // VI thread only.

#define DEFINE_INPUT(name, value, readable) uint16_t(value##u),
    const std::array n64_button_values = {
        DEFINE_N64_BUTTON_INPUTS()
    };
#undef DEFINE_INPUT

    // Returns the controller for a joystick ID if it's still connected. The frontend never closes controllers, so the
    // pointer stays valid after a disconnect; it's just no longer returned here.
    SDL_GameController* get_connected_controller(SDL_JoystickID id) {
        if (id == no_controller) {
            return nullptr;
        }
        return recompinput::get_controller_from_joystick_id(id);
    }

    int get_port_for_controller(SDL_JoystickID id) {
        for (int port = 0; port < num_ports; port++) {
            if (port_controllers[port].load() == id) {
                return port;
            }
        }
        return -1;
    }

    bool controller_pressing_anything(SDL_GameController* controller) {
        for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++) {
            if (SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)button)) {
                return true;
            }
        }
        // Triggers count as presses, sticks don't so that drift can't claim a port.
        constexpr Sint16 trigger_threshold = 16384;
        return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > trigger_threshold ||
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > trigger_threshold;
    }

    void update_port_assignments() {
        // Free the ports of controllers that disconnected.
        for (int port = 0; port < num_ports; port++) {
            SDL_JoystickID id = port_controllers[port].load();
            if (id != no_controller && get_connected_controller(id) == nullptr) {
                printf("[Input] Controller %d left port %d\n", id, port + 1);
                port_controllers[port] = no_controller;
                rumble_active[port] = false;
            }
        }

        // Presses made to navigate the menus shouldn't claim a port.
        if (recompinput::game_input_disabled()) {
            return;
        }

        int num_joysticks = SDL_NumJoysticks();
        for (int i = 0; i < num_joysticks; i++) {
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
            if (id < 0 || get_port_for_controller(id) >= 0) {
                continue;
            }

            SDL_GameController* controller = get_connected_controller(id);
            if (controller == nullptr || !controller_pressing_anything(controller)) {
                continue;
            }

            for (int port = 0; port < num_ports; port++) {
                if (port_controllers[port].load() == no_controller) {
                    printf("[Input] Controller %d (%s) joined port %d\n", id, SDL_GameControllerName(controller), port + 1);
                    port_controllers[port] = id;
                    break;
                }
            }
        }
    }

    bool controller_digital(SDL_GameController* controller, const recompinput::InputField& field) {
        return controller != nullptr &&
            field.input_type == recompinput::InputType::ControllerDigital &&
            field.input_id >= 0 && field.input_id < SDL_CONTROLLER_BUTTON_MAX &&
            SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)field.input_id);
    }

    // Matches the frontend's controller_axis_state: input_id is the axis + 1, negated for the negative direction.
    float controller_axis(SDL_GameController* controller, const recompinput::InputField& field) {
        if (controller == nullptr || field.input_type != recompinput::InputType::ControllerAnalog) {
            return 0.0f;
        }
        int axis = std::abs(field.input_id) - 1;
        if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) {
            return 0.0f;
        }
        float value = SDL_GameControllerGetAxis(controller, (SDL_GameControllerAxis)axis) * (1 / 32768.0f);
        if (field.input_id < 0) {
            value = -value;
        }
        return std::clamp(value, 0.0f, 1.0f);
    }

    bool get_digital(int profile_index, recompinput::GameInput input, SDL_GameController* controller, bool use_keyboard) {
        int kb_profile_index = recompinput::profiles::get_sp_keyboard_profile_index();
        for (size_t binding = 0; binding < recompinput::num_bindings_per_input; binding++) {
            const recompinput::InputField& field = recompinput::profiles::get_input_binding(profile_index, input, binding);
            if (controller_digital(controller, field) || controller_axis(controller, field) >= recompinput::axis_digital_threshold) {
                return true;
            }
            if (use_keyboard && recompinput::get_input_digital(0, recompinput::profiles::get_input_binding(kb_profile_index, input, binding))) {
                return true;
            }
        }
        return false;
    }

    float get_analog(int profile_index, recompinput::GameInput input, SDL_GameController* controller) {
        float ret = 0.0f;
        for (size_t binding = 0; binding < recompinput::num_bindings_per_input; binding++) {
            const recompinput::InputField& field = recompinput::profiles::get_input_binding(profile_index, input, binding);
            ret += controller_digital(controller, field) ? 1.0f : controller_axis(controller, field);
        }
        return std::clamp(ret, 0.0f, 1.0f);
    }

    float get_keyboard_analog(recompinput::GameInput input) {
        int kb_profile_index = recompinput::profiles::get_sp_keyboard_profile_index();
        float ret = 0.0f;
        for (size_t binding = 0; binding < recompinput::num_bindings_per_input; binding++) {
            ret += recompinput::get_input_analog(0, recompinput::profiles::get_input_binding(kb_profile_index, input, binding));
        }
        return std::clamp(ret, 0.0f, 1.0f);
    }
}

void rush2::input::poll() {
    recompinput::poll_inputs();
    update_port_assignments();
}

bool rush2::input::is_port_connected(int port) {
    // Port 1 is always connected since the keyboard drives it.
    return port == 0 || (port > 0 && port < num_ports && port_controllers[port].load() != no_controller);
}

bool rush2::input::get_n64_input(int port, uint16_t* buttons_out, float* x_out, float* y_out) {
    *buttons_out = 0;
    *x_out = 0.0f;
    *y_out = 0.0f;

    if (!is_port_connected(port)) {
        return false;
    }

    if (recompinput::game_input_disabled()) {
        return true;
    }

    using recompinput::GameInput;
    SDL_GameController* controller = get_connected_controller(port_controllers[port].load());
    bool use_keyboard = port == 0;
    int profile_index = recompinput::profiles::get_sp_controller_profile_index();

    uint16_t buttons = 0;
    for (size_t i = 0; i < n64_button_values.size(); i++) {
        GameInput input = static_cast<GameInput>(static_cast<size_t>(GameInput::N64_BUTTON_START) + i);
        if (get_digital(profile_index, input, controller, use_keyboard)) {
            buttons |= n64_button_values[i];
        }
    }

    float x = get_analog(profile_index, GameInput::X_AXIS_POS, controller) - get_analog(profile_index, GameInput::X_AXIS_NEG, controller);
    float y = get_analog(profile_index, GameInput::Y_AXIS_POS, controller) - get_analog(profile_index, GameInput::Y_AXIS_NEG, controller);
    recompinput::apply_joystick_deadzone(x, y, &x, &y);
    x = std::copysign(std::pow(std::fabs(x), steering_exponent.load()), x);

    if (use_keyboard) {
        x += get_keyboard_analog(GameInput::X_AXIS_POS) - get_keyboard_analog(GameInput::X_AXIS_NEG);
        y += get_keyboard_analog(GameInput::Y_AXIS_POS) - get_keyboard_analog(GameInput::Y_AXIS_NEG);
    }

    *buttons_out = buttons;
    *x_out = std::clamp(x, -1.0f, 1.0f);
    *y_out = std::clamp(y, -1.0f, 1.0f);
    return true;
}

void rush2::input::set_steering_exponent(float exponent) {
    steering_exponent = exponent;
}

void rush2::input::set_rumble(int port, bool on) {
    if (port >= 0 && port < num_ports) {
        rumble_active[port] = on;
    }
}

// Same ramp up and fall off as the frontend's update_rumble, but each port only rumbles its own controller.
void rush2::input::update_rumble() {
    if (!recompui::config::general::has_rumble_strength_option()) {
        return;
    }

    for (int port = 0; port < num_ports; port++) {
        if (rumble_active[port]) {
            cur_rumble[port] = std::min(cur_rumble[port] + 0.17f, 1.0f);
        }
        else {
            cur_rumble[port] = std::max(cur_rumble[port] * 0.92f - 0.01f, 0.0f);
        }

        SDL_GameController* controller = get_connected_controller(port_controllers[port].load());
        if (controller == nullptr) {
            rumble_failed[port] = false;
            continue;
        }
        if (rumble_failed[port]) {
            continue;
        }

        float amount = cur_rumble[port];
        float smooth_rumble = amount * amount * (3.0f - 2.0f * amount);
        uint16_t strength = (uint16_t)(smooth_rumble * (recompui::config::general::get_rumble_strength() * 0xFFFF / 100));
        // Long enough to last until the next update; the game turns rumble off on its own.
        constexpr uint32_t duration_ms = 1000000;
        if (SDL_GameControllerRumble(controller, 0, strength, duration_ms) != 0) {
            rumble_failed[port] = true;
        }
    }
}
