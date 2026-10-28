// Rush 2049 race tracks, raced in a borrowed Rush 2 track slot (the host slot).
//
// The track select offers the 2049 tracks as extra entries (src/rush2049/track2049_menu.cpp). When one is raced, the race
// setup hook turns its menu id into the host slot's id. The track is then converted from the user's 2049 ROM
// (src/rush2049/track2049_convert.cpp) and replaces the host slot's five files (geometry 0x33+slot, placement 0x3F+slot,
// collision 0x4B+slot, forward and backward AI paths 0x57+slot / 0x63+slot), the slot's entries in the per-track
// tables the race code reads, and the slot's in-race logo. The original values are put back before a Rush 2 track is
// raced, so the host track itself is unchanged.
//
// Rush 2049's stunt arenas are hosted the same way by Rush 2's stunt track (slot 11, STUNT1), whose literal track
// tests give the race Rush 2's stunt mode: stunt scoring, start on the path's spine, no backward or mirror, no
// drones. An arena has one AI path, used both ways, and no visibility table (Rush 2049 draws every section of it);
// it keeps Rush 2's stunt song and STUNT1's records.
//
// Rush 2049's battle arenas (DM1-DM8) are hosted in the stunt slot the same way: a free-roaming arena with no
// checkpoints, whose drones are the computer opponents (src/rush2049/battle_ai.cpp). Their game type is battle
// (game_type), so the rules ported by type apply: no stuck reset, 2.25 s wreck wait, 0.6 s re-arm, no map or radar. The battle
// itself is src/rush2049/battle.cpp.
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
//   Rush 2 also builds no sky in split screen, while Rush 2049 and SF Rush do, so the hook at 0x800A4600 builds it
//   for any player count, on every track. The obstacle course has no sky model (Rush 2049 shows its black fog color
//   around it), so it builds none.

#include <cstdio>
#include <algorithm>
#include <optional>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <cstring>


#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "battle_ai.h"
#include "librecomp/game.hpp"
#include "rush2_hooks.h"
#include "battle.h"
#include "rush2.h"
#include "assets.h"
#include "track1.h"
#include "calspeed.h"
#include "track2049.h"
#include "track2049_convert.h"
#include "track_cache.h"
#include "car2049.h"
#include "rush2049_rom.h"
#include "track_mod_runtime.h"
#include "sim_run.h"
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
    std::shared_ptr<const rush2::rom2049::Source> loaded_source; // The Rush 2049 source it was converted from.
    int slot = host_slot;              // The slot `track` is applied to.
    std::atomic_int applied_slot = -1; // `slot` while applied, or -1.
    std::atomic_bool applied_road_hold = false; // A custom track whose track.toml asks for road_hold is applied.
    std::atomic_bool applied_random_song = false; // A custom track without [audio] music is applied.
    std::atomic_bool applied_smooth = false;    // A custom track racing with smooth collision is applied.
    std::atomic_bool applied_p2p = false;       // A point-to-point custom track is applied.
    std::atomic<float> applied_smooth_fold = 20.0f; // Its fold angle (degrees).
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

    // Custom tracks from mods (src/track_mod/) are hosted here too, as k = mod_k + their index, and California Speed's
    // tracks (src/calspeed/) as k = rush2::calspeed::calspeed_k + their index.
    constexpr int mod_k = 1000;
    constexpr int cs_k = rush2::calspeed::calspeed_k;
    static_assert(cs_k > mod_k + rush2::track2049::mod_capacity);
    bool is_mod_k(int k) {
        return k >= mod_k && k < cs_k;
    }
    bool is_cs_k(int k) {
        return k >= cs_k && k < cs_k + rush2::calspeed::track_count;
    }
    rush2::calspeed::Converted cs_track;       // The raced Cal Speed track's extras (its files are moved to `track`).
    const std::vector<uint8_t>* cs_rom = nullptr;  // The Cal Speed ROM it was converted from.
    bool cs_scenery = false;                        // The Extended Scenery setting it was converted with.
    bool cs_billboards = false;                     // The Billboard Trees setting it was converted with.
    rush2::track_mod::Settings mod_settings;   // The raced custom track's track.toml.
    std::array<std::optional<float>, 16> mod_drag;  // Its [surfaces] drag by polygon material slot.
    std::array<uint8_t, 16> mod_like{};             // The stock material each slot passes for: road 0, dirt 1, grass 2.
    // The custom material slot under each wheel, by car + 2 x wheel (the wheel's +0x60C holds the `like` material
    // instead, see rush2_trackmod_wheel_material).
    std::unordered_map<uint32_t, uint8_t> wheel_custom;

    // Rush 2's shared model names (assets 0x12 and 0x14), which the converted tracks leave out: read once from the
    // ROM, on any thread.
    const std::set<std::string>& shared_model_names() {
        static std::mutex shared_mutex;
        std::lock_guard lock{ shared_mutex };
        if (shared_models.empty()) {
            auto rush2_rom = recomp::get_rom();
            std::vector<uint8_t> copy(rush2_rom.begin(), rush2_rom.end());
            rush2::track2049::rush2_shared_model_names(copy, rush2::assets::rom_offset(0x12), rush2::assets::rom_offset(0x14),
                                                       shared_models);
        }
        return shared_models;
    }

    // The source key of a 2049 track's cache entry: the N64 ROM's hash, or for a Dreamcast disc the track's own
    // geometry and collision files.
    uint64_t source_key(const rush2::rom2049::Source& rom, const std::vector<uint8_t>& geometry,
                        const std::vector<uint8_t>& collision) {
        static std::mutex hash_mutex;
        static const rush2::rom2049::Source* hashed_rom = nullptr;
        static uint64_t rom_hash = 0;
        if (const std::vector<uint8_t>* n64 = rom.n64_rom()) {
            std::lock_guard lock{ hash_mutex };
            if (hashed_rom != &rom) {
                rom_hash = rush2::track_cache::hash(n64->data(), n64->size());
                hashed_rom = &rom;
            }
            return rom_hash;
        }
        uint64_t source = rush2::track_cache::hash(geometry.data(), geometry.size(), 0xDC);
        return rush2::track_cache::hash(collision.data(), collision.size(), source);
    }

    // 2049 track k (convert_track's k) for the slot whose prefix is `prefix`, with its geometry and collision files
    // (the movers read them): from the disk cache (src/track_cache.cpp) when this build converted it from the same
    // source before, else converted and stored. Any thread: the background builds (rush2::track2049::prepare) do every
    // track ahead of the races, and a race waits on the entry's claim while one converts it.
    bool cached_or_converted(const rush2::rom2049::Source& rom, int k, const std::string& prefix,
                             rush2::track2049::ConvertedTrack& out, std::vector<uint8_t>& geometry,
                             std::vector<uint8_t>& collision, std::string& error) {
        if (!rom.read_file(100 + k, geometry) || !rom.read_file(138 + k, collision)) {
            error = "can't read its files";
            return false;
        }
        uint64_t source = source_key(rom, geometry, collision);
        auto claim = rush2::track_cache::claim(rush2::track_cache::entry_2049(k));
        if (rush2::track_cache::load_2049(k, prefix, source, out)) return true;
        if (!rush2::track2049::convert_track(rom, k, prefix, shared_model_names(), true, out, error)) return false;
        rush2::track_cache::save_2049(k, prefix, source, out);
        return true;
    }

    // Converts 2049 track k (convert_track's k) from the user's ROM for the slot in `slot`.
    bool convert(uint8_t* rdram, int k) {
        if (is_cs_k(k)) {
            auto rom = rush2::calspeed::get_rom();
            if (rom == nullptr) {
                return false;
            }
            std::string error;
            cs_scenery = rush2::calspeed::scenery_enabled();
            cs_billboards = rush2::calspeed::billboards_enabled();
            if (!rush2::calspeed::load_track(*rom, k - cs_k, prefix_of(slot), shared_model_names(), cs_scenery, cs_billboards,
                                             cs_track, error)) {
                printf("[CalSpeed] Couldn't convert track %d: %s\n", k - cs_k, error.c_str());
                return false;
            }
            cs_rom = rom.get();
            track = std::move(cs_track.track);
            rush2::calspeed::set_race_path(track.path, track.collision);
            track_has_sky = rush2::track2049::model_names(track.geometry).contains("SKYO1");
            rush2::track2049::set_mover_data(0, {}, {}, {}, {}, {});
            rush2::track2049::set_texanim_data(track.tex_anims);
            rush2::track2049::set_prop_data({}, track.geometry);
            rush2::battle::set_data({}, {}, track.geometry, {});
            return true;
        }
        if (is_mod_k(k)) {
            rush2::track_mod::Converted mod;
            if (!rush2::track_mod::load(k - mod_k, prefix_of(slot), mod)) {
                return false;
            }
            track = std::move(mod.track);
            mod_settings = mod.settings;
            mod_drag = {};
            mod_like = {};
            wheel_custom.clear();
            for (const auto& [name, sf] : mod_settings.surfaces) {
                mod_drag[size_t(sf.material & 15)] = sf.drag;
                mod_like[size_t(sf.material & 15)] = sf.like == "dirt" ? 1 : sf.like == "grass" ? 2 : 0;
            }
            {
                std::set<std::string> names = rush2::track2049::model_names(track.geometry);
                track_has_sky = names.contains("SKYO1");   // a SKY mesh
                // Moving objects (MOVE_*), in the 2049 layouts the movers code reads; k = 0 marks a custom track.
                rush2::track2049::set_mover_data(0, track.mover_paths, track.mover_collision, track.path_records, {},
                                                 std::vector<std::string>(names.begin(), names.end()));
            }
            rush2::track2049::set_texanim_data({});
            rush2::track2049::set_prop_data(track.prop_records, track.geometry);   // PROP_KNOCK_* / PROP_TIP_*
            // A battle arena's PICKUP_ markers (drawn with Rush 2049's models, src/rush2049/battle.cpp) and every custom
            // track's solid triangles, which the weapons' shots stop on.
            rush2::battle::set_data(track.pickup_records, {}, track.geometry, track.solid_triangles);
            if (rush2::track_mod::is_arena(mod_settings) && mod_settings.type == "battle" && track.pickup_records.empty()) {
                rush2::battle_ai::set_arena(track.solid_triangles);   // set_data builds the opponents' grid only with pickups
            }
            return true;
        }
        auto rom = rush2::wings::get_rom();
        if (rom == nullptr) {
            return false;
        }
        std::vector<uint8_t> geometry_2049, collision_2049;
        std::string error;
        if (!cached_or_converted(*rom, k, prefix_of(slot), track, geometry_2049, collision_2049, error)) {
            printf("[2049] Couldn't convert track %d: %s\n", k, error.c_str());
            return false;
        }
        std::set<std::string> names = rush2::track2049::model_names(track.geometry);
        track_has_sky = names.contains("SKYO1");
        rush2::track2049::set_mover_data(k, geometry_2049, collision_2049, track.path_records, track.spin_records,
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

    // Whether the custom track's [audio] music names a song: "track1".."track8".
    bool mod_song_set() {
        const std::string& m = mod_settings.music;
        return m.size() == 6 && m.compare(0, 5, "track") == 0 && m[5] >= '1' && m[5] <= '8';
    }

    void apply(uint8_t* rdram) {
        if (!applied) {
            save_tables(rdram);
        }
        if (!race_logo.empty()) {
            rush2::assets::replace(rdram, 4 + slot, race_logo);
        }
        rush2::assets::replace(rdram, 0x33 + slot, track.geometry, loaded_source != nullptr && loaded_source->is_dreamcast()
                                   ? rush2::origin::Game::Rush2049DC : rush2::origin::Game::Rush2049);
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
        else if (is_cs_k(loaded_track)) {
            // src/music.cpp plays California Speed's songs on its tracks; Rush 2's eight in turn are the slot's song
            // for when none of them can play.
            MEM_H(0, (int32_t)(songs + slot * 2)) = int16_t((loaded_track - cs_k) % 8);
            MEM_W(0, (int32_t)(cloud_scroll + slot * 4)) = (int32_t)cs_track.cloud_scroll;
            MEM_W(0, (int32_t)(sky_parallax + slot * 4)) = (int32_t)cs_track.sky_parallax;
        }
        else if (is_mod_k(loaded_track)) {
            // music = "track1".."track8": Rush 2's songs by their track select order. Without one src/music.cpp plays
            // a random song of every one the player switched on (mod_random_song); the slot gets a random one of
            // Rush 2's for when it leaves the song to the game, never the host's.
            int16_t song;
            if (mod_song_set()) {
                song = int16_t(mod_settings.music[5] - '1');
            }
            else {
                static std::mt19937 rng{ std::random_device{}() };
                song = int16_t(std::uniform_int_distribution<int>(0, 7)(rng));
            }
            MEM_H(0, (int32_t)(songs + slot * 2)) = song;
            uint32_t bits;
            memcpy(&bits, &mod_settings.cloud_scroll, 4);
            MEM_W(0, (int32_t)(cloud_scroll + slot * 4)) = bits;
            memcpy(&bits, &mod_settings.sky_parallax, 4);
            MEM_W(0, (int32_t)(sky_parallax + slot * 4)) = bits;
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
        // Cal Speed's hit objects as Rush 2 breakables drawn with its own models (src/calspeed/calspeed_convert.cpp).
        if (is_cs_k(loaded_track)) {
            rush2::track1::set_redirects(rdram, cs_track.record_models, cs_track.piece_models);
        }
        else {
            rush2::track1::clear_redirects();   // another track replaced a Cal Speed one in the slot
        }
        applied = true;
        applied_slot = slot;
        applied_road_hold = is_mod_k(loaded_track) && mod_settings.road_hold;
        applied_random_song = is_mod_k(loaded_track) && !mod_song_set();
        applied_smooth = is_mod_k(loaded_track) && mod_settings.smooth_collision;
        applied_p2p = is_mod_k(loaded_track) && mod_settings.point_to_point && !rush2::track_mod::is_arena(mod_settings);
        applied_smooth_fold = mod_settings.smooth_fold;
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
        if (is_cs_k(loaded_track)) {
            rush2::track1::clear_redirects();
        }
        applied = false;
        applied_slot = -1;
        applied_road_hold = false;
        applied_random_song = false;
        applied_smooth = false;
        applied_p2p = false;
    }

    bool hosting(uint8_t* rdram) {
        return applied && MEM_B(0, (int32_t)track_id) == slot;
    }

    bool hosting_mod(uint8_t* rdram) {
        return hosting(rdram) && is_mod_k(loaded_track);
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

bool rush2::track2049::mod_smooth_collision(uint8_t* rdram, float* fold) {
    int s = applied_slot;
    if (!applied_smooth || s < 0 || MEM_B(0, (int32_t)track_id) != s) return false;
    if (fold) *fold = applied_smooth_fold;
    return true;
}

bool rush2::track2049::mod_random_song(uint8_t* rdram) {
    int s = applied_slot;
    return applied_random_song && s >= 0 && MEM_B(0, (int32_t)track_id) == s;
}

bool rush2::track2049::mod_point_to_point(uint8_t* rdram) {
    int s = applied_slot;
    return applied_p2p && s >= 0 && MEM_B(0, (int32_t)track_id) == s;
}

bool rush2::track2049::mod_road_hold(uint8_t* rdram) {
    int s = applied_slot;
    return applied_road_hold && s >= 0 && MEM_B(0, (int32_t)track_id) == s;
}

bool rush2::track2049::obstacle_race(uint8_t* rdram) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    // The track select's id before the race setup turns it into the host slot's.
    return t == obstacle_menu_id || (t == host_slot && raced_track == obstacle);
}

// A custom track raced as an arena (track.toml type), in the stunt slot: rush2::track_mod::race_type, or race_race.
static int mod_arena(int t) {
    return t == stunt_host_slot && rush2::track_mod::race_track() >= 0 ? rush2::track_mod::race_type()
                                                                         : rush2::track_mod::race_race;
}

bool rush2::track2049::battle_race(uint8_t* rdram) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    return (t >= battle_menu_id && t < battle_menu_id + battle_count) || (t == stunt_host_slot && raced_battle != 0) ||
           mod_arena(t) == rush2::track_mod::race_battle ||
           (t >= mod_menu_id && t < mod_menu_id + mod_capacity && rush2::track_mod::type_of(t - mod_menu_id) == rush2::track_mod::race_battle);
}

bool rush2::track2049::battle_arena_race(uint8_t* rdram) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    return t == stunt_host_slot && (raced_battle != 0 || mod_arena(t) == rush2::track_mod::race_battle);
}

rush2::track2049::GameType rush2::track2049::game_type(uint8_t* rdram) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    if (t == host_slot && raced_track == obstacle) return GameType::obstacle;
    if (t == host_slot && raced_track != 0) return GameType::race;
    if (t == stunt_host_slot && raced_stunt != 0) return GameType::stunt;
    if (t == stunt_host_slot && raced_battle != 0) return GameType::battle;
    if (int m = mod_arena(t); m != rush2::track_mod::race_race) return m == rush2::track_mod::race_stunt ? GameType::stunt : GameType::battle;
    return GameType::none;
}

bool rush2::track2049::stuck_reset_off(uint8_t* rdram) {
    GameType g = game_type(rdram);
    return g == GameType::stunt || g == GameType::obstacle || g == GameType::battle;
}

float rush2::track2049::wreck_wait(uint8_t* rdram) {
    return game_type(rdram) == GameType::battle ? 2.25f : 3.5f;
}

bool rush2::track2049::no_map(uint8_t* rdram) {
    GameType g = game_type(rdram);
    return g == GameType::obstacle || g == GameType::battle;
}

// Start of func_800A4C98, which queues the race's track files.
extern "C" void rush2_track49_load(uint8_t* rdram, recomp_context* ctx) {
    // SF Rush tracks race in the same slot (src/rush1/track1.cpp); it puts the slot's own values back first.
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
    else if (rush2::track_mod::race_track() >= 0 && t == host_slot && rush2::track_mod::race_type() == rush2::track_mod::race_race) {
        k = mod_k + rush2::track_mod::race_track();
        want_slot = host_slot;
    }
    else if (mod_arena(t) != rush2::track_mod::race_race) {
        // A custom stunt or battle arena, in the stunt slot as Rush 2049's arenas.
        k = mod_k + rush2::track_mod::race_track();
        want_slot = stunt_host_slot;
    }
    else if (rush2::calspeed::race_track() > 0 && t == host_slot) {
        k = cs_k + rush2::calspeed::race_track() - 1;
        want_slot = host_slot;
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
    // The Games tab can switch the Rush 2049 source between races: a track converted from the other one is redone.
    if (k < mod_k && loaded_source != rush2::wings::get_rom()) {
        loaded_track = 0;
    }
    // A Cal Speed ROM selected again in the Games tab is another copy: its tracks are converted (or read) again.
    if (is_cs_k(k) && cs_rom != rush2::calspeed::get_rom().get()) {
        loaded_track = 0;
    }
    // So is one whose California Speed Extended Scenery or Billboard Trees setting changed.
    if (is_cs_k(k) && (cs_scenery != rush2::calspeed::scenery_enabled() ||
                       cs_billboards != rush2::calspeed::billboards_enabled())) {
        loaded_track = 0;
    }
    if (loaded_track != k) {
        loaded_track = 0;
        loaded_source = rush2::wings::get_rom();
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
        if (is_cs_k(k)) {
            auto cs = rush2::calspeed::get_rom();
            if (cs == nullptr || !rush2::assets::read_original(rdram, 4 + slot, logo) ||
                !rush2::calspeed::build_race_logo(logo, *cs, k - cs_k, race_logo)) {
                race_logo.clear();
            }
        }
        else if (k >= mod_k) {
            // The custom track's [menu] logo, so its race and post-race screens never show the host's.
            if (!rush2::track2049::mod_race_logo(rdram, k - mod_k, 4 + slot, race_logo)) race_logo.clear();
        }
        else if (rom == nullptr || !rush2::assets::read_original(rdram, 4 + slot, logo) ||
            !rush2::track2049::build_race_logo(logo, *rom, k, race_logo)) {
            race_logo.clear();
        }
        loaded_track = k;
    }
    apply(rdram);
    rush2::track2049::reset_race_objects();
}

// Every Rush 2049 track and arena with the slot it races in: the races and the obstacle course in HAWAII's, the stunt
// and battle arenas in STUNT1's.
static std::vector<std::pair<int, int>> tracks_and_slots() {
    std::vector<std::pair<int, int>> out;
    for (int k = 1; k <= rush2::track2049::track_count; k++) out.push_back({ k, host_slot });
    out.push_back({ rush2::track2049::obstacle, host_slot });
    for (int n = 0; n < rush2::track2049::stunt_count; n++) out.push_back({ rush2::track2049::stunt_first + n, stunt_host_slot });
    for (int n = 0; n < rush2::track2049::battle_count; n++) out.push_back({ rush2::track2049::battle_first + n, stunt_host_slot });
    return out;
}

void rush2::track2049::prepare() {
    if (auto rom = rush2::wings::get_rom(); rom != nullptr && available()) {
        for (auto [k, s] : tracks_and_slots()) {
            // An N64 ROM's entries are keyed on the ROM, so what is on disk is known without reading the track's files.
            if (rom->n64_rom() != nullptr && rush2::track_cache::has_2049(k, prefix_of(s), source_key(*rom, {}, {}))) continue;
            rush2::track2049::ConvertedTrack converted;
            std::vector<uint8_t> geometry, collision;
            std::string error;
            if (!cached_or_converted(*rom, k, prefix_of(s), converted, geometry, collision, error)) {
                printf("[2049] Couldn't convert track %d: %s\n", k, error.c_str());
            }
        }
    }
    if (auto cs = rush2::calspeed::get_rom(); cs != nullptr && rush2::calspeed::available()) {
        for (int t = 0; t < rush2::calspeed::track_count; t++) {
            rush2::calspeed::prepare_track(*cs, t, prefix_of(host_slot), shared_model_names());
        }
    }
}

void rush2::track2049::preload(int t) {
    if (t >= first_menu_id && t < first_menu_id + track_count) {
        rush2::track_cache::preload_2049(t - first_menu_id + 1);
    }
    else if (t == obstacle_menu_id) {
        rush2::track_cache::preload_2049(obstacle);
    }
    else if (t >= stunt_menu_id && t < stunt_menu_id + stunt_count) {
        rush2::track_cache::preload_2049(stunt_first + t - stunt_menu_id);
    }
    else if (t >= battle_menu_id && t < battle_menu_id + battle_count) {
        rush2::track_cache::preload_2049(battle_first + t - battle_menu_id);
    }
    else if (t >= rush2::calspeed::first_menu_id && t < rush2::calspeed::first_menu_id + rush2::calspeed::track_count) {
        rush2::calspeed::preload_track(t - rush2::calspeed::first_menu_id);
    }
}

void rush2::track2049::reset_race_objects() {
    reset_movers();
    reset_props();
    rush2::battle::reset();
    texanim_reset();
}

// func_8007C27C at 0x8007C480: 0x5C($sp) = the section mask chosen for the camera's region 0x78($sp) (-1 = none,
// which uses the all-visible default).
extern "C" void rush2_track49_pvs(uint8_t* rdram, recomp_context* ctx) {
    // Rush 2's own region mask tables per track slot (jump table 0x800CF9A4).
    static constexpr uint32_t rush2_tables[12] = { 0x800C6538, 0x800C6A78, 0x800C7048, 0x800C7788, 0x800C7F38,
        0x800C8588, 0x800C8B28, 0x800C9268, 0x800C9508, 0x800C9818, 0x800C9BA8, 0x800C9F18 };
    uint32_t sp = (uint32_t)ctx->r29;
    uint32_t table = 0;
    if (!rush2::track1::pvs(rdram, sp, table)) {
        if (hosting(rdram)) {
            int32_t region = (int32_t)MEM_W(0, (int32_t)(sp + 0x78));
            if (region >= 0 && region < track.pvs_count) {
                MEM_W(0, (int32_t)(sp + 0x5C)) = pvs_table + region * 16;
            }
            table = pvs_table;
        }
        else {
            int slot = MEM_B(0, (int32_t)track_id);
            table = slot >= 0 && slot < 12 ? rush2_tables[slot] : 0;
        }
    }
    rush2::draw_distance_pvs(rdram, sp, table);
}

// A custom track with sky = "clouds", or a Cal Speed track (whose game draws Rush 2's dome), and no sky model of its
// own: Rush 2's cloud dome (the branch every track but Las Vegas, Atari and the stunt arena takes), drawn as Rush 2
// draws it.
static bool mod_cloud_dome(uint8_t* rdram) {
    return !track_has_sky && ((hosting_mod(rdram) && mod_settings.sky == "clouds") ||
                              (hosting(rdram) && is_cs_k(loaded_track)));
}

// func_800A45A8 at 0x800A4644: $v0 = track id; 0 builds Las Vegas's sky model, 11 the stunt arena's, others the
// cloud dome (the host slot's id, HAWAII, takes that branch).
extern "C" void rush2_track49_sky(uint8_t* rdram, recomp_context* ctx) {
    if (hosting(rdram) && !mod_cloud_dome(rdram)) {
        ctx->r2 = 0;
    }
}


// func_800A45A8 at 0x800A4600: $t8 = the player count; Rush 2 only builds the sky for one player, to save drawing
// time. SF Rush (sky_setup_800A7494 in Rush 1) and Rush 2049 draw it in split screen too, so it is built for any
// count; the per-view sky arrays hold four views (tools/players4/relocate.py). A Rush 2049 track without a sky model
// builds none.
extern "C" void rush2_track49_sky_players(uint8_t* rdram, recomp_context* ctx) {
    ctx->r24 = hosting(rdram) && !mod_cloud_dome(rdram) && !track_has_sky ? 2 : 1;
}

// render_sky_cloud_scroll_8008918C at 0x800891F8: $t7 = the view's sky matrix, $v1 = its camera. A sky model only
// follows the camera's x and z (0x800891EC-0x80089200) and keeps its world height, which suits the stock skies. A
// custom track's SKY mesh past 2,000 ft is shrunk by sky_scale about its origin to fit the model's s16 vertices
// (docs/custom_tracks.md#sky): at world height a big dome (Alcatraz's arcade sky, 27,600 ft, drawn 2,048) ended below
// the track's top. Shrunk about the camera instead, it looks as it would at full size: its height is (1 - sky_scale) x
// the camera's, which also keeps distant land and water in the sky at their true angles below the horizon. A stock sky
// model named in track.toml is used only when there is no SKY mesh.
extern "C" void rush2_track49_sky_height(uint8_t* rdram, recomp_context* ctx) {
    if (!hosting_mod(rdram) || !track_has_sky) return;
    const rush2::track_mod::StockSky* k = rush2::track_mod::stock_sky(mod_settings.sky);
    if (k && k->model[0]) return;
    uint32_t bits = (uint32_t)MEM_W(0x28, (int32_t)ctx->r3);
    float y;
    memcpy(&y, &bits, 4);
    y *= 1.0f - track.sky_scale;
    memcpy(&bits, &y, 4);
    MEM_W(0x28, (int32_t)ctx->r15) = (int32_t)bits;
}

// func_80093048 at 0x80093290, after it found the path's 4 AI lanes (pointers 0x800D57A0, counts 0x800D5780) and
// before func_800924E4 works out the race's time from them: the lanes' target speeds (u8 mph at point + 6) go through
// the Car Speeds lane map (src/rush2049/car2049.cpp): Rush 2049's lanes are set for its faster cars. The drones' driver
// (func_80074990) aims for the lane speed x 1.05 x their rubber band (+0x808). The path file can stay
// loaded from one race to the next, so the lanes scaled last are remembered and not scaled again.
namespace {
    constexpr uint32_t lane_points = 0x800D57A0, lane_counts = 0x800D5780;

    struct ScaledLane {
        uint32_t points = 0;
        std::vector<uint8_t> speeds;
    };
    ScaledLane scaled_lanes[4];

    // A California Speed track's lanes: the speeds func_800924E4 works the race time out from (`timed`: California
    // Speed's / 1.2, the times its tracks have always had) and the ones the drones drive (`driven`: its true mph,
    // capped where a Rush 2 car can't hold them, rush2::calspeed::capped_lane_speeds), both through the Car Speeds lane
    // map. The lanes hold `timed` until the race time is worked out (rush2_race_lane_speeds), then `driven`
    // (rush2_track49_race_time). `mph` and `capped` are the file's speeds and their caps, mapped again every race (the
    // Car Speeds setting can change between two races on the same loaded path). `partial` is capped for CS Physics
    // Partial: the computer cars run California Speed's physics (7.6 g of grip, its road hold over crests) and the
    // player's car its Rush 2 values under it (3.28 g: Rush 2's 2.6 tire grip on California Speed's 1100 lb tire load,
    // the Rush 2 average car's 108.4 slugs), so its curves are capped at 3.25 g and its crests not at all. Uncapped,
    // the drones beat the best a player's car could lap 14% more than on Rush 2's circuits; capped, within 2%
    // (docs/calspeed_research.md#5-paths).
    struct CalSpeedLane {
        uint32_t points = 0;
        std::vector<uint8_t> mph, capped, partial, timed, driven;
    };
    constexpr double partial_corner_g = 3.25;
    CalSpeedLane calspeed_lanes[4];

    bool lane_holds(uint8_t* rdram, uint32_t points, const std::vector<uint8_t>& speeds) {
        for (size_t i = 0; i < speeds.size(); i++) {
            if (MEM_BU(0, (int32_t)(points + i * 8 + 6)) != speeds[i]) return false;
        }
        return true;
    }

    void write_lane(uint8_t* rdram, uint32_t points, const std::vector<uint8_t>& speeds) {
        for (size_t i = 0; i < speeds.size(); i++) {
            MEM_B(0, (int32_t)(points + i * 8 + 6)) = (int8_t)speeds[i];
        }
    }

    bool calspeed_path(uint8_t* rdram) {
        return rush2::calspeed::race_track() > 0 && MEM_B(0, (int32_t)track_id) == host_slot;
    }

    void calspeed_lane_speeds(uint8_t* rdram) {
        int t = rush2::calspeed::race_track() - 1;
        for (int lane = 0; lane < 4; lane++) {
            uint32_t points = (uint32_t)MEM_W(0, (int32_t)(lane_points + lane * 4));
            uint32_t count = MEM_HU(0, (int32_t)(lane_counts + lane * 2));
            CalSpeedLane& l = calspeed_lanes[lane];
            if (points == 0 || count > 4096) {
                continue;
            }
            // The path file can stay loaded from one race to the next, holding the speeds written last time.
            bool again = l.points == points && l.timed.size() == count &&
                         (lane_holds(rdram, points, l.driven) || lane_holds(rdram, points, l.timed));
            if (!again) {
                std::vector<std::array<float, 3>> pos(count);
                l.mph.resize(count);
                for (uint32_t i = 0; i < count; i++) {
                    for (int k = 0; k < 3; k++) pos[i][k] = (float)(int16_t)MEM_H(0, (int32_t)(points + i * 8 + k * 2));
                    l.mph[i] = MEM_BU(0, (int32_t)(points + i * 8 + 6));
                }
                bool loop = !rush2::calspeed::point_to_point(t);
                l.capped = rush2::calspeed::capped_lane_speeds(pos, l.mph, loop);
                l.partial = rush2::calspeed::capped_lane_speeds(pos, l.mph, loop, partial_corner_g, 0.0);
                l.points = points;
            }
            l.timed.resize(count);
            l.driven.resize(count);
            // CS Physics Full CS: California Speed's cars hold its own speeds, uncapped. Partial: `partial`.
            using rush2::calspeed::physics::Mode;
            Mode mode = rush2::calspeed::physics::get_mode();
            const std::vector<uint8_t>& drive = mode == Mode::CalSpeed ? l.mph : mode == Mode::Rushified ? l.partial : l.capped;
            for (uint32_t i = 0; i < count; i++) {
                l.timed[i] = l.mph[i] ? (uint8_t)std::max(1, rush2::car2049::map_lane_speed(l.mph[i], true)) : 0;
                l.driven[i] = drive[i] ? (uint8_t)std::max(1, rush2::car2049::map_lane_speed(drive[i], false)) : 0;
            }
            write_lane(rdram, points, l.timed);
        }
    }
}

extern "C" void rush2_race_lane_speeds(uint8_t* rdram, recomp_context* ctx) {
    if (calspeed_path(rdram)) {
        calspeed_lane_speeds(rdram);
        return;
    }
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
static void path_race_time(uint8_t* rdram) {
    constexpr uint32_t header = 0x8010BCE8;       // Copy of the path header, checkpoints at +0xC, 0x50 bytes each.
    constexpr uint32_t path_pointer = 0x800D575C; // The loaded path file.
    constexpr float rush2_lane_speed = 139.3f;    // Distance-weighted lane 0 speed of Rush 2's 9 race paths, both ways.
    // SF Rush tracks get SF Rush's own times (src/rush1/track1.cpp).
    if (rush2::track1::race_time(rdram)) {
        return;
    }
    int count = (int16_t)MEM_H(0, (int32_t)(header + 8));
    // A custom track with times in its track.toml: start_time is the clock at the start and also what crossing the
    // finish line adds; checkpoint_time[n - 1] is what CHECKPOINT_n adds on lap 1 and on later laps (Rush 2 has no
    // separate lap 3+ time). Without them, and in backward races (whose checkpoints come in the other order), the
    // game's own times from the AI lanes stand.
    if (hosting_mod(rdram)) {
        constexpr uint32_t backward = 0x80119848;
        if (mod_settings.start_time && count > 0 && count <= 10 && MEM_B(0, (int32_t)backward) == 0) {
            int16_t start = (int16_t)std::lround(*mod_settings.start_time);
            MEM_H(0, (int32_t)header) = start;
            MEM_H(0, (int32_t)(header + 0xC + 0x1E)) = start;
            MEM_H(0, (int32_t)(header + 0xC + 0x20)) = start;
            for (int i = 1; i < count && i - 1 < (int)mod_settings.checkpoint_time.size(); i++) {
                const auto& t = mod_settings.checkpoint_time[i - 1];
                MEM_H(0, (int32_t)(header + 0xC + i * 0x50 + 0x1E)) = (int16_t)std::lround(t[0]);
                MEM_H(0, (int32_t)(header + 0xC + i * 0x50 + 0x20)) = (int16_t)std::lround(t[1]);
            }
        }
        return;
    }
    if (rush2::track2049::race_track() == 0 || MEM_B(0, (int32_t)track_id) != host_slot) {
        return;
    }
    uint32_t file = (uint32_t)MEM_W(0, (int32_t)path_pointer);
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

// func_80093048 at 0x80093298 (see path_race_time). A race with the track select's BATTLE row (rush2::battle::race_battle_applies)
// gets half as much time again: the start time and every checkpoint's extensions times 1.5 (fights cost time).
// Entry of func_80092BC4 (spine, $a0 = checkpoint) and func_80092884 (lane, $a0 = checkpoint, $a1 = lane 0-3), which
// func_80092D6C calls at path load and stores into the header copy's checkpoint +0x22 slots. A custom track's path file
// (0x800D575C) already holds its crossings there, worked out by the converter from its markers in 3D (src/track_mod/
// track_mod_convert.cpp), so they are returned as they stand: the game's search (the first change of side within the
// radius, seen from above) can land on another stretch of a compact track's road. Other tracks, and a slot the
// converter left at -1, keep the search.
extern "C" bool rush2_mod_checkpoint_crossing(uint8_t* rdram, recomp_context* ctx, bool lane) {
    constexpr uint32_t path_pointer = 0x800D575C; // The loaded path file: checkpoints at +0xC, 0x50 bytes each.
    constexpr uint32_t spine_count = 0x80111940; // u16
    constexpr uint32_t lane_counts = 0x800D5780; // u16[4]
    if (!hosting_mod(rdram)) return false;
    int cp = (int16_t)ctx->r4;
    int l = lane ? (int16_t)ctx->r5 : -1;
    if (cp < 0 || cp >= 10 || l < -1 || l > 3) return false;
    // The file's own slots: the header copy's are cleared before the search.
    uint32_t file = (uint32_t)MEM_W(0, (int32_t)path_pointer);
    if (file == 0) return false;
    int v = (int16_t)MEM_H(0, (int32_t)(file + 0x2E + cp * 0x50 + (l + 1) * 2));
    int count = lane ? MEM_HU(0, (int32_t)(lane_counts + l * 2)) : MEM_HU(0, (int32_t)spine_count);
    if (v < 0 || v >= count) return false;
    // Test aid: RUSH2_TEST_CROSSINGS=1 logs each crossing used.
    static const bool log = getenv("RUSH2_TEST_CROSSINGS") != nullptr;
    if (log) fprintf(stderr, "[trackmod] checkpoint %d %s%d: crossing %d\n", cp, lane ? "lane " : "spine", lane ? l : 0, v);
    ctx->r2 = v;
    return true;
}

extern "C" void rush2_track49_race_time(uint8_t* rdram, recomp_context* ctx) {
    path_race_time(rdram);
    // The race time is worked out: a California Speed track's drones get their lanes' driving speeds.
    if (calspeed_path(rdram)) {
        for (int lane = 0; lane < 4; lane++) {
            const CalSpeedLane& l = calspeed_lanes[lane];
            uint32_t points = (uint32_t)MEM_W(0, (int32_t)(lane_points + lane * 4));
            if (points != 0 && points == l.points && MEM_HU(0, (int32_t)(lane_counts + lane * 2)) == l.driven.size()) {
                write_lane(rdram, points, l.driven);
            }
        }
    }
    if (!rush2::battle::race_battle_applies(rdram)) return;
    constexpr uint32_t header = 0x8010BCE8;
    int count = (int16_t)MEM_H(0, (int32_t)(header + 8));
    int before = MEM_HU(0, (int32_t)header);
    auto scale = [&](uint32_t at) { MEM_H(0, (int32_t)at) = (int16_t)std::min(32767, (int)std::lround(MEM_HU(0, (int32_t)at) * 1.5)); };
    scale(header);
    for (int i = 0; i < count && i < 10; i++) {
        scale(header + 0xC + (uint32_t)i * 0x50 + 0x1E);
        scale(header + 0xC + (uint32_t)i * 0x50 + 0x20);
    }
    if (getenv("R2_BATTLE_TEST") != nullptr) fprintf(stderr, "[Battle] race time: start %d -> %d s, %d checkpoints x 1.5\n", before, MEM_HU(0, (int32_t)header), count);
}

// The Cal Speed track (0-15) of the race being set up (its track select id) or run (the host slot), or -1.
static int calspeed_race(uint8_t* rdram) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    if (rush2::calspeed::is_menu_track(t)) return t - rush2::calspeed::first_menu_id;
    if (t == host_slot && rush2::calspeed::race_track() > 0) return rush2::calspeed::race_track() - 1;
    return -1;
}

// The custom track (index into rush2::track_mod::tracks()) of the race being set up (its track select id) or run (the
// host slot), or -1.
static int mod_race(uint8_t* rdram) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    if (t >= rush2::track2049::mod_menu_id && t < rush2::track2049::mod_menu_id + rush2::track2049::mod_capacity) {
        return t - rush2::track2049::mod_menu_id;
    }
    if (t == host_slot && rush2::track_mod::race_track() >= 0) return rush2::track_mod::race_track();
    return -1;
}

// End of func_80094698, which copied the race options into the race's settings: the obstacle course is raced alone
// over one lap, forwards and unmirrored, with checkpoints on (Rush 2049 has no such options for it). A Cal Speed
// track is raced forwards (California Speed has no backward races, and its tracks one path), a point-to-point one over
// one lap (its finish line isn't its start); so is a point-to-point custom track (track.toml point_to_point). A run-in
// custom track (run_in) is raced forwards: its path has one direction.
extern "C" void rush2_track49_obstacle_settings(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t laps = 0x8010C0E2;          // s16
    constexpr uint32_t drones = 0x800D3E90;        // s16
    constexpr uint32_t checkpoints = 0x8010C17B;   // u8: the race is against the clock
    constexpr uint32_t backward = 0x80119848;
    constexpr uint32_t mirror = 0x800D0190;
    // AI-agent test aid, not in the menu: RUSH2_TEST_BACKWARD=1 races backward without going to the option row.
    static const bool test_backward = getenv("RUSH2_TEST_BACKWARD") != nullptr;
    if (test_backward) MEM_B(0, (int32_t)backward) = 1;
    if (rush2::track2049::battle_race(rdram)) {
        // Battle arenas are stunt mode, which has no drones; Rush 2049's battle was multiplayer only. The drones here
        // are the computer opponents (src/rush2049/battle_ai.cpp): the track select's DRONES, which the BATTLE select
        // keeps open (src/rush2049/track2049_menu.cpp).
        constexpr uint32_t players = 0x8010C3E2;   // s16
        constexpr uint32_t menu_drones = 0x800D5761;   // s8: menu settings +0x1
        MEM_H(0, (int32_t)drones) = (int16_t)rush2::battle_ai::race_opponents(MEM_B(0, (int32_t)menu_drones) & 7,
                                                                               std::max<int>(1, (int16_t)MEM_H(0, (int32_t)players)));
        MEM_B(0, (int32_t)backward) = 0;
        MEM_B(0, (int32_t)mirror) = 0;
        return;
    }
    // Test aid: R2_BATTLE_TEST=bots<n> races n drones on any course too, whatever DRONES says.
    if (const char* test = getenv("R2_BATTLE_TEST"); test != nullptr && strstr(test, "bots") != nullptr &&
        !rush2::track2049::obstacle_race(rdram)) {
        constexpr uint32_t players = 0x8010C3E2;   // s16
        MEM_H(0, (int32_t)drones) = (int16_t)rush2::battle_ai::race_opponents(0, std::max<int>(1, (int16_t)MEM_H(0, (int32_t)players)));
    }
    if (int cs = calspeed_race(rdram); cs >= 0) {
        if (rush2::calspeed::point_to_point(cs)) MEM_H(0, (int32_t)laps) = 1;
        MEM_B(0, (int32_t)backward) = 0;
        return;
    }
    if (int mod = mod_race(rdram); mod >= 0 && rush2::track_mod::point_to_point(mod)) {
        MEM_H(0, (int32_t)laps) = 1;
        MEM_B(0, (int32_t)backward) = 0;
        return;
    }
    if (int mod = mod_race(rdram); mod >= 0 && rush2::track_mod::run_in(mod)) {
        MEM_B(0, (int32_t)backward) = 0;
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
// running. Rush 2's stunt mode skips it already. A simulation run's parked player 1 isn't reset either
// (src/sim_run.cpp).
extern "C" void rush2_track49_stuck_timer(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::stuck_reset_off(rdram) || rush2::sim::parked(rdram, (uint32_t)ctx->r16)) {
        MEM_W(0, (int32_t)((uint32_t)ctx->r16 + 0x7F8)) = 0;
    }
}

// func_8008E40C at 0x8008E678, a wrecked player waiting to be put back: $f16 = 3.5, the seconds since the wreck
// after which the car is put back. Rush 2049's wait (func_800D138C at 0x800D1454, its car state +0x358 going from 1
// to 2 once its time passes the +0x10C stamp) is 2.25 s in a battle (game type 6) and 3.5 s otherwise. The car stays
// burnt (flag 0x10) until it is put back, so a shorter wait shows it whole inside its own explosion.
extern "C" void rush2_track49_respawn_delay(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::game_type(rdram) != rush2::track2049::GameType::none) {
        ctx->f16.fl = rush2::track2049::wreck_wait(rdram);
    }
}

// func_800A5ADC (race setup) before it stores gravity (0x80110018, 32.2 x the gravity cheat's multiplier) at 0x800A5E4C
// ($f8) and 0x800A6170 ($f6). Rush 2049 races with 32.2 x {1.0, 1.15, 1.3, 1.5} (0x801202F8) where Rush 2 has {0.8,
// 1.0, 1.5, 2.0} (0x800CB580), both Normal at index 1, so its tracks get 2049's multiplier for the same setting
// (docs/rush2049_research/car_physics.md, "Gravity: Rush 2 vs Rush 2049"). A custom race track gets it when its
// track.toml says gravity = "rush2049" (custom arenas, raced as 2049's, always do).
extern "C" void rush2_track49_gravity(uint8_t* rdram, float* gravity) {
    bool mod_2049 = false;
    {
        std::lock_guard lock{ track_mutex };
        mod_2049 = hosting_mod(rdram) && mod_settings.gravity == "rush2049";
    }
    if (rush2::track2049::game_type(rdram) == rush2::track2049::GameType::none && !mod_2049) {
        return;
    }
    constexpr float rush2_scale[4] = { 0.8f, 1.0f, 1.5f, 2.0f };
    constexpr float rush2049_scale[4] = { 1.0f, 1.15f, 1.3f, 1.5f };
    float scale = *gravity / 32.2f;
    int index = 1;
    for (int i = 0; i < 4; i++) {
        if (std::fabs(scale - rush2_scale[i]) < std::fabs(scale - rush2_scale[index])) index = i;
    }
    *gravity = 32.2f * rush2049_scale[index];
    fprintf(stderr, "[2049] gravity %.2f ft/s^2 (setting %d)\n", *gravity, index);
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
// Rush 2049's battle respawn (its tracker func_800D348C for game type 6): the route point farthest in x and z from
// every other car, each taken where it is or, while it is being put back (+0x6C8 != -1; 2049 +0x6C4), at its respawn
// point. The points are tried from a random one on, so a tie goes to a random point. func_800D3B28 then moves on no
// points in a battle and gives the car no speed. Drones (the battle's computer opponents) go by the same rule.
static int battle_respawn_point(uint8_t* rdram, uint32_t self) {
    constexpr uint32_t cars = 0x800F5470, car_size = 0x81C;
    constexpr uint32_t route = 0x80111940;  // Route header: +0 s16 spine points, +4 the points (s16 x, y, z).
    static uint32_t seed = 12345;
    int count = MEM_H(0, (int32_t)route);
    if (count <= 0) {
        return -1;
    }
    uint32_t points = (uint32_t)MEM_W(0, (int32_t)(route + 4));
    auto f = [&](uint32_t addr) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)addr);
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    };
    seed = seed * 0x41C64E6D + 0x3039;
    int start = (int)(((seed >> 16) & 0x7FFF) * (uint32_t)count / 0x8000);
    int best = start;
    float best_d = -1.0f;
    for (int k = 0; k < count; k++) {
        int p = (start + k) % count;
        uint32_t point = points + (uint32_t)p * 6;
        float px = (float)MEM_H(0, (int32_t)point), pz = (float)MEM_H(0, (int32_t)(point + 4));
        float nearest = 1e12f;
        for (int i = 0; i < 8; i++) {
            uint32_t car = cars + (uint32_t)i * car_size;
            if (car == self || MEM_H(0, (int32_t)(car + 0x7E4)) == 0) {
                continue;
            }
            bool respawning = MEM_H(0, (int32_t)(car + 0x6C8)) >= 0;
            float x = f(car + (respawning ? 0x664 : 0x224)), z = f(car + (respawning ? 0x66C : 0x22C));
            nearest = std::min(nearest, (px - x) * (px - x) + (pz - z) * (pz - z));
        }
        if (nearest > best_d) {
            best_d = nearest;
            best = p;
        }
    }
    return best;
}

extern "C" void rush2_track49_respawn_zone_place(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::game_type(rdram) == rush2::track2049::GameType::battle) {
        uint32_t car = (uint32_t)ctx->r20;
        int p = battle_respawn_point(rdram, car);
        if (p < 0) {
            return;
        }
        uint32_t point = (uint32_t)MEM_W(0, (int32_t)0x80111944) + (uint32_t)p * 6;
        float pos[3] = { (float)MEM_H(0, (int32_t)point), (float)MEM_H(0, (int32_t)(point + 2)) + 3.0f,
                         (float)MEM_H(0, (int32_t)(point + 4)) };
        for (int k = 0; k < 3; k++) {
            uint32_t bits;
            std::memcpy(&bits, &pos[k], sizeof(bits));
            MEM_W(0, (int32_t)(car + 0x664 + k * 4)) = (int32_t)bits;
        }
        MEM_W(0, (int32_t)(car + 0x6C0)) = 0;
        MEM_W(0, (int32_t)(car + 0x7F4)) = MEM_W(0, (int32_t)(car + 0x7F4)) & ~0x10;
        MEM_B(0, (int32_t)(car + 0x648)) = 0;
        if (getenv("R2_BATTLE_TEST") != nullptr) {
            fprintf(stderr, "[Battle] car %d respawns at route point %d (%.0f %.0f %.0f)\n", (int)((car - 0x800F5470) / 0x81C),
                    p, pos[0], pos[1], pos[2]);
        }
        // Faced along the route there (func_8006D348: route point 0x1A8($sp), loaded into $a1 next, of branch $a2 /
        // 0x1B0($sp), -1 the spine; $a3 = on the route rather than a drone's lane).
        MEM_W(0, (int32_t)((uint32_t)ctx->r29 + 0x1A8)) = p;
        MEM_W(0, (int32_t)((uint32_t)ctx->r29 + 0x1B0)) = -1;
        ctx->r6 = -1;
        ctx->r7 = 1;
        return;
    }
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

// Rush 2049's battle start (func_800C4F68 case 0, game types 6 and 4): no start grid. Player p starts on checkpoint
// record [0, 2, 1, 3][p] of the arena's path (every battle arena has 4, one in each corner: its spawn points), at the
// record's position, facing against its direction (x and z negated, y 0; the stock files' directions point back
// against the race), and func_800D3B28 adds no grid slot (+0x704 = 0 for types 4, 5, 6). Rush 2049 has 4 cars at
// most; a computer opponent past those (cars 4-7) starts around the arena as the battle's respawn does
// (battle_respawn_point): the route point farthest in x and z from the starts already taken, facing along the route.
// Rush 2 shares the layout (offsets are 2049's + 4): respawn_place_car_80090A40's start branch (+0x6C4 == 0) copied
// the start point +0x670 to +0x6D8 and the matrix +0x67C to +0x6E4 just before 0x80090C1C, where $t9 holds
// 0x800FAE6C, which (nonzero) skips the grid slot (table 0x800CC2BC into +0x708). Without this the 8-slot grid's back
// row lay past the floor's edge on course 8 and those cars fell under the arena.
static bool battle_start_point(uint8_t* rdram, int index, float pos[3], float forward[3]) {
    constexpr uint32_t checkpoints = 0x8010BCF4, checkpoint_size = 0x50;   // path header copy 0x8010BCE8: +8 count
    constexpr int spawn_of_player[4] = { 0, 2, 1, 3 };
    auto f = [&](uint32_t addr) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)addr);
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    };
    int spawns = std::min<int>(4, MEM_H(0, (int32_t)0x8010BCF0));
    if (index < spawns) {
        uint32_t cp = checkpoints + (uint32_t)spawn_of_player[index] * checkpoint_size;
        for (int k = 0; k < 3; k++) pos[k] = f(cp + k * 4);
        forward[0] = -f(cp + 0xC);
        forward[1] = 0.0f;
        forward[2] = -f(cp + 0x14);
    }
    else {
        int count = MEM_H(0, (int32_t)0x80111940);
        if (count <= 1) return false;
        uint32_t points = (uint32_t)MEM_W(0, (int32_t)0x80111944);
        auto point = [&](int p, float out[3]) {
            uint32_t at = points + (uint32_t)(p % count) * 6;
            for (int k = 0; k < 3; k++) out[k] = (float)MEM_H(0, (int32_t)(at + k * 2));
        };
        // The starts taken: the spawn points, then the opponents before this one.
        std::vector<std::array<float, 3>> taken;
        float fwd[3];
        for (int j = 0; j < index; j++) {
            std::array<float, 3> p;
            if (battle_start_point(rdram, j, p.data(), fwd)) taken.push_back(p);
        }
        int best = 0;
        float best_d = -1.0f;
        for (int p = 0; p < count; p++) {
            float at[3];
            point(p, at);
            float nearest = 1e12f;
            for (const auto& t : taken) nearest = std::min(nearest, (at[0] - t[0]) * (at[0] - t[0]) + (at[2] - t[2]) * (at[2] - t[2]));
            if (nearest > best_d) {
                best_d = nearest;
                best = p;
            }
        }
        float next[3];
        point(best, pos);
        point(best + 1, next);
        forward[0] = next[0] - pos[0];
        forward[1] = 0.0f;
        forward[2] = next[2] - pos[2];
    }
    float len = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (len < 1e-4f) return false;
    forward[0] /= len;
    forward[2] /= len;
    return true;
}

extern "C" void rush2_track49_battle_start(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track2049::game_type(rdram) != rush2::track2049::GameType::battle) {
        return;
    }
    uint32_t car = (uint32_t)ctx->r20;
    int index = (int)((car - 0x800F5470) / 0x81C);
    float pos[3], forward[3];
    if (index < 0 || index >= 8 || !battle_start_point(rdram, index, pos, forward)) {
        return;
    }
    // Rows: right (up x forward), up, forward, as func_800C4F68 builds them.
    const float matrix[9] = { forward[2], 0.0f, -forward[0], 0.0f, 1.0f, 0.0f, forward[0], 0.0f, forward[2] };
    auto write = [&](uint32_t addr, float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        MEM_W(0, (int32_t)addr) = (int32_t)bits;
    };
    for (int k = 0; k < 3; k++) {
        write(car + 0x670 + k * 4, pos[k]);
        write(car + 0x6D8 + k * 4, pos[k]);
        write(car + 0x708 + k * 4, 0.0f);
    }
    for (int k = 0; k < 9; k++) {
        write(car + 0x67C + k * 4, matrix[k]);
        write(car + 0x6E4 + k * 4, matrix[k]);
    }
    ctx->r25 = 1;   // no grid slot
    if (getenv("R2_BATTLE_TEST") != nullptr) {
        fprintf(stderr, "[Battle] car %d starts at (%.0f %.0f %.0f) facing (%.2f %.2f)\n", index, pos[0], pos[1], pos[2], forward[0],
                forward[2]);
    }
}

// In a battle arena (2049's game type 6) a car only wrecks by landing on its roof or by weapons: func_800E0B20 skips
// the hard-impact, over-400 ft/s and +0x50 out-of-range wrecks and func_800CE358 the car-on-car ones there. Rush 2's
// car_car_collision_response_8006E2A8 and car_crash_damage_stage_800715E8 test this before each of those wrecks
// (us.toml) and take their no-wreck path instead.
extern "C" int rush2_track49_battle_arena(uint8_t* rdram) {
    return rush2::track2049::game_type(rdram) == rush2::track2049::GameType::battle;
}

// car_apply_damage_state_to_preview_8008D3F8 at 0x8008D5CC, where a respawned car stops blinking and can be hit again
// (car +0x804 = 1) once $f0 (whole seconds since the car state's +0x304 stamp, 0 while it glides back) passes $f4
// (5). 2049's func_800E571C waits 0.6 s (D_80124480) in a battle instead, counted to the frame; the stamp is renewed
// every frame of the glide, so the 0.6 s start when the car is placed.
extern "C" void rush2_track49_battle_rearm(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2_track49_battle_arena(rdram)) {
        return;
    }
    uint32_t state = (uint32_t)ctx->r2;
    float now, stamp;
    int32_t bits = MEM_W(0, (int32_t)0x80117488);
    std::memcpy(&now, &bits, sizeof(now));
    bits = MEM_W(0, (int32_t)(state + 0x304));
    std::memcpy(&stamp, &bits, sizeof(stamp));
    ctx->f4.fl = 0.6f;
    ctx->f0.fl = now - stamp;
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

// Custom surfaces ([surfaces] in track.toml) use polygon material slots 3-15. The wheel probe func_8006F704 stores the
// type (flags & 15: road 0, dirt 1, grass 2) at car +0x5FC + 4 x wheel and the material ((flags & 0xF0) >> 4) at
// +0x60C + 2 x wheel. The type drives the physics; the material drives the dust and skid effects
// (func_800663CC: dust when a wheel's material is 1), the surface sounds (func_80067358), the particles
// (func_8008270C) and the stunt scoring (func_80065588), which know only materials 0-2 and 8 (airborne). So at
// 0x8006FA90 a custom material is stored as its `like` material, and the custom slot is kept here for the drag and
// torque below. 0x8006FBE4 (no polygon under the wheel: dirt, material 1) and 0x80070620 (airborne: 8, the caller
// func_800706D0) overwrite +0x60C without the hook; custom_slot checks +0x60C still holds the slot's `like` material.
extern "C" void rush2_trackmod_wheel_material(uint8_t* rdram, recomp_context* ctx) {
    if (!hosting_mod(rdram)) return;
    uint32_t key = (uint32_t)ctx->r4;   // car + 2 x wheel
    int slot = (int)ctx->r14 & 15;
    if (mod_drag[size_t(slot)]) {
        wheel_custom[key] = uint8_t(slot);
        ctx->r14 = mod_like[size_t(slot)];
    }
    else {
        wheel_custom.erase(key);
    }
}

extern "C" void rush2_trackmod_wheel_fallback(uint8_t* rdram, recomp_context* ctx) {
    if (!wheel_custom.empty()) wheel_custom.erase((uint32_t)ctx->r3);
}

extern "C" void car_engine_torque_lookup_80070A78(uint8_t* rdram, recomp_context* ctx);

namespace {
    float f32_at(uint8_t* rdram, uint32_t addr) {
        int32_t bits = MEM_W(0, (int32_t)addr);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // The custom material slot under wheel `wheel` of `car`, or -1.
    int custom_slot(uint8_t* rdram, uint32_t car, int wheel) {
        if (!hosting_mod(rdram)) return -1;
        auto it = wheel_custom.find(car + uint32_t(wheel) * 2);
        if (it == wheel_custom.end()) return -1;
        // signed offset (an unsigned one misaddresses)
        if ((MEM_H(0x60C + wheel * 2, (int32_t)car) & 15) != mod_like[it->second]) return -1;
        return it->second;
    }

    // func_8006AFD8 at 0x8006B63C ($f10) and 0x8006B6A4 ($f8): the drag coefficient of the dirt or grass both rear
    // wheels are on (0x800C1A30 / 0x800C1A34: -10 / -15), about to be multiplied by 1 - the off-road factor and the
    // forward speed and added to the longitudinal force (+0x54). Rear wheels on a custom surface take its -drag, the
    // average when the two wheels are on different ones.
    float surface_coefficient(uint8_t* rdram, uint32_t car, float stock) {
        float sum = 0.0f;
        for (int wheel = 2; wheel < 4; wheel++) {
            int slot = custom_slot(rdram, car, wheel);
            sum += slot >= 0 ? -*mod_drag[size_t(slot)] : stock;
        }
        return sum * 0.5f;
    }

    // func_80070DB8 (engine torque) with both rear wheels on dirt or grass (types at +0x604 / +0x608) above 40 rad/s
    // of wheel speed (+0x3D4) uses the off-road torque: the common torque map (+0x400) x descriptor +0xC4, in place of
    // the gear's map (+0x3FC) x the gear's scale (+0xB8 gear 1, +0xBC gear 2, +0xC0 gear 3+), hooked at 0x80070E6C (dirt,
    // $f6) and 0x80070F14 (grass, $f10). That is part of the off-road slowdown, so a custom surface scales it with its
    // drag: drag 0 drives on the road torque, the stock dirt (10) or grass (15) drag or more on the off-road torque,
    // in between a blend; the two rear wheels averaged.
    float surface_torque(uint8_t* rdram, recomp_context* ctx, float offroad) {
        uint32_t car = (uint32_t)ctx->r16;
        float weight = 0.0f;
        bool custom = false;
        for (int wheel = 2; wheel < 4; wheel++) {
            int slot = custom_slot(rdram, car, wheel);
            if (slot < 0) { weight += 1.0f; continue; }
            custom = true;
            float stock = mod_like[size_t(slot)] == 2 ? 15.0f : 10.0f;
            weight += std::clamp(*mod_drag[size_t(slot)] / stock, 0.0f, 1.0f);
        }
        weight *= 0.5f;
        if (!custom || weight >= 1.0f) return offroad;

        // The gear's torque as func_80070DB8 computes it at 0x80070F20-0x8007108C.
        int32_t gear = MEM_H(0x3DE, (int32_t)car);
        int index = gear == 1 ? 0 : gear == 2 ? 1 : 2;
        uint32_t sp = (uint32_t)ctx->r29;
        int32_t outgoing[2] = { MEM_W(0x10, (int32_t)sp), MEM_W(0x14, (int32_t)sp) };
        recomp_context saved = *ctx;
        float rpm = f32_at(rdram, car + 0x3F8) * f32_at(rdram, 0x800CF848 + index * 4);   // rad/s x 60 / 2 pi
        ctx->r4 = (uint64_t)(int64_t)(int32_t)car;
        ctx->r5 = (uint64_t)(int64_t)(int16_t)(int32_t)rpm;
        ctx->r6 = (uint64_t)(int64_t)(int16_t)(int32_t)(f32_at(rdram, car + 0x3B4) * 128.0f);
        ctx->r7 = (uint64_t)(int64_t)MEM_H(0x3DA, (int32_t)car);
        MEM_W(0x10, (int32_t)sp) = MEM_H(0x3DC, (int32_t)car);
        MEM_W(0x14, (int32_t)sp) = MEM_W(0x3FC, (int32_t)car);
        car_engine_torque_lookup_80070A78(rdram, ctx);
        float road = float((int32_t)ctx->r2) * f32_at(rdram, (uint32_t)MEM_W(0, (int32_t)car) + 0xB8 + index * 4);
        *ctx = saved;
        MEM_W(0x10, (int32_t)sp) = outgoing[0];
        MEM_W(0x14, (int32_t)sp) = outgoing[1];
        return offroad * weight + road * (1.0f - weight);
    }
}

extern "C" void rush2_trackmod_drag_dirt(uint8_t* rdram, recomp_context* ctx) {
    ctx->f10.fl = surface_coefficient(rdram, (uint32_t)ctx->r16, ctx->f10.fl);
}

extern "C" void rush2_trackmod_drag_grass(uint8_t* rdram, recomp_context* ctx) {
    ctx->f8.fl = surface_coefficient(rdram, (uint32_t)ctx->r16, ctx->f8.fl);
}

extern "C" void rush2_trackmod_torque_dirt(uint8_t* rdram, recomp_context* ctx) {
    ctx->f6.fl = surface_torque(rdram, ctx, ctx->f6.fl);
}

extern "C" void rush2_trackmod_torque_grass(uint8_t* rdram, recomp_context* ctx) {
    ctx->f10.fl = surface_torque(rdram, ctx, ctx->f10.fl);
}
