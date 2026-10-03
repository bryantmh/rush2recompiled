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
// that touch or overlap are grouped and anchored together by their combined bounds, so multi-part elements
// (a panel and its digits, a box and its borders) never split apart. Callback widgets have no bounds and print
// their own text (lap times), so each string they print is anchored by its x position instead. The laps-left
// number is printed earlier in the frame by a sprite callback (func_800B9D48) and takes the anchor of the HUD
// widget it's drawn over (the track map).

#include <algorithm>
#include <cstdint>

#include "recomp.h"

#define F3DEX_GBI_2
#define G_SC_NON_INTERLACE 0 // gbi.h scissor mode, which the extended GBI header doesn't define.
#include "rt64_extended_gbi.h"

namespace {
    // Game addresses.
    constexpr uint32_t widget_array = 0x800F9558; // 2D widgets, 0x20 bytes each.
    constexpr uint32_t widget_size = 0x20;
    constexpr uint32_t widget_count = 0x800F5444;
    constexpr uint32_t max_widgets = 200;
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

    // Whether each widget slot was last allocated by the race HUD setup.
    bool hud_slot[max_widgets];
    bool building_hud = false;

    uint16_t slot_origin[max_widgets];
    uint16_t current_origin = G_EX_ORIGIN_NONE;
    bool scissor_widened = false;
    bool in_hud_callback = false;

    struct Rect {
        int32_t x0, y0, x1, y1;
    };

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

    void set_rect_origin(uint8_t* rdram, uint16_t origin) {
        if (origin == current_origin) {
            return;
        }

        // The extended GBI is enabled every time because RT64 disables it at the start of every display list task.
        GfxCommand cmds[6];
        uint32_t count = 0;
        gEXEnable(&cmds[count++]);

        // The game's scissor covers the 4:3 area, which would clip anchored widgets. Widen it to the whole window
        // until the widget loop ends.
        if (origin != G_EX_ORIGIN_NONE && !scissor_widened) {
            gEXPushScissor(&cmds[count++]);
            gEXSetScissor(&cmds[count], G_SC_NON_INTERLACE, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, 0, 240);
            count += 2;
            scissor_widened = true;
        }

        // Rectangle coordinates are relative to the origin, so right-anchored ones are offset by the screen width.
        int32_t offset = (origin == G_EX_ORIGIN_RIGHT) ? -screen_width * 4 : 0;
        gEXSetRectAlign(&cmds[count], origin, origin, offset, 0, offset, 0);
        count += 2;
        write_2d_commands(rdram, cmds, count);
        current_origin = origin;
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
}

extern "C" {

// func_800A06F8 entry and exit: the race HUD setup, which creates all of its widgets.
void rush2_hud_build_begin(uint8_t* rdram, recomp_context* ctx) {
    building_hud = true;
}

void rush2_hud_build_end(uint8_t* rdram, recomp_context* ctx) {
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
        if (image == 0) {
            r.x1 = r.x0 + w - 1;
            r.y1 = r.y0 + h - 1;
        }
        else {
            r.x1 = r.x0 + MEM_H(widget_src_x1, widget) - MEM_H(widget_src_x0, widget);
            r.y1 = r.y0 + MEM_H(widget_src_y1, widget) - MEM_H(widget_src_y0, widget);
        }

        for (uint32_t j = 0; j < i; j++) {
            if (anchored[j] && touching(rects[i], rects[j])) {
                parent[find_group(parent, (int)i)] = find_group(parent, (int)j);
            }
        }
    }

    // Horizontal bounds of each group, stored at the group's root.
    int32_t group_x0[max_widgets];
    int32_t group_x1[max_widgets];
    for (uint32_t i = 0; i < count; i++) {
        group_x0[i] = INT32_MAX;
        group_x1[i] = INT32_MIN;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (anchored[i]) {
            int root = find_group(parent, (int)i);
            group_x0[root] = std::min(group_x0[root], rects[i].x0);
            group_x1[root] = std::max(group_x1[root], rects[i].x1);
        }
    }

    // Anchor each group by the screen third its center falls in.
    for (uint32_t i = 0; i < count; i++) {
        if (anchored[i]) {
            int root = find_group(parent, (int)i);
            slot_origin[i] = origin_for_x((group_x0[root] + group_x1[root]) / 2);
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
    set_rect_origin(rdram, slot_origin[slot]);
}

// func_800734E0 entry (prints a string at ($a0, $a1), justified around $a0). Anchors text printed by HUD callback
// widgets.
void rush2_hud_print(uint8_t* rdram, recomp_context* ctx) {
    if (in_hud_callback) {
        set_rect_origin(rdram, origin_for_x((int16_t)ctx->r4));
    }
}

// func_8007D9DC, after the widget draw loop.
void rush2_hud_draw_end(uint8_t* rdram, recomp_context* ctx) {
    in_hud_callback = false;
    end_anchoring(rdram);
}

// func_800B9D48, around its printf of the laps-left number at ($a0, $a1). The text sits on the track map, so it
// takes the map widget's origin (from the last widget loop), or its screen third if no HUD widget is under it.
void rush2_hud_laps_begin(uint8_t* rdram, recomp_context* ctx) {
    int32_t x = (int16_t)ctx->r4;
    int32_t y = (int16_t)ctx->r5;
    uint16_t origin = origin_for_x(x);
    for (uint32_t i = 0; i < widgets_checked; i++) {
        const Rect& r = widget_rect[i];
        if (widget_anchored[i] && x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) {
            origin = slot_origin[i];
            break;
        }
    }

    current_origin = G_EX_ORIGIN_NONE;
    scissor_widened = false;
    set_rect_origin(rdram, origin);
}

void rush2_hud_laps_end(uint8_t* rdram, recomp_context* ctx) {
    end_anchoring(rdram);
}

}
