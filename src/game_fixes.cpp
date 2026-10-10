// Fixes for bugs in the original Rush 2 (US). Each is a small hook into the game's own code; none has a toggle.
//
// Mountain Dew dragster paint (GitHub issue #5). func_803B9478 (car select frame) works out every frame, at
// 0x803BA644-0x803BA7E4, whether slot $s7's cursor row can be changed for the slot's car type, into the byte
// 0x803CB3AC[slot] (left and right do nothing on the row while it is 0, 0x803B983C). By option id
// (0x803CB3B8[row]): the DEW (type 21) blocks 2 MAIN COLOR, 3 ACCENT COLOR, 4 STRIPE and 5 STRIPE COLOR; the TAXI
// (16) blocks 3; FORMULA (18) blocks 4 and 5; ROCKET (20) blocks 1, 4, 5, 8 and $s3's id. Yet the car paint builder
// (func_8008582C) still tints the DEW: its palette class table 0x800C5698 (the other cars use 0x800C5670) gives
// entries 1-31 MAIN (flags 0x22) and 33-63 ACCENT (0x42) like every car, and fixes 64-255, which is why computer
// opponents in a Dew show other colors. It only skips the stripe for types 18, 20 and 21 (0x80085CA8). So MAIN and
// ACCENT are unblocked for the DEW; STRIPE and STRIPE COLOR stay blocked, as the builder would ignore them.
//
// Engines silent after the clock runs out (GitHub issue #4). In a race (game state 3, func_800AE670 at 0x800AEC14),
// once the time left is down to 0.5 s the game sets the out of time flag 0x800FAE98 and the engines off byte
// 0x800E7BCE (0x800AEF94), which makes func_800650DC give the engine sounds rpm 0. A checkpoint reached while
// coasting adds time, and the next frame clears 0x800FAE98 (0x800AEDC4), but nothing clears 0x800E7BCE until the next
// race setup (func_800A5ADC, 0x800A6134), so the engines stay silent for the rest of the race. Rush 2049 fixed it;
// here it is cleared along with the flag.

#include "recomp.h"

#include "rush2_hooks.h"

namespace {
    constexpr uint32_t option_ids = 0x803CB3B8;     // s32 [16]: car select rows' option ids
    constexpr uint32_t slot_cursor = 0x803C6990;    // s32 [2]: each panel's cursor row
    constexpr uint32_t slot_type = 0x803CB362;      // s8 [2]: each panel's car type
    constexpr uint32_t row_changeable = 0x803CB3AC; // u8 [2]: the cursor row can be changed (left / right)
    constexpr int type_dew = 21;
    constexpr int option_main = 2;
    constexpr int option_accent = 3;

    constexpr uint32_t engines_off = 0x800E7BCE;    // u8: engine sounds get rpm 0 (out of time)
}

// func_803B9478 at 0x803BA7E8, after slot $s7's row-changeable byte is set: MAIN and ACCENT COLOR for the Dew.
extern "C" void rush2_fix_dew_paint_rows(uint8_t* rdram, recomp_context* ctx) {
    int slot = (int32_t)ctx->r23;
    if (slot < 0 || slot > 1 || (int8_t)MEM_B(0, (int32_t)(slot_type + slot)) != type_dew) {
        return;
    }
    int row = MEM_W(0, (int32_t)(slot_cursor + slot * 4));
    if (row < 0 || row >= 16) {
        return;
    }
    int option = MEM_W(0, (int32_t)(option_ids + row * 4));
    if (option == option_main || option == option_accent) {
        MEM_B(0, (int32_t)(row_changeable + slot)) = 1;
    }
}

// func_800AE670 at 0x800AEDC0, about to clear the out of time flag (time was added): the engines run again.
extern "C" void rush2_fix_engines_after_timeout(uint8_t* rdram, recomp_context* ctx) {
    MEM_B(0, (int32_t)engines_off) = 0;
}
