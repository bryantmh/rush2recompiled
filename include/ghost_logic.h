#ifndef __GHOST_LOGIC_H__
#define __GHOST_LOGIC_H__

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Rush 2049's ghost recording and playback, ported as pure logic: no RDRAM, no recomp context (the Rush 2 side is
// src/ghost.cpp). Addresses are Rush 2049's (tools/rush2049/out/d49m.asm):
//
// - func_800F6AB8 (race setup, game mode 2 = practice): each human player gets a 0x58-byte ghost header
//   (0x80142530 + i * 0x58) recording its track (+6), direction (+7), car (+8, +9, colors +0xA), name (+0x16), time
//   step (+0x34, 0x8002AFB8) and a sample buffer (+0x4C) of (max_time[track] + 10) * 60 samples, max_time being
//   the f32[6] at 0x80111754. Loaded ghosts become extra "player" cars driven from their samples.
// - func_800E5D64 (each physics tick, from the car loop of func_800E6AF8, before the car's step): a recording car
//   (+5 < 0) that hasn't finished writes one sample of its inputs at +0x50 and advances it, until +0x54 samples; a
//   ghost (+5 > 0) reads one into its car's inputs, or once they run out lets go of everything. Its return value
//   makes the caller skip a step (-1) or take an extra one (1) when the ghost was recorded at another time step
//   (PAL 0.02 s against NTSC 1/60 s).
// - func_800D510C (race end): a finished recording keeps its race time (+0x38) and its sample count (+0x54 =
//   +0x50); an unfinished one is dropped.
// - func_800CC50C / func_800CC040: the kept recording is packed (the three sample streams moved together, then
//   compressed) and written to the Controller Pak, replacing that track's ghost.
//
// Rush 2049's sample is 3 bytes: gear + 1 and two button bits (+0x730, +0x731 wings, +0x732 abort), steering x 127
// (+0x720), and throttle and brake x 15 (+0x728, +0x72C), and the ghost car is simulated from them. Rush 2 differs:
// its inputs are continuous floats and its game thread sets the clutch and gear once per frame, so a sample keeps
// the values exactly, plus the wing stick the recomp's wings read (Rush 2049's ghosts read their own controller's).
// Rush 2's game thread also changes a car between physics ticks (respawns after wrecks, aborts, damage), so each tick
// keeps the words of the car's physics struct that changed since its last step, and the struct at release and every
// second, so the ghost car repeats the recorded car exactly.
namespace rush2::ghost {
    constexpr float tick_rate = 60.0f;              // 0x8002AFB4
    constexpr float tick_step = 1.0f / 60.0f;       // 0x8002AFB8 (NTSC)
    constexpr float pal_step = 0.02f;               // 0x80124490
    constexpr float ntsc_step = 1.0f / 60.0f;       // 0x80124494
    constexpr float max_time_2049[6] = { 225.0f, 285.0f, 300.0f, 390.0f, 330.0f, 450.0f }; // 0x80111754
    constexpr int keyframe_ticks = 60;

    // One physics tick's inputs: Rush 2's car +0x728 steering, +0x72C clutch, +0x730 brake, +0x734 throttle, +0x738
    // gear, and the wings' button (bit 0) and style (bits 1-2) and stick. Rush 2049 steps its ghost on its own clock
    // (car +0x714 = sample count x step, +0x718 = the step); Rush 2 takes a car's step from its physics clock
    // (0x8010C0E4) and puts a wrecked car back by its game clock (0x80117488), so the sample keeps both as the
    // recorded car's step saw them.
    struct Input {
        float steer = 0.0f;
        float clutch = 0.0f;
        float throttle = 0.0f;
        float brake = 0.0f;
        int16_t gear = 1;
        uint8_t wings = 0;
        float stick_x = 0.0f;
        float stick_y = 0.0f;
        float physics_clock = 0.0f;
        float game_clock = 0.0f;
    };

    // A word the game thread changed on the recorded car before a tick: word index (the car's physics struct, then
    // its respawn target), value.
    struct Patch {
        uint16_t word;
        uint32_t value;
    };

    // The ghost header (2049: 0x58 bytes at 0x80142530 + i * 0x58).
    struct Header {
        int8_t state = 0;           // +5: < 0 recording, 0 idle, > 0 playing
        uint8_t track = 0;          // +6: track select id (Rush 2: 0-29)
        uint8_t backward = 0;       // +7
        uint8_t mirror = 0;         // Rush 2049 has no mirror option
        uint8_t laps = 0;           // Rush 2049 keeps a ghost per track; Rush 2's races have 1-8 laps
        uint8_t car_type = 0;       // +8
        uint8_t car_entry[9] = {};  // +9..: Rush 2's car table entry (0x800D9C10 + i * 9): type, colors, stripe
        char name[16] = {};         // +0x16: the profile's name
        float step = tick_step;     // +0x34
        float race_time = 0.0f;     // +0x38: seconds from release to the finish
        uint32_t position = 0;      // +0x50: next sample
        uint32_t count = 0;         // +0x54: capacity while recording, sample count once kept
    };

    struct Ghost {
        Header header;
        std::vector<Input> inputs;          // +0x4C: one per tick, from the car's release
        std::vector<uint32_t> patch_start;  // per tick, its first patch (one more entry than inputs)
        std::vector<Patch> patches;
        std::vector<uint32_t> start;        // the car's physics struct words at release
        std::vector<uint32_t> descriptor;   // the car's descriptor (car + 0) words
        std::vector<std::vector<uint32_t>> keyframes;   // the struct before every keyframe_ticks-th tick
        float physics_time = 0.0f;          // Rush 2's physics clock (0x8010C0E4) at release
        float game_time = 0.0f;             // and its game clock (0x80117488)
    };

    // Sample capacity of a recording on track select id `track`: (max_time + 10) * 60, as func_800F6AB8. Rush 2049's
    // table is per race track at its own lap count; Rush 2's and SF Rush's tracks, and races with more laps, allow
    // 150 s a lap.
    uint32_t capacity(int track, int laps);

    // func_800F6AB8: starts recording into `g` (state -1, empty sample buffer of `capacity` samples).
    void begin_recording(Ghost& g, const Header& header, uint32_t capacity);

    // func_800E5D64, recording branch: stores tick `g.header.position`'s inputs and the struct words the game thread
    // changed before it. False once the buffer is full (the sample is not kept).
    bool record(Ghost& g, const Input& in, const std::vector<Patch>& changed);

    // func_800E5D64, playback branch. Returns -1 when the ghost car skips this tick, 1 when it takes another step
    // after this one, else 0 (time steps differ only between PAL and NTSC recordings). `in` gets the tick's inputs,
    // or, past the last sample, the car lets go of everything (2049 zeroes steering, throttle, brake and buttons).
    // `patches`/`patch_count` are the tick's struct words, `keyframe` the struct at a keyframe tick (else null).
    int play(Ghost& g, int32_t tick_counter, Input& in, const Patch*& patches, size_t& patch_count,
             const std::vector<uint32_t>*& keyframe);

    // func_800D510C: the race ended. A finished recording keeps its time and sample count and stops (state 0); an
    // unfinished one is dropped. Returns whether `g` holds a ghost to keep.
    bool finish(Ghost& g, bool finished, float race_time);

    // Whether `candidate` beats the kept ghost `best` (func_800CC040 replaces the track's ghost on the pak; the recomp
    // keeps the fastest).
    bool better(const Ghost& candidate, const Ghost& best);

    // Ghost files (the recomp's Controller Pak notes): func_800CC50C packs the streams together.
    bool save(const std::filesystem::path& path, const Ghost& g);
    bool load(const std::filesystem::path& path, Ghost& g);
    // Just the header (the ghost choice lists ghosts by their driver and time).
    bool load_header(const std::filesystem::path& path, Header& h);
}

#endif
