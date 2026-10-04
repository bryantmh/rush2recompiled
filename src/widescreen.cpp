// Player views for widescreen.
//
// Each player view's 3D scissor is inset by the overscan border, (12,10)-(308,230) in single player. RT64 only widens
// projections whose scissor spans the framebuffer, so views touching the border are extended to the framebuffer
// edges. The 2 player car select draws each half of the screen as its own view, (12,10)-(160,230) and
// (160,10)-(308,230), so neither spans the framebuffer and both kept the 4:3 menu background: their outer edges are
// anchored to the window's edges with an extended GBI scissor instead, which reveals the (extended) background
// past the 4:3 area at the same scale. In 2 player races the top view's viewport starts at the border (y = 10),
// which left a bar above it; it's moved up to the screen's edge.
//
// Frame clear for widescreen.
//
// The frame setup (func_800B45B4) fills the framebuffer with the fog color only inside the overscan inset
// (12,10)-(308,230), then fills the border around it with black. On hardware the player views are scissored to
// the inset, so the border stays a black frame. The scissor hook in us.toml extends the views to the framebuffer
// edges, so the 3D scene now draws over the border, but on tracks whose sky is the clear color (e.g. Los Angeles)
// nothing covers it: the black strips show through the sky, the top one stretched across the screen and the side
// ones at the edges of the original 4:3 area. Zeroing the insets while the clear is emitted makes it fill the
// whole screen and leaves the border fills empty (they still cover the screen position offset, if any).
//
// Menu background for widescreen.
//
// The scrolling "RUSH2" background of the main menu and the car and track select screens is an animated 3D mesh
// (one model per animation frame) drawn in a full-screen view, so the widened projection shows past its left and
// right edges, which are only about 12 pixels outside the 4:3 area. Each frame is a 480x320 grid of vertices whose
// texture coordinates are a linear function of position (the texture repeats), and the two outermost vertex columns
// on each side fade to dark. Moving those columns outward, with their texture coordinates, extends the pattern
// seamlessly past the window edges: the next column in has the same color, so the stretched triangles look like the
// rest of the grid and the fade moves off screen. The frames are recognized by their geometry when drawn (models are
// loaded into different banks per menu), and moved back for the original aspect ratio.

#include "recomp.h"
#include "rush2_hooks.h"

#include <cstdlib>

#include "ultramodern/config.hpp"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

namespace {
    constexpr uint32_t inset_x = 0x80023058; // s16, 12
    constexpr uint32_t inset_y = 0x8002305A; // s16, 10

    int16_t saved_inset_x;
    int16_t saved_inset_y;

    // Player view scissors: the framebuffer and the overscan border the game insets them by.
    constexpr int32_t screen_width = 320;
    constexpr int32_t screen_height = 240;
    constexpr int32_t border_left = 12;
    constexpr int32_t border_top = 10;
    constexpr int32_t border_right = 308;
    constexpr int32_t border_bottom = 230;

    // F3DEX2 viewport fields, in quarter pixels.
    constexpr int32_t vp_scale_y = 0x2;
    constexpr int32_t vp_trans_y = 0xA;

    // Extended GBI origins for the scissor of the view being drawn.
    uint16_t pending_left_origin = G_EX_ORIGIN_NONE;
    uint16_t pending_right_origin = G_EX_ORIGIN_NONE;

    // Side display lists holding the extended scissors, in spare RDRAM below 16MB (see src/controls_menu.cpp for the
    // other users). RT64 reads display lists asynchronously, so this is a ring holding many frames.
    constexpr uint32_t view_dl_start = 0x80CF0000;
    constexpr uint32_t view_dl_end = 0x80D00000;
    uint32_t view_dl_cursor = view_dl_start;

    // Menu background vertex columns (x in model units) moved outward: x <= -198 on the left, x >= 202 on the right.
    constexpr int32_t bg_left_min = -238;
    constexpr int32_t bg_left_max = -198;
    constexpr int32_t bg_right_min = 202;
    constexpr int32_t bg_right_max = 244;
    // Distance the outer columns are moved, enough for about 580 pixels past each side of the 4:3 area (~6:1). The
    // texture coordinate moves by 25.6 per unit, so this must be a multiple of 5, and the right edge's s must stay
    // within s16 (12371 + 20352).
    constexpr int32_t bg_extend = 795;
    static_assert(bg_extend % 5 == 0 && 12371 + bg_extend * 128 / 5 <= INT16_MAX);
    constexpr uint32_t bg_min_vertices = 64;
    constexpr uint32_t max_model_commands = 256;

    // F3DEX2 vertex fields.
    constexpr int32_t vtx_x = 0x0;
    constexpr int32_t vtx_y = 0x2;
    constexpr int32_t vtx_z = 0x4;
    constexpr int32_t vtx_s = 0x8;
    constexpr int32_t vtx_t = 0xA;
    constexpr uint32_t vtx_size = 0x10;

    // Calls fn(address, count) for each G_VTX at the top level of a model display list. Returns false if the list
    // uses a segment other than 0 (the menu background doesn't).
    template <typename Fn>
    bool for_each_vertex_load(uint8_t* rdram, uint32_t dl, Fn&& fn) {
        if ((dl & 0x0F000000) != 0) {
            return false;
        }
        dl = 0x80000000 | (dl & 0x00FFFFFF);
        for (uint32_t i = 0; i < max_model_commands; i++, dl += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)dl);
            uint32_t w1 = (uint32_t)MEM_W(4, (int32_t)dl);
            switch (w0 >> 24) {
                case 0x01: // G_VTX
                    if ((w1 & 0x0F000000) != 0) {
                        return false;
                    }
                    fn(0x80000000 | (w1 & 0x00FFFFFF), (w0 >> 12) & 0xFF);
                    break;
                case 0xDE: // G_DL, a branch ends the list
                    if ((w0 & 0x00010000) != 0) {
                        return true;
                    }
                    break;
                case 0xDF: // G_ENDDL
                    return true;
            }
        }
        return true;
    }

    // Whether a vertex is on the menu background grid: y within the mesh and texture coordinates
    // s = 25.6 x + 6135, t = 21.33 y + 3397 (within one texel).
    bool is_background_vertex(uint8_t* rdram, int32_t v) {
        int32_t x = MEM_H(vtx_x, v);
        int32_t y = MEM_H(vtx_y, v);
        int32_t z = MEM_H(vtx_z, v);
        int32_t s = MEM_H(vtx_s, v);
        int32_t t = MEM_H(vtx_t, v);
        return y >= -160 && y <= 160 && z >= -16 && z <= 16 &&
            std::abs(5 * s - (128 * x + 30675)) <= 5 * 32 &&
            std::abs(3 * t - (64 * y + 10191)) <= 3 * 32;
    }

    bool is_menu_background(uint8_t* rdram, uint32_t dl) {
        uint32_t vertices = 0;
        bool matches = true;
        bool valid = for_each_vertex_load(rdram, dl, [&](uint32_t addr, uint32_t count) {
            for (uint32_t i = 0; i < count && matches; i++) {
                matches = is_background_vertex(rdram, (int32_t)(addr + i * vtx_size));
            }
            vertices += count;
        });
        return valid && matches && vertices >= bg_min_vertices;
    }

    // Moves the background's outer vertex columns to `extend` units past their original position.
    void extend_menu_background(uint8_t* rdram, uint32_t dl, int32_t extend) {
        for_each_vertex_load(rdram, dl, [&](uint32_t addr, uint32_t count) {
            for (uint32_t i = 0; i < count; i++) {
                int32_t v = (int32_t)(addr + i * vtx_size);
                int32_t x = MEM_H(vtx_x, v);
                int32_t delta;
                if (x >= bg_left_min - bg_extend && x <= bg_left_max - bg_extend) {
                    delta = -extend + bg_extend; // Left column, moved.
                }
                else if (x >= bg_left_min && x <= bg_left_max) {
                    delta = -extend; // Left column, original.
                }
                else if (x >= bg_right_min && x <= bg_right_max) {
                    delta = extend;
                }
                else if (x >= bg_right_min + bg_extend && x <= bg_right_max + bg_extend) {
                    delta = extend - bg_extend;
                }
                else {
                    continue;
                }
                MEM_H(vtx_x, v) = (int16_t)(x + delta);
                MEM_H(vtx_s, v) = (int16_t)(MEM_H(vtx_s, v) + delta * 128 / 5);
            }
        });
    }
}

// func_8007C624, before the view's G_SETSCISSOR is built. $t2..$t5 = ulx, uly, lrx, lry in pixels, $s1 = the view's
// viewport.
extern "C" void rush2_view_scissor(uint8_t* rdram, recomp_context* ctx) {
    int32_t ulx = (int32_t)ctx->r10;
    int32_t uly = (int32_t)ctx->r11;
    int32_t lrx = (int32_t)ctx->r12;
    int32_t lry = (int32_t)ctx->r13;
    bool left = ulx <= border_left;
    bool right = lrx >= border_right;
    bool top = uly <= border_top;
    bool bottom = lry >= border_bottom;

    // Views touching the border on both sides span the framebuffer, so RT64 widens them. A view touching one side
    // (each half of the 2 player car select) keeps its 4:3 projection, but its outer edge is moved to the window's
    // edge with an extended GBI origin (rush2_view_scissor_written), revealing the scene past the 4:3 area.
    pending_left_origin = G_EX_ORIGIN_NONE;
    pending_right_origin = G_EX_ORIGIN_NONE;
    if (left) {
        ulx = 0;
        if (!right) {
            pending_left_origin = G_EX_ORIGIN_LEFT;
        }
    }
    if (right) {
        lrx = screen_width;
        if (!left) {
            pending_right_origin = G_EX_ORIGIN_RIGHT;
        }
    }
    if (top) {
        uly = 0;
    }
    if (bottom) {
        lry = screen_height;
    }

    // In 2 player races the top view's viewport is moved down to start at the border (y = 10), so nothing draws
    // above it now that the scissor reaches the top edge. Move a viewport that leaves such a gap on one side back to
    // the edge, as long as it still covers the scissor's other side.
    int32_t vp = (int32_t)ctx->r17;
    int32_t scale_y = std::abs((int32_t)MEM_H(vp_scale_y, vp));
    int32_t trans_y = MEM_H(vp_trans_y, vp);
    int32_t vp_top = (trans_y - scale_y) / 4;
    int32_t vp_bottom = (trans_y + scale_y) / 4;
    int32_t gap_top = vp_top - uly;
    int32_t gap_bottom = lry - vp_bottom;
    if (top && gap_top > 0 && gap_top <= border_top && vp_bottom - gap_top >= lry) {
        MEM_H(vp_trans_y, vp) = (int16_t)(trans_y - gap_top * 4);
    }
    else if (bottom && gap_bottom > 0 && gap_bottom <= screen_height - border_bottom && vp_top + gap_bottom <= uly) {
        MEM_H(vp_trans_y, vp) = (int16_t)(trans_y + gap_bottom * 4);
    }

    ctx->r10 = ulx;
    ctx->r11 = uly;
    ctx->r12 = lrx;
    ctx->r13 = lry;
}

// func_8007C624, after the view's G_SETSCISSOR is written at $a1. Swaps it for a call to an extended GBI scissor
// when rush2_view_scissor anchored one of its edges to the window.
extern "C" void rush2_view_scissor_written(uint8_t* rdram, recomp_context* ctx) {
    if (pending_left_origin == G_EX_ORIGIN_NONE && pending_right_origin == G_EX_ORIGIN_NONE) {
        return;
    }

    uint32_t cmd_addr = (uint32_t)ctx->r5;
    uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)cmd_addr);
    uint32_t w1 = (uint32_t)MEM_W(4, (int32_t)cmd_addr);
    uint32_t mode = (w1 >> 24) & 3;
    int32_t ulx = (int32_t)((w0 >> 12) & 0xFFF) / 4;
    int32_t uly = (int32_t)(w0 & 0xFFF) / 4;
    int32_t lrx = (int32_t)((w1 >> 12) & 0xFFF) / 4;
    int32_t lry = (int32_t)(w1 & 0xFFF) / 4;
    // Coordinates are relative to their origin.
    if (pending_right_origin == G_EX_ORIGIN_RIGHT) {
        lrx -= screen_width;
    }

    constexpr uint32_t count = 3;
    GfxCommand cmds[count];
    gEXEnable(&cmds[0]);
    gEXSetScissor(&cmds[1], mode, pending_left_origin, pending_right_origin, ulx, uly, lrx, lry);

    uint32_t bytes = (count + 1) * 8;
    if (view_dl_cursor + bytes > view_dl_end) {
        view_dl_cursor = view_dl_start;
    }
    uint32_t dl = view_dl_cursor;
    for (uint32_t i = 0; i < count; i++) {
        MEM_W(0, (int32_t)(dl + i * 8 + 0)) = (int32_t)cmds[i].values.word0;
        MEM_W(0, (int32_t)(dl + i * 8 + 4)) = (int32_t)cmds[i].values.word1;
    }
    MEM_W(0, (int32_t)(dl + count * 8 + 0)) = (int32_t)0xDF000000; // G_ENDDL
    MEM_W(0, (int32_t)(dl + count * 8 + 4)) = 0;
    view_dl_cursor += bytes;

    MEM_W(0, (int32_t)(cmd_addr + 0)) = (int32_t)0xDE000000; // G_DL (call)
    MEM_W(0, (int32_t)(cmd_addr + 4)) = (int32_t)dl;
    pending_left_origin = G_EX_ORIGIN_NONE;
    pending_right_origin = G_EX_ORIGIN_NONE;
}

extern "C" void rush2_frame_clear_begin(uint8_t* rdram, recomp_context* ctx) {
    saved_inset_x = MEM_H(0, (int32_t)inset_x);
    saved_inset_y = MEM_H(0, (int32_t)inset_y);
    MEM_H(0, (int32_t)inset_x) = 0;
    MEM_H(0, (int32_t)inset_y) = 0;
}

extern "C" void rush2_frame_clear_end(uint8_t* rdram, recomp_context* ctx) {
    MEM_H(0, (int32_t)inset_x) = saved_inset_x;
    MEM_H(0, (int32_t)inset_y) = saved_inset_y;
}

// func_8007AA48, before it emits the G_DL for a scene graph node's model. $fp = model display list.
extern "C" void rush2_model_draw(uint8_t* rdram, recomp_context* ctx) {
    uint32_t dl = (uint32_t)ctx->r30;
    if (is_menu_background(rdram, dl)) {
        bool widescreen = ultramodern::renderer::get_graphics_config().ar_option != ultramodern::renderer::AspectRatio::Original;
        extend_menu_background(rdram, dl, widescreen ? bg_extend : 0);
    }
    // Rush 2049 wings on car bodies (src/wings_render.cpp).
    rush2_wings_model_draw(rdram, ctx);
}
