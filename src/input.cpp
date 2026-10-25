// Per-port input for Rush 2.
//
// Rush 2 has up to four players (two in the original game, src/players4.cpp), on N64 ports 1-4. Which port a device
// drives isn't chosen: the game makes whoever presses START a player (player 1 on the title screen, players 2-4 on
// Select Player), so a device only needs a port to press START on. The Controls tab (src/controls_tab.cpp) enables or
// disables each controller and the keyboard (all enabled by default):
// - An enabled controller takes the first free port when it presses a button, and keeps it until it disconnects or is
//   disabled. Controllers that never press anything (idle pads, virtual duplicates from Steam Input or DS4Windows)
//   never take a port, and disabled ones never do. Disabled controllers are remembered across launches by their GUID
//   and serial number.
// - The keyboard is a controller like the others: it takes the first free port when one of its keys is pressed.
// The frontend's own player assignment isn't used, and neither is its N64 binding profile: src/controls.cpp turns each
// port's devices into N64 input, with the player's bindings during races and a fixed layout in menus.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "json/json.hpp"

#include "control_profiles.h"
#include "data_files.h"
#include "dualsense_haptics.h"
#include "recomp.h"
#include "librecomp/game.hpp"
#include "recompinput/recompinput.h"
#include "recompinput/input_state.h"
#include "recompui/config.h"

#include "rush2.h"
#include "game_filter.h"
#include "online.h"
#include "rush2_hooks.h"
#include "steam_haptics.h"
#include "steam_input_filter.h"

using rush2::input::num_ports;

namespace {
    constexpr SDL_JoystickID no_controller = -1;

    // Game addresses.
    // Game state: 0 = front menus, 1 = track select, 2 = car select, 7 = loading a race, 10 = race intro, 9 = countdown
    // (set when the intro timer runs out, func_800AE7xx), 3 = racing. 10, 9 and 3 use the race layout, so gas held
    // through the countdown keeps working when the race starts. The pause menu (func_800AFD44) only runs in 3 and 10.
    constexpr uint32_t game_mode = 0x8010C0D0;
    constexpr int32_t mode_racing = 3;
    constexpr int32_t mode_race_start = 10;
    constexpr int32_t mode_countdown = 9;
    // GAS (A) and BRAKE (B) in the race layout. They're held, not pressed, so holding them into a race is fine.
    constexpr uint16_t race_held_buttons = 0x8000 | 0x4000;
    constexpr uint32_t pause_state = 0x8002305C; // Nonzero while the pause menu (or one of its screens) is open.

    std::atomic<uint8_t*> game_rdram = nullptr;

    // The Controls tab's controller settings, guarded by players_mutex (read by the game thread, changed by the UI).
    std::mutex players_mutex;
    std::vector<rush2::input::DisabledController> disabled_controllers;
    bool keyboard_enabled = true;

    // The port the keyboard drives (-1 until it presses something). Written on the game thread (and by the UI when
    // it's turned off), read everywhere.
    std::atomic<int> keyboard_port = -1;

    // Instance ID of the SDL controller driving each port. Written on the game thread, read by the VI and UI threads.
    std::array<std::atomic<SDL_JoystickID>, num_ports> port_controllers = { no_controller, no_controller, no_controller,
                                                                            no_controller };

    // Steering response, reverse, the stick deadzone and every rumble setting are per profile: each port reads its
    // player's (include/control_profiles.h). Steering response is an exponent applied to stick X before the game's
    // steering: the game cubes the normalized stick X (func_80076694), tuned for the stiff N64 stick, and |x|^p with
    // p < 1 cancels part (p = 1/3: all) of that curve. Reverse Hold presses GAS with REVERSE (C-down in the race
    // layout) instead of only selecting the reverse gear.
    std::atomic_bool any_port_directional = false; // Set by the VI thread: gates the impact hook (src/rumble.cpp).
    std::atomic_bool rumble_debug = false; // Prints Enhanced's impacts and levels (--debug-rumble).
    std::atomic_bool rumble_muted = false; // Test runs: no rumble or haptics at all (set_rumble_muted).

    // Rumble state is kept per slot: a port's, or test_slot's, which plays the Controls tab's rumble tests on the last
    // controller used when it isn't driving a port (update_rumble).
    constexpr int test_slot = num_ports;
    constexpr int num_slots = num_ports + 1;

    // The controller last used for anything, menus included (update_port_assignments): the rumble tests play on it.
    std::atomic<SDL_JoystickID> last_used_controller = no_controller;

    std::atomic_bool rumble_active[num_slots] = {};
    // The latest impact on a port's car, set by the game thread from the hook in src/rumble.cpp and taken by the VI
    // thread, packed in one word so the side and hardness always travel together: the side's float bits (-1 right to
    // 1 left) in the low 32 bits, then impact_pending and impact_hard.
    constexpr uint64_t impact_pending = 1ull << 32;
    constexpr uint64_t impact_hard = 1ull << 33;
    std::atomic<uint64_t> rumble_impact[num_slots] = {};
    // Set by the VI thread outside a race (or out of Enhanced) so the hook drops its previous tick's acceleration
    // instead of comparing the next race's first tick with the last one's.
    std::atomic_bool rumble_impact_reset[num_slots] = {};
    std::array<float, num_slots> cur_rumble{}; // VI thread only.
    std::array<float, num_slots> rumble_side{}; // VI thread only. Side of the last impact, fading out.
    std::array<int, num_slots> rumble_hold{}; // VI thread only. Frames left of the minimum rumble after a hit.
    // VI thread only. Frames since a hit while the game hasn't rumbled for it yet, or -1 once it has (or with no hit).
    std::array<int, num_slots> rumble_unconfirmed = [] { std::array<int, num_slots> a; a.fill(-1); return a; }();
    std::array<int, num_slots> rumble_log_frames{}; // VI thread only. Counts frames between --debug-rumble lines.
    // VI thread only. Whether the slot's controller plays on its actuators (dualsense_haptics.h, steam_haptics.h) instead
    // of SDL rumble, and the device path and kind (Steam tones or DualSense) it plays on, to silence it when it stops.
    std::array<bool, num_slots> rumble_on_haptics{};
    std::array<std::string, num_slots> rumble_haptics_path{};
    std::array<bool, num_slots> rumble_haptics_steam{};
    // VI thread only: the controller the test slot played on last frame, so it's stopped once another plays there.
    SDL_JoystickID test_slot_controller = no_controller;
    // VI thread only: frames until a slot whose rumble call failed tries again (a controller can refuse while it
    // reconnects or switches driver).
    constexpr int rumble_retry_frames = 120;
    std::array<int, num_slots> rumble_retry{};
    // VI thread only. Whether the port's trigger motors were last set non-zero, so leaving them stops them.
    std::array<bool, num_slots> rumble_triggers_on{};
    // The cornering side, set every physics tick by src/rumble.cpp (set_rumble_cornering).
    std::atomic<float> rumble_cornering[num_slots] = {};
    // Wall grinding's side and intensity, set every physics tick by src/rumble.cpp (set_rumble_grinding).
    std::atomic<float> rumble_grind_side[num_slots] = {};
    std::atomic<float> rumble_grind_intensity[num_slots] = {};
    // VI thread only: the grinding's smoothed level and side.
    std::array<float, num_slots> grind_level{};
    std::array<float, num_slots> grind_side{};
    // VI thread only, Enhanced: the impact thump's level (1 at a hard hit, fading) and side, frames
    // since the last hard hit, and the smoothed cornering side the screech plays on.
    std::array<float, num_slots> impact_level{};
    std::array<float, num_slots> impact_side{};
    std::array<int, num_slots> frames_since_impact{};
    std::array<float, num_slots> screech_side{};

    // Rumble Mode > Enhanced tuning. A frame is one update_rumble call, once per VI (60 a second).

    // The game's own rumble after a hit lasts only a few frames, too short to feel which side is on (a grip motor is
    // slow to spin up), so a hard hit (wall, car) keeps rumbling for at least this long. Soft ones, from the tires
    // sliding, only set the side: the game already rumbles for those, and holding them made slides rumble on.
    constexpr int soft_hit_hold_frames = 0;
    constexpr int hard_hit_hold_frames = 20; // Keep this above the soft hold: it's how a hard hit is told apart.

    // Once the hold is over, how much of the hit's side is left each frame. 0.95 takes about half a second to fade.
    constexpr float side_fade = 0.95f;

    // How much the far side of a hit is cut by a hit square on to that side (a side of 1) is the profile's Far Side
    // Cut: 100% turns it off, 0 leaves it at the full level. Hits at an angle are cut by less, head-on ones not at all.

    // A hit only holds the rumble if the game rumbles for it within this many frames, so Enhanced never rumbles
    // when the game wouldn't (its Rumble Pak off, or a jolt under its lowest threshold).
    constexpr int hit_confirm_frames = 8;

    // Rumble Effects > Distinct tuning.
    // How much of the impact thump is left each frame: 0.86 fades it out in about a quarter of a second.
    constexpr float impact_fade = 0.86f;
    // The game's rumble right after a hard hit is the hit's own, so it's only played as tire screech this many frames
    // after the last one.
    constexpr int screech_after_impact_frames = 12;
    // Each effect's loudness is the profile's Crash Rumble and Road Rumble (control_profiles.h: crash, screech and grind
    // strength). Rumble motors and triggers play them like the DualSense's voices, as steady levels of different weight
    // rather than rhythms (pulsing the screech on and off and jumping the grind around felt like stuttering):
    // - the screech is a light steady rumble, motor_screech_gain of its level, following the game's rumble as closely
    //   as the DualSense's buzz does (smoothing it made it drag on after the slide),
    constexpr float motor_screech_gain = 0.35f;
    // - a crash is a heavy hit on both motors (the light one at motor_hit_light_gain) fading with the thump, and a kick
    //   on the hit side's trigger that dies faster than the burst (the thump's level to this power),
    constexpr float motor_hit_light_gain = 0.8f;
    constexpr float trigger_kick_power = 2.0f;
    // - grinding along a wall is the heaviest steady rumble: its level times motor_grind_gain (past 1 it saturates the
    //   motor at full grinding), the light motor at motor_grind_light_gain of the heavy one's.
    constexpr float motor_grind_gain = 1.25f;
    constexpr float motor_grind_light_gain = 0.7f;
    // A matched pair (a Steam Deck's) has no triggers to carry the hit's kick, and each side's motor is all of that
    // side's effects, so full grinding left a hit nothing to rise to: hits along a wall (most of them) weren't felt.
    // There the steady effects (screech and grind) stop at pair_steady_max and the thump plays on top of them.
    constexpr float pair_steady_max = 0.6f;
    // Xbox 360 pads (and wheels and adapters that pose as one, which often only react to the heavy motor): the
    // screech plays on the heavy motor too, at this much of the light one's level.
    constexpr float xbox360_screech_heavy_gain = 1.0f;
    // Xbox triggers: loudness of each effect on its side's trigger.
    constexpr float trigger_kick_gain = 1.0f;
    constexpr float trigger_screech_gain = 0.6f;
    constexpr float trigger_grind_gain = 1.0f;
    // How fast the screech's side follows the cornering side each frame (1 = at once).
    constexpr float screech_side_follow = 0.2f;
    // Wall grinding: how fast its level rises to the grinding's intensity and falls once the car leaves the wall each
    // frame (physics ticks don't line up with frames, so a frame can miss a tick of contact).
    constexpr float grind_attack = 0.5f;
    constexpr float grind_release = 0.15f;

    // Under this much side a hit counts as head on in the --debug-rumble lines.
    constexpr float head_on_side = 0.05f;

    // Frames between the console's rumble level lines (--debug-rumble) while Enhanced is rumbling.
    constexpr int rumble_log_interval_frames = 6;

    // Game thread only.
    struct PortState {
        bool last_race = false;
        uint16_t held_over = 0; // Buttons held across a menu/race switch, ignored until released.
    };
    std::array<PortState, num_ports> port_states{};
    std::array<std::atomic_bool, num_ports> blocked_until_release{};

    // How far GAS and BRAKE were pressed in the input last given to the game (0 when their buttons weren't sent).
    // Written by get_n64_input (the game's controller reads and the online race's own read), read by the pedal hook and
    // the online race (get_pedals).
    std::array<std::atomic<float>, num_ports> pedal_gas{};
    std::array<std::atomic<float>, num_ports> pedal_brake{};

    // The game's pedal function (func_80076854) and its player and car structs.
    constexpr uint16_t n64_a = 0x8000;
    constexpr uint16_t n64_b = 0x4000;
    constexpr uint16_t n64_c_down = 0x0004;
    constexpr int player_gas_mask = 0xC;
    constexpr int player_brake_mask = 0xE;
    constexpr int car_throttle = 0x734;
    constexpr int car_brake = 0x730;
    constexpr uint32_t brake_disabled = 0x80125C64; // Nonzero: the game holds the brake at 0.

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
            if (controller != nullptr) {
                printf("[Input] Controller %d (%s, %04X:%04X, type %d, rumble %d, trigger rumble %d) joined port %d\n", id,
                    SDL_GameControllerName(controller), SDL_GameControllerGetVendor(controller),
                    SDL_GameControllerGetProduct(controller), (int)SDL_GameControllerGetType(controller),
                    (int)SDL_GameControllerHasRumble(controller), (int)SDL_GameControllerHasRumbleTriggers(controller),
                    port + 1);
            }
            else {
                printf("[Input] Controller %d (?) joined port %d\n", id, port + 1);
            }
        }
    }

    struct Connected {
        SDL_JoystickID id;
        SDL_GameController* controller;
        std::string key;
    };

    std::vector<Connected> connected_controllers() {
        std::lock_guard lock{ rush2::input::sdl_device_mutex() };
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

    std::string key_serial(const std::string& key) {
        size_t colon = key.find(':');
        return colon == std::string::npos ? std::string() : key.substr(colon + 1);
    }

    // The connected controllers that are disabled in the Controls tab: by their exact key, or else by serial number and
    // vendor and product (the GUID's checksum and driver bytes change when SDL opens the controller through another
    // driver: RawInput, XInput, HIDAPI). Controllers without a serial only match exactly, so disabling one of two
    // identical controllers doesn't also disable the other.
    std::vector<SDL_JoystickID> find_disabled(const std::vector<Connected>& connected,
                                              const std::vector<rush2::input::DisabledController>& disabled) {
        std::vector<SDL_JoystickID> out;
        auto product = [](const std::string& key) {
            std::string guid = key_guid(key);
            return guid.size() >= 24 ? guid.substr(8, 16) : guid;
        };
        for (const Connected& c : connected) {
            for (const auto& d : disabled) {
                bool same_serial = !key_serial(d.key).empty() && key_serial(d.key) == key_serial(c.key) &&
                    product(d.key) == product(c.key);
                if (c.key == d.key || same_serial) {
                    out.push_back(c.id);
                    break;
                }
            }
        }
        return out;
    }

    // Keys that count as the keyboard joining: the menu layout's keys (src/controls.cpp).
    bool keyboard_pressing_anything() {
        int num_keys = 0;
        const Uint8* keys = SDL_GetKeyboardState(&num_keys);
        constexpr SDL_Scancode join_keys[] = { SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_BACKSPACE,
            SDL_SCANCODE_RETURN, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT };
        for (SDL_Scancode key : join_keys) {
            if (key < num_keys && keys[key]) {
                return true;
            }
        }
        return false;
    }

    // With Background Input off a window without focus leaves the controllers alone: SDL drops their presses, and this
    // game sends them nothing either (rumble, trigger rumble, DualSense and Steam haptics), claims no port and reads
    // no input. Otherwise two games on one machine fought over the same controller: the unfocused one's race kept
    // writing its rumble and haptics to the controller the focused one was driving with.
    bool hands_off() {
        return !recompui::config::general::get_background_input_mode_enabled() && SDL_GetKeyboardFocus() == nullptr;
    }

    // Whether a port has a device: a controller or the keyboard.
    bool port_taken(int port) {
        return port_controllers[port].load() != no_controller || keyboard_port.load() == port;
    }

    // What the Controls tab shows, as of the input thread's last poll: the menu draws on another thread, so it reads
    // this instead of calling SDL (see sdl_device_mutex).
    std::mutex snapshot_mutex;
    std::vector<rush2::input::ControllerInfo> snapshot_controllers; // Under snapshot_mutex.
    std::vector<SDL_JoystickID> snapshot_off;                       // Under snapshot_mutex.

    void update_snapshot(const std::vector<Connected>& connected, const std::vector<SDL_JoystickID>& off) {
        std::vector<rush2::input::ControllerInfo> out;
        for (const Connected& c : connected) {
            SDL_GameControllerType type = SDL_GameControllerGetType(c.controller);
            const char* name = SDL_GameControllerName(c.controller);
            out.push_back({ c.id, name ? name : "Controller", c.key,
                type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4 || type == SDL_CONTROLLER_TYPE_PS5,
                type == SDL_CONTROLLER_TYPE_PS5 });
        }
        std::lock_guard lock{ snapshot_mutex };
        snapshot_controllers = std::move(out);
        snapshot_off = off;
    }

    void update_port_assignments() {
        // Skipped while the main thread holds the lock: it holds it while it pumps the window's events, which goes on
        // for as long as the window is dragged or resized (Windows runs its own message loop inside the pump), and the
        // game's frame waits on the controller read that runs this. The ports stay as they are until the next frame.
        std::unique_lock device_lock{ rush2::input::sdl_device_mutex(), std::try_to_lock };
        if (!device_lock.owns_lock()) return;
        std::vector<rush2::input::DisabledController> disabled;
        bool keyboard_on;
        {
            std::lock_guard lock{ players_mutex };
            disabled = disabled_controllers;
            keyboard_on = keyboard_enabled;
        }
        std::vector<Connected> connected = connected_controllers();
        std::vector<SDL_JoystickID> off = find_disabled(connected, disabled);
        update_snapshot(connected, off);

        // A Steam Controller or Steam Deck (not Steam Input's virtual pad): Steam types keys for it too.
        bool steam_controller = false;
        for (const Connected& c : connected) {
            steam_controller |= SDL_GameControllerGetVendor(c.controller) == 0x28DE &&
                SDL_GameControllerGetProduct(c.controller) != 0x11FF;
        }
        rush2::steam_input_filter::set_steam_controller_connected(steam_controller);

        // Free the ports of controllers that disconnected or were disabled.
        for (int port = 0; port < num_ports; port++) {
            SDL_JoystickID id = port_controllers[port].load();
            if (get_connected_controller(id) == nullptr || std::find(off.begin(), off.end(), id) != off.end()) {
                set_port_controller(port, no_controller);
            }
        }

        if (!keyboard_on) {
            keyboard_port = -1;
        }

        for (const Connected& c : connected) {
            if (std::find(off.begin(), off.end(), c.id) == off.end() && controller_pressing_anything(c.controller)) {
                last_used_controller = c.id;
                break;
            }
        }

        // Presses made to navigate the menus shouldn't claim a port, nor those made in another window.
        if (recompinput::game_input_disabled() || hands_off()) {
            return;
        }

        auto first_free = [&]() {
            for (int port = 0; port < num_ports; port++) {
                if (!port_taken(port)) {
                    return port;
                }
            }
            return -1;
        };
        for (const Connected& c : connected) {
            if (get_port_for_controller(c.id) >= 0 || std::find(off.begin(), off.end(), c.id) != off.end() ||
                !controller_pressing_anything(c.controller)) {
                continue;
            }
            int port = first_free();
            if (port >= 0) {
                set_port_controller(port, c.id);
            }
        }
        if (keyboard_on && keyboard_port.load() < 0 && keyboard_pressing_anything()) {
            int port = first_free();
            if (port >= 0) {
                keyboard_port = port;
                printf("[Input] The keyboard joined port %d\n", port + 1);
            }
        }
    }

    bool in_race() {
        uint8_t* rdram = game_rdram.load();
        if (rdram == nullptr) {
            return false;
        }
        int32_t mode = MEM_W(0, (int32_t)game_mode);
        return (mode == mode_racing || mode == mode_race_start || mode == mode_countdown) && MEM_B(0, (int32_t)pause_state) == 0;
    }

    // players.json's section (include/data_files.h).
    const std::string players_section = "controllers";

    void save_players() {
        nlohmann::json disabled = nlohmann::json::array();
        nlohmann::json keyboard;
        {
            std::lock_guard lock{ players_mutex };
            for (const auto& c : disabled_controllers) {
                disabled.push_back({ { "key", c.key }, { "name", c.name } });
            }
            keyboard = { { "enabled", keyboard_enabled } };
        }
        rush2::data_files::write(rush2::data_files::File::Players, players_section,
            nlohmann::json{ { "disabled", disabled }, { "keyboard", keyboard } }.dump());
    }
}

void rush2::input::load_players() {
    std::string text = rush2::data_files::read(rush2::data_files::File::Players, players_section);
    if (text.empty()) {
        return;
    }
    bool migrated = false;
    try {
        nlohmann::json j = nlohmann::json::parse(text);
        std::lock_guard lock{ players_mutex };
        if (j.contains("players")) {
            // Older versions pinned controllers to players ("players": a choice per port, "keyboard": the keyboard's
            // player number or 0). Controllers now take whatever port is free, so only the keyboard's setting carries
            // over: on any player it's enabled, and 0 turns it off.
            keyboard_enabled = j.value("keyboard", 1) != 0;
            migrated = true;
        }
        else {
            for (const auto& c : j.value("disabled", nlohmann::json::array())) {
                disabled_controllers.push_back({ c.value("key", ""), c.value("name", "") });
            }
            const auto& keyboard = j.value("keyboard", nlohmann::json::object());
            keyboard_enabled = keyboard.value("enabled", true);
        }
    }
    catch (const std::exception& e) {
        printf("[Input] Couldn't read the players' controllers: %s\n", e.what());
    }
    if (migrated) {
        save_players();
    }
}

// sdl2-compat keeps one joystick list with no lock ("this will break if you multithread it"): SDL_NumJoysticks and the
// event pump (on a controller connecting) free and replace it, and index calls read it. Two threads doing that at once
// corrupt the heap (crashes in SDL3's free under SDL_PollEvent or connected_controllers). Everything that enumerates
// controllers or pumps events holds this.
std::recursive_mutex& rush2::input::sdl_device_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

std::vector<rush2::input::ControllerInfo> rush2::input::get_controllers() {
    std::lock_guard lock{ snapshot_mutex };
    return snapshot_controllers;
}

std::vector<rush2::input::DisabledController> rush2::input::get_disabled_controllers() {
    std::lock_guard lock{ players_mutex };
    return disabled_controllers;
}

void rush2::input::set_controller_enabled(const std::string& key, const std::string& name, bool enabled) {
    {
        std::lock_guard lock{ players_mutex };
        auto it = std::find_if(disabled_controllers.begin(), disabled_controllers.end(),
                               [&](const DisabledController& c) { return c.key == key; });
        if (enabled && it != disabled_controllers.end()) {
            disabled_controllers.erase(it);
        }
        else if (!enabled && it == disabled_controllers.end()) {
            disabled_controllers.push_back({ key, name });
        }
    }
    save_players();
}

bool rush2::input::is_controller_enabled(int32_t joystick_id) {
    std::lock_guard lock{ snapshot_mutex };
    return std::find(snapshot_off.begin(), snapshot_off.end(), (SDL_JoystickID)joystick_id) == snapshot_off.end();
}

int32_t rush2::input::get_port_controller(int port) {
    return (port >= 0 && port < num_ports) ? port_controllers[port].load() : no_controller;
}

// RUSH2_TEST_VPAD's virtual controller, once attached (start_test_vpad).
static std::atomic<int32_t> test_vpad_id{ -1 };

bool rush2::input::is_test_vpad_port(int port) {
    int32_t id = get_port_controller(port);
    return id != no_controller && id == test_vpad_id.load();
}

int rush2::input::get_keyboard_port() {
    return keyboard_port.load();
}

bool rush2::input::get_keyboard_enabled() {
    std::lock_guard lock{ players_mutex };
    return keyboard_enabled;
}

void rush2::input::set_keyboard_enabled(bool enabled) {
    {
        std::lock_guard lock{ players_mutex };
        keyboard_enabled = enabled;
    }
    if (!enabled) {
        keyboard_port = -1;
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

bool rush2::input::takes_input() {
    return !recompinput::game_input_disabled() && !hands_off();
}

void rush2::input::suppress_until_released(int port) {
    if (port >= 0 && port < num_ports) {
        blocked_until_release[port] = true;
    }
}

void rush2::input::set_rdram(uint8_t* rdram) {
    game_rdram = rdram;
    rush2::control_profiles::update_ports(rdram);
}

// Test aid: RUSH2_TEST_VPAD="<seconds>:<input>[:<hold seconds>],..." attaches a virtual game controller (SDL's) and
// presses its inputs at those times after the first input poll (about 4 s after launch, so 4 s later than an
// --input-script press of the same time): A B X Y BACK START LS RS LB RB UP DOWN LEFT RIGHT (buttons), RT LT
// (the triggers, fully) and SL SR SU SD (the left stick fully left, right, up or down); a press holds 0.15 s unless given. Unlike
// --input-script it goes through everything a real controller does: claiming a port, its bindings and control
// profile, SDL's controller reads, and SDL's focus rule (with Background Input off an unfocused window sees none of
// it). A thread of its own sets the pad's state, so it moves whatever the game's threads are doing. A simulation run
// (RUSH2_SIM) reads the port it claims, though it ignores every real controller.
namespace {
    void start_test_vpad() {
        const char* spec = std::getenv("RUSH2_TEST_VPAD");
        if (spec == nullptr) {
            return;
        }
        struct Press {
            double time;
            double duration;
            int button = -1;
            int axis = -1;
            int16_t value = 0;
        };
        static const std::pair<const char*, int> button_names[] = {
            { "A", SDL_CONTROLLER_BUTTON_A }, { "B", SDL_CONTROLLER_BUTTON_B }, { "X", SDL_CONTROLLER_BUTTON_X },
            { "Y", SDL_CONTROLLER_BUTTON_Y }, { "BACK", SDL_CONTROLLER_BUTTON_BACK },
            { "START", SDL_CONTROLLER_BUTTON_START }, { "LS", SDL_CONTROLLER_BUTTON_LEFTSTICK },
            { "RS", SDL_CONTROLLER_BUTTON_RIGHTSTICK }, { "LB", SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
            { "RB", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER }, { "UP", SDL_CONTROLLER_BUTTON_DPAD_UP },
            { "DOWN", SDL_CONTROLLER_BUTTON_DPAD_DOWN }, { "LEFT", SDL_CONTROLLER_BUTTON_DPAD_LEFT },
            { "RIGHT", SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
        };
        std::vector<Press> presses;
        std::string text = spec;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find(',', pos);
            std::string entry = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            size_t c1 = entry.find(':');
            if (c1 == std::string::npos) {
                continue;
            }
            Press p{ std::atof(entry.c_str()), 0.15 };
            std::string name = entry.substr(c1 + 1);
            size_t c2 = name.find(':');
            if (c2 != std::string::npos) {
                p.duration = std::atof(name.c_str() + c2 + 1);
                name = name.substr(0, c2);
            }
            for (const auto& [n, b] : button_names) {
                if (name == n) p.button = b;
            }
            if (name == "RT") { p.axis = SDL_CONTROLLER_AXIS_TRIGGERRIGHT; p.value = 32767; }
            if (name == "LT") { p.axis = SDL_CONTROLLER_AXIS_TRIGGERLEFT; p.value = 32767; }
            if (name == "SL") { p.axis = SDL_CONTROLLER_AXIS_LEFTX; p.value = -32768; }
            if (name == "SR") { p.axis = SDL_CONTROLLER_AXIS_LEFTX; p.value = 32767; }
            if (name == "SU") { p.axis = SDL_CONTROLLER_AXIS_LEFTY; p.value = -32768; }
            if (name == "SD") { p.axis = SDL_CONTROLLER_AXIS_LEFTY; p.value = 32767; }
            if (p.button >= 0 || p.axis >= 0) {
                presses.push_back(p);
            }
        }
        SDL_VirtualJoystickDesc desc;
        SDL_zero(desc);
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
        desc.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
        desc.button_mask = (1u << (SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1)) - 1;
        desc.name = "Rush 2 Test Pad";
        int index = SDL_JoystickAttachVirtualEx(&desc);
        SDL_Joystick* pad = index < 0 ? nullptr : SDL_JoystickOpen(index);
        if (pad == nullptr) {
            printf("[Input] RUSH2_TEST_VPAD: no virtual controller (%s)\n", SDL_GetError());
            return;
        }
        test_vpad_id = SDL_JoystickInstanceID(pad);
        printf("[Input] RUSH2_TEST_VPAD: virtual controller attached\n");
        auto launch = std::chrono::steady_clock::now();
        std::thread([pad, presses, launch]() {
            for (;;) {
                double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - launch).count();
                std::array<bool, SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1> down{};
                std::array<int16_t, SDL_CONTROLLER_AXIS_MAX> axes{};
                axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = -32768;   // released, whether SDL reads the trigger as a
                axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = -32768;  // full or a half range axis
                for (const Press& p : presses) {
                    if (now >= p.time && now < p.time + p.duration) {
                        if (p.button >= 0) down[p.button] = true;
                        if (p.axis >= 0) axes[p.axis] = p.value;
                    }
                }
                for (int b = 0; b < (int)down.size(); b++) {
                    SDL_JoystickSetVirtualButton(pad, b, down[b] ? SDL_PRESSED : SDL_RELEASED);
                }
                for (int a = 0; a < (int)axes.size(); a++) {
                    SDL_JoystickSetVirtualAxis(pad, a, axes[a]);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }).detach();
    }
}

void rush2::input::poll() {
    static bool vpad_started = false;
    if (!vpad_started) {
        vpad_started = true;
        start_test_vpad();
    }
    recompinput::poll_inputs();
    update_port_assignments();
}

static std::atomic<bool> at_title_screen = false;

void rush2::input::set_at_title(bool at_title) {
    at_title_screen = at_title;
}

int rush2::input::port_player(int port) {
    uint8_t* rdram = game_rdram.load();
    if (rdram == nullptr || port < 0 || port >= num_ports || at_title_screen.load()) {
        return -1;
    }
    // An online race's records have no ports (src/online/online_game.cpp): port i + 1 is this machine's player i's.
    // Reading the records there flickered, as the race's presentation lends records ports for each frame's drawing.
    if (rush2::online::game::local_race_player() >= 0) {
        return rush2::online::game::local_car(port);
    }
    // The players' records (0x800C2140, 0x28 each, +1 the port or 5 for none; four of them, src/players4.cpp). Player 2
    // only counts while the game has 2 players: its record keeps its port after it leaves.
    constexpr uint32_t records = 0x800C2140;
    constexpr uint32_t record_size = 0x28;
    constexpr uint32_t num_players = 0x8010C3E2;
    int players = MEM_H(0, (int32_t)num_players);
    for (int player = 0; player < 4; player++) {
        if (player == 1 && players < 2) {
            continue;
        }
        if (MEM_BU(1, (int32_t)(records + player * record_size)) == port) {
            return player;
        }
    }
    return -1;
}

bool rush2::input::port_has_device(int port) {
    return port >= 0 && port < num_ports && (port_controllers[port].load() != no_controller || get_keyboard_port() == port);
}

static std::atomic<bool> port_scripted[rush2::input::num_ports];

void rush2::input::set_port_scripted(int port) {
    if (port >= 0 && port < num_ports) {
        port_scripted[port] = true;
    }
}

bool rush2::input::is_port_scripted(int port) {
    return port >= 0 && port < num_ports && port_scripted[port].load();
}

bool rush2::input::is_port_connected(int port) {
    // Port 1 is always connected so the game always has a player 1.
    return port == 0 || port_has_device(port);
}

bool rush2::input::get_n64_input(int port, uint16_t* buttons_out, float* x_out, float* y_out) {
    *buttons_out = 0;
    *x_out = 0.0f;
    *y_out = 0.0f;
    // The pedals are written once, with this read's values: another thread's get_pedals never sees a pedal zeroed
    // midway through a read (the online race reads them from the game thread while the controller thread reads too).
    auto no_pedals = [port]() {
        if (port >= 0 && port < num_ports) {
            pedal_gas[port] = 0.0f;
            pedal_brake[port] = 0.0f;
        }
    };

    if (!is_port_connected(port)) {
        no_pedals();
        return false;
    }

    if (recompinput::game_input_disabled() || hands_off()) {
        no_pedals();
        return true;
    }

    // While the Controller Setup screen listens for a binding, and after it binds one until everything is let go, the
    // game sees nothing from the port.
    if (rush2::controls::is_listening(port)) {
        no_pedals();
        return true;
    }
    // The track or car select's game filter panel is open: it reads the ports itself (src/game_filter.cpp).
    if (rush2::game_filter::input_captured()) {
        no_pedals();
        return true;
    }
    if (blocked_until_release[port]) {
        if (rush2::controls::any_input_held(port)) {
            no_pedals();
            return true;
        }
        blocked_until_release[port] = false;
    }

    uint16_t buttons;
    float x, y;
    float gas = 0.0f;
    float brake = 0.0f;
    bool race = in_race();
    rush2::control_profiles::Settings settings = rush2::control_profiles::port_settings(port);
    if (race) {
        rush2::controls::get_race_input(port, &buttons, &x, &y, settings.steering_exponent(), settings.deadzone / 100.0f,
            &gas, &brake);
    }
    else {
        rush2::controls::get_menu_input(port, &buttons, &x, &y);
    }

    // A button held while switching between the menu and race layouts (Start to pause, A to resume) stays released
    // until it's let go, so it doesn't press whatever it's bound to in the other layout. Gas and brake are exempt when
    // entering a race: holding gas while the race loads should drive the car as soon as it can move.
    PortState& state = port_states[port];
    if (race != state.last_race) {
        state.held_over = race ? (buttons & ~race_held_buttons) : buttons;
        state.last_race = race;
    }
    state.held_over &= buttons;
    buttons &= ~state.held_over;

    if (race && settings.reverse == rush2::control_profiles::ReverseControl::Hold && (buttons & n64_c_down) &&
        !(buttons & n64_a)) {
        buttons |= n64_a;
        gas = 1.0f;
    }

    pedal_gas[port] = (buttons & n64_a) ? gas : 0.0f;
    pedal_brake[port] = (buttons & n64_b) ? brake : 0.0f;

    *buttons_out = buttons;
    *x_out = x;
    *y_out = y;
    return true;
}

void rush2::input::set_rumble_cornering(int port, float side) {
    if (port >= 0 && port < num_ports) {
        rumble_cornering[port].store(side, std::memory_order_relaxed);
    }
}

void rush2::input::set_rumble_grinding(int port, float side, float intensity) {
    if (port >= 0 && port < num_ports) {
        rumble_grind_side[port].store(side, std::memory_order_relaxed);
        rumble_grind_intensity[port].store(intensity, std::memory_order_relaxed);
    }
}

bool rush2::input::rumble_debug_enabled() {
    return rumble_debug.load(std::memory_order_relaxed);
}

void rush2::input::set_rumble_muted(bool muted) {
    rumble_muted = muted;
}

void rush2::input::set_rumble_debug(bool enabled) {
    rumble_debug = enabled;
}

bool rush2::input::rumble_directional() {
    return any_port_directional.load(std::memory_order_relaxed);
}

bool rush2::input::take_rumble_impact_reset(int port) {
    return port >= 0 && port < num_ports && rumble_impact_reset[port].exchange(false);
}

void rush2::input::set_rumble_impact(int port, float side, bool hard) {
    if (port < 0 || port >= num_ports) {
        return;
    }
    uint32_t side_bits;
    memcpy(&side_bits, &side, sizeof(side_bits));
    uint64_t value = side_bits | impact_pending | (hard ? impact_hard : 0);
    uint64_t prev = rumble_impact[port].load();
    do {
        // A soft impact doesn't replace a hard one the VI thread hasn't taken yet.
        if (!hard && (prev & impact_hard)) {
            return;
        }
    } while (!rumble_impact[port].compare_exchange_weak(prev, value));
}

void rush2::input::set_rumble(int port, bool on) {
    if (port >= 0 && port < num_ports) {
        rumble_active[port] = on;
    }
}

namespace {
    // Where a slot's Enhanced rumble plays.
    enum class RumbleOutput {
        Haptics,   // A DualSense's two actuators over USB (dualsense_haptics.h): matched, and each plays every voice.
        SteamTones, // A Steam Controller's (2026) actuators as tones (steam_haptics.h), like the DualSense's.
        Triggers,  // An Xbox One/Series pad's trigger motors (matched) carry the sides; its unmatched grip motors play the
                   // thump on the heavy one and the screech on the light one.
        Pair,      // The two SDL motors as left and right: a Steam Deck's matched rumble, or a Steam Controller's
                   // until (or if) its tones open.
        Grips,     // Unmatched grip motors (an Xbox pad without trigger rumble, a DualSense over Bluetooth, most other
                   // pads): no sides, since a big left and small right motor make hits on one side feel unlike the
                   // other. The heavy motor plays the hit and the grind, the light one the screech under them.
        Xbox360,   // An Xbox 360 pad's grip motors (SDL's Xbox 360 type, which XInput wheels and adapters such as
                   // XOutput also report): as Grips, with the screech on the heavy motor too.
    };

    // Valve controllers with a matched pair of haptics behind SDL's two rumble channels (low = left, high = right):
    // the Steam Controller and the Steam Deck through SDL3's own drivers (SDL_hidapi_steam_triton.c and
    // SDL_hidapi_steamdeck.c, any vendor 0x28DE product), and on a Steam Deck (Steam sets SteamDeck=1) Steam Input's
    // virtual pad (0x11FF) too. Elsewhere the virtual pad can stand in for any controller, so it isn't counted.
    bool has_matched_pair(SDL_GameController* controller) {
        constexpr uint16_t vendor_valve = 0x28DE;
        constexpr uint16_t product_steam_virtual_gamepad = 0x11FF;
        static const bool on_deck = [] {
            const char* value = getenv("SteamDeck");
            return value != nullptr && strcmp(value, "1") == 0;
        }();
        return SDL_GameControllerGetVendor(controller) == vendor_valve &&
            (SDL_GameControllerGetProduct(controller) != product_steam_virtual_gamepad || on_deck);
    }

    // How much of a level each side keeps for something on `side` (-1 right to 1 left): the near side all of it, the
    // far side less the squarer on it was (far_cut: the profile's Far Side Cut, 0 to 1).
    void side_gains(float side, float far_cut, float* left, float* right) {
        *left = side >= 0.0f ? 1.0f : 1.0f + far_cut * side;
        *right = side <= 0.0f ? 1.0f : 1.0f - far_cut * side;
    }

    // A test of one effect from the Controls tab (test_rumble): played with the tested profile's settings, as if in a
    // race, on the controller last used (the one in the player's hands as they press the button), in its port's slot,
    // or in test_slot when it isn't driving a port. Screech and grind play on the left, then the right.
    constexpr int test_frames = 90;
    std::mutex test_mutex;
    rush2::input::RumbleTest test_kind = rush2::input::RumbleTest::LeftCrash; // Under test_mutex.
    rush2::control_profiles::Settings test_settings;                          // Under test_mutex.
    int test_left = 0;                                                         // Frames left; under test_mutex.

    // The controller a test plays on: the last one used, or else the first one connected.
    SDL_JoystickID test_target() {
        SDL_JoystickID id = last_used_controller.load();
        if (get_connected_controller(id) != nullptr && rush2::input::is_controller_enabled(id)) {
            return id;
        }
        for (const Connected& c : connected_controllers()) {
            if (rush2::input::is_controller_enabled(c.id)) {
                return c.id;
            }
        }
        return no_controller;
    }

    // Silences a slot's haptics (the DualSense's or Steam tones) if it was playing on them.
    void stop_slot_haptics(int slot) {
        if (!rumble_on_haptics[slot]) {
            return;
        }
        if (rumble_haptics_steam[slot]) {
            rush2::steam_haptics::set_levels(rumble_haptics_path[slot], {}, 0.0f, 0.0f, 0.0f);
        }
        else {
            rush2::haptics::set_levels(rumble_haptics_path[slot], {}, 0.0f, 0.0f, 0.0f);
        }
        rumble_on_haptics[slot] = false;
        rumble_haptics_path[slot].clear();
    }
}

void rush2::input::test_rumble(RumbleTest test, const rush2::control_profiles::Settings& settings) {
    std::lock_guard lock{ test_mutex };
    test_kind = test;
    test_settings = settings;
    test_left = test_frames;
}

// Same ramp up and fall off as the frontend's update_rumble, but each port only rumbles its own controller. Original
// rumbles both motors at the game's level. Enhanced plays impacts as a fading thump on their side, the rest of the
// game's rumble (tire screech) as a lighter buzz on the outside of the turn and wall grinding as a rasp on the wall's
// side, each on the controller's best output (RumbleOutput).
void rush2::input::update_rumble() {
    if (!recompui::config::general::has_rumble_strength_option()) {
        return;
    }

    using rush2::control_profiles::RumbleMode;

    // Losing focus with Background Input off stops everything this game plays on the controllers once, then sends
    // nothing until focus comes back (hands_off). A test run (rumble_muted) never sends any.
    static bool was_hands_off = false;
    if (hands_off() || rumble_muted) {
        if (!was_hands_off) {
            was_hands_off = true;
            for (int slot = 0; slot < num_slots; slot++) {
                SDL_GameController* controller = slot == test_slot ? get_connected_controller(test_slot_controller)
                                                                   : get_connected_controller(port_controllers[slot].load());
                if (controller != nullptr) {
                    SDL_GameControllerRumble(controller, 0, 0, 0);
                    if (rumble_triggers_on[slot]) {
                        SDL_GameControllerRumbleTriggers(controller, 0, 0, 0);
                    }
                }
                stop_slot_haptics(slot);
                rumble_triggers_on[slot] = false;
                cur_rumble[slot] = 0.0f;
            }
            rush2::haptics::set_controllers({});
            rush2::steam_haptics::set_controllers({});
        }
        return;
    }
    was_hands_off = false;

    // A test from the Controls tab replaces its slot's settings and race state while it plays.
    bool testing;
    RumbleTest test;
    int test_frame;
    rush2::control_profiles::Settings tested;
    {
        std::lock_guard lock{ test_mutex };
        testing = test_left > 0;
        test = test_kind;
        test_frame = test_frames - test_left;
        tested = test_settings;
        if (testing) {
            test_left--;
        }
    }

    // The test's slot, and the controller in test_slot: the tested one while it isn't driving a port, and afterwards
    // the last one tested there until it joins a port (its rumble falls off and stops like a port's).
    int testing_slot = -1;
    SDL_JoystickID slot_controller = test_slot_controller;
    if (testing) {
        SDL_JoystickID target = test_target();
        int port = target == no_controller ? -1 : get_port_for_controller(target);
        testing_slot = target == no_controller ? -1 : port >= 0 ? port : test_slot;
        slot_controller = port >= 0 ? no_controller : target;
    }
    if (slot_controller != no_controller &&
        (get_connected_controller(slot_controller) == nullptr || get_port_for_controller(slot_controller) >= 0)) {
        slot_controller = no_controller;
    }
    if (slot_controller != test_slot_controller) {
        SDL_GameController* old = get_connected_controller(test_slot_controller);
        if (old != nullptr && get_port_for_controller(test_slot_controller) < 0) {
            SDL_GameControllerRumble(old, 0, 0, 0);
            if (rumble_triggers_on[test_slot]) {
                SDL_GameControllerRumbleTriggers(old, 0, 0, 0);
            }
        }
        stop_slot_haptics(test_slot);
        rumble_triggers_on[test_slot] = false;
        test_slot_controller = slot_controller;
    }

    std::array<SDL_GameController*, num_slots> controllers{};
    std::array<rush2::control_profiles::Settings, num_slots> settings;
    std::array<bool, num_slots> enhanced{};
    std::array<RumbleOutput, num_slots> outputs{};
    std::array<std::string, num_slots> haptics_paths;
    bool any_enhanced = false;
    std::vector<std::string> dualsenses;                         // Wanting haptics.
    std::vector<rush2::steam_haptics::Controller> steam_devices; // Wanting tones.
    for (int slot = 0; slot < num_slots; slot++) {
        SDL_GameController* controller = slot == test_slot ? get_connected_controller(test_slot_controller)
                                                           : get_connected_controller(port_controllers[slot].load());
        controllers[slot] = controller;
        settings[slot] = slot == test_slot || slot == testing_slot ? tested : rush2::control_profiles::port_settings(slot);
        enhanced[slot] = settings[slot].rumble_mode != RumbleMode::Original;
        if (controller == nullptr) {
            continue;
        }
        any_enhanced |= enhanced[slot] && slot != test_slot;
        bool triggers = SDL_GameControllerHasRumbleTriggers(controller);
        // SDL reports trigger motors only on Xbox One and Series pads, so a 360 pad (or anything posing as one) falls
        // through to its own grips mix.
        if (triggers) {
            outputs[slot] = RumbleOutput::Triggers;
        }
        else if (has_matched_pair(controller)) {
            outputs[slot] = RumbleOutput::Pair;
        }
        else if (SDL_GameControllerGetType(controller) == SDL_CONTROLLER_TYPE_XBOX360) {
            outputs[slot] = RumbleOutput::Xbox360;
        }
        else {
            outputs[slot] = RumbleOutput::Grips;
        }
        const char* path = SDL_GameControllerPath(controller);
        if (!enhanced[slot] || path == nullptr || path[0] == '\0') {
            continue;
        }
        // Every DualSense and Steam Controller plays on its own haptics once they're open.
        uint16_t vendor = SDL_GameControllerGetVendor(controller);
        uint16_t product = SDL_GameControllerGetProduct(controller);
        if (SDL_GameControllerGetType(controller) == SDL_CONTROLLER_TYPE_PS5) {
            dualsenses.push_back(path);
            if (rush2::haptics::active(path)) {
                outputs[slot] = RumbleOutput::Haptics;
                haptics_paths[slot] = path;
            }
        }
        else if (rush2::steam_haptics::is_supported(vendor, product)) {
            steam_devices.push_back({ path, product });
            if (rush2::steam_haptics::active(path)) {
                outputs[slot] = RumbleOutput::SteamTones;
                haptics_paths[slot] = path;
            }
        }
    }
    any_port_directional = any_enhanced;
    rush2::haptics::set_controllers(dualsenses);
    rush2::steam_haptics::set_controllers(steam_devices);

    // Hits only rumble during an unpaused race in Enhanced; anything else drops them along with any hold still
    // running, so a hit just before the finish or a pause doesn't keep rumbling into the menus.
    bool racing = in_race();
    for (int slot = 0; slot < num_slots; slot++) {
        const rush2::control_profiles::Settings& s = settings[slot];
        float far_cut = s.far_side_cut / 100.0f;
        float rumble_strength = s.rumble_strength / 100.0f;
        bool slot_testing = slot == testing_slot;
        // test_slot has no car: it only ever plays tests.
        bool live = slot_testing || (racing && slot != test_slot);

        // Take the impact the game thread reported since the last frame.
        uint64_t impact = rumble_impact[slot].exchange(0);
        if (slot_testing) {
            // The tested effect, as the hooks would report it: a hard hit at the start, or a second and a half of the
            // game rumbling for a slide or of wall contact, on the left side, then the right.
            float side = test == RumbleTest::RightCrash || test_frame >= test_frames / 2 ? -1.0f : 1.0f;
            impact = 0;
            rumble_active[slot] = test == RumbleTest::Screech;
            rumble_cornering[slot] = test == RumbleTest::Screech ? side : 0.0f;
            rumble_grind_intensity[slot] = test == RumbleTest::Grind ? 1.0f : 0.0f;
            rumble_grind_side[slot] = side;
            if (test_frame == 0) {
                frames_since_impact[slot] = screech_after_impact_frames;
                if (test == RumbleTest::LeftCrash || test == RumbleTest::RightCrash) {
                    uint32_t side_bits;
                    memcpy(&side_bits, &side, sizeof(side_bits));
                    impact = side_bits | impact_pending | impact_hard;
                    rumble_active[slot] = true; // The game rumbles for the hit.
                }
            }
            if (test_frame == test_frames - 1) {
                rumble_active[slot] = false; // The end of the test lets go of everything.
                rumble_grind_intensity[slot] = 0.0f;
            }
        }
        if (!enhanced[slot] || !live) {
            rumble_hold[slot] = 0;
            rumble_side[slot] = 0.0f;
            rumble_unconfirmed[slot] = -1;
            rumble_impact_reset[slot] = true;
            impact_level[slot] = 0.0f;
            screech_side[slot] = 0.0f;
            frames_since_impact[slot] = screech_after_impact_frames;
            grind_level[slot] = 0.0f;
            rumble_grind_intensity[slot] = 0.0f;
        }
        else if (impact & impact_pending) {
            float side;
            uint32_t side_bits = (uint32_t)impact;
            memcpy(&side, &side_bits, sizeof(side));
            bool hard = (impact & impact_hard) != 0;
            // A hold longer than a soft hit's means a hard impact is still rumbling; soft ones don't cut it short.
            if (hard) {
                impact_level[slot] = 1.0f;
                impact_side[slot] = side;
                frames_since_impact[slot] = 0;
            }
            if (hard || rumble_hold[slot] <= soft_hit_hold_frames) {
                rumble_side[slot] = side;
                rumble_hold[slot] = hard ? hard_hit_hold_frames : soft_hit_hold_frames;
                if (rumble_unconfirmed[slot] < 0 && !rumble_active[slot]) {
                    rumble_unconfirmed[slot] = 0;
                }
                if (rumble_debug) {
                    fprintf(stderr, "rumble slot %d: %s hit %s (side %+.2f)\n", slot, hard ? "HARD" : "soft",
                        std::fabs(side) < head_on_side ? "HEAD ON" : side > 0.0f ? "LEFT" : "RIGHT", side);
                }
            }
        }
        else if (rumble_hold[slot] > 0) {
            rumble_hold[slot]--;
        }
        else {
            rumble_side[slot] *= side_fade;
        }

        // A hit the game hasn't rumbled for in time doesn't play either.
        if (rumble_unconfirmed[slot] >= 0) {
            if (rumble_active[slot]) {
                rumble_unconfirmed[slot] = -1;
            }
            else if (++rumble_unconfirmed[slot] > hit_confirm_frames) {
                rumble_unconfirmed[slot] = -1;
                rumble_hold[slot] = 0;
                impact_level[slot] = 0.0f;
            }
        }

        // The thump fades from the hit (playing once the game rumbled for it), and the screech's side follows the
        // cornering side.
        float thump = rumble_unconfirmed[slot] < 0 ? impact_level[slot] : 0.0f;
        impact_level[slot] *= impact_fade;
        if (frames_since_impact[slot] < screech_after_impact_frames) {
            frames_since_impact[slot]++;
        }
        screech_side[slot] += (rumble_cornering[slot].load(std::memory_order_relaxed) - screech_side[slot]) *
            screech_side_follow;
        if (enhanced[slot] && live) {
            float grinding = rumble_grind_intensity[slot].load(std::memory_order_relaxed);
            grind_level[slot] += (grinding - grind_level[slot]) *
                (grinding > grind_level[slot] ? grind_attack : grind_release);
            if (grinding > 0.0f) {
                grind_side[slot] = rumble_grind_side[slot].load(std::memory_order_relaxed);
            }
        }

        // The game's rumble ramps the level up. Otherwise it falls off.
        if (rumble_active[slot]) {
            cur_rumble[slot] = std::min(cur_rumble[slot] + 0.17f, 1.0f);
        }
        else {
            cur_rumble[slot] = std::max(cur_rumble[slot] * 0.92f - 0.01f, 0.0f);
        }

        SDL_GameController* controller = controllers[slot];
        if (controller == nullptr) {
            stop_slot_haptics(slot);
            rumble_retry[slot] = 0;
            continue;
        }

        float amount = cur_rumble[slot];
        float smooth_rumble = amount * amount * (3.0f - 2.0f * amount);
        float strength = smooth_rumble * rumble_strength;
        // Long enough to last until the next update; the game turns rumble off on its own.
        constexpr uint32_t duration_ms = 1000000;
        RumbleOutput output = outputs[slot];
        if (rumble_triggers_on[slot] && (!enhanced[slot] || output != RumbleOutput::Triggers)) {
            SDL_GameControllerRumbleTriggers(controller, 0, 0, 0);
            rumble_triggers_on[slot] = false;
        }
        bool on_haptics = output == RumbleOutput::Haptics || output == RumbleOutput::SteamTones;
        if (rumble_on_haptics[slot] && (!enhanced[slot] || !on_haptics || haptics_paths[slot] != rumble_haptics_path[slot])) {
            stop_slot_haptics(slot);
        }
        // A rumble call that failed is tried again after a while rather than every frame.
        auto rumble = [&](uint16_t low, uint16_t high) {
            if (rumble_retry[slot] > 0) {
                rumble_retry[slot]--;
            }
            else if (SDL_GameControllerRumble(controller, low, high, duration_ms) != 0) {
                rumble_retry[slot] = rumble_retry_frames;
            }
        };
        if (!enhanced[slot]) {
            // Both motors at the game's level (the frontend only ran the light one, which made wall grinding, the
            // game's longest rumble, hard to feel).
            rumble((uint16_t)(strength * 0xFFFF), (uint16_t)(strength * 0xFFFF));
            continue;
        }

        // Each side's thump (impacts), buzz (screech) and grind, favoring its side.
        float hit = thump * rumble_strength * s.crash_strength / 100.0f;
        float gain_left, gain_right;
        side_gains(impact_side[slot], far_cut, &gain_left, &gain_right);
        float thump_left = hit * gain_left;
        float thump_right = hit * gain_right;
        float screech = 0.0f;
        if (frames_since_impact[slot] >= screech_after_impact_frames) {
            screech = strength * s.screech_strength / 100.0f;
        }
        side_gains(screech_side[slot], far_cut, &gain_left, &gain_right);
        float buzz_left = screech * gain_left;
        float buzz_right = screech * gain_right;
        float grind = grind_level[slot] * s.grind_strength / 100.0f * rumble_strength;
        side_gains(grind_side[slot], far_cut, &gain_left, &gain_right);
        float grind_left = grind * gain_left;
        float grind_right = grind * gain_right;
        if (rumble_debug && (thump_left > 0.0f || thump_right > 0.0f || buzz_left > 0.0f || buzz_right > 0.0f ||
                             grind_left > 0.0f || grind_right > 0.0f) &&
            ++rumble_log_frames[slot] % rumble_log_interval_frames == 0) {
            static const char* output_names[] = { "haptics", "steam tones", "triggers", "matched motors", "grip motors",
                                                  "xbox 360 motors" };
            fprintf(stderr, "rumble slot %d (%s): thump L %3d%% R %3d%%  screech L %3d%% R %3d%%  grind L %3d%% R %3d%%\n",
                slot, output_names[(int)output], (int)(thump_left * 100.0f), (int)(thump_right * 100.0f),
                (int)(buzz_left * 100.0f), (int)(buzz_right * 100.0f), (int)(grind_left * 100.0f),
                (int)(grind_right * 100.0f));
        }

        if (on_haptics) {
            if (!rumble_on_haptics[slot]) {
                // Stop SDL's rumble, which would otherwise keep going: the actuators take over from here.
                SDL_GameControllerRumble(controller, 0, 0, 0);
                rumble_on_haptics[slot] = true;
                rumble_haptics_path[slot] = haptics_paths[slot];
                rumble_haptics_steam[slot] = output == RumbleOutput::SteamTones;
            }
            rush2::haptics::Levels levels;
            levels.thump[0] = thump_left;
            levels.thump[1] = thump_right;
            levels.buzz[0] = buzz_left;
            levels.buzz[1] = buzz_right;
            levels.grind[0] = grind_left;
            levels.grind[1] = grind_right;
            if (output == RumbleOutput::Haptics) {
                rush2::haptics::set_levels(haptics_paths[slot], levels, s.thump_hz, s.buzz_hz, s.grind_hz);
            }
            else {
                rush2::steam_haptics::set_levels(haptics_paths[slot], levels, s.thump_hz, s.buzz_hz, s.grind_hz);
            }
            continue;
        }

        // Motors play the effects as steady levels of different weight (see motor_screech_gain): they add up, so a hit
        // during a slide feels like both.
        auto to_motor = [](float level) { return (uint16_t)(std::clamp(level, 0.0f, 1.0f) * 0xFFFF); };
        float light_screech = screech * motor_screech_gain;
        float heavy_grind = grind * motor_grind_gain;
        float low, high;
        if (output == RumbleOutput::Pair) {
            // The low-frequency motor as the left side and the high-frequency one as the right (a Steam Deck's
            // matched pair, or a Steam Controller's without its tones).
            side_gains(screech_side[slot], far_cut, &gain_left, &gain_right);
            low = thump_left + std::min(light_screech * gain_left + grind_left * motor_grind_gain, pair_steady_max);
            high = thump_right + std::min(light_screech * gain_right + grind_right * motor_grind_gain, pair_steady_max);
        }
        else {
            // The grips aren't matched: the heavy low-frequency motor carries the hit and the grind, the light
            // high-frequency one the screech under the rest of the hit and grind.
            low = hit + heavy_grind;
            if (output == RumbleOutput::Xbox360) {
                low += light_screech * xbox360_screech_heavy_gain;
            }
            high = hit * motor_hit_light_gain + heavy_grind * motor_grind_light_gain + light_screech;
        }
        if (output == RumbleOutput::Triggers) {
            // The matched triggers carry the sides: a quick kick on the hit's side, the screech on the outside of the
            // turn and the grind on the wall's side.
            float kick = std::pow(thump, trigger_kick_power) * rumble_strength * s.crash_strength / 100.0f *
                trigger_kick_gain;
            side_gains(impact_side[slot], far_cut, &gain_left, &gain_right);
            float trigger_left = kick * gain_left;
            float trigger_right = kick * gain_right;
            side_gains(screech_side[slot], far_cut, &gain_left, &gain_right);
            trigger_left += light_screech * trigger_screech_gain * gain_left;
            trigger_right += light_screech * trigger_screech_gain * gain_right;
            side_gains(grind_side[slot], far_cut, &gain_left, &gain_right);
            trigger_left += heavy_grind * trigger_grind_gain * gain_left;
            trigger_right += heavy_grind * trigger_grind_gain * gain_right;
            SDL_GameControllerRumbleTriggers(controller, to_motor(trigger_left), to_motor(trigger_right), duration_ms);
            rumble_triggers_on[slot] = true;
        }
        rumble(to_motor(low), to_motor(high));
    }
}

void rush2::input::get_pedals(int port, float* gas, float* brake) {
    *gas = (port >= 0 && port < num_ports) ? pedal_gas[port].load() : 0.0f;
    *brake = (port >= 0 && port < num_ports) ? pedal_brake[port].load() : 0.0f;
}

void rush2::input::press_pedals(int port, uint16_t buttons) {
    if (port >= 0 && port < num_ports) {
        if (buttons & n64_a) pedal_gas[port] = 1.0f;
        if (buttons & n64_b) pedal_brake[port] = 1.0f;
    }
}

// End of func_80076854, after it set the car's pedals from the player's held buttons: $s0 = car, $s1 = player.
// A pressed GAS or BRAKE button sets its pedal to 1.0; this scales it by how far the bound trigger is pressed. Pedals
// bound to the game's own stick modes (masks other than A and B) are left alone.
extern "C" void rush2_analog_pedals(uint8_t* rdram, recomp_context* ctx) {
    gpr player = ctx->r17;
    gpr car = ctx->r16;
    int port = MEM_BU(1, player);
    // Online races take every player's pedals from the frame's inputs (src/online/online_game.cpp).
    float online_gas = 0.0f, online_brake = 0.0f;
    bool online = rush2::online::game::pedals((int)(((uint32_t)player - 0x800C2140u) / 0x28u), &online_gas, &online_brake);
    if (port >= num_ports && !online) {
        return;
    }
    auto scale = [&](int offset, float pressure) {
        uint32_t bits = MEM_W(offset, car);
        float value;
        memcpy(&value, &bits, sizeof(value));
        if (value == 1.0f) {
            memcpy(&bits, &pressure, sizeof(bits));
            MEM_W(offset, car) = bits;
        }
    };
    if (MEM_HU(player_gas_mask, player) == n64_a) {
        scale(car_throttle, online ? online_gas : pedal_gas[port].load());
    }
    if (MEM_HU(player_brake_mask, player) == n64_b && MEM_B(0, (int32_t)brake_disabled) == 0) {
        scale(car_brake, online ? online_brake : pedal_brake[port].load());
    }
}
