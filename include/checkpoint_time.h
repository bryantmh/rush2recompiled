#pragma once
// Checkpoint Time Difficulty (General tab, Gameplay; src/checkpoint_time.cpp): the DIFFICULTY that sets the race
// clock's time, apart from the one that sets the computer cars' pace.

#include <cstdint>

namespace rush2::checkpoint_time {
    // The option's values: Linked uses the race's DIFFICULTY (as Rush 2 does); Difficulty0 + d uses DIFFICULTY d.
    enum class Setting : uint32_t { Linked, Difficulty0, Difficulty1, Difficulty2, Difficulty3, Difficulty4, Difficulty5 };

    void set(Setting setting);
    // The DIFFICULTY (0-5) the race clock's time uses: the setting's, or the race's (0x8010C211) when Linked.
    int difficulty(uint8_t* rdram);
}
