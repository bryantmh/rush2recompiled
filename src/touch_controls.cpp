// On-screen touch controls (include/touch_controls.h).
//
// Touches come from an SDL event filter: it sees each finger event as SDL queues it, on the thread that pumps the
// window's events (the main thread). Each finger holds what it first landed on: a button (sliding onto another button
// moves it there, so a thumb can roll from GAS to BRAKE or around the D-pad) or, in the race layout, the steering stick,
// which floats: it centers where the finger lands anywhere in the left half of the screen below the top quarter. What
// the fingers hold is read by src/controls.cpp, merged into port 1's race or menu input.
//
// SDL also turns the first finger into mouse events (SDL_TOUCH_MOUSEID). While the game has the touchscreen those are
// dropped, or each touch would switch this settings UI to the mouse and show the cursor under the thumb. With one of
// its menus up (or the option Off) they pass, and the touchscreen clicks the menus like a mouse.
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
#endif

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "elements/ui_label.h"
#include "librecomp/config.hpp"
#include "ultramodern/ultramodern.hpp"

#include "battle.h"
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
    enum class Only { Always, Battle, LookBack };

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
        { Layout::Race, Kind::Action, act(Action::ShiftUp), "SHIFT ▲", HAnchor::Right, 390, 330, 110, 110 },
        { Layout::Race, Kind::Action, act(Action::ShiftDown), "SHIFT ▼", HAnchor::Right, 520, 260, 110, 110 },
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
    constexpr float stick_deadzone = 0.08f;
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
    bool touched = false;       // Auto: touched since a controller or the keyboard was last used.
    float stick_cx = 0.0f, stick_cy = 0.0f;   // The held stick's center, dp from the left and the top.
    float stick_x = 0.0f, stick_y = 0.0f;     // Its knob, -1 to 1 each way (y down).
    rush2::touch::Held held_state;
    bool dirty = true;          // The overlay needs redrawing.

    bool shown(const Control& c) {
        return c.layout == layout && (c.only == Only::Always || (c.only == Only::Battle && battle_on) ||
            (c.only == Only::LookBack && look_back));
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

    void on_finger(const SDL_TouchFingerEvent& e) {
        // Only touchscreens: a trackpad's fingers (macOS sends them as touches too) move the mouse.
        if (SDL_GetTouchDeviceType(e.touchId) != SDL_TOUCH_DEVICE_DIRECT && e.touchId != test_touch_id) {
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
            if (f.control >= 0 && touch_buttons[f.control].kind == Kind::Menu) {
                open_menu();
                f.control = no_control;
            }
            bool stick_free = std::none_of(fingers.begin(), fingers.end(), [](const Finger& o) { return o.control == stick_control; });
            if (f.control == no_control && layout == Layout::Race && stick_free && x < width * stick_zone_width &&
                y > screen_dp * stick_zone_top) {
                // The stick centers under the finger, kept whole on screen.
                float r = stick_size / 2.0f;
                stick_cx = std::clamp(x, r, width - r);
                stick_cy = std::clamp(y, r, screen_dp - r);
                f.control = stick_control;
                set_stick(x, y);
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

    int SDLCALL event_filter(void*, SDL_Event* e) {
        switch (e->type) {
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP:
                on_finger(e->tfinger);
                break;
            case SDL_MOUSEMOTION:
                return !(e->motion.which == SDL_TOUCH_MOUSEID && game_owns_touch.load());
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                return !(e->button.which == SDL_TOUCH_MOUSEID && game_owns_touch.load());
            case SDL_MOUSEWHEEL:
                return !(e->wheel.which == SDL_TOUCH_MOUSEID && game_owns_touch.load());
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

    // Test aid: RUSH2_TEST_TOUCH="<seconds>:<x>,<y>[><x2>,<y2>][:<hold seconds>];..." touches the screen at those
    // times after launch (the first update, as the window opens), at x, y as fractions of the window's width and
    // height; with >x2,y2 the finger slides there over the first half of the hold (default 0.15 s). The fingers go
    // through SDL's queue and the event filter like a touchscreen's. A simulation run (RUSH2_SIM) reads port 1 while
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
                int area = 0;
                for (Uint32 id = 1; id < 16; id++) {
                    SDL_Window* w = SDL_GetWindowFromID(id);
                    int ww = 0, wh = 0;
                    if (w != nullptr) SDL_GetWindowSize(w, &ww, &wh);
                    if (ww * wh > area) { area = ww * wh; window_id = id; }
                }
                for (size_t i = 0; i < touches.size(); i++) {
                    const Touch& t = touches[i];
                    bool on = now >= t.time && now < t.time + t.duration;
                    float k = t.duration > 0.0 ? std::min(1.0, (now - t.time) / (t.duration / 2.0)) : 1.0f;
                    SDL_Event e{};
                    e.tfinger.type = !down[i] && on ? SDL_FINGERDOWN : down[i] && on ? SDL_FINGERMOTION : SDL_FINGERUP;
                    if (!down[i] && !on) continue;
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
        "They control player 1, along with player 1's controller. Takes effect at once.",
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
    }
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

    std::vector<bool> held(touch_buttons.size(), false);
    std::vector<bool> show(touch_buttons.size(), false);
    bool visible, stick_held = false;
    float cx, cy, kx, ky;
    {
        std::lock_guard lock{ touch_mutex };
        if (now != layout || battle_now != battle_on || look_now != look_back) {
            // The fingers down stay on nothing until lifted: the controls under them are gone or mean something else.
            for (Finger& f : fingers) f.control = no_control;
            layout = now;
            battle_on = battle_now;
            look_back = look_now;
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
    }
}
