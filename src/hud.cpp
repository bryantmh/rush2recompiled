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
// Side by side split screen (src/splitscreen.cpp). The 2 player HUD is laid out for views stacked top and bottom,
// each player's elements inside their half. While a race is drawn side by side, each of its element groups is moved
// for the draw loop from where it sits in its player's top or bottom half to the same place in their left or right half: horizontally by thirds of the half, an
// element near an edge keeping its distance from that edge and one in the middle staying centered, and vertically by
// the nearer edge unless it's close to the middle. Groups that cross between the stacked halves are shared (the
// track map) and are centered on the line between the side by side halves. Text printed by callbacks moves with the
// widget under it, or by its own position if there is none. In widescreen each half's elements are anchored to that
// half: those on its outer side to the window's edge, those in its middle to the middle of the half (an RT64 origin
// at a quarter of the window), and those on its inner side stay at the screen's center. Each player's time,
// speedometer, position, deaths skull and radar are laid out by place_role, the same margin from the screen's edges and
// the lines between the views, measured to their opaque texels (the panels and the track map have transparent edges);
// the shared time left sits centered above the track map. In manual, each player's tachometer moves with their
// speedometer and their gear sits on the inner side under the top panels. With 3 or 4 players the skull is drawn at half size, and the
// bottom row's speedometers sit at the top of their quadrants. The game's 2D clip inset is lifted for the draw loop so
// the HUD can sit closer to the edges than the overscan border.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "rush2.h"
#include "players4.h"
#include "battle.h"

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

    // Each player's area of the screen with the views stacked (top, bottom) and side by side (left, right).
    struct Area {
        int32_t x0, y0, x1, y1;
    };
    constexpr Area stacked_area[2] = {
        { inset_x, inset_y, screen_width - inset_x, screen_height / 2 - 1 },
        { inset_x, screen_height / 2 + 1, screen_width - inset_x, screen_height - inset_y },
    };
    constexpr Area side_area[2] = {
        { inset_x, inset_y, screen_width / 2 - 1, screen_height - inset_y },
        { screen_width / 2 + 1, inset_y, screen_width - inset_x, screen_height - inset_y },
    };
    // Each player's quadrant with 3 or 4 players.
    constexpr Area quad_area[4] = {
        { inset_x, inset_y, screen_width / 2 - 1, screen_height / 2 - 1 },
        { screen_width / 2 + 1, inset_y, screen_width - inset_x, screen_height / 2 - 1 },
        { inset_x, screen_height / 2 + 1, screen_width / 2 - 1, screen_height - inset_y },
        { screen_width / 2 + 1, screen_height / 2 + 1, screen_width - inset_x, screen_height - inset_y },
    };
    // RT64 origins at a quarter and three quarters of the window's width, the middles of the halves.
    constexpr uint16_t origin_left_half = G_EX_ORIGIN_CENTER / 2;
    constexpr uint16_t origin_right_half = G_EX_ORIGIN_CENTER + G_EX_ORIGIN_CENTER / 2;

    // current_origin while a widget is stretched to the window; never equal to a real origin.
    constexpr uint16_t origin_stretched = 0xFFFF;

    // Whether each widget slot was last allocated by the race HUD setup.
    bool hud_slot[max_widgets];
    bool building_hud = false;

    uint16_t slot_origin[max_widgets];
    // Side by side: how far each widget is moved for the draw loop, and its position to restore afterwards.
    bool side_by_side = false;
    int quad_views = 0; // 3 or 4 while the race is drawn in quadrants.
    int16_t slot_dx[max_widgets];
    int16_t slot_dy[max_widgets];
    bool slot_moved[max_widgets];
    int16_t saved_x[max_widgets];
    int16_t saved_y[max_widgets];
    // Split, the clip inset is lifted for the draw loop (the HUD sits closer to the edges), and restored after.
    bool inset_lifted = false;
    // With 3 or 4 players, the deaths skull is drawn at half size (a quadrant is too short for it, the time and the
    // radar): its texture rectangles are scaled after the game writes them, from half_start to the 2D cursor.
    bool slot_half[max_widgets];
    bool half_pending = false;
    int32_t half_start = 0;
    float half_scale_x = 0.5f, half_scale_y = 0.5f;
    // Widgets another file places in final screen coordinates and draws scaled (the battle HUD, src/battle.cpp):
    // they aren't anchored, grouped or moved for split screen, and their texture rectangles are scaled by these
    // factors about their top left corner (0 = not such a widget).
    float managed_scale_x[max_widgets];
    float managed_scale_y[max_widgets];
    uint16_t managed_origin[max_widgets];
    // Where the track map moved to as of the last widget loop: the laps-left number printed above it follows it.
    bool map_known = false;
    // The laps-left number's position as printed (before it follows the map), from this frame, and the height of its
    // text: the time left goes above both.
    bool laps_known = false;
    int32_t laps_y = 0;
    constexpr int32_t laps_height = 8;
    constexpr int32_t lifted_inset = 64;
    int32_t map_dx = 0;
    // Top of each player's lap (checkpoint) time box, part of their time under the race time, as of this frame.
    int32_t lap_top[4];
    int32_t map_dy = 0;
    uint16_t map_origin = G_EX_ORIGIN_NONE;
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

    // How far text may start outside the widget it's printed on (justified text, outlines).
    constexpr int32_t text_margin = 4;

    // Bounds of the anchored HUD widgets as of the last widget loop.
    Rect widget_rect[max_widgets];
    bool widget_anchored[max_widgets];
    uint32_t widgets_checked = 0;

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

    // Image fields.
    constexpr int32_t image_width = 0x10;
    constexpr int32_t image_size = 0x14;    // G_IM_SIZ
    constexpr int32_t image_format = 0x15;  // G_IM_FMT
    constexpr int32_t image_palette = 0x16; // s16 index into the palette table, if the widget names no palette.
    constexpr int32_t image_pixels = 0x18;
    constexpr int32_t widget_palette = 0x0; // Palette (its colors at +0x14), or 0.
    constexpr int32_t widget_palette_bank = 0x8; // u16, >> 10 selects the palette table.
    constexpr uint32_t palette_tables = 0x80119428; // 8 bytes each, a pointer to 0x18 byte palettes.
    constexpr int32_t palette_colors = 0x14;

    bool is_rdram(uint32_t address) {
        return address >= 0x80000000 && address < 0x80800000;
    }

    // Whether a texel of a widget's image is opaque (alpha above zero). Formats the HUD doesn't use count as opaque.
    // palette = the colors of a color indexed image (RGBA16).
    bool texel_opaque(uint8_t* rdram, uint32_t pixels, uint32_t palette, int fmt, int siz, int32_t index) {
        switch (fmt) {
            case 2: { // CI
                int32_t i = siz == 0 ? (MEM_BU(0, (int32_t)(pixels + index / 2)) >> ((index & 1) ? 0 : 4)) & 0xF
                    : MEM_BU(index, (int32_t)pixels);
                return palette == 0 || (MEM_HU(i * 2, (int32_t)palette) & 1) != 0;
            }
            case 0: // RGBA
                return siz == 3 ? MEM_BU(index * 4 + 3, (int32_t)pixels) != 0 : (MEM_HU(index * 2, (int32_t)pixels) & 1) != 0;
            case 3: // IA
                if (siz == 0) {
                    return ((MEM_BU(0, (int32_t)(pixels + index / 2)) >> ((index & 1) ? 0 : 4)) & 1) != 0;
                }
                return siz == 1 ? (MEM_BU(index, (int32_t)pixels) & 0xF) != 0 : MEM_BU(index * 2 + 1, (int32_t)pixels) != 0;
            default:
                return true;
        }
    }

    // Shrinks an image widget's bounds to its opaque texels: the HUD's panels have transparent columns (the position
    // and speedometer panels on their right, the track map around the track), which made margins and centering look
    // uneven. Leaves the bounds as they are if the image can't be read or has no opaque texels.
    void trim_to_opaque(uint8_t* rdram, int32_t widget, Rect& r) {
        uint32_t image = (uint32_t)MEM_W(widget_image, widget);
        if (!is_rdram(image)) {
            return;
        }
        uint32_t pixels = (uint32_t)MEM_W(image_pixels, (int32_t)image);
        int fmt = MEM_BU(image_format, (int32_t)image);
        int siz = MEM_BU(image_size, (int32_t)image);
        int32_t stride = MEM_HU(image_width, (int32_t)image);
        if (!is_rdram(pixels) || stride <= 0) {
            return;
        }
        uint32_t palette = 0;
        if (fmt == 2) {
            uint32_t named = (uint32_t)MEM_W(widget_palette, widget);
            int32_t index = MEM_H(image_palette, (int32_t)image);
            if (is_rdram(named)) {
                palette = (uint32_t)MEM_W(palette_colors, (int32_t)named);
            }
            else if (index >= 0) {
                uint32_t table = (uint32_t)MEM_W((MEM_HU(widget_palette_bank, widget) >> 10) * 8, (int32_t)palette_tables);
                if (is_rdram(table)) {
                    palette = (uint32_t)MEM_W(index * 0x18 + palette_colors, (int32_t)table);
                }
            }
            if (!is_rdram(palette)) {
                return;
            }
        }
        int32_t sx0 = MEM_H(widget_src_x0, widget);
        int32_t sy0 = MEM_H(widget_src_y0, widget);
        int32_t w = r.x1 - r.x0 + 1;
        int32_t h = r.y1 - r.y0 + 1;
        int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                if (texel_opaque(rdram, pixels, palette, fmt, siz, (sy0 + y) * stride + sx0 + x)) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
            }
        }
        if (x0 <= x1) {
            r.x1 = r.x0 + x1;
            r.x0 += x0;
            r.y1 = r.y0 + y1;
            r.y0 += y0;
        }
    }

    // Scales the texture rectangles written to the 2D display list since half_start by half_scale_x/y (a half for the
    // skull), about the first one's top left corner (the image's), dividing their texture steps by the same.
    void finish_half(uint8_t* rdram) {
        if (!half_pending) {
            return;
        }
        half_pending = false;
        float sx = half_scale_x, sy = half_scale_y;
        half_scale_x = half_scale_y = 0.5f;
        int32_t end = MEM_W(0, (int32_t)dl_2d_cursor);
        bool anchored = false;
        uint32_t ax = 0;
        uint32_t ay = 0;
        for (int32_t c = half_start; c + 24 <= end; c += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, c);
            uint32_t op = w0 >> 24;
            if (op != 0xE4 && op != 0xE5) {
                continue;
            }
            uint32_t w1 = (uint32_t)MEM_W(4, c);
            uint32_t x1 = (w0 >> 12) & 0xFFF;
            uint32_t y1 = w0 & 0xFFF;
            uint32_t x0 = (w1 >> 12) & 0xFFF;
            uint32_t y0 = w1 & 0xFFF;
            if (!anchored) {
                ax = x0;
                ay = y0;
                anchored = true;
            }
            x0 = ax + (uint32_t)std::lround((float)(x0 - ax) * sx);
            y0 = ay + (uint32_t)std::lround((float)(y0 - ay) * sy);
            x1 = std::min<uint32_t>(ax + (uint32_t)std::lround((float)(x1 - ax) * sx), 0xFFF);
            y1 = std::min<uint32_t>(ay + (uint32_t)std::lround((float)(y1 - ay) * sy), 0xFFF);
            MEM_W(0, c) = (int32_t)((op << 24) | (x1 << 12) | y1);
            MEM_W(4, c) = (int32_t)((w1 & 0xFF000000u) | (x0 << 12) | y0);
            // G_RDPHALF_2 with the steps (s5.10 each) follows the G_RDPHALF_1 with the texture coordinates.
            int32_t steps = c + 16;
            uint32_t st = (uint32_t)MEM_W(4, steps);
            // Signed: an image drawn flipped steps backward.
            uint32_t dsdx = (uint32_t)std::lround((float)(int16_t)(st >> 16) / sx) & 0xFFFF;
            uint32_t dtdy = (uint32_t)std::lround((float)(int16_t)(st & 0xFFFF) / sy) & 0xFFFF;
            MEM_W(4, steps) = (int32_t)((dsdx << 16) | dtdy);
            c += 16;
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

    // Origin for an x position (in 320-wide screen coordinates) by the screen third it falls in.
    uint16_t origin_for_x(int32_t x) {
        if (3 * x < screen_width) {
            return G_EX_ORIGIN_LEFT;
        }
        if (3 * x > 2 * screen_width) {
            return G_EX_ORIGIN_RIGHT;
        }
        return G_EX_ORIGIN_NONE;
    }

    // Which third of [a0, a1] a position falls in: 0, 1 or 2.
    int third_of(int32_t pos, int32_t a0, int32_t a1) {
        int32_t rel = 3 * (pos - a0);
        return rel < (a1 - a0) ? 0 : rel > 2 * (a1 - a0) ? 2 : 1;
    }

    // How far an element spanning [c0, c1] moves from [from0, from1] to [to0, to1]: one near an edge keeps its
    // distance from that edge, one in the middle stays centered.
    int32_t move_along(int32_t c0, int32_t c1, int32_t from0, int32_t from1, int32_t to0, int32_t to1) {
        switch (third_of((c0 + c1) / 2, from0, from1)) {
            case 0: return to0 - from0;
            case 2: return to1 - from1;
            default: return (to0 + to1) / 2 - (from0 + from1) / 2;
        }
    }

    // The player whose stacked half holds [y0, y1], or -1 if it crosses between the halves.
    int stacked_player(int32_t y0, int32_t y1) {
        if (y1 <= stacked_area[0].y1) {
            return 0;
        }
        if (y0 >= stacked_area[1].y0) {
            return 1;
        }
        return -1;
    }

    // Vertically, the halves are short, so an element is centered only if it's close to the middle; otherwise it
    // keeps its distance from the nearer edge.
    int32_t move_vertically(int32_t c0, int32_t c1, const Area& from, const Area& to) {
        int32_t center = c0 + c1;
        int32_t middle = from.y0 + from.y1;
        int32_t band = (from.y1 - from.y0) / 4; // An eighth of the height, in doubled coordinates.
        if (center < middle - band) {
            return to.y0 - from.y0;
        }
        if (center > middle + band) {
            return to.y1 - from.y1;
        }
        return (to.y0 + to.y1) / 2 - (from.y0 + from.y1) / 2;
    }

    // How far an element moves from its stacked half to its side by side half, or with 3 or 4 players to its player's
    // quadrant (player: the element's, if known; players 3 and 4's elements are copies of player 2's, in the bottom
    // half). Shared elements, which cross between the stacked halves (the track map), are centered on the line
    // between the halves, or on the cross between the quadrants.
    void move_to_side(const Rect& r, int32_t& dx, int32_t& dy, int player = -1) {
        if (player < 0) {
            player = stacked_player(r.y0, r.y1);
        }
        if (player < 0) {
            int32_t cx = screen_width / 2;
            int32_t cy = (r.y0 + r.y1 + 1) / 2;
            if (quad_views) {
                cy = screen_height / 2;
            }
            dx = cx - (r.x0 + r.x1 + 1) / 2;
            dy = cy - (r.y0 + r.y1 + 1) / 2;
            return;
        }
        const Area& from = stacked_area[player == 0 ? 0 : 1];
        const Area& to = quad_views ? quad_area[std::min(player, 3)] : side_area[player];
        dx = move_along(r.x0, r.x1, from.x0, from.x1, to.x0, to.x1);
        dy = move_vertically(r.y0, r.y1, from, to);
    }

    // A player's half (side by side) or quadrant out to the screen's edges: the split views reach the framebuffer's
    // edges, so the HUD keeps the same margin from the screen's edges as from the lines between the views.
    Area split_edges(int player) {
        bool right_column = (player & 1) != 0;
        bool bottom_row = quad_views && player >= 2;
        Area a;
        a.x0 = right_column ? screen_width / 2 + 1 : 0;
        a.x1 = right_column ? screen_width - 1 : screen_width / 2 - 1;
        a.y0 = bottom_row ? screen_height / 2 + 1 : 0;
        a.y1 = quad_views && !bottom_row ? screen_height / 2 - 1 : screen_height - 1;
        return a;
    }

    // Margin of a player's HUD from the edges of their half or quadrant.
    constexpr int32_t split_margin = 5;
    // Gap between the time left and the track map under it.
    constexpr int32_t stack_gap = 2;
    // Gap between the deaths skull and the time or position above it.
    constexpr int32_t deaths_gap = 5;

    bool valid(const Rect& r) {
        return r.x0 <= r.x1 && r.y0 <= r.y1;
    }

    // Where a player's elements go in their half (side by side) or quadrant, each the same margin from its edges: the
    // time, speedometer and position along the outer edge (top, or the bottom row's bottom), the time at the left, the
    // position at the right and the speedometer between them (centered in rush2_hud_draw_begin), all with their tops
    // (bottom row: bottoms) lined up; the deaths skull on the outer side under the time (right column: the position;
    // bottom row: above them); the radar against the middle of the screen (side by side and the top row: the bottom;
    // the bottom row: the top), on the outer side, or beside the skull if a quadrant is too short for both; the gear (manual)
// on the inner side under the top panels' height, the same in every half or quadrant. roles =
    // the bounds of each of the player's roles. Returns false for other elements.
    bool place_role(rush2::players4::HudRole role, int player, const Rect* roles, int32_t& dx, int32_t& dy) {
        using rush2::players4::HudRole;
        if (role == HudRole::Other || player < 0 || player > 3 || (!quad_views && player > 1)) {
            return false;
        }
        const Rect& g = roles[(int)role];
        const Area a = split_edges(player);
        bool bottom_row = quad_views && player >= 2;
        bool right_column = (player & 1) != 0;
        int32_t w = g.x1 - g.x0;
        int32_t h = g.y1 - g.y0;
        // The top row's edge (bottom row: its bottom edge).
        auto outer_y = [&](int32_t height) {
            return bottom_row ? a.y1 - split_margin - height : a.y0 + split_margin;
        };
        // Where the deaths skull goes (its top left corner).
        auto deaths_at = [&](int32_t& x, int32_t& y) {
            const Rect& d = roles[(int)HudRole::Deaths];
            const Rect& above = roles[(int)(right_column ? HudRole::Position : HudRole::Time)];
            int32_t ah = valid(above) ? above.y1 - above.y0 + 1 + deaths_gap : 0;
            x = right_column ? a.x1 - split_margin - (d.x1 - d.x0) : a.x0 + split_margin;
            y = bottom_row ? outer_y(d.y1 - d.y0) - ah : outer_y(d.y1 - d.y0) + ah;
            // In the top row of quadrants, there's no room for the skull under the lap time box (shown after a
            // checkpoint) and above the radar: it goes under the race time, and the box covers it while it's shown.
            const Rect& radar = roles[(int)HudRole::Radar];
            if (quad_views && !bottom_row && !right_column && valid(above) && valid(radar) &&
                lap_top[player] > above.y0 && lap_top[player] <= above.y1 &&
                y + (d.y1 - d.y0) + 1 >= a.y1 - split_margin - (radar.y1 - radar.y0)) {
                y = outer_y(d.y1 - d.y0) + lap_top[player] - above.y0 + deaths_gap;
            }
        };
        int32_t x0 = g.x0;
        int32_t y0 = g.y0;
        switch (role) {
            case HudRole::Time:
                x0 = a.x0 + split_margin;
                y0 = outer_y(h);
                break;
            case HudRole::Speed:
                // The bottom row's at the top of the quadrant, clear of the time and position at its bottom.
                x0 = (a.x0 + a.x1) / 2 - w / 2;
                y0 = bottom_row ? a.y0 + split_margin : outer_y(h);
                break;
            case HudRole::Position:
                x0 = a.x1 - split_margin - w;
                y0 = outer_y(h);
                break;
            case HudRole::Deaths:
                deaths_at(x0, y0);
                break;
            case HudRole::Gear: {
                // The gear (manual only) on the inner side, the side the skull isn't on, under the top row's panel there
                // (the position; right column: the time), lined up with the panel's edge away from the screen's middle:
                // centered, it ran into the time left's panel there. It clears the taller of the time and position, so
                // every player's gear is at the same height in their half or quadrant.
                const Rect& above = roles[(int)(right_column ? HudRole::Time : HudRole::Position)];
                const Rect& other = roles[(int)(right_column ? HudRole::Position : HudRole::Time)];
                if (!valid(above)) {
                    return false;
                }
                int32_t aw = above.x1 - above.x0;
                int32_t ah = std::max(above.y1 - above.y0, valid(other) ? other.y1 - other.y0 : 0) + 1 + deaths_gap;
                int32_t ax0 = right_column ? a.x0 + split_margin : a.x1 - split_margin - aw;
                x0 = right_column ? ax0 + aw - w : ax0;
                y0 = a.y0 + split_margin + ah;
                break;
            }
            case HudRole::Radar: {
                x0 = right_column ? a.x1 - split_margin - w : a.x0 + split_margin;
                y0 = bottom_row ? a.y0 + split_margin : a.y1 - split_margin - h;
                const Rect& d = roles[(int)HudRole::Deaths];
                if (valid(d)) {
                    int32_t dx0, dy0;
                    deaths_at(dx0, dy0);
                    // Only if they overlap: the skull under the right column's taller position panel ends just
                    // short of the radar, and moving the radar for that left it off the edge.
                    if (dy0 <= y0 + h + 1 && y0 <= dy0 + (d.y1 - d.y0) + 1) {
                        int32_t step = d.x1 - d.x0 + 1 + deaths_gap;
                        x0 += right_column ? -step : step;
                    }
                }
                break;
            }
            case HudRole::Banner:
                // The place once finished: its parts move together, like any other element.
                move_to_side(g, dx, dy, player);
                return true;
            default:
                return false;
        }
        dx = x0 - g.x0;
        dy = y0 - g.y0;
        return true;
    }

    // How far a 320-wide screen position with an RT64 origin is drawn from where it would be without one, for HUD
    // width `spread` past 320.
    int32_t screen_offset(uint16_t origin, float spread) {
        return origin < G_EX_ORIGIN_NONE ? (int32_t)std::lround((origin / (float)G_EX_ORIGIN_RIGHT - 0.5f) * spread) : 0;
    }

    // Origin for an element centered at x side by side: its half's outer edge, middle or inner edge (the center).
    uint16_t side_origin_for_x(int32_t x) {
        static constexpr uint16_t origins[2][3] = {
            { G_EX_ORIGIN_LEFT, origin_left_half, G_EX_ORIGIN_NONE },
            { G_EX_ORIGIN_NONE, origin_right_half, G_EX_ORIGIN_RIGHT },
        };
        int player = x < screen_width / 2 ? 0 : 1;
        return origins[player][third_of(x, side_area[player].x0, side_area[player].x1)];
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

    // Origin for text printed at (x, y): that of the smallest anchored widget under it as of the last widget loop,
    // or the screen third of x if there is none. Screen fills (the pause dimming) are skipped, as they cover all
    // text and are stretched rather than anchored. Side by side, (x, y) is also moved with that widget, or by itself
    // if there is none.
    uint16_t origin_at(int32_t& x, int32_t& y) {
        int32_t dx = 0;
        int32_t dy = 0;
        if (side_by_side || quad_views) {
            move_to_side(Rect{ x, y, x, y, false, false }, dx, dy);
        }
        uint16_t origin = side_by_side || quad_views ? side_origin_for_x(x + dx) : origin_for_x(x);
        int64_t best_area = INT64_MAX;
        for (uint32_t i = 0; i < widgets_checked; i++) {
            const Rect& r = widget_rect[i];
            if (!widget_anchored[i] || r.screen_fill ||
                x < r.x0 - text_margin || x > r.x1 + text_margin || y < r.y0 - text_margin || y > r.y1 + text_margin) {
                continue;
            }
            int64_t area = (int64_t)(r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1);
            if (area < best_area) {
                best_area = area;
                origin = slot_origin[i];
                dx = slot_dx[i];
                dy = slot_dy[i];
            }
        }
        x += dx;
        y += dy;
        return origin;
    }

    int find_group(int* parent, int i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }

    bool touching(const Rect& a, const Rect& b) {
        return a.x0 <= b.x1 + 1 && b.x0 <= a.x1 + 1 && a.y0 <= b.y1 + 1 && b.y0 <= a.y1 + 1;
    }

    // Whether two widgets are parts of one element. Fills are linked when they touch (a box is built from fills
    // that share edges). Images are linked when the smaller one's center lies on the larger (digits on a panel,
    // dots and the flag on the track map), so an element that only overlaps another's corner stays separate: the
    // blinking bonus time banner overlaps the track map's corner, and grouping them pulled the map to the center
    // whenever the banner was visible. Fills and images are never linked.
    bool same_element(const Rect& a, const Rect& b) {
        if (a.fill != b.fill || !touching(a, b)) {
            return false;
        }
        if (a.fill) {
            return true;
        }

        int64_t area_a = (int64_t)(a.x1 - a.x0 + 1) * (a.y1 - a.y0 + 1);
        int64_t area_b = (int64_t)(b.x1 - b.x0 + 1) * (b.y1 - b.y0 + 1);
        const Rect& small = area_a <= area_b ? a : b;
        const Rect& large = area_a <= area_b ? b : a;
        int32_t cx = (small.x0 + small.x1) / 2;
        int32_t cy = (small.y0 + small.y1) / 2;
        return cx >= large.x0 && cx <= large.x1 && cy >= large.y0 && cy <= large.y1;
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

void rush2::hud::set_widget_scale(int slot, float scale_x, float scale_y, float anchor) {
    if (slot >= 0 && slot < (int)max_widgets) {
        managed_scale_x[slot] = scale_x;
        managed_scale_y[slot] = scale_y;
        managed_origin[slot] = (uint16_t)std::lround(std::clamp(anchor, 0.0f, 1.0f) * G_EX_ORIGIN_RIGHT);
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
}

void rush2_hud_build_end(uint8_t* rdram, recomp_context* ctx) {
    // Players 3 and 4's elements, built while the widgets they create still count as the HUD's.
    rush2::players4::hud_built(rdram, ctx);
    rush2::battle::hud_built(rdram, ctx);
    building_hud = false;
}

// func_800A0FB4, around building the race's finish overlay (each player's place): its widgets are laid out like the
// HUD's. At the end, $v0 = the overlay's element list.
void rush2_hud_finish_begin(uint8_t* rdram, recomp_context* ctx) {
    building_hud = true;
}

void rush2_hud_finish_end(uint8_t* rdram, recomp_context* ctx) {
    rush2::players4::finish_overlay_built(rdram, ctx, (uint32_t)ctx->r2);
    building_hud = false;
}

// func_800541F8 (widget allocator), after the widget is initialized. $v1 = slot.
void rush2_hud_widget_created(uint8_t* rdram, recomp_context* ctx) {
    uint32_t slot = (uint32_t)ctx->r3;
    if (slot < max_widgets) {
        hud_slot[slot] = building_hud;
    }
}

// func_8007D9DC, before the widget draw loop. Groups the visible HUD widgets and picks an origin for each group.
void rush2_hud_draw_begin(uint8_t* rdram, recomp_context* ctx) {
    uint32_t count = (uint32_t)MEM_W(0, (int32_t)widget_count);
    count = std::min(count, max_widgets);
    side_by_side = rush2::splitscreen::is_side_by_side(rdram);
    for (int32_t& top : lap_top) {
        top = INT32_MAX;
    }
    quad_views = rush2::splitscreen::quadrant_views(rdram);
    bool split = side_by_side || quad_views;
    Rect* rects = widget_rect;
    bool* anchored = widget_anchored;
    int parent[max_widgets];
    widgets_checked = count;
    for (uint32_t i = 0; i < count; i++) {
        int32_t widget = (int32_t)(widget_array + i * widget_size);
        uint32_t image = (uint32_t)MEM_W(widget_image, widget);
        int32_t w = MEM_H(widget_w, widget);
        int32_t h = MEM_H(widget_h, widget);
        slot_origin[i] = G_EX_ORIGIN_NONE;
        slot_half[i] = false;
        slot_dx[i] = 0;
        slot_dy[i] = 0;
        parent[i] = (int)i;
        anchored[i] = hud_slot[i] && managed_scale_x[i] == 0.0f && MEM_B(widget_hidden, widget) == 0 &&
            (MEM_BU(widget_flags, widget) & flag_callback) == 0 && w != 0 && h != 0;
        if (!anchored[i]) {
            continue;
        }

        // Fill widgets cover w x h, image widgets draw their source rectangle at (x, y).
        Rect& r = rects[i];
        r.x0 = MEM_H(widget_x, widget);
        r.y0 = MEM_H(widget_y, widget);
        r.fill = image == 0;
        r.screen_fill = is_screen_fill(rdram, widget);
        if (r.fill) {
            r.x1 = r.x0 + w - 1;
            r.y1 = r.y0 + h - 1;
        }
        else {
            r.x1 = r.x0 + MEM_H(widget_src_x1, widget) - MEM_H(widget_src_x0, widget);
            r.y1 = r.y0 + MEM_H(widget_src_y1, widget) - MEM_H(widget_src_y0, widget);
            if (split) {
                trim_to_opaque(rdram, widget, r);
            }
            slot_half[i] = quad_views && rush2::players4::hud_widget_role((int)i) == rush2::players4::HudRole::Deaths;
            if (slot_half[i]) {
                int32_t wx = MEM_H(widget_x, widget);
                int32_t wy = MEM_H(widget_y, widget);
                r.x0 = wx + (r.x0 - wx) / 2;
                r.x1 = wx + (r.x1 - wx) / 2;
                r.y0 = wy + (r.y0 - wy) / 2;
                r.y1 = wy + (r.y1 - wy) / 2;
            }
        }

        for (uint32_t j = 0; j < i; j++) {
            // Split, each player's time, speedometer, position and radar move on their own (players 3 and 4's start
            // out on top of player 2's). Shared elements group by bounds alone (the track map and its car dots).
            int player_i = rush2::players4::hud_widget_player((int)i);
            if (anchored[j] && same_element(rects[i], rects[j]) &&
                (!split || (player_i == rush2::players4::hud_widget_player((int)j) &&
                            (player_i < 0 ||
                             rush2::players4::hud_widget_role((int)i) == rush2::players4::hud_widget_role((int)j))))) {
                parent[find_group(parent, (int)i)] = find_group(parent, (int)j);
            }
        }
    }

    // Bounds of each group and the player it belongs to (if any of its widgets is known to belong to one), stored at
    // the group's root.
    Rect group[max_widgets];
    int group_player[max_widgets];
    rush2::players4::HudRole group_role[max_widgets];
    // Bounds of each player's time, speedometer, position and radar (each can be more than one group), and of the
    // shared elements' roles (the time left, the track map) at [shared].
    using rush2::players4::HudRole;
    constexpr int roles = (int)HudRole::Count;
    constexpr int shared = 4;
    Rect role_rect[5][roles];
    bool has_role[max_widgets][2] = {}; // Whether each shared group holds the time left, the track map.
    for (auto& player_rects : role_rect) {
        for (Rect& r : player_rects) {
            r = Rect{ INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, false, false };
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        group[i] = Rect{ INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, false, false };
        group_player[i] = -1;
        group_role[i] = rush2::players4::HudRole::Other;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (anchored[i]) {
            int root = find_group(parent, (int)i);
            if (split && group_player[root] < 0) {
                group_player[root] = rush2::players4::hud_widget_player((int)i);
                group_role[root] = rush2::players4::hud_widget_role((int)i);
            }
            int player = rush2::players4::hud_widget_player((int)i);
            int role = (int)rush2::players4::hud_widget_role((int)i);
            if (split && player < 4 && role != 0) {
                Rect& r = role_rect[player < 0 ? shared : player][role];
                if (player >= 0 && rush2::players4::hud_widget_lap_time((int)i)) {
                    lap_top[player] = std::min(lap_top[player], rects[i].y0);
                }
                r.x0 = std::min(r.x0, rects[i].x0);
                r.y0 = std::min(r.y0, rects[i].y0);
                r.x1 = std::max(r.x1, rects[i].x1);
                r.y1 = std::max(r.y1, rects[i].y1);
            }
            Rect& g = group[root];
            g.x0 = std::min(g.x0, rects[i].x0);
            g.y0 = std::min(g.y0, rects[i].y0);
            g.x1 = std::max(g.x1, rects[i].x1);
            g.y1 = std::max(g.y1, rects[i].y1);
            g.screen_fill = g.screen_fill || rects[i].screen_fill;
            has_role[root][0] = has_role[root][0] || (player < 0 && role == (int)HudRole::TimeLeft);
            has_role[root][1] = has_role[root][1] || (player < 0 && role == (int)HudRole::Map);
        }
    }
    // The time, speedometer and position count their hidden widgets too: the speedometer and position are hidden
    // through most of the countdown, and the time's checkpoint (lap) time box under the race time shows only after a
    // checkpoint, so the elements laid out from them (the tachometer, the gear, the skull, the speedometer between the
    // time and position) jumped when they appeared. Their widgets keep their layout positions while hidden.
    if (split) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t widget = (int32_t)(widget_array + i * widget_size);
            HudRole role = rush2::players4::hud_widget_role((int)i);
            int player = rush2::players4::hud_widget_player((int)i);
            if (anchored[i] || !hud_slot[i] || (role != HudRole::Time && role != HudRole::Speed && role != HudRole::Position) || player < 0 ||
                player > 3 || (MEM_BU(widget_flags, widget) & flag_callback) != 0 || MEM_H(widget_w, widget) == 0 ||
                MEM_H(widget_h, widget) == 0) {
                continue;
            }
            Rect b;
            b.x0 = MEM_H(widget_x, widget);
            b.y0 = MEM_H(widget_y, widget);
            if (MEM_W(widget_image, widget) == 0) {
                b.x1 = b.x0 + MEM_H(widget_w, widget) - 1;
                b.y1 = b.y0 + MEM_H(widget_h, widget) - 1;
            }
            else {
                b.x1 = b.x0 + MEM_H(widget_src_x1, widget) - MEM_H(widget_src_x0, widget);
                b.y1 = b.y0 + MEM_H(widget_src_y1, widget) - MEM_H(widget_src_y0, widget);
                // A hidden digit's source is its whole digit strip (the speed's 16 x 143, the time's 96 x 11), not
                // one digit. The digits sit on their panels, which give the bounds: a widget whose top left corner
                // is on another widget of its role and player, further up and left, is left out.
                bool on_panel = false;
                for (uint32_t j = 0; j < count && !on_panel; j++) {
                    int32_t other = (int32_t)(widget_array + j * widget_size);
                    if (j == i || !hud_slot[j] || rush2::players4::hud_widget_role((int)j) != role ||
                        rush2::players4::hud_widget_player((int)j) != player || MEM_W(widget_image, other) == 0 ||
                        (MEM_BU(widget_flags, other) & flag_callback) != 0) {
                        continue;
                    }
                    int32_t ox = MEM_H(widget_x, other);
                    int32_t oy = MEM_H(widget_y, other);
                    int32_t ox1 = ox + MEM_H(widget_src_x1, other) - MEM_H(widget_src_x0, other);
                    int32_t oy1 = oy + MEM_H(widget_src_y1, other) - MEM_H(widget_src_y0, other);
                    on_panel = ox <= b.x0 && oy <= b.y0 && (ox != b.x0 || oy != b.y0) && b.x0 <= ox1 && b.y0 <= oy1;
                }
                if (on_panel) {
                    continue;
                }
                trim_to_opaque(rdram, widget, b);
            }
            Rect& r = role_rect[player][(int)role];
            if (rush2::players4::hud_widget_lap_time((int)i)) {
                lap_top[player] = std::min(lap_top[player], b.y0);
            }
            r.x0 = std::min(r.x0, b.x0);
            r.y0 = std::min(r.y0, b.y0);
            r.x1 = std::max(r.x1, b.x1);
            r.y1 = std::max(r.y1, b.y1);
        }
    }
    // A role the game hides for a while (the position while a player's car is wrecked) keeps its last bounds, so the
    // elements laid out around it (the skull under it, the speedometer beside it) stay put.
    static Rect last_role_rect[5][roles];
    static bool last_role_known[5][roles];
    if (split) {
        for (int p = 0; p < 5; p++) {
            for (int role = 1; role < roles; role++) {
                if (valid(role_rect[p][role])) {
                    last_role_rect[p][role] = role_rect[p][role];
                    last_role_known[p][role] = true;
                }
                else if (last_role_known[p][role]) {
                    role_rect[p][role] = last_role_rect[p][role];
                }
            }
        }
    }

    // Anchor each group by the screen third its center falls in, or side by side, move it to its player's half and
    // anchor it by the third of the half. Stored at the group's root.
    int32_t group_dx[max_widgets];
    int32_t group_dy[max_widgets];
    uint16_t group_origin[max_widgets];
    bool map_moved = false;
    Rect map_rect{};
    map_dx = 0;
    map_dy = 0;
    map_origin = G_EX_ORIGIN_NONE;
    for (uint32_t i = 0; i < count; i++) {
        const Rect& g = group[i];
        group_dx[i] = 0;
        group_dy[i] = 0;
        if (!anchored[i] || find_group(parent, (int)i) != (int)i) {
            continue;
        }
        if (split && !g.screen_fill) {
            int player = group_player[i];
            // A role's groups (the parts of a player's time, place...) move and anchor as one block.
            const Rect* block = &g;
            if (player >= 0 && player <= 3 && group_role[i] != HudRole::Other &&
                place_role(group_role[i], player, role_rect[player], group_dx[i], group_dy[i])) {
                block = &role_rect[player][(int)group_role[i]];
            }
            else {
                move_to_side(g, group_dx[i], group_dy[i], group_player[i]);
            }
            group_origin[i] = side_origin_for_x((block->x0 + block->x1) / 2 + group_dx[i]);
            if (group_role[i] == HudRole::Gear && block != &g) {
                // The gear is lined up with the panel above it, so it takes that panel's anchor.
                HudRole panel = (player & 1) ? HudRole::Time : HudRole::Position;
                int32_t pdx = 0;
                int32_t pdy = 0;
                if (place_role(panel, player, role_rect[player], pdx, pdy)) {
                    const Rect& b = role_rect[player][(int)panel];
                    group_origin[i] = side_origin_for_x((b.x0 + b.x1) / 2 + pdx);
                }
            }
            if (player < 0 && has_role[i][1]) {
                map_moved = true;
                map_rect = Rect{ g.x0 + group_dx[i], g.y0 + group_dy[i], g.x1 + group_dx[i], g.y1 + group_dy[i], false, false };
                map_dx = group_dx[i];
                map_dy = group_dy[i];
                map_origin = group_origin[i];
            }
        }
        else {
            group_origin[i] = origin_for_x((g.x0 + g.x1) / 2);
        }
    }

    // Side by side, a half is narrow, so a centered element can run into one at the half's edge (in 4:3, the timer
    // and the speedometer): it slides away from it, as placed on the screen with their anchors.
    if (split) {
        constexpr int32_t gap = 2;
        float spread = rush2::splitscreen::hud_width() - screen_width;
        auto screen_offset = [&](uint16_t origin) {
            return ::screen_offset(origin, spread);
        };

        // The time left is shared by the players: centered on the screen, just above the track map (without a map:
        // side by side, at the top; in quadrants, on the cross between them).
        const Rect& left = role_rect[shared][(int)HudRole::TimeLeft];
        if (valid(left)) {
            int32_t cx = screen_width / 2 - (left.x0 + left.x1 + 1) / 2;
            int32_t map_top = map_rect.y0;
            if (laps_known) {
                map_top = std::min(map_top, laps_y + map_dy);
            }
            int32_t y0 = map_moved ? map_top - stack_gap - (left.y1 - left.y0 + 1)
                : quad_views ? screen_height / 2 - (left.y1 - left.y0 + 1) / 2 : split_margin;
            for (uint32_t i = 0; i < count; i++) {
                if (anchored[i] && find_group(parent, (int)i) == (int)i && has_role[i][0]) {
                    group_dx[i] = cx;
                    group_dy[i] = y0 - left.y0;
                    group_origin[i] = G_EX_ORIGIN_NONE;
                }
            }
        }

        // Each player's speedometer is centered between their time and position as drawn (with widescreen anchors,
        // the time and position move apart). Where it goes is kept for the tachometer, also while it's hidden.
        int32_t speed_dx[4] = {};
        int32_t speed_dy[4] = {};
        uint16_t speed_origin[4] = {};
        bool speed_placed[4] = {};
        for (int player = 0; player < (quad_views ? 4 : 2); player++) {
            const Rect* r = role_rect[player];
            const Rect& time = r[(int)HudRole::Time];
            const Rect& speed = r[(int)HudRole::Speed];
            const Rect& place = r[(int)HudRole::Position];
            if (!valid(time) || !valid(speed) || !valid(place)) {
                continue;
            }
            // A role's groups all move by the same amount and share an origin, so any group of it gives them.
            int32_t time_x1 = INT32_MIN;
            int32_t place_x0 = INT32_MIN;
            int speed_root = -1;
            for (uint32_t i = 0; i < count; i++) {
                if (!anchored[i] || find_group(parent, (int)i) != (int)i || group_player[i] != player) {
                    continue;
                }
                if (group_role[i] == HudRole::Time) {
                    time_x1 = time.x1 + group_dx[i] + screen_offset(group_origin[i]);
                }
                else if (group_role[i] == HudRole::Position) {
                    place_x0 = place.x0 + group_dx[i] + screen_offset(group_origin[i]);
                }
                else if (group_role[i] == HudRole::Speed) {
                    speed_root = (int)i;
                }
            }
            // The time or position hidden for now: where it would be.
            auto placed_x = [&](HudRole role, int32_t x) {
                int32_t dx = 0;
                int32_t dy = 0;
                place_role(role, player, r, dx, dy);
                const Rect& b = r[(int)role];
                return x + dx + screen_offset(side_origin_for_x((b.x0 + b.x1) / 2 + dx));
            };
            if (time_x1 == INT32_MIN) {
                time_x1 = placed_x(HudRole::Time, time.x1);
            }
            if (place_x0 == INT32_MIN) {
                place_x0 = placed_x(HudRole::Position, place.x0);
            }
            if (speed_root >= 0) {
                speed_origin[player] = group_origin[speed_root];
                speed_dy[player] = group_dy[speed_root];
            }
            else {
                int32_t dx = 0;
                place_role(HudRole::Speed, player, r, dx, speed_dy[player]);
                speed_origin[player] = side_origin_for_x((speed.x0 + speed.x1) / 2 + dx);
            }
            int32_t x0 = (time_x1 + 1 + place_x0) / 2 - (speed.x1 - speed.x0 + 1) / 2 -
                screen_offset(speed_origin[player]);
            speed_dx[player] = x0 - speed.x0;
            speed_placed[player] = true;
            for (uint32_t i = 0; i < count; i++) {
                if (anchored[i] && find_group(parent, (int)i) == (int)i && group_player[i] == player &&
                    group_role[i] == HudRole::Speed) {
                    group_dx[i] = x0 - speed.x0;
                }
            }
        }

        auto centered = [](uint16_t origin) {
            return origin == origin_left_half || origin == origin_right_half;
        };
        for (uint32_t m = 0; m < count; m++) {
            // Elements of more than one group (a player's time, place...) don't slide, so their parts stay together.
            if (!anchored[m] || find_group(parent, (int)m) != (int)m || !centered(group_origin[m]) ||
                (group_role[m] != HudRole::Other && group_role[m] != HudRole::Speed)) {
                continue;
            }
            for (uint32_t e = 0; e < count; e++) {
                // The tachometers move with the speedometers (below), and the gears come and go with the
                // transmission, so they don't push them.
                if (!anchored[e] || find_group(parent, (int)e) != (int)e || centered(group_origin[e]) ||
                    group[e].screen_fill || group_role[e] == HudRole::Tach || group_role[e] == HudRole::Gear) {
                    continue;
                }
                int32_t m0 = group[m].x0 + group_dx[m] + screen_offset(group_origin[m]);
                int32_t m1 = group[m].x1 + group_dx[m] + screen_offset(group_origin[m]);
                int32_t e0 = group[e].x0 + group_dx[e] + screen_offset(group_origin[e]);
                int32_t e1 = group[e].x1 + group_dx[e] + screen_offset(group_origin[e]);
                bool rows_overlap = group[m].y0 + group_dy[m] <= group[e].y1 + group_dy[e] &&
                    group[e].y0 + group_dy[e] <= group[m].y1 + group_dy[m];
                if (!rows_overlap || m0 > e1 + gap || e0 > m1 + gap) {
                    continue;
                }
                group_dx[m] += (e0 + e1 < m0 + m1) ? e1 + gap + 1 - m0 : e0 - gap - 1 - m1;
            }
        }

        // Each player's tachometer (shown in manual) hangs under their speedometer's panel: it moves with the panel
        // (the speedometer's largest group), or where the speedometer would go while it's hidden.
        for (int player = 0; player < (quad_views ? 4 : 2); player++) {
            int speed_root = -1;
            int64_t speed_area = -1;
            for (uint32_t i = 0; i < count; i++) {
                if (anchored[i] && find_group(parent, (int)i) == (int)i && group_player[i] == player &&
                    group_role[i] == HudRole::Speed) {
                    int64_t area = (int64_t)(group[i].x1 - group[i].x0 + 1) * (group[i].y1 - group[i].y0 + 1);
                    if (area > speed_area) {
                        speed_area = area;
                        speed_root = (int)i;
                    }
                }
            }
            int32_t dx = 0;
            int32_t dy = 0;
            uint16_t origin;
            if (speed_root >= 0) {
                dx = group_dx[speed_root];
                dy = group_dy[speed_root];
                origin = group_origin[speed_root];
            }
            else if (speed_placed[player]) {
                dx = speed_dx[player];
                dy = speed_dy[player];
                origin = speed_origin[player];
            }
            else {
                continue;
            }
            for (uint32_t i = 0; i < count; i++) {
                if (anchored[i] && find_group(parent, (int)i) == (int)i && group_player[i] == player &&
                    group_role[i] == HudRole::Tach) {
                    group_dx[i] = dx;
                    group_dy[i] = dy;
                    group_origin[i] = origin;
                }
            }
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        if (anchored[i]) {
            int root = find_group(parent, (int)i);
            slot_dx[i] = (int16_t)group_dx[root];
            slot_dy[i] = (int16_t)group_dy[root];
            slot_origin[i] = group_origin[root];
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

    map_known = map_moved;
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
    if (slot >= max_widgets || MEM_B(widget_hidden, widget) != 0) {
        return;
    }

    in_hud_callback = hud_slot[slot] && (MEM_BU(widget_flags, widget) & flag_callback) != 0;
    if (is_screen_fill(rdram, widget)) {
        stretch_rect(rdram);
        return;
    }
    set_rect_origin(rdram, managed_scale_x[slot] != 0.0f ? managed_origin[slot] : slot_origin[slot]);
    if (managed_scale_x[slot] != 0.0f) {
        if (MEM_W(widget_image, widget) != 0 && (managed_scale_x[slot] != 1.0f || managed_scale_y[slot] != 1.0f)) {
            half_pending = true;
            half_start = MEM_W(0, (int32_t)dl_2d_cursor);
            half_scale_x = managed_scale_x[slot];
            half_scale_y = managed_scale_y[slot];
        }
    }
    else if (slot_half[slot] && (MEM_BU(widget_flags, widget) & flag_callback) == 0) {
        half_pending = true;
        half_start = MEM_W(0, (int32_t)dl_2d_cursor);
    }
}

// func_800734E0 entry (prints a string at ($a0, $a1), justified around $a0). Anchors text printed by HUD callback
// widgets, and moves it side by side.
void rush2_hud_print(uint8_t* rdram, recomp_context* ctx) {
    if (in_hud_callback) {
        int32_t x = (int16_t)ctx->r4;
        int32_t y = (int16_t)ctx->r5;
        set_rect_origin(rdram, origin_at(x, y));
        ctx->r4 = x;
        ctx->r5 = y;
    }
}

// func_8007D9DC, after the widget draw loop. Puts moved widgets back, and draws the menus' join hint for players 3 and 4.
void rush2_hud_draw_end(uint8_t* rdram, recomp_context* ctx) {
    in_hud_callback = false;
    finish_half(rdram);
    laps_known = false; // Printed again before the next widget loop, if it's shown.
    rush2::battle::hud_draw(rdram, ctx);
    end_anchoring(rdram);
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
    }
    rush2::players4::draw_join_hint(rdram, ctx);
}

// func_800B9D48, around its printf of the laps-left number at ($a0, $a1). The text sits on the track map (above its
// start), so it takes the map widget's origin (from the last widget loop), and split, moves with the map: it's
// printed outside the map's bounds, where it was matched to whichever element was nearest.
void rush2_hud_laps_begin(uint8_t* rdram, recomp_context* ctx) {
    int32_t x = (int16_t)ctx->r4;
    int32_t y = (int16_t)ctx->r5;
    laps_known = true;
    laps_y = y - laps_height;
    uint16_t origin;
    if ((side_by_side || quad_views) && map_known) {
        x += map_dx;
        y += map_dy;
        origin = map_origin;
    }
    else {
        origin = origin_at(x, y);
    }
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
