// Frame clear for widescreen.
//
// The frame setup (func_800B45B4) fills the framebuffer with the fog color only inside the overscan inset
// (12,10)-(308,230), then fills the border around it with black. On hardware the player views are scissored to
// the inset, so the border stays a black frame. The scissor hook in us.toml extends the views to the framebuffer
// edges, so the 3D scene now draws over the border, but on tracks whose sky is the clear color (e.g. Los Angeles)
// nothing covers it: the black strips show through the sky, the top one stretched across the screen and the side
// ones at the edges of the original 4:3 area. Zeroing the insets while the clear is emitted makes it fill the
// whole screen and leaves the border fills empty (they still cover the screen position offset, if any).

#include "recomp.h"
#include "rush2_hooks.h"

namespace {
    constexpr uint32_t inset_x = 0x80023058; // s16, 12
    constexpr uint32_t inset_y = 0x8002305A; // s16, 10

    int16_t saved_inset_x;
    int16_t saved_inset_y;
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
