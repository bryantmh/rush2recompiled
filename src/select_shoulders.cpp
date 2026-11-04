// The shoulder buttons in the track and car selects (issue #48): L and R (the menus' LB / RB, Q / E on the keyboard,
// src/controls.cpp get_menu_input) step the TRACK or CAR row from whatever row the cursor is on, as left and right do
// on that row. The game's own left / right step runs for it: the press is turned into a D-pad left or right and the
// row's id into TRACK's or CAR's for that one step.
//
// Car select (menu_screen_car_select_frame_803B9478, per panel $s7, player record $s0 = 0x800C2140 + slot * 0x28):
// at 0x803B9738 $v0 holds the record's repeating presses (+6); D-up / D-down move the cursor, D-left / D-right / Z
// (0x2300) step the cursor row's option, whose id is read at 0x803B98A0 (0x803CB3B8[cursor 0x803C6990[slot]]) and picks
// the case at 0x803B98A4. CAR is id 0.
//
// Track select (trackselect_frame_803ABE0C): at 0x803ABEF8 $v0 holds the presses (0x80125A68); D-left / D-right / Z
// step the option of the cursor row (0x803D05D8[cursor 0x803D05BC]), whose id the steps read several times on their
// way; every step ends at 0x803AC410 (or 0x803AC554, an option that is greyed). TRACK is id 0. The cursor is put on
// TRACK's row for the step and back after it.

#include <cstdint>

#include "recomp.h"
#include "rush2_hooks.h"

namespace {
    constexpr uint16_t button_l = 0x0020, button_r = 0x0010;
    constexpr int option_car = 0;  // the car select's CAR row id
    constexpr uint16_t dpad_up = 0x0800, dpad_down = 0x0400, dpad_left = 0x0200, dpad_right = 0x0100, button_z = 0x2000;

    // Left (-1) or right (1) for a shoulder press with no other menu move in it, else 0.
    int shoulder_step(uint32_t buttons) {
        if (buttons & (dpad_up | dpad_down | dpad_left | dpad_right | button_z)) return 0;
        bool l = (buttons & button_l) != 0, r = (buttons & button_r) != 0;
        return l == r ? 0 : l ? -1 : 1;
    }

    // Car select: the panels whose step this frame is a shoulder press.
    bool car_pending[2] = {};

    // Track select: the cursor row to put back after a shoulder press's step (-1: none).
    constexpr uint32_t track_rows = 0x803D05D8;     // s32 [rows]: option id per row
    constexpr uint32_t track_cursor = 0x803D05BC;   // s32
    constexpr uint32_t track_count = 0x803D05CC;    // s32
    constexpr int option_track = 0;
    int track_saved_cursor = -1;
}

// menu_screen_car_select_frame_803B9478 at 0x803B9738: $v0 = panel $s7's presses.
extern "C" void rush2_shoulder_car_buttons(uint8_t* rdram, recomp_context* ctx) {
    int slot = (int32_t)ctx->r23;
    if (slot < 0 || slot > 1) return;
    int step = shoulder_step((uint32_t)ctx->r2 & 0xFFFF);
    car_pending[slot] = step != 0;
    if (step != 0) ctx->r2 |= step < 0 ? dpad_left : dpad_right;
}

// menu_screen_car_select_frame_803B9478 at 0x803B98A4 ($t7 = the cursor row's id, about to pick the case; before
// rush2_wings_car_step, which reads it too): a shoulder press steps CAR.
extern "C" void rush2_shoulder_car_row(uint8_t* rdram, recomp_context* ctx) {
    int slot = (int32_t)ctx->r23;
    if (slot < 0 || slot > 1 || !car_pending[slot]) return;
    car_pending[slot] = false;
    ctx->r15 = option_car;
}

// trackselect_frame_803ABE0C at 0x803ABEF8: $v0 = the presses.
extern "C" void rush2_shoulder_track_buttons(uint8_t* rdram, recomp_context* ctx) {
    rush2_shoulder_track_restore(rdram, ctx);
    int step = shoulder_step((uint32_t)ctx->r2 & 0xFFFF);
    if (step == 0) return;
    int count = (int32_t)MEM_W(0, (int32_t)track_count);
    for (int row = 0; row < count && row < 16; row++) {
        if ((int32_t)MEM_W(0, (int32_t)(track_rows + row * 4)) == option_track) {
            track_saved_cursor = (int32_t)MEM_W(0, (int32_t)track_cursor);
            MEM_W(0, (int32_t)track_cursor) = row;
            ctx->r2 |= step < 0 ? dpad_left : dpad_right;
            return;
        }
    }
}

// trackselect_frame_803ABE0C at 0x803AC410 and 0x803AC554, where the steps end: the cursor goes back.
extern "C" void rush2_shoulder_track_restore(uint8_t* rdram, recomp_context* ctx) {
    if (track_saved_cursor < 0) return;
    int count = (int32_t)MEM_W(0, (int32_t)track_count);
    int row = track_saved_cursor < count ? track_saved_cursor : count - 1;
    MEM_W(0, (int32_t)track_cursor) = row < 0 ? 0 : row;
    track_saved_cursor = -1;
}
