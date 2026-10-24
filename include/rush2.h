#ifndef __RUSH2_H__
#define __RUSH2_H__

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "control_profiles.h"

namespace recompui {
    class ContextId;
    class Element;
}

namespace rush2 {
    inline const std::u8string program_id = u8"Rush2Recompiled";
    inline const std::string program_name = "Rush 2: Recompiled";

    // Whether a game pointer is in the game's 16 MB of RDRAM (mod memory starts at 0x81000000). The game heap reaches
    // 0x80AE0000 (src/assets.cpp), so a check against the old 8 MB end rejects data a big track pushes past 0x80800000.
    inline bool in_game_ram(uint32_t address) {
        return address >= 0x80000000u && address < 0x81000000u;
    }

    void register_overlays();

    // Creates the config menu tabs. Must be called before recomp::start.
    void init_config();

    // Forces every model to its most detailed LOD and turns off LOD distance culling (src/lod.cpp).
    void set_lod_disabled(bool disabled);

    // Stunt Respawn in 1st Gear (src/game_fixes.cpp): stunt mode respawns put the human cars in 1st gear, not 2nd.
    void set_stunt_first_gear(bool enabled);
    // Hide Respawning Cars (src/game_fixes.cpp): a computer car is hidden through its glide back after a wreck.
    void set_hide_respawning_cars(bool enabled);
    // Hood View Front Wheels (src/game_fixes.cpp): the open-wheel cars show their front wheels in the hood camera.
    void set_hood_front_wheels(bool enabled);
    // Chase Camera (src/camera.cpp): the VIEW button also reaches the attract demo's high chase camera.
    void set_attract_camera(bool enabled);
    // Look Back (src/camera.cpp): the LOOK BACK button and its Controller Setup row.
    void set_look_back(bool enabled);
    bool look_back_enabled();
    // LOOK BACK turns a view's camera only while the frame is drawn (src/camera.cpp): before the view is drawn, and back
    // after the frame's views and HUD.
    void look_back_draw_view(uint8_t* rdram, int view);
    void look_back_draw_end(uint8_t* rdram);
    // Car model fixes (src/game_fixes.cpp): replaces the ROCKET's asset with its windshield unpainted, the FORMULA's
    // with its wishbones mirrored and the STALLION's with its spoiler centered. Call at boot.
    void fix_car_models(uint8_t* rdram);
    // Draw distance factor (src/draw_distance.cpp): 1 is the original. Scales the projection's far plane, the fog and
    // the placed object and LOD cull distances; above 1 it draws every track section but the distant stand-ins
    // regardless of visibility and gives nodes past the fixed-point matrix range float matrices.
    void set_draw_distance(float factor);
    float draw_distance();
    // Visibility hook helper, after the region's mask is chosen: when the draw distance is extended, widens it to
    // every section but the distant stand-ins. table = the track's region mask table (0 = unknown).
    void draw_distance_pvs(uint8_t* rdram, uint32_t sp, uint32_t table);
    // True (once) if the node matrix at addr was written as floats, so its G_MTX must become a gEXMatrixFloat.
    bool draw_distance_take_float_matrix(uint32_t addr);
    // True (once per view) if the view's fog must be replaced by these float factors (gEXSetFogFloat), emitted after
    // the extended GBI is enabled.
    bool draw_distance_take_fog(float& mul, float& offset);

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
        // The Split Screen option (src/config.cpp, shown in the Controls tab); setting it saves it.
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
    // Draws scene node `node` under `leader`'s interpolation ID (alias the leader to itself too): nodes shown one at a
    // time in the same place, such as a custom track's flip-book frames, so the one shown pairs with the one it
    // replaces. Cleared when a track's moving objects are set up.
    void interpolation_clear_node_aliases();
    void interpolation_node_alias(uint32_t node, uint32_t leader);

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
        // Draws widget `slot` (dx, dy) pixels from where it is, past the 4:3 edges if need be (the game clips widgets to
        // the 4:3 screen, so one fully on screen is shifted when drawn). Forgotten when its slot is allocated again.
        void set_widget_shift(int slot, int32_t dx, int32_t dy = 0);
        void clear_widget_scales();
        void set_anchor(uint8_t* rdram, float fraction);
        // A filled rectangle (screen pixels, RGBA blended by its alpha) in the 2D display list, anchored as above.
        void draw_rect(uint8_t* rdram, float x0, float y0, float x1, float y1, uint32_t rgba, float anchor);
        // An RGBA16 image of w x h texels at `address` (at most 2048 texels) over that rectangle, blended by its alpha.
        void draw_image(uint8_t* rdram, uint32_t address, int w, int h, float x0, float y0, float x1, float y1, float anchor);
        // The part of it src_w x src_h texels from (src_x, src_y) over that rectangle, point sampled if `point`, its
        // alpha scaled by `alpha` (0-255).
        void draw_image_part(uint8_t* rdram, uint32_t address, int w, int h, int src_x, int src_y, int src_w, int src_h,
                             float x0, float y0, float x1, float y1, float anchor, bool point = false, uint8_t alpha = 255);
        // The part of an I4 image of w x h texels at `address` (w a multiple of 16, at most 8192 texels) over that
        // rectangle: its color goes from `shade` to `rgba` (RGBA8888) with the texel's intensity, its alpha is the
        // texel's times rgba's. The image is loaded whole and clamped, so a texture pack can replace it by its hash
        // (the high-resolution fonts' Rush 2049 sheets, tools/build_font_pack.py).
        void draw_i4_part(uint8_t* rdram, uint32_t address, int w, int h, int src_x, int src_y, int src_w, int src_h,
                          float x0, float y0, float x1, float y1, float anchor, uint32_t rgba, uint32_t shade);
        // Calls display list `dl` from the 2D display list, with the render state (other mode, combiner, geometry mode,
        // primitive color, viewport and projection) put back after it.
        void call_list(uint8_t* rdram, uint32_t dl);
        // Limits what is drawn next to a rectangle of the 4:3 screen anchored at `anchor` (as set_widget_scale), until
        // pop_scissor.
        void push_scissor(uint8_t* rdram, float x0, float y0, float x1, float y1, float anchor);
        void pop_scissor(uint8_t* rdram);
        // Leaves the primitive color at rgba after the 2D drawing so far (G_SETPRIMCOLOR in the 2D display list).
        void set_prim_color(uint8_t* rdram, uint32_t rgba);
        // Draws a number (up to 6 digits) centered on (center_x, center_y) of the 4:3 screen, `height` pixels tall,
        // white with a shadow, anchored at `anchor` (as set_widget_scale). After the widget loop.
        void draw_number(uint8_t* rdram, const char* digits, float center_x, float center_y, float height, float anchor);
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
        // No Subways or Buses: no trains or SF Rush buses at the next race setup.
        bool no_trains();
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
        // The player (0-3) on a port, or -1: the game's player records, none on the title screen.
        int port_player(int port);
        // The title screen is up (backing out of the main menu, or at boot) until START is pressed on it: every port
        // is back to no player, and whoever presses START next becomes player 1 (src/players4.cpp).
        void set_at_title(bool at_title);
        bool get_n64_input(int port, uint16_t* buttons, float* x, float* y);
        // Scripted presses (--input-script) of GAS (A) or BRAKE (B) count as fully pressed pedals.
        void press_pedals(int port, uint16_t buttons);
        // How far GAS and BRAKE were pressed (0-1) in the input get_n64_input last gave for a port.
        void get_pedals(int port, float* gas, float* brake);
        // Steering response, reverse, the stick deadzone and rumble are per profile (include/control_profiles.h).
        // Plays one effect on the controller last used with a profile's settings, from the Controls tab: a crash on
        // the left or right, or a second and a half of tire screech or wall grinding (left, then right).
        enum class RumbleTest { LeftCrash, RightCrash, Screech, Grind };
        void test_rumble(RumbleTest test, const rush2::control_profiles::Settings& settings);
        // The side the port's car is cornering hard on this tick (-1 right to 1 left, 0 = not), from src/rumble.cpp.
        void set_rumble_cornering(int port, float side);
        // The side of a wall the port's car is grinding along this tick and how hard (0 to 1, by its speed along the
        // wall; 0 = not touching one), from src/rumble.cpp.
        void set_rumble_grinding(int port, float side, float intensity);
        bool rumble_debug_enabled(); // --debug-rumble
        // Prints each impact and the side levels in Directional mode to the console (--debug-rumble), to tune it.
        void set_rumble_debug(bool enabled);
        // Sends no rumble or haptics to any controller: test runs (sim runs, --input-script) that would otherwise
        // vibrate the controller of the user working at the machine.
        void set_rumble_muted(bool muted);
        // Side of the latest impact on a port's car: -1 = right, 1 = left, near 0 = head on. A hard impact (wall,
        // car) rumbles for longer and isn't overridden by the soft ones from the tires sliding.
        void set_rumble_impact(int port, float side, bool hard);
        // Whether any port rumbles Directional, the only mode that uses set_rumble_impact.
        bool rumble_directional();
        // True once after a race ends, a pause or a change out of Directional: the impact hook's previous tick is stale.
        bool take_rumble_impact_reset(int port);
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
            bool dualsense; // Has haptics actuators (include/dualsense_haptics.h).
        };
        // Connected controllers, in SDL's order.
        std::vector<ControllerInfo> get_controllers();
        // Held around everything that enumerates SDL controllers or pumps SDL events (src/input.cpp explains why).
        std::recursive_mutex& sdl_device_mutex();

        // Controllers turned off in the Controls tab (they never take a port), remembered by key.
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
        // Whether RUSH2_TEST_VPAD's virtual controller drives the port (a simulation run reads it like a real one).
        bool is_test_vpad_port(int port);
        // The port the keyboard drives, or -1 for none (off, or it hasn't pressed anything yet).
        int get_keyboard_port();
        bool get_keyboard_enabled();
        void set_keyboard_enabled(bool enabled);
        // True if the port's controller is a PlayStation controller (for button glyphs).
        bool port_has_playstation_controller(int port);

        // Blocks a port's game input from now until every input on its devices is released (after the Controller
        // Setup screen captures a binding, so the press that was bound isn't also seen by the game).
        void suppress_until_released(int port);
        // Whether the game takes input now: not while the recomp menu has it, or while the window is unfocused with
        // Background Input off.
        bool takes_input();
    }

    // Button bindings for driving (src/controls.cpp). In races each player's bound inputs are turned into the game's
    // default N64 layout, which the game's own binding table is locked to; menus use a fixed layout.
    namespace controls {
        // The actions of the game's Controller Setup screen. Fire and DropWeapon are the battle arenas' weapon
        // buttons (src/rush2049/battle.cpp), read with battle_buttons, and LookBack turns the camera around while held
        // (src/camera.cpp, look_back_held): none of them are N64 buttons of the game's layout.
        enum class Action : uint8_t {
            Gas, Brake, Steering, ShiftUp, ShiftDown, Reverse, Abort, View, Horn, Wings, Fire, DropWeapon, LookBack, Count
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
        // deadzone is the player's profile's stick deadzone, 0 to 1 of the stick's travel.
        void get_race_input(int port, uint16_t* buttons, float* x, float* y, float steering_exponent, float deadzone,
                            float* gas, float* brake);
        // The fixed menu layout.
        void get_menu_input(int port, uint16_t* buttons, float* x, float* y);
        // The battle arenas' weapon buttons held on a port as of its last race input: battle_fire, battle_drop, and
        // battle_back while the steering stick is held back (the D-pad's down when it steers, the keyboard's down
        // arrow), which fires a weapon backward (rush2::battle::set_fire_backward).
        constexpr uint8_t battle_fire = 1, battle_drop = 2, battle_back = 4;
        uint8_t battle_buttons(int port);
        // Whether LOOK BACK is held on a port, as of its last race input.
        bool look_back_held(int port);
        // True if an input on the port's devices is held (used to wait for releases).
        bool any_input_held(int port);

        // Glyphs of the menu layout's buttons, as port's controller or keyboard shows them (src/controls_menu.cpp),
        // drawn from a widget's text callback: begin, add any number (x, y: top left; w x h pixels, 16x14 the
        // Controller Setup screen's size), end.
        // Y is the menus' C-up (Y or Triangle, I on the keyboard). add_menu_rect fills a rectangle (rgba with alpha)
        // in the same list, under what is added after it.
        enum class MenuButton : uint8_t { A, B, L, R, Y };
        void begin_menu_glyphs(uint8_t* rdram);
        void add_menu_glyph(uint8_t* rdram, int port, MenuButton button, int x, int y, int w, int h);
        void add_menu_rect(uint8_t* rdram, int x, int y, int w, int h, uint32_t rgba);
        void end_menu_glyphs(uint8_t* rdram);
    }

    // Controls tab: controllers, split screen and each profile's control and rumble settings (src/controls_tab.cpp).
    namespace controls_tab {
        void create_tab();
    }

    // Games tab: the Rush 2049 and SF Rush ROMs and what they add (src/games_tab.cpp).
    namespace games {
        void create_tab();
        // The tab's page, for the setup screen's other games step (src/setup.cpp).
        void create_page(recompui::ContextId context, recompui::Element* parent);
    }
}

#endif
