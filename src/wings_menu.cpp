// Rush 2049 wings: the car select's WINGS row, where each player picks their wings as on Rush 2049's car setup screen.
//
// Rush 2's car select (menu overlay) lists its options by id (func_803B81F0 builds the list: 0x803CB3B8[row], count
// 0x803CB3B0; labels 0x800C4924 + language * 60 + id * 4): 0 CAR, 1 TRANSMISSION, 2 MAIN COLOR, 3 ACCENT COLOR,
// 4 STRIPE, 5 STRIPE COLOR, 6 TIRE RIMS, 7 HORN, 8 ENGINE, 9 TORQUE, 10 SUSPENSION, 11 TIRES, 12 DURABILITY, and with
// the cheat byte 0x8010C3D4 set 13 TIRE SIZE F and 14 TIRE SIZE R. While wings are on (a Rush 2049 ROM and the Wings
// option), the list gains WINGS (id 15) above DURABILITY. The list's 0x3C bytes then take the word after them,
// 0x803CB3F4, which nothing reads or writes.
//
// The screen's code reads ids through jump tables of 15 cases that skip larger ids (func_803B9478's left/right
// handling, func_803BC048's values, func_803BB9F8's row art) and compares them elsewhere, so id 15 does nothing there;
// hooks give it its label (the label table has 15 entries per language), its value and its left/right steps. Each
// player's choice is kept in the Games config (wing_style_p1-p4, src/wings.cpp). The screen has two players' panels
// (car select slots 0 and 1); with 3 or 4 players its second round is players 3 and 4 (src/players4.cpp).

#include <algorithm>
#include <string>

#include "recomp.h"

#include "rush2_hooks.h"
#include "players4.h"
#include "wings_internal.h"

namespace {
    constexpr uint32_t option_ids = 0x803CB3B8;     // s32 [16]: the rows' option ids
    constexpr uint32_t option_count = 0x803CB3B0;   // s32
    constexpr uint32_t label_table = 0x800C4924;    // char* [language][15]
    constexpr int option_durability = 12;
    constexpr int option_wings = 15;
    constexpr int max_options = 16;
    constexpr int styles = 3;

    // Strings for the row, after the track select's (src/track2049_menu.cpp, 0x80300000-0x80300D7F).
    constexpr uint32_t wings_label = 0x80300D80;      // "WINGS"
    constexpr uint32_t wings_label_ptr = 0x80300D90;  // char* to it, read in place of the label table's entry
    constexpr uint32_t style_names = 0x80300DA0;      // "STYLE 1" .. "STYLE 3", 16 bytes each

    void write_string(uint8_t* rdram, uint32_t addr, const std::string& s) {
        for (size_t i = 0; i <= s.size(); i++) {
            MEM_B(0, (int32_t)(addr + i)) = i < s.size() ? s[i] : 0;
        }
    }

    void write_strings(uint8_t* rdram) {
        write_string(rdram, wings_label, "WINGS");
        MEM_W(0, (int32_t)wings_label_ptr) = (int32_t)wings_label;
        for (int s = 0; s < styles; s++) {
            write_string(rdram, style_names + s * 16, "STYLE " + std::to_string(s + 1));
        }
    }

    // The player a car select slot (0 or 1) is choosing for.
    int slot_player(int slot) {
        return slot + (rush2::players4::car_select_second_round() ? 2 : 0);
    }

    // The offset that, added to 0x800C0000 and read at +0x4924, reads the WINGS label pointer.
    uint64_t label_offset() {
        return (uint64_t)(int64_t)(int32_t)(wings_label_ptr - label_table);
    }
}

// func_803B81F0 at 0x803B8488, after the option list is built: WINGS goes above DURABILITY.
extern "C" void rush2_wings_car_rows(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2::wings::enabled()) {
        return;
    }
    write_strings(rdram);
    int count = (int32_t)MEM_W(0, (int32_t)option_count);
    if (count >= max_options) {
        return;
    }
    int at = count;
    for (int row = 0; row < count; row++) {
        if ((int32_t)MEM_W(0, (int32_t)(option_ids + row * 4)) == option_durability) {
            at = row;
            break;
        }
    }
    for (int row = count; row > at; row--) {
        MEM_W(0, (int32_t)(option_ids + row * 4)) = MEM_W(0, (int32_t)(option_ids + (row - 1) * 4));
    }
    MEM_W(0, (int32_t)(option_ids + at * 4)) = option_wings;
    MEM_W(0, (int32_t)option_count) = count + 1;
}

// func_803B9478 at 0x803B98A4: left or right ($a1 = -1 or 1) on slot $s7's row, whose id ($t7) is about to pick the
// case (ids past 14 change nothing). WINGS steps the player's style.
extern "C" void rush2_wings_car_step(uint8_t* rdram, recomp_context* ctx) {
    int step = (int32_t)ctx->r5;
    if ((int32_t)ctx->r15 != option_wings || step == 0) {
        return;
    }
    int player = slot_player((int32_t)ctx->r23);
    int style = (rush2::wings::get_style_option(player) + step + styles) % styles;
    rush2::wings::set_style_option(player, style);
}

// func_803BC048, a row's label: at 0x803BC310 ($t7 + 0x800C0000 is about to be read at +0x4924 for its width) and
// 0x803BC350 ($t4, for its text); $s0 = the row's entry in the option list.
extern "C" void rush2_wings_car_label_t7(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)MEM_W(0, (int32_t)ctx->r16) == option_wings) ctx->r15 = label_offset();
}

extern "C" void rush2_wings_car_label_t4(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)MEM_W(0, (int32_t)ctx->r16) == option_wings) ctx->r12 = label_offset();
}

// func_803BAFD8 (the option arrows, placed by the cursor row's label width) at 0x803BB2E4 ($t9, the row's id in
// $t6) and 0x803BB34C ($t2, the id in $t9), the same reads.
extern "C" void rush2_wings_car_arrow_t9(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r14 == option_wings) ctx->r25 = label_offset();
}

extern "C" void rush2_wings_car_arrow_t2(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r25 == option_wings) ctx->r10 = label_offset();
}

// func_803BC048 at 0x803BC63C: $s0 = the value text of row $s1 for slot $s2 (0: none). WINGS shows the player's style.
extern "C" void rush2_wings_car_value(uint8_t* rdram, recomp_context* ctx) {
    int row = (int32_t)ctx->r17;
    if (row < 0 || row >= max_options || (int32_t)MEM_W(0, (int32_t)(option_ids + row * 4)) != option_wings) {
        return;
    }
    write_strings(rdram);
    int style = std::clamp(rush2::wings::get_style_option(slot_player((int32_t)ctx->r18)), 0, styles - 1);
    ctx->r16 = (uint64_t)(int64_t)(int32_t)(style_names + style * 16);
}
