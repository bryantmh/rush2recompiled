// WINGS row in the game's Controller Setup screen.
//
// The screen (func_800AD7E4, reached from Options and the pause menu) lists 9 actions per player: GAS, BRAKE,
// STEERING, SHIFT UP, SHIFT DOWN, REVERSE, ABORT, VIEW, HORN. Each action's binding is a code stored per controller
// port in 9 bytes at 0x80125A70 + port * 9 (0-8 = A, B, C-up, C-down, C-left, C-right, L, R, Z; 10 = stick;
// 11 = D-pad). The cursor at 0x800D5438 + player * 2 picks a row, D-left/right cycle its code, and L+R restores the
// defaults. Two actions on the same button set both rows' conflict flags (0x800D5658 + player * 9), which shows a
// "!" icon and blocks leaving the screen.
//
// A tenth row, WINGS, is added without moving any of the game's data:
// - The screen's widget table (41 entries at 0x800C1130, built into objects by func_800604FC) is copied into the
//   recomp heap with four more entries for the WINGS binding and conflict icons of both players, and the rows are
//   moved 15 pixels apart instead of 17 so the extra row fits in the panel.
// - The label table the text callback (func_800B6134) reads is copied the same way with "WINGS" appended.
// - The wings binding of each port lives here and is saved to wings_controls.json in the config folder. The game's
//   per-port bindings are followed by the next port's, and its Controller Pak profiles are checksummed, so neither
//   has room for a tenth byte.
// - Row 9 is edited here (the game's code would write into the next port's bindings), WINGS conflicts are added to
//   the game's conflict pass, and the icon callback (func_800AF598) is pointed at the wings binding for row 0xC.
// - Instruction patches in us.toml widen the cursor, label and widget counts and include row 8 in the exit checks,
//   since a WINGS/HORN conflict only sets HORN's flag.
//
// The row is only there while wings are enabled (Rush 2049 tab), decided when the screen opens. Without it the
// screen keeps the game's own widget table, 9 rows 17 pixels apart, and the cursor wraps at row 8.

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "rush2_hooks.h"
#include "wings_internal.h"

extern "C" void func_80064908(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr int num_ports = 4;
    constexpr int8_t default_code = 4; // C-left: the only button the default bindings leave free.
    constexpr int max_code = 8;        // Wings can use any button (A..Z), not the stick or D-pad.
    constexpr int wings_row = 9;       // Cursor row.
    constexpr uint32_t wings_icon_row = 0xC; // Row nibble of the WINGS icons' widget parameter.

    constexpr uint32_t widget_table = 0x800C1130;
    constexpr int widget_size = 0x28;
    constexpr int widget_count = 41;
    constexpr int new_widget_count = widget_count + 4;
    constexpr uint32_t label_table = 0x800C4B08;
    constexpr uint32_t players = 0x800C2140;
    constexpr int player_size = 0x28;
    constexpr uint32_t button_masks = 0x800C1028;
    constexpr uint32_t pause_state = 0x8002305C;
    constexpr uint8_t no_port = 5;

    constexpr int original_row_spacing = 17;
    constexpr int row_spacing = 15;
    constexpr int original_rows = 9;
    constexpr int first_icon_y = 42;
    constexpr int defaults_icon_y = 195;

    std::array<std::atomic<int8_t>, num_ports> wings_code = { default_code, default_code, default_code, default_code };
    std::array<bool, 2> wings_conflict{};

    uint32_t new_widget_table = 0;
    uint32_t new_label_table = 0;

    // Whether the open Controls screen has the WINGS row (latched when its widgets are created).
    bool menu_has_wings = false;

    std::mutex save_mutex;

    std::filesystem::path save_path() {
        return recomp::get_config_path() / "wings_controls.json";
    }

    void save_bindings() {
        std::lock_guard lock{ save_mutex };
        std::ofstream out{ save_path() };
        if (!out) {
            return;
        }
        out << "{\n  \"wings_buttons\": [";
        for (int port = 0; port < num_ports; port++) {
            out << (port ? ", " : "") << int(wings_code[port].load());
        }
        out << "]\n}\n";
    }

    uint32_t heap_address(uint8_t* rdram, void* ptr) {
        return uint32_t(reinterpret_cast<uint8_t*>(ptr) - rdram) + 0x80000000u;
    }

    // Copies the widget table, respacing the rows and adding the WINGS icons before the text callback entry.
    void build_widget_table(uint8_t* rdram) {
        new_widget_table = heap_address(rdram, recomp::alloc(rdram, new_widget_count * widget_size));
        auto copy_entry = [&](int from, int to) {
            for (int i = 0; i < widget_size; i += 4) {
                MEM_W(0, (int32_t)(new_widget_table + to * widget_size + i)) = MEM_W(0, (int32_t)(widget_table + from * widget_size + i));
            }
        };
        auto set_y = [&](int entry, int y) {
            MEM_H(0, (int32_t)(new_widget_table + entry * widget_size + 0x6)) = int16_t(y);
        };

        // Entries 0-35: binding and conflict icons for rows 1-9 of both players; 36-39: L and R icons.
        for (int entry = 0; entry < 40; entry++) {
            copy_entry(entry, entry);
            uint32_t row = MEM_W(0, (int32_t)(widget_table + entry * widget_size + 0x24)) & 0xF;
            if (row >= 1 && row <= 9) {
                set_y(entry, first_icon_y + row_spacing * (row - 1));
            }
            else {
                set_y(entry, defaults_icon_y);
            }
        }

        // WINGS icons, copied from HORN's (row 9) icons: P1 binding, P1 conflict, P2 binding, P2 conflict.
        const int horn_entries[4] = { 8, 17, 26, 35 };
        for (int i = 0; i < 4; i++) {
            int entry = 40 + i;
            copy_entry(horn_entries[i], entry);
            set_y(entry, first_icon_y + row_spacing * wings_row);
            uint32_t param = MEM_W(0, (int32_t)(new_widget_table + entry * widget_size + 0x24));
            MEM_W(0, (int32_t)(new_widget_table + entry * widget_size + 0x24)) = (param & ~0xFu) | wings_icon_row;
        }

        // The text callback stays last.
        copy_entry(40, new_widget_count - 1);
    }

    // The 9 action labels plus "WINGS" (only the US language block is needed; the language index is always 0).
    void build_label_table(uint8_t* rdram) {
        uint32_t text = heap_address(rdram, recomp::alloc(rdram, 8));
        const char* wings = "WINGS";
        for (int i = 0; i < 6; i++) {
            MEM_B(0, (int32_t)(text + i)) = wings[i];
        }

        new_label_table = heap_address(rdram, recomp::alloc(rdram, 10 * 4));
        for (int i = 0; i < 9; i++) {
            MEM_W(0, (int32_t)(new_label_table + i * 4)) = MEM_W(0, (int32_t)(label_table + i * 4));
        }
        MEM_W(0, (int32_t)(new_label_table + 9 * 4)) = (int32_t)text;
    }

    uint8_t player_port(uint8_t* rdram, int player) {
        return MEM_BU(0, (int32_t)(players + player * player_size + 1));
    }

    void play_menu_sound(uint8_t* rdram, recomp_context* ctx, int sound) {
        recomp_context saved = *ctx;
        ctx->r4 = sound;
        func_80064908(rdram, ctx);
        *ctx = saved;
    }
}

void rush2::wings::load_controls() {
    std::ifstream in{ save_path() };
    if (!in) {
        return;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t pos = text.find('[');
    for (int port = 0; port < num_ports && pos != std::string::npos; port++) {
        pos = text.find_first_of("0123456789", pos);
        if (pos == std::string::npos) {
            break;
        }
        int code = std::atoi(text.c_str() + pos);
        if (code >= 0 && code <= max_code) {
            wings_code[port] = int8_t(code);
        }
        pos = text.find_first_not_of("0123456789", pos);
    }
}

bool rush2::wings::button_held(uint8_t* rdram, int player) {
    uint8_t port = player_port(rdram, player);
    if (port >= num_ports) {
        return false;
    }
    uint16_t held = MEM_HU(0, (int32_t)(players + player * player_size + 4));
    uint16_t mask = MEM_HU(0, (int32_t)(button_masks + wings_code[port].load() * 2));
    return MEM_B(0, (int32_t)pause_state) == 0 && (held & mask) != 0;
}

// func_800AD7E4, before the widget list is created ($a2 = table; the count in $a3 is set here, its instruction is
// patched out): the extended table with wings, the game's own without.
extern "C" void rush2_wings_menu_widgets(uint8_t* rdram, recomp_context* ctx) {
    menu_has_wings = rush2::wings::enabled();
    if (!menu_has_wings) {
        ctx->r7 = widget_count;
        return;
    }
    if (new_widget_table == 0) {
        build_widget_table(rdram);
    }
    ctx->r6 = (int32_t)new_widget_table;
    ctx->r7 = new_widget_count;
}

// func_800AD7E4, after D-up/D-down moved the cursor ($s6) with wrapping patched to rows 0-9. Pressed buttons in $t1.
extern "C" void rush2_wings_menu_cursor(uint8_t* rdram, recomp_context* ctx) {
    if (!menu_has_wings && MEM_H(0, ctx->r22) == wings_row) {
        // D-down from row 8 wraps to the top; D-up from row 0 wraps to row 8.
        MEM_H(0, ctx->r22) = (ctx->r9 & 0x0400) ? 0 : original_rows - 1;
    }
}

// func_800AD7E4, after L+R wrote the default bindings to the port in $s5 (player struct in $t2).
extern "C" void rush2_wings_menu_defaults(uint8_t* rdram, recomp_context* ctx) {
    uint8_t port = MEM_BU(1, ctx->r10);
    if (port < num_ports) {
        wings_code[port] = default_code;
        save_bindings();
    }
}

// func_800AD7E4, at the D-left/D-right handling (L_800ADCA0, which every path reaches): edits the WINGS row. Returns true to skip the game's edit code,
// whose bounds checks only know rows 0-8. Player struct in $t2, pressed buttons in $t1, cursor pointer in $s6.
extern "C" int rush2_wings_menu_edit(uint8_t* rdram, recomp_context* ctx) {
    if (!menu_has_wings || MEM_H(0, ctx->r22) != wings_row) {
        return 0;
    }
    uint8_t port = MEM_BU(1, ctx->r10);
    uint16_t pressed = uint16_t(ctx->r9);
    int step = (pressed & 0x0200) ? -1 : (pressed & 0x0100) ? 1 : 0;
    if (step != 0 && port < num_ports) {
        play_menu_sound(rdram, ctx, 0x49);
        int code = wings_code[port] + step;
        if (code < 0) {
            code = max_code;
        }
        else if (code > max_code) {
            code = 0;
        }
        wings_code[port] = int8_t(code);
        save_bindings();
    }
    return 1;
}

// func_800AD7E4, after the conflict pass: mark rows sharing the WINGS button. Player index in $t3, conflict flags
// in $s0, bindings in $s5.
extern "C" void rush2_wings_menu_conflicts(uint8_t* rdram, recomp_context* ctx) {
    int player = int(ctx->r11);
    if (!menu_has_wings || player < 0 || player > 1) {
        return;
    }
    uint8_t port = player_port(rdram, player);
    bool conflict = false;
    if (port < num_ports) {
        int8_t code = wings_code[port];
        for (int row = 0; row < 9; row++) {
            if (MEM_B(row, ctx->r21) == code) {
                MEM_B(row, ctx->r16) = 1;
                conflict = true;
            }
        }
    }
    wings_conflict[player] = conflict;
}

// func_800AF598 (icon callback), after it read the conflict flag of a conflict icon's row into $a1 (the next
// instruction turns it into the icon's hidden state). Object in $a0.
extern "C" void rush2_wings_menu_icon_conflict(uint8_t* rdram, recomp_context* ctx) {
    uint32_t param = MEM_W(0x2C, ctx->r4);
    if ((param & 0xF) == wings_icon_row) {
        int player = (param >> 12) & 0xF;
        ctx->r5 = (player < 2 && wings_conflict[player]) ? 1 : 0;
    }
}

// func_800AF598, after it read the binding code for the icon's row into $v0. Object in $a0, row in $v1.
extern "C" void rush2_wings_menu_icon_binding(uint8_t* rdram, recomp_context* ctx) {
    if ((ctx->r3 & 0xF) == wings_icon_row) {
        int player = (MEM_W(0x2C, ctx->r4) >> 12) & 0xF;
        uint8_t port = player_port(rdram, player);
        ctx->r2 = port < num_ports ? wings_code[port].load() : default_code;
    }
}

// func_800B6134 (text callback), after it loaded the label table into $s6.
extern "C" void rush2_wings_menu_labels(uint8_t* rdram, recomp_context* ctx) {
    if (!menu_has_wings) {
        return;
    }
    if (new_label_table == 0) {
        build_label_table(rdram);
    }
    ctx->r22 = (int32_t)new_label_table;
}

// func_800B6134, at the end of each label row, after the row counter ($s3) was incremented and before the loop
// test (patched to 10 rows) and the row step of 17 pixels in $s2. Ends the loop after 9 rows without wings, and
// makes the step 15 pixels with them.
extern "C" void rush2_wings_menu_label_row(uint8_t* rdram, recomp_context* ctx) {
    if (!menu_has_wings) {
        if ((int32_t)ctx->r19 >= original_rows) {
            ctx->r19 = original_rows + 1;
        }
        return;
    }
    ctx->r18 = (int32_t)ctx->r18 - (original_row_spacing - row_spacing);
}
