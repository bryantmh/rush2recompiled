#ifndef __TEXTURE_PREWARM_H__
#define __TEXTURE_PREWARM_H__

#include <cstdint>

// Texture prewarming (src/texture_prewarm.cpp, docs/hitch_profiler.md#race-start-textures): RT64 uploads and decodes
// every texture the first time the game draws with it while that frame waits, and a race's first frame brings hundreds.
// The textures a track's race start uploaded are kept on disk per track, and handed to RT64 again ahead of the next
// race of that track, at its car select, so they are in its texture cache before the race draws with them.
namespace rush2::texture_prewarm {
    // The track the next race is on (its track select id): prewarms its textures and records new ones in that race.
    // The car select calls it (frame); a race started without the menus (src/test_race.cpp) calls it itself.
    void set_track(int menu_id);
    // Once per game frame (rush2::prewarm::frame).
    void frame(uint8_t* rdram);
}

#endif
