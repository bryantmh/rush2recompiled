#ifndef __RUSH2_H__
#define __RUSH2_H__

#include <cstdint>
#include <string>
#include <vector>

namespace rush2 {
    inline const std::u8string program_id = u8"Rush2Recompiled";
    inline const std::string program_name = "Rush 2: Recompiled";

    void register_overlays();

    // Creates the config menu tabs. Must be called before recomp::start.
    void init_config();

    // Forces every model to its most detailed LOD and turns off LOD distance culling (src/lod.cpp).
    void set_lod_disabled(bool disabled);

    // High-resolution fonts (src/fonts.cpp). install_font_pack copies the built-in pack into the mods folder and must
    // run after the config path is registered. set_hires_fonts_enabled turns the pack and its tile clamping on or off.
    void install_font_pack();
    void set_hires_fonts_enabled(bool enabled);

    // Where settings and saves are stored (src/data_location.cpp): the user's app data folder, or the program's folder
    // in portable mode (portable.txt next to the executable). Switching takes effect on the next launch.
    namespace data_location {
        bool is_portable();
        // Creates or removes portable.txt and schedules this session's data to be copied to the new folder on the next
        // launch. Returns false if the files couldn't be written.
        bool set_portable(bool portable);
        // Performs a copy scheduled by set_portable. Call at startup, before the config path is registered.
        void apply_pending_move();
    }

    // Cheats tab: the in-game cheat menu and forced cheats (src/cheats.cpp).
    namespace cheats {
        void create_tab();
    }

    // Per-port input (src/input.cpp). Each player (N64 port 1 or 2) gets a controller and optionally the keyboard,
    // either chosen in the Players tab (src/players_tab.cpp) or, for a port left on Auto, the first unassigned
    // controller to press a button.
    namespace input {
        constexpr int num_ports = 2;

        void poll();
        bool is_port_connected(int port);
        bool get_n64_input(int port, uint16_t* buttons, float* x, float* y);
        // Response curve for controller stick X: x is replaced by sign(x) * |x|^exponent.
        void set_steering_exponent(float exponent);
        void set_rumble(int port, bool on);
        void update_rumble();

        // Remembers rdram so input reads can tell races from menus. Called from a game thread hook every frame.
        void set_rdram(uint8_t* rdram);

        // Loads the saved player assignments. Call after the config path is registered.
        void load_players();

        struct ControllerInfo {
            int32_t joystick_id;
            std::string name;
            std::string key; // GUID and serial, identifies the controller across launches.
            bool playstation;
        };
        // Connected controllers, in SDL's order.
        std::vector<ControllerInfo> get_controllers();

        // What each port's controller slot is set to in the Players tab.
        struct PortChoice {
            enum class Kind { Auto, None, Controller };
            Kind kind = Kind::Auto;
            std::string controller_key; // Kind::Controller only.
            std::string controller_name; // Kind::Controller only, for showing a controller that isn't connected.
        };
        PortChoice get_port_choice(int port);
        // Assigns a port's controller slot and saves. A controller chosen for one port is taken off the other.
        void set_port_choice(int port, const PortChoice& choice);
        // The controller currently driving a port, or -1.
        int32_t get_port_controller(int port);
        // The port the keyboard drives, or -1 for none.
        int get_keyboard_port();
        void set_keyboard_port(int port);
        // True if the port's controller is a PlayStation controller (for button glyphs).
        bool port_has_playstation_controller(int port);

        // Blocks a port's game input from now until every input on its devices is released (after the Controller
        // Setup screen captures a binding, so the press that was bound isn't also seen by the game).
        void suppress_until_released(int port);
    }

    // Button bindings for driving (src/controls.cpp). In races each player's bound inputs are turned into the game's
    // default N64 layout, which the game's own binding table is locked to; menus use a fixed layout.
    namespace controls {
        // The rows of the game's Controller Setup screen, in order.
        enum class Action : uint8_t {
            Gas, Brake, Steering, ShiftUp, ShiftDown, Reverse, Abort, View, Horn, Wings, Count
        };
        constexpr int action_count = static_cast<int>(Action::Count);

        enum class Device : uint8_t { Controller, Keyboard, Count };
        constexpr int device_count = static_cast<int>(Device::Count);

        // One physical input. Steering on a controller is a Stick; on the keyboard it's two keys (left, right).
        struct Input {
            enum class Type : uint8_t { None, Button, AxisPositive, AxisNegative, Key, Stick };
            Type type = Type::None;
            int32_t id = 0; // SDL_GameControllerButton, SDL_GameControllerAxis, SDL_Scancode or Stick.
            bool operator==(const Input&) const = default;
        };
        enum Stick : int32_t { LeftStick, RightStick, Dpad };
        constexpr int slots_per_action = 2;

        void load();
        void save();
        // Copies of every binding, for the Controller Setup screen's Cancel.
        void snapshot();
        void restore_snapshot();
        void reset_defaults(int port, Device device);

        Input get_binding(int port, Device device, Action action, int slot);

        // Listening for a new binding on a port's devices. update_listen returns true once the listen has ended
        // (an input was bound, or it was cancelled or timed out).
        void begin_listen(int port, Action action);
        void cancel_listen(int port);
        bool is_listening(int port);
        // For keyboard steering, the slot (0 = left, 1 = right) being waited on.
        int listen_slot(int port);
        bool update_listen(int port);

        // The game's N64 buttons and stick for a port during a race, and how far GAS and BRAKE are pressed (0 to 1).
        void get_race_input(int port, uint16_t* buttons, float* x, float* y, float steering_exponent, float* gas,
                            float* brake);
        // The fixed menu layout.
        void get_menu_input(int port, uint16_t* buttons, float* x, float* y);
        // True if an input on the port's devices is held (used to wait for releases).
        bool any_input_held(int port);
    }

    // Players tab: which controller and keyboard each player uses (src/players_tab.cpp).
    namespace players {
        void create_tab();
    }
}

#endif
