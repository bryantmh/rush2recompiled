// Simulation runs: RUSH2_SIM=1 (set by tools/trackview/runs.py) races a track headless at the fastest tick rate the
// machine manages, for the track previewer's AI replays (docs/custom_tracks.md#race-replays).
//
// - Settings: RUSH2_DATA_DIR replaces the data folder (src/main.cpp registers it as the config path before anything
//   reads one), so the run starts from default settings and a fresh save, and nothing the player has set (cheats,
//   options, opponents, unlocks, records) changes the race or is written to. runs.py fills it with the ROMs and the
//   one mod the track is in.
// - Input: real controllers and the keyboard are ignored; only the --input-script presses reach the game, on player 1.
// - Output: the window is hidden and no audio is queued (src/main.cpp).
// - Speed: the menus run in real time; from the first physics tick of the race, ultramodern's turbo
//   (lib/patches/N64ModernRuntime.patch) runs the game's clocks on virtual time, sends the next VI as soon as the game
//   has gone idle after the last one, and draws nothing. Game threads run one at a time by N64 priority, so waiting
//   for idle (not just for the physics tick) leaves the lower-priority game thread its share: pacing on the physics
//   tick alone starved it, and the race's countdown never ended. Rush 2's physics steps a fixed 1/60 s per VI
//   (physics_thread_step_once_80076D70), and every game thread finishes its work for a VI before the next one, so
//   the race plays as on hardware that never drops a frame, only faster.
// - Race: the time limit is off (Rush 2's No Race Timer cheat flag, 0x800D9E88 = 1, as src/cheats.cpp sets it), so
//   the race lasts until the computer cars finish however slow they are. Player 1 is parked at the back of the grid:
//   the script's presses stop once turbo is on (a 0.15 s press would last seconds of virtual time), and the stuck
//   reset, which would otherwise put it back further along the course every few seconds and into the race, skips it
//   (parked), as do the car-against-car tests (src/ghost.cpp's collision hooks), so lapping cars drive through it
//   instead of wrecking on the grid. The computer cars keep every rule of the base game, stuck resets and catch-up
//   included.
// - RUSH2_SIM_TURBO=0 runs the same race in real time, to tell what turbo changes apart from the rest.
// - End: once src/race_record.cpp has written the recording the process exits.

#include <cstdio>
#include <cstdlib>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"

#include "sim_run.h"

namespace {
    constexpr uint32_t no_race_timer = 0x800D9E88;   // s8: 1 = no time limit (the No Race Timer cheat, src/cheats.cpp)
}

bool rush2::sim::active() {
    static const bool on = [] {
        const char* v = std::getenv("RUSH2_SIM");
        return v != nullptr && *v != 0 && *v != '0';
    }();
    return on;
}

std::filesystem::path rush2::sim::data_dir() {
    if (!active()) return {};
    const char* v = std::getenv("RUSH2_DATA_DIR");
    return v != nullptr && *v != 0 ? std::filesystem::u8path(v) : std::filesystem::path{};
}

void rush2::sim::race_tick(uint8_t* rdram) {
    if (!active()) return;
    MEM_B(0, (int32_t)no_race_timer) = 1;
    static const bool turbo = [] {   // RUSH2_SIM_TURBO=0: the same run in real time (to tell turbo's effects apart)
        const char* v = std::getenv("RUSH2_SIM_TURBO");
        return v == nullptr || *v != '0';
    }();
    if (turbo && !ultramodern::is_turbo()) {
        std::printf("[sim] race started: turbo on\n");
        std::fflush(stdout);
        ultramodern::set_turbo();
    }
}

bool rush2::sim::parked(uint8_t* rdram, uint32_t car) {
    return active() && MEM_BU(0, (int32_t)(car + 0x7E8)) == 2;
}

void rush2::sim::finish() {
    std::printf("[sim] done, exiting\n");
    std::fflush(stdout);
    std::_Exit(0);
}
