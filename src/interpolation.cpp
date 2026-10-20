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
// - Nodes that ride with a view's camera (the battle arenas' HUD models) are drawn in view space instead: see
//   rush2_interp_node_matrix.
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
#include "calspeed_billboards.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"
#include "scene_nodes.h"

namespace {
    // Game addresses.
    constexpr uint32_t node_pool = rush2::scene_nodes::table;  // Scene graph nodes, 0x38 bytes each.
    constexpr uint32_t node_size = 0x38;
    constexpr uint32_t max_nodes = rush2::scene_nodes::capacity; // node_id holds 12 bits of index
    static_assert(max_nodes <= 0x1000);
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
    constexpr uint32_t G_MTX_LOAD_PROJECTION_W0 = 0xDA380007;
    constexpr uint32_t G_MTX_MUL_PROJECTION_W0 = 0xDA380005;

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
    //   nodes:      1vv0 gggg gggg gggg gggg nnnn nnnn nnnn  (12 bits of node: the table holds 2,340, scene_nodes.h)
    //   polygons:   0001 vvgg gggg gggg gggg pppp pppp pppp
    //   projection: 0010 0000 0000 00vv gggg gggg gggg gggg
    //   view space: 0010 0001 0000 00vv 0000 0000 0000 0000  (the projection of camera-attached nodes)
    //   root view:  0011 0000 0000 00vv gggg gggg gggg gggg
    uint32_t node_id(uint32_t view, uint32_t gen, uint32_t node) {
        return 0x80000000u | ((view & 3) << 29) | ((gen & 0xFFFF) << 12) | (node & 0xFFF);
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
        float scale = 0;
        uint32_t gen = 0;
    };
    NodeState nodes[max_nodes];

    // Nodes that ride with a view's camera (HUD models, rush2::interpolation_view_attached): they move as far as
    // the camera does each frame, which is no teleport, and are drawn in view space (rush2_interp_node_matrix).
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

    // Nodes drawn under another node's ID (rush2::interpolation_node_alias): a custom track's flip-book frames, one
    // shown at a time on a shared pivot. A frame shown after being hidden has no group in RT64's previous frame to
    // pair with, so it was drawn unblended for a game frame while the camera, whose position is in every node's
    // matrix, was blended: the frame shook against the walls around it at every step. Under the book's ID it pairs
    // with the frame it replaces, whose matrix is the same.
    struct NodeAlias {
        uint32_t node, leader;
    };
    NodeAlias node_aliases[256];
    int node_alias_count = 0;

    uint32_t alias_of(uint32_t node) {
        for (int i = 0; i < node_alias_count; i++) {
            if (node_aliases[i].node == node) {
                return node_aliases[i].leader;
            }
        }
        return 0;
    }

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
    bool level_view_space[max_depth];  // It is drawn in view space: its pop puts the view's projection back.

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
    uint32_t view_lookat[max_views];     // The view's rotation, which the game multiplies into the projection.

    // The view's projection group. Cameras look best with simple (non-decomposed) interpolation, like RT64's default
    // for projections. Takes two command slots.
    void projection_group(GfxCommand* cmd, uint32_t id) {
        gEXMatrixGroup(cmd, id, G_EX_INTERPOLATE_SIMPLE, G_EX_NOPUSH, G_MTX_PROJECTION_EX,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
    }
    uint32_t clip_poly = 0;              // The polygon whose G_VTX is at clip_vtx_cmd.
    uint32_t clip_vtx_cmd = 0;

    // Clip-space w below which the sky's triangles are cut, in polygon units (16x world units, camera-relative):
    // 16 ft in front of the camera. The dome's lid is flat at a fixed 900 ft world height (tables at 0x800C1AA4,
    // render_sky_cloud_scroll_8008918C moves the dome with the camera in x and z only) and Alcatraz's road climbs to
    // 733 ft, so at its highest jump the lid is within 150 ft overhead: cutting at 256 ft (4096) removed the lid above
    // 25 degrees up and the clear color showed at the top of the screen. The interpolated frames stay in front of
    // this plane because the triangles are clipped under both frames' planes (SkyClipPlane); the w of a vertex in
    // them differs from the blend of its two w's only by the camera's movement times its turn in one frame, a few units.
    constexpr float sky_clip_w = 256.0f;

    // The clip-space w column of each view's full transform (root modelview x view rotation x projection), this game
    // frame and the last. RT64 draws the frames between the two with the projection interpolated element by element
    // (projection_group), so a vertex's w there is a blend of its w under the two; the sky is clipped against both
    // planes so no interpolated frame puts a sky vertex behind the camera during a fast turn.
    struct SkyClipPlane {
        bool valid = false;
        float col[4]; // w = x * col[0] + y * col[1] + z * col[2] + col[3]
        uint32_t gen = 0;
    };
    SkyClipPlane sky_plane_cur[max_views];
    SkyClipPlane sky_plane_prev[max_views];

    struct ClipVtx {
        float pos[3];
        float tc[2];
        float col[4];
        float w[2]; // Under the current frame's plane and the last frame's.
    };

    float plane_w(const float pos[3], const SkyClipPlane& p) {
        return pos[0] * p.col[0] + pos[1] * p.col[1] + pos[2] * p.col[2] + p.col[3];
    }

    ClipVtx read_vtx(uint8_t* rdram, uint32_t addr, const SkyClipPlane& cur, const SkyClipPlane& prev) {
        ClipVtx v;
        for (int i = 0; i < 3; i++) {
            v.pos[i] = (float)(int16_t)MEM_H(i * 2, (int32_t)addr);
        }
        v.tc[0] = (float)(int16_t)MEM_H(8, (int32_t)addr);
        v.tc[1] = (float)(int16_t)MEM_H(10, (int32_t)addr);
        for (int i = 0; i < 4; i++) {
            v.col[i] = (float)MEM_BU(12 + i, (int32_t)addr);
        }
        v.w[0] = plane_w(v.pos, cur);
        v.w[1] = prev.valid ? plane_w(v.pos, prev) : v.w[0];
        return v;
    }

    ClipVtx lerp_vtx(const ClipVtx& a, const ClipVtx& b, float t) {
        ClipVtx v;
        for (int i = 0; i < 3; i++) v.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
        for (int i = 0; i < 2; i++) v.tc[i] = a.tc[i] + (b.tc[i] - a.tc[i]) * t;
        for (int i = 0; i < 4; i++) v.col[i] = a.col[i] + (b.col[i] - a.col[i]) * t;
        for (int i = 0; i < 2; i++) v.w[i] = a.w[i] + (b.w[i] - a.w[i]) * t;
        return v;
    }

    // The point where the edge from a (kept: w >= sky_clip_w under both planes) to b (cut under one or both) leaves
    // the kept region: the first of its crossings of the planes b is behind.
    ClipVtx exit_vtx(const ClipVtx& a, const ClipVtx& b) {
        float t = 1.0f;
        for (int p = 0; p < 2; p++) {
            if (b.w[p] < sky_clip_w) {
                t = std::fmin(t, (sky_clip_w - a.w[p]) / (b.w[p] - a.w[p]));
            }
        }
        return lerp_vtx(a, b, t);
    }

    // Clips the triangle v[0..2] to the kept region into six slots, a convex polygon in the triangle's winding: slot
    // 2i is vertex i or, when it is cut, the point where the edge from its previous neighbor leaves the region, slot
    // 2i + 1 the same toward its next neighbor (both the vertex itself when it is kept, both the one exit point when
    // only one neighbor is kept). The slot count and order never change, so RT64's vertex interpolation, which pairs
    // a group's vertices by index when their counts match (GameFrame::matchScene, lib/rt64 rt64_game_frame.cpp),
    // pairs each slot with itself in the last frame, and each slot moves continuously as the camera turns.
    // Sutherland-Hodgman output, whose count and order depend on which vertices are cut, paired unrelated corners
    // whenever the counts happened to match and swung whole triangles across the sky in the interpolated frames.
    // Returns false when no vertex is kept: the slots are then one point and draw nothing.
    bool clip_tri_slots(const ClipVtx v[3], ClipVtx out[6]) {
        bool in[3];
        int kept = 0;
        for (int i = 0; i < 3; i++) {
            in[i] = v[i].w[0] >= sky_clip_w && v[i].w[1] >= sky_clip_w;
            kept += in[i];
        }
        if (kept == 0) {
            for (int i = 0; i < 6; i++) out[i] = v[0];
            return false;
        }
        for (int i = 0; i < 3; i++) {
            int prev = (i + 2) % 3, next = (i + 1) % 3;
            if (in[i]) {
                out[2 * i] = out[2 * i + 1] = v[i];
            } else if (in[prev] && in[next]) {
                out[2 * i] = exit_vtx(v[prev], v[i]);
                out[2 * i + 1] = exit_vtx(v[next], v[i]);
            } else {
                out[2 * i] = out[2 * i + 1] = exit_vtx(v[in[prev] ? prev : next], v[i]);
            }
        }
        return true;
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

    if (sky_plane_cur[cur_view].valid) {
        sky_plane_prev[cur_view] = sky_plane_cur[cur_view];
    }
    sky_plane_cur[cur_view].valid = false;
}

// func_8007C624, after the projection G_MTX (load) is written at $v0. This is the first matrix of the view, so
// it also enables the extended GBI (RT64 disables it at the start of every display list).
void rush2_interp_projection(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cmd_addr = (uint32_t)ctx->r2;
    GfxCommand cmds[6];
    uint32_t count = 0;
    gEXEnable(&cmds[count++]);
    // The draw distance's fog, which needs the extended GBI enabled.
    float fog_mul, fog_offset;
    if (rush2::draw_distance_take_fog(fog_mul, fog_offset)) {
        uint32_t mul_bits, offset_bits;
        memcpy(&mul_bits, &fog_mul, sizeof(mul_bits));
        memcpy(&offset_bits, &fog_offset, sizeof(offset_bits));
        gEXSetFogFloat(&cmds[count], mul_bits, offset_bits);
        count += 2;
    }
    projection_group(&cmds[count], view_id(0x20000000u, cur_view, views[cur_view].gen));
    count += 2;
    cmds[count] = read_command(rdram, cmd_addr);
    view_projection[cur_view] = cmds[count].values.word1;
    view_lookat[cur_view] = 0;
    count++;
    replace_with_call(rdram, cmd_addr, write_side_dl(rdram, cmds, count));
}

// func_8007C624, after the root modelview G_MTX (load) is written at $v0.
// Also points RSP segment 0x0D at this view's camera-facing turn for the California Speed billboard trees
// (src/calspeed/calspeed_billboards.cpp): view_billboard_basis_8007BF2C's yaw-only rotation (0x800E7C78, f32 4x4,
// set for this view earlier in func_8007C624) as an Mtx in the side buffer, one per view and frame, built from it as
// the game builds its own camera-facing nodes' matrices.
void rush2_interp_root_modelview(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cmd_addr = (uint32_t)ctx->r2;
    GfxCommand cmds[4];
    object_group(&cmds[0], view_id(0x30000000u, cur_view, views[cur_view].gen), G_EX_NOPUSH, G_EX_COMPONENT_SKIP);
    cmds[2] = read_command(rdram, cmd_addr);
    constexpr uint32_t billboard_basis = 0x800E7C78;
    uint32_t turn = side_alloc(64);
    for (int i = 0; i < 16; i++) {
        int row = i / 4, col = i % 4;
        // By columns, as gfx_build_node_matrix_cols_8007B290 builds a flag 0x8000 node's matrix from it: Mtx row r is
        // the basis's column r (rows taken as they are turn the card the other way about y).
        float f = row < 3 && col < 3 ? read_f32(rdram, billboard_basis + (col * 4 + row) * 4) : (row == 3 && col == 3 ? 1.0f : 0.0f);
        int32_t fixed = (int32_t)std::lround(f * 65536.0f);
        MEM_H(i * 2, (int32_t)turn) = (int16_t)(fixed >> 16);
        MEM_H(32 + i * 2, (int32_t)turn) = (int16_t)(fixed & 0xFFFF);
    }
    cmds[3].values.word0 = 0xDB060000u | (rush2::calspeed::billboard_segment * 4);  // G_MOVEWORD G_MW_SEGMENT
    cmds[3].values.word1 = turn;
    views[cur_view].root_mtx = cmds[2].values.word1;
    // The view's rotation is the matrix the game multiplied into the projection a few commands back.
    for (uint32_t back = 1; back <= 8; back++) {
        GfxCommand before = read_command(rdram, cmd_addr - back * 8);
        if (before.values.word0 == G_MTX_MUL_PROJECTION_W0) {
            view_lookat[cur_view] = before.values.word1;
            break;
        }
    }
    replace_with_call(rdram, cmd_addr, write_side_dl(rdram, cmds, 4));
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

// sky_setup_800A45A8 at 0x800A48F8: $s7 = the dome's primitive depth (0x7FFF), stored at each piece's +6.
// The dome draws after the world with Z_CMP so it only fills where nothing was drawn. RT64 turns depth 0x7FFF into
// exactly 1.0 ((z & 0x7FFF) / 32767), the same as the game's Z clear (0xFFFC), and compares with LESS, so every sky
// pixel failed and the fog colour showed instead of the clouds. One step nearer (0.99994) passes against the clear and
// is still behind any geometry the far plane lets through.
void rush2_sky_depth(uint8_t* rdram, recomp_context* ctx) {
    ctx->r23 = 0x7FFE;
}

// func_8007C624 at L_8007D890, after a world-space polygon's G_VTX and triangles. $fp = polygon.
//
// The sky dome's polygons (flag 0x2000) are drawn at the primitive depth 0x7FFE so they stay behind everything
// (rush2_sky_depth). RT64 outputs prim depth as z = 0.99994 * w, so the GPU's near clip (z >= 0) lands on the camera plane (w = 0)
// instead of the projection's near plane. The dome surrounds the camera, so many of its triangles have a vertex
// beside or behind it: the overhead cap's center is within a few degrees of the camera plane whenever the camera is
// level, and the lower rings wrap around past the widescreen edges. Those triangles get cut into vertices at w = 0
// that project to infinity, and draw stretched (striped) or drop out, flickering as the camera pitches and turns.
// The RSP clips them at the real near plane instead. Every dome triangle is clipped here at sky_clip_w into six
// vertex slots (clip_tri_slots) and redrawn from a side display list, with its texture coordinates and colors
// interpolated along the edges. The slots keep the piece's vertex count and order the same every frame, which RT64's
// vertex interpolation needs to pair the right vertices. They are clipped under this frame's view and the last
// one's (SkyClipPlane), so the frames RT64 interpolates between them keep the sky in front of the camera during fast
// turns such as the race start flyover.
void rush2_interp_poly_end(uint8_t* rdram, recomp_context* ctx) {
    uint32_t poly = (uint32_t)ctx->r30;
    uint32_t vtx_cmd = clip_vtx_cmd;
    bool ours = clip_poly == poly;
    clip_poly = 0;
    // Other polygons at a primitive depth (+6) sit in front of everything (the battle arenas' arrows, src/rush2049/battle.cpp).
    if (!ours || (MEM_H(2, (int32_t)poly) & 0x2000) == 0 || MEM_H(6, (int32_t)poly) < 0x4000) {
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

    // The RSP transforms by the modelview times the projection stack, which is the view's rotation (guLookAtF with
    // the eye at the origin, multiplied in after the projection load) times the perspective. The root modelview is
    // only the identity or a mirror, and the polygons are camera-relative in world axes, so without the rotation w
    // would measure a fixed world axis rather than the view direction.
    SkyClipPlane& cur = sky_plane_cur[cur_view];
    if (!cur.valid) {
        float mv[4][4], p[4][4], m[4][4];
        read_mtx(rdram, kseg0(root), false, mv);
        read_mtx(rdram, kseg0(proj), false, p);
        if (view_lookat[cur_view] != 0) {
            float lookat[4][4];
            read_mtx(rdram, kseg0(view_lookat[cur_view]), false, lookat);
            mul_mtx(lookat, p, p);
        }
        mul_mtx(mv, p, m);
        for (int i = 0; i < 4; i++) {
            cur.col[i] = m[i][3];
        }
        cur.gen = views[cur_view].gen;
        cur.valid = true;
    }
    // After a camera cut RT64 snaps to the new view, so the last frame's plane doesn't matter.
    SkyClipPlane prev = sky_plane_prev[cur_view];
    prev.valid = prev.valid && prev.gen == cur.gen;

    ClipVtx in[4];
    for (uint32_t i = 0; i < n; i++) {
        in[i] = read_vtx(rdram, kseg0(vtx.values.word1) + i * 16, cur, prev);
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

    // Six slots per triangle, drawn as a fan of four triangles (two G_TRI2) each.
    ClipVtx out[4 * 6];
    int out_count = 0;
    for (int t = 0; t < tri_count; t++) {
        ClipVtx tri[3] = { in[tris[t][0]], in[tris[t][1]], in[tris[t][2]] };
        clip_tri_slots(tri, &out[out_count]);
        out_count += 6;
    }

    // The original triangles become no-ops; the G_VTX calls the slots and their fans.
    for (uint32_t addr = vtx_cmd + 8; addr < head; addr += 8) {
        MEM_W(0, (int32_t)addr) = (int32_t)0xE7000000; // G_RDPPIPESYNC
        MEM_W(4, (int32_t)addr) = 0;
    }
    uint32_t vtx_addr = side_alloc(out_count * 16);
    for (int i = 0; i < out_count; i++) {
        write_vtx(rdram, vtx_addr + i * 16, out[i]);
    }
    GfxCommand cmds[1 + 4 * 2];
    cmds[0].values.word0 = 0x01000000u | ((uint32_t)out_count << 12) | ((uint32_t)out_count << 1);
    cmds[0].values.word1 = vtx_addr;
    auto tri_word = [](int a, int b, int c) {
        return (uint32_t)(a * 2 << 16) | (uint32_t)(b * 2 << 8) | (uint32_t)(c * 2);
    };
    for (int t = 0; t < tri_count; t++) {
        int b = t * 6;
        cmds[1 + t * 2].values.word0 = 0x06000000u | tri_word(b, b + 1, b + 2);
        cmds[1 + t * 2].values.word1 = tri_word(b, b + 2, b + 3);
        cmds[2 + t * 2].values.word0 = 0x06000000u | tri_word(b, b + 3, b + 4);
        cmds[2 + t * 2].values.word1 = tri_word(b, b + 4, b + 5);
    }
    replace_with_call(rdram, vtx_cmd, write_side_dl(rdram, cmds, 1 + tri_count * 2));
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

// The matrices of a view as func_8007C624 last set them: its root modelview, its projection without the view's
// rotation and the rotation (src/rush2049/battle_render.cpp draws models of its own with them).
bool rush2_interp_view_matrices(uint32_t view, uint32_t* root, uint32_t* projection, uint32_t* lookat) {
    if (view >= max_views || views[view].root_mtx == 0 || view_projection[view] == 0 || view_lookat[view] == 0) {
        return false;
    }
    *root = views[view].root_mtx;
    *projection = view_projection[view];
    *lookat = view_lookat[view];
    return true;
}

// The current view and the generation of the node being drawn, for other code that adds matrices under it
// (src/rush2049/wings_render.cpp).
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
        level_view_space[depth] = false;
    }
}

// The modelview the RSP has while the current node draws: the view's root modelview times the G_MTXs of the nodes
// down the scene graph path. For drawing parts of a node later in the view (src/rush2049/car2049.cpp).
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
    uint32_t leader = node_alias_count ? alias_of(node) : 0;
    uint32_t index = ((leader ? leader : node) - node_pool) / node_size;

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
    // A scale step (a custom track's flip-book frame shown or hidden by scaling it about its pivot, docs/custom_tracks.md)
    // is a snap too: blending it draws shrunken copies between the pivot and the frame.
    Vec3 axis = read_vec3(rdram, transform);
    float scale = dot(axis, axis);
    bool scale_step = state.scale > 0 && (scale > state.scale * 16.0f || scale * 16.0f < state.scale);
    // The frames of a flip-book share their leader's state, each with its own transform: not a reused node.
    if ((transform != state.transform && leader == 0) || scale_step ||
        (distance(pos, state.pos) > node_teleport_distance && !view_attached((uint32_t)node))) {
        state.gen++;
    }
    state.transform = transform;
    state.pos = pos;
    state.scale = scale;

    uint32_t gen = level_base[depth] + state.gen;
    level_current[depth] = gen;

    GfxCommand cmds[10];
    uint32_t push = (params ^ 1) & 1;
    uint32_t cmd_count = 0;

    // A node that rides with the camera. The game keeps a view's rotation in its projection matrix, so RT64 takes it
    // for the view matrix and a node's matrix for its place in the world. Between two game frames it moves the node's
    // position along a straight line while the view turns, and a node fixed to a fast-turning camera (the race
    // start's) slips on screen: by the square of the turn, 10 pixels and more at the start's 15 degrees a frame. Such
    // a node is drawn in view space instead: its matrix times the view's rotation is loaded as the whole modelview,
    // under the view's projection without the rotation, which the node's pop puts back (rush2_interp_node_pop).
    float view_space[4][4];
    bool in_view_space = push != 0 && (params & 2) == 0 && view_attached((uint32_t)node) && view_projection[cur_view] != 0 &&
                         view_lookat[cur_view] != 0 && rush2_interp_get_modelview(rdram, view_space);
    if (in_view_space) {
        float lookat[4][4];
        read_mtx(rdram, kseg0(view_lookat[cur_view]), false, lookat);
        mul_mtx(view_space, lookat, view_space);
        uint32_t floats = side_alloc(64);
        for (int i = 0; i < 16; i++) {
            uint32_t bits;
            memcpy(&bits, &view_space[i / 4][i % 4], sizeof(bits));
            MEM_W(0, (int32_t)(floats + i * 4)) = (int32_t)bits;
        }
        projection_group(&cmds[0], view_id(0x21000000u, cur_view, 0));
        cmds[2].values.word0 = G_MTX_LOAD_PROJECTION_W0;
        cmds[2].values.word1 = view_projection[cur_view];
        cmd_count = 3;
        gEXMatrixFloat(&mtx[0], floats, params | 2); // G_MTX_LOAD
        mtx_count = 2;
        level_view_space[depth] = true;
    }

    object_group(&cmds[cmd_count], node_id(cur_view, views[cur_view].gen + gen, index), push, G_EX_COMPONENT_SKIP);
    cmd_count += 2;
    for (uint32_t i = 0; i < mtx_count; i++) {
        cmds[cmd_count++] = mtx[i];
    }
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
    GfxCommand cmds[6];
    uint32_t cmd_count = 2;
    cmds[0] = read_command(rdram, cmd_addr);
    gEXPopMatrixGroup(&cmds[1], G_MTX_MODELVIEW_EX);
    // After a node drawn in view space: the view's projection again, with its rotation.
    if (depth >= 0 && depth < max_depth && level_view_space[depth]) {
        level_view_space[depth] = false;
        projection_group(&cmds[2], view_id(0x20000000u, cur_view, views[cur_view].gen));
        cmds[4].values.word0 = G_MTX_LOAD_PROJECTION_W0;
        cmds[4].values.word1 = view_projection[cur_view];
        cmds[5].values.word0 = G_MTX_MUL_PROJECTION_W0;
        cmds[5].values.word1 = view_lookat[cur_view];
        cmd_count = 6;
    }
    replace_with_call(rdram, cmd_addr, write_side_dl(rdram, cmds, cmd_count));
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

void rush2::interpolation_clear_node_aliases() {
    node_alias_count = 0;
}

void rush2::interpolation_node_alias(uint32_t node, uint32_t leader) {
    if (node != 0 && leader != 0 && alias_of(node) == 0 && node_alias_count < (int)std::size(node_aliases)) {
        node_aliases[node_alias_count++] = { node, leader };
    }
}

void rush2::interpolation_view_attached(uint32_t node) {
    if (node != 0 && !view_attached(node) && attached_count < (int)std::size(attached_nodes)) {
        attached_nodes[attached_count++] = node;
    }
}
