// Rush 2049 Menus (include/menus2049.h): Rush 2's front end dressed as Rush 2049's. Reverse engineering in
// docs/rush2049_research/menus.md section 11; tools/rush2049/menu49.py reads the same art and runs 2049's own texture
// loader and widget draw on it.
//
// - Fonts. Rush 2's text drawer (text_draw_string_80072A38) and measurer (text_measure_string_800732AC) read the font
//   table 0x800BEE88 (0x10 bytes a font: +2..+4 the line spacing, +5 a space's advance, +9 the range count, +0xC the
//   ranges; a range is 0x14 bytes: first and last character, the sheet's size and I4 texels, 8-byte glyph rectangles).
//   Rush 2's menu fonts 8, 9 and 10 (8, 10 and 12 pixel capitals) get records in that format built from Rush 2049's
//   menu fonts of the same sizes (AERO-PLAIN7, AERO-PLAIN10 and 4BAERO-PLAIN12), so the game draws and measures
//   them itself. Their line spacing stays Rush 2's, so every screen keeps its layout. Rush 2 prints a list's row under
//   the cursor in the other menu font; 2049 only changes its style, so that row is printed in the list's own font,
//   about the center the game gave it.
// - Colors. 2049's menus draw their text in style 1 (0xE6E6E6, shadow 0x3C3C3C) and the row under the cursor in its
//   style 0x16, black blended to 0xE0E000 by a triangle wave of period 0.5 s (func_800947F0, func_800B74C0). Rush 2's
//   green title styles (9-11) take 2049's white and gray, and Rush 2's flashing style 0x14 takes 2049's pulse.
// - Background. 2049's menus (func_800B8374, func_800FC9F8) draw BACKGROUND_SFX, a 64 x 64 CI4 swirl, on four
//   camera-space quads that meet at the screen's center, tinted by their vertex color 0x0000C0 and scrolled 0.2
//   texture widths a second; the 320 x 220 circuit board (BACKGROUND_A + _B, copy mode with alpha compare) goes over
//   it at (0, 0), its traces transparent, so the swirl runs through them. Past its edges (the 20 lines below it, and
//   the sides in widescreen) the board goes on mirrored about them. Here both are drawn at the start of the
//   frame's display list, after the clear and before the 3D views, and Rush 2's scrolling "RUSH2" mesh is not drawn.
// - Buttons. 2049's menu rows are 3D models (file 54 BUTTON_S0G1..S8G1, 160..480 units wide) at z = 300, turned
//   rotX(angle - pi/2) (func_800D6914): the angle runs to pi at 4 pi a second for the row under the cursor and back
//   to 0 for the others, the texture is BUTTON up to pi/2 and BUTTON_SELECT past it, and the row's text is hidden
//   while the button is within 40 degrees of edge-on (func_800FEE04). Rush 2's list rows get them, drawn in the 2D
//   display list just before each row's text, at Rush 2's row positions and scaled to its row spacing (2049's rows
//   are 40 units = 21.3 pixels apart).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "rush2.h"
#include "rush2_hooks.h"
#include "font2049.h"
#include "menus2049.h"
#include "rush2049_rom.h"
#include "wings.h"

#include "recompui/recompui.h"
#include "ultramodern/config.hpp"

#define F3DEX_GBI_2
#define G_SC_NON_INTERLACE 0 // gbi.h scissor mode, which the extended GBI header doesn't define.
#include "rt64_extended_gbi.h"

extern "C" void text_measure_string_800732AC(uint8_t* rdram, recomp_context* ctx);
extern "C" void text_print_string_800734E0(uint8_t* rdram, recomp_context* ctx);

namespace {
    // Rush 2.
    constexpr uint32_t game_state = 0x8010C0D0;       // s32: 0 front end, 1 track select, 2 car select, 6 post-race
    constexpr uint32_t front_end_screen = 0x800E7BC4; // s16: 0x11, 0x12 the attract demo
    constexpr uint32_t pause_state = 0x8002305C;      // s8
    constexpr uint32_t font_table = 0x800BEE88;
    constexpr uint32_t font_byte = 0x800D402C;        // u8: the font the drawer uses
    constexpr uint32_t font_height = 0x800D3E60;      // s8: byte 4 of the font's table entry
    constexpr uint32_t letter_spacing = 0x800D3E92;   // s8: the font's letter spacing (font table entry + 6)
    constexpr uint32_t style_table = 0x800BEF6C;      // 8 bytes a style: text RGBA, shadow RGBA
    constexpr uint32_t text_fg = 0x800D3E38;          // the text's RGBA (func_800735A8 layer 0)
    constexpr uint32_t text_bg = 0x800D3E3C;          // its shadow's (layer 1)
    constexpr uint32_t text_justify = 0x800BEF5C;     // s32: 0 left, 1 centered on x, 2 right
    constexpr uint32_t text_x = 0x800D3F2A;           // s16: where the next string starts
    constexpr uint32_t text_y = 0x800D3F78;
    constexpr int32_t keep_position = -0x8000;        // text_print_string's "where the last one ended"
    constexpr uint32_t frame_dt = 0x80023028;         // f32: seconds of this game frame (VIs x the VI period)
    constexpr uint32_t main_dl_cursor = 0x80022FD8;   // the frame's display list
    constexpr uint32_t dl_2d_cursor = 0x80125ABC;     // the 2D display list
    constexpr int flash_style = 0x14;
    constexpr int first_gray_style = 2, last_gray_style = 4; // Rush 2's grays: 0xA0, 0x78, 0x50 (style 1 is 0xE6)
    constexpr int16_t home_screen = 0;                // front_end_screen: Start Game
    constexpr int16_t select_player_screen = 1;
    constexpr int32_t header_bottom = 42;             // the header plates' band, y 10-42 (docs/menus.md)
    // 2049's title node (func_800D71D0, entry 11 of 0x801102B4): BUTTON_S2G1 textured BUTTON_TITLE at (0, 115, 200),
    // turned rotX(-pi/2) (0x801241C0) to face the camera; func_800FEE04 prints the title in font 13 (AERO-PLAIN16),
    // style 1, centered on the node's projection (160, 28) with vertical mode 1.
    constexpr int title_model = 2;
    constexpr float title_z = 200.0f, title_y = 115.0f;
    constexpr int title_cx = 160, title_cy = 28;       // 120 - 115 * 160 / 200
    constexpr int home_cx = 160;                       // 2049's menu lists are centered (x = 0)
    constexpr uint32_t start_game_frame_callback = 0x803C3560; // the Start Game panel and HALFBOTBAR (menu_widget_handler_font6_menu_bytes)
    // TMEM caches the game's own draws trust: the 2D image last loaded (func_80055F78 skips loading it again) and the
    // palette last loaded (func_80078190). Both are cleared after a draw here loads textures.
    constexpr uint32_t last_2d_image = 0x80119638;
    constexpr uint32_t palette_cache = 0x80111954;

    // RDRAM below 16 MB: the game heap's top is lowered to 0x80AA0000 for this (src/assets.cpp).
    constexpr uint32_t region = 0x80AA0000;
    constexpr uint32_t region_end = 0x80ACA000;
    constexpr int frame_slots = 4;                    // per-frame matrices and vertices, rotated

    constexpr int rush2_fonts[3] = { 8, 9, 10 };
    constexpr int fonts_2049[4] = { 12, 11, 10, 13 }; // AERO-PLAIN7, AERO-PLAIN10, 4BAERO-PLAIN12; AERO-PLAIN16 titles
    constexpr int title_font = 10;                    // Rush 2's header titles are printed in font 10
    constexpr int button_models = 9;                  // BUTTON_S0G1..S8G1
    constexpr uint32_t button_bytes = 128 + 512;      // a button's two matrices, then its display list
    constexpr uint32_t group_ids = 0x4D000000;        // RT64 matrix group IDs: | generation << 12 | list << 8 | row

    // 2049 constants (docs/rush2049_research/menus.md section 11).
    constexpr float pi = 3.14159265f;
    constexpr float flip_rate = 4.0f * pi;            // 0x801241B4, radians a second
    constexpr float text_hidden_min = 0.6981317f;     // 0x80124834: 40 degrees
    constexpr float text_hidden_max = 2.4434609f;     // 0x80124830: 140 degrees
    constexpr float button_z = 300.0f;                // 2049's buttons' depth
    constexpr float row_units = 40.0f;                // between 2049's rows
    constexpr float text_inset = 19.7f;               // 2049 pixels from a button's end to its text (0x37 in S3)
    constexpr float near_plane = 2.5f, far_plane = 2000.0f; // func_800A5908, both x 16 for guPerspective
    constexpr float sfx_depth = 1025.0f;              // the swirl's quads (0x801391E4)
    constexpr float sfx_speed = 0.2f;                 // texture widths a second (0x80114274/8)
    constexpr uint32_t sfx_color = 0x0000C0FF;        // their vertex color (0x8011464C)
    constexpr uint32_t text_white = 0xE6E6E6FF, shadow_white = 0x3C3C3CFF;
    constexpr uint32_t text_gray = 0xA0A0A0FF, shadow_gray = 0x282828FF;
    constexpr uint8_t pulse_yellow = 0xE0;            // 0x801146BC = E0E000FF, blended from 0x801146C0 = black

    struct Texture {
        uint32_t texels = 0, palette = 0;
        int w = 0, h = 0, siz = 0, colors = 0;        // siz: 0 four bits a texel, 1 eight
    };

    // Everything built from the ROM, laid out from `region` as it goes into RDRAM.
    struct Art {
        std::vector<uint8_t> image;
        uint32_t frame_area = 0;                      // the per-frame slots start here
        uint32_t frame_slot_size = 0;
        int frame_slot_buttons = 0;
        uint32_t empty_dl = 0, empty_string = 0, viewport = 0;
        struct FontInfo { uint32_t ranges = 0; int range_count = 0; int space = 0; int cap = 0; int half_height = 0; }
            fonts[4];                                 // Rush 2's fonts 8, 9, 10, then the title font
        Texture sfx, bg_a, bg_b, button[2], title;
        struct Model { uint32_t dl = 0; float half_width = 0; } models[button_models];
    };

    std::atomic_int option_mode = 0;                 // rush2::menus2049::Mode
    bool layout = false;                              // Style and Layout, while the menus are up (game thread)
    std::mutex art_mutex;
    std::shared_ptr<const Art> built;                 // set by the build thread
    std::shared_ptr<const rush2::rom2049::Source> built_from;
    std::atomic_bool building = false;
    std::atomic<uint64_t> source_generation = 1;
    uint64_t built_generation = 0;

    // Game thread state.
    std::shared_ptr<const Art> art;                   // the art in RDRAM
    bool active = false;
    bool fonts_swapped = false;
    std::array<std::array<uint8_t, 16>, 3> saved_fonts{};
    bool styles_swapped = false;
    std::array<uint8_t, 24> saved_styles{};
    uint32_t frame_counter = 0;
    uint32_t background_seen = 0;                     // the frame Rush 2's menu background was last drawn
    float pulse_time = 0.0f;
    int32_t sfx_u = 0, sfx_v = 0;                     // the swirl's center texture coordinates, 1/32 texel
    bool sfx_started = false;
    int last_style = 0;
    int row_list = -1;                                // the list the next text_print_string prints a row of
    int slot_buttons = 0;
    bool forced_font = false;                         // before_print changed the font for one print
    bool record_forced = false;                       // and font 10's record to the title font's
    uint8_t saved_record[16] = {};
    uint32_t plate_frame = 0;                         // the frame a header plate was last replaced
    int8_t saved_font[3] = {};                        // the font, height and letter spacing it changed

    struct Row { float angle = 0.0f; uint32_t frame = 0; uint32_t gen = 0; int model = -1; float scale = 0.0f; };
    std::map<uint32_t, Row> rows;
    struct List { float width = 0.0f; int normal_font = 0; int pitch = 16; int prev_y = keep_position; int min_dy = 0; int rows = 0; };
    std::array<List, 4> lists;                        // 0 Start Game, 1 the circuit menu, 2 Select Player, 3 SETUP

    float read_f32(uint8_t* rdram, uint32_t addr) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }

    uint16_t be16(const std::vector<uint8_t>& d, size_t o) { return (uint16_t)(d[o] << 8 | d[o + 1]); }
    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3];
    }

    // The region's image, filled from its start.
    struct Builder {
        Art& a;
        uint32_t at(size_t offset) { return region + (uint32_t)offset; }
        uint32_t add(const uint8_t* data, size_t size, size_t align = 8) {
            size_t o = (a.image.size() + align - 1) / align * align;
            a.image.resize(o + size, 0);
            if (data != nullptr) memcpy(&a.image[o], data, size);
            return at(o);
        }
        void put32(uint32_t addr, uint32_t v) {
            size_t o = addr - region;
            a.image[o] = (uint8_t)(v >> 24); a.image[o + 1] = (uint8_t)(v >> 16);
            a.image[o + 2] = (uint8_t)(v >> 8); a.image[o + 3] = (uint8_t)v;
        }
        void put16(uint32_t addr, uint16_t v) {
            size_t o = addr - region;
            a.image[o] = (uint8_t)(v >> 8); a.image[o + 1] = (uint8_t)v;
        }
    };

    // File 54's chunk table.
    struct Container {
        const std::vector<uint8_t>& d;
        uint32_t imag = 0, txhd = 0, ntx = 0, plhd = 0, npl = 0, obhd = 0, nob = 0;
        explicit Container(const std::vector<uint8_t>& data) : d(data) {
            if (d.size() < 4) return;
            for (size_t o = be32(d, 0); o + 12 <= d.size(); o += 12) {
                uint32_t v = be32(d, o + 4), n = be32(d, o + 8);
                if (memcmp(&d[o], "IMAG", 4) == 0) imag = v;
                else if (memcmp(&d[o], "TXHD", 4) == 0) { txhd = v; ntx = n; }
                else if (memcmp(&d[o], "PLHD", 4) == 0) { plhd = v; npl = n; }
                else if (memcmp(&d[o], "OBHD", 4) == 0) { obhd = v; nob = n; }
            }
        }
        // A paletted image (TXHD 0x24 bytes: name[16], u16 w, h, u8 siz, fmt, u16 palette, u32 texels from IMAG;
        // PLHD 0x18 bytes: name[16], u32 (colors - 1) << 16, u32 colors from IMAG), copied as the N64 loads it.
        bool texture(Builder& b, const char* name, Texture& t) {
            for (uint32_t i = 0; i < ntx; i++) {
                size_t r = txhd + (size_t)i * 0x24;
                if (r + 0x24 > d.size() || strncmp((const char*)&d[r], name, 16) != 0) continue;
                t.w = be16(d, r + 0x10);
                t.h = be16(d, r + 0x12);
                t.siz = d[r + 0x14];
                if (d[r + 0x15] != 2 || t.siz > 1) return false;   // CI4 or CI8
                size_t texels = imag + be32(d, r + 0x18);
                size_t bytes = (size_t)t.w * t.h * (t.siz ? 8 : 4) / 8;
                uint32_t pal = be16(d, r + 0x16);
                size_t p = plhd + (size_t)pal * 0x18;
                if (pal >= npl || p + 0x18 > d.size() || texels + bytes > d.size()) return false;
                t.colors = (int)((be32(d, p + 0x10) >> 16) & 0xFF) + 1;
                size_t colors = imag + be32(d, p + 0x14);
                if (colors + (size_t)t.colors * 2 > d.size()) return false;
                t.texels = b.add(&d[texels], bytes);
                t.palette = b.add(&d[colors], (size_t)t.colors * 2);
                return true;
            }
            return false;
        }
        // A model's first LOD (OBHD 0x58 bytes: name[16], f32 radius, u16, s16 LOD count, LODs of 16 bytes at +0x18:
        // +8 the display list's file offset). Its list goes in with G_VTX pointed at the copied vertices; the
        // othermode and combiner it sets are left out (the draw sets 2049's).
        bool model(Builder& b, const char* name, Art::Model& m) {
            for (uint32_t i = 0; i < nob; i++) {
                size_t r = obhd + (size_t)i * 0x58;
                if (r + 0x58 > d.size() || strncmp((const char*)&d[r], name, 16) != 0) continue;
                size_t dl = be32(d, r + 0x20);
                std::vector<std::pair<uint32_t, uint32_t>> cmds;
                size_t vmin = SIZE_MAX, vmax = 0;
                for (size_t o = dl;; o += 8) {
                    if (o + 8 > d.size() || cmds.size() > 4096) return false;
                    uint32_t w0 = be32(d, o), w1 = be32(d, o + 4);
                    uint8_t op = (uint8_t)(w0 >> 24);
                    if (op == 0x01) {
                        size_t n = (w0 >> 12) & 0xFF;
                        vmin = std::min(vmin, (size_t)w1);
                        vmax = std::max(vmax, (size_t)w1 + n * 16);
                    }
                    else if (op == 0xE2 || op == 0xFC) {
                        continue;
                    }
                    else if (op != 0x05 && op != 0x06 && op != 0xE7 && op != 0xDF) {
                        fprintf(stderr, "[menus2049] %s: unexpected command %08X\n", name, w0);
                        return false;
                    }
                    cmds.emplace_back(w0, w1);
                    if (op == 0xDF) break;
                }
                if (vmin >= vmax || vmax > d.size()) return false;
                uint32_t verts = b.add(&d[vmin], vmax - vmin);
                float half = 0.0f;
                for (size_t v = vmin; v < vmax; v += 16) half = std::max(half, std::fabs((float)(int16_t)be16(d, v)) / 16.0f);
                std::vector<uint8_t> list(cmds.size() * 8);
                for (size_t c = 0; c < cmds.size(); c++) {
                    uint32_t w0 = cmds[c].first, w1 = cmds[c].second;
                    if ((w0 >> 24) == 0x01) w1 = (verts - 0x80000000u) + (w1 - (uint32_t)vmin);
                    for (int k = 0; k < 4; k++) {
                        list[c * 8 + k] = (uint8_t)(w0 >> (24 - 8 * k));
                        list[c * 8 + 4 + k] = (uint8_t)(w1 >> (24 - 8 * k));
                    }
                }
                m.dl = b.add(list.data(), list.size());
                m.half_width = half;
                return true;
            }
            return false;
        }
    };

    // Rush 2 font records for 2049 font f. Its glyphs are packed once each, in character order, into 128-wide I4
    // sheets of at most 64 rows (4 KB, what the drawer's image load takes); a glyph's rectangle is inclusive and never
    // starts on row 0 (the drawer takes y1 = 0 for a missing glyph). The ranges cover 0x16-0x7E like Rush 2's own,
    // each a run of characters whose glyphs are on one sheet; lower case points at the capitals' cells (2049's menu
    // fonts have none). `dump` gets the sheets in the RUSH2_DUMP_FONT_TABLES format (src/fonts.cpp) for the
    // high-resolution font pack (tools/build_font_pack.py trace-2049-menus).
    bool build_font(Builder& b, const rush2::font2049::Font& f, Art::FontInfo& info, std::string& dump) {
        constexpr int first = 0x16, last = 0x7E, sheet_w = 128, max_rows = 64;
        struct Cell { int sheet = -1; int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
        struct Sheet { int h = 0; std::vector<uint8_t> pixels; };
        std::vector<Sheet> sheets;
        std::array<Cell, 128> cells{};
        int x = 0, y = 1, shelf = 0;
        sheets.push_back({ 0, std::vector<uint8_t>(sheet_w / 2 * max_rows, 0) });
        for (int c = first; c <= last; c++) {
            if ((c >= 'a' && c <= 'z') || c == ' ') continue;
            auto g = f.glyphs.find((char)c);
            if (g == f.glyphs.end()) continue;
            int w = g->second.x1 - g->second.x0 + 1, h = g->second.y1 - g->second.y0 + 1;
            if (x + w > sheet_w) { x = 0; y += shelf + 1; shelf = 0; }
            if (y + h > max_rows) {
                sheets.push_back({ 0, std::vector<uint8_t>(sheet_w / 2 * max_rows, 0) });
                x = 0; y = 1; shelf = 0;
            }
            Sheet& sh = sheets.back();
            const auto& page = f.pages[g->second.page];
            for (int yy = 0; yy < h; yy++) {
                for (int xx = 0; xx < w; xx++) {
                    int cov = page.coverage[(size_t)(g->second.y0 + yy) * page.w + g->second.x0 + xx];
                    int v = std::min(15, (cov + 8) / 17);
                    size_t p = (size_t)(y + yy) * sheet_w + x + xx;
                    sh.pixels[p / 2] |= (uint8_t)((p & 1) ? v : v << 4);
                }
            }
            cells[c] = { (int)sheets.size() - 1, (int16_t)x, (int16_t)y, (int16_t)(x + w - 1), (int16_t)(y + h - 1) };
            sh.h = std::max(sh.h, y + h);
            shelf = std::max(shelf, h);
            x += w + 1;
        }
        for (int c = 'a'; c <= 'z'; c++) cells[c] = cells[c - 32];

        std::vector<uint32_t> sheet_addr;
        for (Sheet& sh : sheets) {
            sh.h = std::max(sh.h, 2);
            sh.pixels.resize((size_t)sheet_w / 2 * sh.h);
            sheet_addr.push_back(b.add(sh.pixels.data(), sh.pixels.size()));
        }
        // Ranges: maximal runs of characters on one sheet (missing ones join any run).
        std::vector<uint8_t> table;
        int count = 0;
        for (int c = first; c <= last;) {
            int sheet = -1, end = c;
            for (int k = c; k <= last; k++) {
                if (cells[k].sheet >= 0) {
                    if (sheet >= 0 && cells[k].sheet != sheet) break;
                    sheet = cells[k].sheet;
                }
                end = k;
            }
            if (sheet < 0) sheet = 0;
            std::vector<uint8_t> glyphs((size_t)(end - c + 1) * 8, 0);
            for (int k = c; k <= end; k++) {
                if (cells[k].sheet < 0) continue;
                const int16_t r[4] = { cells[k].x0, cells[k].y0, cells[k].x1, cells[k].y1 };
                for (int j = 0; j < 4; j++) {
                    glyphs[(size_t)(k - c) * 8 + j * 2] = (uint8_t)(r[j] >> 8);
                    glyphs[(size_t)(k - c) * 8 + j * 2 + 1] = (uint8_t)r[j];
                }
            }
            uint32_t glyph_table = b.add(glyphs.data(), glyphs.size());
            uint8_t e[0x14] = {};
            e[0] = (uint8_t)(c >> 8); e[1] = (uint8_t)c;
            e[2] = (uint8_t)(end >> 8); e[3] = (uint8_t)end;
            e[6] = (uint8_t)(sheet_w >> 8); e[7] = (uint8_t)sheet_w;
            e[8] = (uint8_t)(sheets[sheet].h >> 8); e[9] = (uint8_t)sheets[sheet].h;
            for (int k = 0; k < 4; k++) {
                e[0xC + k] = (uint8_t)(sheet_addr[sheet] >> (24 - 8 * k));
                e[0x10 + k] = (uint8_t)(glyph_table >> (24 - 8 * k));
            }
            table.insert(table.end(), e, e + 0x14);
            count++;
            c = end + 1;
        }
        info.ranges = b.add(table.data(), table.size());
        info.range_count = count;
        // A space adds header byte 7 alone (func_800B3FA4), as Rush 2's measurer and drawer add byte 5; a glyph adds
        // its width plus the gap (byte 8, 1 in every menu font), as Rush 2 adds its font's spacing (byte 6, 1 in fonts 8-10).
        info.space = f.space;
        auto a = f.glyphs.find('A');
        info.cap = a != f.glyphs.end() ? a->second.y1 - a->second.y0 + 1 : f.line_height;
        // func_800B71D4's vertical centering: half the ascent and descent, truncated toward zero.
        info.half_height = (f.ascent + f.descent) / 2;

        // The dump: a page per sheet with the characters packed on it (not lower case, which shares the capitals').
        dump += "\"pages\":[";
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sheets.size(); i++) {
            char head[96];
            snprintf(head, sizeof(head), "%s{\"first\":0,\"last\":127,\"w\":%d,\"h\":%d,\"glyphs\":[", i ? "," : "",
                     sheet_w, sheets[i].h);
            dump += head;
            for (int c = 0; c < 128; c++) {
                bool own = cells[c].sheet == (int)i && !(c >= 'a' && c <= 'z');
                char g[48];
                snprintf(g, sizeof(g), "%s[%d,%d,%d,%d]", c ? "," : "", own ? cells[c].x0 : 0, own ? cells[c].y0 : 0,
                         own ? cells[c].x1 : 0, own ? cells[c].y1 : 0);
                dump += g;
            }
            dump += "],\"pixels\":\"";
            for (uint8_t v : sheets[i].pixels) { dump += hex[v >> 4]; dump += hex[v & 15]; }
            dump += "\"}";
        }
        dump += "]";
        return count <= 15;
    }

    std::shared_ptr<const Art> build(const rush2::rom2049::Source& rom) {
        auto out = std::make_shared<Art>();
        Builder b{ *out };
        static const uint8_t end_dl[8] = { 0xDF, 0, 0, 0, 0, 0, 0, 0 };
        out->empty_dl = b.add(end_dl, 8);
        out->empty_string = b.add(nullptr, 8);
        // Viewport over the 320 x 240 screen (vscale, vtrans in quarter pixels; G_MAXZ / 2).
        static const uint8_t vp[16] = { 0x02, 0x80, 0x01, 0xE0, 0x01, 0xFF, 0, 0, 0x02, 0x80, 0x01, 0xE0, 0x01, 0xFF, 0, 0 };
        out->viewport = b.add(vp, 16);
        std::string dump = "[\n";
        for (int i = 0; i < 4; i++) {
            auto f = rush2::font2049::load(rom, fonts_2049[i]);
            char head[64];
            if (i < 3) snprintf(head, sizeof(head), "%s{\"id\":\"rush2049_menu_%d\",", i ? ",\n" : "", rush2_fonts[i]);
            else snprintf(head, sizeof(head), ",\n{\"id\":\"rush2049_menu_title\",");
            dump += head;
            if (!f || !build_font(b, *f, out->fonts[i], dump)) {
                fprintf(stderr, "[menus2049] Rush 2049 font %d couldn't be read\n", fonts_2049[i]);
                return nullptr;
            }
            dump += "}";
        }
        // RUSH2_DUMP_MENU_FONTS=<path>: the sheets as font tables, for tools/build_font_pack.py trace-2049-menus.
        if (const char* path = getenv("RUSH2_DUMP_MENU_FONTS")) {
            if (FILE* fp = fopen(path, "w")) {
                fputs((dump + "\n]\n").c_str(), fp);
                fclose(fp);
            }
        }
        std::vector<uint8_t> file;
        if (!rom.read_file(54, file)) {
            fprintf(stderr, "[menus2049] Rush 2049 file 54 couldn't be read\n");
            return nullptr;
        }
        Container c(file);
        bool ok = c.texture(b, "BACKGROUND_SFX", out->sfx) && c.texture(b, "BACKGROUND_A", out->bg_a) &&
                  c.texture(b, "BACKGROUND_B", out->bg_b) && c.texture(b, "BUTTON", out->button[0]) &&
                  c.texture(b, "BUTTON_SELECT", out->button[1]) && c.texture(b, "BUTTON_TITLE", out->title);
        for (int k = 0; ok && k < button_models; k++) {
            char name[16];
            snprintf(name, sizeof(name), "BUTTON_S%dG1", k);
            ok = c.model(b, name, out->models[k]);
        }
        if (!ok) {
            fprintf(stderr, "[menus2049] Rush 2049's menu art couldn't be read\n");
            return nullptr;
        }
        // Per frame: the swirl's projection, modelview and 16 vertices, then for each button its projection,
        // modelview and display list.
        out->frame_area = b.add(nullptr, 0, 64);
        uint32_t left = region_end - out->frame_area;
        out->frame_slot_size = left / frame_slots / 64 * 64;
        out->frame_slot_buttons = (int)((out->frame_slot_size - 128 - 256) / button_bytes);
        if (out->frame_slot_buttons < 12) {
            fprintf(stderr, "[menus2049] No room for the menu buttons (%u bytes left)\n", left);
            return nullptr;
        }
        return out;
    }

    void start_build() {
        if (building.exchange(true)) return;
        uint64_t generation = source_generation.load();
        std::thread([generation]() {
            // The N64 ROM even with the Dreamcast disc as the source (the disc has no menu fonts in this format).
            auto rom = rush2::wings::get_rom();
            if (rom == nullptr || rom->is_dreamcast()) rom = rush2::wings::open_source(false);
            std::shared_ptr<const Art> a = rom != nullptr ? build(*rom) : nullptr;
            {
                std::lock_guard lock{ art_mutex };
                built = a;
                built_from = rom;
                built_generation = generation;
            }
            building = false;
        }).detach();
    }

    // Commands.
    struct Cmds {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        void operator()(uint32_t w0, uint32_t w1) { v.emplace_back(w0, w1); }
        void add(const GfxCommand* g, int n) { for (int i = 0; i < n; i++) v.emplace_back(g[i].values.word0, g[i].values.word1); }
    };
#define EX(macro) do { GfxCommand g_[4]{}; macro(&g_[0]); c.add(g_, 1); } while (0)

    uint32_t phys(uint32_t a) { return a - 0x80000000u; }

    void write_cmds(uint8_t* rdram, uint32_t cursor_global, const Cmds& c) {
        int32_t cursor = MEM_W(0, (int32_t)cursor_global);
        for (const auto& [w0, w1] : c.v) {
            MEM_W(0, cursor) = (int32_t)w0;
            MEM_W(4, cursor) = (int32_t)w1;
            cursor += 8;
        }
        MEM_W(0, (int32_t)cursor_global) = cursor;
    }

    // G_SETCOMBINE from (a - b) * c + d for color and alpha, the same in both cycles.
    void combine(Cmds& c, int a, int b, int cc, int d, int aa, int ab, int ac, int ad) {
        c(0xFC000000u | (uint32_t)(a & 0xF) << 20 | (uint32_t)(cc & 0x1F) << 15 | (uint32_t)(aa & 7) << 12 |
              (uint32_t)(ac & 7) << 9 | (uint32_t)(a & 0xF) << 5 | (uint32_t)(cc & 0x1F),
          (uint32_t)(b & 0xF) << 28 | (uint32_t)(b & 0xF) << 24 | (uint32_t)(aa & 7) << 21 | (uint32_t)(ac & 7) << 18 |
              (uint32_t)(d & 7) << 15 | (uint32_t)(ab & 7) << 12 | (uint32_t)(ad & 7) << 9 | (uint32_t)(d & 7) << 6 |
              (uint32_t)(ab & 7) << 3 | (uint32_t)(ad & 7));
    }
    // Combiner inputs.
    constexpr int cc_texel0 = 1, cc_prim = 3, cc_shade = 4, cc_zero_a = 15, cc_zero_c = 31, cc_zero_d = 7;
    constexpr int ac_texel0 = 1, ac_prim = 3, ac_zero = 7;
    // Other mode high bits.
    constexpr uint32_t om_persp = 0x80000, om_tlut_rgba16 = 0x8000, om_bilerp = 0x2000, om_tc_filt = 0xC00,
                       om_no_dither = 0xF0;
    // Blender: IN x IN alpha + MEM x (1 - alpha), forced; z compare and update (2049's button mode's low bits).
    constexpr uint32_t rm_blend = 0x00504240, rm_blend_z = 0x00504A70, ac_threshold = 1;

    // The texture load func_80099BFC writes for a paletted image with wrapping tiles (tools/rush2049/menu49.py
    // texload gives the same commands for BUTTON, BUTTON_SELECT and BACKGROUND_SFX).
    void load_ci(Cmds& c, const Texture& t) {
        uint32_t bits = t.siz ? 8 : 4;
        uint32_t masks = 0, maskt = 0;
        while ((1 << masks) < t.w) masks++;
        while ((1 << maskt) < t.h) maskt++;
        uint32_t tile = maskt << 14 | masks << 4;
        uint32_t row_words = (uint32_t)t.w * bits / 64;
        c(0xFD500000, phys(t.texels));
        c(0xF5500000, 0x07000000 | tile);
        c(0xE6000000, 0);
        c(0xF3000000, 0x07000000 | ((uint32_t)t.w * t.h * bits / 16 - 1) << 12 | (2048 + row_words - 1) / row_words);
        c(0xE7000000, 0);
        c(0xF5400000 | (uint32_t)t.siz << 19 | row_words << 9, tile);
        c(0xF2000000, (uint32_t)(t.w - 1) << 14 | (uint32_t)(t.h - 1) << 2);
        c(0xFD100000, phys(t.palette));
        c(0xE8000000, 0);
        c(0xF5000100, 0x07000000);
        c(0xE6000000, 0);
        c(0xF0000000, 0x07000000 | (uint32_t)(t.colors - 1) << 14);
        c(0xE7000000, 0);
    }

    // Matrices, rows as the N64 multiplies them (v' = v M), written in its s15.16 layout.
    using M4 = std::array<std::array<float, 4>, 4>;
    M4 mul(const M4& a, const M4& b) {
        M4 r{};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                for (int k = 0; k < 4; k++) r[i][j] += a[i][k] * b[k][j];
        return r;
    }
    void write_mtx(uint8_t* rdram, uint32_t addr, const M4& m) {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                int32_t f = (int32_t)std::lround(m[i][j] * 65536.0f);
                MEM_H((i * 4 + j) * 2, (int32_t)addr) = (int16_t)(f >> 16);
                MEM_H(32 + (i * 4 + j) * 2, (int32_t)addr) = (int16_t)(f & 0xFFFF);
            }
        }
    }
    // 2049's menu projection (func_800A5744 with a 90 degree horizontal field of view at 320 x 240, near 2.5 and far
    // 2000 times 16, guPerspective), its camera looking down +z with x right and y up.
    M4 projection() {
        float fovy = 2.0f * std::atan(0.75f);
        float f = 1.0f / std::tan(fovy / 2.0f), n = near_plane * 16.0f, fa = far_plane * 16.0f;
        M4 p{};
        p[0][0] = f / (4.0f / 3.0f);
        p[1][1] = f;
        p[2][2] = (n + fa) / (n - fa);
        p[2][3] = -1.0f;
        p[3][2] = 2.0f * n * fa / (n - fa);
        M4 flip{};
        flip[0][0] = 1.0f; flip[1][1] = 1.0f; flip[2][2] = -1.0f; flip[3][3] = 1.0f;
        return mul(flip, p);
    }

    // The width of the 3D image in 4:3 pixels (320 at 4:3), as RT64 widens projections for the aspect ratio.
    float wide_screen_width() {
        float aspect = 4.0f / 3.0f;
        switch (ultramodern::renderer::get_graphics_config().ar_option) {
            case ultramodern::renderer::AspectRatio::Manual:
                aspect = 16.0f / 9.0f;
                break;
            case ultramodern::renderer::AspectRatio::Expand: {
                int width = 0, height = 0;
                recompui::get_window_size(width, height);
                if (width > 0 && height > 0) aspect = std::max(aspect, (float)width / (float)height);
                break;
            }
            default:
                break;
        }
        return 320.0f * aspect / (4.0f / 3.0f);
    }

    uint32_t slot_base() {
        return art->frame_area + (uint32_t)(frame_counter % frame_slots) * art->frame_slot_size;
    }

    void put_vtx(uint8_t* rdram, uint32_t addr, float x, float y, float z, int32_t s, int32_t t, uint32_t rgba) {
        MEM_H(0, (int32_t)addr) = (int16_t)std::lround(x * 16.0f);
        MEM_H(2, (int32_t)addr) = (int16_t)std::lround(y * 16.0f);
        MEM_H(4, (int32_t)addr) = (int16_t)std::lround(z * 16.0f);
        MEM_H(6, (int32_t)addr) = 0;
        MEM_H(8, (int32_t)addr) = (int16_t)std::clamp<int32_t>(s, -32768, 32767);
        MEM_H(10, (int32_t)addr) = (int16_t)std::clamp<int32_t>(t, -32768, 32767);
        MEM_W(12, (int32_t)addr) = (int32_t)rgba;
    }

    // The swirl and the circuit board, at the start of the frame's display list.
    void draw_background(uint8_t* rdram) {
        uint32_t slot = slot_base();
        uint32_t proj = slot, view = slot + 64, verts = slot + 128;
        write_mtx(rdram, proj, projection());
        M4 identity{};
        for (int i = 0; i < 4; i++) identity[i][i] = 1.0f;
        write_mtx(rdram, view, identity);

        // The four quads of func_800B8374 (0x80114264 + i * 0x38: x from a to b, y from c to d, at z = 1025), their
        // vertex 0 at the center with texture coordinate (u, v) and the far corner one texture away, widened with
        // the projection past the 4:3 edges.
        float wide = std::max(1.0f, wide_screen_width() / 320.0f);
        const int32_t tile = art->sfx.w << 5, tile_t = art->sfx.h << 5;
        static const float quads[4][4] = { { -1, 0, -1, 0 }, { 1, 0, -1, 0 }, { 1, 0, 1, 0 }, { -1, 0, 1, 0 } };
        for (int q = 0; q < 4; q++) {
            float a = quads[q][0] * sfx_depth * wide, b = quads[q][1], c = quads[q][2] * sfx_depth, d = quads[q][3];
            int32_t far_u = sfx_u - (int32_t)std::lround(tile * wide), far_v = sfx_v - tile_t;
            uint32_t v = verts + q * 64;
            put_vtx(rdram, v + 0, b, d, sfx_depth, sfx_u, sfx_v, sfx_color);
            put_vtx(rdram, v + 16, a, d, sfx_depth, far_u, sfx_v, sfx_color);
            put_vtx(rdram, v + 32, a, c, sfx_depth, far_u, far_v, sfx_color);
            put_vtx(rdram, v + 48, b, c, sfx_depth, sfx_u, far_v, sfx_color);
        }

        Cmds c;
        EX(gEXEnable);
        EX(gEXPushOtherMode);
        EX(gEXPushCombineMode);
        EX(gEXPushGeometryMode);
        EX(gEXPushProjectionMatrix);
        EX(gEXPushViewport);
        EX(gEXPushScissor);
        EX(gEXPushPrimColor);
        EX(gEXPushBlendColor);
        {
            GfxCommand g[2]{};
            gEXSetScissor(&g[0], G_SC_NON_INTERLACE, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, 0, 240);
            c.add(g, 2);
        }
        c(0xE7000000, 0);
        c(0xDC080008, phys(art->viewport));
        c(0xDA380007, phys(proj));                    // G_MTX projection, load
        c(0xDA380002, phys(view));                    // G_MTX modelview, load, push
        c(0xD9000000, 0x00200004);                    // geometry: shade, smooth
        c(0xD7000002, 0xFFFFFFFF);                    // G_TEXTURE on, scale 1
        c(0xEF000000 | om_persp | om_tlut_rgba16 | om_bilerp | om_tc_filt | om_no_dither, rm_blend);
        c(0xFA000000, sfx_color);
        // 2049's SFX polygons: TEXEL0 x SHADE, alpha TEXEL0 x PRIM (FC1217FF FFFFFE38 in two cycles, fog 0 here).
        combine(c, cc_texel0, cc_zero_a, cc_shade, cc_zero_d, ac_texel0, ac_zero, ac_prim, ac_zero);
        load_ci(c, art->sfx);
        c(0x01000000 | 16 << 12 | 16 << 1, phys(verts));
        for (int q = 0; q < 4; q++) {
            uint32_t i0 = q * 4, i1 = i0 + 1, i2 = i0 + 2, i3 = i0 + 3;
            c(0x06000000 | (i0 * 2) << 16 | (i2 * 2) << 8 | i1 * 2, (i0 * 2) << 16 | (i2 * 2) << 8 | i3 * 2);
        }
        c(0xE7000000, 0);
        c(0xD8380002, 64);                            // G_POPMTX

        // The circuit board at (0, 0): 2049 draws it in copy mode, a texel a pixel with alpha compare, rows bottom
        // up (its images are stored flipped; tools/rush2049/menu49.py widget). Here point sampled in one cycle.
        c(0xEF000000 | om_tlut_rgba16 | om_tc_filt | om_no_dither, rm_blend | ac_threshold);
        c(0xF9000000, 0x00000080);
        combine(c, cc_zero_a, cc_zero_a, cc_zero_c, cc_texel0, ac_zero, ac_zero, ac_zero, ac_texel0);
        c(0xFD100000, phys(art->bg_a.palette));
        c(0xE8000000, 0);
        c(0xF5000100, 0x07000000);
        c(0xE6000000, 0);
        c(0xF0000000, 0x07000000 | (uint32_t)(art->bg_a.colors - 1) << 14);
        c(0xE7000000, 0);
        // Past the 4:3 edges (widescreen) the board goes on mirrored about each edge, so its traces run on unbroken,
        // as Rush 2's own background is extended (src/widescreen.cpp): each strip is drawn again flipped
        // horizontally, moved out with the extended GBI's rectangle offsets.
        bool widescreen = wide_screen_width() > 320.5f;
        auto align = [&](int dx) {
            GfxCommand g[2]{};
            gEXSetRectAlign(&g[0], G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, dx * 4, 0, dx * 4, 0);
            c.add(g, 2);
        };
        int bx = 0;
        for (const Texture* t : { &art->bg_a, &art->bg_b }) {
            int per = 2048 / t->w;                    // rows a load: 2 KB of TMEM beside the palette
            uint32_t row_words = (uint32_t)t->w / 8;
            for (int y = 0; y < t->h; y += per) {
                int n = std::min(per, t->h - y);
                int stored = t->h - y - n;            // the strip's first row in the (flipped) texels
                c(0xFD500000, phys(t->texels + (uint32_t)(stored * t->w)));
                c(0xF5500000, 0x07080200);            // load tile, clamped
                c(0xE6000000, 0);
                c(0xF3000000, 0x07000000 | ((uint32_t)t->w * n / 2 - 1) << 12 | (2048 + row_words - 1) / row_words);
                c(0xE7000000, 0);
                c(0xF5480000 | row_words << 9, 0x00080200);
                c(0xF2000000, (uint32_t)(t->w - 1) << 14 | (uint32_t)(n - 1) << 2);
                // rect(top, s start, t start, dsdx, dtdy) for the strip's n rows from screen row top, at the align
                // offset in force.
                auto rect = [&](int top, uint32_t s0, uint32_t t0, uint32_t steps) {
                    uint32_t x0 = (uint32_t)bx * 4, x1 = (uint32_t)(bx + t->w) * 4;
                    uint32_t y0 = (uint32_t)top * 4, y1 = (uint32_t)(top + n) * 4;
                    c(0xE4000000 | x1 << 12 | y1, x0 << 12 | y0);
                    c(0xE1000000, s0 << 16 | (t0 & 0xFFFF));
                    c(0xF1000000, steps);
                };
                const uint32_t s_right = (uint32_t)(t->w * 32);
                // The board's own place: s from 0 rightward, t from the strip's end going up (its rows are flipped).
                rect(y, 0, (uint32_t)(n * 32), 0x0400FC00);   // dsdx 1, dtdy -1
                // Below its last row (the board is 220 rows, the screen 240) it goes on mirrored about that edge:
                // screen row k of the mirrored strip shows the row the strip's row n - 1 - k shows, t from 1 counting
                // down.
                int mirror_top = 2 * t->h - y - n;
                bool below = mirror_top < 240;
                if (below) rect(mirror_top, 0, 32, 0x04000400);   // dsdx 1, dtdy 1
                if (widescreen) {
                    // Mirrored about x = 0 and x = 320: s from the strip's right edge leftward.
                    for (int dx : { -(2 * bx + t->w), 640 - 2 * bx - t->w }) {
                        align(dx);
                        rect(y, s_right, (uint32_t)(n * 32), 0xFC00FC00);   // dsdx -1, dtdy -1
                        if (below) rect(mirror_top, s_right, 32, 0xFC000400);   // dsdx -1, dtdy 1
                    }
                    align(0);
                }
            }
            bx += t->w;
        }
        c(0xE7000000, 0);
        EX(gEXPopBlendColor);
        EX(gEXPopPrimColor);
        EX(gEXPopScissor);
        EX(gEXPopViewport);
        EX(gEXPopProjectionMatrix);
        EX(gEXPopGeometryMode);
        EX(gEXPopCombineMode);
        EX(gEXPopOtherMode);
        write_cmds(rdram, main_dl_cursor, c);
        MEM_W(0, (int32_t)last_2d_image) = 0;
        MEM_W(0, (int32_t)palette_cache) = 0;
    }

    // A 2049 button at row center (cx, cy), at `scale` of 2049's size, turned by angle: its matrices and display list
    // at m (a button slot), the matrices in RT64 group `id` so frame interpolation pairs the same row's button.
    void draw_button(uint8_t* rdram, uint32_t m, uint32_t id, int model, float cx, float cy, float scale, float angle,
                     float z = button_z, const Texture* texture = nullptr) {
        // 2049's node: rotX(angle - pi/2) (func_800B5898: y' = y cos - z sin, z' = y sin + z cos), at the point
        // of z = 300 that its camera sees at (cx, cy).
        float r = angle - pi / 2.0f, cs = std::cos(r), sn = std::sin(r);
        float px = (cx - 160.0f) / 160.0f * z, py = (120.0f - cy) / 160.0f * z;
        M4 view{};
        view[0][0] = 1.0f;
        view[1][1] = cs; view[1][2] = sn;
        view[2][1] = -sn; view[2][2] = cs;
        view[3][0] = px * 16.0f; view[3][1] = py * 16.0f; view[3][2] = z * 16.0f; view[3][3] = 1.0f;
        // Then scaled about (cx, cy) on the screen, its depth kept in front of the 3D views' (squeezed to the near
        // end of the depth range) while ordering its own faces.
        float ncx = (cx - 160.0f) / 160.0f, ncy = (120.0f - cy) / 120.0f;
        M4 post{};
        post[0][0] = scale; post[1][1] = scale;
        post[2][2] = 0.01f;
        post[3][0] = (1.0f - scale) * ncx; post[3][1] = (1.0f - scale) * ncy; post[3][2] = -0.985f; post[3][3] = 1.0f;
        write_mtx(rdram, m, mul(projection(), post));
        write_mtx(rdram, m + 64, view);

        Cmds c;
        EX(gEXEnable);
        EX(gEXPushOtherMode);
        EX(gEXPushCombineMode);
        EX(gEXPushGeometryMode);
        EX(gEXPushProjectionMatrix);
        EX(gEXPushViewport);
        {
            GfxCommand g[4]{};
            gEXMatrixGroupSimple(&g[0], id, G_EX_PUSH, 1 /* projection */,
                G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_SKIP,
                G_EX_COMPONENT_SKIP, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
            gEXMatrixGroup(&g[2], id, G_EX_INTERPOLATE_DECOMPOSE, G_EX_PUSH, 0 /* modelview */,
                G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,
                G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
            c.add(g, 4);
        }
        c(0xE7000000, 0);
        c(0xDC080008, phys(art->viewport));
        c(0xDA380007, phys(m));
        c(0xDA380002, phys(m + 64));
        c(0xD9000000, 0x00200005);                    // geometry: z buffer, shade, smooth
        c(0xD7000002, 0xFFFFFFFF);
        c(0xEF000000 | om_persp | om_tlut_rgba16 | om_bilerp | om_tc_filt | om_no_dither, rm_blend_z);
        // 2049's button (FC127E03 FF0FF3FF): TEXEL0 x SHADE x PRIM, alpha TEXEL0 x PRIM, PRIM the node's white.
        combine(c, cc_texel0, cc_zero_a, cc_shade, cc_zero_d, ac_zero, ac_zero, ac_zero, ac_texel0);
        load_ci(c, texture != nullptr ? *texture : art->button[angle > pi / 2.0f ? 1 : 0]);
        c(0xDE000000, phys(art->models[model].dl));
        c(0xE7000000, 0);
        c(0xD8380002, 64);
        {
            GfxCommand g[2]{};
            gEXPopMatrixGroup(&g[0], 0 /* modelview */);
            gEXPopMatrixGroup(&g[1], 1 /* projection */);
            c.add(g, 2);
        }
        EX(gEXPopViewport);
        EX(gEXPopProjectionMatrix);
        EX(gEXPopGeometryMode);
        EX(gEXPopCombineMode);
        EX(gEXPopOtherMode);
        c(0xDF000000, 0);                             // G_ENDDL
        if (c.v.size() * 8 > button_bytes - 128) return;
        uint32_t dl = m + 128;
        for (const auto& [w0, w1] : c.v) {
            MEM_W(0, (int32_t)dl) = (int32_t)w0;
            MEM_W(4, (int32_t)dl) = (int32_t)w1;
            dl += 8;
        }
    }

    // The rows' buttons are drawn once the frame's 2D display list is complete (finish_buttons), when every row of
    // each list is known, so a list's first frame is laid out like the rest: a row leaves a call to its button's
    // display list in the 2D list, at its place between the texts, filled in then.
    struct Pending { int list; uint32_t key; float cx, cy, angle; uint32_t slot; };
    std::vector<Pending> pending;

    // A button slot called from the 2D display list here (its display list empty until it is drawn), or 0.
    uint32_t place_slot(uint8_t* rdram) {
        int32_t cursor = MEM_W(0, (int32_t)dl_2d_cursor);
        if (slot_buttons >= art->frame_slot_buttons || cursor == 0) return 0;
        uint32_t m = slot_base() + 128 + 256 + (uint32_t)slot_buttons * button_bytes;
        slot_buttons++;
        MEM_W(0, (int32_t)(m + 128)) = (int32_t)0xDF000000;
        MEM_W(4, (int32_t)(m + 128)) = 0;
        MEM_W(0, cursor) = (int32_t)0xDE000000;                // G_DL
        MEM_W(4, cursor) = (int32_t)phys(m + 128);
        MEM_W(0, (int32_t)dl_2d_cursor) = cursor + 8;
        // The game's next image and palette loads must not be skipped: the button loads its own between them.
        MEM_W(0, (int32_t)last_2d_image) = 0;
        MEM_W(0, (int32_t)palette_cache) = 0;
        return m;
    }

    void queue_button(uint8_t* rdram, int list, uint32_t key, float cx, float cy, float angle) {
        if (uint32_t m = place_slot(rdram)) pending.push_back({ list, key, cx, cy, angle, m });
    }

    // Style and Layout: a row of the home screen (Start Game), printed once its list's buttons are laid out
    // (list_end), left-aligned at 2049's inset from its button's end.
    struct RowText { uint32_t str; int32_t y; uint32_t fg, bg; int font; };
    std::vector<RowText> row_texts;

    void set_colors(uint8_t* rdram, uint32_t fg, uint32_t bg) {
        for (int k = 0; k < 4; k++) {
            MEM_B(k, (int32_t)text_fg) = (int8_t)(fg >> (24 - 8 * k));
            MEM_B(k, (int32_t)text_bg) = (int8_t)(bg >> (24 - 8 * k));
        }
    }

    // 2049's style 0x16: black blended to E0E000 by f, a triangle wave of the time (func_800947F0: f = frac(2 t),
    // folded at 0.5, doubled), converted with truncation; the shadow stays the last style's (style 1's in its menus).
    void set_pulse(uint8_t* rdram) {
        float f = 2.0f * pulse_time;
        f -= (float)(int32_t)f;
        if (f > 0.5f) f = 1.0f - f;
        f *= 2.0f;
        uint32_t y = (uint32_t)(int32_t)((float)pulse_yellow * f);
        set_colors(rdram, y << 24 | y << 16 | 0xFF, shadow_white);
    }

    bool menus_up(uint8_t* rdram) {
        int32_t state = MEM_W(0, (int32_t)game_state);
        if (state != 0 && state != 1 && state != 2 && state != 6) return false;
        if (MEM_B(0, (int32_t)pause_state) != 0) return false;
        int16_t screen = MEM_H(0, (int32_t)front_end_screen);
        return !(state == 0 && (screen == 0x11 || screen == 0x12));
    }

    void upload(uint8_t* rdram) {
        for (size_t i = 0; i < art->image.size(); i++) {
            MEM_B((int32_t)i, (int32_t)region) = (int8_t)art->image[i];
        }
    }

    void swap_fonts(uint8_t* rdram, bool on) {
        if (on == fonts_swapped) return;
        for (int i = 0; i < 3; i++) {
            int32_t rec = (int32_t)(font_table + rush2_fonts[i] * 0x10);
            if (on) {
                for (int k = 0; k < 16; k++) saved_fonts[i][k] = (uint8_t)MEM_B(k, rec);
                const Art::FontInfo& f = art->fonts[i];
                MEM_B(5, rec) = (int8_t)f.space;
                MEM_B(9, rec) = (int8_t)f.range_count;
                MEM_W(0xC, rec) = (int32_t)f.ranges;
            }
            else {
                for (int k = 0; k < 16; k++) MEM_B(k, rec) = (int8_t)saved_fonts[i][k];
            }
        }
        fonts_swapped = on;
    }

    // Rush 2's green styles 9-11 (titles, labels) as 2049's white and gray.
    void swap_styles(uint8_t* rdram, bool on) {
        if (on == styles_swapped) return;
        int32_t first = (int32_t)(style_table + 9 * 8);
        if (on) {
            for (int k = 0; k < 24; k++) saved_styles[k] = (uint8_t)MEM_B(k, first);
            const uint32_t colors[3][2] = { { text_white, shadow_white }, { text_white, shadow_white }, { text_gray, shadow_gray } };
            for (int s = 0; s < 3; s++) {
                MEM_W(s * 8, first) = (int32_t)colors[s][0];
                MEM_W(s * 8 + 4, first) = (int32_t)colors[s][1];
            }
        }
        else {
            for (int k = 0; k < 24; k++) MEM_B(k, first) = (int8_t)saved_styles[k];
        }
        styles_swapped = on;
    }

    int32_t measure(uint8_t* rdram, recomp_context* ctx, int32_t str) {
        recomp_context saved = *ctx;
        ctx->r4 = str;
        ctx->r5 = -1;
        text_measure_string_800732AC(rdram, ctx);
        int32_t w = (int32_t)ctx->r2;
        *ctx = saved;
        return w;
    }

    // A string's width in Rush 2 font `font` (its number and letter spacing set for the measure, then put back).
    int32_t measure_in(uint8_t* rdram, recomp_context* ctx, int32_t str, int font) {
        int8_t saved[3] = { MEM_B(0, (int32_t)font_byte), MEM_B(0, (int32_t)font_height), MEM_B(0, (int32_t)letter_spacing) };
        MEM_B(0, (int32_t)font_byte) = (int8_t)font;
        MEM_B(0, (int32_t)font_height) = MEM_B(font * 16 + 4, (int32_t)font_table);
        MEM_B(0, (int32_t)letter_spacing) = MEM_B(font * 16 + 6, (int32_t)font_table);
        int32_t w = measure(rdram, ctx, str);
        MEM_B(0, (int32_t)font_byte) = saved[0];
        MEM_B(0, (int32_t)font_height) = saved[1];
        MEM_B(0, (int32_t)letter_spacing) = saved[2];
        return w;
    }

    bool is_home_screen(uint8_t* rdram) {
        return MEM_W(0, (int32_t)game_state) == 0 && MEM_H(0, (int32_t)front_end_screen) == home_screen;
    }

    // Puts back the font before_print changed for the row under the cursor, once that row is printed: at the next
    // print, the next font select (text_select_font_80088C24 entry) or the next frame.
    void restore_font(uint8_t* rdram) {
        if (record_forced) {
            record_forced = false;
            int32_t rec = (int32_t)(font_table + title_font * 0x10);
            for (int k = 0; k < 16; k++) MEM_B(k, rec) = (int8_t)saved_record[k];
        }
        if (!forced_font) return;
        forced_font = false;
        MEM_B(0, (int32_t)font_byte) = saved_font[0];
        MEM_B(0, (int32_t)font_height) = saved_font[1];
        MEM_B(0, (int32_t)letter_spacing) = saved_font[2];
    }
}

void rush2::menus2049::set_option(Mode mode) {
    option_mode = (int)mode;
}

void rush2::menus2049::source_changed() {
    source_generation++;
}

uint32_t rush2::menus2049::model_list(uint8_t* rdram, uint32_t dl, bool menu_background) {
    if (!menu_background || !active) return dl;
    background_seen = frame_counter;
    return art->empty_dl;
}

void rush2::menus2049::finish_buttons(uint8_t* rdram) {
    if (pending.empty() || art == nullptr) return;
    // Each list's layout from this frame's rows: 2049's row spacing (40 units at z = 300) scaled to the list's, its
    // buttons the narrowest model that holds the widest row with 2049's inset at each end.
    for (List& l : lists) {
        if (l.min_dy > 0) l.pitch = l.min_dy;
    }
    for (const Pending& p : pending) {
        const List& l = lists[p.list];
        float scale = (float)l.pitch / (row_units * 160.0f / button_z);
        float need = (l.width / 2.0f / scale + text_inset) * button_z / 160.0f;
        int model = button_models - 1;
        for (int k = 0; k < button_models; k++) {
            if (art->models[k].half_width >= need) { model = k; break; }
        }
        Row& r = rows[p.key];
        // A new layout (another screen in the same list) isn't interpolated from the old one.
        if (r.model != model || std::abs(r.scale - scale) > 0.001f) r.gen++;
        r.model = model;
        r.scale = scale;
        uint32_t id = group_ids | (r.gen & 0xFFF) << 12 | (p.key & 0xFFF);
        draw_button(rdram, p.slot, id, model, p.cx, p.cy, scale, p.angle);
    }
    pending.clear();
}

void rush2::menus2049::before_print(uint8_t* rdram, recomp_context* ctx) {
    if (!active) return;
    restore_font(rdram);
    int list = row_list;
    row_list = -1;
    int font = MEM_BU(0, (int32_t)font_byte);
    if (MEM_BU(0, (int32_t)ctx->r6) == 0) return;

    // Style and Layout: the header's title (font 10 in the plates' band, on a frame whose plates were replaced)
    // goes on 2049's title button, in 2049's title font, as func_800FEE04 places it.
    if (list < 0 && layout && font == title_font && plate_frame != 0 && frame_counter - plate_frame <= 1) {
        int32_t y = (int16_t)ctx->r5;
        if (y == keep_position) y = MEM_H(0, (int32_t)text_y);
        if (y >= 0 && y < header_bottom) {
            int32_t rec = (int32_t)(font_table + title_font * 0x10);
            for (int k = 0; k < 16; k++) saved_record[k] = (uint8_t)MEM_B(k, rec);
            record_forced = true;
            const Art::FontInfo& t = art->fonts[3];
            MEM_B(5, rec) = (int8_t)t.space;
            MEM_B(9, rec) = (int8_t)t.range_count;
            MEM_W(0xC, rec) = (int32_t)t.ranges;
            int32_t width = measure(rdram, ctx, (int32_t)ctx->r6);
            int32_t x = title_cx - (int32_t)((uint32_t)width >> 1);
            int justify = MEM_W(0, (int32_t)text_justify);
            ctx->r4 = justify == 1 ? x + width / 2 : justify == 2 ? x + width : x;
            ctx->r5 = title_cy - t.half_height;
            set_colors(rdram, text_white, shadow_white);
        }
        return;
    }
    if (list < 0 || (font != 8 && font != 9)) return;

    // The row's center as the game places it, in the font it chose.
    int32_t str = (int32_t)ctx->r6;
    int32_t x = (int16_t)ctx->r4, y = (int16_t)ctx->r5;
    int justify = MEM_W(0, (int32_t)text_justify);
    int32_t width = measure(rdram, ctx, str);
    if (x == keep_position) x = MEM_H(0, (int32_t)text_x);
    else if (justify == 1) x -= width / 2;
    else if (justify == 2) x -= width;
    if (y == keep_position) y = MEM_H(0, (int32_t)text_y);
    float cx = (float)x + (float)width / 2.0f, cy = (float)y + (float)art->fonts[font == 8 ? 0 : 1].cap / 2.0f;

    // Rush 2 prints the row under the cursor in the other menu font; Rush 2049 (func_800FEE04) only changes the
    // style, so that row is printed in the font of the list's other rows, for this one print (restore_font), about
    // the same center.
    List& l = lists[list];
    bool selected = last_style == flash_style;
    // A row too wide for 2049's widest button (at the list's row spacing) keeps Rush 2's look: no button.
    {
        int32_t own = selected && l.normal_font != 0 && l.normal_font != font ? measure_in(rdram, ctx, str, l.normal_font)
                                                                              : width;
        float scale = (float)l.pitch / (row_units * 160.0f / button_z);
        if (((float)own / 2.0f / scale + text_inset) * button_z / 160.0f > art->models[button_models - 1].half_width) {
            return;
        }
    }
    if (!selected) l.normal_font = font;
    else if (l.normal_font != 0 && l.normal_font != font) {
        forced_font = true;
        saved_font[0] = MEM_B(0, (int32_t)font_byte);
        saved_font[1] = MEM_B(0, (int32_t)font_height);
        saved_font[2] = MEM_B(0, (int32_t)letter_spacing);
        font = l.normal_font;
        MEM_B(0, (int32_t)font_byte) = (int8_t)font;
        MEM_B(0, (int32_t)font_height) = MEM_B(font * 16 + 4, (int32_t)font_table);
        MEM_B(0, (int32_t)letter_spacing) = MEM_B(font * 16 + 6, (int32_t)font_table);
        width = measure(rdram, ctx, str);
        x = (int32_t)std::lround(cx - (float)width / 2.0f);
        y = (int32_t)std::lround(cy - (float)art->fonts[font == 8 ? 0 : 1].cap / 2.0f);
        ctx->r4 = justify == 1 ? x + width / 2 : justify == 2 ? x + width : x;
        ctx->r5 = y;
    }

    if (l.prev_y != keep_position) {
        int dy = std::abs(y - l.prev_y);
        if (dy >= 6 && (l.min_dy == 0 || dy < l.min_dy)) l.min_dy = dy;
    }
    l.prev_y = y;
    l.width = std::max(l.width, (float)width);

    uint32_t key = (uint32_t)list << 8 | (uint32_t)l.rows++;
    Row& r = rows[key];
    if (r.frame + 1 < frame_counter) {
        r.angle = selected ? pi : 0.0f;               // a row that wasn't up last frame starts settled
        r.gen++;                                      // and isn't paired with its old button by interpolation
    }
    else if (r.frame != frame_counter) {
        // func_800D6914: toward pi under the cursor, toward 0 elsewhere.
        float step = flip_rate * read_f32(rdram, frame_dt);
        r.angle = selected ? std::min(pi, r.angle + step) : std::max(0.0f, r.angle - step);
    }
    r.frame = frame_counter;

    // Style and Layout on the home screen: the list centered like 2049's, its rows (in the list's own font, as with
    // Style) printed at the end of the frame's widgets once the list's widest row is known (list_end).
    bool home = layout && list == 0 && is_home_screen(rdram);
    if (home) cx = (float)home_cx;
    queue_button(rdram, list, key, cx, cy, r.angle);

    // The text: style 1 while the blue side shows, the pulse once the yellow side does, none near edge-on. A row the
    // game greys out (Rush 2's gray styles 2-4: the circuit menu's ENTER CODE or CONTINUE CIRCUIT when they can't be
    // picked, func_803B6C18) stays gray in 2049's gray; 2049's own lists have no disabled rows.
    if (r.angle > pi / 2.0f) set_pulse(rdram);
    else if (last_style >= first_gray_style && last_style <= last_gray_style) set_colors(rdram, text_gray, shadow_gray);
    else set_colors(rdram, text_white, shadow_white);
    bool hidden = r.angle > text_hidden_min && r.angle < text_hidden_max;
    if (home) {
        if (!hidden) {
            int32_t top = (int32_t)std::lround(cy - (float)art->fonts[font == 8 ? 0 : 1].cap / 2.0f);
            uint32_t fg = 0, bg = 0;
            for (int k = 0; k < 4; k++) {
                fg = fg << 8 | MEM_BU(k, (int32_t)text_fg);
                bg = bg << 8 | MEM_BU(k, (int32_t)text_bg);
            }
            row_texts.push_back({ (uint32_t)str, top, fg, bg, font });
        }
        ctx->r6 = (int32_t)art->empty_string;
    }
    else if (hidden) {
        ctx->r6 = (int32_t)art->empty_string;
    }
}

bool rush2::menus2049::skip_widget(uint8_t* rdram, const char* name, uint32_t callback, bool hidden) {
    if (!active || !layout || hidden) return false;
    static const char* const left_plates[] = { "STARTTITLELEFT", "TITLELEFT", "PAKSCRTITLEL", "CRSCRTITLEL", "AUDIOTITLEL" };
    static const char* const right_plates[] = { "STARTTITLERIGHT", "TITLERIGHT", "PAKSCRTITLER", "CRSCRTITLER", "AUDIOTITLER" };
    bool select_player = MEM_W(0, (int32_t)game_state) == 0 && MEM_H(0, (int32_t)front_end_screen) == select_player_screen;
    if (!select_player) {
        for (const char* n : left_plates) {
            if (strcmp(name, n) != 0) continue;
            // 2049's title button in place of the header's left plate.
            if (uint32_t m = place_slot(rdram)) {
                draw_button(rdram, m, group_ids | 0xFFF, title_model, (float)title_cx, (float)title_cy, 1.0f, 0.0f,
                            title_z, &art->title);
            }
            plate_frame = frame_counter;
            return true;
        }
        for (const char* n : right_plates) {
            if (strcmp(name, n) == 0) return true;
        }
    }
    // The home screen's panel and bottom bar: 2049's home screen is its buttons on the background.
    return callback == start_game_frame_callback && is_home_screen(rdram);
}

void rush2::menus2049::list_end(uint8_t* rdram, recomp_context* ctx) {
    if (!active) {
        row_texts.clear();
        return;
    }
    finish_buttons(rdram);
    if (row_texts.empty()) return;
    // The last row's one-print font change (the selected row's, when it is the list's last) is settled first: put
    // back by the first print below, it would print that row in the selected row's larger font.
    restore_font(rdram);
    // The home screen's rows, left-aligned 2049's inset in from their buttons' left end (func_800FEE04 prints at the
    // button's center less 0x37 on its S3 buttons: 19.7 pixels in, at 2049's size).
    const List& l = lists[0];
    float scale = (float)l.pitch / (row_units * 160.0f / button_z);
    float half = 0.0f;
    for (const auto& [key, r] : rows) {
        if ((key >> 8) == 0 && r.frame == frame_counter && r.model >= 0) {
            half = art->models[r.model].half_width * 160.0f / button_z * scale;
            break;
        }
    }
    int32_t x = (int32_t)std::lround((float)home_cx - half + text_inset * scale);
    recomp_context saved = *ctx;
    int8_t font_state[3] = { MEM_B(0, (int32_t)font_byte), MEM_B(0, (int32_t)font_height), MEM_B(0, (int32_t)letter_spacing) };
    int32_t justify = MEM_W(0, (int32_t)text_justify);
    uint32_t colors[2] = { (uint32_t)MEM_W(0, (int32_t)text_fg), (uint32_t)MEM_W(0, (int32_t)text_bg) };
    MEM_W(0, (int32_t)text_justify) = 0;
    for (const RowText& t : row_texts) {
        MEM_B(0, (int32_t)font_byte) = (int8_t)t.font;
        MEM_B(0, (int32_t)font_height) = MEM_B(t.font * 16 + 4, (int32_t)font_table);
        MEM_B(0, (int32_t)letter_spacing) = MEM_B(t.font * 16 + 6, (int32_t)font_table);
        set_colors(rdram, t.fg, t.bg);
        ctx->r4 = x;
        ctx->r5 = t.y;
        ctx->r6 = (int32_t)t.str;
        text_print_string_800734E0(rdram, ctx);
    }
    row_texts.clear();
    MEM_B(0, (int32_t)font_byte) = font_state[0];
    MEM_B(0, (int32_t)font_height) = font_state[1];
    MEM_B(0, (int32_t)letter_spacing) = font_state[2];
    MEM_W(0, (int32_t)text_justify) = justify;
    MEM_W(0, (int32_t)text_fg) = (int32_t)colors[0];
    MEM_W(0, (int32_t)text_bg) = (int32_t)colors[1];
    *ctx = saved;
}

extern "C" {

// func_800B45B4 at 0x800B4A20: the frame's clear is written. Once per game frame.
void rush2_menus49_frame(uint8_t* rdram, recomp_context* ctx) {
    restore_font(rdram);
    frame_counter++;
    slot_buttons = 0;
    float dt = read_f32(rdram, frame_dt);
    pulse_time += dt;
    pending.clear();
    row_texts.clear();
    for (List& l : lists) {
        l.width = 0.0f;
        l.min_dy = 0;
        l.prev_y = keep_position;
        l.rows = 0;
    }

    bool option = option_mode.load() != (int)rush2::menus2049::Mode::Off;
    if (option && rush2::wings::rom_available()) {
        std::shared_ptr<const Art> ready;
        bool stale;
        {
            std::lock_guard lock{ art_mutex };
            ready = built;
            stale = built_generation != source_generation.load();
        }
        if (stale) start_build();
        if (ready != nullptr && ready != art) {
            swap_fonts(rdram, false);
            art = ready;
            upload(rdram);
        }
    }
    bool want = option && art != nullptr && rush2::wings::rom_available() && menus_up(rdram);
    if (want != active) {
        active = want;
        swap_fonts(rdram, active);
        swap_styles(rdram, active);
        if (!active) rows.clear();
    }
    if (!active) {
        layout = false;
        return;
    }
    layout = option_mode.load() == (int)rush2::menus2049::Mode::Layout;

    // func_800FC9F8: each vertex's texture coordinates move 0.2 texture widths a second, truncated a frame.
    if (!sfx_started) {
        sfx_u = (art->sfx.w << 6) - 16;               // polygon_set's corner 0: 2w - 0.5 texels
        sfx_v = (art->sfx.h << 6) - 16;
        sfx_started = true;
    }
    sfx_u += (int32_t)(sfx_speed * dt * (float)(art->sfx.w << 5));
    sfx_v += (int32_t)(sfx_speed * dt * (float)(art->sfx.h << 5));
    if (sfx_u - (art->sfx.w << 5) >= (art->sfx.w << 5)) sfx_u -= art->sfx.w << 5;
    if (sfx_v - (art->sfx.h << 5) >= (art->sfx.h << 5)) sfx_v -= art->sfx.h << 5;

    if (frame_counter - background_seen <= 2) {
        draw_background(rdram);
    }
}

// text_select_style_800737E4 entry ($a0 = the style) and exit (0x80073FB0).
void rush2_menus49_style_begin(uint8_t* rdram, recomp_context* ctx) {
    last_style = (int)ctx->r4;
}

void rush2_menus49_style_end(uint8_t* rdram, recomp_context* ctx) {
    if (active && last_style == flash_style) set_pulse(rdram);
}

// Before a text_print_string call that prints a row of a menu list (us.toml lists them): the row gets a button.
void rush2_menus49_row(uint8_t* rdram, recomp_context* ctx, int list) {
    row_list = list;
}

// text_select_font_80088C24 entry: the game picks a font, so the one before_print set for a row is put back first
// (the select returns the font it replaces).
void rush2_menus49_font(uint8_t* rdram, recomp_context* ctx) {
    restore_font(rdram);
}

}
