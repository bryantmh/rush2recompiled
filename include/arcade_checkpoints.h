#ifndef __ARCADE_CHECKPOINTS_H__
#define __ARCADE_CHECKPOINTS_H__

#include <cstdint>

#include "recomp.h"

// The Arcade Checkpoints option's banners (src/arcade2049/arcade_checkpoints.cpp, include/arcade2049.h).
namespace rush2::arcade2049 {
    // Every game frame (game thread, src/collectibles.cpp's frame hook): places or hides the banners. ctx is used for
    // Rush 2's ground query when a race's banners are placed.
    void checkpoints_frame(uint8_t* rdram, recomp_context* ctx);
}

#endif
