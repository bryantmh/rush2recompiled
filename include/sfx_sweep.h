#ifndef __SFX_SWEEP_H__
#define __SFX_SWEEP_H__

#include <cstdint>

#include "recomp.h"

// Test aid RUSH2_TEST_SFX_SWEEP (src/sfx_sweep.cpp): every sound effect of one game on its own, for level comparisons.
namespace rush2::sfx_sweep {
    // Rush 2's audio thread, once per audio frame (from rush2_music_preview, src/music.cpp).
    void audio_frame(uint8_t* rdram, recomp_context* ctx);
    // The stream frames RUSH2_AUDIO_LEVELS has measured so far, and their rate (src/main.cpp log_audio_levels).
    uint64_t dump_frames();
    uint32_t dump_rate();
}

#endif
