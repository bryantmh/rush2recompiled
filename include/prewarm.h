#ifndef __PREWARM_H__
#define __PREWARM_H__

#include <cstdint>

// Background builds of what the game converts from the other games' ROMs and from mods (src/prewarm.cpp,
// docs/prewarm.md): the cars at launch, ahead of the boot that installs them, then every track into the disk cache,
// so no menu or race start converts anything on the game thread.
namespace rush2::prewarm {
    // From the launcher's start (main.cpp launcher_init), once.
    void start();
    // Once per game frame (rush2_cheats_frame): at the car select, reads the chosen track's cache entry into memory for
    // the race about to load it.
    void frame(uint8_t* rdram);
}

#endif
