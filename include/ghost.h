#ifndef __GHOST_H__
#define __GHOST_H__

#include <cstdint>

#include "recomp.h"

// Ghost races (src/ghost.cpp, src/rush2049/ghost_logic.cpp): Rush 2049's ghosts in Rush 2. Player 1's car is recorded in every
// race (Settings > General > Save Ghosts; else only in GHOST RACE, the Start Game menu's row); each profile's fastest
// finished runs on each track, direction and lap count (Settings > General > Ghosts Kept) are kept, a file each, and in
// GHOST RACE up to three kept ghosts (chosen on the car select) race as translucent cars.
namespace rush2::ghost {
    // The Start Game menu (src/rush2049/track2049_menu.cpp): whether GHOST RACE was the row chosen.
    void set_chosen(bool chosen);
    bool chosen();
    // Whether race car `index` is a ghost (ghosts don't hit props, breakables or coins).
    bool is_ghost_car(int index);
    // The Save Ghosts option: record every race, not only GHOST RACE.
    void set_save_all(bool on);
    // The Ghosts Kept option: how many of a profile's fastest runs per track, direction and lap count are kept.
    void set_ghosts_kept(int n);
    // A battle's invisible car (src/rush2049/battle.cpp), as Rush 2049 fades one: `view` is the one view that still draws it,
    // translucent as a ghost; the others don't draw it. -1 puts the car back to normal.
    void set_faded(int car, int view);
    // Called before func_8007AA48 emits the G_DL to a model's display list (ctx: $fp = the list, $s7 = the node).
    void draw_model(uint8_t* rdram, recomp_context* ctx);
}

#endif
