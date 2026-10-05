// Split screen layouts (Settings > Graphics > Split Screen) and the views of 3 and 4 player races.
//
// The race view setup (func_80054BB0) lays out one view per local player in the view array (D_80111888, moved to
// 0x802401F0 for four views, src/players4.cpp): in 2 player races two 320x120 views stacked top and bottom, scissored
// to (12,10)-(308,119) and (12,121)-(308,230), with a black line drawn over rows 119-120 by the frame setup
// (func_800B45B4). It knows only one and two views, and sets up two for three or four players.
//
// After the setup, the views are rewritten for the layout in use:
// - 2 players side by side: 160x240 halves, (12,10)-(159,230) and (161,10)-(308,230), the line turned into columns
//   159-160.
// - 3 and 4 players: quadrants, (12,10)-(159,119), (161,10)-(308,119), (12,121)-(159,230), (161,121)-(308,230), the
//   line becoming a cross. With 3 players the bottom right quadrant is filled black.
// Views 2 and 3 start as copies of view 1, with their own viewports and scissor rects.
//
// Each rewritten view keeps the single player view's vertical field of view, so the scene is drawn at the same scale
// as in single player (a quadrant is the full view shrunk; a half is a narrower slice of it). In widescreen, RT64
// widens views that span the framebuffer but leaves views that don't alone: each view is instead sized to its part of
// the widened screen in the game's own 4:3 coordinates (its outer edge past the 4:3 area), so its projection fills
// it. The scissor hook in src/widescreen.cpp anchors each view's outer scissor edges to the window's edges.
//
// The layout is picked when a race sets up its views and kept until the next one; the view sizes are refreshed
// every frame (by the frame setup's call to func_80054BB0) so they follow the window's aspect ratio.
//
// The race HUD is rearranged to match in src/hud.cpp.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "rush2.h"
#include "rush2_hooks.h"
#include "players4.h"

#include "recompui/recompui.h"
#include "ultramodern/config.hpp"

#define F3DEX_GBI_2
#define G_SC_NON_INTERLACE 0 // gbi.h scissor mode, which the extended GBI header doesn't define.
#include "rt64_extended_gbi.h"

namespace {
    // Game addresses.
    constexpr uint32_t view_array = 0x802401F0; // Player views, 0x54 bytes each (moved from 0x80111888).
    constexpr uint32_t view_size = 0x54;
    constexpr uint32_t view_count = 0x800E7D39; // u8, views set up by func_80054BB0.
    constexpr uint32_t split_flag = 0x800E7D51; // u8, set while two or more views are up.
    constexpr uint32_t inset_x = 0x80023058;    // s16, 12
    constexpr uint32_t inset_y = 0x8002305A;    // s16, 10
    constexpr uint32_t screen_x = 0x80022FEC;   // s16, screen position offset
    constexpr uint32_t screen_y = 0x80022FEE;   // s16
    constexpr uint32_t scissor_rects[4] = { 0x8010C018, 0x8010C028, rush2::players4::scissors,
                                            rush2::players4::scissors + 8 }; // u16 ulx, uly, lrx, lry
    constexpr uint32_t viewport_2p = 0x800CA2E8; // Viewport of view 1, copied for views 2 and 3.

    // View fields.
    constexpr int32_t view_viewport = 0x0;   // Viewport pointer.
    constexpr int32_t view_scissor = 0x4;    // Scissor rect pointer.
    constexpr int32_t view_ortho = 0x8;      // Set when the view has no field of view.
    constexpr int32_t view_hfov = 0xC;       // Radians.
    constexpr int32_t view_vfov = 0x10;
    constexpr int32_t view_tan_h = 0x14;     // tan(hfov / 2)
    constexpr int32_t view_tan_v = 0x18;
    constexpr int32_t view_inv_tan_h = 0x1C; // 0.5 / tan(hfov / 2)
    constexpr int32_t view_inv_tan_v = 0x20;
    constexpr int32_t view_width = 0x24;     // Floats, in screen pixels.
    constexpr int32_t view_height = 0x28;
    constexpr int32_t view_center_x = 0x2C;
    constexpr int32_t view_center_y = 0x30;

    constexpr int32_t screen_width = 320;
    constexpr int32_t screen_height = 240;

    std::atomic<rush2::splitscreen::Layout> chosen_layout = rush2::splitscreen::Layout::TopBottom;

    enum class Active { None, SideBySide, Quadrants };

    // The current race's layout, its number of views, and tan(hfov / 2) of the views it set up (the field of view
    // the game asked for).
    Active active = Active::None;
    int active_views = 0;
    float tan_half_hfov = 1.0f;
    bool in_setup_call = false;

    uint32_t side_dl_cursor = rush2::players4::side_dl_start;

    float read_float(uint8_t* rdram, int32_t addr) {
        int32_t bits = MEM_W(0, addr);
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    }

    void write_float(uint8_t* rdram, int32_t addr, float f) {
        int32_t bits;
        memcpy(&bits, &f, sizeof(bits));
        MEM_W(0, addr) = bits;
    }

    // The width of the 3D image in 4:3 screen pixels (320 at 4:3), as RT64 widens it for the aspect ratio setting.
    float wide_screen_width() {
        float aspect = 4.0f / 3.0f;
        switch (ultramodern::renderer::get_graphics_config().ar_option) {
            case ultramodern::renderer::AspectRatio::Manual:
                aspect = 16.0f / 9.0f;
                break;
            case ultramodern::renderer::AspectRatio::Expand: {
                int width = 0;
                int height = 0;
                recompui::get_window_size(width, height);
                if (width > 0 && height > 0) {
                    aspect = std::max(aspect, (float)width / (float)height);
                }
                break;
            }
            default:
                break;
        }
        return screen_width * aspect / (4.0f / 3.0f);
    }

    // Places a view at (cx, cy), w x h in 4:3 screen pixels, with the single player view's vertical field of view,
    // scissored to (x0,y0)-(x1,y1).
    void place_view(uint8_t* rdram, int index, float cx, float cy, float w, float h,
                    int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        int32_t view = (int32_t)(view_array + index * view_size);
        float tan_v = tan_half_hfov * screen_height / screen_width; // The single player view's.
        float tan_h = tan_v * w / h;
        write_float(rdram, view + view_width, w);
        write_float(rdram, view + view_height, h);
        write_float(rdram, view + view_center_x, cx);
        // The view drawer (func_8007C624) moves the first view down by the inset while views are split.
        write_float(rdram, view + view_center_y, cy - (index == 0 ? MEM_H(0, (int32_t)inset_y) : 0));
        if (MEM_W(view_ortho, view) == 0) {
            write_float(rdram, view + view_hfov, 2 * std::atan(tan_h));
            write_float(rdram, view + view_vfov, 2 * std::atan(tan_v));
            write_float(rdram, view + view_tan_h, tan_h);
            write_float(rdram, view + view_tan_v, tan_v);
            write_float(rdram, view + view_inv_tan_h, 0.5f / tan_h);
            write_float(rdram, view + view_inv_tan_v, 0.5f / tan_v);
        }

        int32_t rect = (int32_t)scissor_rects[index];
        MEM_W(view_scissor, view) = rect;
        MEM_H(0, rect) = (int16_t)x0;
        MEM_H(2, rect) = (int16_t)y0;
        MEM_H(4, rect) = (int16_t)x1;
        MEM_H(6, rect) = (int16_t)y1;
    }

    void apply_layout(uint8_t* rdram) {
        float half = wide_screen_width() / 2;
        int32_t left = MEM_H(0, (int32_t)inset_x);
        int32_t top = MEM_H(0, (int32_t)inset_y);
        int32_t mid_x = screen_width / 2;
        int32_t mid_y = screen_height / 2;
        if (active == Active::SideBySide) {
            place_view(rdram, 0, mid_x - half / 2, mid_y, half, screen_height, left, top, mid_x - 1, screen_height - top);
            place_view(rdram, 1, mid_x + half / 2, mid_y, half, screen_height, mid_x + 1, top, screen_width - left,
                       screen_height - top);
        }
        else if (active == Active::Quadrants) {
            for (int i = 0; i < active_views; i++) {
                bool right = (i & 1) != 0;
                bool bottom = (i & 2) != 0;
                place_view(rdram, i, mid_x + (right ? half / 2 : -half / 2), bottom ? mid_y * 3 / 2 : mid_y / 2, half,
                           mid_y, right ? mid_x + 1 : left, bottom ? mid_y + 1 : top,
                           right ? screen_width - left : mid_x - 1, bottom ? screen_height - top : mid_y - 1);
            }
        }
    }

    // Views 2 and 3 start as copies of view 1, with their own viewports and scissor rects.
    void set_up_extra_views(uint8_t* rdram, int count) {
        int32_t src = (int32_t)(view_array + view_size);
        for (int i = 2; i < count; i++) {
            int32_t view = (int32_t)(view_array + i * view_size);
            for (uint32_t b = 0; b < view_size; b++) {
                MEM_B(0, (int32_t)(view + b)) = MEM_B(0, (int32_t)(src + b));
            }
            int32_t viewport = (int32_t)(rush2::players4::viewports + (i - 2) * 0x10);
            for (uint32_t b = 0; b < 0x10; b++) {
                MEM_B(0, (int32_t)(viewport + b)) = MEM_B(0, (int32_t)(viewport_2p + b));
            }
            MEM_W(view_viewport, view) = viewport;
        }
    }

    // Writes commands to a side display list (ending it) and returns its address. RT64 reads display lists
    // asynchronously, so this is a ring holding many frames.
    uint32_t write_side_dl(uint8_t* rdram, const GfxCommand* cmds, uint32_t count) {
        uint32_t bytes = (count + 1) * 8;
        if (side_dl_cursor + bytes > rush2::players4::side_dl_end) {
            side_dl_cursor = rush2::players4::side_dl_start;
        }
        uint32_t dl = side_dl_cursor;
        for (uint32_t i = 0; i < count; i++) {
            MEM_W(0, (int32_t)(dl + i * 8 + 0)) = (int32_t)cmds[i].values.word0;
            MEM_W(0, (int32_t)(dl + i * 8 + 4)) = (int32_t)cmds[i].values.word1;
        }
        MEM_W(0, (int32_t)(dl + count * 8 + 0)) = (int32_t)0xDF000000; // G_ENDDL
        MEM_W(0, (int32_t)(dl + count * 8 + 4)) = 0;
        side_dl_cursor += bytes;
        return dl;
    }

    // G_FILLRECT, inclusive in fill mode.
    void fill_rect(GfxCommand* cmd, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        cmd->values.word0 = 0xF6000000 | ((x1 & 0x3FF) << 14) | ((y1 & 0x3FF) << 2);
        cmd->values.word1 = ((x0 & 0x3FF) << 14) | ((y0 & 0x3FF) << 2);
    }
}

namespace rush2::splitscreen {
    void set_layout(Layout layout) {
        chosen_layout = layout;
    }

    bool views_active(uint8_t* rdram) {
        return active != Active::None && MEM_BU(0, (int32_t)split_flag) != 0 &&
            MEM_BU(0, (int32_t)view_count) == active_views;
    }

    bool is_side_by_side(uint8_t* rdram) {
        return active == Active::SideBySide && views_active(rdram);
    }

    int quadrant_views(uint8_t* rdram) {
        return active == Active::Quadrants && views_active(rdram) ? active_views : 0;
    }

    float hud_width() {
        float width = wide_screen_width();
        switch (ultramodern::renderer::get_graphics_config().hr_option) {
            case ultramodern::renderer::HUDRatioMode::Full:
                return width;
            case ultramodern::renderer::HUDRatioMode::Clamp16x9:
                return std::min(width, screen_width * (16.0f / 9.0f) / (4.0f / 3.0f));
            default:
                return (float)screen_width;
        }
    }
}

extern "C" {

// func_80054BB0 entry. $a0 = the number of views to set up, or 0 to only return the split flag (every frame).
void rush2_split_views_begin(uint8_t* rdram, recomp_context* ctx) {
    in_setup_call = (int32_t)ctx->r4 > 0;
    rush2::players4::frame(rdram);
}

// func_80054BB0, at its return.
void rush2_split_views_end(uint8_t* rdram, recomp_context* ctx) {
    if (in_setup_call) {
        in_setup_call = false;
        int count = MEM_BU(0, (int32_t)view_count);
        active = Active::None;
        active_views = count;
        if (MEM_BU(0, (int32_t)split_flag) != 0) {
            if (count >= 3) {
                set_up_extra_views(rdram, count);
                active = Active::Quadrants;
            }
            else if (count == 2 && chosen_layout == rush2::splitscreen::Layout::SideBySide) {
                active = Active::SideBySide;
            }
            tan_half_hfov = read_float(rdram, (int32_t)(view_array + view_tan_h));
        }
    }
    if (rush2::splitscreen::views_active(rdram)) {
        apply_layout(rdram);
    }
}

// func_800B45B4, after the black line between stacked views is written at $v1 ($v0 = split flag). Side by side it
// becomes a column between the halves; in quadrants, a call to a cross (and the empty quadrant of 3 players).
void rush2_split_divider(uint8_t* rdram, recomp_context* ctx) {
    if (ctx->r2 == 0 || !rush2::splitscreen::views_active(rdram) || active == Active::None) {
        return;
    }
    int32_t ox = MEM_H(0, (int32_t)screen_x);
    int32_t oy = MEM_H(0, (int32_t)screen_y);
    int32_t mid_x = ox + screen_width / 2;
    int32_t mid_y = oy + screen_height / 2;
    int32_t cmd = (int32_t)ctx->r3;
    if (active == Active::SideBySide) {
        GfxCommand fill;
        fill_rect(&fill, mid_x - 1, oy, mid_x, oy + screen_height - 1);
        MEM_W(0, cmd) = (int32_t)fill.values.word0;
        MEM_W(4, cmd) = (int32_t)fill.values.word1;
        return;
    }

    GfxCommand cmds[12];
    uint32_t count = 0;
    fill_rect(&cmds[count++], ox, mid_y - 1, ox + screen_width - 1, mid_y);       // Spans the width, so RT64 widens it.
    fill_rect(&cmds[count++], mid_x - 1, oy, mid_x, oy + screen_height - 1);
    if (active_views == 3) {
        // The empty quadrant, its right edge anchored to the window's right edge.
        gEXEnable(&cmds[count++]);
        gEXPushScissor(&cmds[count++]);
        gEXSetScissor(&cmds[count], G_SC_NON_INTERLACE, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, 0, screen_height);
        count += 2;
        gEXSetRectAlign(&cmds[count], G_EX_ORIGIN_NONE, G_EX_ORIGIN_RIGHT, 0, 0, -screen_width * 4, 0);
        count += 2;
        fill_rect(&cmds[count++], mid_x + 1, mid_y + 1, ox + screen_width - 1, oy + screen_height - 1);
        gEXSetRectAlign(&cmds[count], G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0);
        count += 2;
        gEXPopScissor(&cmds[count++]);
    }
    uint32_t dl = write_side_dl(rdram, cmds, count);
    MEM_W(0, cmd) = (int32_t)0xDE000000; // G_DL (call)
    MEM_W(4, cmd) = (int32_t)dl;
}

}
