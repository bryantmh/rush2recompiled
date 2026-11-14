// Dreamcast Rush 2049 models' translucent strips, drawn after the view's opaque scenery (include/dc_translucent.h,
// docs/rush2049_research/dreamcast.md#translucent-strips-drawn-last-v).
//
// The disc's PowerVR draws its translucent list after the opaque and punch-through lists, sorted by depth per pixel,
// so a see-through texture (Marina's CHAINLINK fence, glass) never hides what is behind it. The converter
// (src/rush2049dc/rush2049_dc_model.cpp, late_marker) puts a model's translucent strips at the end of its list behind
// a slot. Here, when the game draws such a list (model_draw_8007AA48, hook rush2_model_draw), the slot becomes a branch
// past the tail (once per load: the list in RDRAM keeps it) and the tail is queued under the node's modelview;
// func_8007C624 draws the queue after the view's world-space polygons (L_8007D980, the hook the Rocket ZX flames use,
// src/rush2049/car2049.cpp), farthest model first. Drawn among the opaque scenery and writing no depth, a tail was
// painted over by whatever farther was drawn after it (the trees behind the fence).

#include <algorithm>
#include <cstring>
#include <vector>

#include "recomp.h"
#include "rt64_extended_gbi.h"

#include "dc_translucent.h"
#include "scene_nodes.h"

extern "C" {
    uint32_t rush2_interp_side_dl(uint8_t* rdram, const GfxCommand* cmds, uint32_t count);
    uint32_t rush2_interp_side_alloc(uint32_t bytes);
    void rush2_interp_get_generation(uint32_t* view, uint32_t* gen);
    bool rush2_interp_get_modelview(uint8_t* rdram, float out[4][4]);
}

namespace {
    // The converter's commands (rush2049_dc_model.cpp): the list's first command holds the slot's offset, the slot the
    // end's; a slot made a branch is DE01 to the end.
    constexpr uint32_t late_marker = 0xE0000DC0;
    constexpr uint32_t late_slot = 0xE0000DC1;
    constexpr uint32_t branch = 0xDE010000;

    constexpr uint32_t node_distance = 0x80117490;  // f32: model_draw's camera distance of the node being drawn.
    constexpr uint32_t lighting_cache = 0x800E7DE1; // func_8007AA48: nonzero while G_LIGHTING is on.
    constexpr uint32_t palette_cache = 0x80111954;  // func_80078190: palette last loaded into TMEM, 0 for none.

    struct Tail {
        float mtx[4][4];
        float distance;
        uint32_t list;      // the tail's first command
        uint32_t id;        // matrix group
        bool lit;
    };
    std::vector<Tail> queue;
    bool emitted = false;   // the view has drawn its queue; later draws (the game's late node pass) add the tail inline

    // Model lists are loaded below 16 MB (display list addresses are 24 bits).
    bool in_rdram(uint32_t addr) {
        return addr >= 0x80000000u && addr < 0x81000000u - 8;
    }

    void add(std::vector<GfxCommand>& out, uint32_t w0, uint32_t w1) {
        GfxCommand c;
        c.values.word0 = w0;
        c.values.word1 = w1;
        out.push_back(c);
    }

    // The state the converted lists are drawn with in the scene (as the Rocket ZX flames set it, car2049.cpp): 2-cycle,
    // perspective-correct bilinear texturing, z-buffered, smooth shaded and fogged without lighting, texturing on. The
    // lists set their own textures, render modes and combiners.
    constexpr uint32_t prologue[][2] = {
        { 0xE7000000, 0x00000000 }, // G_RDPPIPESYNC
        { 0xE3000A01, 0x00100000 }, // CYCLETYPE = 2CYCLE
        { 0xE3000C00, 0x00080000 }, // TEXTPERSP = PERSP
        { 0xE3001201, 0x00002000 }, // TEXTFILT = BILERP
        { 0xD9F9FFFF, 0x00000000 }, // clear LIGHTING | TEXTURE_GEN
        { 0xD9FFFFFF, 0x00210005 }, // set ZBUFFER | SHADE | SHADING_SMOOTH | FOG
        { 0xD7000002, 0xFFFFFFFF }, // G_TEXTURE on, scale 1
    };
}

void rush2::dc_translucent::view_begin() {
    queue.clear();
    emitted = false;
}

bool rush2::dc_translucent::pending() {
    return !queue.empty();
}

void rush2::dc_translucent::model_draw(uint8_t* rdram, recomp_context* ctx) {
    uint32_t list = (uint32_t)ctx->r30;
    if (!in_rdram(list) || (uint32_t)MEM_W(0, (int32_t)list) != late_marker) {
        return;
    }
    uint32_t slot = list + (uint32_t)MEM_W(4, (int32_t)list);
    if (!in_rdram(slot)) {
        return;
    }
    uint32_t slot_w0 = (uint32_t)MEM_W(0, (int32_t)slot);
    if (slot_w0 == late_slot) {
        uint32_t end = list + (uint32_t)MEM_W(4, (int32_t)slot);
        MEM_W(0, (int32_t)slot) = (int32_t)branch;
        MEM_W(4, (int32_t)slot) = (int32_t)(end & 0xFFFFFF);
    }
    else if (slot_w0 != branch) {
        return;
    }
    uint32_t tail = slot + 8;

    uint32_t node = (uint32_t)ctx->r23;
    Tail t;
    bool queued = !emitted && node >= rush2::scene_nodes::table && node < rush2::scene_nodes::table_end &&
        rush2_interp_get_modelview(rdram, t.mtx);
    if (!queued) {
        // Too late for the queue (or no matrix): the tail right after the list, as the converted list draws without
        // the slot.
        GfxCommand cmds[2];
        cmds[0].values.word0 = 0xDE000000;
        cmds[0].values.word1 = list;
        cmds[1].values.word0 = 0xDE000000;
        cmds[1].values.word1 = tail;
        ctx->r30 = (uint64_t)(int64_t)(int32_t)rush2_interp_side_dl(rdram, cmds, 2);
        return;
    }
    uint32_t view = 0, gen = 0;
    rush2_interp_get_generation(&view, &gen);
    uint32_t index = (node - rush2::scene_nodes::table) / rush2::scene_nodes::node_size;
    // Matrix group IDs 0110 00vv gggg gggg gggg nnnn nnnn nnnn (n = node), apart from interpolation.cpp's, the
    // flames' (0101), the wings' (0100), Fill Scenery Gaps' (0111) and the pause menu's (0x6E, 0x6F).
    t.id = 0x60000000u | ((view & 3) << 24) | ((gen & 0xFFF) << 12) | (index & 0xFFF);
    uint32_t bits = (uint32_t)MEM_W(0, (int32_t)node_distance);
    memcpy(&t.distance, &bits, 4);
    t.list = tail;
    t.lit = MEM_BU(0, (int32_t)lighting_cache) != 0;
    queue.push_back(t);
}

bool rush2::dc_translucent::emit(uint8_t* rdram, uint32_t cursor_ptr) {
    emitted = true;
    if (queue.empty()) {
        return false;
    }
    // Farthest first, so nearer glass blends over farther.
    std::stable_sort(queue.begin(), queue.end(), [](const Tail& a, const Tail& b) { return a.distance > b.distance; });

    std::vector<GfxCommand> cmds;
    cmds.reserve(16 + queue.size() * 12);
    for (const auto& c : prologue) {
        add(cmds, c[0], c[1]);
    }
    bool lit = false;
    for (const Tail& t : queue) {
        if (t.lit != lit) {
            add(cmds, t.lit ? 0xD9FFFFFF : 0xD9FDFFFF, t.lit ? 0x00020000 : 0);
            lit = t.lit;
        }
        uint32_t mtx = rush2_interp_side_alloc(64);
        for (int i = 0; i < 16; i++) {
            uint32_t bits;
            memcpy(&bits, &t.mtx[i / 4][i % 4], 4);
            MEM_W(0, (int32_t)(mtx + i * 4)) = (int32_t)bits;
        }
        GfxCommand c[2];
        gEXMatrixGroup(c, t.id, G_EX_INTERPOLATE_DECOMPOSE, G_EX_PUSH, 0 /* modelview */,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_AUTO,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
        cmds.push_back(c[0]);
        cmds.push_back(c[1]);
        // G_MTX_PUSH | G_MTX_LOAD, stored XORed with G_MTX_PUSH as F3DEX2 does.
        gEXMatrixFloat(c, mtx, 2);
        cmds.push_back(c[0]);
        cmds.push_back(c[1]);
        add(cmds, 0xDE000000, t.list);
        add(cmds, 0xD8380002, 0x40); // G_POPMTX
        gEXPopMatrixGroup(c, 0);
        cmds.push_back(c[0]);
    }
    // func_8007AA48 only emits G_LIGHTING changes when a model's lighting differs from what it last set.
    if (MEM_BU(0, (int32_t)lighting_cache) != 0) {
        add(cmds, 0xD9FFFFFF, 0x00020000);
    }
    else if (lit) {
        add(cmds, 0xD9FDFFFF, 0);
    }
    // The tails load their own textures over whatever palette func_80078190 thinks is in TMEM.
    MEM_W(0, (int32_t)palette_cache) = 0;
    queue.clear();

    uint32_t dl = rush2_interp_side_dl(rdram, cmds.data(), (uint32_t)cmds.size());
    uint32_t cursor = (uint32_t)MEM_W(0, (int32_t)cursor_ptr);
    MEM_W(0, (int32_t)cursor) = (int32_t)0xDE000000;
    MEM_W(4, (int32_t)cursor) = (int32_t)dl;
    MEM_W(0, (int32_t)cursor_ptr) = (int32_t)(cursor + 8);
    return true;
}
