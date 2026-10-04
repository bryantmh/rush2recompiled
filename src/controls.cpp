// Button bindings for driving.
//
// The game binds its driving actions to N64 buttons in its Controller Setup screen (one code per action and port at
// 0x80125A70, see src/controls_menu.cpp). That table is locked to the game's default layout, and the inputs bound here
// are turned into that layout during races: GAS = A, BRAKE = B, STEERING = stick, SHIFT UP = R, SHIFT DOWN = Z,
// REVERSE = C-down, ABORT = C-up, VIEW = L, HORN = C-right, and WINGS = C-left, the one button the defaults leave free.
// The game never sees which physical button was pressed, so any controller button, trigger, stick direction or key can
// drive any action, and the game's own nine-button limit and conflict checks don't apply.
//
// Menus use a fixed N64 layout instead (get_menu_input), so binding GAS to a trigger doesn't make the trigger the
// menus' confirm button. src/input.cpp decides which one a port gets each frame.
//
// Each player (port) has one set of bindings for its controller and one for the keyboard; the keyboard set is used
// when the keyboard is assigned to that port. Every action has one input, except keyboard steering, which has a left
// and a right key. Bindings are saved to bindings.json in the config folder.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "json/json.hpp"
#include "librecomp/game.hpp"
#include "recompinput/recompinput.h"
#include "recompinput/input_state.h"

#include "rush2.h"

using rush2::controls::Action;
using rush2::controls::Device;
using rush2::controls::Input;
using rush2::input::num_ports;

namespace {
    using Slots = std::array<Input, rush2::controls::slots_per_action>;
    using ActionBindings = std::array<Slots, rush2::controls::action_count>;
    using PortBindings = std::array<ActionBindings, rush2::controls::device_count>;

    std::mutex bindings_mutex;
    std::array<PortBindings, num_ports> bindings{};
    std::array<PortBindings, num_ports> snapshot_bindings{};

    // N64 buttons of the game's default layout, by action (steering is the stick).
    constexpr std::array<uint16_t, rush2::controls::action_count> action_buttons = {
        0x8000, // GAS: A
        0x4000, // BRAKE: B
        0x0000, // STEERING: stick
        0x0010, // SHIFT UP: R
        0x2000, // SHIFT DOWN: Z
        0x0004, // REVERSE: C-down
        0x0008, // ABORT: C-up
        0x0020, // VIEW: L
        0x0001, // HORN: C-right
        0x0002, // WINGS: C-left
    };

    constexpr uint16_t n64_a = 0x8000;
    constexpr uint16_t n64_b = 0x4000;
    constexpr uint16_t n64_z = 0x2000;
    constexpr uint16_t n64_start = 0x1000;
    constexpr uint16_t n64_dpad_up = 0x0800;
    constexpr uint16_t n64_dpad_down = 0x0400;
    constexpr uint16_t n64_dpad_left = 0x0200;
    constexpr uint16_t n64_dpad_right = 0x0100;
    constexpr uint16_t n64_l = 0x0020;
    constexpr uint16_t n64_r = 0x0010;
    constexpr uint16_t n64_c_up = 0x0008;
    constexpr uint16_t n64_c_down = 0x0004;
    constexpr uint16_t n64_c_left = 0x0002;
    constexpr uint16_t n64_c_right = 0x0001;

    constexpr float digital_threshold = 0.5f;
    // GAS and BRAKE are analog pedals: a trigger counts as pressed past the deadzone, and the hook in src/input.cpp
    // scales the game's pedal by how far it's pressed. Pressure from the deadzone to near the end maps to 0..1.
    constexpr float pedal_deadzone = 0.06f;
    constexpr float pedal_full = 0.95f;
    // Listening needs a firmer push, so a resting trigger or a drifting stick doesn't get bound.
    constexpr float listen_threshold = 0.75f;
    constexpr auto listen_timeout = std::chrono::seconds(6);

    Input button(SDL_GameControllerButton b) { return { Input::Type::Button, b }; }
    Input axis(SDL_GameControllerAxis a, bool positive) { return { positive ? Input::Type::AxisPositive : Input::Type::AxisNegative, a }; }
    Input key(SDL_Scancode k) { return { Input::Type::Key, k }; }
    Input stick(rush2::controls::Stick s) { return { Input::Type::Stick, s }; }

    ActionBindings default_bindings(Device device) {
        ActionBindings b{};
        auto set = [&](Action action, Input in, Input in2 = {}) {
            b[static_cast<int>(action)] = { in, in2 };
        };
        if (device == Device::Controller) {
            set(Action::Gas, axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true));
            set(Action::Brake, axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true));
            set(Action::Steering, stick(rush2::controls::LeftStick));
            set(Action::ShiftUp, button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
            set(Action::ShiftDown, button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
            set(Action::Reverse, button(SDL_CONTROLLER_BUTTON_X));
            set(Action::Abort, button(SDL_CONTROLLER_BUTTON_B));
            set(Action::View, button(SDL_CONTROLLER_BUTTON_Y));
            set(Action::Horn, button(SDL_CONTROLLER_BUTTON_LEFTSTICK));
            set(Action::Wings, button(SDL_CONTROLLER_BUTTON_A));
        }
        else {
            set(Action::Gas, key(SDL_SCANCODE_UP));
            set(Action::Brake, key(SDL_SCANCODE_DOWN));
            set(Action::Steering, key(SDL_SCANCODE_LEFT), key(SDL_SCANCODE_RIGHT));
            set(Action::ShiftUp, key(SDL_SCANCODE_E));
            set(Action::ShiftDown, key(SDL_SCANCODE_Q));
            set(Action::Reverse, key(SDL_SCANCODE_R));
            set(Action::Abort, key(SDL_SCANCODE_F));
            set(Action::View, key(SDL_SCANCODE_C));
            set(Action::Horn, key(SDL_SCANCODE_H));
            set(Action::Wings, key(SDL_SCANCODE_SPACE));
        }
        return b;
    }

    Device device_of(const Input& in) {
        return in.type == Input::Type::Key ? Device::Keyboard : Device::Controller;
    }

    // The devices driving a port this frame.
    struct PortDevices {
        SDL_GameController* controller = nullptr;
        const Uint8* keys = nullptr;
        int num_keys = 0;
    };

    PortDevices get_devices(int port) {
        PortDevices d{};
        int32_t id = rush2::input::get_port_controller(port);
        if (id >= 0) {
            d.controller = recompinput::get_controller_from_joystick_id(id);
        }
        if (rush2::input::get_keyboard_port() == port) {
            d.keys = SDL_GetKeyboardState(&d.num_keys);
        }
        return d;
    }

    float axis_value(SDL_GameController* c, int axis_id) {
        return SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)axis_id) * (1 / 32768.0f);
    }

    bool key_down(const PortDevices& d, int scancode) {
        return d.keys != nullptr && scancode >= 0 && scancode < d.num_keys && d.keys[scancode];
    }

    bool button_down(const PortDevices& d, SDL_GameControllerButton b) {
        return d.controller != nullptr && SDL_GameControllerGetButton(d.controller, b);
    }

    // How far an input is pressed, 0 to 1. Sticks aren't buttons and read as 0.
    float input_value(const PortDevices& d, const Input& in) {
        switch (in.type) {
            case Input::Type::Button:
                return (d.controller != nullptr && in.id >= 0 && in.id < SDL_CONTROLLER_BUTTON_MAX &&
                    SDL_GameControllerGetButton(d.controller, (SDL_GameControllerButton)in.id)) ? 1.0f : 0.0f;
            case Input::Type::AxisPositive:
            case Input::Type::AxisNegative: {
                if (d.controller == nullptr || in.id < 0 || in.id >= SDL_CONTROLLER_AXIS_MAX) {
                    return 0.0f;
                }
                float v = axis_value(d.controller, in.id);
                return std::clamp(in.type == Input::Type::AxisPositive ? v : -v, 0.0f, 1.0f);
            }
            case Input::Type::Key:
                return key_down(d, in.id) ? 1.0f : 0.0f;
            default:
                return 0.0f;
        }
    }

    // How far an action is pressed, 0 to 1: the furthest of its bound inputs.
    float action_value(const PortDevices& d, const PortBindings& b, Action action) {
        float value = 0.0f;
        for (int device = 0; device < rush2::controls::device_count; device++) {
            for (const Input& in : b[device][static_cast<int>(action)]) {
                value = std::max(value, input_value(d, in));
            }
        }
        return value;
    }

    bool action_held(const PortDevices& d, const PortBindings& b, Action action) {
        return action_value(d, b, action) >= digital_threshold;
    }

    // An analog pedal's pressure past the deadzone, 0 to 1.
    float pedal_value(const PortDevices& d, const PortBindings& b, Action action) {
        float v = action_value(d, b, action);
        return v <= pedal_deadzone ? 0.0f : std::min((v - pedal_deadzone) / (pedal_full - pedal_deadzone), 1.0f);
    }

    void stick_axes(SDL_GameController* c, int stick_id, float* x, float* y) {
        if (stick_id == rush2::controls::RightStick) {
            *x = axis_value(c, SDL_CONTROLLER_AXIS_RIGHTX);
            *y = -axis_value(c, SDL_CONTROLLER_AXIS_RIGHTY);
        }
        else {
            *x = axis_value(c, SDL_CONTROLLER_AXIS_LEFTX);
            *y = -axis_value(c, SDL_CONTROLLER_AXIS_LEFTY);
        }
    }

    // Uses a stick (or the D-pad) for steering, which also removes any action bound to one of its directions.
    bool input_uses_stick(const Input& in, int stick_id) {
        if (stick_id == rush2::controls::Dpad) {
            return in.type == Input::Type::Button &&
                (in.id == SDL_CONTROLLER_BUTTON_DPAD_UP || in.id == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
                 in.id == SDL_CONTROLLER_BUTTON_DPAD_LEFT || in.id == SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        }
        if (in.type != Input::Type::AxisPositive && in.type != Input::Type::AxisNegative) {
            return false;
        }
        if (stick_id == rush2::controls::RightStick) {
            return in.id == SDL_CONTROLLER_AXIS_RIGHTX || in.id == SDL_CONTROLLER_AXIS_RIGHTY;
        }
        return in.id == SDL_CONTROLLER_AXIS_LEFTX || in.id == SDL_CONTROLLER_AXIS_LEFTY;
    }

    int steering_stick(const PortBindings& b) {
        const Input& in = b[static_cast<int>(Device::Controller)][static_cast<int>(Action::Steering)][0];
        return in.type == Input::Type::Stick ? in.id : -1;
    }

    // Binds `in` to an action slot. Whatever already used `in` on the same device takes the slot's old input.
    void assign(int port, Action action, int slot, Input in) {
        std::lock_guard lock{ bindings_mutex };
        int device = static_cast<int>(device_of(in));
        ActionBindings& set = bindings[port][device];
        Input old = set[static_cast<int>(action)][slot];
        for (int a = 0; a < rush2::controls::action_count; a++) {
            for (int s = 0; s < rush2::controls::slots_per_action; s++) {
                if ((a != static_cast<int>(action) || s != slot) && set[a][s] == in) {
                    set[a][s] = old;
                }
            }
        }
        set[static_cast<int>(action)][slot] = in;

        if (in.type == Input::Type::Stick) {
            for (int a = 0; a < rush2::controls::action_count; a++) {
                for (Input& other : set[a]) {
                    if (input_uses_stick(other, in.id)) {
                        other = {};
                    }
                }
            }
        }
    }

    // Listening state per port. Only touched from the game thread.
    struct Listen {
        bool active = false;
        bool waiting_release = false;
        Action action = Action::Gas;
        int slot = 0;
        std::chrono::steady_clock::time_point start;
    };
    std::array<Listen, num_ports> listens{};

    // Buttons that can't be bound: Start pauses and the frontend uses Back and Guide to open its menu.
    bool reserved_button(int b) {
        return b == SDL_CONTROLLER_BUTTON_START || b == SDL_CONTROLLER_BUTTON_BACK || b == SDL_CONTROLLER_BUTTON_GUIDE;
    }
    // Return pauses and Escape opens the frontend's menu.
    bool reserved_key(int k) {
        return k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_ESCAPE;
    }

    // The input being pressed on a port's devices, if any, for binding to `action`.
    enum class Scan { Nothing, Captured, Cancel };
    Scan scan_input(int port, Action action, Input* out) {
        PortDevices d = get_devices(port);
        int steer = steering_stick(bindings[port]);

        if (d.controller != nullptr) {
            if (button_down(d, SDL_CONTROLLER_BUTTON_START)) {
                return Scan::Cancel;
            }
            if (action == Action::Steering) {
                for (int s : { rush2::controls::LeftStick, rush2::controls::RightStick }) {
                    float x, y;
                    stick_axes(d.controller, s, &x, &y);
                    if (std::sqrt(x * x + y * y) >= listen_threshold) {
                        *out = stick(static_cast<rush2::controls::Stick>(s));
                        return Scan::Captured;
                    }
                }
                for (SDL_GameControllerButton b : { SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                                   SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT }) {
                    if (button_down(d, b)) {
                        *out = stick(rush2::controls::Dpad);
                        return Scan::Captured;
                    }
                }
            }
            else {
                for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
                    Input in = button((SDL_GameControllerButton)b);
                    if (!reserved_button(b) && !input_uses_stick(in, steer) && button_down(d, (SDL_GameControllerButton)b)) {
                        *out = in;
                        return Scan::Captured;
                    }
                }
                for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
                    float v = axis_value(d.controller, a);
                    Input in = axis((SDL_GameControllerAxis)a, v > 0.0f);
                    if (std::fabs(v) >= listen_threshold && !input_uses_stick(in, steer)) {
                        *out = in;
                        return Scan::Captured;
                    }
                }
            }
        }

        if (d.keys != nullptr) {
            for (int k = SDL_SCANCODE_A; k < d.num_keys; k++) {
                if (!d.keys[k]) {
                    continue;
                }
                if (k == SDL_SCANCODE_RETURN) {
                    return Scan::Cancel;
                }
                if (!reserved_key(k)) {
                    *out = key((SDL_Scancode)k);
                    return Scan::Captured;
                }
            }
        }
        return Scan::Nothing;
    }

    // JSON names for actions and inputs.
    constexpr std::array<const char*, rush2::controls::action_count> action_names = {
        "gas", "brake", "steering", "shift_up", "shift_down", "reverse", "abort", "view", "horn", "wings",
    };
    constexpr std::array<const char*, 3> stick_names = { "left", "right", "dpad" };

    nlohmann::json input_to_json(const Input& in) {
        switch (in.type) {
            case Input::Type::Button: {
                const char* name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)in.id);
                return { { "button", name ? name : "" } };
            }
            case Input::Type::AxisPositive:
            case Input::Type::AxisNegative: {
                const char* name = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)in.id);
                return { { "axis", std::string(in.type == Input::Type::AxisPositive ? "+" : "-") + (name ? name : "") } };
            }
            case Input::Type::Key:
                return { { "key", SDL_GetScancodeName((SDL_Scancode)in.id) } };
            case Input::Type::Stick:
                return { { "stick", stick_names[std::clamp<int>(in.id, 0, 2)] } };
            default:
                return nullptr;
        }
    }

    bool input_from_json(const nlohmann::json& j, Input* out) {
        *out = {};
        if (!j.is_object()) {
            return j.is_null();
        }
        if (j.contains("button")) {
            auto b = SDL_GameControllerGetButtonFromString(j["button"].get<std::string>().c_str());
            if (b == SDL_CONTROLLER_BUTTON_INVALID) {
                return false;
            }
            *out = button(b);
        }
        else if (j.contains("axis")) {
            std::string s = j["axis"].get<std::string>();
            if (s.size() < 2) {
                return false;
            }
            auto a = SDL_GameControllerGetAxisFromString(s.c_str() + 1);
            if (a == SDL_CONTROLLER_AXIS_INVALID) {
                return false;
            }
            *out = axis(a, s[0] == '+');
        }
        else if (j.contains("key")) {
            SDL_Scancode k = SDL_GetScancodeFromName(j["key"].get<std::string>().c_str());
            if (k == SDL_SCANCODE_UNKNOWN) {
                return false;
            }
            *out = key(k);
        }
        else if (j.contains("stick")) {
            auto it = std::find(stick_names.begin(), stick_names.end(), j["stick"].get<std::string>());
            if (it == stick_names.end()) {
                return false;
            }
            *out = stick(static_cast<rush2::controls::Stick>(it - stick_names.begin()));
        }
        return true;
    }

    std::filesystem::path save_path() {
        return recomp::get_config_path() / "bindings.json";
    }
}

void rush2::controls::load() {
    std::lock_guard lock{ bindings_mutex };
    for (int port = 0; port < num_ports; port++) {
        for (int device = 0; device < device_count; device++) {
            bindings[port][device] = default_bindings(static_cast<Device>(device));
        }
    }

    std::ifstream in{ save_path() };
    if (!in) {
        return;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(in);
        const auto& ports = j.at("players");
        for (int port = 0; port < num_ports && port < (int)ports.size(); port++) {
            for (int device = 0; device < device_count; device++) {
                const char* device_name = device == static_cast<int>(Device::Controller) ? "controller" : "keyboard";
                if (!ports[port].contains(device_name)) {
                    continue;
                }
                const auto& actions = ports[port][device_name];
                for (int a = 0; a < action_count; a++) {
                    if (!actions.contains(action_names[a])) {
                        continue;
                    }
                    // Each action is one input, or a list of inputs for keyboard steering.
                    nlohmann::json list = actions[action_names[a]];
                    if (!list.is_array()) {
                        list = nlohmann::json::array({ list });
                    }
                    Slots slots{};
                    bool ok = true;
                    for (int s = 0; s < slots_per_action && s < (int)list.size(); s++) {
                        ok = ok && input_from_json(list[s], &slots[s]);
                    }
                    if (ok) {
                        bindings[port][device][a] = slots;
                    }
                }
            }
        }
    }
    catch (const std::exception& e) {
        printf("[Controls] Couldn't read %s: %s\n", save_path().string().c_str(), e.what());
    }
}

void rush2::controls::save() {
    nlohmann::json ports = nlohmann::json::array();
    {
        std::lock_guard lock{ bindings_mutex };
        for (int port = 0; port < num_ports; port++) {
            nlohmann::json p;
            for (int device = 0; device < device_count; device++) {
                nlohmann::json actions = nlohmann::json::object();
                for (int a = 0; a < action_count; a++) {
                    const Slots& slots = bindings[port][device][a];
                    if (slots[1].type == Input::Type::None) {
                        actions[action_names[a]] = input_to_json(slots[0]);
                    }
                    else {
                        actions[action_names[a]] = { input_to_json(slots[0]), input_to_json(slots[1]) };
                    }
                }
                p[device == static_cast<int>(Device::Controller) ? "controller" : "keyboard"] = actions;
            }
            ports.push_back(p);
        }
    }

    std::ofstream out{ save_path() };
    if (out) {
        out << nlohmann::json{ { "players", ports } }.dump(2) << "\n";
    }
}

void rush2::controls::snapshot() {
    std::lock_guard lock{ bindings_mutex };
    snapshot_bindings = bindings;
}

void rush2::controls::restore_snapshot() {
    std::lock_guard lock{ bindings_mutex };
    bindings = snapshot_bindings;
}

void rush2::controls::reset_defaults(int port, Device device) {
    std::lock_guard lock{ bindings_mutex };
    if (port >= 0 && port < num_ports) {
        bindings[port][static_cast<int>(device)] = default_bindings(device);
    }
}

Input rush2::controls::get_binding(int port, Device device, Action action, int slot) {
    std::lock_guard lock{ bindings_mutex };
    if (port < 0 || port >= num_ports || slot < 0 || slot >= slots_per_action) {
        return {};
    }
    return bindings[port][static_cast<int>(device)][static_cast<int>(action)][slot];
}

void rush2::controls::begin_listen(int port, Action action) {
    if (port < 0 || port >= num_ports) {
        return;
    }
    listens[port] = { true, true, action, 0, std::chrono::steady_clock::now() };
}

void rush2::controls::cancel_listen(int port) {
    if (port >= 0 && port < num_ports && listens[port].active) {
        listens[port].active = false;
        rush2::input::suppress_until_released(port);
    }
}

bool rush2::controls::is_listening(int port) {
    return port >= 0 && port < num_ports && listens[port].active;
}

int rush2::controls::listen_slot(int port) {
    return is_listening(port) ? listens[port].slot : 0;
}

bool rush2::controls::update_listen(int port) {
    if (!is_listening(port)) {
        return true;
    }
    Listen& l = listens[port];
    if (recompinput::game_input_disabled() || std::chrono::steady_clock::now() - l.start > listen_timeout) {
        cancel_listen(port);
        return true;
    }
    // The press that started listening (or bound the left steering key) has to be released first.
    if (l.waiting_release) {
        l.waiting_release = any_input_held(port);
        return false;
    }

    Input in;
    switch (scan_input(port, l.action, &in)) {
        case Scan::Nothing:
            return false;
        case Scan::Cancel:
            cancel_listen(port);
            return true;
        case Scan::Captured:
            break;
    }

    // Keyboard steering takes a left key and then a right key.
    bool keyboard_steering = l.action == Action::Steering && in.type == Input::Type::Key;
    assign(port, l.action, keyboard_steering ? l.slot : 0, in);
    if (!keyboard_steering) {
        std::lock_guard lock{ bindings_mutex };
        bindings[port][static_cast<int>(device_of(in))][static_cast<int>(l.action)][1] = {};
    }
    if (keyboard_steering && l.slot == 0) {
        l.slot = 1;
        l.waiting_release = true;
        l.start = std::chrono::steady_clock::now();
        return false;
    }
    l.active = false;
    rush2::input::suppress_until_released(port);
    return true;
}

bool rush2::controls::any_input_held(int port) {
    PortDevices d = get_devices(port);
    if (d.controller != nullptr) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            if (SDL_GameControllerGetButton(d.controller, (SDL_GameControllerButton)b)) {
                return true;
            }
        }
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
            if (std::fabs(axis_value(d.controller, a)) >= digital_threshold) {
                return true;
            }
        }
    }
    if (d.keys != nullptr) {
        for (int k = SDL_SCANCODE_A; k < d.num_keys; k++) {
            if (d.keys[k]) {
                return true;
            }
        }
    }
    return false;
}

void rush2::controls::get_race_input(int port, uint16_t* buttons_out, float* x_out, float* y_out, float steering_exponent,
                                     float* gas_out, float* brake_out) {
    PortBindings b;
    {
        std::lock_guard lock{ bindings_mutex };
        b = bindings[port];
    }
    PortDevices d = get_devices(port);

    uint16_t buttons = 0;
    for (int a = 0; a < action_count; a++) {
        if (a != static_cast<int>(Action::Steering) && a != static_cast<int>(Action::Gas) &&
            a != static_cast<int>(Action::Brake) && action_held(d, b, static_cast<Action>(a))) {
            buttons |= action_buttons[a];
        }
    }
    // Pedals press their button as soon as they leave the deadzone; the game then sees their pressure.
    float gas = pedal_value(d, b, Action::Gas);
    float brake = pedal_value(d, b, Action::Brake);
    buttons |= gas > 0.0f ? action_buttons[static_cast<int>(Action::Gas)] : 0;
    buttons |= brake > 0.0f ? action_buttons[static_cast<int>(Action::Brake)] : 0;
    *gas_out = gas;
    *brake_out = brake;

    if (button_down(d, SDL_CONTROLLER_BUTTON_START) || key_down(d, SDL_SCANCODE_RETURN)) {
        buttons |= n64_start;
    }

    float x = 0.0f;
    float y = 0.0f;
    int steer = steering_stick(b);
    if (d.controller != nullptr) {
        if (steer == LeftStick || steer == RightStick) {
            float sx, sy;
            stick_axes(d.controller, steer, &sx, &sy);
            recompinput::apply_joystick_deadzone(sx, sy, &sx, &sy);
            x += std::copysign(std::pow(std::fabs(sx), steering_exponent), sx);
            y += sy;
        }
        else if (steer == Dpad) {
            x += button_down(d, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) - button_down(d, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            y += button_down(d, SDL_CONTROLLER_BUTTON_DPAD_UP) - button_down(d, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        }

        // The D-pad still reaches the game when nothing is bound to it.
        bool dpad_bound = steer == Dpad;
        for (const Slots& slots : b[static_cast<int>(Device::Controller)]) {
            for (const Input& in : slots) {
                dpad_bound = dpad_bound || input_uses_stick(in, Dpad);
            }
        }
        if (!dpad_bound) {
            buttons |= button_down(d, SDL_CONTROLLER_BUTTON_DPAD_UP) ? n64_dpad_up : 0;
            buttons |= button_down(d, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ? n64_dpad_down : 0;
            buttons |= button_down(d, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? n64_dpad_left : 0;
            buttons |= button_down(d, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? n64_dpad_right : 0;
        }
    }
    if (d.keys != nullptr) {
        const Slots& keys = b[static_cast<int>(Device::Keyboard)][static_cast<int>(Action::Steering)];
        x += input_value(d, keys[1]) - input_value(d, keys[0]);
    }

    *buttons_out = buttons;
    *x_out = std::clamp(x, -1.0f, 1.0f);
    *y_out = std::clamp(y, -1.0f, 1.0f);
}

void rush2::controls::get_menu_input(int port, uint16_t* buttons_out, float* x_out, float* y_out) {
    PortDevices d = get_devices(port);
    uint16_t buttons = 0;
    float x = 0.0f;
    float y = 0.0f;

    if (d.controller != nullptr) {
        struct Map { SDL_GameControllerButton from; uint16_t to; };
        constexpr Map map[] = {
            { SDL_CONTROLLER_BUTTON_A, n64_a },
            { SDL_CONTROLLER_BUTTON_B, n64_b },
            { SDL_CONTROLLER_BUTTON_START, n64_start },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, n64_l },
            { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, n64_r },
            { SDL_CONTROLLER_BUTTON_Y, n64_c_up },
            { SDL_CONTROLLER_BUTTON_X, n64_c_left },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, n64_dpad_up },
            { SDL_CONTROLLER_BUTTON_DPAD_DOWN, n64_dpad_down },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, n64_dpad_left },
            { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, n64_dpad_right },
        };
        for (const Map& m : map) {
            buttons |= button_down(d, m.from) ? m.to : 0;
        }
        buttons |= axis_value(d.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >= digital_threshold ? n64_z : 0;
        buttons |= axis_value(d.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >= digital_threshold ? n64_r : 0;
        float rx = axis_value(d.controller, SDL_CONTROLLER_AXIS_RIGHTX);
        float ry = axis_value(d.controller, SDL_CONTROLLER_AXIS_RIGHTY);
        buttons |= rx <= -digital_threshold ? n64_c_left : 0;
        buttons |= rx >= digital_threshold ? n64_c_right : 0;
        buttons |= ry <= -digital_threshold ? n64_c_up : 0;
        buttons |= ry >= digital_threshold ? n64_c_down : 0;

        stick_axes(d.controller, LeftStick, &x, &y);
        recompinput::apply_joystick_deadzone(x, y, &x, &y);
    }

    if (d.keys != nullptr) {
        struct Map { SDL_Scancode from; uint16_t to; };
        constexpr Map map[] = {
            { SDL_SCANCODE_SPACE, n64_a },
            { SDL_SCANCODE_LSHIFT, n64_b },
            { SDL_SCANCODE_BACKSPACE, n64_b },
            { SDL_SCANCODE_RETURN, n64_start },
            { SDL_SCANCODE_Q, n64_l },
            { SDL_SCANCODE_E, n64_r },
            { SDL_SCANCODE_Z, n64_z },
            { SDL_SCANCODE_I, n64_c_up },
            { SDL_SCANCODE_K, n64_c_down },
            { SDL_SCANCODE_J, n64_c_left },
            { SDL_SCANCODE_L, n64_c_right },
            { SDL_SCANCODE_UP, n64_dpad_up },
            { SDL_SCANCODE_DOWN, n64_dpad_down },
            { SDL_SCANCODE_LEFT, n64_dpad_left },
            { SDL_SCANCODE_RIGHT, n64_dpad_right },
        };
        for (const Map& m : map) {
            buttons |= key_down(d, m.from) ? m.to : 0;
        }
        x += key_down(d, SDL_SCANCODE_D) - key_down(d, SDL_SCANCODE_A);
        y += key_down(d, SDL_SCANCODE_W) - key_down(d, SDL_SCANCODE_S);
    }

    *buttons_out = buttons;
    *x_out = std::clamp(x, -1.0f, 1.0f);
    *y_out = std::clamp(y, -1.0f, 1.0f);
}
