#ifndef __PAUSE_MENU_H__
#define __PAUSE_MENU_H__

#include <cstdint>

#include "recomp.h"

// The race pause menu's additions (src/pause_menu.cpp, the General tab's Pause Menu Extras option): a SKIP TRACK row
// between AUDIO and RESTART, a box in the bottom right corner with the song playing, and in a one player race the
// keys, Dew cans or coins found on the track in the top right corner.
namespace rush2::pause_menu {
    void set_enabled(bool enabled);
    // After the race HUD's widget loop (src/hud.cpp): draws the row's label and the boxes while the menu is open.
    void draw(uint8_t* rdram, recomp_context* ctx);
}

#endif
