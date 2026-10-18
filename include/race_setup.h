#ifndef __RACE_SETUP_H__
#define __RACE_SETUP_H__

#include <cstdint>

#include "recomp.h"

// Setting a race up without the menus (src/race_setup.cpp): what the Select Player, track select and car select screens
// leave for the race start (game state 7), for races that skip them. Used by online races (src/online/online_game.cpp)
// and the RUSH2_TEST_RACE test aid (src/test_race.cpp).
namespace rush2::race_setup {
    // Whether the game is in the menus (title screen, front end, track or car select, not the attract demo), where a
    // race can be set up.
    bool in_menus(uint8_t* rdram);

    // Player `player`'s record (0x800C2140 + player * 0x28) as a race starts it: blank but for the button masks (the
    // game's default layout, which the binding tables are locked to, src/controls.cpp), its car slot (the player's
    // number), controller `port` (5 none), TRANSMISSION (`automatic`, 1 automatic) and the stick's starting range. The
    // profile pointer (+0x24) is left 0 for the caller.
    void init_record(uint8_t* rdram, int player, uint8_t port, uint8_t automatic);

    // Fills the car type tables' player rows (0x80200000, row p + 1 for player p, include/car_types.h) for players
    // 0 to players - 1 from the profile in each one's record (+0x24), as the car select does for its own players
    // (carselect_build_list_803B81F0 at 0x803B8B10-0x803B8F7C): the car's settings, colors, rims and wheel scales.
    void fill_player_rows(uint8_t* rdram, recomp_context* ctx, int players);

    // From the front end, a track select or a car select (not the car select's own start, which leaves by itself):
    // leaves the front end for the race start as its loop does for a race (func_800AE288 at 0x800AE428-0x800AE458):
    // the screen up is told it's left and none is up, then game state 7. The title screen keeps its data in the heap
    // the race reuses, and would otherwise be left after the race with that data gone.
    void leave_front_end(uint8_t* rdram, recomp_context* ctx);
}

#endif
