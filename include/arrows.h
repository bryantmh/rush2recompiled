#ifndef __ARROWS_H__
#define __ARROWS_H__

#include <cstdint>

#include "recomp.h"

// The views of a race as they are drawn (src/arrows.cpp): shared by the arrows over the other players' cars and the
// battle HUD (src/rush2049/battle.cpp).
namespace rush2::views {
    // The part of the 320 x 240 screen a player's view covers.
    struct View {
        int x0, y0, x1, y1;
        bool right, bottom, wide;   // in the right column or bottom row of a split; one of two stacked views
        // Where the view is drawn, in the same 4:3 screen pixels: in widescreen its outer side reaches the window's edge.
        float left = 0.0f, right_edge = 320.0f;
    };
    View view_of(uint8_t* rdram, int player, int players);
    // The number of local players (1-4) and the car of player p (-1: none).
    int local_players(uint8_t* rdram);
    int player_car(uint8_t* rdram, int player);
    // The view whose camera position `at` points to or equals (func_8007C27C's argument), or -1.
    int view_of_camera(uint8_t* rdram, uint32_t at);
    // The axes view p is drawn with (rows right, up, forward): its camera's, turned about the forward axis by the
    // game's tilted view (func_8007C624: flag 0x800D0171, angle 0x800CF9DC, func_8005ADFC) when that is on.
    void axes(uint8_t* rdram, int p, float out[9]);
    void eye(uint8_t* rdram, int p, float out[3]);
    // The tangents of half view p's field of view, across and up, as it is drawn.
    void tangents(uint8_t* rdram, int p, const View& v, float& tan_x, float& tan_y);
}

// The arrows over the other players' cars (src/arrows.cpp). Rush 2 has one per car for its two views; with three or
// four players, and in a battle (Rush 2049's arrows, which also point to the cars out of view), every view gets its
// own arrow for every other car.
namespace rush2::arrows {
    // Whether a world polygon is one of these arrows: each belongs to one view (src/players4.cpp draws the polygons
    // of views 0 and 1 in views 2 and 3 too).
    bool single_view(uint32_t polygon);
}

#endif
