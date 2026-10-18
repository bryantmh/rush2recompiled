#ifndef __GAME_FILTER_H__
#define __GAME_FILTER_H__

#include <cstdint>

#include "recomp.h"

namespace recomp::config {
    class Config;
}

// The track and car selects' game filter (src/game_filter.cpp): with more than one game's tracks (or cars) on a select,
// Y (Triangle; I on the keyboard; the menus' C-up) opens a panel that picks which games' tracks (cars) it offers.
// The track select's side lives in src/rush2049/track2049_menu.cpp (track_selectable), the car select's in
// filter_car_lists below. The choices are kept in the saves ("game_filter", include/data_files.h).
namespace rush2::game_filter {
    enum class Game : int { Rush2, SFRush, Rush2049, CalSpeed, Custom, Count };
    enum class Screen : int { Tracks, Cars };

    constexpr unsigned bit(Game g) {
        return 1u << int(g);
    }

    // The General tab's Game Filter option (on by default; hot-swappable). apply sets it from the loaded config
    // (a fresh install loads without calling option change callbacks).
    void add_option(recomp::config::Config& config);
    const char* option_id();
    void apply(bool on);
    bool enabled();

    // Which game a track select id (0-79) or a car type (0-56) is from.
    Game track_game(int t);
    Game car_game(int type);

    // Whether the filter hides a game's tracks or cars.
    bool hidden(Screen s, Game g);

    // A select's frame (game thread), before it sets itself up for the frame. `present`: the games with something on
    // the select, 0 where the filter can't apply (the prompt shows with two games or more, while the option is on;
    // the select filters its list by the option as it was when it set itself up, and a change rebuilds it). Opens and runs
    // the panel; returns true when the filter changed and the select must be built again (tear it down and skip the
    // rest of the frame: it sets itself up again on the next one).
    bool frame(uint8_t* rdram, recomp_context* ctx, Screen s, unsigned present);
    // Draws the prompt, and the panel while it is open, from the select's text callback.
    void draw(uint8_t* rdram, recomp_context* ctx, Screen s, unsigned present);
    // The panel is open: the game sees no input from any port (src/input.cpp).
    bool input_captured();
    // The select set itself up (each visit, and after a rebuild).
    void screen_built(Screen s);
    // Once a frame (top of the game's main loop): closes the panel when its select is no longer up.
    void game_frame(uint8_t* rdram);

    // The car select's setup, after every player's list is built (rush2_cheats_car_list): filters the lists of the
    // first `players` players and notes which games they had.
    void filter_car_lists(uint8_t* rdram, int players);
}

#endif
