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

    // Draw distance factor (src/draw_distance.cpp): 1 is the original. Scales the projection's near (up to 2x) and
    // far planes and the placed object and LOD cull distances; above 1 it draws every track section regardless of
    // visibility and gives nodes past the fixed-point matrix range float matrices.
    void set_draw_distance(float factor);
    float draw_distance();
    // Visibility hook helper: picks the all-visible section mask when the draw distance is extended.
    bool draw_distance_pvs(uint8_t* rdram, uint32_t sp);
    // True (once) if the node matrix at addr was written as floats, so its G_MTX must become a gEXMatrixFloat.
    bool draw_distance_take_float_matrix(uint32_t addr);

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

    // Split screen layout of 2 player races (src/splitscreen.cpp).
    namespace splitscreen {
        enum class Layout : uint32_t { TopBottom, SideBySide };
        // Takes effect when the next race sets up its views.
        void set_layout(Layout layout);
        // The Split Screen option (src/config.cpp, shown in the Players tab); setting it saves it.
        Layout get_layout_option();
        void set_layout_option(Layout layout);
        // True while a 2 player race is drawn side by side.
        bool is_side_by_side(uint8_t* rdram);
        // The number of views (3 or 4) while a race is drawn in quadrants, otherwise 0.
        int quadrant_views(uint8_t* rdram);
        // The width, in 4:3 screen pixels (320 at 4:3), that RT64 spreads HUD elements anchored to the window's
        // edges over (Settings > Graphics > HUD Placement).
        float hud_width();
        // The window's width in 4:3 screen pixels (320 at 4:3): the 3D views are drawn out to the window's edges.
        float window_width();
        // tan(half the vertical field of view) view `index` is drawn with now (it changes with the layout).
        float view_tan_v(uint8_t* rdram, int index);
    }

    // Frame interpolation (src/interpolation.cpp): scene nodes that are placed in front of a view's camera each frame,
    // so a fast camera doesn't count as the node teleporting (which would stop its interpolation and leave it
    // behind the camera on the frames in between).
    void interpolation_clear_view_attached();
    void interpolation_view_attached(uint32_t node);
    // Draws a scene node in the primitive color `rgba` (for models that use it; Rush 2's nodes carry no color).
    // interpolation_clear_view_attached also forgets these.
    void interpolation_node_color(uint32_t node, uint32_t rgba);

    // Race HUD placement (src/hud.cpp).
    namespace hud {
        // Before printing text at (x, y) in the race HUD's coordinates after the widget loop: gives it the anchor of the
        // widget it is over (or its screen third) and moves (x, y) with that widget in split screen.
        void anchor_text(uint8_t* rdram, int32_t& x, int32_t& y);
        // Widgets placed by another file (the battle HUD, src/rush2049/battle.cpp): a widget with a scale isn't anchored, grouped
        // or moved for split screen by src/hud.cpp; its image is drawn scaled about its top left corner, and it is
        // anchored at `anchor`, a fraction of the screen's width: its x is kept from that point of the 4:3 screen, which
        // HUD Placement puts at the same fraction of the HUD's width (0 the left edge, 0.5 the middle, 1 the right
        // edge). clear_widget_scales forgets them all (when a HUD is built); set_anchor anchors what is drawn next
        // (text printed after the widget loop) the same way.
        void set_widget_scale(int slot, float scale_x, float scale_y, float anchor = 0.5f);
        void clear_widget_scales();
        void set_anchor(uint8_t* rdram, float fraction);
        // A filled rectangle (screen pixels, RGBA blended by its alpha) in the 2D display list, anchored as above.
        void draw_rect(uint8_t* rdram, float x0, float y0, float x1, float y1, uint32_t rgba, float anchor);
        // An RGBA16 image of w x h texels at `address` (at most 2048 texels) over that rectangle, blended by its alpha.
        void draw_image(uint8_t* rdram, uint32_t address, int w, int h, float x0, float y0, float x1, float y1, float anchor);
        // The part of it src_w x src_h texels from (src_x, src_y) over that rectangle, point sampled if `point`.
        void draw_image_part(uint8_t* rdram, uint32_t address, int w, int h, int src_x, int src_y, int src_w, int src_h,
                             float x0, float y0, float x1, float y1, float anchor, bool point = false);
        // Leaves the primitive color at rgba after the 2D drawing so far (G_SETPRIMCOLOR in the 2D display list).
        void set_prim_color(uint8_t* rdram, uint32_t rgba);
        // Draws a number (up to 6 digits) centered on (center_x, center_y) of the 4:3 screen, `height` pixels tall,
        // white with a shadow, anchored at `anchor` (as set_widget_scale). After the widget loop.
        void draw_number(uint8_t* rdram, const char* digits, float center_x, float center_y, float height, float anchor);
        // Split screen: where player's time and position panels were drawn this frame (the time with its lap time box,
        // shown or not), as drawn: 4:3 screen pixels moved by their anchors, the space of an anchored x. time_* and
        // place_* = the time's and position's left, right and top edges, y0-y1 = the top and bottom of the row of
        // both. False if unknown.
        struct PanelBounds {
            float time_x0, time_x1, time_y0, place_x0, place_x1, place_y0, y0, y1;
        };
        bool panel_row(int player, PanelBounds& out);
    }

    // Cheats tab: the in-game cheat menu and forced cheats (src/cheats.cpp).
    namespace cheats {
        void create_tab();
        // The Cheats tab's unlocks, and the unlock system's option (shown on the Progress tab; src/unlocks.cpp).
        bool unlock_all_cars();
        bool unlock_all_tracks();
        bool unlock_all_parts();
        bool unlock_system();
        void set_unlock_system(bool enabled);
    }

    // Per-port input (src/input.cpp). Each enabled controller takes the first free N64 port when it presses a button;
    // the keyboard is one more controller. Which player a port is follows who presses START.
    namespace input {
        constexpr int num_ports = 4;

        void poll();
        bool is_port_connected(int port);
        // Ports an --input-script presses buttons on (src/main.cpp): the game reads them like a connected controller.
        void set_port_scripted(int port);
        bool is_port_scripted(int port);
        // Whether a controller or the keyboard drives the port.
        bool port_has_device(int port);
        // The player (0-3) on a port, or -1: the game's player records.
        int port_player(int port);
        bool get_n64_input(int port, uint16_t* buttons, float* x, float* y);
        // Scripted presses (--input-script) of GAS (A) or BRAKE (B) count as fully pressed pedals.
        void press_pedals(int port, uint16_t buttons);
        // Response curve for controller stick X: x is replaced by sign(x) * |x|^exponent.
        void set_steering_exponent(float exponent);
        // With this on, holding REVERSE also presses the gas, so the car backs up without it.
        void set_reverse_holds_gas(bool enabled);
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

        // Controllers turned off in the Players tab (they never take a port), remembered by key.
        struct DisabledController {
            std::string key;
            std::string name; // For showing a disabled controller that isn't connected.
        };
        std::vector<DisabledController> get_disabled_controllers();
        // Enables or disables a controller and saves. A disabled controller leaves its port at once.
        void set_controller_enabled(const std::string& key, const std::string& name, bool enabled);
        bool is_controller_enabled(int32_t joystick_id);
        // The controller currently driving a port, or -1.
        int32_t get_port_controller(int port);
        // The port the keyboard drives, or -1 for none (off, or it hasn't pressed anything yet).
        int get_keyboard_port();
        bool get_keyboard_enabled();
        void set_keyboard_enabled(bool enabled);
        // True if the port's controller is a PlayStation controller (for button glyphs).
        bool port_has_playstation_controller(int port);

        // Blocks a port's game input from now until every input on its devices is released (after the Controller
        // Setup screen captures a binding, so the press that was bound isn't also seen by the game).
        void suppress_until_released(int port);
    }

    // Button bindings for driving (src/controls.cpp). In races each player's bound inputs are turned into the game's
    // default N64 layout, which the game's own binding table is locked to; menus use a fixed layout.
    namespace controls {
        // The rows of the game's Controller Setup screen, in order. Fire and DropWeapon are the battle arenas' weapon
        // buttons (src/rush2049/battle.cpp): they aren't N64 buttons of the game's layout, and are read with battle_buttons.
        enum class Action : uint8_t {
            Gas, Brake, Steering, ShiftUp, ShiftDown, Reverse, Abort, View, Horn, Wings, Fire, DropWeapon, Count
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
        // The battle arenas' weapon buttons held on a port as of its last race input: battle_fire, battle_drop, and
        // battle_back while the steering stick is held back (the D-pad's down when it steers, the keyboard's down
        // arrow), which fires a weapon backward (rush2::battle::set_fire_backward).
        constexpr uint8_t battle_fire = 1, battle_drop = 2, battle_back = 4;
        uint8_t battle_buttons(int port);
        // True if an input on the port's devices is held (used to wait for releases).
        bool any_input_held(int port);

        // Glyphs of the menu layout's buttons, as port's controller or keyboard shows them (src/controls_menu.cpp),
        // drawn from a widget's text callback: begin, add any number (x, y: top left; w x h pixels, 16x14 the
        // Controller Setup screen's size), end.
        enum class MenuButton : uint8_t { A, B, L, R };
        void begin_menu_glyphs(uint8_t* rdram);
        void add_menu_glyph(uint8_t* rdram, int port, MenuButton button, int x, int y, int w, int h);
        void end_menu_glyphs(uint8_t* rdram);
    }

    // Players tab: which controller and keyboard each player uses, their wings and the split screen layout
    // (src/players_tab.cpp).
    namespace players {
        void create_tab();
    }

    // Games tab: the Rush 2049 and SF Rush ROMs and what they add (src/games_tab.cpp).
    namespace games {
        void create_tab();
    }
}

#endif
