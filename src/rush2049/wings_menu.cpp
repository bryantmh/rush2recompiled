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
// player's choice is kept in the saves' "wings" section (rush2::wings::save_style). The screen has two players' panels
// (car select slots 0 and 1); with 3 or 4 players its second round is players 3 and 4 (src/players4.cpp).
//
// A 2049 car's colors are set as Rush 2049 sets them, so each panel has its own rows (the game keeps one list for
// both): a Dreamcast disc's car has paint jobs, each its own livery, picked by one STYLE row (MAIN COLOR, its value
// taken modulo the car's jobs: rush2::car2049::paint_jobs) in place of the three color rows and STRIPE; an N64 ROM's
// car has three paint ramps, so it keeps its three colors (STRIPE COLOR named TERTIARY COLOR) without STRIPE. The
// whole list is kept when func_803B81F0 builds it (once per visit) and each panel's list is put in its place before
// anything reads it: func_803B9478's per-slot loop (0x803B951C, slot $s7), func_803BC048's (0x803BC0C8, slot $s2)
// and the row widgets' callbacks (their slot from the widget's +0x2C).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>

#include "recomp.h"

#include "rush2_hooks.h"
#include "car2049.h"
#include "data_files.h"
#include "players4.h"
#include "wings_internal.h"

namespace {
    constexpr uint32_t option_ids = 0x803CB3B8;     // s32 [16]: the rows' option ids
    constexpr uint32_t option_count = 0x803CB3B0;   // s32
    constexpr uint32_t label_table = 0x800C4924;    // char* [language][15]
    constexpr uint32_t slot_cursor = 0x803C6990;    // s32 [2]: each panel's row
    constexpr uint32_t slot_top = 0x803CB3A0;       // s32 [2]: each panel's first shown row (4 are shown)
    constexpr uint32_t slot_type = 0x803CB362;      // s8 [2]: each panel's car type
    constexpr uint32_t first_slot = 0x803CB3F8;     // s32: the widget id of slot 0's panel
    constexpr uint32_t main_colour = 0x80201190 + rush2::car2049::types; // s8 [slot][36], src/rush2049/car2049.cpp
    constexpr int option_main = 2;
    constexpr int option_accent = 3;
    constexpr int option_stripe = 4;
    constexpr int option_stripe_colour = 5;
    constexpr int option_durability = 12;
    constexpr int option_wings = 15;
    constexpr int option_style = 16; // STYLE (MAIN COLOR) as the swatch widget sees it, so it draws no swatch
    constexpr int max_options = 16;
    constexpr int styles = 3;

    // Strings for the row, after the track select's (src/rush2049/track2049_menu.cpp, 0x80300000-0x80300D7F).
    constexpr uint32_t wings_label = 0x80300D80;      // "WINGS"
    constexpr uint32_t wings_label_ptr = 0x80300D90;  // char* to it, read in place of the label table's entry
    constexpr uint32_t style_names = 0x80300DA0;      // "STYLE 1" .. "STYLE 3", 16 bytes each
    // The 2049 cars' rows.
    constexpr uint32_t style_label = 0x80300E00;      // "STYLE"
    constexpr uint32_t tertiary_label = 0x80300E10;   // "TERTIARY COLOR"
    constexpr uint32_t style_label_ptr = 0x80300E20;  // char* to each
    constexpr uint32_t tertiary_label_ptr = 0x80300E24;
    constexpr uint32_t job_names = 0x80300E30;        // "STYLE 1" .. "STYLE 16", 16 bytes each
    constexpr int max_jobs = 16;

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
        write_string(rdram, style_label, "STYLE");
        write_string(rdram, tertiary_label, "TERTIARY COLOR");
        MEM_W(0, (int32_t)style_label_ptr) = (int32_t)style_label;
        MEM_W(0, (int32_t)tertiary_label_ptr) = (int32_t)tertiary_label;
        for (int s = 0; s < max_jobs; s++) {
            write_string(rdram, job_names + s * 16, "STYLE " + std::to_string(s + 1));
        }
    }

    // The whole option list as func_803B81F0 built it, and the slot whose list is in place.
    int base_ids[max_options];
    int base_count = 0;
    int current_slot = 0;

    enum class Rows { Rush2, Ramps, Jobs };

    int slot_car(uint8_t* rdram, int slot) {
        return (int8_t)MEM_B(0, (int32_t)(slot_type + slot));
    }

    // Which rows a slot's car has: Rush 2's, a 2049 car's three ramps, or a Dreamcast car's paint jobs.
    Rows slot_rows(uint8_t* rdram, int slot) {
        int type = slot_car(rdram, slot);
        if (type < rush2::car2049::first_type || type >= rush2::car2049::types) return Rows::Rush2;
        return rush2::car2049::paint_jobs(type) > 0 ? Rows::Jobs : Rows::Ramps;
    }

    // Puts slot's list in place. swatch: for the color swatch widget, which sees STYLE as option_style.
    void use_slot(uint8_t* rdram, int slot, bool swatch = false) {
        if (base_count == 0 || slot < 0 || slot > 1) {
            return;
        }
        current_slot = slot;
        Rows rows = slot_rows(rdram, slot);
        int count = 0;
        for (int i = 0; i < base_count; i++) {
            int id = base_ids[i];
            if (rows == Rows::Jobs && (id == option_accent || id == option_stripe || id == option_stripe_colour)) continue;
            if (rows == Rows::Ramps && id == option_stripe) continue;
            if (rows == Rows::Jobs && swatch && id == option_main) id = option_style;
            MEM_W(0, (int32_t)(option_ids + count * 4)) = id;
            count++;
        }
        MEM_W(0, (int32_t)option_count) = count;
        int cursor = std::clamp((int32_t)MEM_W(0, (int32_t)(slot_cursor + slot * 4)), 0, count - 1);
        int top = std::clamp((int32_t)MEM_W(0, (int32_t)(slot_top + slot * 4)), std::max(0, cursor - 3), cursor);
        MEM_W(0, (int32_t)(slot_cursor + slot * 4)) = cursor;
        MEM_W(0, (int32_t)(slot_top + slot * 4)) = top;
    }

    // The label pointer that stands in for row id's label on the current slot (0: its own).
    uint32_t row_label(uint8_t* rdram, int id) {
        if (id == option_wings) return wings_label_ptr;
        Rows rows = slot_rows(rdram, current_slot);
        if (rows == Rows::Jobs && (id == option_main || id == option_style)) return style_label_ptr;
        if (rows == Rows::Ramps && id == option_stripe_colour) return tertiary_label_ptr;
        return 0;
    }

    // The player a car select slot (0 or 1) is choosing for.
    int slot_player(int slot) {
        return slot + (rush2::players4::car_select_second_round() ? 2 : 0);
    }

    // The offset that, added to 0x800C0000 and read at +0x4924, reads the label pointer at ptr.
    uint64_t label_offset(uint32_t ptr) {
        return (uint64_t)(int64_t)(int32_t)(ptr - label_table);
    }
}

// func_803B81F0 at 0x803B8488 (after rush2_wings_car_rows): the whole list, kept for the panels' own lists.
extern "C" void rush2_car_rows_built(uint8_t* rdram, recomp_context* ctx) {
    write_strings(rdram);
    base_count = std::clamp((int32_t)MEM_W(0, (int32_t)option_count), 0, max_options);
    for (int i = 0; i < base_count; i++) base_ids[i] = (int32_t)MEM_W(0, (int32_t)(option_ids + i * 4));
}

// func_803B9478's per-slot loop at 0x803B951C (slot $s7) and func_803BC048's at 0x803BC0C8 (slot $s2).
extern "C" void rush2_car_rows_frame(uint8_t* rdram, recomp_context* ctx) {
    use_slot(rdram, (int32_t)ctx->r23);
}

extern "C" void rush2_car_rows_text(uint8_t* rdram, recomp_context* ctx) {
    use_slot(rdram, (int32_t)ctx->r18);
}

// A row widget's callback at its start ($a0 = the widget): its panel's id is at bit `shift` of +0x2C. swatch: the
// color swatch's (func_803BAA44).
extern "C" void rush2_car_rows_widget(uint8_t* rdram, recomp_context* ctx, int shift, int swatch) {
    uint32_t bits = (uint32_t)MEM_W(0x2C, (int32_t)ctx->r4);
    use_slot(rdram, (int32_t)((bits >> shift) & 0xF) == (int32_t)MEM_W(0, (int32_t)first_slot) ? 0 : 1, swatch != 0);
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
    int slot = (int32_t)ctx->r23;
    if ((int32_t)ctx->r15 == option_main && step != 0 && slot_rows(rdram, slot) == Rows::Jobs) {
        // STYLE: the game adds the step to the MAIN COLOR byte and wraps it at 32; set it so the sum wraps at the jobs.
        int type = slot_car(rdram, slot);
        int jobs = rush2::car2049::paint_jobs(type);
        uint32_t at = main_colour + slot * rush2::car2049::types + type;
        int job = ((int8_t)MEM_B(0, (int32_t)at) % jobs + jobs) % jobs;
        MEM_B(0, (int32_t)at) = (int8_t)((job + step + jobs) % jobs - step);
        return;
    }
    if ((int32_t)ctx->r15 != option_wings || step == 0) {
        return;
    }
    int player = slot_player(slot);
    int style = (rush2::wings::saved_style(player) + step + styles) % styles;
    rush2::wings::save_style(player, style);
}

// func_803BC048, a row's label: at 0x803BC310 ($t7 + 0x800C0000 is about to be read at +0x4924 for its width) and
// 0x803BC350 ($t4, for its text); $s0 = the row's entry in the option list. WINGS, STYLE and TERTIARY COLOR get theirs.
extern "C" void rush2_wings_car_label_t7(uint8_t* rdram, recomp_context* ctx) {
    if (uint32_t ptr = row_label(rdram, (int32_t)MEM_W(0, (int32_t)ctx->r16))) ctx->r15 = label_offset(ptr);
}

extern "C" void rush2_wings_car_label_t4(uint8_t* rdram, recomp_context* ctx) {
    if (uint32_t ptr = row_label(rdram, (int32_t)MEM_W(0, (int32_t)ctx->r16))) ctx->r12 = label_offset(ptr);
}

// func_803BAFD8 (the option arrows, placed by the cursor row's label width) at 0x803BB2E4 ($t9, the row's id in
// $t6) and 0x803BB34C ($t2, the id in $t9), the same reads.
extern "C" void rush2_wings_car_arrow_t9(uint8_t* rdram, recomp_context* ctx) {
    if (uint32_t ptr = row_label(rdram, (int32_t)ctx->r14)) ctx->r25 = label_offset(ptr);
}

extern "C" void rush2_wings_car_arrow_t2(uint8_t* rdram, recomp_context* ctx) {
    if (uint32_t ptr = row_label(rdram, (int32_t)ctx->r25)) ctx->r10 = label_offset(ptr);
}

// func_803BC048 at 0x803BC63C: $s0 = the value text of row $s1 for slot $s2 (0: none). WINGS shows the player's style,
// a Dreamcast car's STYLE its paint job.
extern "C" void rush2_wings_car_value(uint8_t* rdram, recomp_context* ctx) {
    int row = (int32_t)ctx->r17;
    if (row < 0 || row >= max_options) {
        return;
    }
    int id = (int32_t)MEM_W(0, (int32_t)(option_ids + row * 4));
    int slot = (int32_t)ctx->r18;
    if (id == option_main && slot_rows(rdram, slot) == Rows::Jobs) {
        write_strings(rdram);
        int type = slot_car(rdram, slot);
        int jobs = std::min(rush2::car2049::paint_jobs(type), max_jobs);
        int job = ((int8_t)MEM_B(0, (int32_t)(main_colour + slot * rush2::car2049::types + type)) % jobs + jobs) % jobs;
        ctx->r16 = (uint64_t)(int64_t)(int32_t)(job_names + job * 16);
        return;
    }
    if (id != option_wings) {
        return;
    }
    write_strings(rdram);
    int style = std::clamp(rush2::wings::saved_style(slot_player((int32_t)ctx->r18)), 0, styles - 1);
    ctx->r16 = (uint64_t)(int64_t)(int32_t)(style_names + style * 16);
}

// The wing styles in the saves: {"styles": [p1, p2, p3, p4]}, each 0-2.
namespace {
    const std::string styles_section = "wings";
    std::array<std::atomic<int>, rush2::wings::max_players> saved_styles = {};
    std::mutex styles_mutex;
}

int rush2::wings::saved_style(int player) {
    return player >= 0 && player < max_players ? saved_styles[player].load() : 0;
}

void rush2::wings::save_style(int player, int style) {
    if (player < 0 || player >= max_players) {
        return;
    }
    std::lock_guard lock{ styles_mutex };
    saved_styles[player] = std::clamp(style, 0, styles - 1);
    set_player_style(player, saved_styles[player]);
    std::string json = "{\"styles\": [";
    for (int i = 0; i < max_players; i++) {
        json += (i ? ", " : "") + std::to_string(saved_styles[i].load());
    }
    rush2::data_files::write(rush2::data_files::File::Saves, styles_section, json + "]}");
}

void rush2::wings::load_styles() {
    std::lock_guard lock{ styles_mutex };
    std::string text = rush2::data_files::read(rush2::data_files::File::Saves, styles_section);
    size_t at = text.find('[');
    const char* p = at == std::string::npos ? nullptr : text.c_str() + at + 1;
    for (int i = 0; i < max_players; i++) {
        int style = 0;
        if (p) {
            char* end = nullptr;
            long value = std::strtol(p, &end, 10);
            if (end != p) {
                style = std::clamp((int)value, 0, styles - 1);
                p = end;
                while (*p == ',' || *p == ' ') p++;
            }
            else {
                p = nullptr;
            }
        }
        saved_styles[i] = style;
        set_player_style(i, style);
    }
}
