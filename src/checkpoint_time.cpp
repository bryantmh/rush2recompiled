// Checkpoint Time Difficulty (General tab, Gameplay): the race clock's time set apart from the computer cars' pace.
//
// Rush 2 reads DIFFICULTY (0x8010C211, 0-5) for both. The computer cars' pace comes from it in rubber_band_800A1A98
// (docs/npc_cars.md#drone-catch-up); the clock's time is multiplied by f = 1 + (5 - difficulty) x 0.075 in three
// places (docs/rush2049_research/checkpoints.md, section 2):
//   - race start, checkpoints on (race_start_timer_800AE670): time allowed = header +0 x f, load at 0x800AEAC8;
//   - a checkpoint passed (checkpoint_passed_8008F220): + the extension x f, load at 0x8008F640;
//   - race start, checkpoints off (race_compute_total_time_no_checkpoints_800AB1B4): the whole race's time x f,
//     load at 0x800AB1B8.
// Hooks right after each load (us.toml) put this setting's difficulty in the loaded register. The C++ that scales time
// the same way (Rush 1's checkpoint times in src/rush1/track1.cpp, the PS1 Time Sprint clocks in src/time_sprint.cpp)
// reads difficulty() too.
#include <atomic>

#include "recomp.h"
#include "checkpoint_time.h"
#include "rush2_hooks.h"

namespace {
    constexpr uint32_t race_difficulty = 0x8010C211; // s8 0-5: the race's DIFFICULTY (track select, or the circuit's)
    std::atomic<uint32_t> setting{ (uint32_t)rush2::checkpoint_time::Setting::Linked };
}

void rush2::checkpoint_time::set(Setting value) {
    setting.store((uint32_t)value, std::memory_order_relaxed);
}

int rush2::checkpoint_time::difficulty(uint8_t* rdram) {
    uint32_t value = setting.load(std::memory_order_relaxed);
    if (value >= (uint32_t)Setting::Difficulty0 && value <= (uint32_t)Setting::Difficulty5) {
        return (int)(value - (uint32_t)Setting::Difficulty0);
    }
    return (int)(int8_t)MEM_B(0, (int32_t)race_difficulty);
}

// After the DIFFICULTY loads at 0x8008F640 ($t7), 0x800AB1B8 ($t6) and 0x800AEAC8 ($t6).
extern "C" void rush2_checkpoint_time_difficulty(uint8_t* rdram, recomp_context* ctx, int reg) {
    gpr value = (gpr)(int32_t)rush2::checkpoint_time::difficulty(rdram);
    if (reg == 15) {
        ctx->r15 = value;
    } else {
        ctx->r14 = value;
    }
}
