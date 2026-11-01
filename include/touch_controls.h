#ifndef __TOUCH_CONTROLS_H__
#define __TOUCH_CONTROLS_H__

#include <cstdint>

namespace recomp::config {
    class Config;
}

// On-screen touch controls (src/touch_controls.cpp): a translucent controller drawn over the game for touchscreens
// (a Steam Deck's, a tablet's or a touchscreen laptop's). It drives player 1's port (N64 port 1) alongside whatever
// controller or keyboard plays there, so a Steam Deck's own buttons and its screen work together.
//
// It has two layouts, switched with the input layouts of src/input.cpp: during a race its buttons are the game's
// driving actions (GAS, BRAKE, SHIFT, VIEW... and a floating steering stick), not the inputs bound to them; in the
// menus (and the pause menu) they're N64 buttons (D-pad, A, B, START, L, R, Z, C-up). Both have a MENU button that
// opens this settings menu, which a touchscreen otherwise can't reach (it's Escape or the controller's Back button).
namespace rush2::touch {
    // The Controls tab's Touch Controls option (hot-swappable). It's saved with the general settings (general.json),
    // but the General tab leaves it out (src/config.cpp shown_in_other_tab).
    // Auto shows the controls once the screen is touched and hides them again when a controller or the keyboard is
    // used; Off ignores the touchscreen in game (it still clicks this settings menu).
    enum class Mode : uint32_t { Auto, On, Off };
    void add_option(recomp::config::Config& config);
    const char* option_id();
    // Sets the mode from the loaded config (a fresh install loads without calling option change callbacks).
    void apply(Mode mode);

    // Main thread, from update_gfx after the window's events are handled: installs the touch event filter on its first
    // call, then shows, hides and redraws the overlay.
    void update();

    // The port the touch controls drive.
    constexpr int port = 0;

    // What the touch controls hold now (nothing while they're hidden or a menu of this settings UI is up).
    struct Held {
        uint32_t actions = 0;   // Race layout: a bit per rush2::controls::Action.
        uint16_t buttons = 0;   // N64 buttons: the menu layout's, and START (PAUSE) in the race layout.
        float x = 0.0f;         // The steering stick, -1 (left) to 1, and -1 (back) to 1 (forward).
        float y = 0.0f;
        bool any() const { return actions != 0 || buttons != 0 || x != 0.0f || y != 0.0f; }
    };
    Held held();

    // RUSH2_TEST_TOUCH is set (src/touch_controls.cpp): a simulation run reads the touch controls' port.
    bool test_active();
}

#endif
