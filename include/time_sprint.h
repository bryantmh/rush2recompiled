#ifndef __TIME_SPRINT_H__
#define __TIME_SPRINT_H__

#include <cstdint>
#include <string>

#include "recomp.h"

// CHECKPOINTS' third value, CLOCKS: the SF Rush PlayStation version's Time Sprint mode (docs/sfrush_ps1.md#time-sprint)
// on every version of the courses the PS1 places its clocks on (Golden Gate, Embarcadero and Market: N64, arcade and
// the PS1 mod's snow, night and stunt versions; Chinatown's three). The race is against the clock as with CHECKPOINTS
// on (the race global 0x8010C17B holds 2, which every game test of it reads as on), but passing a checkpoint adds no
// time: the PS1's hourglasses (its own model, texture and pickup sound, from the sfrush_ps1 mod) on the track do, 20 s
// each scaled by difficulty, and each is taken once a race (src/time_sprint.cpp; positions from
// tools/sfrush_ps1/time_sprint_clocks.py in src/sfrush_ps1/time_sprint_clocks.inc; the track select row in
// src/rush2049/track2049_menu.cpp). Without the sfrush_ps1 mod there is no CLOCKS value. On a track without clocks the
// value races as ON.
namespace rush2::time_sprint {
    constexpr int clocks_value = 2;          // the track select's CHECKPOINTS (0x800D5760 + 0x19) and 0x8010C17B
    constexpr float clock_seconds = 20.0f;   // PS1 0x800415C4: gp+0x144 += 0x14 per clock

    // Whether a version has clocks: the N64 SF Rush course (0-6), or a custom track by its mod id and folder.
    bool has_clocks_n64(int course);
    bool has_clocks(const std::string& mod_id, const std::string& folder);
    // Whether the PS1 clock can be drawn: the sfrush_ps1 mod is enabled and its clock files read (on a worker, started
    // by the first call; true while that first read runs). Any thread.
    bool available();
    // Game thread. Whether this race is a Time Sprint: CHECKPOINTS at CLOCKS on a version with clocks.
    bool on(uint8_t* rdram);
}

#endif
