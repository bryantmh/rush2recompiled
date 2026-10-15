#ifndef __SIM_RUN_H__
#define __SIM_RUN_H__

#include <cstdint>
#include <filesystem>

// Simulation runs (tools/trackview's AI races, src/sim_run.cpp): RUSH2_SIM=1 runs a race headless at the fastest tick
// rate the machine manages, from settings and a save of its own. See docs/custom_tracks.md#race-replays.
namespace rush2::sim {
    // RUSH2_SIM is set.
    bool active();

    // The folder a sim run keeps its settings, saves, ROMs and mods in (RUSH2_DATA_DIR), in place of the player's
    // data folder: the run starts from default settings and a fresh save and never touches the player's. Empty when
    // not a sim run or the variable isn't set.
    std::filesystem::path data_dir();

    // From the end of each physics tick while a race runs (src/race_record.cpp): the first one turns on turbo
    // (ultramodern::set_turbo: virtual clock, VIs as fast as the game goes idle, no drawing); each keeps the race's
    // time limit off.
    void race_tick(uint8_t* rdram);

    // Player 1's car (physics struct `car`, kind +0x7E8 = 2 human) in a sim run: it is parked on the grid, out of the
    // stuck reset that would put it back further on every few seconds (rush2_track49_stuck_timer) and of the
    // car-against-car tests (rush2_ghost_collide_self / _other).
    bool parked(uint8_t* rdram, uint32_t car);

    // Ends the process once the recording is written.
    [[noreturn]] void finish();
}

#endif
