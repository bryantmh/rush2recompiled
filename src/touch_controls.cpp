// On-screen touch controls (include/touch_controls.h).
//
// Touches come from an SDL event filter: it sees each finger event as SDL queues it, on the thread that pumps the
// window's events (the main thread). Each finger holds what it first landed on: a button (sliding onto another button
// moves it there, so a thumb can roll from GAS to BRAKE or around the D-pad) or, in the race layout, the steering stick,
// which floats: it centers where the finger lands anywhere in the left half of the screen below the top quarter. What
// the fingers hold is read by src/controls.cpp, merged into port 1's race or menu input.
//
// SDL also turns the first finger into mouse events (SDL_TOUCH_MOUSEID). Those are always dropped: while the game has
// the touchscreen they'd switch this settings UI to the mouse and show the cursor under the thumb, and with one of its
// menus up (or the option Off) menu_touch makes a finger into a mouse that also scrolls and drags.
//
// The fingers tick player 1's controller (rush2::input::touch_tick): landing on a button or the stick, and the stick
// leaving its center and reaching its rim. In Steam's Game Mode the game asks gamescope for native touch while it runs
// (gamescope_touch below).
//
// The overlay is a recompui context that never takes input, laid out in dp: recompui scales 1 dp to 1/1080 of the
// window's height, so the screen is 1080 dp tall and 1080 * width / height dp wide, and a finger's normalized position
// converts to the same units the controls are placed in. Controls are anchored to the left or right edge (or the
// middle) and the bottom (or top), so wider and narrower windows keep them under the thumbs. The overlay is only
// shown while it's on screen; RmlUi is only touched when something on it changed.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#include <dlfcn.h>
#endif

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "elements/ui_label.h"
#include "librecomp/config.hpp"
#include "ultramodern/ultramodern.hpp"

#include "battle.h"
#include "race_setup.h"
#include "rush2.h"
#include "touch_controls.h"

using rush2::controls::Action;

namespace {
    const std::string option = "touch_controls";

    std::atomic<rush2::touch::Mode> mode = rush2::touch::Mode::Auto;

    // Main thread (update) to the event filter: the game has the touchscreen (the option isn't Off, the game is
    // running and none of this settings UI's menus or prompts are up).
    std::atomic_bool game_owns_touch = false;

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

    constexpr float screen_dp = 1080.0f; // The window's height in dp.

    enum class Layout { Race, Menu };
    enum class Kind { Action, Button, Menu };
    enum class HAnchor { Left, Right, Center };
    enum class Only { Always, Battle, LookBack, Manual };

    // One button. cx is the distance of its center from its horizontal anchor (Center: from the middle, + right), cy
    // from the bottom (top_anchored: from the top), all in dp. Square ones are circles, the others pills.
    struct Control {
        Layout layout;
        Kind kind;
        int value;          // Kind::Action: the rush2::controls::Action; Kind::Button: N64 buttons.
        const char* label;
        HAnchor h;
        float cx, cy, w, h_dp;
        bool top_anchored = false;
        Only only = Only::Always;
    };

    constexpr int act(Action a) { return static_cast<int>(a); }

    // Sizes and places were picked for thumbs on a 7" 16:10 screen (a Steam Deck's): the pedals biggest, nearest the
    // right thumb's rest; the buttons used less often further in; nothing under the middle of the screen. The race
    // layout keeps clear of the race HUD's corners at 16:9 and 4:3: the timer (top left), speedometer (top middle),
    // position and lap (top right), tachometer (left, from 280 to 600 dp down) and the map (right, out to 312 dp from
    // the edge, from 300 to 744 dp down), so the buttons on the right sit inward of the map and the left ones under the
    // tachometer.
    const std::vector<Control> touch_buttons = {
        // Race: the right thumb's pedals and the buttons above and left of them.
        { Layout::Race, Kind::Action, act(Action::Gas), "GAS", HAnchor::Right, 160, 190, 220, 220 },
        { Layout::Race, Kind::Action, act(Action::Brake), "BRAKE", HAnchor::Right, 400, 130, 170, 170 },
        { Layout::Race, Kind::Action, act(Action::ShiftUp), "SHIFT ▲", HAnchor::Right, 390, 330, 110, 110, false, Only::Manual },
        { Layout::Race, Kind::Action, act(Action::ShiftDown), "SHIFT ▼", HAnchor::Right, 520, 260, 110, 110, false, Only::Manual },
        { Layout::Race, Kind::Action, act(Action::View), "VIEW", HAnchor::Right, 400, 470, 110, 110 },
        { Layout::Race, Kind::Action, act(Action::Wings), "WINGS", HAnchor::Right, 530, 400, 110, 110 },
        { Layout::Race, Kind::Action, act(Action::Fire), "FIRE", HAnchor::Right, 660, 170, 130, 130, false, Only::Battle },
        { Layout::Race, Kind::Action, act(Action::DropWeapon), "DROP", HAnchor::Right, 670, 330, 100, 100, false, Only::Battle },
        // Race: the left thumb's buttons, between the tachometer and the stick.
        { Layout::Race, Kind::Action, act(Action::Reverse), "REVERSE", HAnchor::Left, 90, 425, 100, 100 },
        { Layout::Race, Kind::Action, act(Action::Horn), "HORN", HAnchor::Left, 210, 425, 100, 100 },
        { Layout::Race, Kind::Action, act(Action::Abort), "ABORT", HAnchor::Left, 330, 425, 100, 100 },
        { Layout::Race, Kind::Action, act(Action::LookBack), "LOOK", HAnchor::Left, 450, 425, 100, 100, false, Only::LookBack },
        // Race: between the timer and the speedometer, and the speedometer and the position.
        { Layout::Race, Kind::Button, n64_start, "PAUSE", HAnchor::Left, 470, 50, 150, 56, true },
        { Layout::Race, Kind::Menu, 0, "MENU", HAnchor::Right, 470, 50, 150, 56, true },
        // Menus: the D-pad on the left, A and B on the right.
        { Layout::Menu, Kind::Button, n64_dpad_up, "▲", HAnchor::Left, 230, 380, 120, 120 },
        { Layout::Menu, Kind::Button, n64_dpad_down, "▼", HAnchor::Left, 230, 140, 120, 120 },
        { Layout::Menu, Kind::Button, n64_dpad_left, "◀", HAnchor::Left, 110, 260, 120, 120 },
        { Layout::Menu, Kind::Button, n64_dpad_right, "▶", HAnchor::Left, 350, 260, 120, 120 },
        { Layout::Menu, Kind::Button, n64_l, "L", HAnchor::Left, 110, 480, 130, 60 },
        { Layout::Menu, Kind::Button, n64_z, "Z", HAnchor::Left, 260, 480, 130, 60 },
        { Layout::Menu, Kind::Button, n64_a, "A", HAnchor::Right, 160, 220, 170, 170 },
        { Layout::Menu, Kind::Button, n64_b, "B", HAnchor::Right, 360, 140, 140, 140 },
        { Layout::Menu, Kind::Button, n64_c_up, "C▲", HAnchor::Right, 380, 330, 100, 100 },
        { Layout::Menu, Kind::Button, n64_start, "START", HAnchor::Right, 180, 420, 170, 64 },
        { Layout::Menu, Kind::Button, n64_r, "R", HAnchor::Right, 380, 470, 130, 60 },
        { Layout::Menu, Kind::Menu, 0, "MENU", HAnchor::Left, 110, 42, 150, 56, true },
    };

    // The steering stick (race layout): its base's diameter, its knob's, and where it rests (from the left and bottom).
    constexpr float stick_size = 300.0f;
    constexpr float knob_size = 130.0f;
    constexpr float stick_rest_x = 230.0f;
    constexpr float stick_rest_y = 220.0f;
    // Under this much of the stick's travel it reads 0.
    constexpr float stick_deadzone = 0.03f;
    // The stick's ticks on player 1's controller (rush2::input::touch_tick): a finger landing on a button or the stick,
    // the stick leaving its center and reaching its rim.
    constexpr float press_tick = 0.5f;
    constexpr float grab_tick = 0.35f;
    constexpr float leave_center_tick = 0.2f;
    constexpr float rim_tick = 0.4f;
    constexpr float rim = 0.97f;
    // Where a finger can land to steer: the left half of the screen, below the top quarter.
    constexpr float stick_zone_width = 0.5f;
    constexpr float stick_zone_top = 0.25f;
    // A button reacts to a finger a little outside it.
    constexpr float hit_slack = 1.15f;

    // Colors: dark translucent buttons with a light rim, filled in light while held.
    constexpr recompui::Color fill_color{ 0, 0, 0, 90 };
    constexpr recompui::Color held_color{ 255, 255, 255, 110 };
    constexpr recompui::Color rim_color{ 255, 255, 255, 140 };
    constexpr recompui::Color text_color{ 255, 255, 255, 220 };
    constexpr recompui::Color stick_fill_color{ 0, 0, 0, 70 };
    constexpr recompui::Color knob_color{ 255, 255, 255, 120 };

    constexpr int no_control = -1;
    constexpr int stick_control = -2;

    struct Finger {
        SDL_FingerID id;
        int control;    // An index into touch_buttons, stick_control or no_control (landed on nothing, or the layout changed).
    };

    // Everything the fingers do, under touch_mutex: written by the event filter and update, read by held.
    std::mutex touch_mutex;
    std::vector<Finger> fingers;
    Layout layout = Layout::Menu;
    bool battle_on = false;
    bool look_back = false;
    bool manual = false;        // Player 1 shifts gears (TRANSMISSION manual): the SHIFT buttons show.
    bool touched = false;       // Auto: touched since a controller or the keyboard was last used.
    float stick_cx = 0.0f, stick_cy = 0.0f;   // The held stick's center, dp from the left and the top.
    float stick_x = 0.0f, stick_y = 0.0f;     // Its knob, -1 to 1 each way (y down).
    int stick_zone = 0;         // Where the knob is, for the ticks: 0 in the deadzone, 1 out of it, 2 at the rim.
    rush2::touch::Held held_state;
    bool dirty = true;          // The overlay needs redrawing.

    bool shown(const Control& c) {
        return c.layout == layout && (c.only == Only::Always || (c.only == Only::Battle && battle_on) ||
            (c.only == Only::LookBack && look_back) || (c.only == Only::Manual && manual));
    }

    // A control's center in dp from the screen's left and top, on a screen `width` dp wide.
    void center(const Control& c, float width, float* x, float* y) {
        *x = c.h == HAnchor::Left ? c.cx : c.h == HAnchor::Right ? width - c.cx : width / 2.0f + c.cx;
        *y = c.top_anchored ? c.cy : screen_dp - c.cy;
    }

    // The control under a point (dp), or no_control: the nearest one the point is on, counting the slack.
    int hit(float x, float y, float width) {
        int best = no_control;
        float best_d = 0.0f;
        for (int i = 0; i < (int)touch_buttons.size(); i++) {
            const Control& c = touch_buttons[i];
            if (!shown(c)) continue;
            float cx, cy;
            center(c, width, &cx, &cy);
            // Distance in half-sizes: under 1 is on it. A pill counts as its box.
            float dx = std::fabs(x - cx) / (c.w / 2.0f * hit_slack);
            float dy = std::fabs(y - cy) / (c.h_dp / 2.0f * hit_slack);
            float d = c.w == c.h_dp ? std::sqrt(dx * dx + dy * dy) : std::max(dx, dy);
            if (d <= 1.0f && (best == no_control || d < best_d)) {
                best = i;
                best_d = d;
            }
        }
        return best;
    }

    void set_stick(float x, float y) {
        float r = stick_size / 2.0f;
        float dx = (x - stick_cx) / r;
        float dy = (y - stick_cy) / r;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.0f) {
            dx /= len;
            dy /= len;
        }
        stick_x = dx;
        stick_y = dy;
        // A tick as the knob leaves the center and as it reaches the rim, like a stick's notches.
        len = std::min(len, 1.0f);
        int zone = len >= rim ? 2 : len >= stick_deadzone ? 1 : 0;
        if (zone > stick_zone) {
            rush2::input::touch_tick(rush2::touch::port, zone == 2 ? rim_tick : leave_center_tick);
        }
        stick_zone = zone;
    }

    // Under touch_mutex: what the fingers hold now.
    void update_held() {
        rush2::touch::Held h;
        for (const Finger& f : fingers) {
            if (f.control == stick_control) {
                auto dead = [](float v) {
                    return std::fabs(v) < stick_deadzone ? 0.0f : std::copysign((std::fabs(v) - stick_deadzone) / (1.0f - stick_deadzone), v);
                };
                h.x = dead(stick_x);
                h.y = -dead(stick_y);
            }
            else if (f.control >= 0) {
                const Control& c = touch_buttons[f.control];
                if (c.kind == Kind::Action) h.actions |= 1u << c.value;
                if (c.kind == Kind::Button) h.buttons |= (uint16_t)c.value;
            }
        }
        held_state = h;
        dirty = true;
    }

    void release_all() {
        fingers.clear();
        stick_x = stick_y = 0.0f;
        update_held();
    }

    void open_menu() {
        recompui::queue_ui_task([]() { recompui::config::open(); });
    }

    // The screen's width in dp for a window.
    float window_width_dp(Uint32 window_id) {
        SDL_Window* window = SDL_GetWindowFromID(window_id);
        int w = 0, h = 0;
        if (window != nullptr) {
            SDL_GetWindowSize(window, &w, &h);
        }
        return h > 0 ? screen_dp * w / h : screen_dp * 16.0f / 9.0f;
    }

    // The touch device of RUSH2_TEST_TOUCH's fingers.
    constexpr SDL_TouchID test_touch_id = 0x52325443;
    std::atomic_bool test_touch = false;

    // The mouse's left button is a finger too (one at a time) while the controls are on screen, so they can be clicked.
    // Steam's Game Mode (gamescope) also turns touchscreen taps into the left button unless the game's controller layout
    // has Touchscreen Native Support on, so the game never sees a finger: under gamescope the left button is a finger
    // even while the controls are hidden, and shows them in Auto (steering and the pedals together need native touch).
    const bool under_gamescope = std::getenv("GAMESCOPE_WAYLAND_DISPLAY") != nullptr;
    constexpr SDL_TouchID mouse_touch_id = 0x52324D53;
    constexpr SDL_FingerID mouse_finger_id = 0x7FFF0000;
    bool mouse_down = false;    // The event filter's: the left button went down on the game as a finger.
    std::atomic_bool overlay_on_screen = false;     // Main thread (update) to the event filter.

    // While the game doesn't have the touchscreen (this settings UI's menus, the launcher, or the option Off), one finger
    // is a mouse: a tap clicks, a drag up or down scrolls (as a mouse wheel: RmlUi doesn't scroll on a drag) and a drag
    // sideways holds the button (sliders). SDL's own mouse events from touches (SDL_TOUCH_MOUSEID) are dropped instead:
    // they only click, and recompui skips a move within 100 px of the cursor after a controller was used, so the click
    // landed where the cursor had been.
    enum class Gesture { None, Pending, Drag, Scroll };
    struct MenuFinger {
        SDL_FingerID id = 0;
        Gesture gesture = Gesture::None;
        Uint32 window = 0;
        int x0 = 0, y0 = 0;     // Where it landed, in window pixels.
        float scrolled_y = 0;   // Scroll: where the last wheel step was taken, in window pixels.
    };
    std::mutex menu_mutex;
    MenuFinger menu_finger;
    // How far a finger moves before a tap becomes a drag, and how far a scroll moves per wheel step, in dp.
    constexpr float tap_slop = 16.0f;
    constexpr float scroll_step = 45.0f;

    SDL_Event mouse_motion(Uint32 window, int x, int y, bool held) {
        SDL_Event m{};
        m.motion.type = SDL_MOUSEMOTION;
        m.motion.windowID = window;
        m.motion.state = held ? SDL_BUTTON_LMASK : 0;
        m.motion.x = x;
        m.motion.y = y;
        return m;
    }

    SDL_Event mouse_button(Uint32 window, int x, int y, bool down) {
        SDL_Event b{};
        b.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        b.button.windowID = window;
        b.button.button = SDL_BUTTON_LEFT;
        b.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        b.button.clicks = 1;
        b.button.x = x;
        b.button.y = y;
        return b;
    }

    // Whether a finger is the menus' (handled here, pushing mouse events).
    bool menu_touch(const SDL_TouchFingerEvent& e) {
        std::vector<SDL_Event> out;
        {
            std::lock_guard lock{ menu_mutex };
            MenuFinger& m = menu_finger;
            bool mine = m.gesture != Gesture::None && m.id == e.fingerId;
            if (!mine) {
                // A new finger is the menus' only while the game doesn't have the touchscreen, and one at a time.
                if (e.type != SDL_FINGERDOWN || game_owns_touch.load()) {
                    return !game_owns_touch.load();
                }
                if (m.gesture != Gesture::None) {
                    return true;
                }
            }
            SDL_Window* window = SDL_GetWindowFromID(e.windowID);
            int w = 0, h = 0;
            if (window != nullptr) {
                SDL_GetWindowSize(window, &w, &h);
            }
            if (w <= 0 || h <= 0) {
                return true;
            }
            int x = (int)(e.x * w);
            int y = (int)(e.y * h);
            float px_per_dp = h / screen_dp;
            if (e.type == SDL_FINGERDOWN) {
                m = MenuFinger{ e.fingerId, Gesture::Pending, e.windowID, x, y, (float)y };
                // Off the window first, so the move to the finger is more than 100 px and recompui takes it.
                out.push_back(mouse_motion(m.window, -10000, -10000, false));
                out.push_back(mouse_motion(m.window, x, y, false));
            }
            else if (e.type == SDL_FINGERMOTION) {
                float dx = (float)(x - m.x0), dy = (float)(y - m.y0);
                if (m.gesture == Gesture::Pending && std::sqrt(dx * dx + dy * dy) > tap_slop * px_per_dp) {
                    if (std::fabs(dy) >= std::fabs(dx)) {
                        m.gesture = Gesture::Scroll;
                    }
                    else {
                        m.gesture = Gesture::Drag;
                        out.push_back(mouse_button(m.window, m.x0, m.y0, true));
                    }
                }
                if (m.gesture == Gesture::Drag) {
                    out.push_back(mouse_motion(m.window, x, y, true));
                }
                else if (m.gesture == Gesture::Scroll) {
                    // The page follows the finger: dragging down shows what's above (a wheel turned up).
                    float step = scroll_step * px_per_dp;
                    while (std::fabs(y - m.scrolled_y) >= step) {
                        int dir = y > m.scrolled_y ? 1 : -1;
                        SDL_Event wheel{};
                        wheel.wheel.type = SDL_MOUSEWHEEL;
                        wheel.wheel.windowID = m.window;
                        wheel.wheel.y = dir;
                        wheel.wheel.preciseY = (float)dir;
                        wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
                        out.push_back(wheel);
                        m.scrolled_y += dir * step;
                    }
                }
            }
            else {
                if (m.gesture == Gesture::Pending) {
                    out.push_back(mouse_button(m.window, m.x0, m.y0, true));
                    out.push_back(mouse_button(m.window, m.x0, m.y0, false));
                }
                else if (m.gesture == Gesture::Drag) {
                    out.push_back(mouse_button(m.window, x, y, false));
                }
                m.gesture = Gesture::None;
            }
        }
        for (SDL_Event& ev : out) {
            // A wheel event goes straight to this UI's queue: pushed through SDL's (SDL 2.32's on SDL 3), its steps
            // came out as its mouseY.
            if (ev.type == SDL_MOUSEWHEEL) {
                recompui::queue_event(ev);
            }
            else {
                SDL_PushEvent(&ev);
            }
        }
        return true;
    }

    void on_finger(const SDL_TouchFingerEvent& e) {
        // Only touchscreens: a trackpad's fingers (macOS sends them as touches too) move the mouse.
        if (SDL_GetTouchDeviceType(e.touchId) != SDL_TOUCH_DEVICE_DIRECT && e.touchId != test_touch_id &&
            e.touchId != mouse_touch_id) {
            return;
        }
        if (e.touchId != mouse_touch_id && menu_touch(e)) {
            return;
        }
        std::lock_guard lock{ touch_mutex };
        float width = window_width_dp(e.windowID);
        float x = e.x * width;
        float y = e.y * screen_dp;
        auto found = std::find_if(fingers.begin(), fingers.end(), [&](const Finger& f) { return f.id == e.fingerId; });

        if (e.type == SDL_FINGERDOWN) {
            if (!game_owns_touch.load()) {
                return;
            }
            touched = true;
            Finger f{ e.fingerId, hit(x, y, width) };
            if (f.control >= 0) {
                rush2::input::touch_tick(rush2::touch::port, press_tick);
            }
            if (f.control >= 0 && touch_buttons[f.control].kind == Kind::Menu) {
                open_menu();
                f.control = no_control;
            }
            bool stick_free = std::none_of(fingers.begin(), fingers.end(), [](const Finger& o) { return o.control == stick_control; });
            if (f.control == no_control && layout == Layout::Race && stick_free && x < width * stick_zone_width &&
                y > screen_dp * stick_zone_top) {
                // The stick centers under the finger, even by an edge (drawn partly off screen): moved inward, it
                // would start off center and need a long slide back before steering the other way.
                stick_cx = x;
                stick_cy = y;
                stick_zone = 0;
                f.control = stick_control;
                set_stick(x, y);
                rush2::input::touch_tick(rush2::touch::port, grab_tick);
            }
            if (found != fingers.end()) {
                *found = f;
            }
            else {
                fingers.push_back(f);
            }
            update_held();
        }
        else if (e.type == SDL_FINGERMOTION) {
            if (found == fingers.end()) {
                return;
            }
            if (found->control == stick_control) {
                set_stick(x, y);
                update_held();
            }
            else if (found->control >= 0) {
                // Sliding onto another button moves the finger to it; off every button it keeps the one it had.
                int over = hit(x, y, width);
                if (over >= 0 && over != found->control && touch_buttons[over].kind != Kind::Menu) {
                    found->control = over;
                    rush2::input::touch_tick(rush2::touch::port, press_tick);
                    update_held();
                }
            }
        }
        else if (found != fingers.end()) {
            bool was_stick = found->control == stick_control;
            fingers.erase(found);
            if (was_stick) {
                stick_x = stick_y = 0.0f;
            }
            update_held();
        }
    }

    // A controller or the keyboard was used: Auto hides the controls until the screen is touched again.
    void on_device_used() {
        std::lock_guard lock{ touch_mutex };
        if (touched && fingers.empty()) {
            touched = false;
            dirty = true;
        }
    }

    // The left button under gamescope as a finger (`type` SDL_FINGERDOWN, MOTION or UP) at x, y in the window.
    void mouse_finger(Uint32 type, Uint32 window_id, int x, int y) {
        SDL_Window* window = SDL_GetWindowFromID(window_id);
        int w = 0, h = 0;
        if (window != nullptr) {
            SDL_GetWindowSize(window, &w, &h);
        }
        if (w <= 0 || h <= 0) {
            return;
        }
        SDL_TouchFingerEvent f{};
        f.type = type;
        f.touchId = mouse_touch_id;
        f.fingerId = mouse_finger_id;
        f.x = (float)x / w;
        f.y = (float)y / h;
        f.pressure = type == SDL_FINGERUP ? 0.0f : 1.0f;
        f.windowID = window_id;
        on_finger(f);
    }

    // Whether a mouse event is the left button taken as a finger (then dropped).
    bool mouse_as_finger(const SDL_Event* e) {
        if (e->type == SDL_MOUSEMOTION) {
            if (e->motion.which == SDL_TOUCH_MOUSEID || !mouse_down) {
                return false;
            }
            mouse_finger(SDL_FINGERMOTION, e->motion.windowID, e->motion.x, e->motion.y);
            return true;
        }
        if (e->button.which == SDL_TOUCH_MOUSEID || e->button.button != SDL_BUTTON_LEFT) {
            return false;
        }
        if (e->type == SDL_MOUSEBUTTONDOWN) {
            if (!game_owns_touch.load() || !(under_gamescope || overlay_on_screen.load())) {
                return false;
            }
            mouse_down = true;
            mouse_finger(SDL_FINGERDOWN, e->button.windowID, e->button.x, e->button.y);
            return true;
        }
        if (!mouse_down) {
            return false;
        }
        mouse_down = false;
        mouse_finger(SDL_FINGERUP, e->button.windowID, e->button.x, e->button.y);
        return true;
    }

    int SDLCALL event_filter(void*, SDL_Event* e) {
        switch (e->type) {
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP:
                on_finger(e->tfinger);
                break;
            // SDL's mouse events from touches never pass: the game's fingers are the controls' and the menus' are
            // made into mouse events by menu_touch.
            case SDL_MOUSEMOTION:
                if (mouse_as_finger(e)) return 0;
                return e->motion.which != SDL_TOUCH_MOUSEID;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (mouse_as_finger(e)) return 0;
                return e->button.which != SDL_TOUCH_MOUSEID;
            case SDL_MOUSEWHEEL:
                return e->wheel.which != SDL_TOUCH_MOUSEID;
            case SDL_KEYDOWN:
                if (!e->key.repeat) on_device_used();
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                on_device_used();
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (std::abs((int)e->caxis.value) > 16384) on_device_used();
                break;
        }
        return 1;
    }

    // Steam's Game Mode: gamescope sends touches to a game as fingers only in its Passthrough touch mode, which it takes
    // from the STEAM_TOUCH_CLICK_MODE property of its X displays' root windows (gamescope's TouchClickModes: 0 hover,
    // 1 left click, 2 right, 3 middle, 4 passthrough, 5 disabled, 6 trackpad). Steam sets it when a game takes focus,
    // from the game's controller layout (Touchscreen Native Support), and otherwise leaves a click. So while the touch
    // controls aren't Off and the game's window has focus, the game sets passthrough itself, checking twice a second in
    // case Steam set it back; when they're turned Off, the window loses focus (the Steam menu) or the game quits, it puts
    // back the mode Steam set. Xlib is loaded at run time, so the AppImage needs no new library.
    std::atomic_bool native_touch_wanted = false;   // Main thread (update) to the thread below.
#ifdef __linux__
    namespace gamescope_touch {
        using Display = void;
        using XID = unsigned long;
        constexpr long passthrough = 4;
        constexpr long left_click = 1;
        constexpr XID xa_cardinal = 6;
        constexpr int prop_mode_replace = 0;

        Display* (*open_display)(const char*);
        XID (*default_root)(Display*);
        XID (*intern_atom)(Display*, const char*, int);
        int (*get_property)(Display*, XID, XID, long, long, int, XID, XID*, int*, unsigned long*, unsigned long*,
                            unsigned char**);
        int (*change_property)(Display*, XID, XID, XID, int, int, const unsigned char*, int);
        int (*flush)(Display*);
        int (*x_free)(void*);

        struct Connection {
            Display* display;
            XID root, atom;
        };
        std::mutex mutex;
        std::vector<Connection> connections;
        bool set_by_us = false;
        bool stopped = false;           // The game is quitting: Steam's mode is back for good.
        long steam_mode = left_click;   // What Steam last set, put back when the game lets go.

        // The mode on a display, or -1 when it has none.
        long read_mode(const Connection& c) {
            XID type = 0;
            int format = 0;
            unsigned long count = 0, after = 0;
            unsigned char* data = nullptr;
            long mode = -1;
            if (get_property(c.display, c.root, c.atom, 0, 1, 0, xa_cardinal, &type, &format, &count, &after, &data) == 0 &&
                data != nullptr && format == 32 && count == 1) {
                mode = *(const long*)data;
            }
            if (data != nullptr) {
                x_free(data);
            }
            return mode;
        }

        void write_mode(const Connection& c, long mode) {
            change_property(c.display, c.root, c.atom, xa_cardinal, 32, prop_mode_replace, (const unsigned char*)&mode, 1);
            flush(c.display);
        }

        // Sets passthrough on every display (want), or puts Steam's mode back if the game set it.
        void apply(bool want, bool stop = false) {
            std::lock_guard lock{ mutex };
            if (stopped) {
                return;
            }
            stopped = stop;
            if (want && !stop) {
                for (const Connection& c : connections) {
                    long mode = read_mode(c);
                    if (mode != passthrough) {
                        if (mode >= 0) {
                            steam_mode = mode;
                        }
                        write_mode(c, passthrough);
                    }
                }
                set_by_us = true;
            }
            else if (set_by_us) {
                for (const Connection& c : connections) {
                    write_mode(c, steam_mode);
                }
                set_by_us = false;
            }
        }

        // Under gamescope: opens its displays (the game's and Steam's, :0) and keeps the mode set while it's wanted.
        void start() {
            void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
            if (x11 == nullptr) {
                return;
            }
            open_display = (decltype(open_display))dlsym(x11, "XOpenDisplay");
            default_root = (decltype(default_root))dlsym(x11, "XDefaultRootWindow");
            intern_atom = (decltype(intern_atom))dlsym(x11, "XInternAtom");
            get_property = (decltype(get_property))dlsym(x11, "XGetWindowProperty");
            change_property = (decltype(change_property))dlsym(x11, "XChangeProperty");
            flush = (decltype(flush))dlsym(x11, "XFlush");
            x_free = (decltype(x_free))dlsym(x11, "XFree");
            if (!open_display || !default_root || !intern_atom || !get_property || !change_property || !flush || !x_free) {
                return;
            }
            std::vector<std::string> names;
            if (const char* display = std::getenv("DISPLAY")) {
                names.push_back(display);
            }
            if (std::find(names.begin(), names.end(), ":0") == names.end()) {
                names.push_back(":0");
            }
            for (const std::string& name : names) {
                if (Display* d = open_display(name.c_str())) {
                    connections.push_back({ d, default_root(d), intern_atom(d, "STEAM_TOUCH_CLICK_MODE", 0) });
                }
            }
            if (connections.empty()) {
                return;
            }
            printf("[Touch] gamescope: native touch while the touch controls are on (%zu displays)\n", connections.size());
            std::atexit([]() { apply(false, true); });
            std::thread([]() {
                for (;;) {
                    apply(native_touch_wanted.load());
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }).detach();
        }
    }
#endif

    // Test aid: RUSH2_TEST_TOUCH="<seconds>:<x>,<y>[><x2>,<y2>][:<hold seconds>];..." touches the screen at those
    // times after launch (the first update, as the window opens), at x, y as fractions of the window's width and
    // height; with >x2,y2 the finger slides there over the first half of the hold (default 0.15 s). An entry starting
    // with m ("m<seconds>:...") is the mouse's left button instead. The fingers go through SDL's queue and the event
    // filter like a touchscreen's. A simulation run (RUSH2_SIM) reads port 1 while
    // it's set, as it does the test pad's. Example, GAS held from 8 s for 6 s and steering right from 9 s at 16:9:
    // RUSH2_TEST_TOUCH="8:0.917,0.824:6;9:0.12,0.8>0.2,0.8:4".
    void start_test_touch() {
        const char* spec = std::getenv("RUSH2_TEST_TOUCH");
        if (spec == nullptr) {
            return;
        }
        struct Touch {
            double time, duration = 0.15;
            float x, y, x2, y2;
            bool mouse = false;
        };
        std::vector<Touch> touches;
        std::string text = spec;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find(';', pos);
            std::string entry = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            Touch t{};
            t.duration = 0.15;
            t.mouse = !entry.empty() && entry[0] == 'm';
            if (t.mouse) {
                entry.erase(0, 1);
            }
            char slide = 0;
            int n = std::sscanf(entry.c_str(), "%lf:%f,%f%c", &t.time, &t.x, &t.y, &slide);
            if (n < 3) {
                continue;
            }
            t.x2 = t.x;
            t.y2 = t.y;
            if (slide == '>') {
                std::sscanf(entry.c_str() + entry.find('>') + 1, "%f,%f", &t.x2, &t.y2);
            }
            size_t hold = entry.find(':', entry.find(':') + 1);
            if (hold != std::string::npos) {
                t.duration = std::atof(entry.c_str() + hold + 1);
            }
            touches.push_back(t);
        }
        test_touch = true;
        printf("[Touch] RUSH2_TEST_TOUCH: %zu touches\n", touches.size());
        auto launch = std::chrono::steady_clock::now();
        std::thread([touches, launch]() {
            std::vector<bool> down(touches.size(), false);
            for (;;) {
                double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - launch).count();
                // The game's window: the largest one.
                Uint32 window_id = 0;
                int area = 0, width = 0, height = 0;
                for (Uint32 id = 1; id < 16; id++) {
                    SDL_Window* w = SDL_GetWindowFromID(id);
                    int ww = 0, wh = 0;
                    if (w != nullptr) SDL_GetWindowSize(w, &ww, &wh);
                    if (ww * wh > area) { area = ww * wh; window_id = id; width = ww; height = wh; }
                }
                for (size_t i = 0; i < touches.size(); i++) {
                    const Touch& t = touches[i];
                    bool on = now >= t.time && now < t.time + t.duration;
                    float k = t.duration > 0.0 ? std::min(1.0, (now - t.time) / (t.duration / 2.0)) : 1.0f;
                    SDL_Event e{};
                    if (!down[i] && !on) continue;
                    if (t.mouse) {
                        int x = (int)((on ? t.x + (t.x2 - t.x) * k : t.x2) * width);
                        int y = (int)((on ? t.y + (t.y2 - t.y) * k : t.y2) * height);
                        if (down[i] && on) {
                            e.motion.type = SDL_MOUSEMOTION;
                            e.motion.windowID = window_id;
                            e.motion.state = SDL_BUTTON_LMASK;
                            e.motion.x = x;
                            e.motion.y = y;
                        }
                        else {
                            e.button.type = on ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                            e.button.windowID = window_id;
                            e.button.button = SDL_BUTTON_LEFT;
                            e.button.state = on ? SDL_PRESSED : SDL_RELEASED;
                            e.button.clicks = 1;
                            e.button.x = x;
                            e.button.y = y;
                        }
                        SDL_PushEvent(&e);
                        down[i] = on;
                        continue;
                    }
                    e.tfinger.type = !down[i] && on ? SDL_FINGERDOWN : down[i] && on ? SDL_FINGERMOTION : SDL_FINGERUP;
                    e.tfinger.touchId = test_touch_id;
                    e.tfinger.fingerId = (SDL_FingerID)i + 1;
                    e.tfinger.x = on ? t.x + (t.x2 - t.x) * k : t.x2;
                    e.tfinger.y = on ? t.y + (t.y2 - t.y) * k : t.y2;
                    e.tfinger.pressure = on ? 1.0f : 0.0f;
                    e.tfinger.windowID = window_id;
                    SDL_PushEvent(&e);
                    down[i] = on;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }).detach();
    }

    // The overlay's elements (main thread).
    struct Overlay {
        recompui::ContextId context = recompui::ContextId::null();
        std::vector<recompui::Element*> buttons; // One per control.
        std::vector<bool> drawn_held;
        std::vector<bool> drawn_shown;
        recompui::Element* stick = nullptr;
        recompui::Element* knob = nullptr;
        bool stick_shown = false;
        bool visible = false;
    };
    Overlay overlay;

    recompui::Element* make_circle(recompui::ContextId context, recompui::Element* parent, float w, float h) {
        using namespace recompui;
        Element* e = context.create_element<Element>(parent);
        e->set_position(Position::Absolute);
        e->set_width(w);
        e->set_height(h);
        e->set_border_radius(std::min(w, h) / 2.0f);
        e->set_border_width(3.0f);
        e->set_border_color(rim_color);
        e->set_background_color(fill_color);
        e->set_display(Display::Flex);
        e->set_align_items(AlignItems::Center);
        e->set_justify_content(JustifyContent::Center);
        return e;
    }

    void create_overlay() {
        using namespace recompui;
        ContextId previous = try_close_current_context();
        ContextId context = create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        Element* root = context.create_element<Element>(context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_right(0);
        root->set_bottom(0);
        root->set_left(0);
        root->set_background_color(theme::color::Transparent);

        overlay.stick = make_circle(context, root, stick_size, stick_size);
        overlay.stick->set_background_color(stick_fill_color);
        overlay.knob = make_circle(context, root, knob_size, knob_size);
        overlay.knob->set_background_color(knob_color);
        overlay.knob->set_border_width(0.0f);
        // Hidden until the race layout shows them (draw), or the menus would have them in the top left corner.
        overlay.stick->set_display(Display::None);
        overlay.knob->set_display(Display::None);

        for (const Control& c : touch_buttons) {
            Element* e = make_circle(context, root, c.w, c.h_dp);
            if (c.h == HAnchor::Left) {
                e->set_left(c.cx - c.w / 2.0f);
            }
            else if (c.h == HAnchor::Right) {
                e->set_right(c.cx - c.w / 2.0f);
            }
            else {
                e->set_left(50.0f, Unit::Percent);
                e->set_margin_left(c.cx - c.w / 2.0f);
            }
            if (c.top_anchored) {
                e->set_top(c.cy - c.h_dp / 2.0f);
            }
            else {
                e->set_bottom(c.cy - c.h_dp / 2.0f);
            }
            Label* label = context.create_element<Label>(e, c.label, LabelStyle::Normal);
            label->set_font_size(std::min(c.w, c.h_dp) >= 160.0f ? 38.0f : std::strlen(c.label) > 6 ? 18.0f : 22.0f);
            label->set_color(text_color);
            e->set_display(Display::None);
            overlay.buttons.push_back(e);
        }
        overlay.drawn_held.assign(touch_buttons.size(), false);
        overlay.drawn_shown.assign(touch_buttons.size(), false);
        context.close();
        overlay.context = context;
        if (previous != ContextId::null()) {
            previous.open();
        }
    }

    // Redraws what changed (main thread, the overlay's context open).
    void draw(const std::vector<bool>& held, const std::vector<bool>& show, bool stick_on, bool stick_held,
              float cx, float cy, float kx, float ky) {
        using namespace recompui;
        for (size_t i = 0; i < touch_buttons.size(); i++) {
            if (show[i] != overlay.drawn_shown[i]) {
                overlay.buttons[i]->set_display(show[i] ? Display::Flex : Display::None);
                overlay.drawn_shown[i] = show[i];
            }
            if (held[i] != overlay.drawn_held[i]) {
                overlay.buttons[i]->set_background_color(held[i] ? held_color : fill_color);
                overlay.drawn_held[i] = held[i];
            }
        }
        if (stick_on != overlay.stick_shown) {
            overlay.stick->set_display(stick_on ? Display::Flex : Display::None);
            overlay.knob->set_display(stick_on ? Display::Flex : Display::None);
            overlay.stick_shown = stick_on;
        }
        if (stick_on) {
            // At rest the stick sits at its place; held, it's where the finger landed (positions from the bottom left
            // corner, so the window's width doesn't matter).
            float x = stick_held ? cx : stick_rest_x;
            float y = stick_held ? screen_dp - cy : stick_rest_y;
            float r = stick_size / 2.0f;
            overlay.stick->set_left(x - stick_size / 2.0f);
            overlay.stick->set_bottom(y - stick_size / 2.0f);
            overlay.knob->set_left(x + kx * r - knob_size / 2.0f);
            overlay.knob->set_bottom(y - ky * r - knob_size / 2.0f);
        }
    }
}

void rush2::touch::add_option(recomp::config::Config& config) {
    config.add_enum_option(
        option,
        "Touch Controls",
        "Buttons drawn over the game for touchscreens. "
        "<recomp-color primary>Auto</recomp-color> shows them when the screen is touched and hides them when a "
        "controller or the keyboard is used. "
        "<recomp-color primary>On</recomp-color> always shows them. "
        "<recomp-color primary>Off</recomp-color> never does. "
        "They control player 1, along with player 1's controller, which ticks as they're pressed. They can also be "
        "clicked with the mouse. Takes effect at once.",
        {
            { Mode::Auto, "Auto", "Auto" },
            { Mode::On, "On", "On" },
            { Mode::Off, "Off", "Off" },
        },
        Mode::Auto
    );
    config.add_option_change_callback(option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            apply(static_cast<Mode>(std::get<uint32_t>(cur_value)));
        });
}

const char* rush2::touch::option_id() {
    return option.c_str();
}

void rush2::touch::apply(Mode m) {
    mode = m;
}

bool rush2::touch::test_active() {
    return test_touch.load();
}

rush2::touch::Held rush2::touch::held() {
    std::lock_guard lock{ touch_mutex };
    return held_state;
}

void rush2::touch::update() {
    static bool filter_installed = false;
    if (!filter_installed) {
        filter_installed = true;
        SDL_SetEventFilter(event_filter, nullptr);
        start_test_touch();
#ifdef __linux__
        if (under_gamescope) {
            gamescope_touch::start();
        }
#endif
    }
    native_touch_wanted = mode.load() != Mode::Off && SDL_GetKeyboardFocus() != nullptr;
    if (!ultramodern::is_game_started()) {
        return;
    }
    if (overlay.context == recompui::ContextId::null()) {
        create_overlay();
    }

    Mode m = mode.load();
    bool owns = m != Mode::Off && !recompui::is_context_capturing_input() && !recompui::is_prompt_open();
    game_owns_touch = owns;
    Layout now = rush2::input::in_race_layout() ? Layout::Race : Layout::Menu;
    uint8_t* rdram = rush2::input::get_rdram();
    bool battle_now = now == Layout::Race && rdram != nullptr && rush2::battle::active(rdram);
    bool look_now = rush2::look_back_enabled();
    bool manual_now = now == Layout::Race && rdram != nullptr && !rush2::race_setup::port_automatic(rdram, rush2::touch::port);

    std::vector<bool> held(touch_buttons.size(), false);
    std::vector<bool> show(touch_buttons.size(), false);
    bool visible, stick_held = false;
    float cx, cy, kx, ky;
    {
        std::lock_guard lock{ touch_mutex };
        if (now != layout || battle_now != battle_on || look_now != look_back || manual_now != manual) {
            // The fingers down stay on nothing until lifted: the controls under them are gone or mean something else.
            for (Finger& f : fingers) f.control = no_control;
            layout = now;
            battle_on = battle_now;
            look_back = look_now;
            manual = manual_now;
            stick_x = stick_y = 0.0f;
            update_held();
        }
        if (!owns && !fingers.empty()) {
            release_all();
        }
        visible = owns && (m == Mode::On || touched);
        if (!dirty && visible == overlay.visible) {
            return;
        }
        dirty = false;
        for (const Finger& f : fingers) {
            if (f.control >= 0) held[f.control] = true;
            stick_held |= f.control == stick_control;
        }
        for (size_t i = 0; i < touch_buttons.size(); i++) {
            show[i] = shown(touch_buttons[i]);
        }
        cx = stick_cx;
        cy = stick_cy;
        kx = stick_x;
        ky = stick_y;
    }

    if (visible) {
        overlay.context.open();
        draw(held, show, now == Layout::Race, stick_held, cx, cy, kx, ky);
        overlay.context.close();
    }
    if (visible != overlay.visible) {
        if (visible) {
            recompui::show_context(overlay.context, "");
        }
        else {
            recompui::hide_context(overlay.context);
        }
        overlay.visible = visible;
        overlay_on_screen = visible;
    }
}
