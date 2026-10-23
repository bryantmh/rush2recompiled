// HUD edge anchoring for widescreen (Settings > Graphics > HUD Placement).
//
// The race HUD is a list of 2D widgets (images and fill rects, 32 bytes each) that the game draws as texture
// and fill rectangles after the 3D views (func_8007D9DC). RT64 keeps rectangles in the centered 4:3 area unless
// they carry an extended GBI alignment, so before each widget is drawn this file inserts a gEXSetRectAlign into
// the game's 2D display list: widgets in the left third of the screen are anchored to the left edge, widgets in
// the right third to the right edge, and the rest stay centered. RT64's HUD ratio setting then decides how far
// the edges are (Original = 4:3, Clamp16x9 = 16:9, Full = the window's edges).
//
// Only widgets created by the race HUD setup (func_800A06F8) are anchored, so menus keep their layout. Widgets
// that make up one element are grouped and anchored together by their combined bounds, so multi-part elements
// (a panel and its digits, a box and its borders) never split apart. Callback widgets have no bounds and print
// their own text (lap times, the best averages' names and title), so each string they print takes the anchor of
// the HUD widget it's drawn over, or its screen third if it isn't over one: anchoring text by its x alone split
// the best averages' names (left third) from their boxes (centered). The laps-left number is printed earlier in
// the frame by a sprite callback (func_800B9D48) and is anchored the same way (it sits on the track map).
//
// Fill widgets that cover the whole screen (the pause menu's translucent dimming) are stretched to the window's
// edges in every mode. The game places them at the overscan inset (12,10) and its 2D scissor clips them to the
// inset, which left an undimmed frame around the 3D view now that it extends to the framebuffer edges.
//
// Where the race HUD's elements go is decided by the layout engine (src/hud_layout.cpp, docs/hud_layout.md) from
// the race's settings alone (collect_config) and the HUD catalog (src/hud_catalog.cpp: every part the HUD can draw,
// from the ROM's layout tables and the game's callbacks), never from what the game drew, so tools/hud_preview shows
// exactly the same layout without the game. This file only applies it: each widget is matched to its catalog part by
// how it was built (rush2_hud_entry_created, rush2_hud_box_part), and its part's place moves it for the draw loop,
// picks its RT64 origin and scales it (finish_half); the odometer is drawn where the engine put it, and text printed
// over an element goes with it (origin_at). With one view or two stacked nothing moves; split, each player's
// elements are placed in their half or quadrant by the engine's rules. A race HUD widget with no catalog part, or
// drawn outside its part's reach, is logged once (check_widgets): the catalog is missing something. The game's 2D
// clip inset is lifted for the draw loop so the HUD can sit closer to the edges than the overscan border.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "recomp.h"
#include "rush2.h"
#include "players4.h"
#include "hud_catalog.h"
#include "hud_layout.h"
#include "battle.h"
#include "car_mods.h"
#include "online.h"
#include "odometer2049.h"
#include "pause_menu.h"
#include "stunt2049.h"
#include "track2049.h"

#include "recompui/recompui.h"
#include "ultramodern/config.hpp"

#define F3DEX_GBI_2
#define G_SC_NON_INTERLACE 0 // gbi.h scissor mode, which the extended GBI header doesn't define.
#include "rt64_extended_gbi.h"

namespace {
    // Game addresses.
    constexpr uint32_t widget_array = 0x802F6C00; // 2D widgets, 0x20 bytes each (moved from 0x800F9558, src/players4.cpp).
    constexpr uint32_t widget_size = 0x20;
    constexpr uint32_t widget_count = 0x800F5444;
    constexpr uint32_t max_widgets = 400;
    constexpr uint32_t dl_2d_cursor = 0x80125ABC; // Next free command in the 2D display list.

    // Widget fields.
    constexpr int32_t widget_image = 0x4;   // Image, or the draw callback if flags & 0x80.
    constexpr int32_t widget_x = 0xA;
    constexpr int32_t widget_y = 0xC;
    constexpr int32_t widget_w = 0x10;
    constexpr int32_t widget_h = 0x12;
    constexpr int32_t widget_flags = 0x15;
    constexpr int32_t widget_hidden = 0x16;
    constexpr int32_t widget_src_y0 = 0x18;  // Source rectangle within the image.
    constexpr int32_t widget_src_x0 = 0x1A;
    constexpr int32_t widget_src_y1 = 0x1C;
    constexpr int32_t widget_src_x1 = 0x1E;
    constexpr uint8_t flag_callback = 0x80;

    constexpr int32_t screen_width = 320;
    constexpr int32_t screen_height = 240;
    constexpr int32_t inset_x = 12; // Overscan inset of the game's 2D scissor.
    constexpr int32_t inset_y = 10;
    constexpr uint32_t clip_inset_x = 0x80023058; // s16: the game clips 2D rectangles to this inset (func_80058AFC).
    constexpr uint32_t clip_inset_y = 0x8002305A;

    // current_origin while a widget is stretched to the window; never equal to a real origin.
    constexpr uint16_t origin_stretched = 0xFFFF;
    // current_origin while a widget is drawn shifted (rush2::hud::set_widget_shift), by (current_dx, current_dy).
    constexpr uint16_t origin_shifted = 0xFFFE;
    int32_t current_dx = 0;
    int32_t current_dy = 0;

    // Whether each widget slot was last allocated by the race HUD setup.
    bool hud_slot[max_widgets];
    bool building_hud = false;

    namespace layout = rush2::hud_layout;
    namespace catalog = rush2::hud_catalog;
    // How each widget was built (rush2_hud_entry_created, rush2_hud_box_part), to find its catalog part, and that
    // part in frame_output.parts (-1: none). slots_dirty: a widget was built since the slots were matched.
    catalog::Identity slot_identity[max_widgets];
    catalog::Identity box_pending;
    int16_t slot_part[max_widgets];
    bool slots_dirty = true;
    // Widgets check_widgets has reported, so each is reported once.
    bool slot_reported[max_widgets];

    // The layout (src/hud_layout.cpp), solved again when the race's settings change, applied to the widgets for the
    // draw loop (rush2_hud_draw_begin) and to the text HUD callbacks print (rush2_hud_print).
    bool layout_solved = false;
    layout::Output frame_output;
    // The parts drawn this frame (frame_output.parts), for the text printed over them (origin_at).
    std::vector<bool> part_shown;

    uint16_t slot_origin[max_widgets];
    int16_t slot_shift_x[max_widgets];
    int16_t slot_shift_y[max_widgets];
    // How far each widget is moved for the draw loop, and its position to restore afterwards.
    int quad_views = 0; // 3 or 4 while the race is drawn in quadrants.
    int16_t slot_dx[max_widgets];
    int16_t slot_dy[max_widgets];
    bool slot_anchored[max_widgets]; // its place is the layout's (it's drawn with slot_origin)
    bool slot_moved[max_widgets];
    int16_t saved_x[max_widgets];
    int16_t saved_y[max_widgets];
    // Split, the clip inset is lifted for the draw loop (the HUD sits closer to the edges), and restored after.
    bool inset_lifted = false;
    // Widgets the layout draws scaled (3-4 players: the skull and the stunt boxes at half size; with the odometer, the
    // time and position at small_scale): their texture rectangles are scaled after the game writes them (finish_half)
    // by slot_scale, about their element's top left corner as drawn (slot_draw_pivot). 0 for other widgets.
    float slot_scale[max_widgets];
    // The scale nearest 75% at which a texel covers a whole number of output pixels, so the shrunk images point sample
    // evenly (every border line the same width). Output pixels per 4:3 screen pixel: 1 at Original resolution, 2 at
    // Original 2x, the window's height / 240 at Auto. Where no whole number comes near enough (low resolutions), 75%.
    float fitting_scale() {
        float px = 1.0f;
        switch (ultramodern::renderer::get_graphics_config().res_option) {
            case ultramodern::renderer::Resolution::Original:
                break;
            case ultramodern::renderer::Resolution::Original2x:
                px = 2.0f;
                break;
            default: {
                int width = 0;
                int height = 0;
                recompui::get_window_size(width, height);
                if (height > 0) {
                    px = (float)height / (float)screen_height;
                }
                break;
            }
        }
        float k = std::round(0.75f * px);
        float s = k / px;
        return k >= 1.0f && s >= 0.6f && s <= 0.85f ? s : 0.75f;
    }
    int32_t slot_draw_pivot_x[max_widgets];
    int32_t slot_draw_pivot_y[max_widgets];
    // The point finish_half scales about, in 4:3 screen pixels; without one, the first rectangle's top left corner.
    bool half_pivot = false;
    int32_t half_pivot_x = 0;
    int32_t half_pivot_y = 0;
    bool half_pending = false;
    int32_t half_start = 0;
    float half_scale_x = 0.5f, half_scale_y = 0.5f;
    // Widgets another file places in final screen coordinates and draws scaled (the battle HUD, src/rush2049/battle.cpp):
    // they aren't anchored, grouped or moved for split screen, and their texture rectangles are scaled by these
    // factors about their top left corner (0 = not such a widget).
    float managed_scale_x[max_widgets];
    float managed_scale_y[max_widgets];
    uint16_t managed_origin[max_widgets];
    // Where the finish flag and laps left (on the track map) moved to this frame: the laps-left number printed with it
    // follows it.
    bool map_known = false;
    int32_t map_dx = 0;
    int32_t map_dy = 0;
    // Its scale about its top left corner (map_x0, map_y0 as the game lays it out): the number's place scales with it.
    float map_scale = 1.0f;
    int32_t map_x0 = 0, map_y0 = 0;
    uint16_t map_origin = G_EX_ORIGIN_NONE;
    constexpr int32_t lifted_inset = 64;
    int16_t saved_inset_x;
    int16_t saved_inset_y;
    uint16_t current_origin = G_EX_ORIGIN_NONE;
    bool scissor_widened = false;
    bool in_hud_callback = false;

    struct Rect {
        int32_t x0, y0, x1, y1;
        bool fill;
        bool screen_fill;
    };
    bool valid(const Rect& r) {
        return r.x0 <= r.x1 && r.y0 <= r.y1;
    }

    // How far text may start outside the widget it's printed on (justified text, outlines).
    constexpr int32_t text_margin = 4;

    // The stunt panel (src/players4.cpp, stunt_boxes): each player's boxes are drawn where the layout puts them, at its
    // scale (half size in quadrants, the scores' size for the trick name in split screen), and their text with them.
    // stunt_box = each player's boxes (0 scores, 1 trick name) where the game lays them out; stunt_text = where their
    // text goes this frame (the boxes' top left corners as drawn, their move, origin and scale), for rush2_hud_print,
    // for a box that's moved or scaled.
    // The race's track id (s8), 11 (STUNT1) in the stunt arenas (Rush 2049's are hosted in that slot).
    constexpr uint32_t stunt_track_id = 0x8010C3F0;
    constexpr int8_t stunt_track = 0xB;
    // In a race in quadrants, the trick name's box shows in the scores' box's place: while it shows, the scores' box
    // is hidden for the draw loop (stunt_hidden: widget slots whose hidden byte was set, restored after) and its text
    // isn't drawn (stunt_swap, per player: set while the trick name's box shows over the scores').
    bool stunt_swap[4];
    bool stunt_hidden[max_widgets];
    Rect stunt_box[4][2];
    struct StuntText {
        bool valid;
        int32_t dx, dy, pivot_x, pivot_y;
        uint16_t origin;
        float scale;
    };
    StuntText stunt_text[4][2];
    // Whether the stunt panel's text callback (func_800B7654) is running (rush2_hud_stunt_text).
    bool in_stunt_text = false;

    void write_2d_commands(uint8_t* rdram, const GfxCommand* cmds, uint32_t count) {
        int32_t cursor = MEM_W(0, (int32_t)dl_2d_cursor);
        if (cursor == 0) {
            return;
        }

        for (uint32_t i = 0; i < count; i++) {
            MEM_W(0, (int32_t)(cursor + i * 8 + 0)) = (int32_t)cmds[i].values.word0;
            MEM_W(0, (int32_t)(cursor + i * 8 + 4)) = (int32_t)cmds[i].values.word1;
        }
        MEM_W(0, (int32_t)dl_2d_cursor) = cursor + (int32_t)(count * 8);
    }

    // Enables the extended GBI and, if `widen_scissor`, widens the scissor to the whole window until the widget loop
    // ends (the game's scissor covers the 4:3 area, which would clip anchored widgets). Returns the command count.
    uint32_t begin_rect_align(GfxCommand* cmds, bool widen_scissor) {
        // The extended GBI is enabled every time because RT64 disables it at the start of every display list task.
        uint32_t count = 0;
        gEXEnable(&cmds[count++]);
        if (widen_scissor && !scissor_widened) {
            gEXPushScissor(&cmds[count++]);
            gEXSetScissor(&cmds[count], G_SC_NON_INTERLACE, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, 0, screen_height);
            count += 2;
            scissor_widened = true;
        }
        return count;
    }

    void set_rect_origin(uint8_t* rdram, uint16_t origin) {
        if (origin == current_origin) {
            return;
        }

        GfxCommand cmds[6];
        uint32_t count = begin_rect_align(cmds, origin != G_EX_ORIGIN_NONE);

        // Rectangle coordinates are relative to the origin, so they're offset by its position on the 4:3 screen.
        int32_t offset = (origin < G_EX_ORIGIN_NONE) ? -(int32_t)(origin * screen_width * 4 / G_EX_ORIGIN_RIGHT) : 0;
        gEXSetRectAlign(&cmds[count], origin, origin, offset, 0, offset, 0);
        count += 2;
        write_2d_commands(rdram, cmds, count);
        current_origin = origin;
    }

    // RT64 draws a rectangle over whole 4:3 screen pixels (it rounds each edge up) and stretches the texture across
    // them, so a shrunk image ending between pixels would run up to a pixel past its last texel: into a repeat of its
    // edge (a thicker far border), its first column again where it wraps, or the next glyph of a strip. A point-sampled
    // shrunk image spans whole pixels instead: fit_span puts the span a..b (4:3 pixels) at the nearest pixel, gives it
    // its length rounded, and sets `step` (texels a pixel, signed; 0 for a fill) so the image's texels fill exactly
    // that, which samples the first and last texel alike (an even outline) and every texel to within an output pixel.
    // `start` (s10.5) moves to the edge of its texel for an image drawn backward (flipped): the texture is sampled at
    // pixel centers, so it would otherwise begin a whole texel in.
    void fit_span(float a, float b, float& step, int32_t& start, int32_t& q0, int32_t& q1) {
        int32_t p0 = (int32_t)std::lround(a);
        int32_t size = std::max<int32_t>(1, (int32_t)std::lround(b - a));
        if (step != 0.0f) {
            float texels = std::max(1.0f, std::round((b - a) * std::fabs(step)));
            if (step < 0.0f) {
                start += 32;
            }
            step = std::copysign(texels / (float)size, step);
        }
        q0 = std::clamp<int32_t>(p0 * 4, 0, 0xFFF);
        q1 = std::clamp<int32_t>((p0 + size) * 4, 0, 0xFFF);
    }

    // Stretches the next rectangles to cover the whole window. The game clips rectangles to its 2D clip rect before
    // drawing them, so instead of mapping exact coordinates, the offsets move each edge past the window's edge (left
    // edges are relative to the window's left edge, right edges to its right edge) and the widened scissor clips them.
    void stretch_rect(uint8_t* rdram) {
        GfxCommand cmds[6];
        uint32_t count = begin_rect_align(cmds, true);
        gEXSetRectAlign(&cmds[count], G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, -screen_width * 4, -screen_height * 4, 0, screen_height * 4);
        count += 2;
        write_2d_commands(rdram, cmds, count);
        current_origin = origin_stretched;
    }

    // Shifts the next rectangles (dx, dy) pixels from where the game puts them, over the whole window. With the
    // original aspect ratio the game's scissor stays, so nothing shows past what the 4:3 screen showed.
    void shift_rect(uint8_t* rdram, int32_t dx, int32_t dy) {
        if (current_origin == origin_shifted && current_dx == dx && current_dy == dy) {
            return;
        }
        bool widescreen = ultramodern::renderer::get_graphics_config().ar_option != ultramodern::renderer::AspectRatio::Original;
        GfxCommand cmds[6];
        uint32_t count = begin_rect_align(cmds, widescreen);
        gEXSetRectAlign(&cmds[count], G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, dx * 4, dy * 4, dx * 4, dy * 4);
        count += 2;
        write_2d_commands(rdram, cmds, count);
        current_origin = origin_shifted;
        current_dx = dx;
        current_dy = dy;
    }

    // Image fields.
    constexpr int32_t image_width = 0x10;
    constexpr int32_t image_height = 0x12;  // rows are stored bottom up: the image's row y is pixel row height - 1 - y
    constexpr int32_t image_size = 0x14;    // G_IM_SIZ
    constexpr int32_t image_format = 0x15;  // G_IM_FMT
    constexpr int32_t image_palette = 0x16; // s16 index into the palette table, if the widget names no palette.
    constexpr int32_t image_pixels = 0x18;
    constexpr int32_t widget_palette = 0x0; // Palette (its colors at +0x14), or 0.
    constexpr int32_t widget_palette_bank = 0x8; // u16, >> 10 selects the palette table.
    constexpr uint32_t palette_tables = 0x80119428; // 8 bytes each, a pointer to 0x18 byte palettes.
    constexpr int32_t palette_colors = 0x14;

    bool is_rdram(uint32_t address) {
        return rush2::in_game_ram(address);
    }

    // Where a widget's image texels are: pixels, the colors of a color indexed image (RGBA16), format, texel size and
    // row stride in texels. False if the image can't be read.
    struct ImageInfo {
        uint32_t pixels = 0, palette = 0;
        int fmt = 0, siz = 0;
        int32_t stride = 0;
        int32_t height = 0;
        // The texel index of the image's (x, y) (gfx_draw_frame_views_8007D9DC loads rows height - 1 - y).
        int32_t index(int32_t x, int32_t y) const {
            return (height - 1 - y) * stride + x;
        }
    };
    bool image_info(uint8_t* rdram, int32_t widget, ImageInfo& out) {
        uint32_t image = (uint32_t)MEM_W(widget_image, widget);
        if (!is_rdram(image)) {
            return false;
        }
        out.pixels = (uint32_t)MEM_W(image_pixels, (int32_t)image);
        out.fmt = MEM_BU(image_format, (int32_t)image);
        out.siz = MEM_BU(image_size, (int32_t)image);
        out.stride = MEM_HU(image_width, (int32_t)image);
        out.height = MEM_HU(image_height, (int32_t)image);
        if (!is_rdram(out.pixels) || out.stride <= 0 || out.height <= 0) {
            return false;
        }
        out.palette = 0;
        if (out.fmt == 2) {
            uint32_t named = (uint32_t)MEM_W(widget_palette, widget);
            int32_t index = MEM_H(image_palette, (int32_t)image);
            if (is_rdram(named)) {
                out.palette = (uint32_t)MEM_W(palette_colors, (int32_t)named);
            }
            else if (index >= 0) {
                uint32_t table = (uint32_t)MEM_W((MEM_HU(widget_palette_bank, widget) >> 10) * 8, (int32_t)palette_tables);
                if (is_rdram(table)) {
                    out.palette = (uint32_t)MEM_W(index * 0x18 + palette_colors, (int32_t)table);
                }
            }
            if (!is_rdram(out.palette)) {
                return false;
            }
        }
        return true;
    }

    // Scales the texture rectangles written to the 2D display list since half_start by half_scale_x/y (a half for the
    // skull), about the first one's top left corner (the image's), dividing their texture steps by the same.
    // The track map's widget, drawn this loop from map_start in the 2D display list (finish_map).
    bool map_pending = false;
    int32_t map_start = 0;

    // With three players the fourth quadrant is black, and the track map (a CI texture of a dark outline, drawn as
    // texel x primitive color 0xFFFFFF80 over the cross between the views) disappears over it: the part of the map
    // over that quadrant is drawn again, clipped to the quadrant, opaque and in grey (the outline's shape from the
    // texels' alpha), so the black of the quadrant around it stays as it is.
    constexpr uint32_t map_quadrant_color = 0x909090FF;
    constexpr uint32_t track_map_callback = 0x800B8188; // minimap_draw_800B8188: the track map's widget
    void finish_map(uint8_t* rdram) {
        if (!map_pending) {
            return;
        }
        map_pending = false;
        int32_t end = MEM_W(0, (int32_t)dl_2d_cursor);
        constexpr int max_copied = 120;
        if (rush2::splitscreen::quadrant_views(rdram) != 3 || end <= map_start || (end - map_start) / 8 > max_copied) {
            return;
        }
        GfxCommand cmds[max_copied + 8];
        uint32_t count = 0;
        gEXEnable(&cmds[count++]);
        gEXPushScissor(&cmds[count++]);
        // The empty quadrant (as src/splitscreen.cpp fills it), its right edge at the window's right edge.
        gEXSetScissor(&cmds[count], G_SC_NON_INTERLACE, G_EX_ORIGIN_NONE, G_EX_ORIGIN_RIGHT, screen_width / 2 + 1,
                      screen_height / 2 + 1, 0, screen_height);
        count += 2;
        for (int32_t c = map_start; c < end; c += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, c);
            uint32_t w1 = (uint32_t)MEM_W(4, c);
            if ((w0 >> 24) == 0xFA) {
                w1 = map_quadrant_color; // G_SETPRIMCOLOR
            }
            else if ((w0 >> 24) == 0xFC) {
                // G_SETCOMBINE: the map's texel x primitive becomes color = primitive, alpha = texel x primitive (the
                // same in both cycles).
                constexpr uint32_t c_a = 15, c_b = 15, c_c = 31, c_d = 3, a_a = 1, a_b = 7, a_c = 3, a_d = 7;
                w0 = 0xFC000000 | (c_a << 20) | (c_c << 15) | (a_a << 12) | (a_c << 9) | (c_a << 5) | c_c;
                w1 = (c_b << 28) | (c_b << 24) | (a_a << 21) | (a_c << 18) | (c_d << 15) | (a_b << 12) | (a_d << 9) |
                     (c_d << 6) | (a_b << 3) | a_d;
            }
            cmds[count].values.word0 = w0;
            cmds[count].values.word1 = w1;
            count++;
        }
        gEXPopScissor(&cmds[count++]);
        write_2d_commands(rdram, cmds, count);
    }

    void finish_half(uint8_t* rdram) {
        if (!half_pending) {
            return;
        }
        half_pending = false;
        float sx = half_scale_x, sy = half_scale_y;
        half_scale_x = half_scale_y = 0.5f;
        int32_t end = MEM_W(0, (int32_t)dl_2d_cursor);
        bool anchored = false;
        uint32_t ax = (uint32_t)std::max(0, half_pivot_x * 4);
        uint32_t ay = (uint32_t)std::max(0, half_pivot_y * 4);
        bool fixed_pivot = half_pivot;
        half_pivot = false;
        int32_t first_rect = 0; // valid once anchored
        // Shrunk by less than half: point sampled (below).
        bool point = sx > 0.5f && sx < 1.0f;
        // A texture rectangle is three commands, a fill rectangle one (the stunt boxes' are their widget's last).
        for (int32_t c = half_start; c + 8 <= end; c += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, c);
            uint32_t op = w0 >> 24;
            if ((op != 0xE4 && op != 0xE5 && op != 0xF6) || (op != 0xF6 && c + 24 > end)) {
                continue;
            }
            uint32_t w1 = (uint32_t)MEM_W(4, c);
            uint32_t x1 = (w0 >> 12) & 0xFFF;
            uint32_t y1 = w0 & 0xFFF;
            uint32_t x0 = (w1 >> 12) & 0xFFF;
            uint32_t y0 = w1 & 0xFFF;
            if (!anchored) {
                if (!fixed_pivot) {
                    ax = x0;
                    ay = y0;
                }
                anchored = true;
                first_rect = c;
            }
            auto about = [](uint32_t v, uint32_t a, float s) {
                return (float)a + ((float)v - (float)a) * s;
            };
            auto quarter = [](float v) {
                return (uint32_t)std::clamp<int32_t>((int32_t)std::lround(v), 0, 0xFFF);
            };
            float fx0 = about(x0, ax, sx), fy0 = about(y0, ay, sy);
            float fx1 = about(x1, ax, sx), fy1 = about(y1, ay, sy);
            // A texture rectangle: G_RDPHALF_1 with the texture coordinates (s10.5 each), then G_RDPHALF_2 with the
            // steps (s5.10 each), signed (an image drawn flipped steps backward).
            bool textured = op != 0xF6;
            int32_t coords = c + 8;
            int32_t steps = c + 16;
            float step_x = 0.0f, step_y = 0.0f;
            if (textured) {
                uint32_t st = (uint32_t)MEM_W(4, steps);
                step_x = (float)(int16_t)(st >> 16) / sx / 1024.0f;
                step_y = (float)(int16_t)(st & 0xFFFF) / sy / 1024.0f;
            }
            // Point sampled (below), the image spans whole pixels.
            int32_t start = textured ? MEM_W(4, coords) : 0;
            int32_t start_s = (int16_t)((uint32_t)start >> 16), start_t = (int16_t)(start & 0xFFFF);
            if (point) {
                int32_t qx0, qx1, qy0, qy1;
                fit_span(fx0 / 4.0f, fx1 / 4.0f, step_x, start_s, qx0, qx1);
                fit_span(fy0 / 4.0f, fy1 / 4.0f, step_y, start_t, qy0, qy1);
                x0 = (uint32_t)qx0;
                y0 = (uint32_t)qy0;
                x1 = (uint32_t)qx1;
                y1 = (uint32_t)qy1;
            }
            else {
                x0 = quarter(fx0);
                y0 = quarter(fy0);
                x1 = quarter(fx1);
                y1 = quarter(fy1);
            }
            MEM_W(0, c) = (int32_t)((op << 24) | (x1 << 12) | y1);
            MEM_W(4, c) = (int32_t)((w1 & 0xFF000000u) | (x0 << 12) | y0);
            if (!textured) {
                // A fill rectangle (the scaled time's boxes) has no texture steps.
                continue;
            }
            uint32_t dsdx = (uint32_t)std::lround(step_x * 1024.0f) & 0xFFFF;
            uint32_t dtdy = (uint32_t)std::lround(step_y * 1024.0f) & 0xFFFF;
            MEM_W(4, steps) = (int32_t)((dsdx << 16) | dtdy);
            MEM_W(4, coords) = (int32_t)(((uint32_t)start_s & 0xFFFF) << 16 | ((uint32_t)start_t & 0xFFFF));
            c += 16;
        }
        // Shrunk by less than half (the 3-4 player time and position), filtering blends the color of the images'
        // transparent texels (magenta) into their edges: the rectangles are point sampled, between a push and a pop of
        // the other modes. The commands from the first rectangle on move down to make room.
        if (point && anchored) {
            GfxCommand before[3];
            uint32_t n = 0;
            gEXEnable(&before[n++]);
            gEXPushOtherMode(&before[n++]);
            // G_SETOTHERMODE_H, G_MDSFT_TEXTFILT (shift 12, 2 bits) = G_TF_POINT.
            before[n].values.word0 = 0xE3000000u | ((32u - 12u - 2u) << 8) | (2u - 1u);
            before[n++].values.word1 = 0;
            int32_t shift = (int32_t)(n * 8);
            for (int32_t c = end - 8; c >= first_rect; c -= 8) {
                MEM_W(shift, c) = MEM_W(0, c);
                MEM_W(shift + 4, c) = MEM_W(4, c);
            }
            for (uint32_t i = 0; i < n; i++) {
                MEM_W(0, first_rect + (int32_t)(i * 8)) = (int32_t)before[i].values.word0;
                MEM_W(4, first_rect + (int32_t)(i * 8)) = (int32_t)before[i].values.word1;
            }
            MEM_W(0, (int32_t)dl_2d_cursor) = end + shift;
            GfxCommand after;
            gEXPopOtherMode(&after);
            write_2d_commands(rdram, &after, 1);
        }
    }

    // Whether a widget is a fill that covers the screen inside the overscan inset.
    bool is_screen_fill(uint8_t* rdram, int32_t widget) {
        int32_t x = MEM_H(widget_x, widget);
        int32_t y = MEM_H(widget_y, widget);
        return MEM_W(widget_image, widget) == 0 && (MEM_BU(widget_flags, widget) & flag_callback) == 0 &&
            x <= inset_x && y <= inset_y &&
            x + MEM_H(widget_w, widget) >= screen_width - inset_x && y + MEM_H(widget_h, widget) >= screen_height - inset_y;
    }

    // Restores the default alignment and the game's scissor after anchored draws.
    void end_anchoring(uint8_t* rdram) {
        set_rect_origin(rdram, G_EX_ORIGIN_NONE);
        if (scissor_widened) {
            GfxCommand cmd;
            gEXPopScissor(&cmd);
            write_2d_commands(rdram, &cmd, 1);
            scissor_widened = false;
        }
    }

    // Origin for text a HUD callback prints at (x, y), which also moves (x, y) with the element under it (the
    // layout's place_text: the smallest widget drawn there; split, by itself if there is none). Screen fills (the
    // pause dimming) are never under it: they're stretched, not anchored.
    uint16_t origin_at(int32_t& x, int32_t& y) {
        layout::TextPlace t = layout::place_text(frame_output, x, y, &part_shown);
        x += t.dx;
        // Text on an element hidden by hand goes off the screen.
        y = t.hidden ? -1000 : y + t.dy;
        return t.origin;
    }
}

void rush2::hud::anchor_text(uint8_t* rdram, int32_t& x, int32_t& y) {
    set_rect_origin(rdram, origin_at(x, y));
}

void rush2::hud::set_anchor(uint8_t* rdram, float fraction) {
    set_rect_origin(rdram, (uint16_t)std::lround(std::clamp(fraction, 0.0f, 1.0f) * G_EX_ORIGIN_RIGHT));
}

// Digits drawn straight into the 2D display list (the game's own text is queued and drawn later, so it can't take
// an anchor here): an 8 x 10 intensity image per digit in spare RDRAM (0x80C9F000; src/controls_menu.cpp lists the
// ranges in use), drawn as a texture rectangle tinted by the primitive color, a black copy first as its shadow.
namespace {
    constexpr uint32_t digit_images = 0x80C9F000;
    constexpr int digit_w = 8, digit_h = 10;
    const char* const digit_rows[10][digit_h] = {
        { ".######.", "########", "###..###", "###..###", "###..###", "###..###", "###..###", "###..###", "########", ".######." },
        { "...###..", "..####..", ".#####..", "...###..", "...###..", "...###..", "...###..", "...###..", ".######.", ".######." },
        { ".######.", "########", "###..###", ".....###", "....###.", "..####..", ".###....", "###.....", "########", "########" },
        { ".######.", "########", "###..###", ".....###", "..#####.", "..#####.", ".....###", "###..###", "########", ".######." },
        { "....###.", "...####.", "..#####.", ".###.###", "###..###", "########", "########", ".....###", ".....###", ".....###" },
        { "########", "########", "###.....", "#######.", "########", ".....###", ".....###", "###..###", "########", ".######." },
        { ".######.", "########", "###.....", "#######.", "########", "###..###", "###..###", "###..###", "########", ".######." },
        { "########", "########", ".....###", "....###.", "....###.", "...###..", "...###..", "..###...", "..###...", "..###..." },
        { ".######.", "########", "###..###", "###..###", ".######.", "########", "###..###", "###..###", "########", ".######." },
        { ".######.", "########", "###..###", "###..###", "########", ".#######", ".....###", "###..###", "########", ".######." },
    };
    bool digits_written = false;

    void write_digit_images(uint8_t* rdram) {
        for (int d = 0; d < 10; d++) {
            for (int y = 0; y < digit_h; y++) {
                for (int x = 0; x < digit_w; x++) {
                    MEM_B(d * digit_w * digit_h + y * digit_w + x, (int32_t)digit_images) = digit_rows[d][y][x] == '#' ? (int8_t)0xFF : 0;
                }
            }
        }
        digits_written = true;
    }
}

void rush2::hud::set_prim_color(uint8_t* rdram, uint32_t rgba) {
    GfxCommand cmd;
    cmd.values.word0 = 0xFA000000;
    cmd.values.word1 = rgba;
    write_2d_commands(rdram, &cmd, 1);
}

void rush2::hud::draw_number(uint8_t* rdram, const char* digits, float center_x, float center_y, float height, float anchor) {
    int32_t dl = MEM_W(0, (int32_t)dl_2d_cursor);
    size_t length = strlen(digits);
    if (dl == 0 || length == 0 || length > 6) {
        return;
    }
    if (!digits_written) {
        write_digit_images(rdram);
    }
    set_anchor(rdram, anchor);
    GfxCommand cmds[160];
    uint32_t count = 0;
    auto cmd = [&](uint32_t w0, uint32_t w1) {
        cmds[count].values.word0 = w0;
        cmds[count].values.word1 = w1;
        count++;
    };
    gEXEnable(&cmds[count++]);
    gEXPushPrimColor(&cmds[count++]);
    gEXPushOtherMode(&cmds[count++]);
    gEXPushCombineMode(&cmds[count++]);
    cmd(0xE7000000, 0);
    // 1-cycle, bilinear, translucent surface blending, no Z; color = primitive, alpha = texel x primitive alpha (as
    // src/controls_menu.cpp's glyphs).
    cmd(0xEF000000 | 0x002CF0, 0x00504240);
    uint32_t c_sa = 15, c_sb = 15, c_m = 31, c_a = 3, a_sa = 1, a_sb = 7, a_m = 3, a_a = 7;
    cmd(0xFC000000 | (c_sa << 20) | (c_m << 15) | (a_sa << 12) | (a_m << 9) | (c_sa << 5) | c_m,
        (c_sb << 28) | (c_a << 15) | (a_sb << 12) | (a_a << 9) | (c_sb << 24) | (a_sa << 21) | (a_m << 18) | (c_a << 6) | (a_sb << 3) | a_a);
    constexpr uint32_t fmt_i = 4, siz_8b = 1, siz_16b = 2, clamp = 2;
    uint32_t tile_clamp = (clamp << 18) | (clamp << 8);
    float scale = height / (float)digit_h;
    float glyph = (float)digit_w * scale, advance = glyph + scale;
    float left = center_x - (advance * (float)length - scale) * 0.5f, top = center_y - height * 0.5f;
    for (int pass = 0; pass < 2; pass++) {
        // The shadow, a pixel down and right, then the digits in white.
        cmd(0xFA000000, pass == 0 ? 0x000000FFu : 0xFFFFFFFFu);
        float shift = pass == 0 ? std::max(1.0f, scale) : 0.0f;
        for (size_t i = 0; i < length; i++) {
            if (digits[i] < '0' || digits[i] > '9') {
                continue;
            }
            uint32_t image = (digit_images + (uint32_t)(digits[i] - '0') * digit_w * digit_h) & 0x1FFFFFFF;
            cmd(0xFD000000 | (fmt_i << 21) | (siz_16b << 19), image);
            cmd(0xF5000000 | (fmt_i << 21) | (siz_16b << 19), (7u << 24) | tile_clamp);
            cmd(0xE6000000, 0);
            cmd(0xF3000000, (7u << 24) | ((uint32_t)((digit_w * digit_h + 1) / 2 - 1) << 12) | 2048);
            cmd(0xE7000000, 0);
            cmd(0xF5000000 | (fmt_i << 21) | (siz_8b << 19) | (1u << 9), tile_clamp);
            cmd(0xF2000000, ((uint32_t)((digit_w - 1) << 2) << 12) | (uint32_t)((digit_h - 1) << 2));
            int32_t x0 = (int32_t)std::lround((left + advance * (float)i + shift) * 4.0f);
            int32_t y0 = (int32_t)std::lround((top + shift) * 4.0f);
            int32_t x1 = x0 + (int32_t)std::lround(glyph * 4.0f), y1 = y0 + (int32_t)std::lround(height * 4.0f);
            if (x0 < 0 || y0 < 0 || x1 > 0xFFF || y1 > 0xFFF) {
                continue;
            }
            cmd(0xE4000000 | ((uint32_t)x1 << 12) | (uint32_t)y1, ((uint32_t)x0 << 12) | (uint32_t)y0);
            cmd(0xE1000000, 0);
            uint32_t step = (uint32_t)std::lround(1024.0f / scale) & 0xFFFF;
            cmd(0xF1000000, (step << 16) | step);
        }
    }
    cmd(0xE7000000, 0);
    gEXPopCombineMode(&cmds[count++]);
    gEXPopOtherMode(&cmds[count++]);
    gEXPopPrimColor(&cmds[count++]);
    write_2d_commands(rdram, cmds, count);
}

// A filled rectangle in the 2D display list, blended by its alpha, anchored like draw_number's digits.
void rush2::hud::draw_rect(uint8_t* rdram, float x0, float y0, float x1, float y1, uint32_t rgba, float anchor) {
    if (MEM_W(0, (int32_t)dl_2d_cursor) == 0 || x1 <= x0 || y1 <= y0) {
        return;
    }
    set_anchor(rdram, anchor);
    int32_t fx0 = (int32_t)std::lround(x0 * 4.0f), fy0 = (int32_t)std::lround(y0 * 4.0f);
    int32_t fx1 = (int32_t)std::lround(x1 * 4.0f), fy1 = (int32_t)std::lround(y1 * 4.0f);
    if (fx0 < 0 || fy0 < 0 || fx1 > 0xFFF || fy1 > 0xFFF) {
        return;
    }
    GfxCommand cmds[16];
    uint32_t count = 0;
    auto cmd = [&](uint32_t w0, uint32_t w1) {
        cmds[count].values.word0 = w0;
        cmds[count].values.word1 = w1;
        count++;
    };
    gEXEnable(&cmds[count++]);
    gEXPushPrimColor(&cmds[count++]);
    gEXPushOtherMode(&cmds[count++]);
    gEXPushCombineMode(&cmds[count++]);
    cmd(0xE7000000, 0);
    // 1-cycle, translucent surface blending, no Z; color and alpha = primitive (the game's own filled polygons,
    // func_8007C624).
    cmd(0xEF000000 | 0x002CF0, 0x00504240);
    cmd(0xFCFFFFFF, 0xFFFDF6FB);
    cmd(0xFA000000, rgba);
    cmd(0xF6000000 | ((uint32_t)fx1 << 12) | (uint32_t)fy1, ((uint32_t)fx0 << 12) | (uint32_t)fy0);
    cmd(0xE7000000, 0);
    gEXPopCombineMode(&cmds[count++]);
    gEXPopOtherMode(&cmds[count++]);
    gEXPopPrimColor(&cmds[count++]);
    write_2d_commands(rdram, cmds, count);
}

void rush2::hud::draw_image(uint8_t* rdram, uint32_t address, int w, int h, float x0, float y0, float x1, float y1, float anchor) {
    draw_image_part(rdram, address, w, h, 0, 0, w, h, x0, y0, x1, y1, anchor);
}

void rush2::hud::draw_image_part(uint8_t* rdram, uint32_t address, int w, int h, int src_x, int src_y, int src_w, int src_h,
                                 float x0, float y0, float x1, float y1, float anchor, bool point, uint8_t alpha) {
    if (MEM_W(0, (int32_t)dl_2d_cursor) == 0 || x1 <= x0 || y1 <= y0 || w <= 0 || h <= 0 || w * h > 2048 ||
        src_w <= 0 || src_h <= 0) {
        return;
    }
    set_anchor(rdram, anchor);
    float step_x = (float)src_w / (x1 - x0);
    float step_y = (float)src_h / (y1 - y0);
    int32_t fx0 = (int32_t)std::lround(x0 * 4.0f), fy0 = (int32_t)std::lround(y0 * 4.0f);
    int32_t fx1 = (int32_t)std::lround(x1 * 4.0f), fy1 = (int32_t)std::lround(y1 * 4.0f);
    int32_t start_s = src_x * 32, start_t = src_y * 32;
    // Point sampled, the image spans whole pixels (fit_span).
    if (point) {
        fit_span(x0, x1, step_x, start_s, fx0, fx1);
        fit_span(y0, y1, step_y, start_t, fy0, fy1);
    }
    if (fx0 < 0 || fy0 < 0 || fx1 > 0xFFF || fy1 > 0xFFF) {
        return;
    }
    GfxCommand cmds[24];
    uint32_t count = 0;
    auto cmd = [&](uint32_t w0, uint32_t w1) {
        cmds[count].values.word0 = w0;
        cmds[count].values.word1 = w1;
        count++;
    };
    gEXEnable(&cmds[count++]);
    gEXPushPrimColor(&cmds[count++]);
    gEXPushOtherMode(&cmds[count++]);
    gEXPushCombineMode(&cmds[count++]);
    cmd(0xE7000000, 0);
    // 1-cycle, bilinear (or point), translucent surface blending, no Z (draw_number's); color = the texel's, alpha =
    // the texel's times the primitive's.
    cmd(0xEF000000 | (point ? 0x000CF0 : 0x002CF0), 0x00504240);
    cmd(0xFA000000, 0xFFFFFF00u | alpha);
    uint32_t c_sa = 15, c_sb = 15, c_m = 31, c_a = 1, a_sa = 1, a_sb = 7, a_m = 3, a_a = 7;
    cmd(0xFC000000 | (c_sa << 20) | (c_m << 15) | (a_sa << 12) | (a_m << 9) | (c_sa << 5) | c_m,
        (c_sb << 28) | (c_a << 15) | (a_sb << 12) | (a_a << 9) | (c_sb << 24) | (a_sa << 21) | (a_m << 18) | (c_a << 6) | (a_sb << 3) | a_a);
    constexpr uint32_t fmt_rgba = 0, siz_16b = 2, clamp = 2;
    uint32_t tile_clamp = (clamp << 18) | (clamp << 8);
    uint32_t line = ((uint32_t)w * 2 + 7) / 8;
    cmd(0xFD000000 | (fmt_rgba << 21) | (siz_16b << 19), address & 0x1FFFFFFF);
    cmd(0xF5000000 | (fmt_rgba << 21) | (siz_16b << 19), (7u << 24) | tile_clamp);
    cmd(0xE6000000, 0);
    cmd(0xF3000000, (7u << 24) | ((uint32_t)(w * h - 1) << 12) | ((2048 + line - 1) / line));
    cmd(0xE7000000, 0);
    cmd(0xF5000000 | (fmt_rgba << 21) | (siz_16b << 19) | (line << 9), tile_clamp);
    cmd(0xF2000000, ((uint32_t)((w - 1) << 2) << 12) | (uint32_t)((h - 1) << 2));
    cmd(0xE4000000 | ((uint32_t)fx1 << 12) | (uint32_t)fy1, ((uint32_t)fx0 << 12) | (uint32_t)fy0);
    cmd(0xE1000000, ((uint32_t)start_s & 0xFFFF) << 16 | ((uint32_t)start_t & 0xFFFF));
    cmd(0xF1000000, ((uint32_t)std::lround(1024.0f * step_x) & 0xFFFF) << 16 | ((uint32_t)std::lround(1024.0f * step_y) & 0xFFFF));
    cmd(0xE7000000, 0);
    gEXPopCombineMode(&cmds[count++]);
    gEXPopOtherMode(&cmds[count++]);
    gEXPopPrimColor(&cmds[count++]);
    write_2d_commands(rdram, cmds, count);
}

void rush2::hud::draw_i4_part(uint8_t* rdram, uint32_t address, int w, int h, int src_x, int src_y, int src_w, int src_h,
                               float x0, float y0, float x1, float y1, float anchor, uint32_t rgba, uint32_t shade) {
    if (MEM_W(0, (int32_t)dl_2d_cursor) == 0 || x1 <= x0 || y1 <= y0 || w <= 0 || h <= 0 || (w & 15) != 0 ||
        w * h > 8192 || src_w <= 0 || src_h <= 0) {
        return;
    }
    set_anchor(rdram, anchor);
    float step_x = (float)src_w / (x1 - x0);
    float step_y = (float)src_h / (y1 - y0);
    int32_t fx0 = (int32_t)std::lround(x0 * 4.0f), fy0 = (int32_t)std::lround(y0 * 4.0f);
    int32_t fx1 = (int32_t)std::lround(x1 * 4.0f), fy1 = (int32_t)std::lround(y1 * 4.0f);
    int32_t start_s = src_x * 32, start_t = src_y * 32;
    if (fx0 < 0 || fy0 < 0 || fx1 > 0xFFF || fy1 > 0xFFF) {
        return;
    }
    GfxCommand cmds[26];
    uint32_t count = 0;
    auto cmd = [&](uint32_t w0, uint32_t w1) {
        cmds[count].values.word0 = w0;
        cmds[count].values.word1 = w1;
        count++;
    };
    gEXEnable(&cmds[count++]);
    gEXPushPrimColor(&cmds[count++]);
    gEXPushEnvColor(&cmds[count++]);
    gEXPushOtherMode(&cmds[count++]);
    gEXPushCombineMode(&cmds[count++]);
    cmd(0xE7000000, 0);
    // 1-cycle, bilinear, translucent surface blending, no Z (draw_image_part's); color = shade to rgba by the texel's
    // intensity, alpha = the texel's times rgba's.
    cmd(0xEF000000 | 0x002CF0, 0x00504240);
    cmd(0xFA000000, rgba);
    cmd(0xFB000000, shade);
    uint32_t c_a = 3, c_b = 5, c_c = 1, c_d = 5, a_a = 1, a_b = 7, a_c = 3, a_d = 7;
    cmd(0xFC000000 | (c_a << 20) | (c_c << 15) | (a_a << 12) | (a_c << 9) | (c_a << 5) | c_c,
        (c_b << 28) | (c_d << 15) | (a_b << 12) | (a_d << 9) | (c_b << 24) | (a_a << 21) | (a_c << 18) | (c_d << 6) | (a_b << 3) | a_d);
    // gDPLoadTextureBlock_4b with both axes clamped (no masks), so RT64 hashes just the image: texture packs replace
    // it by that hash (tools/build_font_pack.py rt64_hash).
    constexpr uint32_t fmt_i = 4, siz_4b = 0, siz_16b = 2, clamp = 2;
    uint32_t tile_clamp = (clamp << 18) | (clamp << 8);
    uint32_t line = ((uint32_t)w / 2 + 7) / 8;
    uint32_t words = (uint32_t)w / 16;
    cmd(0xFD000000 | (fmt_i << 21) | (siz_16b << 19), address & 0x1FFFFFFF);
    cmd(0xF5000000 | (fmt_i << 21) | (siz_16b << 19), (7u << 24) | tile_clamp);
    cmd(0xE6000000, 0);
    cmd(0xF3000000, (7u << 24) | ((uint32_t)((w * h + 3) / 4 - 1) << 12) | ((2048 + words - 1) / words));
    cmd(0xE7000000, 0);
    cmd(0xF5000000 | (fmt_i << 21) | (siz_4b << 19) | (line << 9), tile_clamp);
    cmd(0xF2000000, ((uint32_t)((w - 1) << 2) << 12) | (uint32_t)((h - 1) << 2));
    cmd(0xE4000000 | ((uint32_t)fx1 << 12) | (uint32_t)fy1, ((uint32_t)fx0 << 12) | (uint32_t)fy0);
    cmd(0xE1000000, ((uint32_t)start_s & 0xFFFF) << 16 | ((uint32_t)start_t & 0xFFFF));
    cmd(0xF1000000, ((uint32_t)std::lround(1024.0f * step_x) & 0xFFFF) << 16 | ((uint32_t)std::lround(1024.0f * step_y) & 0xFFFF));
    cmd(0xE7000000, 0);
    gEXPopCombineMode(&cmds[count++]);
    gEXPopOtherMode(&cmds[count++]);
    gEXPopEnvColor(&cmds[count++]);
    gEXPopPrimColor(&cmds[count++]);
    write_2d_commands(rdram, cmds, count);
}

void rush2::hud::push_scissor(uint8_t* rdram, float x0, float y0, float x1, float y1, float anchor) {
    if (MEM_W(0, (int32_t)dl_2d_cursor) == 0) {
        return;
    }
    uint16_t origin = (uint16_t)std::lround(std::clamp(anchor, 0.0f, 1.0f) * G_EX_ORIGIN_RIGHT);
    // Coordinates relative to the origin's place on the 4:3 screen.
    float shift = (float)origin * (float)screen_width / (float)G_EX_ORIGIN_RIGHT;
    GfxCommand cmds[4];
    uint32_t count = 0;
    gEXEnable(&cmds[count++]);
    gEXPushScissor(&cmds[count++]);
    gEXSetScissor(&cmds[count], G_SC_NON_INTERLACE, origin, origin, (int)std::lround(x0 - shift), (int)std::lround(y0),
        (int)std::lround(x1 - shift), (int)std::lround(y1));
    count += 2;
    write_2d_commands(rdram, cmds, count);
}

void rush2::hud::pop_scissor(uint8_t* rdram) {
    if (MEM_W(0, (int32_t)dl_2d_cursor) == 0) {
        return;
    }
    GfxCommand cmds[2];
    uint32_t count = 0;
    gEXEnable(&cmds[count++]);
    gEXPopScissor(&cmds[count++]);
    write_2d_commands(rdram, cmds, count);
}

void rush2::hud::call_list(uint8_t* rdram, uint32_t dl) {
    if (MEM_W(0, (int32_t)dl_2d_cursor) == 0) {
        return;
    }
    GfxCommand cmds[16];
    uint32_t count = 0;
    gEXEnable(&cmds[count++]);
    gEXPushOtherMode(&cmds[count++]);
    gEXPushCombineMode(&cmds[count++]);
    gEXPushGeometryMode(&cmds[count++]);
    gEXPushPrimColor(&cmds[count++]);
    gEXPushViewport(&cmds[count++]);
    gEXPushProjectionMatrix(&cmds[count++]);
    cmds[count].values.word0 = 0xDE000000;
    cmds[count].values.word1 = dl;
    count++;
    gEXPopProjectionMatrix(&cmds[count++]);
    gEXPopViewport(&cmds[count++]);
    gEXPopPrimColor(&cmds[count++]);
    gEXPopGeometryMode(&cmds[count++]);
    gEXPopCombineMode(&cmds[count++]);
    gEXPopOtherMode(&cmds[count++]);
    write_2d_commands(rdram, cmds, count);
}

bool rush2::hud::battle_place(int player, rush2::hud_layout::BattlePlace& out) {
    if (player < 0 || player > 3 || !layout_solved || !frame_output.battle[player].valid) {
        return false;
    }
    out = frame_output.battle[player];
    return true;
}

bool rush2::hud::stunt_place(int player, rush2::hud_layout::StuntPlace& out) {
    if (player < 0 || player > 3 || !layout_solved) {
        return false;
    }
    out = rush2::hud_layout::StuntPlace{};
    bool any = false;
    for (const layout::Element& e : frame_output.elements) {
        int k = e.role == layout::Role::StuntScore ? 0 : e.role == layout::Role::StuntTrick ? 1 : -1;
        if (k < 0 || e.player != player || !e.possible || e.hidden) {
            continue;
        }
        rush2::hud_layout::StuntPlace::Box& b = out.box[k];
        b.valid = true;
        b.x = (float)e.x;
        b.y = (float)e.y;
        b.w = (float)(e.game.x1 - e.game.x0);
        b.h = (float)(e.game.y1 - e.game.y0);
        b.scale = e.scale;
        b.anchor = e.origin < G_EX_ORIGIN_NONE ? e.origin / (float)G_EX_ORIGIN_RIGHT : 0.5f;
        any = true;
    }
    out.swap = frame_output.stunt_swap[player];
    return any;
}

void rush2::hud::set_widget_scale(int slot, float scale_x, float scale_y, float anchor) {
    if (slot >= 0 && slot < (int)max_widgets) {
        managed_scale_x[slot] = scale_x;
        managed_scale_y[slot] = scale_y;
        managed_origin[slot] = (uint16_t)std::lround(std::clamp(anchor, 0.0f, 1.0f) * G_EX_ORIGIN_RIGHT);
    }
}

void rush2::hud::set_widget_shift(int slot, int32_t dx, int32_t dy) {
    if (slot >= 0 && slot < (int)max_widgets) {
        slot_shift_x[slot] = (int16_t)dx;
        slot_shift_y[slot] = (int16_t)dy;
    }
}

void rush2::hud::clear_widget_scales() {
    for (uint32_t i = 0; i < max_widgets; i++) {
        managed_scale_x[i] = managed_scale_y[i] = 0.0f;
    }
}

extern "C" {

// func_800A06F8 entry and exit: the race HUD setup, which creates all of its widgets.
void rush2_hud_build_begin(uint8_t* rdram, recomp_context* ctx) {
    building_hud = true;
    rush2::online::game::hud_players_begin(rdram);
}

void rush2_hud_build_end(uint8_t* rdram, recomp_context* ctx) {
    // Players 3 and 4's elements, built while the widgets they create still count as the HUD's.
    rush2::players4::hud_built(rdram, ctx);
    rush2::battle::hud_built(rdram, ctx);
    rush2::online::game::hud_built(rdram);
    rush2::online::game::hud_players_end(rdram);
    building_hud = false;
}

// func_800A0FB4, around building the race's finish overlay (each player's place): its widgets are laid out like the
// HUD's. At the end, $v0 = the overlay's element list.
void rush2_hud_finish_begin(uint8_t* rdram, recomp_context* ctx) {
    building_hud = true;
    rush2::players4::finish_boxes(rdram, ctx);
}

void rush2_hud_finish_end(uint8_t* rdram, recomp_context* ctx) {
    rush2::players4::finish_overlay_built(rdram, ctx, (uint32_t)ctx->r2);
    building_hud = false;
}

// func_800606A8 (high-score screen), around its widgets_create (0x80060774). The race HUD setup's first widget table
// (0x800C3F34) has func_800BAD44 as its callback, which widgets_create runs at once: it opens the high-score screen's
// first page from inside the HUD setup, and those widgets were anchored like the HUD's (the rank column pinned to the
// left edge, away from the names and times). Pages opened later are built from the widget loop and never were. The
// screen is laid out as a whole, so its widgets are never the HUD's.
namespace {
    bool highscore_saved_building = false;
}

void rush2_hud_highscore_create_begin(uint8_t* rdram, recomp_context* ctx) {
    highscore_saved_building = building_hud;
    building_hud = false;
}

void rush2_hud_highscore_create_end(uint8_t* rdram, recomp_context* ctx) {
    building_hud = highscore_saved_building;
}

// func_800541F8 (widget allocator), after the widget is initialized. $v1 = slot.
void rush2_hud_widget_created(uint8_t* rdram, recomp_context* ctx) {
    uint32_t slot = (uint32_t)ctx->r3;
    if (slot < max_widgets) {
        hud_slot[slot] = building_hud;
        slot_shift_x[slot] = 0;
        slot_shift_y[slot] = 0;
        slot_identity[slot] = catalog::Identity{};
        slot_reported[slot] = false;
        slots_dirty = true;
    }
}

// func_80093B38 entry (builds a box: x0, y0, x1, y1 in $a0-$a3, the alpha, callback, parameter and kept flag at
// $sp + 0x10 - 0x1C): the identity its five widgets get.
void rush2_hud_box_begin(uint8_t* rdram, recomp_context* ctx) {
    catalog::Identity& b = box_pending;
    b = catalog::Identity{};
    b.kind = catalog::Identity::Kind::Box;
    int32_t x0 = (int32_t)ctx->r4, y0 = (int32_t)ctx->r5, x1 = (int32_t)ctx->r6, y1 = (int32_t)ctx->r7;
    b.x = (int16_t)std::min(x0, x1);
    b.y = (int16_t)std::min(y0, y1);
    b.x1 = (int16_t)std::max(x0, x1);
    b.y1 = (int16_t)std::max(y0, y1);
    int32_t sp = (int32_t)ctx->r29;
    b.callback = (uint32_t)MEM_W(0x14, sp);
    b.param = (uint32_t)MEM_W(0x18, sp);
}

// func_80093B38 at 0x80093C20, just after building one of the box's five elements ($v0; $s2 = which: 0 the top edge,
// 1 the left edge, 2 the fill, 3 the right edge, 4 the bottom edge).
void rush2_hud_box_part(uint8_t* rdram, recomp_context* ctx) {
    int32_t element = (int32_t)ctx->r2;
    if (element == 0) {
        return;
    }
    int slot = MEM_H(0x34, element);
    if (slot >= 0 && slot < (int)max_widgets) {
        slot_identity[slot] = box_pending;
        slot_identity[slot].part = (int8_t)ctx->r18;
        slots_dirty = true;
        // The finish overlay builds its boxes (GAME OVER's, the places') before its widgets count as the HUD's
        // (rush2_hud_finish_begin): a box the race's catalog knows is the HUD's, so it moves with its element.
        if (!hud_slot[slot] && layout_solved && catalog::find(frame_output.parts, slot_identity[slot]) >= 0) {
            hud_slot[slot] = true;
        }
    }
}

// func_800604FC (builds the elements of a layout table) at 0x80060594, just after building one ($s0 the entry, $v0
// the element, whose +0x34 is its widget slot) and before the entry's callback first runs: the widget's identity.
void rush2_hud_entry_created(uint8_t* rdram, recomp_context* ctx) {
    int32_t entry = (int32_t)ctx->r16;
    int32_t element = (int32_t)ctx->r2;
    if (element == 0) {
        return;
    }
    int slot = MEM_H(0x34, element);
    if (slot < 0 || slot >= (int)max_widgets) {
        return;
    }
    catalog::Identity& id = slot_identity[slot];
    id = catalog::Identity{};
    id.kind = catalog::Identity::Kind::Entry;
    uint32_t name = (uint32_t)MEM_W(0, entry);
    if (name == 0xFFFFFFFFu) {
        std::snprintf(id.name, sizeof(id.name), "-1");
    }
    else {
        for (int i = 0; name != 0 && is_rdram(name) && i < (int)sizeof(id.name) - 1; i++) {
            char ch = (char)MEM_B(i, (int32_t)name);
            if (ch == 0) break;
            id.name[i] = ch;
        }
    }
    id.x = MEM_H(4, entry);
    id.y = MEM_H(6, entry);
    id.callback = (uint32_t)MEM_W(0x20, entry);
    id.param = (uint32_t)MEM_W(0x24, entry);
    slots_dirty = true;
}

namespace {
    // Each player's Rush 2049 odometer (src/rush2049/odometer.cpp) as the layout placed it this frame: its top left
    // corner and origin (rush2_hud_draw_begin).
    struct OdometerPlace {
        bool valid;
        int32_t x, y;
        uint16_t origin;
        float scale;
    };
    OdometerPlace odometer_place[4];
}

// The layout engine's input (include/hud_layout.h): the race's settings, read from the game. Nothing about the widgets
// goes in: where each can be is the HUD catalog's (src/hud_catalog.cpp).
namespace {
    static_assert(layout::origin_none == G_EX_ORIGIN_NONE && layout::origin_right == G_EX_ORIGIN_RIGHT &&
                  layout::origin_center == G_EX_ORIGIN_CENTER, "the layout's origins are RT64's");

    constexpr uint32_t game_clock = 0x80117488; // f32
    constexpr uint32_t race_type_word = 0x8010C3E8;         // s32: 0 race, 1 circuit, 2 stunt, 3 practice
    constexpr uint32_t stunts_all_tracks_byte = 0x8010C200; // the Stunts on All Tracks cheat
    constexpr uint32_t race_game_mode_byte = 0x800FAE6C;    // nonzero in the attract demo
    constexpr uint32_t ui_language = 0x800C4618;            // s32: 0 English, 1 French, 2 German
    constexpr uint32_t race_deaths = 0x8010BC98;            // s8: the DEATHS option (the skull)
    constexpr uint32_t race_mode_option = 0x8010C169;       // u8: the stunt mode's option (the IT badge)

    // HUD mods: the first enabled mod (in load order) holding rush2/hud_placements.inc (the HUD editor's placements,
    // src/hud_overrides.inc's format) puts its placements over the built-in ones (merge_overrides). Checked every 60
    // frames: mods turned on or off, and the file's time (a folder mod's file, else the .rtz), so a placement saved
    // from the editor shows in a running game.
    constexpr const char* hud_mod_file = "rush2/hud_placements.inc";
    std::vector<layout::Override> mod_overrides;
    bool mod_overrides_on = false;
    std::string mod_signature;
    int mod_check_frames = 0;

    // Whether the mods' placements changed (the layout is solved again then).
    bool refresh_mod_overrides() {
        if (mod_check_frames-- > 0) return false;
        mod_check_frames = 60;
        std::string signature;
        std::vector<rush2::mod_files::ModFile> mods = rush2::mod_files::enabled_mods(signature);
        for (const rush2::mod_files::ModFile& m : mods) {
            std::error_code ec;
            std::filesystem::path file = std::filesystem::is_directory(m.path, ec)
                ? m.path / std::filesystem::u8path(hud_mod_file) : m.path;
            auto time = std::filesystem::last_write_time(file, ec);
            signature += ec ? std::string("-") : std::to_string(time.time_since_epoch().count());
            signature += ";";
        }
        if (signature == mod_signature) return false;
        mod_signature = signature;
        bool was_on = mod_overrides_on;
        mod_overrides_on = false;
        for (const rush2::mod_files::ModFile& m : mods) {
            std::vector<uint8_t> data;
            if (!rush2::mod_files::read_mod_file(m, hud_mod_file, data)) continue;
            std::vector<layout::Override> list;
            std::string error;
            if (!layout::parse_overrides(std::string(data.begin(), data.end()), m.mod_id + "/" + hud_mod_file, list, error)) {
                fprintf(stderr, "[hud] mod placements not used: %s\n", error.c_str());
                break;
            }
            mod_overrides = layout::merge_overrides(layout::builtin_overrides(), list);
            mod_overrides_on = true;
            printf("[hud] %zu placements from mod %s\n", list.size(), m.mod_id.c_str());
            fflush(stdout);
            break;
        }
        return mod_overrides_on || was_on;
    }

    layout::Config collect_config(uint8_t* rdram) {
        layout::Config c;
        int quads = rush2::splitscreen::quadrant_views(rdram);
        // Split, the game lays the HUD out for two stacked views (src/players4.cpp); 3-4 players are the quadrants.
        c.players = quads != 0 ? quads : std::clamp<int>(MEM_H(0, (int32_t)rush2::players4::num_views), 1, 2);
        c.arrangement = quads != 0 ? layout::Arrangement::Quadrants
            : rush2::splitscreen::is_side_by_side(rdram) ? layout::Arrangement::SideBySide : layout::Arrangement::Single;
        c.hud_width = rush2::splitscreen::hud_width();
        c.widescreen = ultramodern::renderer::get_graphics_config().ar_option != ultramodern::renderer::AspectRatio::Original;
        c.language = std::clamp<int>(MEM_W(0, (int32_t)ui_language), 0, 2);
        c.odometer = rush2::odometer2049::enabled();
        c.odometer_h = rush2::odometer2049::height;
        c.odometer_w = rush2::odometer2049::width;
        c.odometer_gap = rush2::odometer2049::gap;
        c.small_scale = fitting_scale();
        c.stunt_arena = MEM_B(0, (int32_t)stunt_track_id) == stunt_track;
        c.race_type = MEM_W(0, (int32_t)race_type_word);
        c.battle = rush2::track2049::battle_race(rdram);
        c.battle_hud = rush2::battle::hud_mode();
        c.battle_time_limit = rush2::battle::time_limit_minutes() > 0;
        c.stunts_all_tracks = MEM_B(0, (int32_t)stunts_all_tracks_byte) != 0;
        c.deaths = MEM_B(0, (int32_t)race_deaths) != 0;
        c.mode_option = MEM_BU(0, (int32_t)race_mode_option) != 0;
        c.attract = MEM_BU(0, (int32_t)race_game_mode_byte) != 0;
        auto setting = [&](uint32_t address) { return MEM_BU(0, (int32_t)address) != 0; };
        c.shown.time = setting(0x8010C3F1);
        c.shown.speed = setting(0x8010C434);
        c.shown.position = setting(0x8010C441);
        c.shown.radar = setting(0x8010C3EE);
        c.shown.map = setting(0x8010C3E4);
        c.shown.time_left = setting(0x8010C3F2) && (!setting(0x800D9E88) || c.battle);
        c.shown.tach = setting(0x8010C425);
        c.shown.gear = setting(0x8010C44A);
        c.shown.lap_banner = setting(0x8010C452);
        for (int p = 0; p < 4; p++) {
            c.manual[p] = MEM_BU(0x20, rush2::players4::record(p)) == 0;
        }
        return c;
    }

    // Each race HUD widget's catalog part in frame_output.parts.
    void match_slots() {
        for (uint32_t i = 0; i < max_widgets; i++) {
            slot_part[i] = hud_slot[i] ? (int16_t)catalog::find(frame_output.parts, slot_identity[i]) : (int16_t)-1;
        }
        slots_dirty = false;
    }

    // A widget's rectangle as the game draws it (before the layout moves it), or an invalid one (a callback widget,
    // which only prints).
    layout::Rect widget_rect(uint8_t* rdram, int32_t widget) {
        if ((MEM_BU(widget_flags, widget) & flag_callback) != 0 || MEM_H(widget_w, widget) == 0 || MEM_H(widget_h, widget) == 0) {
            return layout::Rect{};
        }
        int32_t x = MEM_H(widget_x, widget), y = MEM_H(widget_y, widget);
        if (MEM_W(widget_image, widget) == 0) {
            return layout::Rect{ x, y, x + MEM_H(widget_w, widget) - 1, y + MEM_H(widget_h, widget) - 1 };
        }
        return layout::Rect{ x, y, x + MEM_H(widget_src_x1, widget) - MEM_H(widget_src_x0, widget),
                             y + MEM_H(widget_src_y1, widget) - MEM_H(widget_src_y0, widget) };
    }

    // The catalog's check in the game: a race HUD widget shown with no catalog part, drawn outside its part's reach,
    // or of a part the catalog says can't appear in this race means the catalog (and so the layout and the previewer)
    // is missing something. Logged once per widget; the widget is still drawn (uncataloged: anchored by its screen
    // third, or with the element it's over).
    void check_widgets(uint8_t* rdram, uint32_t count) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t widget = (int32_t)(widget_array + i * widget_size);
            if (!hud_slot[i] || slot_reported[i] || MEM_B(widget_hidden, widget) != 0 || managed_scale_x[i] != 0.0f ||
                slot_shift_x[i] != 0 || slot_shift_y[i] != 0 || is_screen_fill(rdram, widget)) {
                continue;
            }
            layout::Rect r = widget_rect(rdram, widget);
            if (!r.valid()) {
                continue;
            }
            const catalog::Identity& id = slot_identity[i];
            const char* problem = nullptr;
            char where[96] = "";
            if (slot_part[i] < 0) {
                problem = "has no HUD catalog part";
            }
            else {
                const catalog::Part& p = frame_output.parts[slot_part[i]];
                if (!p.present) {
                    problem = "is of a part the HUD catalog says can't appear in this race";
                }
                else if (p.kind != catalog::Kind::Text && !p.reach.contains(r)) {
                    problem = "is drawn outside its HUD catalog part's reach";
                    std::snprintf(where, sizeof(where), " (reach (%d,%d)-(%d,%d))", p.reach.x0, p.reach.y0, p.reach.x1, p.reach.y1);
                }
            }
            if (problem == nullptr) {
                continue;
            }
            slot_reported[i] = true;
            std::fprintf(stderr, "[hud] widget %u (%s %s, callback %08X, parameter %08X, built at (%d,%d) part %d) %s: drawn at "
                         "(%d,%d)-(%d,%d)%s. Add it to src/hud_catalog.cpp (docs/hud_layout.md#catalog).\n", i,
                         id.kind == catalog::Identity::Kind::Box ? "box" : id.kind == catalog::Identity::Kind::Entry ? "entry" : "widget",
                         id.name, id.callback, id.param, id.x, id.y, id.part, problem, r.x0, r.y0, r.x1, r.y1, where);
        }
    }
}

// AI-agent test aid, not in the menu: RUSH2_HUD_CAPTURE=<folder> records the race HUD's layout input (every widget,
// what has been seen of it, the settings), the layout's output and the text the HUD prints, every
// RUSH2_HUD_CAPTURE_EVERY frames (default 10), as JSON lines in <folder>/frames.jsonl, and each image a widget draws
// as raw RGBA in <folder>/tex/. tools/hud_preview reads them (docs/hud_layout.md#captures).
namespace {
    const char* const capture_dir = std::getenv("RUSH2_HUD_CAPTURE");
    const int capture_every = [] {
        const char* v = std::getenv("RUSH2_HUD_CAPTURE_EVERY");
        return v != nullptr && std::atoi(v) > 0 ? std::atoi(v) : 10;
    }();
    FILE* capture_file = nullptr;
    int capture_frame = 0;
    bool capture_now = false;
    std::string capture_prints;
    std::vector<std::string> capture_textures;

    // A texel of an image as 0xRRGGBBAA.
    uint32_t texel_rgba(uint8_t* rdram, const ImageInfo& info, int32_t index) {
        auto rgba16 = [](uint32_t c) {
            uint32_t r = (c >> 11) & 0x1F, g = (c >> 6) & 0x1F, b = (c >> 1) & 0x1F;
            return ((r * 255 / 31) << 24) | ((g * 255 / 31) << 16) | ((b * 255 / 31) << 8) | ((c & 1) ? 0xFF : 0);
        };
        auto nibble = [&](uint32_t pixels) {
            return (uint32_t)(MEM_BU(0, (int32_t)(pixels + index / 2)) >> ((index & 1) ? 0 : 4)) & 0xF;
        };
        auto gray = [](uint32_t i, uint32_t a) {
            return (i << 24) | (i << 16) | (i << 8) | a;
        };
        switch (info.fmt) {
            case 0:
                if (info.siz == 3) {
                    return (uint32_t)MEM_W(index * 4, (int32_t)info.pixels);
                }
                return rgba16(MEM_HU(index * 2, (int32_t)info.pixels));
            case 2: {
                int32_t i = info.siz == 0 ? (int32_t)nibble(info.pixels) : (int32_t)MEM_BU(index, (int32_t)info.pixels);
                return info.palette == 0 ? gray(i, 0xFF) : rgba16(MEM_HU(i * 2, (int32_t)info.palette));
            }
            case 3:
                if (info.siz == 0) {
                    uint32_t n = nibble(info.pixels);
                    return gray((n >> 1) * 255 / 7, (n & 1) ? 0xFF : 0);
                }
                if (info.siz == 1) {
                    uint32_t b = MEM_BU(index, (int32_t)info.pixels);
                    return gray((b >> 4) * 17, (b & 0xF) * 17);
                }
                return gray(MEM_BU(index * 2, (int32_t)info.pixels), MEM_BU(index * 2 + 1, (int32_t)info.pixels));
            case 4:
                if (info.siz == 0) {
                    uint32_t n = nibble(info.pixels) * 17;
                    return gray(n, n);
                }
                return gray(MEM_BU(index, (int32_t)info.pixels), MEM_BU(index, (int32_t)info.pixels));
            case 5: {
                // The game's own format 5 (the HUD's big digits): an intensity the draw tints with the primitive
                // color; texels of 0 are transparent.
                uint32_t i = info.siz == 0 ? nibble(info.pixels) * 17 : MEM_BU(index, (int32_t)info.pixels);
                return gray(i, i != 0 ? 0xFF : 0);
            }
            default:
                return 0xFF00FFFF;
        }
    }

    // Saves the texels a widget draws (its source rectangle) once per image, palette and rectangle; returns the
    // file's name in tex/, or "" if it can't be read.
    std::string capture_texture(uint8_t* rdram, int32_t widget) {
        ImageInfo info;
        if (!image_info(rdram, widget, info)) {
            return "";
        }
        int32_t sx0 = MEM_H(widget_src_x0, widget), sy0 = MEM_H(widget_src_y0, widget);
        int32_t w = MEM_H(widget_src_x1, widget) - sx0 + 1, h = MEM_H(widget_src_y1, widget) - sy0 + 1;
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
            return "";
        }
        char name[128];
        std::snprintf(name, sizeof(name), "%08X_%08X_f%ds%d_%d_%d_%dx%d.rgba", (uint32_t)MEM_W(widget_image, widget),
                      info.palette, info.fmt, info.siz, sx0, sy0, w, h);
        if (std::find(capture_textures.begin(), capture_textures.end(), name) != capture_textures.end()) {
            return name;
        }
        capture_textures.push_back(name);
        std::vector<uint8_t> px((size_t)w * h * 4);
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                uint32_t c = sy0 + y < info.height ? texel_rgba(rdram, info, info.index(sx0 + x, sy0 + y)) : 0;
                uint8_t* o = &px[((size_t)y * w + x) * 4];
                o[0] = (uint8_t)(c >> 24);
                o[1] = (uint8_t)(c >> 16);
                o[2] = (uint8_t)(c >> 8);
                o[3] = (uint8_t)c;
            }
        }
        std::string path = std::string(capture_dir) + "/tex/" + name;
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(px.data(), 1, px.size(), f);
            std::fclose(f);
        }
        return name;
    }

    std::string json_rect(const layout::Rect& r) {
        if (!r.valid()) {
            return "null";
        }
        char b[64];
        std::snprintf(b, sizeof(b), "[%d,%d,%d,%d]", r.x0, r.y0, r.x1, r.y1);
        return b;
    }

    std::string json_string(const std::string& s) {
        std::string o = "\"";
        for (char ch : s) {
            if (ch == '"' || ch == '\\') {
                o += '\\';
                o += ch;
            }
            else if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7F) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", (unsigned char)ch);
                o += b;
            }
            else {
                o += ch;
            }
        }
        return o + "\"";
    }

    // Starts this frame's capture (rush2_hud_draw_begin, before the layout moves anything).
    void capture_begin(uint8_t* rdram) {
        capture_now = false;
        if (capture_dir == nullptr) {
            return;
        }
        if (capture_file == nullptr) {
            std::string tex = std::string(capture_dir) + "/tex";
            std::filesystem::create_directories(tex);
            capture_file = std::fopen((std::string(capture_dir) + "/frames.jsonl").c_str(), "ab");
            if (capture_file == nullptr) {
                return;
            }
        }
        // Only frames with the race HUD up.
        bool hud = false;
        for (uint32_t i = 0; i < std::min<uint32_t>((uint32_t)MEM_W(0, (int32_t)widget_count), max_widgets); i++) {
            hud = hud || hud_slot[i];
        }
        if (!hud || capture_frame++ % capture_every != 0) {
            return;
        }
        capture_now = true;
    }

    // A string a HUD callback printed: where the game put it and where it was drawn.
    // The laps-left number is printed before the widget loop (rush2_hud_laps_begin): the last one is kept.
    std::string capture_laps;
    void capture_print(uint8_t* rdram, int32_t x, int32_t y, uint32_t text, int32_t to_x, int32_t to_y, uint16_t origin,
                       const char* kind) {
        bool laps = std::strcmp(kind, "laps") == 0;
        if (capture_dir == nullptr || (!capture_now && !laps)) {
            return;
        }
        std::string& out = laps ? capture_laps : capture_prints;
        if (laps) {
            out.clear();
        }
        std::string s;
        for (int i = 0; text != 0 && is_rdram(text) && i < 64; i++) {
            char ch = (char)MEM_B(i, (int32_t)text);
            if (ch == 0) break;
            s += ch;
        }
        char b[160];
        std::snprintf(b, sizeof(b), "%s{\"x\":%d,\"y\":%d,\"to\":[%d,%d],\"origin\":%u,\"kind\":\"%s\",\"text\":",
                      out.empty() ? "" : ",", x, y, to_x, to_y, origin, kind);
        out += b + json_string(s) + "}";
    }

    // Writes this frame's record (rush2_hud_draw_end, before the widgets are put back).
    void capture_end(uint8_t* rdram) {
        if (!capture_now || capture_file == nullptr) {
            capture_prints.clear();
            return;
        }
        capture_now = false;
        const layout::Config& c = frame_output.config;
        std::string o;
        char b[640];
        float clock = std::bit_cast<float>(MEM_W(0, (int32_t)game_clock));
        std::snprintf(b, sizeof(b),
                      "{\"frame\":%d,\"clock\":%.3f,\"config\":{\"players\":%d,\"arrangement\":%d,\"hud_width\":%.3f,"
                      "\"widescreen\":%d,\"language\":%d,\"odometer\":%d,\"odometer_size\":[%d,%d,%d],\"small_scale\":%.4f,"
                      "\"stunt_arena\":%d,\"manual\":[%d,%d,%d,%d],\"race_type\":%d,\"battle\":%d,\"stunts_all_tracks\":%d,"
                      "\"deaths\":%d,\"mode_option\":%d,\"attract\":%d,\"battle_hud\":%d,\"battle_time_limit\":%d,"
                      "\"shown\":[%d,%d,%d,%d,%d,%d,%d,%d,%d],\"track\":%d},"
                      "\"widgets\":[",
                      capture_frame - 1, clock, c.players, (int)c.arrangement, c.hud_width, c.widescreen, c.language, c.odometer,
                      c.odometer_w, c.odometer_h, c.odometer_gap, c.small_scale, c.stunt_arena, c.manual[0], c.manual[1],
                      c.manual[2], c.manual[3], c.race_type, c.battle, c.stunts_all_tracks, c.deaths, c.mode_option, c.attract, c.battle_hud, c.battle_time_limit,
                      c.shown.time, c.shown.speed, c.shown.position, c.shown.radar, c.shown.map, c.shown.time_left, c.shown.tach,
                      c.shown.lap_banner, c.shown.gear, MEM_B(0, (int32_t)stunt_track_id));
        o += b;
        bool first = true;
        uint32_t count = std::min<uint32_t>((uint32_t)MEM_W(0, (int32_t)widget_count), max_widgets);
        for (uint32_t i = 0; i < count; i++) {
            int32_t widget = (int32_t)(widget_array + i * widget_size);
            // Where the game put it: the layout's move is undone.
            layout::Rect r = widget_rect(rdram, widget);
            if (r.valid()) {
                r = r.moved(-slot_dx[i], -slot_dy[i]);
            }
            int32_t x = MEM_H(widget_x, widget) - slot_dx[i], y = MEM_H(widget_y, widget) - slot_dy[i];
            uint32_t image = (MEM_BU(widget_flags, widget) & flag_callback) != 0 ? 0 : (uint32_t)MEM_W(widget_image, widget);
            std::string tex = image != 0 && hud_slot[i] ? capture_texture(rdram, widget) : "";
            std::snprintf(b, sizeof(b),
                          "%s{\"slot\":%u,\"part\":%d,\"image\":\"%08X\",\"shown\":%d,\"hud\":%d,\"managed\":%d,"
                          "\"shifted\":%d,\"screen_fill\":%d,\"xy\":[%d,%d],\"rect\":",
                          first ? "" : ",", i, slot_part[i], image, MEM_B(widget_hidden, widget) == 0 || stunt_hidden[i],
                          hud_slot[i], managed_scale_x[i] != 0.0f, slot_shift_x[i] != 0 || slot_shift_y[i] != 0,
                          is_screen_fill(rdram, widget), x, y);
            o += b + json_rect(r) + ",\"tex\":" + json_string(tex);
            // How it was built (its catalog identity) and its source rectangle.
            const catalog::Identity& id = slot_identity[i];
            std::snprintf(b, sizeof(b), ",\"id\":{\"kind\":%d,\"xy\":[%d,%d,%d,%d],\"cb\":\"%08X\",\"p\":\"%08X\","
                          "\"part\":%d,\"src\":[%d,%d,%d,%d],\"name\":",
                          (int)id.kind, id.x, id.y, id.x1, id.y1, id.callback, id.param, id.part, MEM_H(widget_src_x0, widget),
                          MEM_H(widget_src_y0, widget), MEM_H(widget_src_x1, widget), MEM_H(widget_src_y1, widget));
            o += b + json_string(id.name) + "}";
            // The layout's place for it, as applied.
            std::snprintf(b, sizeof(b),
                          ",\"out\":{\"anchored\":%d,\"dx\":%d,\"dy\":%d,\"origin\":%u,\"scale\":%.4f,\"pivot\":[%d,%d],"
                          "\"stunt_hidden\":%d,\"managed_origin\":%u}}",
                          slot_anchored[i], slot_dx[i], slot_dy[i], slot_origin[i], slot_scale[i], slot_draw_pivot_x[i],
                          slot_draw_pivot_y[i], stunt_hidden[i], managed_origin[i]);
            o += b;
            first = false;
        }
        o += "],\"odometer\":[";
        for (int p = 0; p < 4; p++) {
            const OdometerPlace& d = odometer_place[p];
            std::snprintf(b, sizeof(b), "%s{\"valid\":%d,\"x\":%d,\"y\":%d,\"origin\":%u,\"scale\":%.4f}", p ? "," : "",
                          d.valid, d.x, d.y, d.origin, d.scale);
            o += b;
        }
        o += "],\"panel_rows\":[";
        for (int p = 0; p < 4; p++) {
            const layout::PanelRow& r = frame_output.panel_rows[p];
            std::snprintf(b, sizeof(b), "%s{\"valid\":%d,\"time\":[%d,%d,%d],\"place\":[%d,%d,%d],\"y\":[%d,%d]}", p ? "," : "",
                          r.valid, r.time_x0, r.time_x1, r.time_y0, r.place_x0, r.place_x1, r.place_y0, r.y0, r.y1);
            o += b;
        }
        std::snprintf(b, sizeof(b), "],\"map\":{\"known\":%d,\"dx\":%d,\"dy\":%d,\"origin\":%u},\"prints\":[", map_known, map_dx,
                      map_dy, map_origin);
        std::string prints = capture_laps;
        if (!prints.empty() && !capture_prints.empty()) {
            prints += ",";
        }
        o += b + prints + capture_prints + "]}\n";
        capture_prints.clear();
        std::fwrite(o.data(), 1, o.size(), capture_file);
        std::fflush(capture_file);
    }
}

namespace {
    // AI-agent test aid, not in the menu: RUSH2_TEST_BONUS_TIME=<seconds> keeps the bonus time banner up from that game
    // clock time on, to check the HUD around it without driving to a checkpoint. The banner (func_80060F24) shows
    // while its end time 0x8010C3C4 (set by func_8008F220 on a checkpoint) is over a second ahead of the clock.
    const double test_bonus_time = [] {
        const char* v = std::getenv("RUSH2_TEST_BONUS_TIME");
        return v != nullptr ? std::atof(v) : 0.0;
    }();
    constexpr uint32_t bonus_time_end = 0x8010C3C4; // f32
}

// func_8007D9DC, before the widget draw loop: lays out the race HUD (src/hud_layout.cpp, docs/hud_layout.md) and
// moves its widgets for the draw loop.
void rush2_hud_draw_begin(uint8_t* rdram, recomp_context* ctx) {
    if (test_bonus_time > 0.0) {
        float clock = std::bit_cast<float>(MEM_W(0, (int32_t)game_clock));
        if (clock >= test_bonus_time) {
            MEM_W(0, (int32_t)bonus_time_end) = std::bit_cast<int32_t>(clock + 3.0f);
        }
    }
    if (refresh_mod_overrides()) layout_solved = false;
    layout::Config config = collect_config(rdram);
    config.overrides = mod_overrides_on ? &mod_overrides : nullptr;
    if (!layout_solved || !(config == frame_output.config)) {
        frame_output = layout::solve(config);
        layout_solved = true;
        slots_dirty = true;
    }
    if (slots_dirty) {
        match_slots();
    }
    capture_begin(rdram);
    quad_views = config.arrangement == layout::Arrangement::Quadrants ? config.players : 0;
    bool split = frame_output.split;
    uint32_t count = std::min<uint32_t>((uint32_t)MEM_W(0, (int32_t)widget_count), max_widgets);
    check_widgets(rdram, count);

    // Each widget's place: its part's, or for a race HUD widget the catalog doesn't know, the element it's over (or
    // its screen third), as text is placed.
    for (uint32_t i = 0; i < max_widgets; i++) {
        slot_origin[i] = G_EX_ORIGIN_NONE;
        slot_dx[i] = 0;
        slot_dy[i] = 0;
        slot_scale[i] = 0.0f;
        slot_anchored[i] = false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!hud_slot[i]) {
            continue;
        }
        if (slot_part[i] >= 0) {
            const layout::Place& p = frame_output.places[slot_part[i]];
            if (!p.placed) {
                continue;
            }
            slot_anchored[i] = true;
            slot_origin[i] = p.origin;
            slot_dx[i] = (int16_t)p.dx;
            slot_dy[i] = (int16_t)p.dy;
            slot_scale[i] = p.scale;
            slot_draw_pivot_x[i] = p.pivot_x;
            slot_draw_pivot_y[i] = p.pivot_y;
            continue;
        }
        // Widgets another file places in final screen coordinates (the battle HUD's bar, set_widget_scale; the
        // shifted ones, set_widget_shift) stay where it put them.
        if (managed_scale_x[i] != 0.0f || slot_shift_x[i] != 0 || slot_shift_y[i] != 0) {
            continue;
        }
        int32_t widget = (int32_t)(widget_array + i * widget_size);
        layout::Rect r = widget_rect(rdram, widget);
        if (r.valid()) {
            layout::TextPlace t = layout::place_text(frame_output, (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2);
            slot_anchored[i] = true;
            slot_origin[i] = t.origin;
            slot_dx[i] = (int16_t)t.dx;
            slot_dy[i] = (int16_t)t.dy;
        }
    }
    part_shown.assign(frame_output.parts.size(), false);
    for (uint32_t i = 0; i < count; i++) {
        if (slot_part[i] >= 0 && MEM_B(widget_hidden, (int32_t)(widget_array + i * widget_size)) == 0) {
            part_shown[slot_part[i]] = true;
        }
    }
    // Text parts (the stunt panel's) have no widget: they show with their element's widgets.
    for (size_t k = 0; k < frame_output.parts.size(); k++) {
        const catalog::Part& p = frame_output.parts[k];
        int e = frame_output.places[k].element;
        if (p.kind != catalog::Kind::Text || e < 0) continue;
        for (int other : frame_output.elements[e].parts) {
            part_shown[k] = part_shown[k] || part_shown[other];
        }
    }
    // The odometer is drawn while its player's race time shows (not over the attract demo or the best averages after a
    // race); its place is kept either way. A player with no race time (players 2-4 on the stunt track) goes with any
    // player's.
    bool time_shown[4] = {};
    bool any_time = false;
    for (uint32_t i = 0; i < count; i++) {
        if (slot_part[i] >= 0 && MEM_B(widget_hidden, (int32_t)(widget_array + i * widget_size)) == 0) {
            const catalog::Part& part = frame_output.parts[slot_part[i]];
            if (part.role == layout::Role::Time && part.player >= 0 && part.player < 4) {
                time_shown[part.player] = true;
                any_time = true;
            }
        }
    }
    for (int p = 0; p < 4; p++) {
        const layout::OdometerPlace& o = frame_output.odometer[p];
        int t = frame_output.element(layout::Role::Time, p);
        bool own_time = t >= 0 && frame_output.elements[t].possible;
        odometer_place[p] = OdometerPlace{ o.valid && (own_time ? time_shown[p] : any_time), o.x, o.y, o.origin, o.scale };
    }

    // Each player's stunt boxes where the game lays them out, and where their text goes.
    for (int p = 0; p < 4; p++) {
        for (int k = 0; k < 2; k++) {
            stunt_box[p][k] = Rect{ INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, false, false };
            stunt_text[p][k].valid = false;
        }
        stunt_swap[p] = frame_output.stunt_swap[p];
    }
    for (const layout::Element& e : frame_output.elements) {
        int k = e.role == layout::Role::StuntScore ? 0 : e.role == layout::Role::StuntTrick ? 1 : -1;
        if (k < 0 || e.player < 0 || e.player > 3) {
            continue;
        }
        stunt_box[e.player][k] = Rect{ e.game.x0, e.game.y0, e.game.x1, e.game.y1, true, false };
        int32_t dx = e.x - e.game.x0, dy = e.y - e.game.y0;
        if (e.possible && e.spot >= 0 && (quad_views || dx != 0 || dy != 0 || e.scale != 1.0f)) {
            StuntText& t = stunt_text[e.player][k];
            t.valid = true;
            t.dx = dx;
            t.dy = dy;
            t.pivot_x = e.x;
            t.pivot_y = e.y;
            t.origin = e.origin;
            t.scale = e.scale;
        }
    }

    // The finish flag and laps left's move (else the track map's), for the laps-left number printed with it.
    map_known = false;
    int flag = frame_output.element(layout::Role::LapsLeft, -1);
    if (flag < 0 || !frame_output.elements[flag].possible) {
        flag = frame_output.map;
    }
    if (split && flag >= 0) {
        const layout::Element& m = frame_output.elements[flag];
        if (m.possible && m.spot >= 0) {
            map_known = true;
            map_dx = m.x - m.game.x0;
            map_dy = m.y - m.game.y0;
            map_scale = m.scale;
            map_x0 = m.game.x0;
            map_y0 = m.game.y0;
            map_origin = m.origin;
        }
    }

    if (split && !inset_lifted) {
        saved_inset_x = MEM_H(0, (int32_t)clip_inset_x);
        saved_inset_y = MEM_H(0, (int32_t)clip_inset_y);
        // Past the screen's edges: the half size skull is clipped at its full size before it's scaled.
        MEM_H(0, (int32_t)clip_inset_x) = -lifted_inset;
        MEM_H(0, (int32_t)clip_inset_y) = -lifted_inset;
        inset_lifted = true;
    }

    // Widgets hidden for the draw loop: the stunt scores' box while the trick name's shows in its place.
    bool trick_shown[4] = {};
    for (uint32_t i = 0; i < count; i++) {
        if (slot_part[i] < 0 || MEM_B(widget_hidden, (int32_t)(widget_array + i * widget_size)) != 0) {
            continue;
        }
        const catalog::Part& part = frame_output.parts[slot_part[i]];
        if (part.role == layout::Role::StuntTrick && part.player >= 0 && part.player < 4) {
            trick_shown[part.player] = true;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        int32_t widget = (int32_t)(widget_array + i * widget_size);
        if (slot_part[i] < 0 || MEM_B(widget_hidden, widget) != 0) {
            continue;
        }
        const catalog::Part& part = frame_output.parts[slot_part[i]];
        // Hidden by a hand placement (the HUD editor's), or the scores' box under its trick name.
        if (frame_output.places[slot_part[i]].hidden ||
            (part.role == layout::Role::StuntScore && part.player >= 0 && part.player < 4 && stunt_swap[part.player] &&
             trick_shown[part.player])) {
            MEM_B(widget_hidden, widget) = 1;
            stunt_hidden[i] = true;
        }
    }
    // From here on: the scores' box is hidden for its trick name's this frame (its text isn't drawn either).
    for (int p = 0; p < 4; p++) {
        stunt_swap[p] = stunt_swap[p] && trick_shown[p];
    }

    // Move the widgets for the draw loop.
    for (uint32_t i = 0; i < count; i++) {
        if (slot_dx[i] != 0 || slot_dy[i] != 0) {
            int32_t widget = (int32_t)(widget_array + i * widget_size);
            saved_x[i] = MEM_H(widget_x, widget);
            saved_y[i] = MEM_H(widget_y, widget);
            MEM_H(widget_x, widget) = (int16_t)(saved_x[i] + slot_dx[i]);
            MEM_H(widget_y, widget) = (int16_t)(saved_y[i] + slot_dy[i]);
            slot_moved[i] = true;
        }
    }

    half_pending = false;
    current_origin = G_EX_ORIGIN_NONE;
    scissor_widened = false;
}

// func_8007D9DC, at the top of the widget draw loop. $fp = widget.
void rush2_hud_draw_widget(uint8_t* rdram, recomp_context* ctx) {
    int32_t widget = (int32_t)ctx->r30;
    uint32_t slot = ((uint32_t)widget - widget_array) / widget_size;
    in_hud_callback = false;
    finish_half(rdram);
    finish_map(rdram);
    if (slot < max_widgets && MEM_B(widget_hidden, widget) == 0 && slot_part[slot] >= 0 &&
        frame_output.parts[slot_part[slot]].id.callback == track_map_callback) {
        map_pending = true;
        map_start = MEM_W(0, (int32_t)dl_2d_cursor);
    }
    if (slot >= max_widgets || MEM_B(widget_hidden, widget) != 0) {
        return;
    }
    // Banners and messages (BONUS TIME, GAME OVER, the place banners) go over the battle HUD: it's drawn before them.
    if (slot_part[slot] >= 0) {
        layout::Role role = frame_output.parts[slot_part[slot]].role;
        if (role == layout::Role::Message || role == layout::Role::Banner) {
            rush2::battle::hud_draw_views(rdram);
        }
    }

    in_hud_callback = hud_slot[slot] && (MEM_BU(widget_flags, widget) & flag_callback) != 0;
    if (is_screen_fill(rdram, widget)) {
        stretch_rect(rdram);
        return;
    }
    if (slot_shift_x[slot] != 0 || slot_shift_y[slot] != 0) {
        shift_rect(rdram, slot_shift_x[slot], slot_shift_y[slot]);
        return;
    }
    uint16_t origin = slot_origin[slot];
    if (slot_part[slot] >= 0 && slot_anchored[slot]) {
        // A sliding part (WRONG WAY with one player): its anchor follows it from where the slide starts to where it
        // ends, by where the game has it this frame.
        const catalog::Part& part = frame_output.parts[slot_part[slot]];
        if (part.end.valid()) {
            // Where the game has it: the widget was moved by (slot_dx, slot_dy) for the draw loop.
            origin = layout::slide_origin(part, frame_output.places[slot_part[slot]], MEM_H(widget_x, widget) - slot_dx[slot],
                                          MEM_H(widget_y, widget) - slot_dy[slot]);
        }
    }
    set_rect_origin(rdram, managed_scale_x[slot] != 0.0f ? managed_origin[slot] : origin);
    if (managed_scale_x[slot] != 0.0f) {
        if (MEM_W(widget_image, widget) != 0 && (managed_scale_x[slot] != 1.0f || managed_scale_y[slot] != 1.0f)) {
            half_pending = true;
            half_start = MEM_W(0, (int32_t)dl_2d_cursor);
            half_scale_x = managed_scale_x[slot];
            half_scale_y = managed_scale_y[slot];
        }
    }
    else if (slot_scale[slot] != 0.0f && (MEM_BU(widget_flags, widget) & flag_callback) == 0) {
        half_pending = true;
        half_start = MEM_W(0, (int32_t)dl_2d_cursor);
        half_scale_x = half_scale_y = slot_scale[slot];
        half_pivot = true;
        half_pivot_x = slot_draw_pivot_x[slot];
        half_pivot_y = slot_draw_pivot_y[slot];
    }
}

// func_800734E0 entry (prints a string at ($a0, $a1), justified around $a0). Anchors text printed by HUD callback
// widgets, and moves it side by side.
void rush2_hud_print(uint8_t* rdram, recomp_context* ctx) {
    // The stunt panel's text (func_800B7654, which prints every player's in a loop with the player in $s6; players 3 and
    // 4's at player 2's place): moved with the player's own box (the one the text is over: the trick name's on the
    // left, the scores' on the right) and drawn at its scale. The text is drawn at once (the callback sets the text
    // scale 0x800BEF48 to 0), so its rectangles are scaled when the next string or widget starts (finish_half).
    if (in_hud_callback && in_stunt_text) {
        int player = (int)ctx->r22;
        int32_t x = (int16_t)ctx->r4;
        int32_t y = (int16_t)ctx->r5;
        if (player >= 0 && player < 4) {
            int k = x < screen_width / 2 ? 1 : 0;
            for (int i = 0; i < 2; i++) {
                const Rect& b = stunt_box[player][i];
                if (valid(b) && x >= b.x0 - text_margin && x <= b.x1 + text_margin && y >= b.y0 - text_margin &&
                    y <= b.y1 + text_margin) {
                    k = i;
                }
            }
            const StuntText& t = stunt_text[player][k];
            if (t.valid && k == 0 && stunt_swap[player]) {
                // The scores' box is hidden while the trick name's shows in its place: its text goes off the screen.
                finish_half(rdram);
                capture_print(rdram, x, y, (uint32_t)ctx->r6, -2000, -2000, 0, "stunt");
                ctx->r4 = -2000;
                ctx->r5 = -2000;
                return;
            }
            if (t.valid) {
                finish_half(rdram);
                set_rect_origin(rdram, t.origin);
                capture_print(rdram, x, y, (uint32_t)ctx->r6, x + t.dx, y + t.dy, t.origin, "stunt");
                ctx->r4 = x + t.dx;
                ctx->r5 = y + t.dy;
                half_pending = true;
                half_start = MEM_W(0, (int32_t)dl_2d_cursor);
                half_scale_x = half_scale_y = t.scale;
                half_pivot = true;
                half_pivot_x = t.pivot_x;
                half_pivot_y = t.pivot_y;
                return;
            }
        }
    }
    if (in_hud_callback) {
        int32_t x = (int16_t)ctx->r4;
        int32_t y = (int16_t)ctx->r5;
        uint16_t origin = origin_at(x, y);
        capture_print(rdram, (int16_t)ctx->r4, (int16_t)ctx->r5, (uint32_t)ctx->r6, x, y, origin, "text");
        set_rect_origin(rdram, origin);
        ctx->r4 = x;
        ctx->r5 = y;
    }
}

// The 2D display list (the HUD, menus and text after the 3D views) is built in one of two buffers, alternating by
// frame: func_800B3A20 starts it (cursor 0x80125ABC = 0x8011CDB8 + frame * 0x4650) and func_800B39AC ends it and
// returns its start. 0x4650 bytes hold 2250 commands, and the second buffer ends where the HUD element pool's counts
// begin (0x80125A58): four players' HUDs with a battle, DEATHS, the odometer and the stunt panels outgrew it, and the
// overflow corrupted the element free list (a crash in hud_widget_free_8005FED8 when the pause menu was set up). The
// buffers move to 0x80AE0000, 0x10000 bytes each (the game heap's top is lowered to make room, src/assets.cpp).
namespace {
    constexpr uint32_t dl_2d_buffers = 0x80AE0000;
    constexpr uint32_t dl_2d_buffer_size = 0x10000;
    constexpr uint32_t dl_2d_frame = 0x80119850; // s32: which buffer (0 or 1)
}

// func_800B3A20 at 0x800B3AF4, after it sets the cursor to the frame's buffer ($t0 = the frame).
void rush2_hud_2d_list_start(uint8_t* rdram, recomp_context* ctx) {
    MEM_W(0, (int32_t)dl_2d_cursor) = (int32_t)(dl_2d_buffers + (uint32_t)ctx->r8 * dl_2d_buffer_size);
}

// func_800B39AC at its return (0x800B3A18): $v0 = the start of the frame's list.
void rush2_hud_2d_list_end(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (int32_t)(dl_2d_buffers + (uint32_t)MEM_W(0, (int32_t)dl_2d_frame) * dl_2d_buffer_size);
}

// func_800B7654 (the stunt panel's text callback) entry and return, around its prints (rush2_hud_print).
void rush2_hud_stunt_text(uint8_t* rdram, recomp_context* ctx, int inside) {
    in_stunt_text = inside != 0;
}

// func_8007D9DC, after the widget draw loop. Puts moved widgets back, and draws the menus' join hint for players 3 and 4.
void rush2_hud_draw_end(uint8_t* rdram, recomp_context* ctx) {
    in_hud_callback = false;
    finish_half(rdram);
    finish_map(rdram);
    rush2::battle::hud_draw(rdram, ctx);
    rush2::odometer2049::update(rdram);
    for (int p = 0; p < 4; p++) {
        const OdometerPlace& o = odometer_place[p];
        if (o.valid) {
            float anchor = o.origin < G_EX_ORIGIN_NONE ? o.origin / (float)G_EX_ORIGIN_RIGHT : 0.5f;
            rush2::odometer2049::draw(rdram, p, (float)o.x, (float)o.y, o.scale, anchor);
        }
    }
    rush2::stunt2049::draw_hud(rdram);
    rush2::pause_menu::draw(rdram, ctx);
    end_anchoring(rdram);
    capture_end(rdram);
    if (inset_lifted) {
        MEM_H(0, (int32_t)clip_inset_x) = saved_inset_x;
        MEM_H(0, (int32_t)clip_inset_y) = saved_inset_y;
        inset_lifted = false;
    }
    for (uint32_t i = 0; i < max_widgets; i++) {
        if (slot_moved[i]) {
            int32_t widget = (int32_t)(widget_array + i * widget_size);
            MEM_H(widget_x, widget) = saved_x[i];
            MEM_H(widget_y, widget) = saved_y[i];
            slot_moved[i] = false;
        }
        if (stunt_hidden[i]) {
            MEM_B(widget_hidden, (int32_t)(widget_array + i * widget_size)) = 0;
            stunt_hidden[i] = false;
        }
    }
    rush2::players4::draw_join_hint(rdram, ctx);
    rush2::look_back_draw_end(rdram);
}

// func_800B9D48, around its printf of the laps-left number at ($a0, $a1). The text sits on the track map (above its
// start), so it takes the map widget's origin (from the last widget loop), and split, moves with the map: it's
// printed outside the map's bounds, where it was matched to whichever element was nearest.
void rush2_hud_laps_begin(uint8_t* rdram, recomp_context* ctx) {
    int32_t x = (int16_t)ctx->r4;
    int32_t y = (int16_t)ctx->r5;
    uint16_t origin;
    if (map_known) {
        x = map_x0 + map_dx + (int32_t)std::lround((float)(x - map_x0) * map_scale);
        y = map_y0 + map_dy + (int32_t)std::lround((float)(y - map_y0) * map_scale);
        origin = map_origin;
    }
    else {
        origin = origin_at(x, y);
    }
    capture_print(rdram, (int16_t)ctx->r4, (int16_t)ctx->r5, (uint32_t)ctx->r6, x, y, origin, "laps");
    ctx->r4 = x;
    ctx->r5 = y;
    current_origin = G_EX_ORIGIN_NONE;
    scissor_widened = false;
    set_rect_origin(rdram, origin);
}

void rush2_hud_laps_end(uint8_t* rdram, recomp_context* ctx) {
    end_anchoring(rdram);
}

}
