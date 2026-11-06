#ifndef __ANNOUNCER_H__
#define __ANNOUNCER_H__

#include <cstdint>

// The race start announcer (src/announcer.cpp, docs/announcer.md): which game's voice calls the countdown and GO.
namespace rush2::announcer {
    // Kept in sound.json ("race_announcer") by key. SF Rush's N64 announcer is Rush 2's (the same samples played the
    // same way) and Rush 2049's is Rush 2's recordings re-encoded, so neither has a choice of its own. 1 was Rush 2049
    // ("Rush2049" in configs, which now loads as the default, Rush 2).
    enum class Choice : uint32_t { Rush2 = 0, CalSpeed = 2 };
    // Hot-swappable: read at each cue.
    void set(Choice choice);
}

#endif
