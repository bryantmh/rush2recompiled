// Draw distance.
//
// Past the LOD tables (src/lod.cpp), four things limit how far the game draws:
// - The projection's far plane: func_8007C624 builds each view's guPerspective from the view's near (+0x34) and far
//   (+0x38) times 16. func_80081074 sets far each frame from tables indexed by player count and the FOG option
//   (2000 with one player and FOG 1), blended by per-track fog zones.
// - The scene walker (func_8007B518) skips placed objects (nodes with box cull flags) farther than the f32 at
//   0x80110040 (1800 with one player and FOG 1).
// - The node matrix builders (func_8007B00C, func_8007B290) skip any node more than 2048 units from the camera on an
//   axis: the translation times 16 must fit the fixed-point matrix's s15.16 range.
// - The visibility table (func_8007C27C): only the sections listed for the camera's region are drawn.
//
// Fog is set in the 0-1000 depth range between the near and far planes (view +0x4C/+0x4E, 996-1000 with FOG 1),
// which is already the narrowest band the RSP's 16-bit fog multiplier can express. Depth is nearly 1 - 2 * near /
// distance, so where the fog starts follows the near plane. Scaling near pushes the fog out with the far plane, but
// a near plane past about 5 units cuts off the road at the bottom of the screen, so near is scaled by at most
// max_near_scale: the fog then starts later and thickens gradually out to the scaled far plane. Only the values passed
// to guPerspective are scaled: the game's procedural sky dome (func_80089218) and the fog zones read the view's stored
// near and far. The object cull distance and LOD distances get the same factor, nodes past the fixed-point range get
// float matrices (RT64's gEXMatrixFloat, emitted by src/interpolation.cpp), and every section is drawn (the game's
// all-visible mask).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "recomp.h"
#include "rush2_hooks.h"
#include "rush2.h"

namespace {
    constexpr uint32_t all_visible_mask = 0x800CA1B8;
    constexpr uint32_t camera_array = 0x800E79D0; // Per-view cameras, 0x40 bytes each; position at +0x24.
    constexpr float max_near_scale = 2.0f;
    constexpr float fixed_range = 2048.0f;        // Largest camera offset the fixed-point matrix holds.

    std::atomic<float> scale = 1.0f;

    // The last float matrix written by the matrix builder hooks, until its G_MTX is emitted.
    uint32_t pending_float_matrix = 0;

    float read_f32(uint8_t* rdram, uint32_t addr) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    }

    void write_f32(uint8_t* rdram, uint32_t addr, float f) {
        uint32_t bits;
        memcpy(&bits, &f, sizeof(bits));
        MEM_W(0, (int32_t)addr) = (int32_t)bits;
    }

    // Shared by both matrix builders. rot(r, c) reads the rotation, pos = the node's position, relative = whether to
    // subtract the camera's. Writes a float matrix (same layout as the fixed one: rows, translation times 16 in row 3)
    // to out when the offset is past the fixed-point range but the draw distance is extended.
    template <typename Rot>
    bool write_float_matrix(uint8_t* rdram, recomp_context* ctx, uint32_t view, uint32_t pos, bool relative,
                            uint32_t out, Rot rot) {
        if (scale.load(std::memory_order_relaxed) == 1.0f) {
            return false;
        }

        float d[3];
        for (int i = 0; i < 3; i++) {
            d[i] = read_f32(rdram, pos + i * 4);
            if (relative) {
                d[i] -= read_f32(rdram, camera_array + view * 0x40 + 0x24 + i * 4);
            }
        }
        if (std::fabs(d[0]) < fixed_range && std::fabs(d[1]) < fixed_range && std::fabs(d[2]) < fixed_range) {
            return false;
        }

        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                write_f32(rdram, out + (r * 4 + c) * 4, rot(r, c));
            }
            write_f32(rdram, out + (r * 4 + 3) * 4, 0.0f);
            write_f32(rdram, out + (12 + r) * 4, d[r] * 16.0f);
        }
        write_f32(rdram, out + 15 * 4, 1.0f);

        pending_float_matrix = out;
        ctx->r2 = 1;
        return true;
    }
}

void rush2::set_draw_distance(float factor) {
    scale.store(factor, std::memory_order_relaxed);
}

float rush2::draw_distance() {
    return scale.load(std::memory_order_relaxed);
}

bool rush2::draw_distance_take_float_matrix(uint32_t addr) {
    if (pending_float_matrix == 0 || pending_float_matrix != addr) {
        return false;
    }
    pending_float_matrix = 0;
    return true;
}

// func_8007C624 at 0x8007CB5C, before guPerspective: $f8 = view far, 0x10($sp) = near * 16 (already stored).
extern "C" void rush2_draw_distance_projection(uint8_t* rdram, recomp_context* ctx) {
    float k = scale.load(std::memory_order_relaxed);
    if (k == 1.0f) {
        return;
    }
    ctx->f8.fl *= k;
    uint32_t near_addr = (uint32_t)ctx->r29 + 0x10;
    write_f32(rdram, near_addr, read_f32(rdram, near_addr) * std::min(k, max_near_scale));
}

// func_8007B518 at 0x8007BA50: $f8 = the placed object cull distance (0x80110040).
extern "C" void rush2_draw_distance_cull(uint8_t* rdram, recomp_context* ctx) {
    ctx->f8.fl *= scale.load(std::memory_order_relaxed);
}

// func_8007B00C entry: $a0 = view, $a1 = transform (3x3 rotation by rows, position at +0x24), $a2 = matrix out,
// $a3 = nonzero if the position is already camera relative.
extern "C" int rush2_draw_distance_matrix_rows(uint8_t* rdram, recomp_context* ctx) {
    uint32_t transform = (uint32_t)ctx->r5;
    return write_float_matrix(rdram, ctx, (uint32_t)ctx->r4 & 3, transform + 0x24, ctx->r7 == 0, (uint32_t)ctx->r6,
        [&](int r, int c) { return read_f32(rdram, transform + (r * 3 + c) * 4); });
}

// func_8007B290 entry: $a0 = view, $a1 = 4x4 rotation by columns, $a2 = position, $a3 = matrix out, 0x10($sp) =
// nonzero if the position is already camera relative.
extern "C" int rush2_draw_distance_matrix_cols(uint8_t* rdram, recomp_context* ctx) {
    uint32_t rotation = (uint32_t)ctx->r5;
    bool relative = MEM_W(0x10, ctx->r29) == 0;
    return write_float_matrix(rdram, ctx, (uint32_t)ctx->r4 & 3, (uint32_t)ctx->r6, relative, (uint32_t)ctx->r7,
        [&](int r, int c) { return read_f32(rdram, rotation + (c * 4 + r) * 4); });
}

// Called from the visibility hook (rush2_track49_pvs): 0x5C($sp) = the section mask for the camera's region.
bool rush2::draw_distance_pvs(uint8_t* rdram, uint32_t sp) {
    if (scale.load(std::memory_order_relaxed) == 1.0f) {
        return false;
    }
    MEM_W(0, (int32_t)(sp + 0x5C)) = all_visible_mask;
    return true;
}
