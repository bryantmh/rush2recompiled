// Race recorder (test aid for tools/trackview): RUSH2_RACE_RECORD=<file> writes every car's pose and race state every
// physics tick of a race, for the track previewer's replay (docs/custom_tracks.md#race-replays).
//
// Called from the end of each physics tick (func_80076578 at 0x8007660C, after the players' and drones' steps; the
// same hook as src/ghost.cpp's ghost steps). Recording starts on the first tick of a race (game state 3 and up) and
// ends when every computer car has finished or gone a minute without reaching a new checkpoint or moving 500 ft
// (stuck), or the race ends; then <file>.done is written (JSON: ticks, why it ended, each car's finish time), so a
// tool can close the game.
// Only the first race of a launch is recorded.
//
// File (little-endian): "R2RC", u32 version (1), u32 header length, the header (JSON: fields, cars, kinds, types,
// track folder, laps, backward), then per tick f32 game clock (0x80117488), f32 physics clock (0x8010C0E4) and, per
// car, one f32 per field. Fields, from the car state (0x801124A0 + car * 0x354: the pose a car is drawn with) and the
// physics struct (0x800F5470 + car * 0x81C):
//   x y z           state +0       position, world feet
//   fx fy fz        state +0x3C    forward row of the orientation (+0x24: right, up, forward)
//   ux uy uz        state +0x30    up row
//   vx vy vz        state +0xC     velocity, ft/s
//   speed           car +0x3D4     ft/s
//   steer           car +0x728     -1..1
//   throttle        car +0x734     0..1 (battle_ai.cpp drives a computer car through it)
//   brake           car +0x730     0..1
//   laps            car +0x7FD     laps completed
//   next_cp         car +0x7FE     next checkpoint
//   place           state +0xE9
//   finished        state +0xEA
//   wreck           state +0x342   wreck state (0 driving)
//   respawn         car +0x6C8     >= 0 while it glides back to the road, -1 once placed
//   step            car +0x714     the physics clock of the car's last step: drones are stepped round robin
//                                  (physics_tick_drones_80075C3C), each every other tick, so their pose holds between
//                                  steps and a replay interpolates between the ticks where this changes

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "recomp.h"
#include "rush2_hooks.h"
#include "track_mod_runtime.h"
#include "sim_run.h"

namespace {
    constexpr uint32_t car_states = 0x801124A0, car_state_size = 0x354;
    constexpr uint32_t cars = 0x800F5470, car_size = 0x81C;
    constexpr uint32_t game_state = 0x8010C0D0;      // 3 and up: a race
    constexpr uint32_t race_cars = 0x8010C15A;       // s8: the race's cars (ranked by func_800A1468)
    constexpr uint32_t race_laps = 0x8010C0E2;       // s16
    constexpr uint32_t backward_flag = 0x80119848;
    constexpr uint32_t game_clock = 0x80117488;      // f32 seconds
    constexpr uint32_t physics_clock = 0x8010C0E4;   // f32
    constexpr uint32_t f_kind = 0x7E8;               // u8: 1 drone, 2 human
    constexpr uint32_t f_type = 0x7EA;               // u8: car type
    constexpr uint32_t race_time = 0xEC;             // state: f32 race time once finished

    enum Source { state_f32, car_f32, car_s8, car_s16, state_s8 };
    struct Field { const char* name; Source src; uint32_t off; };
    constexpr Field fields[] = {
        { "x", state_f32, 0x0 }, { "y", state_f32, 0x4 }, { "z", state_f32, 0x8 },
        { "fx", state_f32, 0x3C }, { "fy", state_f32, 0x40 }, { "fz", state_f32, 0x44 },
        { "ux", state_f32, 0x30 }, { "uy", state_f32, 0x34 }, { "uz", state_f32, 0x38 },
        { "vx", state_f32, 0xC }, { "vy", state_f32, 0x10 }, { "vz", state_f32, 0x14 },
        { "speed", car_f32, 0x3D4 }, { "steer", car_f32, 0x728 }, { "throttle", car_f32, 0x734 },
        { "brake", car_f32, 0x730 }, { "laps", car_s8, 0x7FD }, { "next_cp", car_s16, 0x7FE },
        { "place", state_s8, 0xE9 }, { "finished", state_s8, 0xEA }, { "wreck", state_s8, 0x342 },
        { "respawn", car_s16, 0x6C8 }, { "step", car_f32, 0x714 },
    };
    constexpr size_t field_count = sizeof(fields) / sizeof(fields[0]);

    std::mutex mutex;
    FILE* out = nullptr;
    std::string path;
    bool done = false;
    int n_cars = 0;
    uint32_t ticks = 0;
    std::vector<float> row;
    constexpr uint32_t stuck_ticks = 60 * 60;       // a minute of physics ticks without progress
    constexpr float progress_distance = 500.0f;      // ft from where it last made progress, which counts as progress
    int last_progress[8];                            // laps * 16 + next checkpoint, per car
    uint32_t progress_tick[8];                       // the tick it last changed
    float progress_at[8][2];                         // x, z there
    constexpr uint32_t lost_ticks = 3 * 60 * 60;     // three minutes without a new checkpoint, however far it drove
    uint32_t checkpoint_tick[8];                     // the tick it last reached a new checkpoint

    float f32_at(uint8_t* rdram, uint32_t addr) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        std::memcpy(&f, &w, 4);
        return f;
    }

    float read_field(uint8_t* rdram, int car, const Field& f) {
        uint32_t st = car_states + uint32_t(car) * car_state_size, ph = cars + uint32_t(car) * car_size;
        switch (f.src) {
        case state_f32: return f32_at(rdram, st + f.off);
        case car_f32: return f32_at(rdram, ph + f.off);
        case car_s8: return (float)(int8_t)MEM_B(0, (int32_t)(ph + f.off));
        case car_s16: return (float)(int16_t)MEM_H(0, (int32_t)(ph + f.off));
        case state_s8: return (float)(int8_t)MEM_B(0, (int32_t)(st + f.off));
        }
        return 0.0f;
    }

    void start(uint8_t* rdram) {
        out = std::fopen(path.c_str(), "wb");
        if (out == nullptr) {
            std::printf("[race_record] can't write %s\n", path.c_str());
            done = true;
            return;
        }
        n_cars = std::max(1, std::min(8, (int)(int8_t)MEM_B(0, (int32_t)race_cars)));
        std::string h = "{\"version\":1,\"fields\":[";
        for (size_t i = 0; i < field_count; i++) h += std::string(i ? "," : "") + "\"" + fields[i].name + "\"";
        h += "],\"cars\":" + std::to_string(n_cars) + ",\"kinds\":[";
        for (int c = 0; c < n_cars; c++) h += (c ? "," : "") + std::to_string(MEM_BU(0, (int32_t)(cars + c * car_size + f_kind)));
        h += "],\"types\":[";
        for (int c = 0; c < n_cars; c++) h += (c ? "," : "") + std::to_string(MEM_BU(0, (int32_t)(cars + c * car_size + f_type)));
        std::string folder = rush2::track_mod::race_folder();
        h += "],\"track_folder\":\"" + folder + "\",\"laps\":" + std::to_string((int16_t)MEM_H(0, (int32_t)race_laps)) +
             ",\"backward\":" + std::to_string(MEM_BU(0, (int32_t)backward_flag) != 0 ? 1 : 0) + "}";
        uint32_t version = 1, len = (uint32_t)h.size();
        std::fwrite("R2RC", 1, 4, out);
        std::fwrite(&version, 4, 1, out);
        std::fwrite(&len, 4, 1, out);
        std::fwrite(h.data(), 1, h.size(), out);
        row.assign(2 + size_t(n_cars) * field_count, 0.0f);
        for (int c = 0; c < 8; c++) { last_progress[c] = -1; progress_tick[c] = 0; checkpoint_tick[c] = 0; }
        std::printf("[race_record] recording %d cars to %s\n", n_cars, path.c_str());
        std::fflush(stdout);
    }

    void finish(uint8_t* rdram, const char* why) {
        if (out == nullptr) return;
        std::fclose(out);
        out = nullptr;
        done = true;
        std::string j = "{\"ticks\":" + std::to_string(ticks) + ",\"why\":\"" + why + "\",\"finish_times\":[";
        for (int c = 0; c < n_cars; c++) {
            uint32_t st = car_states + uint32_t(c) * car_state_size;
            bool fin = MEM_B(0, (int32_t)(st + 0xEA)) != 0;
            j += (c ? "," : "") + (fin ? std::to_string(f32_at(rdram, st + race_time)) : std::string("null"));
        }
        j += "]}";
        if (FILE* d = std::fopen((path + ".done").c_str(), "wb")) {
            std::fwrite(j.data(), 1, j.size(), d);
            std::fclose(d);
        }
        std::printf("[race_record] done (%s), %u ticks\n", why, ticks);
        std::fflush(stdout);
    }
}

extern "C" void rush2_race_record_tick(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    static const char* env = std::getenv("RUSH2_RACE_RECORD");
    if (env == nullptr || *env == 0) return;
    std::lock_guard lock{ mutex };
    if (done) return;
    bool racing = (int32_t)MEM_W(0, (int32_t)game_state) >= 3;
    if (out == nullptr) {
        if (!racing) return;
        path = env;
        start(rdram);
        if (out == nullptr) return;
    }
    else if (!racing) {
        finish(rdram, "race ended");
        if (rush2::sim::active()) rush2::sim::finish();
        return;
    }
    rush2::sim::race_tick(rdram);
    row[0] = f32_at(rdram, game_clock);
    row[1] = f32_at(rdram, physics_clock);
    // The race itself never ends while player 1's car (parked on the grid in a trackview run) hasn't finished, and a
    // stuck computer car never does with the Disable Auto-Abort cheat on: the recording ends once each computer car
    // has finished or gone stuck_ticks without reaching a new checkpoint or moving progress_distance (lost_ticks
    // without a checkpoint at all: a car put back again and again drives off each time), or after stuck_ticks when
    // there are none
    // (an arena or a race without drones: nothing would ever end it).
    int computers = 0, settled = 0, finished = 0;
    for (int c = 0; c < n_cars; c++) {
        for (size_t i = 0; i < field_count; i++) row[2 + size_t(c) * field_count + i] = read_field(rdram, c, fields[i]);
        if (MEM_BU(0, (int32_t)(cars + c * car_size + f_kind)) != 1) continue;
        computers++;
        uint32_t ph = cars + uint32_t(c) * car_size;
        int progress = (int8_t)MEM_B(0, (int32_t)(ph + 0x7FD)) * 16 + (int16_t)MEM_H(0, (int32_t)(ph + 0x7FE));
        // A car still covering ground isn't stuck: California Speed's checkpoints can be more than a minute apart.
        float x = f32_at(rdram, car_states + uint32_t(c) * car_state_size);
        float z = f32_at(rdram, car_states + uint32_t(c) * car_state_size + 8);
        if (progress != last_progress[c] || ticks == 0) checkpoint_tick[c] = ticks;
        if (progress != last_progress[c] || ticks == 0 ||
            std::hypot(x - progress_at[c][0], z - progress_at[c][1]) > progress_distance) {
            last_progress[c] = progress;
            progress_tick[c] = ticks;
            progress_at[c][0] = x;
            progress_at[c][1] = z;
        }
        if (MEM_B(0, (int32_t)(car_states + c * car_state_size + 0xEA)) != 0) {
            finished++;
            settled++;
        }
        else if (ticks - progress_tick[c] >= stuck_ticks || ticks - checkpoint_tick[c] >= lost_ticks) {
            settled++;
        }
    }
    std::fwrite(row.data(), sizeof(float), row.size(), out);
    if (++ticks % 120 == 0) std::fflush(out);
    if (computers == 0 && ticks >= stuck_ticks) {
        finish(rdram, "no computer cars");
        if (rush2::sim::active()) rush2::sim::finish();
    }
    else if (computers > 0 && settled == computers) {
        finish(rdram, finished == computers ? "computer cars finished" : "computer cars finished or stuck");
        if (rush2::sim::active()) rush2::sim::finish();
    }
}
