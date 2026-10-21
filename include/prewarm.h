#ifndef __PREWARM_H__
#define __PREWARM_H__

#include <cstdint>
#include <string>

// What the game converts from the other games' ROMs and from mods (src/prewarm.cpp, docs/prewarm.md): every car and
// track built into the disk cache before the game starts, behind the launcher's progress screen, then in the
// background whatever a change in the menus needs. No menu or race start converts anything on the game thread.
namespace rush2::prewarm {
    // The launch's build pass, from the launcher once the Rush 2 ROM is valid; once.
    void start(const std::u8string& game_id);
    // The pass is over: the game can start.
    bool ready();
    struct Progress {
        float fraction = 0.0f;  // of the pass's expected time
        bool finished = false;
        bool building = false;  // something has needed building (else the launch shows nothing)
        std::string job;        // one of the jobs going now ("Rush 2049 cars"), empty between them
    };
    Progress progress();
    // Once per game frame (rush2_cheats_frame): at the car select, reads the chosen track's cache entry into memory for
    // the race about to load it.
    void frame(uint8_t* rdram);
}

#endif
