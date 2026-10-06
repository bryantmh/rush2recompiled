// San Francisco Rush (Rush 1) race tracks, raced in the borrowed Rush 2 track slot the Rush 2049 tracks use (the
// host slot, rush2::track2049::host_slot).
//
// The track select offers the Rush 1 tracks as extra entries (src/track2049_menu.cpp). When one is raced, the race
// setup hook turns its menu id into the host slot's id; the track is converted from the user's Rush 1 ROM
// (src/track1_convert.cpp) and replaces the host slot's five files (geometry, placement, collision, forward and
// backward AI paths), its entries in the per-track tables the race code reads, and its in-race logo, as for a 2049
// track (src/track2049.cpp). The host's own values are put back before anything else is raced there.
//
// Differences from the 2049 tracks:
// - Rush 1 has a collision file per direction, so the collision served depends on the race's backward flag.
// - The sky is Rush 2's procedural dome (Rush 1 uses the same, with the same texture names); no sky hooks apply.
// - Visibility: the converted table lives in recomp memory like the 2049 one. Rush 1's section chain can be longer
//   than its region count (track 2: 126 sections, 111 regions); regions past the table see everything.
// - Fog: Rush 1's fog colour is a game option (default grey 0x9696BE), not per track; that default is used.
// - Breakables: Rush 1's cones, meters, trees, flags, fences, gas signs, windows, traffic lights and trash munchers are Rush 2 breakable
//   class records (src/track1_convert.cpp). While the track is applied, the placement walker's lookup of a class model
//   is redirected to the record's own Rush 1 model, and Rush 2's breakable pieces (CONE1O1, FENCEO1-12, ...) to Rush
//   1's (rush2_track1_record_model, rush2_track1_model_name).
// - Music and the fireworks sound are Rush 1's (src/track1_audio.cpp).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "assets.h"
#include "rush2_hooks.h"
#include "track1.h"
#include "track2049.h"

using rush2::track2049::host_slot;

namespace {
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t backward_flag = 0x80119848;
    constexpr uint32_t fog_colours = 0x800C1D7C;    // 3 bytes per slot.
    constexpr uint32_t cloud_scroll = 0x800C1D14;   // f32 per slot.
    constexpr uint32_t sky_parallax = 0x800C1D44;   // f32 per slot.
    constexpr uint32_t pvs_counts = 0x800CA1A8;     // u8 per slot.
    constexpr uint32_t flag_nudge_x = 0x800C3F84;   // s32 per slot.
    constexpr uint32_t flag_nudge_y = 0x800C3FB4;   // s32 per slot.
    constexpr uint32_t prop_lists = 0x800C5D2C;     // ptr per slot.
    constexpr uint32_t demo_lists = 0x800C456C;     // ptr per slot + 12 * backward.
    constexpr uint32_t demo_counts = 0x800C45CC;    // s16 per slot + 12 * backward.
    constexpr uint32_t songs = 0x800CC37C;          // s16 per slot.
    constexpr uint32_t all_visible = 0x800CA1B8;    // The game's default mask (every section visible).

    const std::string prefix_of_host = "HAWAII";
    constexpr uint8_t fog[3] = { 0x96, 0x96, 0xBE };
    // Rush 2 songs (indices into 0x800CC394) for the Rush 1 tracks.
    constexpr int16_t track_songs[rush2::track1::track_count] = { 1, 0, 4, 2, 3, 5, 6 };

    std::mutex track_mutex;
    std::atomic_int raced_track = 0;   // 1-7, or 0 for none.
    int loaded_track = 0;               // The Rush 1 track in `track`.
    const std::vector<uint8_t>* loaded_rom = nullptr;
    rush2::track1::ConvertedTrack track;
    std::vector<uint8_t> race_logo;
    bool applied = false;
    int applied_backward = -1;
    uint32_t pvs_table = 0;   // In recomp memory.
    std::atomic<uint32_t> pvs_camera = 0;   // Camera position of the view func_8007C27C is working on.
    uint32_t demo_table = 0;
    // Model lookups redirected while the track is applied: name -> address of the replacement name in RDRAM.
    constexpr size_t name_pool_size = 0x1000;
    uint32_t name_pool = 0;
    std::map<std::string, uint32_t> record_redirects, piece_redirects;
    // Breakables' own models (rush2_track1_breakable_model), by breakable address; cleared when the track is applied.
    struct BreakableModel {
        uint32_t node;
        int16_t id;
        uint16_t model;
    };
    std::map<uint32_t, BreakableModel> breakable_models;
    float lap_seconds[rush2::track1::track_count][2] = {};

    struct Saved {
        uint8_t fog[3];
        uint32_t cloud, parallax, nudge_x, nudge_y, props;
        uint16_t song;
        uint32_t demo_list[2];
        uint16_t demo_count[2];
        uint8_t pvs_count;
    };
    Saved saved;

    void save_tables(uint8_t* rdram) {
        for (int i = 0; i < 3; i++) {
            saved.fog[i] = MEM_B(0, (int32_t)(fog_colours + host_slot * 3 + i));
        }
        saved.cloud = MEM_W(0, (int32_t)(cloud_scroll + host_slot * 4));
        saved.parallax = MEM_W(0, (int32_t)(sky_parallax + host_slot * 4));
        saved.nudge_x = MEM_W(0, (int32_t)(flag_nudge_x + host_slot * 4));
        saved.nudge_y = MEM_W(0, (int32_t)(flag_nudge_y + host_slot * 4));
        saved.props = MEM_W(0, (int32_t)(prop_lists + host_slot * 4));
        saved.pvs_count = MEM_B(0, (int32_t)(pvs_counts + host_slot));
        saved.song = MEM_H(0, (int32_t)(songs + host_slot * 2));
        for (int b = 0; b < 2; b++) {
            saved.demo_list[b] = MEM_W(0, (int32_t)(demo_lists + (host_slot + 12 * b) * 4));
            saved.demo_count[b] = MEM_H(0, (int32_t)(demo_counts + (host_slot + 12 * b) * 2));
        }
    }

    void apply(uint8_t* rdram, int backward) {
        if (!applied) {
            save_tables(rdram);
        }
        if (!race_logo.empty()) {
            rush2::assets::replace(rdram, 4 + host_slot, race_logo);
        }
        rush2::assets::replace(rdram, 0x33 + host_slot, track.geometry);
        rush2::assets::replace(rdram, 0x3F + host_slot, track.placement);
        rush2::assets::replace(rdram, 0x4B + host_slot, track.collision[backward]);
        rush2::assets::replace(rdram, 0x57 + host_slot, track.path[0]);
        rush2::assets::replace(rdram, 0x63 + host_slot, track.path[1]);
        for (int i = 0; i < 3; i++) {
            MEM_B(0, (int32_t)(fog_colours + host_slot * 3 + i)) = fog[i];
        }
        MEM_W(0, (int32_t)(cloud_scroll + host_slot * 4)) = 0x3E4CCCCD;   // 0.2, as most Rush 2 tracks
        MEM_W(0, (int32_t)(sky_parallax + host_slot * 4)) = 0;
        MEM_W(0, (int32_t)(flag_nudge_x + host_slot * 4)) = 0;
        MEM_W(0, (int32_t)(flag_nudge_y + host_slot * 4)) = 0;
        MEM_W(0, (int32_t)(prop_lists + host_slot * 4)) = 0;
        MEM_B(0, (int32_t)(pvs_counts + host_slot)) = track.pvs_count;
        MEM_H(0, (int32_t)(songs + host_slot * 2)) = track_songs[loaded_track - 1];
        if (demo_table == 0) {
            demo_table = (uint32_t)((uint8_t*)recomp::alloc(rdram, 2 * 32 * 2) - rdram) + 0x80000000;
        }
        for (int b = 0; b < 2; b++) {
            uint32_t list = demo_table + b * 64;
            size_t n = std::min<size_t>(track.demo_starts[b].size(), 32);
            for (size_t i = 0; i < n; i++) {
                MEM_H(0, (int32_t)(list + i * 2)) = track.demo_starts[b][i];
            }
            MEM_W(0, (int32_t)(demo_lists + (host_slot + 12 * b) * 4)) = list;
            MEM_H(0, (int32_t)(demo_counts + (host_slot + 12 * b) * 2)) = (int16_t)n;
        }
        if (pvs_table == 0) {
            pvs_table = (uint32_t)((uint8_t*)recomp::alloc(rdram, 128 * 16) - rdram) + 0x80000000;
        }
        for (size_t i = 0; i < track.pvs.size() && i < 128 * 16; i++) {
            MEM_B(0, (int32_t)(pvs_table + i)) = track.pvs[i];
        }
        if (name_pool == 0) {
            name_pool = (uint32_t)((uint8_t*)recomp::alloc(rdram, name_pool_size) - rdram) + 0x80000000;
        }
        record_redirects.clear();
        piece_redirects.clear();
        breakable_models.clear();
        uint32_t at = name_pool;
        auto add = [&](std::map<std::string, uint32_t>& to, const std::string& from, const std::string& name) {
            if (at + 16 > name_pool + name_pool_size) return;
            for (size_t i = 0; i < 16; i++) {
                MEM_B(0, (int32_t)(at + i)) = i < name.size() ? name[i] : 0;
            }
            to[from] = at;
            at += 16;
        };
        for (const auto& [from, name] : track.record_models) add(record_redirects, from, name);
        for (const auto& [from, name] : track.piece_models) add(piece_redirects, from, name);
        applied = true;
        applied_backward = backward;
    }

    void restore(uint8_t* rdram) {
        if (!applied) {
            return;
        }
        for (int index : { 0x33, 0x3F, 0x4B, 0x57, 0x63, 4 }) {
            rush2::assets::restore(rdram, index + host_slot);
        }
        for (int i = 0; i < 3; i++) {
            MEM_B(0, (int32_t)(fog_colours + host_slot * 3 + i)) = saved.fog[i];
        }
        MEM_W(0, (int32_t)(cloud_scroll + host_slot * 4)) = saved.cloud;
        MEM_W(0, (int32_t)(sky_parallax + host_slot * 4)) = saved.parallax;
        MEM_W(0, (int32_t)(flag_nudge_x + host_slot * 4)) = saved.nudge_x;
        MEM_W(0, (int32_t)(flag_nudge_y + host_slot * 4)) = saved.nudge_y;
        MEM_W(0, (int32_t)(prop_lists + host_slot * 4)) = saved.props;
        MEM_B(0, (int32_t)(pvs_counts + host_slot)) = saved.pvs_count;
        MEM_H(0, (int32_t)(songs + host_slot * 2)) = saved.song;
        for (int b = 0; b < 2; b++) {
            MEM_W(0, (int32_t)(demo_lists + (host_slot + 12 * b) * 4)) = saved.demo_list[b];
            MEM_H(0, (int32_t)(demo_counts + (host_slot + 12 * b) * 2)) = saved.demo_count[b];
        }
        record_redirects.clear();
        piece_redirects.clear();
        applied = false;
        applied_backward = -1;
    }

    std::string read_name(uint8_t* rdram, uint32_t addr) {
        std::string s;
        for (int i = 0; i < 16; i++) {
            char c = (char)MEM_B(0, (int32_t)(addr + i));
            if (c == 0) break;
            s.push_back(c);
        }
        return s;
    }

    bool convert(uint8_t* rdram, int k) {
        auto rom = rush2::track1::get_rom();
        if (rom == nullptr) {
            return false;
        }
        std::string error;
        if (!rush2::track1::convert_track(*rom, k - 1, prefix_of_host, track, error)) {
            printf("[Rush1] Couldn't convert track %d: %s\n", k, error.c_str());
            return false;
        }
        lap_seconds[k - 1][0] = track.lap_seconds[0];
        lap_seconds[k - 1][1] = track.lap_seconds[1];
        race_logo.clear();
        std::vector<uint8_t> logo;
        if (!rush2::assets::read_original(rdram, 4 + host_slot, logo) ||
            !rush2::track1::build_race_logo(logo, *rom, k - 1, race_logo)) {
            race_logo.clear();
        }
        return true;
    }
}

int rush2::track1::race_track() {
    return raced_track;
}

void rush2::track1::set_race_track(int k) {
    raced_track = k;
}

void rush2::track1::restore_host(uint8_t* rdram) {
    std::lock_guard lock{ track_mutex };
    restore(rdram);
}

bool rush2::track1::load(uint8_t* rdram) {
    int k = raced_track;
    if (k == 0 || MEM_B(0, (int32_t)track_id) != host_slot) {
        restore_host(rdram);
        return false;
    }
    // The 2049 tracks share the host slot: put its own values back before saving them.
    rush2::track2049::restore_host(rdram);
    std::lock_guard lock{ track_mutex };
    auto rom = get_rom();
    if (loaded_track != k || loaded_rom != rom.get()) {
        loaded_track = 0;
        loaded_rom = nullptr;
        if (!convert(rdram, k)) {
            printf("[Rush1] Track %d isn't available; racing the host track\n", k);
            raced_track = 0;
            restore(rdram);
            return false;
        }
        loaded_track = k;
        loaded_rom = rom.get();
    }
    int backward = MEM_B(0, (int32_t)backward_flag) != 0 ? 1 : 0;
    apply(rdram, backward);
    return true;
}

bool rush2::track1::pvs(uint8_t* rdram, uint32_t sp) {
    std::lock_guard lock{ track_mutex };
    if (!applied || MEM_B(0, (int32_t)track_id) != host_slot) {
        return false;
    }
    // Rush 1's region (func_80064544), not Rush 2's: the top-level record whose box holds the camera in x and z and
    // whose top is above it (no bottom test; Rush 2 tests the bottom and not the top, so high in the air it can pick
    // a lower section whose mask leaves out the ground below), the one with the least |dx| + |dz|.
    constexpr uint32_t records = 0x8010C15C;   // Placement record base.
    int32_t region = -1;
    uint32_t base = (uint32_t)MEM_W(0, (int32_t)records);
    if (base != 0 && pvs_camera != 0) {
        auto f = [&](uint32_t addr) {
            uint32_t w = (uint32_t)MEM_W(0, (int32_t)addr);
            float v;
            memcpy(&v, &w, 4);
            return v;
        };
        float cam[3] = { f(pvs_camera), f(pvs_camera + 4), f(pvs_camera + 8) };
        float best = 0.0f;
        uint32_t rec = base;
        for (int k = 0; k < 1024; k++) {
            float dx = cam[0] - f(rec + 0x34), dy = cam[1] - f(rec + 0x38), dz = cam[2] - f(rec + 0x3C);
            if (f(rec + 0x4C) <= dx && dx <= f(rec + 0x58) && dy <= f(rec + 0x5C) && f(rec + 0x54) <= dz &&
                dz <= f(rec + 0x60)) {
                float score = std::fabs(dz) + std::fabs(dx);
                if (region < 0 || score < best) {
                    best = score;
                    region = k;
                }
            }
            int16_t next = (int16_t)MEM_H(0, (int32_t)(rec + 0x44));
            if (next < 0) {
                break;
            }
            rec = base + (uint32_t)next * 0x64;
        }
    }
    // No region, or one past the table (track 2 has more sections than regions): everything is visible.
    if (region >= 0 && region < track.pvs_count) {
        MEM_W(0, (int32_t)(sp + 0x5C)) = pvs_table + region * 16;
    }
    else {
        MEM_W(0, (int32_t)(sp + 0x5C)) = all_visible;
    }
    return true;
}

// Start of func_8007C27C (the section mask for a view): $a1 = the camera position.
extern "C" void rush2_track1_pvs_camera(uint8_t* rdram, recomp_context* ctx) {
    pvs_camera = (uint32_t)ctx->r5;
}

// func_8008CDA4 at 0x8008D3A4, just after it stores a human's wrong-way angle ($s0 = the car's state, +0x34C the
// angle, +0x338 the time it started looking). Rush 1 (func_8009BE68) skips the check while the car's last or next
// checkpoint has flag 4 (track 6's figure 8), clearing both, as Rush 2 does for a car it doesn't check.
extern "C" void rush2_track1_wrong_way(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t states = 0x801124A0, cars = 0x800F5470;
    if (raced_track == 0) {
        return;
    }
    uint32_t state = (uint32_t)ctx->r16;
    uint32_t car = cars + (state - states) / 0x354 * 0x81C;
    int last = (int16_t)MEM_H(0, (int32_t)(car + 0x7FE)), next = (int16_t)MEM_H(0, (int32_t)(car + 0x800));
    std::lock_guard lock{ track_mutex };
    if (!applied || MEM_B(0, (int32_t)track_id) != host_slot) {
        return;
    }
    uint16_t mask = track.timing[applied_backward == 1 ? 1 : 0].no_wrong_way;
    auto flagged = [&](int i) { return i >= 0 && i < 16 && (mask >> i & 1) != 0; };
    if (flagged(last) || flagged(next)) {
        MEM_W(0, (int32_t)(state + 0x338)) = 0;
        MEM_W(0, (int32_t)(state + 0x34C)) = 0;
    }
}

// Rush 1's race timer (func_800B9F8C start, func_8009FFCC checkpoints; docs/rush1_research.md):
//   start = base - 4 x difficulty + 4 x laps + 16, unscaled;
//   reaching checkpoint i adds time[lap](i) x (1 + (5 - difficulty) x 0.05), time[2] from lap 3 on.
// Rush 2 (func_800AE670, func_8008F220) multiplies the start (header +0) and the passed checkpoint's extension (+0x1E
// on lap 1, +0x20 after) by 1 + (5 - difficulty) x 0.075, so the fields get Rush 1's values divided by that. Both
// games start the race with checkpoint 0 passed, and grant a checkpoint's time when it is passed. Rush 2 has no
// lap-3 field; Rush 1's lap 2 and lap 3 times are the same on every track.
bool rush2::track1::race_time(uint8_t* rdram) {
    constexpr uint32_t header = 0x8010BCE8;     // Copy of the path header, checkpoints at +0xC, 0x50 bytes each.
    constexpr uint32_t difficulty = 0x8010C211; // 0-5.
    constexpr uint32_t laps = 0x8010C0E2;
    std::lock_guard lock{ track_mutex };
    if (!applied || raced_track == 0 || MEM_B(0, (int32_t)track_id) != host_slot) {
        return false;
    }
    const ConvertedTrack::Timing& timing = track.timing[applied_backward == 1 ? 1 : 0];
    int count = (int16_t)MEM_H(0, (int32_t)(header + 8));
    if (count != (int)timing.checkpoints.size()) {
        return true;
    }
    int d = std::clamp((int)(int8_t)MEM_B(0, (int32_t)difficulty), 0, 5);
    int lap_count = std::max((int)(int16_t)MEM_H(0, (int32_t)laps), 1);
    double rush1_factor = 1.0 + (5 - d) * 0.05;
    double rush2_factor = 1.0 + (5 - d) * 0.075;
    auto put = [&](uint32_t addr, double seconds) {
        MEM_H(0, (int32_t)addr) = (int16_t)std::max(std::lround(seconds / rush2_factor), 0L);
    };
    put(header, timing.start - 4 * d + 4 * lap_count + 16);
    for (int i = 0; i < count; i++) {
        uint32_t cp = header + 0xC + i * 0x50;
        put(cp + 0x1E, timing.checkpoints[i][0] * rush1_factor);
        put(cp + 0x20, timing.checkpoints[i][1] * rush1_factor);
    }
    return true;
}

float rush2::track1::record_seed(int k, bool backward) {
    if (k < 1 || k > track_count) {
        return 0.0f;
    }
    std::lock_guard lock{ track_mutex };
    float s = lap_seconds[k - 1][backward ? 1 : 0];
    if (s <= 0.0f) {
        // Not converted yet in this session: convert just the times.
        auto rom = get_rom();
        ConvertedTrack ct;
        std::string error;
        if (rom != nullptr && convert_track(*rom, k - 1, prefix_of_host, ct, error)) {
            lap_seconds[k - 1][0] = ct.lap_seconds[0];
            lap_seconds[k - 1][1] = ct.lap_seconds[1];
            s = lap_seconds[k - 1][backward ? 1 : 0];
        }
    }
    return s;
}

// func_80081790 at 0x8008194C, the placement walker's model lookup ($a0 = the record's model name, or its class's
// Rush 2 model): a Rush 1 breakable record draws its own model (record name at 0x800D5790).
extern "C" void rush2_track1_record_model(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t current_record = 0x800D5790;
    std::lock_guard lock{ track_mutex };
    if (record_redirects.empty()) {
        return;
    }
    uint32_t record = (uint32_t)MEM_W(0, (int32_t)current_record);
    auto it = record_redirects.find(read_name(rdram, record));
    if (it != record_redirects.end()) {
        ctx->r4 = (int32_t)it->second;
    }
}

// Start of func_8005BE3C (model lookup by name, $a0): Rush 2 breakable pieces resolve to the Rush 1 track's own.
extern "C" void rush2_track1_model_name(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ track_mutex };
    uint32_t name = (uint32_t)ctx->r4;
    if (piece_redirects.empty() || name == 0) {
        return;
    }
    auto it = piece_redirects.find(read_name(rdram, name));
    if (it != piece_redirects.end()) {
        ctx->r4 = (int32_t)it->second;
    }
}

// func_8008A01C at 0x8008A0CC, the per-frame breakable update ($s0 = the breakable, $t3 = its node, $a0 = the model
// about to be set): Rush 2 draws a breakable with the model of its +0x62 id (the class's CONE1O1, METERO1,
// TREEHIT1O1, ...), which on a Rush 1 track would replace the record's own Rush 1 model (a traffic light would draw
// as a meter, a trash muncher as a tree). While the id is still the one the breakable started with (not hit), the
// node keeps the model it was created with.
extern "C" void rush2_track1_breakable_model(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ track_mutex };
    if (record_redirects.empty()) {
        return;
    }
    uint32_t breakable = (uint32_t)ctx->r16;
    uint32_t node = (uint32_t)ctx->r11;
    int16_t id = (int16_t)MEM_H(0, (int32_t)(breakable + 0x62));
    auto it = breakable_models.find(breakable);
    if (it == breakable_models.end() || it->second.node != node) {
        // First update since the breakable was created: the node still has its own model.
        it = breakable_models.insert_or_assign(breakable,
            BreakableModel{ node, id, (uint16_t)MEM_HU(0, (int32_t)(node + 0xC)) }).first;
    }
    if (id == it->second.id) {
        ctx->r4 = it->second.model;
    }
}

// Start of func_8008B0CC, the car hit test for a breakable ($a0 = the car, $a1 = the breakable): on a Rush 1 track,
// Rush 1's rule (func_8008602C): the breakable's point in the car's frame within the car's footprint, |z| < 7 and
// |x| < 3.5 (Rush 1 3), at any height. Rush 2 also wants it within about 2 units of the car's height, which on Rush
// 1's hills misses traffic lights and trees the car drives straight through. Windows (0x722) keep Rush 2's test.
extern "C" int rush2_track1_breakable_hit(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ track_mutex };
    uint32_t breakable = (uint32_t)ctx->r5;
    if (record_redirects.empty() || MEM_H(0, (int32_t)(breakable + 0x52)) == 0x722) {
        return 0;
    }
    auto f = [&](uint32_t addr) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)addr);
        float v;
        memcpy(&v, &w, 4);
        return v;
    };
    constexpr uint32_t cars = 0x801124A0;   // Car state, 0x354 each: position, then the rotation rows at +0x24.
    uint32_t car = cars + (int16_t)ctx->r4 * 0x354;
    float d[3];
    for (int i = 0; i < 3; i++) {
        d[i] = f(breakable + 0x2C + i * 4) - f(car + i * 4);
    }
    auto row = [&](int r) { return f(car + 0x24 + r * 12) * d[0] + f(car + 0x28 + r * 12) * d[1] + f(car + 0x2C + r * 12) * d[2]; };
    ctx->r2 = std::fabs(row(2)) < 7.0f && std::fabs(row(0)) < 3.5f;
    return 1;
}
