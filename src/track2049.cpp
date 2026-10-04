// Rush 2049 race tracks, raced in a borrowed Rush 2 track slot (the host slot).
//
// The track select offers the 2049 tracks as extra entries (src/track2049_menu.cpp). When one is raced, the race
// setup hook turns its menu id into the host slot's id. The track is then converted from the user's 2049 ROM
// (src/track2049_convert.cpp) and replaces the host slot's five files (geometry 0x33+slot, placement 0x3F+slot,
// collision 0x4B+slot, forward and backward AI paths 0x57+slot / 0x63+slot), the slot's entries in the per-track
// tables the race code reads, and the slot's in-race logo. The original values are put back before a Rush 2 track is
// raced, so the host track itself is unchanged.
//
// Code support for things the files can't express:
// - Visibility: func_8007C27C picks a 16-byte section mask per camera region from a per-track table that is sized
//   for the slot's own track, so the converted table lives in recomp memory and the hook at 0x8007C480 points the
//   game at it.
// - Sky: Rush 2049 draws its tracks' sky as a model, which Rush 2 only does for Las Vegas (func_800A45A8); the
//   converted geometry names it SKYO1, and the hook at 0x800A4644 sends the host slot down Las Vegas's branch.
//   Rush 2 also builds no sky in split screen, while Rush 2049 does (hook at 0x800A4600).

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdlib>


#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "rush2_hooks.h"
#include "assets.h"
#include "track1.h"
#include "track2049.h"
#include "track2049_convert.h"
#include "rush2049_rom.h"
#include "wings.h"

using rush2::track2049::host_slot;

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
    const std::string prefix_of_host = "HAWAII";    // The host slot's track prefix (0x800C182C).

    std::mutex track_mutex;
    std::atomic_bool option_enabled = true;
    std::atomic_int raced_track = 0;  // 1-6, or 0 for none.
    int loaded_track = 0;              // The 2049 track in `track`.
    rush2::track2049::ConvertedTrack track;
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

    // Converts 2049 track k from the user's ROM.
    bool convert(uint8_t* rdram, int k) {
        auto rom = rush2::wings::get_rom();
        if (rom == nullptr) {
            return false;
        }
        if (shared_models.empty()) {
            auto rush2_rom = recomp::get_rom();
            std::vector<uint8_t> copy(rush2_rom.begin(), rush2_rom.end());
            rush2::track2049::rush2_shared_model_names(copy, (uint32_t)MEM_W(0, (int32_t)(asset_offsets + 0x12 * 4)),
                (uint32_t)MEM_W(0, (int32_t)(asset_offsets + 0x14 * 4)), shared_models);
        }
        std::string error;
        if (!rush2::track2049::convert_track(*rom, k, prefix_of_host, shared_models, true, track, error)) {
            printf("[2049] Couldn't convert track %d: %s\n", k, error.c_str());
            return false;
        }
        std::vector<uint8_t> geometry_2049, collision_2049;
        if (!rush2::rom2049::read_file(*rom, 100 + k, geometry_2049) ||
            !rush2::rom2049::read_file(*rom, 138 + k, collision_2049)) {
            return false;
        }
        std::set<std::string> names = rush2::track2049::model_names(track.geometry);
        rush2::track2049::set_mover_data(geometry_2049, collision_2049, track.path_records, track.spin_records,
                                         std::vector<std::string>(names.begin(), names.end()));
        rush2::track2049::set_texanim_data(track.tex_anims);
        return true;
    }

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

    void apply(uint8_t* rdram) {
        if (!applied) {
            save_tables(rdram);
        }
        if (!race_logo.empty()) {
            rush2::assets::replace(rdram, 4 + host_slot, race_logo);
        }
        rush2::assets::replace(rdram, 0x33 + host_slot, track.geometry);
        rush2::assets::replace(rdram, 0x3F + host_slot, track.placement);
        rush2::assets::replace(rdram, 0x4B + host_slot, track.collision);
        rush2::assets::replace(rdram, 0x57 + host_slot, track.path);
        rush2::assets::replace(rdram, 0x63 + host_slot, track.path_backward);
        for (int i = 0; i < 3; i++) {
            MEM_B(0, (int32_t)(fog_colours + host_slot * 3 + i)) = track.fog[i];
        }
        MEM_W(0, (int32_t)(cloud_scroll + host_slot * 4)) = 0;
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
        applied = true;
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
        applied = false;
    }

    bool hosting(uint8_t* rdram) {
        return applied && MEM_B(0, (int32_t)track_id) == host_slot;
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

// Start of func_800A4C98, which queues the race's track files.
extern "C" void rush2_track49_load(uint8_t* rdram, recomp_context* ctx) {
    // SF Rush tracks race in the same slot (src/track1.cpp); it puts the slot's own values back first.
    if (rush2::track1::load(rdram)) {
        return;
    }
    std::lock_guard lock{ track_mutex };
    int k = raced_track;
    if (k == 0 || MEM_B(0, (int32_t)track_id) != host_slot) {
        restore(rdram);
        return;
    }
    if (loaded_track != k) {
        loaded_track = 0;
        if (!convert(rdram, k)) {
            printf("[2049] Track %d isn't available; racing the host track\n", k);
            raced_track = 0; // Everything (records included) then treats the race as the host track's.
            restore(rdram);
            return;
        }
        race_logo.clear();
        std::vector<uint8_t> logo;
        auto rom = rush2::wings::get_rom();
        if (rom == nullptr || !rush2::assets::read_original(rdram, 4 + host_slot, logo) ||
            !rush2::track2049::build_race_logo(logo, *rom, k, race_logo)) {
            race_logo.clear();
        }
        loaded_track = k;
    }
    apply(rdram);
    rush2::track2049::reset_movers();
    rush2::track2049::texanim_reset();
}

// func_8007C27C at 0x8007C480: 0x5C($sp) = the section mask chosen for the camera's region 0x78($sp) (-1 = none,
// which uses the all-visible default).
extern "C" void rush2_track49_pvs(uint8_t* rdram, recomp_context* ctx) {
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
// sky in split screen too.
extern "C" void rush2_track49_sky_players(uint8_t* rdram, recomp_context* ctx) {
    if (hosting(rdram)) {
        ctx->r24 = 1;
    }
}

// func_80093048 at 0x80093298, after func_800924E4 worked out the race's time: the start time (path header +0) and
// each checkpoint's lap-1 and later-lap extensions (+0x1E/+0x20) are the time to drive each stretch at the speeds of
// the path's AI lanes. Rush 2049's lanes are set for its faster cars (it has no countdown), up to 1.3 times Rush 2's,
// so the times are scaled by how much faster this path's first lane is than Rush 2's lanes on average
// (docs/rush2049_research/checkpoints.md).
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
    auto u16 = [&](uint32_t o) { return (uint32_t)MEM_HU(0, (int32_t)(file + o)); };
    auto s16 = [&](uint32_t o) { return (int)(int16_t)MEM_H(0, (int32_t)(file + o)); };
    // Routes (race.md §3.3): header, branch records, total count, spine and branch points, then 4 lanes of
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
    float scale = float(length / time) / rush2_lane_speed;
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
