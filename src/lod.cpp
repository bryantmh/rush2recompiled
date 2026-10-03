// Level of detail override.
//
// func_8007AA48 draws a scene graph node's model. A model has a table of LODs ordered from most to least
// detailed, each with a distance multiplied by the node's scale: LOD i is drawn up to its distance, and the model
// isn't drawn past the last LOD's distance (0 = no limit). The function stores the node's distance from the camera
// in D_80117490 and reads it only to cull and pick the LOD. With LOD disabled, the hook in us.toml zeroes that
// distance, so every model draws its most detailed LOD and is never culled by its LOD table.
//
// Nodes with flag 0x4 select their LOD explicitly (flags & 3) instead of by distance; those are left alone.

#include <atomic>

#include "recomp.h"
#include "rush2_hooks.h"
#include "rush2.h"

namespace {
    constexpr uint32_t lod_distance = 0x80117490;
    constexpr uint32_t node_fixed_lod = 0x4;

    std::atomic<bool> lod_disabled = false;
}

void rush2::set_lod_disabled(bool disabled) {
    lod_disabled.store(disabled, std::memory_order_relaxed);
}

// After the LOD distance is computed: $s7 = node.
extern "C" void rush2_lod_select(uint8_t* rdram, recomp_context* ctx) {
    if (!lod_disabled.load(std::memory_order_relaxed)) {
        return;
    }

    uint32_t flags = (uint32_t)MEM_W(0, (int32_t)ctx->r23);
    if ((flags & node_fixed_lod) == 0) {
        MEM_W(0, (int32_t)lod_distance) = 0; // 0.0f
    }
}
