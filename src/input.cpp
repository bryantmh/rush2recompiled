// Per-port input for Rush 2.
//
// Rush 2 has two players, on N64 ports 1 and 2. The Players tab (src/players_tab.cpp) sets each port's controller to
// Auto, None or one specific controller, and picks the port the keyboard drives (port 1 by default):
// - A specific controller takes its port whenever it's connected, and is remembered across launches by its GUID and
//   serial number.
// - An Auto port is taken by the first controller that presses a button and isn't chosen for a port, and keeps it until
//   that controller disconnects. Controllers that never press anything (idle pads, virtual duplicates from Steam Input
//   or DS4Windows) never take a port.
// - None leaves the port without a controller (it can still have the keyboard).
// The frontend's own player assignment isn't used, and neither is its N64 binding profile: src/controls.cpp turns each
// port's devices into N64 input, with the player's bindings during races and a fixed layout in menus.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "json/json.hpp"
#include "recomp.h"
#include "librecomp/game.hpp"
#include "recompinput/recompinput.h"
#include "recompinput/input_state.h"
#include "recompui/config.h"

#include "rush2.h"

using rush2::input::num_ports;
using rush2::input::PortChoice;

namespace {
    constexpr SDL_JoystickID no_controller = -1;

    // Game addresses.
    // Game state: 0 = front menus, 1 = track select, 2 = car select, 7/10/9 = loading a race, 3 = racing. The game's
    // pause menu (func_800AFD44) only runs in states 3 and 10, so those are the ones that use the race layout.
    constexpr uint32_t game_mode = 0x8010C0D0;
    constexpr int32_t mode_racing = 3;
    constexpr int32_t mode_race_start = 10;
    constexpr uint32_t pause_state = 0x8002305C; // Nonzero while the pause menu (or one of its screens) is open.

    std::atomic<uint8_t*> game_rdram = nullptr;

    // Player assignments from the Players tab, guarded by players_mutex (read by the game thread, changed by the UI).
    std::mutex players_mutex;
    std::array<PortChoice, num_ports> port_choices{};
    int keyboard_port = 0;

    // Instance ID of the SDL controller driving each port. Written on the game thread, read by the VI and UI threads.
    std::array<std::atomic<SDL_JoystickID>, num_ports> port_controllers = { no_controller, no_controller };

    // Exponent applied to stick X before the game's steering. The game cubes the normalized stick X (func_80076694),
    // which was tuned for the stiff N64 stick; |x|^p with p < 1 cancels part (p = 1/3: all) of that curve.
    std::atomic<float> steering_exponent = 1.0f;

    std::atomic_bool rumble_active[num_ports] = {};
    std::array<float, num_ports> cur_rumble{}; // VI thread only.
    std::array<bool, num_ports> rumble_failed{}; // VI thread only.

    // Game thread only.
    struct PortState {
        bool last_race = false;
        uint16_t held_over = 0; // Buttons held across a menu/race switch, ignored until released.
    };
    std::array<PortState, num_ports> port_states{};
    std::array<std::atomic_bool, num_ports> blocked_until_release{};

    // Returns the controller for a joystick ID if it's still connected. The frontend never closes controllers, so the
    // pointer stays valid after a disconnect; it's just no longer returned here.
    SDL_GameController* get_connected_controller(SDL_JoystickID id) {
        if (id == no_controller) {
            return nullptr;
        }
        return recompinput::get_controller_from_joystick_id(id);
    }

    std::string controller_key(SDL_GameController* controller) {
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(controller)), guid, sizeof(guid));
        const char* serial = SDL_GameControllerGetSerial(controller);
        return std::string(guid) + ":" + (serial ? serial : "");
    }

    std::string key_guid(const std::string& key) {
        return key.substr(0, key.find(':'));
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

    void set_port_controller(int port, SDL_JoystickID id) {
        SDL_JoystickID old = port_controllers[port].load();
        if (old == id) {
            return;
        }
        if (old != no_controller) {
            printf("[Input] Controller %d left port %d\n", old, port + 1);
            rumble_active[port] = false;
        }
        port_controllers[port] = id;
        if (id != no_controller) {
            SDL_GameController* controller = get_connected_controller(id);
            printf("[Input] Controller %d (%s) joined port %d\n", id, controller ? SDL_GameControllerName(controller) : "?", port + 1);
        }
    }

    struct Connected {
        SDL_JoystickID id;
        SDL_GameController* controller;
        std::string key;
    };

    std::vector<Connected> connected_controllers() {
        std::vector<Connected> out;
        int num_joysticks = SDL_NumJoysticks();
        for (int i = 0; i < num_joysticks; i++) {
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
            SDL_GameController* controller = id < 0 ? nullptr : get_connected_controller(id);
            if (controller != nullptr) {
                out.push_back({ id, controller, controller_key(controller) });
            }
        }
        return out;
    }

    // The connected controller a port's choice refers to: an exact key match, or else one with the same GUID (some
    // controllers report no serial, or a different one over Bluetooth and USB).
    SDL_JoystickID find_chosen(const std::vector<Connected>& connected, const std::string& key, const std::array<SDL_JoystickID, num_ports>& taken) {
        auto free = [&](SDL_JoystickID id) { return std::find(taken.begin(), taken.end(), id) == taken.end(); };
        for (const Connected& c : connected) {
            if (c.key == key && free(c.id)) {
                return c.id;
            }
        }
        for (const Connected& c : connected) {
            if (key_guid(c.key) == key_guid(key) && free(c.id)) {
                return c.id;
            }
        }
        return no_controller;
    }

    void update_port_assignments() {
        std::array<PortChoice, num_ports> choices;
        {
            std::lock_guard lock{ players_mutex };
            choices = port_choices;
        }
        std::vector<Connected> connected = connected_controllers();

        // Free the ports of controllers that disconnected.
        for (int port = 0; port < num_ports; port++) {
            if (get_connected_controller(port_controllers[port].load()) == nullptr) {
                set_port_controller(port, no_controller);
            }
        }

        // Ports set to a specific controller or to None.
        std::array<SDL_JoystickID, num_ports> chosen;
        chosen.fill(no_controller);
        for (int port = 0; port < num_ports; port++) {
            if (choices[port].kind == PortChoice::Kind::Controller) {
                chosen[port] = find_chosen(connected, choices[port].controller_key, chosen);
            }
        }
        for (int port = 0; port < num_ports; port++) {
            if (choices[port].kind != PortChoice::Kind::Auto) {
                set_port_controller(port, chosen[port]);
            }
        }
        // An Auto port gives up a controller that's now chosen for another port.
        for (int port = 0; port < num_ports; port++) {
            SDL_JoystickID id = port_controllers[port].load();
            if (choices[port].kind == PortChoice::Kind::Auto && std::find(chosen.begin(), chosen.end(), id) != chosen.end()) {
                set_port_controller(port, no_controller);
            }
        }

        // Presses made to navigate the menus shouldn't claim a port.
        if (recompinput::game_input_disabled()) {
            return;
        }

        // Auto ports: the first free controller to press something. Controllers chosen for a port by key never join
        // another port.
        for (const Connected& c : connected) {
            bool reserved = get_port_for_controller(c.id) >= 0;
            for (int port = 0; port < num_ports; port++) {
                reserved = reserved || (choices[port].kind == PortChoice::Kind::Controller && choices[port].controller_key == c.key);
            }
            if (reserved || !controller_pressing_anything(c.controller)) {
                continue;
            }
            for (int port = 0; port < num_ports; port++) {
                if (choices[port].kind == PortChoice::Kind::Auto && port_controllers[port].load() == no_controller) {
                    set_port_controller(port, c.id);
                    break;
                }
            }
        }
    }

    bool in_race() {
        uint8_t* rdram = game_rdram.load();
        if (rdram == nullptr) {
            return false;
        }
        int32_t mode = MEM_W(0, (int32_t)game_mode);
        return (mode == mode_racing || mode == mode_race_start) && MEM_B(0, (int32_t)pause_state) == 0;
    }

    std::filesystem::path save_path() {
        return recomp::get_config_path() / "players.json";
    }

    void save_players() {
        nlohmann::json ports = nlohmann::json::array();
        int keyboard;
        {
            std::lock_guard lock{ players_mutex };
            for (const PortChoice& choice : port_choices) {
                switch (choice.kind) {
                    case PortChoice::Kind::Auto:
                        ports.push_back({ { "controller", "auto" } });
                        break;
                    case PortChoice::Kind::None:
                        ports.push_back({ { "controller", "none" } });
                        break;
                    case PortChoice::Kind::Controller:
                        ports.push_back({ { "controller", { { "key", choice.controller_key }, { "name", choice.controller_name } } } });
                        break;
                }
            }
            keyboard = keyboard_port;
        }
        std::ofstream out{ save_path() };
        if (out) {
            // The keyboard is stored as a player number, 0 for none.
            out << nlohmann::json{ { "players", ports }, { "keyboard", keyboard + 1 } }.dump(2) << "\n";
        }
    }
}

void rush2::input::load_players() {
    std::ifstream in{ save_path() };
    if (!in) {
        return;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(in);
        std::lock_guard lock{ players_mutex };
        const auto& ports = j.at("players");
        for (int port = 0; port < num_ports && port < (int)ports.size(); port++) {
            const auto& c = ports[port].at("controller");
            PortChoice choice{};
            if (c.is_object()) {
                choice.kind = PortChoice::Kind::Controller;
                choice.controller_key = c.at("key").get<std::string>();
                choice.controller_name = c.value("name", "");
            }
            else if (c == "none") {
                choice.kind = PortChoice::Kind::None;
            }
            port_choices[port] = choice;
        }
        int keyboard = j.value("keyboard", 1) - 1;
        keyboard_port = (keyboard >= 0 && keyboard < num_ports) ? keyboard : -1;
    }
    catch (const std::exception& e) {
        printf("[Input] Couldn't read %s: %s\n", save_path().string().c_str(), e.what());
    }
}

std::vector<rush2::input::ControllerInfo> rush2::input::get_controllers() {
    std::vector<ControllerInfo> out;
    for (const Connected& c : connected_controllers()) {
        SDL_GameControllerType type = SDL_GameControllerGetType(c.controller);
        const char* name = SDL_GameControllerName(c.controller);
        out.push_back({ c.id, name ? name : "Controller", c.key,
            type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4 || type == SDL_CONTROLLER_TYPE_PS5 });
    }
    return out;
}

PortChoice rush2::input::get_port_choice(int port) {
    std::lock_guard lock{ players_mutex };
    return (port >= 0 && port < num_ports) ? port_choices[port] : PortChoice{};
}

void rush2::input::set_port_choice(int port, const PortChoice& choice) {
    if (port < 0 || port >= num_ports) {
        return;
    }
    {
        std::lock_guard lock{ players_mutex };
        // Choosing another port's controller swaps the two ports' choices.
        if (choice.kind == PortChoice::Kind::Controller) {
            for (int other = 0; other < num_ports; other++) {
                if (other != port && port_choices[other].kind == PortChoice::Kind::Controller &&
                    port_choices[other].controller_key == choice.controller_key) {
                    port_choices[other] = port_choices[port];
                }
            }
        }
        port_choices[port] = choice;
    }
    save_players();
}

int32_t rush2::input::get_port_controller(int port) {
    return (port >= 0 && port < num_ports) ? port_controllers[port].load() : no_controller;
}

int rush2::input::get_keyboard_port() {
    std::lock_guard lock{ players_mutex };
    return keyboard_port;
}

void rush2::input::set_keyboard_port(int port) {
    {
        std::lock_guard lock{ players_mutex };
        keyboard_port = (port >= 0 && port < num_ports) ? port : -1;
    }
    save_players();
}

bool rush2::input::port_has_playstation_controller(int port) {
    SDL_GameController* controller = get_connected_controller(get_port_controller(port));
    if (controller == nullptr) {
        return false;
    }
    SDL_GameControllerType type = SDL_GameControllerGetType(controller);
    return type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4 || type == SDL_CONTROLLER_TYPE_PS5;
}

void rush2::input::suppress_until_released(int port) {
    if (port >= 0 && port < num_ports) {
        blocked_until_release[port] = true;
    }
}

void rush2::input::set_rdram(uint8_t* rdram) {
    game_rdram = rdram;
}

void rush2::input::poll() {
    recompinput::poll_inputs();
    update_port_assignments();
}

bool rush2::input::is_port_connected(int port) {
    // Port 1 is always connected so the game always has a player 1.
    return port == 0 || (port > 0 && port < num_ports && (port_controllers[port].load() != no_controller || get_keyboard_port() == port));
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

    // While the Controller Setup screen listens for a binding, and after it binds one until everything is let go, the
    // game sees nothing from the port.
    if (rush2::controls::is_listening(port)) {
        return true;
    }
    if (blocked_until_release[port]) {
        if (rush2::controls::any_input_held(port)) {
            return true;
        }
        blocked_until_release[port] = false;
    }

    uint16_t buttons;
    float x, y;
    bool race = in_race();
    if (race) {
        rush2::controls::get_race_input(port, &buttons, &x, &y, steering_exponent.load());
    }
    else {
        rush2::controls::get_menu_input(port, &buttons, &x, &y);
    }

    // A button held while switching between the menu and race layouts (Start to pause, A to resume) stays released
    // until it's let go, so it doesn't press whatever it's bound to in the other layout.
    PortState& state = port_states[port];
    if (race != state.last_race) {
        state.held_over = buttons;
        state.last_race = race;
    }
    state.held_over &= buttons;
    buttons &= ~state.held_over;

    *buttons_out = buttons;
    *x_out = x;
    *y_out = y;
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
