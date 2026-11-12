#ifndef DC_TRANSLUCENT_H
#define DC_TRANSLUCENT_H

#include <cstdint>

#include "recomp.h"

// Dreamcast Rush 2049 models' translucent strips, drawn after the view's opaque scenery as the disc's PowerVR draws
// its translucent list (src/rush2049dc/dc_translucent.cpp; the list layout is in src/rush2049dc/rush2049_dc_model.cpp,
// late_marker).
namespace rush2::dc_translucent {
    // func_8007C624 entry: a new view; empties the queue.
    void view_begin();
    // From rush2_model_draw (model_draw_8007AA48 about to call a node's model: $s7 node, $fp list): for a list with a
    // translucent tail, makes its slot skip the tail and queues the tail, or, once the view has drawn its queue, adds
    // the tail after the list.
    void model_draw(uint8_t* rdram, recomp_context* ctx);
    // True while tails are queued.
    bool pending();
    // Appends a G_DL to the queued tails, farthest first, at the display list cursor *cursor_ptr, and empties the
    // queue (func_8007C624 at L_8007D980, after the world-space polygons, or before the race start countdown sign).
    // Returns whether it drew anything.
    bool emit(uint8_t* rdram, uint32_t cursor_ptr);
}

#endif
