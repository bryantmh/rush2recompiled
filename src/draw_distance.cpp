// Draw distance.
//
// Past the LOD tables (src/lod.cpp), three things limit how far the game draws:
// - The projection's far plane: func_8007C624 builds each view's guPerspective from the view's near (+0x34) and far
//   (+0x38) times 16. func_80081074 sets far each frame from tables indexed by player count and the FOG option
//   (2000 with one player and FOG 1), blended by per-track fog zones.
// - The scene walker (func_8007B518) skips placed objects (nodes with box cull flags) farther than the f32 at
//   0x80110040 (1800 with one player and FOG 1).
// - The visibility table (func_8007C27C): only the sections listed for the camera's region are drawn.
//
// Fog is set in the 0-1000 depth range between the near and far planes (view +0x4C/+0x4E, 996-1000 with FOG 1),
// which is already the narrowest band the RSP's 16-bit fog multiplier can express with near = 2.5. Scaling near and
// far by the same factor keeps every depth, and so the fog band, the same while everything is seen that many times
// farther. Only the values passed to guPerspective are scaled: the game's procedural sky dome (func_80089218) and the
// fog zones read the view's stored near and far. The object cull distance and LOD distances get the same factor, and
// every section is drawn (the game's all-visible mask).

#include <atomic>
#include <cstring>

#include "recomp.h"
#include "rush2_hooks.h"
#include "rush2.h"

namespace {
    constexpr uint32_t all_visible_mask = 0x800CA1B8;

    std::atomic<float> scale = 1.0f;
}

void rush2::set_draw_distance(float factor) {
    scale.store(factor, std::memory_order_relaxed);
}

float rush2::draw_distance() {
    return scale.load(std::memory_order_relaxed);
}

// func_8007C624 at 0x8007CB5C, before guPerspective: $f8 = view far, 0x10($sp) = near * 16 (already stored).
extern "C" void rush2_draw_distance_projection(uint8_t* rdram, recomp_context* ctx) {
    float k = scale.load(std::memory_order_relaxed);
    if (k == 1.0f) {
        return;
    }
    ctx->f8.fl *= k;
    int32_t near_addr = (int32_t)ctx->r29 + 0x10;
    uint32_t bits = (uint32_t)MEM_W(0, near_addr);
    float near_value;
    memcpy(&near_value, &bits, sizeof(near_value));
    near_value *= k;
    memcpy(&bits, &near_value, sizeof(bits));
    MEM_W(0, near_addr) = bits;
}

// func_8007B518 at 0x8007BA50: $f8 = the placed object cull distance (0x80110040).
extern "C" void rush2_draw_distance_cull(uint8_t* rdram, recomp_context* ctx) {
    ctx->f8.fl *= scale.load(std::memory_order_relaxed);
}

// Called from the visibility hook (rush2_track49_pvs): 0x5C($sp) = the section mask for the camera's region.
bool rush2::draw_distance_pvs(uint8_t* rdram, uint32_t sp) {
    if (scale.load(std::memory_order_relaxed) == 1.0f) {
        return false;
    }
    MEM_W(0, (int32_t)(sp + 0x5C)) = all_visible_mask;
    return true;
}
