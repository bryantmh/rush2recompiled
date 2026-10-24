#ifndef __SETUP_H__
#define __SETUP_H__

#include <string>

// Setup (src/setup.cpp, docs/settings_menu.md#setup): the screen a new install starts on, before the game. It picks the
// game rules preset (Original: Rush 2's own rules; Enhanced: the recomp's defaults) and offers the other games' ROMs
// (the Games tab's page). The General tab's Run Setup Again opens it again.
namespace rush2::setup {
    enum class Profile { Original, Enhanced };

    // After every config is loaded (rush2::init_config, before the online tab compares them). fresh_install is true
    // when no general.json existed before the configs loaded. Reads the stored profile and gives the rule settings
    // added since the last launch the profile's value; with no stored profile, an existing install keeps its settings
    // (Enhanced, or Original if they match it) and a fresh one is marked for the setup screen.
    void load(bool fresh_install);

    // The setup screen is waiting to be shown before the game starts (a fresh install that hasn't finished it).
    bool pending();

    // Shows the setup screen, hiding every other screen. From a UI task (recompui::queue_ui_task: no context open).
    // first_launch: the launcher's (no Keep Current choice, and the game starts once it closes); otherwise from the
    // menu, which opens again when it closes.
    void open(bool first_launch);

    // "Original", "Enhanced" or "Custom" (the rule settings match neither preset).
    std::string rules_name();
}

#endif
