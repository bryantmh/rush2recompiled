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
// at a quarter of the window), and those on its inner side stay at the screen's center.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "recomp.h"
#include "rush2.h"
#include "players4.h"

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

    // Where a player's time, speedometer, position and radar go in their half (side by side) or quadrant: the time at
    // the outer top (bottom row: bottom) left, the speedometer at its middle, the position at its right; the radar
    // against the middle of the screen (side by side and the top row: the bottom; the bottom row: the top), on the
    // half's or quadrant's outer side. Each keeps its distance from the edges as placed for stacked views. Returns
    // false for other elements.
    bool place_role(rush2::players4::HudRole role, int player, const Rect& g, int32_t& dx, int32_t& dy) {
        using rush2::players4::HudRole;
        if (role == HudRole::Other || player < 0 || player > 3 || (!quad_views && player > 1)) {
            return false;
        }
        const Area& from = stacked_area[player == 0 ? 0 : 1];
        const Area& to = quad_views ? quad_area[player] : side_area[player];
        bool bottom_row = quad_views && player >= 2;
        bool right_column = (player & 1) != 0;
        int32_t w = g.x1 - g.x0;
        int32_t h = g.y1 - g.y0;
        // Distance from the outer (top or bottom) edge.
        auto outer_y = [&](int32_t margin) {
            return bottom_row ? to.y1 - margin - h : to.y0 + margin;
        };
        constexpr int32_t edge_margin = 4;
        int32_t x0 = g.x0;
        int32_t y0 = g.y0;
        switch (role) {
            case HudRole::Time:
                x0 = to.x0 + (g.x0 - from.x0);
                y0 = outer_y(g.y0 - from.y0);
                break;
            case HudRole::Speed:
                x0 = (to.x0 + to.x1) / 2 - w / 2;
                y0 = outer_y(g.y0 - from.y0);
                break;
            case HudRole::Position:
                x0 = to.x1 - (from.x1 - g.x1) - w;
                y0 = outer_y(edge_margin);
                break;
            case HudRole::Radar:
                x0 = right_column ? to.x1 - (g.x0 - from.x0) - w : to.x0 + (g.x0 - from.x0);
                y0 = bottom_row ? to.y0 + edge_margin : to.y1 - edge_margin - h;
                break;
            case HudRole::Banner:
                // The place once finished: its parts move together, like any other element.
                move_to_side(g, dx, dy, player);
                return true;
            default:
                break;
        }
        dx = x0 - g.x0;
        dy = y0 - g.y0;
        return true;
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

extern "C" {

// func_800A06F8 entry and exit: the race HUD setup, which creates all of its widgets.
void rush2_hud_build_begin(uint8_t* rdram, recomp_context* ctx) {
    building_hud = true;
}

void rush2_hud_build_end(uint8_t* rdram, recomp_context* ctx) {
    // Players 3 and 4's elements, built while the widgets they create still count as the HUD's.
    rush2::players4::hud_built(rdram, ctx);
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
        slot_dx[i] = 0;
        slot_dy[i] = 0;
        parent[i] = (int)i;
        anchored[i] = hud_slot[i] && MEM_B(widget_hidden, widget) == 0 &&
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
        }

        for (uint32_t j = 0; j < i; j++) {
            // Split, each player's time, speedometer, position and radar move on their own (players 3 and 4's start
            // out on top of player 2's).
            if (anchored[j] && same_element(rects[i], rects[j]) &&
                (!split || (rush2::players4::hud_widget_player((int)i) == rush2::players4::hud_widget_player((int)j) &&
                            rush2::players4::hud_widget_role((int)i) == rush2::players4::hud_widget_role((int)j)))) {
                parent[find_group(parent, (int)i)] = find_group(parent, (int)j);
            }
        }
    }

    // Bounds of each group and the player it belongs to (if any of its widgets is known to belong to one), stored at
    // the group's root.
    Rect group[max_widgets];
    int group_player[max_widgets];
    rush2::players4::HudRole group_role[max_widgets];
    // Bounds of each player's time, speedometer, position and radar (each can be more than one group).
    constexpr int roles = 6;
    Rect role_rect[4][roles];
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
            if (split && player >= 0 && player < 4 && role != 0) {
                Rect& r = role_rect[player][role];
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
        }
    }

    // Anchor each group by the screen third its center falls in, or side by side, move it to its player's half and
    // anchor it by the third of the half. Stored at the group's root.
    int32_t group_dx[max_widgets];
    int32_t group_dy[max_widgets];
    uint16_t group_origin[max_widgets];
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
            if (player >= 0 && player <= 3 && group_role[i] != rush2::players4::HudRole::Other &&
                place_role(group_role[i], player, role_rect[player][(int)group_role[i]], group_dx[i], group_dy[i])) {
                block = &role_rect[player][(int)group_role[i]];
            }
            else {
                move_to_side(g, group_dx[i], group_dy[i], group_player[i]);
            }
            group_origin[i] = side_origin_for_x((block->x0 + block->x1) / 2 + group_dx[i]);
        }
        else {
            group_origin[i] = origin_for_x((g.x0 + g.x1) / 2);
        }
    }

    // Side by side, a half is narrow, so a centered element can run into one at the half's edge (in 4:3, the timer
    // and the speedometer): it slides away from it, as placed on the screen with their anchors.
    if (side_by_side || quad_views) {
        constexpr int32_t gap = 2;
        float spread = rush2::splitscreen::hud_width() - screen_width;
        auto screen_offset = [&](uint16_t origin) {
            return origin < G_EX_ORIGIN_NONE ? (int32_t)std::lround((origin / (float)G_EX_ORIGIN_RIGHT - 0.5f) * spread) : 0;
        };
        auto centered = [](uint16_t origin) {
            return origin == origin_left_half || origin == origin_right_half;
        };
        for (uint32_t m = 0; m < count; m++) {
            // Elements of more than one group (a player's time, place...) don't slide, so their parts stay together.
            if (!anchored[m] || find_group(parent, (int)m) != (int)m || !centered(group_origin[m]) ||
                (group_role[m] != rush2::players4::HudRole::Other && group_role[m] != rush2::players4::HudRole::Speed)) {
                continue;
            }
            for (uint32_t e = 0; e < count; e++) {
                if (!anchored[e] || find_group(parent, (int)e) != (int)e || centered(group_origin[e]) ||
                    group[e].screen_fill) {
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
    }

    for (uint32_t i = 0; i < count; i++) {
        if (anchored[i]) {
            int root = find_group(parent, (int)i);
            slot_dx[i] = (int16_t)group_dx[root];
            slot_dy[i] = (int16_t)group_dy[root];
            slot_origin[i] = group_origin[root];
        }
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

    current_origin = G_EX_ORIGIN_NONE;
    scissor_widened = false;
}

// func_8007D9DC, at the top of the widget draw loop. $fp = widget.
void rush2_hud_draw_widget(uint8_t* rdram, recomp_context* ctx) {
    int32_t widget = (int32_t)ctx->r30;
    uint32_t slot = ((uint32_t)widget - widget_array) / widget_size;
    in_hud_callback = false;
    if (slot >= max_widgets || MEM_B(widget_hidden, widget) != 0) {
        return;
    }

    in_hud_callback = hud_slot[slot] && (MEM_BU(widget_flags, widget) & flag_callback) != 0;
    if (is_screen_fill(rdram, widget)) {
        stretch_rect(rdram);
        return;
    }
    set_rect_origin(rdram, slot_origin[slot]);
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
    end_anchoring(rdram);
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

// func_800B9D48, around its printf of the laps-left number at ($a0, $a1). The text sits on the track map, so it
// takes the map widget's origin (from the last widget loop), and moves with it side by side.
void rush2_hud_laps_begin(uint8_t* rdram, recomp_context* ctx) {
    int32_t x = (int16_t)ctx->r4;
    int32_t y = (int16_t)ctx->r5;
    uint16_t origin = origin_at(x, y);
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
