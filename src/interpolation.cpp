// Stable RT64 matrix group IDs for frame interpolation.
//
// Without tags RT64 pairs matrices between frames by guessing (G_EX_ID_AUTO), which mismatches identical
// objects such as cars lined up at the start of a race. The hooks in us.toml call into this file to tag every
// matrix the 3D renderer (func_8007C624, one call per player view) emits with an ID that stays the same while
// the object exists:
//
// - Scene graph nodes (func_8007B518): node index in the node pool. Each node's G_MTX and G_POPMTX is swapped
//   for a G_DL call into a side buffer that wraps the original command with a matrix group push/pop, so the
//   game's display list keeps its size (it rolls back the last command when a subtree draws nothing).
// - The per-view camera (projection load) and root modelview.
// - The world-space polygon list (skid marks, shadows, particles), whose vertices are camera-relative: each
//   polygon gets its own group with vertex interpolation, keyed by its slot (slots are stable between frames).
//
// IDs include a generation that changes when an object teleports or the camera cuts, so RT64 snaps instead of
// sweeping across the screen. Children inherit their ancestors' generations.

#include <cmath>
#include <cstring>

#include "recomp.h"
#include "rush2.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

namespace {
    // Game addresses.
    constexpr uint32_t node_pool = 0x800D9E90;  // Scene graph nodes, 0x38 bytes each.
    constexpr uint32_t node_size = 0x38;
    constexpr uint32_t max_nodes = 1024;
    constexpr uint32_t camera_array = 0x800E79D0; // Per-view cameras, 0x40 bytes each.
    constexpr uint32_t poly_array = 0x800FAF00;   // World-space polygons, 0x58 bytes each.
    constexpr uint32_t poly_size = 0x58;
    constexpr uint32_t max_polys = 4096;
    constexpr uint32_t main_dl_head = 0x80022FD8;
    constexpr uint32_t max_views = 4;

    // Side buffer for the wrapped commands, in unused RDRAM above the game's 4MB. RT64 reads display lists
    // asynchronously, so this is a ring large enough to hold many frames (a race uses a few KB per frame).
    constexpr uint32_t side_start = 0x80B00000;
    constexpr uint32_t side_end = 0x80C00000;
    uint32_t side_cursor = side_start;

    // F3DEX2 commands.
    constexpr uint32_t G_DL_CALL = 0xDE000000;
    constexpr uint32_t G_ENDDL_W0 = 0xDF000000;
    constexpr uint32_t G_MTX_LOAD_MODELVIEW_W0 = 0xDA380003;

    // The `proj` argument of the extended GBI matrix group commands.
    constexpr uint32_t G_MTX_MODELVIEW_EX = 0;
    constexpr uint32_t G_MTX_PROJECTION_EX = 1;

    // Teleport and camera cut detection, per game frame. While racing, cars and the camera move up to ~10 world
    // units and the camera turns well under 30 degrees; cuts and respawns jump hundreds of units.
    constexpr float node_teleport_distance = 32.0f;
    constexpr float poly_teleport_distance = 32.0f * 16.0f; // Polygon coordinates are 16x world units.
    constexpr float camera_cut_distance = 32.0f;
    constexpr float camera_cut_cos = 0.5f; // 60 degrees

    // ID layout (never 0 = G_EX_ID_IGNORE or 0xFFFFFFFF = G_EX_ID_AUTO):
    //   nodes:      1vvg gggg gggg gggg gggg ggnn nnnn nnnn  (bit 28 is always 0)
    //   polygons:   0001 vvgg gggg gggg gggg pppp pppp pppp
    //   projection: 0010 0000 0000 00vv gggg gggg gggg gggg
    //   root view:  0011 0000 0000 00vv gggg gggg gggg gggg
    uint32_t node_id(uint32_t view, uint32_t gen, uint32_t node) {
        return 0x80000000u | ((view & 3) << 29) | ((gen & 0x3FFFF) << 10) | (node & 0x3FF);
    }

    uint32_t poly_id(uint32_t view, uint32_t gen, uint32_t slot) {
        return 0x10000000u | ((view & 3) << 26) | ((gen & 0x3FFF) << 12) | (slot & 0xFFF);
    }

    uint32_t view_id(uint32_t kind, uint32_t view, uint32_t gen) {
        return kind | ((view & 3) << 16) | (gen & 0xFFFF);
    }

    struct Vec3 {
        float x, y, z;
    };

    float read_f32(uint8_t* rdram, uint32_t addr) {
        int32_t bits = MEM_W(0, (int32_t)addr);
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    }

    Vec3 read_vec3(uint8_t* rdram, uint32_t addr) {
        return { read_f32(rdram, addr), read_f32(rdram, addr + 4), read_f32(rdram, addr + 8) };
    }

    float dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    float distance(const Vec3& a, const Vec3& b) {
        Vec3 d{ a.x - b.x, a.y - b.y, a.z - b.z };
        return std::sqrt(dot(d, d));
    }

    // Writes commands into the side buffer, followed by G_ENDDL, and returns their address.
    uint32_t write_side_dl(uint8_t* rdram, const GfxCommand* cmds, uint32_t count) {
        uint32_t bytes = (count + 1) * 8;
        if (side_cursor + bytes > side_end) {
            side_cursor = side_start;
        }

        uint32_t addr = side_cursor;
        for (uint32_t i = 0; i < count; i++) {
            MEM_W(0, (int32_t)(addr + i * 8 + 0)) = (int32_t)cmds[i].values.word0;
            MEM_W(0, (int32_t)(addr + i * 8 + 4)) = (int32_t)cmds[i].values.word1;
        }
        MEM_W(0, (int32_t)(addr + count * 8 + 0)) = (int32_t)G_ENDDL_W0;
        MEM_W(0, (int32_t)(addr + count * 8 + 4)) = 0;

        side_cursor += bytes;
        return addr;
    }

    GfxCommand read_command(uint8_t* rdram, uint32_t addr) {
        GfxCommand cmd;
        cmd.values.word0 = (uint32_t)MEM_W(0, (int32_t)addr);
        cmd.values.word1 = (uint32_t)MEM_W(4, (int32_t)addr);
        return cmd;
    }

    // Replaces the 8-byte command at `cmd_addr` with a call to the given side display list.
    void replace_with_call(uint8_t* rdram, uint32_t cmd_addr, uint32_t side_dl) {
        MEM_W(0, (int32_t)(cmd_addr + 0)) = (int32_t)G_DL_CALL;
        MEM_W(0, (int32_t)(cmd_addr + 4)) = (int32_t)side_dl;
    }

    // Object matrices. Always interpolated: every model matrix includes the camera offset, so RT64's AUTO
    // heuristic (snap when the speed jumps >10x, with speed divided by the cosine of the direction change) mistakes
    // camera motion for teleports, e.g. at the corners of the race start flyover path. Real teleports and camera
    // cuts change the ID instead. Takes two command slots.
    void object_group(GfxCommand* cmd, uint32_t id, uint32_t push, uint32_t vert) {
        gEXMatrixGroup(cmd, id, G_EX_INTERPOLATE_DECOMPOSE, push, G_MTX_MODELVIEW_EX,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_AUTO,
            vert, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
    }

    struct NodeState {
        uint32_t transform = 0;
        Vec3 pos{};
        uint32_t gen = 0;
    };
    NodeState nodes[max_nodes];

    struct PolyState {
        Vec3 pos{};
        int16_t count = 0;
        uint32_t gen = 0;
    };
    PolyState polys[max_polys];

    struct ViewState {
        bool valid = false;
        Vec3 pos{};
        Vec3 fwd{};
        uint32_t gen = 0;
        uint32_t root_mtx = 0;
    };
    ViewState views[max_views];
    uint32_t cur_view = 0;

    // Generation sums down the current scene graph path. depth is the func_8007B518 recursion level.
    constexpr int max_depth = 64;
    int depth = -1;
    uint32_t level_base[max_depth];    // Generation inherited by nodes at this level.
    uint32_t level_current[max_depth]; // Generation of the node currently being drawn at this level.
}

extern "C" {

// func_8007C624 entry. $a0 = view index. Detects camera cuts.
void rush2_interp_view_begin(uint8_t* rdram, recomp_context* ctx) {
    cur_view = (uint32_t)ctx->r4 % max_views;
    depth = -1;

    uint32_t cam = camera_array + cur_view * 0x40;
    Vec3 pos = read_vec3(rdram, cam + 0x24);
    Vec3 fwd = read_vec3(rdram, cam + 0x18);
    ViewState& view = views[cur_view];
    if (view.valid) {
        float len = std::sqrt(dot(fwd, fwd) * dot(view.fwd, view.fwd));
        float cos_angle = len > 0.0f ? dot(fwd, view.fwd) / len : 1.0f;
        if (distance(pos, view.pos) > camera_cut_distance || cos_angle < camera_cut_cos) {
            view.gen++;
        }
    }
    view.valid = true;
    view.pos = pos;
    view.fwd = fwd;
}

// func_8007C624, after the projection G_MTX (load) is written at $v0. This is the first matrix of the view, so
// it also enables the extended GBI (RT64 disables it at the start of every display list).
void rush2_interp_projection(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cmd_addr = (uint32_t)ctx->r2;
    GfxCommand cmds[4];
    gEXEnable(&cmds[0]);
    // Cameras look best with simple (non-decomposed) interpolation, like RT64's default for projections.
    gEXMatrixGroup(&cmds[1], view_id(0x20000000u, cur_view, views[cur_view].gen), G_EX_INTERPOLATE_SIMPLE, G_EX_NOPUSH, G_MTX_PROJECTION_EX,
        G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,
        G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
        G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
    cmds[3] = read_command(rdram, cmd_addr);
    replace_with_call(rdram, cmd_addr, write_side_dl(rdram, cmds, 4));
}

// func_8007C624, after the root modelview G_MTX (load) is written at $v0.
void rush2_interp_root_modelview(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cmd_addr = (uint32_t)ctx->r2;
    GfxCommand cmds[3];
    object_group(&cmds[0], view_id(0x30000000u, cur_view, views[cur_view].gen), G_EX_NOPUSH, G_EX_COMPONENT_SKIP);
    cmds[2] = read_command(rdram, cmd_addr);
    views[cur_view].root_mtx = cmds[2].values.word1;
    replace_with_call(rdram, cmd_addr, write_side_dl(rdram, cmds, 3));
}

// func_8007C624, after the G_RDPPIPESYNC that starts each world-space polygon is written at $v0. $fp = polygon.
// Reloads the root modelview so RT64 creates a transform for the polygon, tagged with vertex interpolation.
void rush2_interp_poly(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cmd_addr = (uint32_t)ctx->r2;
    uint32_t poly = (uint32_t)ctx->r30;
    uint32_t slot = (poly - poly_array) / poly_size;
    if (slot >= max_polys || views[cur_view].root_mtx == 0) {
        return;
    }

    // Vertex count at +0x0, first vertex position at +0x8.
    PolyState& state = polys[slot];
    Vec3 pos = read_vec3(rdram, poly + 0x8);
    int16_t count = MEM_H(0, (int32_t)poly);
    if (count != state.count || distance(pos, state.pos) > poly_teleport_distance) {
        state.gen++;
    }
    state.pos = pos;
    state.count = count;

    GfxCommand cmds[4];
    cmds[0] = read_command(rdram, cmd_addr);
    object_group(&cmds[1], poly_id(cur_view, views[cur_view].gen + state.gen, slot), G_EX_NOPUSH, G_EX_COMPONENT_INTERPOLATE);
    cmds[3].values.word0 = G_MTX_LOAD_MODELVIEW_W0;
    cmds[3].values.word1 = views[cur_view].root_mtx;
    replace_with_call(rdram, cmd_addr, write_side_dl(rdram, cmds, 4));
}

// func_8007C624 exit. Resets the group IDs so later draws (HUD, other views) don't inherit them.
void rush2_interp_view_end(uint8_t* rdram, recomp_context* ctx) {
    GfxCommand cmds[4];
    gEXMatrixGroupSimple(&cmds[0], G_EX_ID_AUTO, G_EX_NOPUSH, G_MTX_MODELVIEW_EX,
        G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO,
        G_EX_ORDER_AUTO, G_EX_EDIT_NONE, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
    gEXMatrixGroupSimple(&cmds[2], G_EX_ID_AUTO, G_EX_NOPUSH, G_MTX_PROJECTION_EX,
        G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO,
        G_EX_ORDER_AUTO, G_EX_EDIT_NONE, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
    uint32_t side_dl = write_side_dl(rdram, cmds, 4);

    uint32_t head = (uint32_t)MEM_W(0, (int32_t)main_dl_head);
    replace_with_call(rdram, head, side_dl);
    MEM_W(0, (int32_t)main_dl_head) = (int32_t)(head + 8);
}

// The current view and the generation of the node being drawn, for other code that adds matrices under it
// (src/wings_render.cpp).
void rush2_interp_get_generation(uint32_t* view, uint32_t* gen) {
    *view = cur_view;
    *gen = views[cur_view].gen + ((depth >= 0 && depth < max_depth) ? level_current[depth] : 0);
}

// func_8007B518 entry: one scene graph level deeper.
void rush2_interp_level_enter(uint8_t* rdram, recomp_context* ctx) {
    int parent = depth;
    depth++;
    if (depth < max_depth) {
        level_base[depth] = (parent >= 0 && parent < max_depth) ? level_current[parent] : 0;
        level_current[depth] = level_base[depth];
    }
}

// func_8007B518 exit.
void rush2_interp_level_exit(uint8_t* rdram, recomp_context* ctx) {
    if (depth >= 0) {
        depth--;
    }
}

// func_8007B518 loop head, before each sibling node.
void rush2_interp_node_begin(uint8_t* rdram, recomp_context* ctx) {
    if (depth >= 0 && depth < max_depth) {
        level_current[depth] = level_base[depth];
    }
}

// func_8007B518, after a node's G_MTX (push or no-push) is written at $v0. $s7 = node.
void rush2_interp_node_matrix(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cmd_addr = (uint32_t)ctx->r2;
    uint32_t node = (uint32_t)ctx->r23;
    uint32_t index = (node - node_pool) / node_size;

    // Nodes past the fixed-point range under an extended draw distance have float matrices (src/draw_distance.cpp).
    // F3DEX2 stores the G_MTX params XORed with G_MTX_PUSH; gEXMatrixFloat takes them the same way.
    GfxCommand mtx[2];
    uint32_t mtx_count = 1;
    mtx[0] = read_command(rdram, cmd_addr);
    uint32_t params = mtx[0].values.word0 & 0xFF;
    uint32_t matrix = mtx[0].values.word1;
    if (rush2::draw_distance_take_float_matrix(matrix)) {
        gEXMatrixFloat(&mtx[0], matrix, params);
        mtx_count = 2;
    }

    if (index >= max_nodes || depth < 0 || depth >= max_depth) {
        if (mtx_count == 2) {
            replace_with_call(rdram, cmd_addr, write_side_dl(rdram, mtx, mtx_count));
        }
        return;
    }

    // The node's transform (+0x4): 3x3 rotation, then the translation at +0x24 (world space for root nodes).
    // A different transform means the node was reused for another object.
    NodeState& state = nodes[index];
    uint32_t transform = (uint32_t)MEM_W(4, (int32_t)node);
    Vec3 pos = read_vec3(rdram, transform + 0x24);
    if (transform != state.transform || distance(pos, state.pos) > node_teleport_distance) {
        state.gen++;
    }
    state.transform = transform;
    state.pos = pos;

    uint32_t gen = level_base[depth] + state.gen;
    level_current[depth] = gen;

    GfxCommand cmds[4];
    uint32_t push = (params ^ 1) & 1;
    object_group(&cmds[0], node_id(cur_view, views[cur_view].gen + gen, index), push, G_EX_COMPONENT_SKIP);
    for (uint32_t i = 0; i < mtx_count; i++) {
        cmds[2 + i] = mtx[i];
    }
    replace_with_call(rdram, cmd_addr, write_side_dl(rdram, cmds, 2 + mtx_count));
}

// func_8007B518, after a pushed node finishes (L_8007BD7C). If anything was drawn ($s2 != 0) the game wrote a
// G_POPMTX just before the display list cursor at $sp+0x10C; otherwise it rolled back the node's G_MTX (our
// G_DL), and there is nothing to pop.
void rush2_interp_node_pop(uint8_t* rdram, recomp_context* ctx) {
    if (ctx->r18 == 0) {
        return;
    }

    uint32_t cmd_addr = (uint32_t)MEM_W(0x10C, ctx->r29) - 8;
    GfxCommand cmds[2];
    cmds[0] = read_command(rdram, cmd_addr);
    gEXPopMatrixGroup(&cmds[1], G_MTX_MODELVIEW_EX);
    replace_with_call(rdram, cmd_addr, write_side_dl(rdram, cmds, 2));
}

}
