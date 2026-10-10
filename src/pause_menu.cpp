// The race pause menu's additions (the General tab's Pause Menu Extras option, on by default; off, the menu is Rush
// 2's own). Rush 2's pause menu (race_pause_menu_step, func_800AFD44; widgets built by func_800AFB50):
// - State 0x8002305C (s8): 0 racing, 1 the menu's list, 2 OPTIONS, 3 CONTROLS, 4 AUDIO, 5 and 6 RESTART's and QUIT's
//   ARE YOU SURE. Entering a state rebuilds the widgets (func_800AFB50(_, state)).
// - Cursor 0x80119635 (s8): 0 CONTINUE, 1 OPTIONS, 2 CONTROLS, 3 AUDIO, 4 RESTART, 5 QUIT. Set to 0 when the race
//   pauses; up and down wrap it through 0-5; A on 1-3 enters state cursor + 1, on 4 and 5 the confirm screens; B,
//   START or A on CONTINUE resume.
// - The list's widgets (table 0x800BF03C, 13 records of 0x28 bytes: +0 model name, +4 x, +6 y, +0x20 update
//   func_800B558C, +0x24 kind) are the PAUSED title (kind 0x80), then per row its image twice: kind 0x89 + row
//   (the image's lower half, grey) and 0x81 + row (its upper half, yellow, shown flashing while the cursor is on the
//   row: kind & 0xF == cursor + 1). The images are models (PAUSECONTINUE .. PAUSEQUIT), rows 15 pixels apart from
//   y 93; the update centres each on x 160.
//
// SKIP TRACK goes between AUDIO and RESTART. The rows' y in the table are moved up before the widgets are built
// (func_800AFB50 entry) to make room: 85 to 175, still 15 apart. SKIP TRACK has no widget: its label is printed after
// the HUD's widget loop, grey, or yellow under the cursor. On the row the cursor is 6, which no widget's kind
// matches, so every image shows grey. The list's input is taken before the game reads it (0x800B0060, $a0 = the
// buttons pressed, $v0 = B or START): up and down move the cursor over the seven rows here (their bits are cleared,
// so the game's 0-5 wrap never runs), and A on SKIP TRACK skips the song (src/music.cpp) and is cleared.
//
// The song box (bottom right) shows the race's song, its game and the courses it plays on, the line scrolling when
// it is longer than the box. The collectibles box (top right, one player races only) counts what player 1's profile
// has found on the track (src/collectibles.cpp), a spinning model of the key, Dew can or coin beside each count as
// Rush 2049's HUD shows its coins. Both sit 8 pixels from the screen's edges, anchored to the window's right edge;
// their text is drawn at once (not queued, as the race's text is) so it takes the anchor. docs/game_fixes.md "Pause
// menu extras" has the details.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "recomp.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

#include "rush2.h"
#include "collectibles.h"
#include "music.h"
#include "pause_menu.h"
#include "track1.h"
#include "track2049.h"

extern "C" void pause_menu_build_widgets_800AFB50(uint8_t* rdram, recomp_context* ctx); // Pause widgets for state $a1.
extern "C" void menu_play_sound_80064908(uint8_t* rdram, recomp_context* ctx);             // Plays a menu sound.
extern "C" void text_select_font_80088C24(uint8_t* rdram, recomp_context* ctx);            // Selects a font.
extern "C" void text_select_style_800737E4(uint8_t* rdram, recomp_context* ctx);           // Selects a text color style.
extern "C" void text_measure_string_800732AC(uint8_t* rdram, recomp_context* ctx);         // Width of a string.
extern "C" void text_print_string_800734E0(uint8_t* rdram, recomp_context* ctx);           // Prints a string at (x, y).
extern "C" void model_find_by_name_8005BE3C(uint8_t* rdram, recomp_context* ctx);          // Model by name ($a0, slots $a1..$a2).

namespace {
    constexpr uint32_t pause_state = 0x8002305C;    // s8, see above.
    constexpr uint32_t cursor = 0x80119635;         // s8
    constexpr uint32_t list_table = 0x800BF03C;
    constexpr uint32_t record_size = 0x28;
    constexpr int record_y = 6;
    constexpr int list_records = 13;
    constexpr uint32_t race_players = 0x8010C3E2;   // s16
    constexpr uint32_t track_id = 0x8010C3F0;       // s8
    constexpr uint32_t font_byte = 0x800D402C;      // u8: the font the text drawer uses.
    // f32: below 0 (as during a race) func_80072DA4 queues text to be drawn later, at 0 or above it draws it at once
    // (func_80072A38), where it takes the anchor set before it. func_80093FA8 sets it, and bit 0x10 of the word
    // 0x800D3F18 above 0.
    constexpr uint32_t text_scale = 0x800BEF48;
    constexpr uint32_t text_scale_flags = 0x800D3F18;

    // The rows' tops: Rush 2's six, and the seven with SKIP TRACK (index 4).
    constexpr int16_t stock_y[6] = { 93, 108, 123, 138, 153, 168 };
    constexpr int16_t extras_y[7] = { 85, 100, 115, 130, 145, 160, 175 };
    constexpr int skip_row = 4;
    constexpr int skip_cursor = 6;  // The cursor's value on SKIP TRACK: no widget's kind matches it.

    constexpr uint16_t button_a = 0x8000;
    constexpr uint16_t button_up = 0x0800;
    constexpr uint16_t button_down = 0x0400;
    constexpr int sound_up = 0x3B;      // The pause menu's own (func_800AFD44).
    constexpr int sound_down = 0x44;
    constexpr int sound_select = 0x31;
    constexpr int sound_refused = 0x32;

    // Text: fonts (tools/font_pack/fonts.json) and styles (0x800BEF6C: 1 white, 2 grey, 0xE yellow).
    constexpr int font_small = 8;
    constexpr int font_medium = 9;
    constexpr int style_white = 1;
    constexpr int style_grey = 2;
    constexpr int style_yellow = 0xE;
    // The boxes' distance from the screen's edges (the game's text clip, x 308, is lifted while they are drawn).
    constexpr int margin = 8;
    constexpr int box_right = 320 - margin;
    constexpr uint32_t clip_inset_x = 0x80023058;   // s16 x, y: the text drawer clips to this inset (func_80058AFC).
    constexpr uint32_t clip_inset_y = 0x8002305A;

    // Spare RDRAM (src/controls_menu.cpp lists the ranges in use): the strings printed this frame and the music icon.
    constexpr uint32_t text_buffer = 0x80C9C000;
    constexpr uint32_t text_buffer_size = 0x800;
    constexpr uint32_t icon_image = 0x80C9C800;     // icon_size x icon_size RGBA16
    constexpr int icon_size = 16;
    constexpr uint32_t label_images = 0x80C9CA00;   // SKIP TRACK, grey then yellow: label_max_w x row_h RGBA16 each
    constexpr int label_max_w = 160;
    constexpr int row_h = 10;   // A row's image half: an outline row, 8 rows of letters, an outline row.

    // The HUD's widgets and their images (src/hud.cpp).
    constexpr uint32_t widget_array = 0x802F6C00;   // 0x20 bytes each
    constexpr uint32_t widget_size = 0x20;
    constexpr uint32_t widget_count = 0x800F5444;
    constexpr int32_t widget_palette = 0x0;         // Palette (colors at +0x14), or 0.
    constexpr int32_t widget_image = 0x4;
    constexpr int32_t widget_palette_bank = 0x8;    // u16, >> 10 selects the palette table.
    constexpr int32_t widget_flags = 0x15;          // 0x80: +4 is a draw callback, not an image.
    constexpr int32_t image_width = 0x10;
    constexpr int32_t image_height = 0x12;
    constexpr int32_t image_palette = 0x16;         // s16 index into the palette table.
    constexpr int32_t image_pixels = 0x18;
    constexpr uint32_t palette_tables = 0x80119428; // 8 bytes each, a pointer to 0x18 byte palettes (colors at +0x14).

    std::atomic<bool> enabled = true;
    bool layout_extras = false;     // Whether the list's widgets were built with SKIP TRACK's room.
    uint32_t text_at = 0;
    bool icon_written = false;
    int label_w = 0;                // The built label's width (texels, padded), 0 until it is built,
    int label_ink = 0;              // and the width its letters and outline take.
    uint32_t label_source = 0;      // The image it was built from (rebuilt if the HUD's images move).
    int flash = 16;                 // The cursor's flash, 0-31 (16 = full).

    // Calls a game function from a hook, keeping the hooked function's registers. Returns $v0.
    int32_t call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0 = 0,
                 int32_t a1 = 0, int32_t a2 = 0, int32_t a3 = 0) {
        recomp_context saved = *ctx;
        ctx->r4 = a0;
        ctx->r5 = a1;
        ctx->r6 = a2;
        ctx->r7 = a3;
        func(rdram, ctx);
        int32_t ret = (int32_t)ctx->r2;
        *ctx = saved;
        return ret;
    }

    void play_sound(uint8_t* rdram, recomp_context* ctx, int sound) {
        call(rdram, ctx, menu_play_sound_80064908, sound);
    }

    // A string in RDRAM for the text drawer; kept until the next frame's drawing (the text is drawn later).
    uint32_t str(uint8_t* rdram, const std::string& s) {
        if (text_at + s.size() + 1 > text_buffer_size) {
            text_at = 0;
        }
        uint32_t at = text_buffer + text_at;
        for (size_t i = 0; i <= s.size(); i++) {
            MEM_B(0, (int32_t)(at + i)) = i < s.size() ? s[i] : 0;
        }
        text_at += (uint32_t)((s.size() + 4) & ~3u);
        return at;
    }

    std::string upper(const std::string& s) {
        std::string out;
        for (char c : s) {
            out.push_back(c >= 'a' && c <= 'z' ? char(c - 32) : c);
        }
        return out;
    }

    int width(uint8_t* rdram, recomp_context* ctx, const std::string& s) {
        return call(rdram, ctx, text_measure_string_800732AC, (int32_t)str(rdram, s), -1);
    }

    // The corner boxes and their text are anchored to the window's right edge (HUD Placement), like the race HUD's
    // right hand elements.
    constexpr float right_edge = 1.0f;

    void print(uint8_t* rdram, recomp_context* ctx, int style, int x, int y, const std::string& s) {
        rush2::hud::set_anchor(rdram, right_edge);
        call(rdram, ctx, text_select_style_800737E4, style);
        call(rdram, ctx, text_print_string_800734E0, x, y, (int32_t)str(rdram, s));
    }

    // s cut to max_w pixels, ending in "..." if it was cut.
    std::string fit(uint8_t* rdram, recomp_context* ctx, std::string s, int max_w) {
        if (width(rdram, ctx, s) <= max_w) {
            return s;
        }
        while (!s.empty() && width(rdram, ctx, s + "...") > max_w) {
            s.pop_back();
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == ',')) {
            s.pop_back();
        }
        return s + "...";
    }

    // A box: dark fill under a one pixel grey frame, as the pause menu's own.
    void draw_box(uint8_t* rdram, float x0, float y0, float x1, float y1) {
        constexpr uint32_t fill = 0x000000F0, frame = 0x8C8C8CFF;
        rush2::hud::draw_rect(rdram, x0, y0, x1, y1, fill, right_edge);
        rush2::hud::draw_rect(rdram, x0, y0, x1, y0 + 1, frame, right_edge);
        rush2::hud::draw_rect(rdram, x0, y1 - 1, x1, y1, frame, right_edge);
        rush2::hud::draw_rect(rdram, x0, y0 + 1, x0 + 1, y1 - 1, frame, right_edge);
        rush2::hud::draw_rect(rdram, x1 - 1, y0 + 1, x1, y1 - 1, frame, right_edge);
    }

    // Two beamed eighth notes, yellow like the menu's highlight.
    void write_icon(uint8_t* rdram) {
        static const char* const rows[icon_size] = {
            "................",
            ".....##########.",
            ".....##########.",
            ".....##......##.",
            ".....##......##.",
            ".....##......##.",
            ".....##......##.",
            ".....##......##.",
            ".....##......##.",
            ".....##......##.",
            "..#####...#####.",
            ".######..######.",
            ".######..######.",
            "..####....####..",
            "................",
            "................",
        };
        constexpr uint16_t yellow = (31 << 11) | (31 << 6) | (0 << 1) | 1;
        for (int y = 0; y < icon_size; y++) {
            for (int x = 0; x < icon_size; x++) {
                MEM_H(0, (int32_t)(icon_image + (y * icon_size + x) * 2)) = (int16_t)(rows[y][x] == '#' ? yellow : 0);
            }
        }
        icon_written = true;
    }

    bool is_rdram(uint32_t address) {
        return address >= 0x80000000 && address < 0x80800000;
    }

    // A row image of the pause list (a CI4 model image, stored upside down: the grey half in rows 0-9, the yellow one
    // in rows 10-19, letters in color 2 or 3 with a one texel color 1 outline), found on the list's widgets.
    struct RowImage {
        uint32_t image = 0, pixels = 0, palette = 0;
        int w = 0, h = 0;
    };

    int texel(uint8_t* rdram, const RowImage& r, int x, int y) {
        uint32_t i = (uint32_t)(y * r.w + x);
        uint8_t b = (uint8_t)MEM_BU(0, (int32_t)(r.pixels + i / 2));
        return (i & 1) ? (b & 0xF) : (b >> 4);
    }

    bool find_row_image(uint8_t* rdram, const char* name, RowImage& out) {
        uint32_t count = std::min<uint32_t>((uint32_t)MEM_W(0, (int32_t)widget_count), 0x200);
        for (uint32_t i = 0; i < count; i++) {
            int32_t widget = (int32_t)(widget_array + i * widget_size);
            uint32_t image = (uint32_t)MEM_W(widget_image, widget);
            if ((MEM_BU(widget_flags, widget) & 0x80) != 0 || !is_rdram(image)) {
                continue;
            }
            bool match = true;
            for (int c = 0; c < 16; c++) {
                if ((char)MEM_B(c, (int32_t)image) != name[c]) {
                    match = false;
                    break;
                }
                if (name[c] == 0) {
                    break;
                }
            }
            if (!match) {
                continue;
            }
            out.image = image;
            out.pixels = (uint32_t)MEM_W(image_pixels, (int32_t)image);
            out.w = MEM_HU(image_width, (int32_t)image);
            out.h = MEM_HU(image_height, (int32_t)image);
            uint32_t named = (uint32_t)MEM_W(widget_palette, widget);
            int32_t index = MEM_H(image_palette, (int32_t)image);
            if (is_rdram(named)) {
                out.palette = (uint32_t)MEM_W(0x14, (int32_t)named);
            }
            else if (index >= 0) {
                uint32_t table = (uint32_t)MEM_W((MEM_HU(widget_palette_bank, widget) >> 10) * 8, (int32_t)palette_tables);
                out.palette = is_rdram(table) ? (uint32_t)MEM_W(index * 0x18 + 0x14, (int32_t)table) : 0;
            }
            return is_rdram(out.pixels) && is_rdram(out.palette) && out.h == row_h * 2 && out.w > 0;
        }
        return false;
    }

    // A letter: its 8 rows (top down) of '#' (letter) and '.'.
    using Glyph = std::vector<std::string>;

    // The letters of a row image's yellow half, left to right: its connected shapes of letter texels (neighboring
    // letters can share columns, as RESTART's S, T and A do, but never touch).
    std::vector<Glyph> letters_of(uint8_t* rdram, const RowImage& r) {
        // Rows 1-8 of the half as drawn (flipped), top down: 0 empty, 1 letter, 2 letter already taken.
        std::vector<std::vector<int>> lit(8, std::vector<int>(r.w));
        for (int row = 0; row < 8; row++) {
            for (int x = 0; x < r.w; x++) {
                lit[row][x] = texel(rdram, r, x, row_h + (row_h - 2 - row)) == 3 ? 1 : 0;
            }
        }
        std::vector<Glyph> out;
        for (int x = 0; x < r.w; x++) {
            for (int row = 0; row < 8; row++) {
                if (lit[row][x] != 1) {
                    continue;
                }
                std::vector<std::pair<int, int>> shape, stack = { { x, row } };
                lit[row][x] = 2;
                while (!stack.empty()) {
                    auto [px, py] = stack.back();
                    stack.pop_back();
                    shape.push_back({ px, py });
                    const int steps[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                    for (const auto& st : steps) {
                        int qx = px + st[0], qy = py + st[1];
                        if (qx >= 0 && qx < r.w && qy >= 0 && qy < 8 && lit[qy][qx] == 1) {
                            lit[qy][qx] = 2;
                            stack.push_back({ qx, qy });
                        }
                    }
                }
                int x0 = r.w, x1 = 0;
                for (const auto& p : shape) {
                    x0 = std::min(x0, p.first);
                    x1 = std::max(x1, p.first);
                }
                Glyph g(8, std::string(x1 - x0 + 1, '.'));
                for (const auto& p : shape) {
                    g[p.second][p.first - x0] = '#';
                }
                out.push_back(g);
            }
        }
        return out;
    }

    // Builds SKIP TRACK in the rows' lettering: S, I, P, T, R, A and C from RESTART, CONTINUE and OPTIONS (read from
    // the list's images, so none of the game's pixels are kept here), K drawn to match (R's square bowl, N's two texel
    // diagonals). Letters are a texel apart, as in the images; the space is 5 texels. False until the images are there.
    bool build_label(uint8_t* rdram) {
        RowImage restart, cont, options;
        if (!find_row_image(rdram, "PAUSERESTART", restart) || !find_row_image(rdram, "PAUSECONTINUE", cont) ||
            !find_row_image(rdram, "PAUSEOPTIONS", options)) {
            return false;
        }
        if (label_w != 0 && label_source == restart.image) {
            return true;
        }
        std::vector<Glyph> r = letters_of(rdram, restart), c = letters_of(rdram, cont), o = letters_of(rdram, options);
        if (r.size() != 7 || c.size() != 8 || o.size() != 7) {
            return false;
        }
        const Glyph k = {
            "##.......##",
            "##......##.",
            "##.....##..",
            "#########..",
            "#########..",
            "##.....##..",
            "##......##.",
            "##.......##",
        };
        const Glyph* text[] = { &r[2], &k, &c[4], &o[1], nullptr, &r[3], &r[0], &r[4], &c[0], &k };
        // The letters on a canvas with a texel of room around them for the outline.
        std::vector<std::string> core(row_h, std::string(label_max_w, '.'));
        int x = 1;
        for (const Glyph* g : text) {
            if (g == nullptr) {
                x += 4;
                continue;
            }
            int gw = (int)(*g)[0].size();
            if (x + gw + 1 > label_max_w) {
                return false;
            }
            for (int row = 0; row < 8; row++) {
                for (int i = 0; i < gw; i++) {
                    if ((*g)[row][i] == '#') {
                        core[row + 1][x + i] = '#';
                    }
                }
            }
            x += gw + 1;
        }
        // Texture rows are loaded in 8 byte units: the image is padded to a multiple of 4 texels.
        int w = (x + 3) & ~3;
        uint16_t outline = (uint16_t)MEM_HU(2, (int32_t)restart.palette);
        const uint16_t colors[2] = { (uint16_t)MEM_HU(4, (int32_t)restart.palette), (uint16_t)MEM_HU(6, (int32_t)restart.palette) };
        for (int half = 0; half < 2; half++) {
            uint32_t base = label_images + (uint32_t)(half * label_max_w * row_h * 2);
            for (int y = 0; y < row_h; y++) {
                for (int i = 0; i < w; i++) {
                    uint16_t value = 0;
                    if (core[y][i] == '#') {
                        value = colors[half];
                    }
                    else {
                        for (int dy = -1; dy <= 1 && value == 0; dy++) {
                            for (int dx = -1; dx <= 1 && value == 0; dx++) {
                                int yy = y + dy, xx = i + dx;
                                if (yy >= 0 && yy < row_h && xx >= 0 && xx < w && core[yy][xx] == '#') {
                                    value = outline;
                                }
                            }
                        }
                    }
                    MEM_H(0, (int32_t)(base + (uint32_t)(y * w + i) * 2)) = (int16_t)value;
                }
            }
        }
        label_w = w;
        label_ink = x;
        label_source = restart.image;
        return true;
    }

    // SKIP TRACK, centred on x 160 as the game centres its rows: grey, or under the cursor yellow, its alpha going
    // up and down between 0x7F and 0xFF a step of 8 a frame as the game's rows do (func_800B558C: 0x800D3A5C, step
    // 0x800D3B20).
    void draw_label(uint8_t* rdram, bool on) {
        if (!build_label(rdram)) {
            return;
        }
        float x0 = 160.0f - (float)(label_ink / 2), y0 = (float)extras_y[skip_row];
        uint32_t image = label_images + (on ? (uint32_t)(label_max_w * row_h * 2) : 0);
        uint8_t alpha = 0xFF;
        if (on) {
            flash = (flash + 1) % 32;
            alpha = (uint8_t)(0x7F + 8 * (flash <= 16 ? flash : 32 - flash));
        }
        else {
            flash = 16;
        }
        rush2::hud::draw_image_part(rdram, image, label_w, row_h, 0, 0, label_w, row_h, x0, y0, x0 + (float)label_w,
                                    y0 + (float)row_h, 0.5f, false, alpha);
    }

    // Sets the list's rows' y in its widget table for the layout.
    void lay_out(uint8_t* rdram, bool extras) {
        for (int i = 1; i < list_records; i++) {
            int row = (i - 1) / 2;
            int16_t y = extras ? extras_y[row < skip_row ? row : row + 1] : stock_y[row];
            MEM_H(0, (int32_t)(list_table + i * record_size + record_y)) = y;
        }
        layout_extras = extras;
    }

    // The row (0-6) the cursor is on, with SKIP TRACK as row 4.
    int row_of(int c) {
        return c == skip_cursor ? skip_row : c < skip_row ? c : c + 1;
    }

    int cursor_of(int row) {
        return row == skip_row ? skip_cursor : row < skip_row ? row : row - 1;
    }

    struct Count {
        const char* label;                  // Shown when the model isn't there.
        std::vector<const char*> models;    // The collectible's model, by the names it can have.
        int found, total;
    };

    // What player 1 has found on the race's track, or nothing on a track without collectibles.
    std::vector<Count> counts(uint8_t* rdram) {
        using namespace rush2::collectibles;
        std::vector<Count> out;
        int8_t t = (int8_t)MEM_B(0, (int32_t)track_id);
        bool host = t == rush2::track2049::host_slot;
        Progress p = player_progress(rdram, 0);
        auto bits = [](unsigned m) { return std::popcount(m); };
        // Rush 2049's coins (converted into the track's models, src/rush2049/track2049_convert.cpp), gold first as
        // Rush 2049's HUD shows them.
        auto coins = [&](uint16_t m) {
            out.push_back({ "GOLD", { "GOLDCOING_COIN" }, bits(m & gold_bits), coins_per_kind });
            out.push_back({ "SILVER", { "SILVERCOINS_COI" }, bits(m & silver_bits), coins_per_kind });
        };
        int k = rush2::track2049::race_track();
        if (host && k > 0) {
            if (k <= rush2049_courses) {
                coins(p.rush2049[k - 1]);
            }
        }
        else if (t == rush2::track2049::stunt_host_slot && rush2::track2049::stunt_arena() > 0) {
            int a = rush2::track2049::stunt_arena();
            if (a <= stunt2049_courses) {
                coins(p.stunt2049[a - 1]);
            }
        }
        else if (t == rush2::track2049::stunt_host_slot && rush2::track2049::battle_arena() > 0) {
            // Battle arenas have none.
        }
        else if (host && rush2::track1::race_track() > 0) {
            // Rush 1's key model, renamed if its name is one of Rush 2's classes (src/rush1/track1_convert.cpp).
            int s = rush2::track1::race_track();
            if (s <= sfrush_courses) {
                out.push_back({ "KEYS", { "KEYL1", "R1KEYL1" }, bits(p.sfrush[s - 1]), sfrush_keys[s - 1] });
            }
        }
        else if (t >= 0 && t < rush2_courses) {
            // Rush 2's key and Dew can (asset 18, loaded for every race).
            out.push_back({ "KEYS", { "KEYO1" }, bits(p.rush2[t] & rush2_key_bits), rush2_keys });
            out.push_back({ "CANS", { "DOTHEDEWO1" }, bits(p.rush2[t] & rush2_can_bits), rush2_cans });
        }
        return out;
    }

    // Spinning models: a display list per model drawn, called from the 2D display list (rush2::hud::call_list), in
    // spare RDRAM rotated over frames so the renderer never reads one being rewritten.
    constexpr uint32_t model_tables = 0x80118D78;   // Per model slot (8 bytes): its model records (0x34 bytes each).
    constexpr uint32_t model_slots = 0x800D5788;    // u8: loaded slots (func_8005BE3C searches 0..n-1).
    constexpr uint32_t palette_cache = 0x80111954;  // func_80078190: palette last loaded into TMEM, 0 for none.
    // func_80055F78: the 2D image last loaded into TMEM (it skips loading the same one again, so text after a model
    // would be drawn from the model's texture).
    constexpr uint32_t last_2d_image = 0x80119638;
    constexpr uint32_t icon_lists = 0x80C9E300;
    constexpr uint32_t icon_list_size = 0x300;
    constexpr int icon_list_count = 4;
    int icon_list_index = 0;

    // The first level of detail's display list of the loaded model called `name`, or 0.
    uint32_t model_list(uint8_t* rdram, recomp_context* ctx, const char* name) {
        int slots = MEM_BU(0, (int32_t)model_slots);
        int32_t h = call(rdram, ctx, model_find_by_name_8005BE3C, (int32_t)str(rdram, name), 0, slots - 1, 1);
        if (h < 0 || (h & 0xFFFF) == 0xFFFF || (h >> 10) >= slots) {
            return 0;
        }
        uint32_t records = (uint32_t)MEM_W(0, (int32_t)(model_tables + (uint32_t)(h >> 10) * 8));
        if (!is_rdram(records)) {
            return 0;
        }
        uint32_t dl = (uint32_t)MEM_W(0, (int32_t)(records + (uint32_t)(h & 0x3FF) * 0x34 + 0xC));
        return (dl & 0xFFFFFF) != 0 ? dl : 0;
    }

    // Grows box (min x, y, z, max x, y, z) by the vertices display list dl loads, and those of the lists it calls
    // (as src/rush2049/battle_render.cpp's dl_bounds).
    void dl_bounds(uint8_t* rdram, uint32_t dl, float box[6], int depth) {
        if (depth > 8) {
            return;
        }
        dl = 0x80000000u | (dl & 0xFFFFFF);
        for (int n = 0; n < 4096; n++, dl += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)dl), w1 = (uint32_t)MEM_W(0, (int32_t)(dl + 4));
            uint32_t op = w0 >> 24, target = 0x80000000u | (w1 & 0xFFFFFF);
            if (op == 0x01) {
                int count = (int)((w0 >> 12) & 0xFF);
                for (int v = 0; v < count; v++) {
                    for (int k = 0; k < 3; k++) {
                        float c = (float)(int16_t)MEM_H(0, (int32_t)(target + (uint32_t)v * 16 + (uint32_t)k * 2));
                        box[k] = std::min(box[k], c);
                        box[3 + k] = std::max(box[3 + k], c);
                    }
                }
            }
            else if (op == 0xDE) {
                dl_bounds(rdram, target, box, depth + 1);
                if (((w0 >> 16) & 0xFF) != 0) {
                    return;
                }
            }
            else if (op == 0xDF) {
                return;
            }
        }
    }

    // Draws model list `dl` spinning about its upright axis in a size x size square centred on (cx, cy) of the 4:3
    // screen, anchored to the window's right edge, in front of everything. A flat model (thinnest top to bottom, a
    // coin lying down) is stood up to face the screen first. `id` (0-7) keeps its matrices apart for the frame
    // interpolation. Its own projection maps the model's bounds to the square (orthographic), and the fog factor is
    // zeroed so the models' fog render modes leave its colors alone (each view sets its fog again, func_8007C624).
    void draw_model(uint8_t* rdram, uint32_t dl, float cx, float cy, float size, int id) {
        float box[6] = { 1e9f, 1e9f, 1e9f, -1e9f, -1e9f, -1e9f };
        dl_bounds(rdram, dl, box, 0);
        if (box[0] > box[3]) {
            return;
        }
        float mid[3], half[3];
        for (int k = 0; k < 3; k++) {
            mid[k] = (box[k] + box[3 + k]) * 0.5f;
            half[k] = std::max((box[3 + k] - box[k]) * 0.5f, 1.0f);
        }
        // Row vectors (v' = v M), as the N64's matrices.
        auto mul = [](const float a[4][4], const float b[4][4], float out[4][4]) {
            float r[4][4];
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
                }
            }
            memcpy(out, r, sizeof(r));
        };
        float m[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { -mid[0], -mid[1], -mid[2], 1 } };
        bool flat = half[1] < half[0] && half[1] < half[2];
        if (flat) {
            const float stand[4][4] = { { 1, 0, 0, 0 }, { 0, 0, 1, 0 }, { 0, -1, 0, 0 }, { 0, 0, 0, 1 } };
            mul(m, stand, m);
            std::swap(half[1], half[2]);
        }
        using clock = std::chrono::steady_clock;
        static const clock::time_point start = clock::now();
        float a = std::chrono::duration<float>(clock::now() - start).count() * 2.0f * 3.14159265f / 2.5f;
        const float spin[4][4] = { { std::cos(a), 0, -std::sin(a), 0 }, { 0, 1, 0, 0 }, { std::sin(a), 0, std::cos(a), 0 },
                                   { 0, 0, 0, 1 } };
        mul(m, spin, m);
        // Sized by its widest side rather than its diagonal, so it fills the square face on (its corners may
        // reach past it for a moment as it turns).
        float reach = std::max(std::max(half[0], half[2]) * 1.1f, half[1]);
        float sc = 1.0f / reach;
        const float scale[4][4] = { { sc, 0, 0, 0 }, { 0, sc, 0, 0 }, { 0, 0, sc, 0 }, { 0, 0, 0, 1 } };
        mul(m, scale, m);
        // Depth just past the near plane, so the model is drawn over the scene and hides its own far side.
        const float projection[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, -0.05f, 0 }, { 0, 0, -0.9f, 1 } };

        uint32_t base = icon_lists + (uint32_t)icon_list_index * icon_list_size;
        icon_list_index = (icon_list_index + 1) % icon_list_count;
        uint32_t mv_at = base, proj_at = base + 64, vp_at = base + 128, list = base + 0x90;
        auto floats = [&](uint32_t at, const float f[4][4]) {
            for (int i = 0; i < 16; i++) {
                MEM_W(0, (int32_t)(at + i * 4)) = (int32_t)std::bit_cast<uint32_t>(f[i / 4][i % 4]);
            }
        };
        floats(mv_at, m);
        floats(proj_at, projection);
        // Viewport (s16 x, y, z, 0 scale, then translation; quarter pixels), x from the right edge (G_EX_ORIGIN_RIGHT).
        const int16_t vp[8] = { (int16_t)std::lround(size * 2.0f), (int16_t)std::lround(size * 2.0f), 0x1FF, 0,
                                (int16_t)std::lround((cx - 320.0f) * 4.0f), (int16_t)std::lround(cy * 4.0f), 0x1FF, 0 };
        for (int i = 0; i < 8; i++) {
            MEM_H(0, (int32_t)(vp_at + i * 2)) = vp[i];
        }
        uint32_t count = 0;
        auto cmd = [&](uint32_t w0, uint32_t w1) {
            MEM_W(0, (int32_t)(list + count * 8)) = (int32_t)w0;
            MEM_W(0, (int32_t)(list + count * 8 + 4)) = (int32_t)w1;
            count++;
        };
        auto ex = [&](const GfxCommand* c, int n) {
            for (int i = 0; i < n; i++) {
                cmd(c[i].values.word0, c[i].values.word1);
            }
        };
        GfxCommand c[2];
        gEXEnable(c);
        ex(c, 1);
        cmd(0xE7000000, 0);
        gEXViewport(c, G_EX_ORIGIN_RIGHT, vp_at);
        ex(c, 2);
        gEXMatrixGroup(c, 0x6F000000u | (uint32_t)id, G_EX_INTERPOLATE_SIMPLE, G_EX_NOPUSH, 1,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
        ex(c, 2);
        gEXMatrixFloat(c, proj_at, 0x07);   // projection, load
        ex(c, 2);
        gEXMatrixGroup(c, 0x6E000000u | (uint32_t)id, G_EX_INTERPOLATE_DECOMPOSE, G_EX_PUSH, 0,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_AUTO,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
        ex(c, 2);
        gEXMatrixFloat(c, mv_at, 0x02);     // modelview, load, push (the push bit is stored inverted)
        ex(c, 2);
        // The render state the game draws its models with (src/rush2049/battle_render.cpp), Rush 2's TLUT mode.
        cmd(0xE3000A01, 0x00100000);    // CYCLETYPE = 2CYCLE
        cmd(0xE3000C00, 0x00080000);    // TEXTPERSP = PERSP
        cmd(0xE3001201, 0x00002000);    // TEXTFILT = BILERP
        cmd(0xE3001001, 0x00008000);    // TEXTLUT = RGBA16
        cmd(0xD9F9FFFF, 0x00000000);    // clear LIGHTING | TEXTURE_GEN
        cmd(0xD9FFFFFF, 0x00210005);    // set ZBUFFER | SHADE | SHADING_SMOOTH | FOG
        cmd(0xD7000002, 0xFFFFFFFF);    // G_TEXTURE on
        cmd(0xDB080000, 0x00000000);    // fog factor 0
        cmd(0xFA000000, 0xFFFFFFFF);
        cmd(0xDE000000, dl);
        cmd(0xD8380002, 0x00000040);    // pop the modelview
        gEXPopMatrixGroup(c, 0);
        ex(c, 1);
        cmd(0xE7000000, 0);
        cmd(0xDF000000, 0);
        rush2::hud::set_anchor(rdram, right_edge);  // widens the scissor to the window
        rush2::hud::call_list(rdram, list);
        MEM_W(0, (int32_t)palette_cache) = 0;
        MEM_W(0, (int32_t)last_2d_image) = 0;
    }

    void draw_counts(uint8_t* rdram, recomp_context* ctx) {
        std::vector<Count> rows = counts(rdram);
        if (rows.empty()) {
            return;
        }
        constexpr int pad = 5, model_size = 16, gap = 6, line_h = 18;
        call(rdram, ctx, text_select_font_80088C24, font_medium);
        std::vector<std::string> values;
        std::vector<uint32_t> lists;
        int value_w = 0, label_w = 0;
        for (const Count& c : rows) {
            values.push_back(std::to_string(c.found) + "/" + std::to_string(c.total));
            value_w = std::max(value_w, width(rdram, ctx, values.back()));
            uint32_t dl = 0;
            for (const char* name : c.models) {
                if (dl == 0) {
                    dl = model_list(rdram, ctx, name);
                }
            }
            lists.push_back(dl);
            label_w = std::max(label_w, dl != 0 ? model_size : width(rdram, ctx, c.label));
        }
        int x1 = box_right, y0 = margin;
        int x0 = x1 - (pad + label_w + gap + value_w + pad);
        int y1 = y0 + pad * 2 + line_h * (int)rows.size();
        draw_box(rdram, (float)x0, (float)y0, (float)x1, (float)y1);
        for (size_t i = 0; i < rows.size(); i++) {
            float mid_y = (float)(y0 + pad) + line_h * ((float)i + 0.5f);
            if (lists[i] != 0) {
                draw_model(rdram, lists[i], (float)(x0 + pad) + model_size * 0.5f, mid_y, (float)model_size, (int)i);
            }
            else {
                print(rdram, ctx, style_grey, x0 + pad, (int)mid_y - 5, rows[i].label);
            }
            bool all = rows[i].found >= rows[i].total;
            print(rdram, ctx, all ? style_yellow : style_white, x1 - pad - width(rdram, ctx, values[i]), (int)mid_y - 5,
                  values[i]);
        }
    }

    void draw_song(uint8_t* rdram, recomp_context* ctx) {
        rush2::music::NowPlaying np;
        if (!rush2::music::now_playing(np)) {
            return;
        }
        constexpr int pad = 5, max_text_w = 170, name_h = 10, line_h = 8;   // max_text_w: the box's text width
        call(rdram, ctx, text_select_font_80088C24, font_medium);
        std::string name = fit(rdram, ctx, upper(np.song), max_text_w);
        call(rdram, ctx, text_select_font_80088C24, font_small);
        std::string from = upper(np.game) + (np.course.empty() ? "" : " / " + upper(np.course));
        int from_w = width(rdram, ctx, from);

        // A fixed width, as a music player's: short names don't shrink the box.
        int text_w = max_text_w;
        int x1 = box_right, y1 = 240 - margin;
        int x0 = x1 - (pad + icon_size + 6 + text_w + pad);
        int y0 = y1 - (pad * 2 + name_h + 2 + line_h);
        draw_box(rdram, (float)x0, (float)y0, (float)x1, (float)y1);
        if (!icon_written) {
            write_icon(rdram);
        }
        float icon_y = (float)(y0 + y1 - icon_size) * 0.5f;
        rush2::hud::draw_image(rdram, icon_image, icon_size, icon_size, (float)(x0 + pad), icon_y,
                               (float)(x0 + pad + icon_size), icon_y + icon_size, right_edge);
        int text_x = x0 + pad + icon_size + 6;
        call(rdram, ctx, text_select_font_80088C24, font_medium);
        print(rdram, ctx, style_white, text_x, y0 + pad, name);
        call(rdram, ctx, text_select_font_80088C24, font_small);
        int from_y = y0 + pad + name_h + 2;
        if (from_w <= text_w) {
            print(rdram, ctx, style_grey, text_x, from_y, from);
            return;
        }
        // Too long for the box: it scrolls left as a music player's does, holding at its start between passes, a
        // second copy following it round.
        constexpr float speed = 20.0f, hold = 2.0f;   // 4:3 pixels a second; seconds at the start
        constexpr int spacing = 24;
        using clock = std::chrono::steady_clock;
        static std::string scrolled;
        static clock::time_point scroll_start;
        if (scrolled != from) {
            scrolled = from;
            scroll_start = clock::now();
        }
        float lap = (float)(from_w + spacing);
        float cycle = hold + lap / speed;
        float t = std::fmod(std::chrono::duration<float>(clock::now() - scroll_start).count(), cycle);
        int shift = t < hold ? 0 : (int)((t - hold) * speed);
        rush2::hud::push_scissor(rdram, (float)text_x, (float)from_y - 1.0f, (float)(text_x + text_w), (float)(from_y + line_h + 2),
                                 right_edge);
        print(rdram, ctx, style_grey, text_x - shift, from_y, from);
        if (text_x - shift + (int)lap < text_x + text_w) {
            print(rdram, ctx, style_grey, text_x - shift + (int)lap, from_y, from);
        }
        rush2::hud::pop_scissor(rdram);
    }
}

void rush2::pause_menu::set_enabled(bool on) {
    enabled = on;
}

void rush2::pause_menu::draw(uint8_t* rdram, recomp_context* ctx) {
    if (!enabled.load() || MEM_B(0, (int32_t)pause_state) != 1) {
        return;
    }
    text_at = 0;
    int saved_font = MEM_BU(0, (int32_t)font_byte);
    int32_t saved_scale = MEM_W(0, (int32_t)text_scale), saved_flags = MEM_W(0, (int32_t)text_scale_flags);
    MEM_W(0, (int32_t)text_scale) = 0;
    MEM_W(0, (int32_t)text_scale_flags) = saved_flags & ~0x10;
    int16_t saved_inset_x = MEM_H(0, (int32_t)clip_inset_x), saved_inset_y = MEM_H(0, (int32_t)clip_inset_y);
    MEM_H(0, (int32_t)clip_inset_x) = 0;
    MEM_H(0, (int32_t)clip_inset_y) = 0;
    if (layout_extras) {
        draw_label(rdram, MEM_B(0, (int32_t)cursor) == skip_cursor);
    }
    draw_song(rdram, ctx);
    if (MEM_H(0, (int32_t)race_players) == 1) {
        draw_counts(rdram, ctx);
    }
    call(rdram, ctx, text_select_font_80088C24, saved_font);
    MEM_W(0, (int32_t)text_scale) = saved_scale;
    MEM_W(0, (int32_t)text_scale_flags) = saved_flags;
    MEM_H(0, (int32_t)clip_inset_x) = saved_inset_x;
    MEM_H(0, (int32_t)clip_inset_y) = saved_inset_y;
}

extern "C" {

// func_800AFB50 entry: before the pause menu's widgets are built, the list's rows are laid out for the option.
void rush2_pause_widgets(uint8_t* rdram, recomp_context* ctx) {
    lay_out(rdram, enabled.load());
}

// race_pause_menu_step (func_800AFD44) at 0x800B0060, the list's input for a player: $a0 = buttons pressed, $v0 =
// B or START (resume, whatever the cursor is on).
void rush2_pause_menu_input(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_B(0, (int32_t)pause_state) != 1) {
        return;
    }
    bool on = enabled.load();
    int c = MEM_B(0, (int32_t)cursor);
    if (on != layout_extras) {
        // The option changed while the menu is open: rebuild the list.
        if (!on && c == skip_cursor) {
            MEM_B(0, (int32_t)cursor) = 3;
        }
        call(rdram, ctx, pause_menu_build_widgets_800AFB50, 0, 1);
        ctx->r4 = 0;
        return;
    }
    if (!on || ctx->r2 != 0) {
        return;
    }
    uint16_t pressed = (uint16_t)ctx->r4;
    int row = row_of(c);
    if (pressed & button_a) {
        if (row != skip_row) {
            return;
        }
        play_sound(rdram, ctx, rush2::music::skip_song(rdram, ctx) ? sound_select : sound_refused);
        ctx->r4 = 0;
        return;
    }
    if (pressed & (button_up | button_down)) {
        bool up = (pressed & button_up) != 0;
        row = (row + (up ? 6 : 1)) % 7;
        MEM_B(0, (int32_t)cursor) = (int8_t)cursor_of(row);
        play_sound(rdram, ctx, up ? sound_up : sound_down);
        ctx->r4 = (int32_t)(pressed & ~(button_up | button_down));
    }
}

}
