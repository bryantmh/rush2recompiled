// The race start announcer: the Sound tab's Announcer picks which game's voice calls a race's countdown and its GO
// (docs/announcer.md). Rush 2's stays the default and plays as the game does.
//
// Rush 2 calls the countdown from race_countdown_update (each frame of the race intro, once per frame for the last
// view): when the countdown digit (0x8010C014, trunc(time left of 3.5 s + 0.999)) drops, 3 plays sound 0x71, 2 plays
// 0x72 and 1 plays 0x69, all through menu_play_sound. As the race starts, func_800AE670 plays 0x6C (GO). The hooks at
// those four calls mark the cue, and the hook at the start of menu_play_sound plays the chosen announcer in its place:
// - California Speed's countdown is 2.5 s (0x80093158), so its digit starts at 3 with no sound; 0x800ADB00 plays
//   0x5B on 2, 0x5C on 1 and 0x5D on 0, as its race starts. Here they play on Rush 2's 2, 1 and GO, and 3 is silent,
//   at California Speed's pitch 0.5 (its table 0x80057D0C) through src/car_engines.cpp.
// - SF Rush's N64 countdown (0x41, 0x42, 0x39 and GO 0x3C) is Rush 2's: the same samples at the same pitches. Rush
//   2049's (0x4B-0x4E) is Rush 2's recordings re-encoded, so neither has a choice.
// Without California Speed's ROM Rush 2's sound plays.

#include <atomic>
#include <cstdio>

#include "recomp.h"
#include "announcer.h"
#include "calspeed.h"
#include "car_engines.h"
#include "rush2_hooks.h"
#include "sfx_sweep.h"

namespace {
    using rush2::announcer::Choice;

    std::atomic<uint32_t> choice = (uint32_t)Choice::Rush2;
    // The cue whose call to menu_play_sound comes next (3, 2, 1 or 0 for GO), or -1.
    std::atomic<int> pending = -1;

    // By cue (0 GO, then 1, 2, 3).
    constexpr int rush2_sounds[4] = { 0x6C, 0x69, 0x72, 0x71 };
    constexpr int calspeed_sounds[4] = { 0x5D, 0x5C, 0x5B, -1 };
    constexpr float calspeed_pitch = 0.5f;   // 0x80057D0C [id * 3] for 0x5B-0x5D

    constexpr uint32_t sounds_on = 0x800D5700;    // s32: menu_play_sound returns at once while 0.
}

void rush2::announcer::set(Choice c) {
    choice = (uint32_t)c;
}

extern "C" void rush2_announcer_cue(uint8_t*, recomp_context*, int cue) {
    pending = choice.load() == (uint32_t)Choice::Rush2 ? -1 : cue;
    // The frame is the audio stream's (RUSH2_AUDIO_LEVELS dumps), for checking a run's recording.
    fprintf(stderr, "[announcer] cue %d, announcer %u, stream frame %llu\n", cue, choice.load(),
            (unsigned long long)rush2::sfx_sweep::dump_frames());
}

// Start of menu_play_sound ($a0 = sound). True when the chosen announcer played in its place.
extern "C" bool rush2_announcer_sound(uint8_t* rdram, recomp_context* ctx) {
    int cue = pending.exchange(-1);
    // menu_play_sound plays nothing while 0x800D5700 is 0; neither does the announcer.
    if (cue < 0 || cue > 3 || (int)ctx->r4 != rush2_sounds[cue] || MEM_W(0, (int32_t)sounds_on) == 0) {
        return false;
    }
    Choice c = (Choice)choice.load();
    if (c != Choice::CalSpeed || rush2::calspeed::get_rom() == nullptr) return false;
    int id = calspeed_sounds[cue];
    if (id >= 0 && !rush2::car_engines::play_calspeed_sound(rdram, id, calspeed_pitch)) return false;
    fprintf(stderr, "[announcer] cue %d played by announcer %u\n", cue, (uint32_t)c);
    ctx->r2 = 0;
    return true;
}
