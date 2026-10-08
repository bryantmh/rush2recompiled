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

#include <iterator>
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

    // Nodes that ride with a view's camera (HUD models, rush2::interpolation_view_attached): they move as far as
    // the camera does each frame, which is no teleport.
    uint32_t attached_nodes[16];
    int attached_count = 0;

    // Nodes drawn in a primitive color of their own (rush2::interpolation_node_color): the color is set with the
    // node's matrix. Rush 2's nodes carry no color, so a model that uses the primitive color takes whatever was set
    // last.
    struct NodeColor {
        uint32_t node, rgba;
    };
    NodeColor node_colors[96];
    int node_color_count = 0;

    bool view_attached(uint32_t node) {
        for (int i = 0; i < attached_count; i++) {
            if (attached_nodes[i] == node) {
                return true;
            }
        }
        return false;
    }

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
    GfxCommand level_mtx[max_depth];   // G_MTX of the node currently being drawn at this level (word1 0: none).
    bool level_mtx_float[max_depth];   // Its matrix is floats (src/draw_distance.cpp).

    // N64 fixed point matrix (s15.16, row vectors): integer halves then fractions. Or 16 floats by rows.
    void read_mtx(uint8_t* rdram, uint32_t addr, bool is_float, float m[4][4]) {
        if (is_float) {
            for (int i = 0; i < 16; i++) {
                m[i / 4][i % 4] = read_f32(rdram, addr + i * 4);
            }
            return;
        }
        for (int i = 0; i < 8; i++) {
            uint32_t hi = (uint32_t)MEM_W(0, (int32_t)(addr + i * 4));
            uint32_t lo = (uint32_t)MEM_W(0, (int32_t)(addr + 0x20 + i * 4));
            int32_t a = (int32_t)((hi & 0xFFFF0000) | (lo >> 16));
            int32_t b = (int32_t)((hi << 16) | (lo & 0xFFFF));
            m[(i * 2) / 4][(i * 2) % 4] = a / 65536.0f;
            m[(i * 2 + 1) / 4][(i * 2 + 1) % 4] = b / 65536.0f;
        }
    }

    // out = a * b (G_MTX_MUL with a as the argument and b the current matrix).
    void mul_mtx(const float a[4][4], const float b[4][4], float out[4][4]) {
        float r[4][4];
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
            }
        }
        memcpy(out, r, sizeof(r));
    }

    uint32_t kseg0(uint32_t addr) {
        return 0x80000000u | (addr & 0x00FFFFFF);
    }

    // Sky clipping (see rush2_interp_poly_end).
    uint32_t view_projection[max_views]; // The view's projection G_MTX address.
    uint32_t clip_poly = 0;              // The polygon whose G_VTX is at clip_vtx_cmd.
    uint32_t clip_vtx_cmd = 0;

    // Clip-space w below which the sky's triangles are cut, in polygon units (16x world units, camera-relative).
    // The dome is thousands of units away, so this plane only removes parts more than ~80 degrees off the view
    // direction, which are never on screen; it stays in front of the camera through the rotation of RT64's
    // interpolated frames.
    constexpr float sky_clip_w = 4096.0f;

    struct ClipVtx {
        float pos[3];
        float tc[2];
        float col[4];
        float w;
    };

    ClipVtx read_vtx(uint8_t* rdram, uint32_t addr, const float m[4][4]) {
        ClipVtx v;
        for (int i = 0; i < 3; i++) {
            v.pos[i] = (float)(int16_t)MEM_H(i * 2, (int32_t)addr);
        }
        v.tc[0] = (float)(int16_t)MEM_H(8, (int32_t)addr);
        v.tc[1] = (float)(int16_t)MEM_H(10, (int32_t)addr);
        for (int i = 0; i < 4; i++) {
            v.col[i] = (float)MEM_BU(12 + i, (int32_t)addr);
        }
        v.w = v.pos[0] * m[0][3] + v.pos[1] * m[1][3] + v.pos[2] * m[2][3] + m[3][3];
        return v;
    }

    ClipVtx lerp_vtx(const ClipVtx& a, const ClipVtx& b, float t) {
        ClipVtx v;
        for (int i = 0; i < 3; i++) v.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
        for (int i = 0; i < 2; i++) v.tc[i] = a.tc[i] + (b.tc[i] - a.tc[i]) * t;
        for (int i = 0; i < 4; i++) v.col[i] = a.col[i] + (b.col[i] - a.col[i]) * t;
        v.w = a.w + (b.w - a.w) * t;
        return v;
    }

    void write_vtx(uint8_t* rdram, uint32_t addr, const ClipVtx& v) {
        for (int i = 0; i < 3; i++) {
            MEM_H(i * 2, (int32_t)addr) = (int16_t)std::lround(v.pos[i]);
        }
        MEM_H(6, (int32_t)addr) = 0;
        MEM_H(8, (int32_t)addr) = (int16_t)std::lround(v.tc[0]);
        MEM_H(10, (int32_t)addr) = (int16_t)std::lround(v.tc[1]);
        for (int i = 0; i < 4; i++) {
            MEM_B(12 + i, (int32_t)addr) = (uint8_t)std::lround(std::fmin(std::fmax(v.col[i], 0.0f), 255.0f));
        }
    }

    // Reserves bytes (8-aligned) in the side buffer.
    uint32_t side_alloc(uint32_t bytes) {
        bytes = (bytes + 7) & ~7u;
        if (side_cursor + bytes > side_end) {
            side_cursor = side_start;
        }
        uint32_t addr = side_cursor;
        side_cursor += bytes;
        return addr;
    }
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
    view_projection[cur_view] = cmds[3].values.word1;
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

// func_8007C624 at 0x8007D740, after the G_VTX of a world-space polygon is allocated at $v0. $fp = polygon.
void rush2_interp_poly_vtx(uint8_t* rdram, recomp_context* ctx) {
    clip_poly = (uint32_t)ctx->r30;
    clip_vtx_cmd = (uint32_t)ctx->r2;
}

// func_8007C624 at L_8007D890, after a world-space polygon's G_VTX and triangles. $fp = polygon.
//
// The sky dome's polygons (flag 0x2000) are drawn at the primitive depth 0x7FFF so they stay behind everything.
// RT64 outputs prim depth as z = 0.99997 * w, so the GPU's near clip (z >= 0) lands on the camera plane (w = 0)
// instead of the projection's near plane. The dome surrounds the camera, so many of its triangles have a vertex
// beside or behind it: the overhead cap's center is within a few degrees of the camera plane whenever the camera is
// level, and the lower rings wrap around past the widescreen edges. Those triangles get cut into vertices at w = 0
// that project to infinity, and draw stretched (striped) or drop out, flickering as the camera pitches and turns.
// The RSP clips them at the real near plane instead. Triangles with a vertex below sky_clip_w are clipped here
// and redrawn from a side display list, with their texture coordinates and colors interpolated along the edges.
void rush2_interp_poly_end(uint8_t* rdram, recomp_context* ctx) {
    uint32_t poly = (uint32_t)ctx->r30;
    uint32_t vtx_cmd = clip_vtx_cmd;
    bool ours = clip_poly == poly;
    clip_poly = 0;
    if (!ours || (MEM_H(2, (int32_t)poly) & 0x2000) == 0) {
        return;
    }
    uint32_t root = views[cur_view].root_mtx;
    uint32_t proj = view_projection[cur_view];
    if (root == 0 || proj == 0) {
        return;
    }

    // G_VTX: 0x01 nnn0 eeee (n vertices, e = end index * 2), then the triangles up to the display list cursor.
    GfxCommand vtx = read_command(rdram, vtx_cmd);
    uint32_t n = (vtx.values.word0 >> 12) & 0xFF;
    uint32_t head = (uint32_t)MEM_W(0, (int32_t)main_dl_head);
    if ((vtx.values.word0 >> 24) != 0x01 || n == 0 || n > 4 || head <= vtx_cmd + 8 || head - vtx_cmd > 8 * 4) {
        return;
    }
    uint32_t first = ((vtx.values.word0 >> 1) & 0x7F) - n;

    float mv[4][4], p[4][4], m[4][4];
    read_mtx(rdram, kseg0(root), false, mv);
    read_mtx(rdram, kseg0(proj), false, p);
    mul_mtx(mv, p, m);

    ClipVtx in[4];
    bool any_near = false;
    for (uint32_t i = 0; i < n; i++) {
        in[i] = read_vtx(rdram, kseg0(vtx.values.word1) + i * 16, m);
        any_near |= in[i].w < sky_clip_w;
    }
    if (!any_near) {
        return;
    }

    // The triangles, in the game's winding.
    int tris[4][3];
    int tri_count = 0;
    auto add_tri = [&](uint32_t word) {
        int a = (int)((word >> 16) & 0xFF) / 2 - (int)first;
        int b = (int)((word >> 8) & 0xFF) / 2 - (int)first;
        int c = (int)(word & 0xFF) / 2 - (int)first;
        if (a < 0 || b < 0 || c < 0 || a >= (int)n || b >= (int)n || c >= (int)n || tri_count >= 4) {
            return false;
        }
        tris[tri_count][0] = a;
        tris[tri_count][1] = b;
        tris[tri_count][2] = c;
        tri_count++;
        return true;
    };
    for (uint32_t addr = vtx_cmd + 8; addr < head; addr += 8) {
        GfxCommand cmd = read_command(rdram, addr);
        uint32_t op = cmd.values.word0 >> 24;
        if (op == 0x05) {
            if (!add_tri(cmd.values.word0)) return;
        }
        else if (op == 0x06) {
            if (!add_tri(cmd.values.word0) || !add_tri(cmd.values.word1)) return;
        }
        else {
            return;
        }
    }

    // Clip each triangle against w >= sky_clip_w (Sutherland-Hodgman: at most 4 vertices each), as a fan.
    ClipVtx out[16];
    int out_count = 0;
    int out_tris[8][3];
    int out_tri_count = 0;
    for (int t = 0; t < tri_count; t++) {
        ClipVtx poly_in[3] = { in[tris[t][0]], in[tris[t][1]], in[tris[t][2]] };
        ClipVtx clipped[4];
        int k = 0;
        for (int i = 0; i < 3; i++) {
            const ClipVtx& a = poly_in[i];
            const ClipVtx& b = poly_in[(i + 1) % 3];
            bool a_in = a.w >= sky_clip_w;
            bool b_in = b.w >= sky_clip_w;
            if (a_in) {
                clipped[k++] = a;
            }
            if (a_in != b_in) {
                clipped[k++] = lerp_vtx(a, b, (sky_clip_w - a.w) / (b.w - a.w));
            }
        }
        if (k < 3) {
            continue;
        }
        int base = out_count;
        for (int i = 0; i < k; i++) {
            out[out_count++] = clipped[i];
        }
        for (int i = 1; i + 1 < k; i++) {
            out_tris[out_tri_count][0] = base;
            out_tris[out_tri_count][1] = base + i;
            out_tris[out_tri_count][2] = base + i + 1;
            out_tri_count++;
        }
    }

    // The original triangles become no-ops; the G_VTX calls the clipped vertices and triangles.
    for (uint32_t addr = vtx_cmd + 8; addr < head; addr += 8) {
        MEM_W(0, (int32_t)addr) = (int32_t)0xE7000000; // G_RDPPIPESYNC
        MEM_W(4, (int32_t)addr) = 0;
    }
    if (out_tri_count == 0) {
        MEM_W(0, (int32_t)vtx_cmd) = (int32_t)0xE7000000;
        MEM_W(4, (int32_t)vtx_cmd) = 0;
        return;
    }

    uint32_t vtx_addr = side_alloc(out_count * 16);
    for (int i = 0; i < out_count; i++) {
        write_vtx(rdram, vtx_addr + i * 16, out[i]);
    }
    GfxCommand cmds[1 + 8];
    cmds[0].values.word0 = 0x01000000u | ((uint32_t)out_count << 12) | ((uint32_t)out_count << 1);
    cmds[0].values.word1 = vtx_addr;
    for (int i = 0; i < out_tri_count; i++) {
        cmds[1 + i].values.word0 = 0x05000000u | (out_tris[i][0] * 2 << 16) | (out_tris[i][1] * 2 << 8) | (out_tris[i][2] * 2);
        cmds[1 + i].values.word1 = 0;
    }
    replace_with_call(rdram, vtx_cmd, write_side_dl(rdram, cmds, 1 + out_tri_count));
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
        level_mtx[depth].values.word1 = 0;
    }
}

// The modelview the RSP has while the current node draws: the view's root modelview times the G_MTXs of the nodes
// down the scene graph path. For drawing parts of a node later in the view (src/car2049.cpp).
bool rush2_interp_get_modelview(uint8_t* rdram, float out[4][4]) {
    if (views[cur_view].root_mtx == 0 || depth < 0 || depth >= max_depth) {
        return false;
    }
    read_mtx(rdram, kseg0(views[cur_view].root_mtx), false, out);
    for (int d = 0; d <= depth; d++) {
        if (level_mtx[d].values.word1 == 0) {
            continue;
        }
        float m[4][4];
        read_mtx(rdram, kseg0(level_mtx[d].values.word1), level_mtx_float[d], m);
        if (level_mtx[d].values.word0 & 2) { // G_MTX_LOAD
            memcpy(out, m, sizeof(m));
        }
        else {
            mul_mtx(m, out, out);
        }
    }
    return true;
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
    level_mtx[depth] = read_command(rdram, cmd_addr);
    level_mtx_float[depth] = mtx_count == 2;

    // The node's transform (+0x4): 3x3 rotation, then the translation at +0x24 (world space for root nodes).
    // A different transform means the node was reused for another object.
    NodeState& state = nodes[index];
    uint32_t transform = (uint32_t)MEM_W(4, (int32_t)node);
    Vec3 pos = read_vec3(rdram, transform + 0x24);
    if (transform != state.transform ||
        (distance(pos, state.pos) > node_teleport_distance && !view_attached((uint32_t)node))) {
        state.gen++;
    }
    state.transform = transform;
    state.pos = pos;

    uint32_t gen = level_base[depth] + state.gen;
    level_current[depth] = gen;

    GfxCommand cmds[5];
    uint32_t push = (params ^ 1) & 1;
    object_group(&cmds[0], node_id(cur_view, views[cur_view].gen + gen, index), push, G_EX_COMPONENT_SKIP);
    for (uint32_t i = 0; i < mtx_count; i++) {
        cmds[2 + i] = mtx[i];
    }
    uint32_t cmd_count = 2 + mtx_count;
    for (int i = 0; i < node_color_count; i++) {
        if (node_colors[i].node == (uint32_t)node) {
            cmds[cmd_count].values.word0 = 0xFA000000; // G_SETPRIMCOLOR
            cmds[cmd_count].values.word1 = node_colors[i].rgba;
            cmd_count++;
            break;
        }
    }
    replace_with_call(rdram, cmd_addr, write_side_dl(rdram, cmds, cmd_count));
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

void rush2::interpolation_clear_view_attached() {
    attached_count = 0;
    node_color_count = 0;
}

void rush2::interpolation_node_color(uint32_t node, uint32_t rgba) {
    if (node == 0) {
        return;
    }
    for (int i = 0; i < node_color_count; i++) {
        if (node_colors[i].node == node) {
            node_colors[i].rgba = rgba;
            return;
        }
    }
    if (node_color_count < (int)std::size(node_colors)) {
        node_colors[node_color_count++] = { node, rgba };
    }
}

void rush2::interpolation_view_attached(uint32_t node) {
    if (node != 0 && !view_attached(node) && attached_count < (int)std::size(attached_nodes)) {
        attached_nodes[attached_count++] = node;
    }
}
