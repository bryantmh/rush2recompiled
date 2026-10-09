// Rush 2049 race tracks, raced in a borrowed Rush 2 track slot (the host slot).
//
// The track select offers the 2049 tracks as extra entries (src/track2049_menu.cpp). When one is raced, the race
// setup hook turns its menu id into the host slot's id. The track is then converted from the user's 2049 ROM
// (src/track2049_convert.cpp) and replaces the host slot's five files (geometry 0x33+slot, placement 0x3F+slot,
// collision 0x4B+slot, forward and backward AI paths 0x57+slot / 0x63+slot), the slot's entries in the per-track
// tables the race code reads, and the slot's in-race logo. The original values are put back before a Rush 2 track is
// raced, so the host track itself is unchanged.
//
// Rush 2049's stunt arenas are hosted the same way by Rush 2's stunt track (slot 11, STUNT1), whose literal track
// tests give the race Rush 2's stunt mode: stunt scoring, start on the path's spine, no backward or mirror, no
// drones. An arena has one AI path, used both ways, and no visibility table (Rush 2049 draws every section of it);
// it keeps Rush 2's stunt song and STUNT1's records.
//
// Rush 2049's battle arenas (DM1-DM8) are hosted in the stunt slot the same way: a free-roaming arena with no drones or
// checkpoints. Their game type is battle (game_type), so the rules ported by type apply: no stuck reset, 0.6 s
// wreck respawn, no map or radar. Weapons, health and kill scoring aren't ported.
//
// Rush 2049's obstacle course runs from a start to a finish line against a 5-minute clock, with no stunt scoring, so
// it is hosted like a race track (raced_track = obstacle) and raced as Rush 2's one-race mode would race it: one lap,
// no drones, no backward or mirror, and checkpoints on, whose clock is set to Rush 2049's 5 minutes with no
// extensions (rush2_track49_obstacle_settings, rush2_track49_obstacle_clock, rush2_track49_race_time).
//
// Code support for things the files can't express:
// - Visibility: func_8007C27C picks a 16-byte section mask per camera region from a per-track table that is sized
//   for the slot's own track, so the converted table lives in recomp memory and the hook at 0x8007C480 points the
//   game at it.
// - Sky: Rush 2049 draws its tracks' sky as a model, which Rush 2 only does for Las Vegas (func_800A45A8); the
//   converted geometry names it SKYO1, and the hook at 0x800A4644 sends the host slot down Las Vegas's branch.
//   Rush 2 also builds no sky in split screen, while Rush 2049 does (hook at 0x800A4600). The obstacle course has
//   no sky model (Rush 2049 shows its black fog colour around it), so it takes the split-screen path: no sky.

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <cstring>


#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "rush2_hooks.h"
#include "battle.h"
#include "rush2.h"
#include "assets.h"
#include "track1.h"
#include "track2049.h"
#include "track2049_convert.h"
#include "track_cache.h"
#include "car2049.h"
#include "rush2049_rom.h"
#include "wings.h"

using rush2::track2049::host_slot;
using rush2::track2049::stunt_host_slot;

namespace {

    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t fog_colours = 0x800C1D7C;    // 3 bytes per slot.
    constexpr uint32_t cloud_scroll = 0x800C1D14;   // f32 per slot.
    constexpr uint32_t sky_parallax = 0x800C1D44;   // f32 per slot.
    constexpr uint32_t pvs_counts = 0x800CA1A8;     // u8 per slot.
    constexpr uint32_t flag_nudge_x = 0x800C3F84;   // s32 per slot.
    constexpr uint32_t flag_nudge_y = 0x800C3FB4;   // s32 per slot.
    constexpr uint32_t prop_lists = 0x800C5D2C;     // ptr per slot.
    constexpr uint32_t demo_lists = 0x800C456C;     // ptr per slot + 12 * backward: demo start spine indices.
    constexpr uint32_t demo_counts = 0x800C45CC;    // s16 per slot + 12 * backward.
    constexpr uint32_t songs = 0x800CC37C;          // s16 per slot: default song (func_8008C370).
    constexpr uint32_t asset_offsets = 0x800C185C;  // ROM offset per asset.

    // Rush 2049's per-track song choice (0x8010FFD4), which happens to index Rush 2's eight songs too.
    constexpr int16_t track_songs[rush2::track2049::track_count] = { 0, 1, 4, 2, 3, 7 };
    // The host slots' track prefixes (0x800C182C).
    std::string prefix_of(int slot) {
        return slot == stunt_host_slot ? "STUNT1" : "HAWAII";
    }

    std::mutex track_mutex;
    std::atomic_bool option_enabled = true;
    std::atomic_int raced_track = 0;  // 1-6 or obstacle, or 0 for none.
    std::atomic_int raced_stunt = 0;  // Stunt arena 1-4, or 0 for none.
    std::atomic_int raced_battle = 0; // Battle arena 1-8, or 0 for none (hosted in the stunt slot like the stunt arenas).
    int loaded_track = 0;              // The 2049 track in `track`, as convert_track's k.
    int slot = host_slot;              // The slot `track` is applied to.
    std::atomic_int applied_slot = -1; // `slot` while applied, or -1.
    rush2::track2049::ConvertedTrack track;
    bool track_has_sky = false;        // The converted geometry has a SKYO1 model.
    std::set<std::string> shared_models;   // Rush 2's shared model names, read once.
    std::vector<uint8_t> race_logo;
    bool applied = false;
    uint32_t pvs_table = 0; // In recomp memory.
    uint32_t demo_table = 0;

    // Original table bytes of the host slot, saved on first use.
    struct Saved {
        uint8_t fog[3];
        uint32_t cloud, parallax, nudge_x, nudge_y, props;
        uint16_t song;
        uint32_t demo_list[2];
        uint16_t demo_count[2];
        uint8_t pvs_count;
    };
    Saved saved;

    // Converts 2049 track k (convert_track's k) from the user's ROM for the slot in `slot`.
    // The converted track comes from the disk cache (src/track_cache.cpp) when this build converted it from the same
    // source before; converting runs inside the race setup, on the race's first frame. The source key is the N64 ROM's
    // hash, or for a Dreamcast disc the track's own geometry and collision files.
    bool convert(uint8_t* rdram, int k) {
        auto rom = rush2::wings::get_rom();
        if (rom == nullptr) {
            return false;
        }
        std::vector<uint8_t> geometry_2049, collision_2049;
        if (!rom->read_file(100 + k, geometry_2049) ||
            !rom->read_file(138 + k, collision_2049)) {
            return false;
        }
        static const rush2::rom2049::Source* hashed_rom = nullptr;
        static uint64_t rom_hash = 0;
        uint64_t source;
        if (const std::vector<uint8_t>* n64 = rom->n64_rom()) {
            if (hashed_rom != rom.get()) {
                rom_hash = rush2::track_cache::hash(n64->data(), n64->size());
                hashed_rom = rom.get();
            }
            source = rom_hash;
        }
        else {
            source = rush2::track_cache::hash(geometry_2049.data(), geometry_2049.size(), 0xDC);
            source = rush2::track_cache::hash(collision_2049.data(), collision_2049.size(), source);
        }
        if (!rush2::track_cache::load_2049(k, prefix_of(slot), source, track)) {
            if (shared_models.empty()) {
                auto rush2_rom = recomp::get_rom();
                std::vector<uint8_t> copy(rush2_rom.begin(), rush2_rom.end());
                rush2::track2049::rush2_shared_model_names(copy, (uint32_t)MEM_W(0, (int32_t)(asset_offsets + 0x12 * 4)),
                    (uint32_t)MEM_W(0, (int32_t)(asset_offsets + 0x14 * 4)), shared_models);
            }
            std::string error;
            if (!rush2::track2049::convert_track(*rom, k, prefix_of(slot), shared_models, true, track, error)) {
                printf("[2049] Couldn't convert track %d: %s\n", k, error.c_str());
                return false;
            }
            rush2::track_cache::save_2049(k, prefix_of(slot), source, track);
        }
        std::set<std::string> names = rush2::track2049::model_names(track.geometry);
        track_has_sky = names.contains("SKYO1");
        rush2::track2049::set_mover_data(geometry_2049, collision_2049, track.path_records, track.spin_records,
                                         std::vector<std::string>(names.begin(), names.end()));
        rush2::track2049::set_texanim_data(track.tex_anims);
        rush2::track2049::set_prop_data(track.prop_records, track.geometry);
        rush2::battle::set_data(track.pickup_records, track.pool_records, track.geometry, track.solid_triangles);
        return true;
    }

    void save_tables(uint8_t* rdram) {
        for (int i = 0; i < 3; i++) {
            saved.fog[i] = MEM_B(0, (int32_t)(fog_colours + slot * 3 + i));
        }
        saved.cloud = MEM_W(0, (int32_t)(cloud_scroll + slot * 4));
        saved.parallax = MEM_W(0, (int32_t)(sky_parallax + slot * 4));
        saved.nudge_x = MEM_W(0, (int32_t)(flag_nudge_x + slot * 4));
        saved.nudge_y = MEM_W(0, (int32_t)(flag_nudge_y + slot * 4));
        saved.props = MEM_W(0, (int32_t)(prop_lists + slot * 4));
        saved.pvs_count = MEM_B(0, (int32_t)(pvs_counts + slot));
        saved.song = MEM_H(0, (int32_t)(songs + slot * 2));
        for (int b = 0; b < 2; b++) {
            saved.demo_list[b] = MEM_W(0, (int32_t)(demo_lists + (slot + 12 * b) * 4));
            saved.demo_count[b] = MEM_H(0, (int32_t)(demo_counts + (slot + 12 * b) * 2));
        }
    }

    void apply(uint8_t* rdram) {
        if (!applied) {
            save_tables(rdram);
        }
        if (!race_logo.empty()) {
            rush2::assets::replace(rdram, 4 + slot, race_logo);
        }
        rush2::assets::replace(rdram, 0x33 + slot, track.geometry);
        rush2::assets::replace(rdram, 0x3F + slot, track.placement);
        rush2::assets::replace(rdram, 0x4B + slot, track.collision);
        rush2::assets::replace(rdram, 0x57 + slot, track.path);
        rush2::assets::replace(rdram, 0x63 + slot, track.path_backward);
        for (int i = 0; i < 3; i++) {
            MEM_B(0, (int32_t)(fog_colours + slot * 3 + i)) = track.fog[i];
        }
        MEM_W(0, (int32_t)(cloud_scroll + slot * 4)) = 0;
        MEM_W(0, (int32_t)(sky_parallax + slot * 4)) = 0;
        MEM_W(0, (int32_t)(flag_nudge_x + slot * 4)) = 0;
        MEM_W(0, (int32_t)(flag_nudge_y + slot * 4)) = 0;
        MEM_W(0, (int32_t)(prop_lists + slot * 4)) = 0;
        MEM_B(0, (int32_t)(pvs_counts + slot)) = track.pvs_count;
        if (loaded_track <= rush2::track2049::track_count) {
            MEM_H(0, (int32_t)(songs + slot * 2)) = track_songs[loaded_track - 1];
        }
        if (demo_table == 0) {
            demo_table = (uint32_t)((uint8_t*)recomp::alloc(rdram, 2 * 32 * 2) - rdram) + 0x80000000;
        }
        for (int b = 0; b < 2; b++) {
            uint32_t list = demo_table + b * 64;
            size_t n = std::min<size_t>(track.demo_starts[b].size(), 32);
            for (size_t i = 0; i < n; i++) {
                MEM_H(0, (int32_t)(list + i * 2)) = track.demo_starts[b][i];
            }
            if (n == 0) {
                // Stunt arenas have no demo starts: the path's first spine point.
                MEM_H(0, (int32_t)list) = 0;
                n = 1;
            }
            MEM_W(0, (int32_t)(demo_lists + (slot + 12 * b) * 4)) = list;
            MEM_H(0, (int32_t)(demo_counts + (slot + 12 * b) * 2)) = (int16_t)n;
        }

        if (pvs_table == 0) {
            pvs_table = (uint32_t)((uint8_t*)recomp::alloc(rdram, 128 * 16) - rdram) + 0x80000000;
        }
        for (size_t i = 0; i < track.pvs.size() && i < 128 * 16; i++) {
            MEM_B(0, (int32_t)(pvs_table + i)) = track.pvs[i];
        }
        applied = true;
        applied_slot = slot;
    }

    void restore(uint8_t* rdram) {
        if (!applied) {
            return;
        }
        for (int index : { 0x33, 0x3F, 0x4B, 0x57, 0x63, 4 }) {
            rush2::assets::restore(rdram, index + slot);
        }
        for (int i = 0; i < 3; i++) {
            MEM_B(0, (int32_t)(fog_colours + slot * 3 + i)) = saved.fog[i];
        }
        MEM_W(0, (int32_t)(cloud_scroll + slot * 4)) = saved.cloud;
        MEM_W(0, (int32_t)(sky_parallax + slot * 4)) = saved.parallax;
        MEM_W(0, (int32_t)(flag_nudge_x + slot * 4)) = saved.nudge_x;
        MEM_W(0, (int32_t)(flag_nudge_y + slot * 4)) = saved.nudge_y;
        MEM_W(0, (int32_t)(prop_lists + slot * 4)) = saved.props;
        MEM_B(0, (int32_t)(pvs_counts + slot)) = saved.pvs_count;
        MEM_H(0, (int32_t)(songs + slot * 2)) = saved.song;
        for (int b = 0; b < 2; b++) {
            MEM_W(0, (int32_t)(demo_lists + (slot + 12 * b) * 4)) = saved.demo_list[b];
            MEM_H(0, (int32_t)(demo_counts + (slot + 12 * b) * 2)) = saved.demo_count[b];
        }
        applied = false;
        applied_slot = -1;
    }

    bool hosting(uint8_t* rdram) {
        return applied && MEM_B(0, (int32_t)track_id) == slot;
    }
}

void rush2::track2049::set_option(bool enabled) {
    option_enabled = enabled;
}

bool rush2::track2049::available() {
    return option_enabled && rush2::wings::rom_available();
}

void rush2::track2049::restore_host(uint8_t* rdram) {
    std::lock_guard lock{ track_mutex };
    restore(rdram);
}

int rush2::track2049::race_track() {
    return raced_track;
}

void rush2::track2049::set_race_track(int k) {
    raced_track = k;
}

int rush2::track2049::stunt_arena() {
    return raced_stunt;
}

void rush2::track2049::set_stunt_arena(int n) {
    raced_stunt = n;
}

int rush2::track2049::battle_arena() {
    return raced_battle;
}

void rush2::track2049::set_battle_arena(int n) {
    raced_battle = n;
}

int rush2::track2049::loaded_slot() {
    return applied_slot;
}

bool rush2::track2049::obstacle_race(uint8_t* rdram) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    // The track select's id before the race setup turns it into the host slot's.
    return t == obstacle_menu_id || (t == host_slot && raced_track == obstacle);
}

bool rush2::track2049::battle_race(uint8_t* rdram) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    return (t >= battle_menu_id && t < battle_menu_id + battle_count) || (t == stunt_host_slot && raced_battle != 0);
}

rush2::track2049::GameType rush2::track2049::game_type(uint8_t* rdram) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    if (t == host_slot && raced_track == obstacle) return GameType::obstacle;
    if (t == host_slot && raced_track != 0) return GameType::race;
    if (t == stunt_host_slot && raced_stunt != 0) return GameType::stunt;
    if (t == stunt_host_slot && raced_battle != 0) return GameType::battle;
    return GameType::none;
}

bool rush2::track2049::stuck_reset_off(uint8_t* rdram) {
    GameType g = game_type(rdram);
    return g == GameType::stunt || g == GameType::obstacle || g == GameType::battle;
}

bool rush2::track2049::quick_respawn(uint8_t* rdram) {
    GameType g = game_type(rdram);
    return g == GameType::obstacle || g == GameType::battle;
}

bool rush2::track2049::no_map(uint8_t* rdram) {
    GameType g = game_type(rdram);
    return g == GameType::obstacle || g == GameType::battle;
}

// Start of func_800A4C98, which queues the race's track files.
extern "C" void rush2_track49_load(uint8_t* rdram, recomp_context* ctx) {
    // SF Rush tracks race in the same slot (src/track1.cpp); it puts the slot's own values back first.
    if (rush2::track1::load(rdram)) {
        return;
    }
    std::lock_guard lock{ track_mutex };
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    int k = 0, want_slot = -1;
    if (raced_track != 0 && t == host_slot) {
        k = raced_track;
        want_slot = host_slot;
    }
    else if (raced_stunt != 0 && t == stunt_host_slot) {
        k = rush2::track2049::stunt_first + raced_stunt - 1;
        want_slot = stunt_host_slot;
    }
    else if (raced_battle != 0 && t == stunt_host_slot) {
        k = rush2::track2049::battle_first + raced_battle - 1;
        want_slot = stunt_host_slot;
    }
    if (k == 0 || (applied && slot != want_slot)) {
        restore(rdram);
    }
    if (k == 0) {
        return;
    }
    if (slot != want_slot) {
        slot = want_slot;
        loaded_track = 0;   // The conversion depends on the slot's track prefix.
    }
    if (loaded_track != k) {
        loaded_track = 0;
        if (!convert(rdram, k)) {
            printf("[2049] Track %d isn't available; racing the host track\n", k);
            // Everything (records included) then treats the race as the host track's.
            raced_track = 0;
            raced_stunt = 0;
            raced_battle = 0;
            restore(rdram);
            return;
        }
        race_logo.clear();
        std::vector<uint8_t> logo;
        auto rom = rush2::wings::get_rom();
        if (rom == nullptr || !rush2::assets::read_original(rdram, 4 + slot, logo) ||
            !rush2::track2049::build_race_logo(logo, *rom, k, race_logo)) {
            race_logo.clear();
        }
        loaded_track = k;
    }
    apply(rdram);
    rush2::track2049::reset_movers();
    rush2::track2049::reset_props();
    rush2::battle::reset();
    rush2::track2049::texanim_reset();
}

// func_8007C27C at 0x8007C480: 0x5C($sp) = the section mask chosen for the camera's region 0x78($sp) (-1 = none,
// which uses the all-visible default).
extern "C" void rush2_track49_pvs(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::draw_distance_pvs(rdram, (uint32_t)ctx->r29)) {
        return;
    }
    if (rush2::track1::pvs(rdram, (uint32_t)ctx->r29)) {
        return;
    }
    if (!hosting(rdram)) {
        return;
    }
    int32_t sp = (int32_t)ctx->r29;
    int32_t region = (int32_t)MEM_W(0, sp + 0x78);
    if (region >= 0 && region < track.pvs_count) {
        MEM_W(0, sp + 0x5C) = pvs_table + region * 16;
    }
}

// func_800A45A8 at 0x800A4644: $v0 = track id; 0 builds Las Vegas's sky model.
extern "C" void rush2_track49_sky(uint8_t* rdram, recomp_context* ctx) {
    if (hosting(rdram)) {
        ctx->r2 = 0;
    }
}


// func_800A45A8 at 0x800A4600: $t8 = the player count; the sky is only built for one player. Rush 2049 draws its
// sky in split screen too. A track without a sky model builds none, as in split screen.
extern "C" void rush2_track49_sky_players(uint8_t* rdram, recomp_context* ctx) {
    if (hosting(rdram)) {
        ctx->r24 = track_has_sky ? 1 : 2;
    }
}

// func_80093048 at 0x80093290, after it found the path's 4 AI lanes (pointers 0x800D57A0, counts 0x800D5780) and
// before func_800924E4 works out the race's time from them: the lanes' target speeds (u8 mph at point + 6) go through
// the Car Speeds lane map (src/car2049.cpp): Rush 2049's lanes are set for its faster cars. The drones' driver
// (func_80074990) aims for the lane speed x 1.05 x their rubber band (+0x808). The path file can stay
// loaded from one race to the next, so the lanes scaled last are remembered and not scaled again.
namespace {
    struct ScaledLane {
        uint32_t points = 0;
        std::vector<uint8_t> speeds;
    };
    ScaledLane scaled_lanes[4];
}

extern "C" void rush2_race_lane_speeds(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t lane_points = 0x800D57A0, lane_counts = 0x800D5780;
    bool rush2049_path = rush2::track2049::race_track() != 0 && MEM_B(0, (int32_t)track_id) == host_slot;
    for (int lane = 0; lane < 4; lane++) {
        uint32_t points = (uint32_t)MEM_W(0, (int32_t)(lane_points + lane * 4));
        uint32_t count = MEM_HU(0, (int32_t)(lane_counts + lane * 2));
        ScaledLane& last = scaled_lanes[lane];
        bool again = last.points == points && last.speeds.size() == count;
        for (uint32_t i = 0; again && i < count; i++) {
            again = MEM_BU(0, (int32_t)(points + i * 8 + 6)) == last.speeds[i];
        }
        if (again || points == 0 || count > 4096) {
            continue;
        }
        last.points = points;
        last.speeds.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t at = points + i * 8 + 6;
            int speed = MEM_BU(0, (int32_t)at);
            if (speed > 0) {
                speed = std::max(1, rush2::car2049::map_lane_speed(speed, rush2049_path));
                MEM_B(0, (int32_t)at) = (int8_t)speed;
            }
            last.speeds[i] = (uint8_t)speed;
        }
    }
}

// func_80093048 at 0x80093298, after func_800924E4 worked out the race's time: the start time (path header +0) and
// each checkpoint's lap-1 and later-lap extensions (+0x1E/+0x20) are the time to drive each stretch at the speeds of
// the path's AI lanes. Some of Rush 2049's lanes run faster than Rush 2's average even after the Car Speeds lane map
// (rush2_race_lane_speeds), so the times are scaled by how much faster this path's first lane is than Rush 2's lanes
// on average in the same Car Speeds mode (docs/rush2049_research/checkpoints.md).
extern "C" void rush2_track49_race_time(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t header = 0x8010BCE8;       // Copy of the path header, checkpoints at +0xC, 0x50 bytes each.
    constexpr uint32_t path_pointer = 0x800D575C; // The loaded path file.
    constexpr float rush2_lane_speed = 139.3f;    // Distance-weighted lane 0 speed of Rush 2's 9 race paths, both ways.
    // SF Rush tracks get SF Rush's own times (src/track1.cpp).
    if (rush2::track1::race_time(rdram)) {
        return;
    }
    if (rush2::track2049::race_track() == 0 || MEM_B(0, (int32_t)track_id) != host_slot) {
        return;
    }
    uint32_t file = (uint32_t)MEM_W(0, (int32_t)path_pointer);
    int count = (int16_t)MEM_H(0, (int32_t)(header + 8));
    if (file == 0 || count <= 0 || count > 10) {
        return;
    }
    if (rush2::track2049::race_track() == rush2::track2049::obstacle) {
        // Rush 2049's obstacle course gives no time at checkpoints (rush2_track49_obstacle_clock sets the clock).
        MEM_H(0, (int32_t)header) = (int16_t)rush2::track2049::obstacle_time;
        for (int i = 0; i < count; i++) {
            MEM_H(0, (int32_t)(header + 0xC + i * 0x50 + 0x1E)) = 0;
            MEM_H(0, (int32_t)(header + 0xC + i * 0x50 + 0x20)) = 0;
        }
        return;
    }
    auto u16 = [&](uint32_t o) { return (uint32_t)MEM_HU(0, (int32_t)(file + o)); };
    auto s16 = [&](uint32_t o) { return (int)(int16_t)MEM_H(0, (int32_t)(file + o)); };
    // Routes (race.md �3.3): header, branch records, total count, spine and branch points, then 4 lanes of
    // {u16 count, u8, u8, 4 bytes} + count x {s16 x, y, z, u8 speed, u8}.
    constexpr uint32_t route = 0x32C;
    uint32_t spine = u16(route), branches = MEM_BU(0, (int32_t)(file + route + 8));
    uint32_t o = route + 16, points = spine;
    for (uint32_t b = 0; b < branches; b++) {
        points += u16(o + b * 16 + 0xA);
    }
    o += 16 * branches + 2 + 6 * points;
    uint32_t lane = u16(o);
    o += 8;
    double length = 0.0, time = 0.0;
    for (uint32_t i = 0; i < lane && lane < 4096; i++) {
        uint32_t a = o + i * 8, b = o + ((i + 1) % lane) * 8;
        double dx = s16(b) - s16(a), dy = s16(b + 2) - s16(a + 2), dz = s16(b + 4) - s16(a + 4);
        double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        int speed = MEM_BU(0, (int32_t)(file + a + 6));
        if (speed > 0) {
            length += d;
            time += d / speed;
        }
    }
    if (time <= 0.0) {
        return;
    }
    float scale = float(length / time) / (rush2_lane_speed * rush2::car2049::rush2_lane_scale());
    if (scale <= 1.0f) {
        return;
    }
    auto scale_field = [&](uint32_t addr) {
        int16_t v = (int16_t)MEM_H(0, (int32_t)addr);
        MEM_H(0, (int32_t)addr) = (int16_t)std::lround(v * scale);
    };
    scale_field(header);
    for (int i = 0; i < count; i++) {
        scale_field(header + 0xC + i * 0x50 + 0x1E);
        scale_field(header + 0xC + i * 0x50 + 0x20);
    }
}

// End of func_80094698, which copied the race options into the race's settings: the obstacle course is raced alone
// over one lap, forwards and unmirrored, with checkpoints on (Rush 2049 has no such options for it).
extern "C" void rush2_track49_obstacle_settings(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t laps = 0x8010C0E2;          // s16
    constexpr uint32_t drones = 0x800D3E90;        // s16
    constexpr uint32_t checkpoints = 0x8010C17B;   // u8: the race is against the clock
    constexpr uint32_t backward = 0x80119848;
    constexpr uint32_t mirror = 0x800D0190;
    if (rush2::track2049::battle_race(rdram)) {
        // Battle arenas are stunt mode without computer cars: Rush 2049's battle was multiplayer only (computer
        // opponents are future work).
        MEM_H(0, (int32_t)drones) = 0;
        MEM_B(0, (int32_t)backward) = 0;
        MEM_B(0, (int32_t)mirror) = 0;
        return;
    }
    if (!rush2::track2049::obstacle_race(rdram)) {
        return;
    }
    MEM_H(0, (int32_t)laps) = 1;
    MEM_H(0, (int32_t)drones) = 0;
    MEM_B(0, (int32_t)checkpoints) = 1;
    MEM_B(0, (int32_t)backward) = 0;
    MEM_B(0, (int32_t)mirror) = 0;
}

// func_800AE670 at 0x800AEB20, at race start with checkpoints on: the time allowed (0x8010C204) was just set from the
// path's start time and the difficulty. The obstacle course gets Rush 2049's 5 minutes whatever the difficulty.
extern "C" void rush2_track49_obstacle_clock(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t time_allowed = 0x8010C204;  // f32
    if (!rush2::track2049::obstacle_race(rdram)) {
        return;
    }
    float t = rush2::track2049::obstacle_time;
    uint32_t bits;
    std::memcpy(&bits, &t, sizeof(bits));
    MEM_W(0, (int32_t)time_allowed) = (int32_t)bits;
}

// func_8008CDA4 at 0x8008D014, for car $s0 that hasn't fallen below y -190: about to run the stuck timer (+0x7F8, the
// time the car became slow; 0 = not slow), which resets the car once it has been slow for 6 s. Rush 2049 skips that
// timer for its stunt, obstacle and battle types (0x800CF830): clearing the start time each frame keeps it from
// running. Rush 2's stunt mode skips it already.
extern "C" void rush2_track49_stuck_timer(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::stuck_reset_off(rdram)) {
        MEM_W(0, (int32_t)((uint32_t)ctx->r16 + 0x7F8)) = 0;
    }
}

// func_8008E40C at 0x8008E678, a wrecked player waiting to be put back: $f16 = 3.5, the seconds since the wreck
// after which the car is put back. Rush 2049 waits 0.6 s for its obstacle and battle types (0x800E5BF8, constant
// 0x80124480) and 5 s otherwise.
extern "C" void rush2_track49_respawn_delay(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::quick_respawn(rdram)) {
        ctx->f16.fl = 0.6f;
    }
}

// Rush 2049's respawn zones: a branch of type 2 (branch record byte 0; race 2's paths have one each way, off the
// route) puts a wrecked car back on the branch point nearest to it, standing still, where other branches and the
// spine put it back within its checkpoint stretch and then some way on (Rush 2049 func_800D348C / func_800D3B28,
// docs/rush2049_research/checkpoints.md section 8). Rush 2 has branch types 0 and 1 only.
static bool respawn_zone(uint8_t* rdram, int32_t branch) {
    constexpr uint32_t route = 0x80111940;  // Route header: +8 u8 branch count, +0xC branch records (0x10 each).
    if (rush2::track2049::game_type(rdram) != rush2::track2049::GameType::race) {
        return false;
    }
    if (branch < 0 || branch >= (int32_t)MEM_BU(0, (int32_t)(route + 8))) {
        return false;
    }
    uint32_t records = (uint32_t)MEM_W(0, (int32_t)(route + 0xC));
    return MEM_BU(0, (int32_t)(records + (uint32_t)branch * 0x10)) == 2;
}

// func_80090570 at 0x80090758, the car's nearest spine or branch point found ($t7 = the branch, -1 for the spine;
// index at 0x58($sp)): Rush 2049 keeps a respawn zone's point as it is, skipping the checkpoint stretch.
extern "C" int rush2_track49_respawn_zone(uint8_t* rdram, recomp_context* ctx) {
    return respawn_zone(rdram, (int32_t)ctx->r15);
}

// func_80090A40 at 0x80091368, about to move a player's respawn point (branch 0x1B4($sp)) $t0 points on along the
// route: a respawn zone's stays where it is.
extern "C" void rush2_track49_respawn_zone_advance(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_BU(0, (int32_t)((uint32_t)ctx->r29 + 0x167)) != 0 &&
        respawn_zone(rdram, (int32_t)MEM_W(0, (int32_t)((uint32_t)ctx->r29 + 0x1B4)))) {
        ctx->r8 = 0;
    }
}

// func_80090A40 at 0x800918E8, car $s4 placed 1.5 ft over its respawn point (branch 0x1B0($sp); $a3 = a player's
// car) with its rolling start speed (+0x6C0 = 58): in a respawn zone Rush 2049 puts it 3 ft up with no speed, and
// clears what its on-the-spot reset clears (flag 0x10 of +0x7F4, +0x648).
extern "C" void rush2_track49_respawn_zone_place(uint8_t* rdram, recomp_context* ctx) {
    if (ctx->r7 == 0 || !respawn_zone(rdram, (int32_t)MEM_W(0, (int32_t)((uint32_t)ctx->r29 + 0x1B0)))) {
        return;
    }
    uint32_t car = (uint32_t)ctx->r20;
    uint32_t bits = (uint32_t)MEM_W(0, (int32_t)(car + 0x668));
    float y;
    std::memcpy(&y, &bits, sizeof(y));
    y += 1.5f;
    std::memcpy(&bits, &y, sizeof(bits));
    MEM_W(0, (int32_t)(car + 0x668)) = (int32_t)bits;
    MEM_W(0, (int32_t)(car + 0x6C0)) = 0;
    MEM_W(0, (int32_t)(car + 0x7F4)) = MEM_W(0, (int32_t)(car + 0x7F4)) & ~0x10;
    MEM_B(0, (int32_t)(car + 0x648)) = 0;
}

// The HUD's track map (func_800B8188 at 0x800B81D8, $s4), its car dots (func_800B8900 at 0x800B8950, $a1), its
// finish flag (func_800B9D48 at 0x800B9D70, $t0) and the radar (func_800B920C at 0x800B9260, $t0; its dots
// func_800B8CC8 at 0x800B8D24, $t2) hide themselves on the stunt track (11), whose id is about to be tested. Courses
// Rush 2049 shows neither on count as it.
extern "C" void rush2_track49_map_s4(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::no_map(rdram)) ctx->r20 = stunt_host_slot;
}

extern "C" void rush2_track49_map_a1(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::no_map(rdram)) ctx->r5 = stunt_host_slot;
}

extern "C" void rush2_track49_map_t0(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::no_map(rdram)) ctx->r8 = stunt_host_slot;
}

extern "C" void rush2_track49_map_t2(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::no_map(rdram)) ctx->r10 = stunt_host_slot;
}
