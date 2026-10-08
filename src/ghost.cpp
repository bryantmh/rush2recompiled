// Ghost races: Rush 2049's ghosts (src/ghost_logic.cpp) in Rush 2.
//
// Player 1's car is recorded in every race (Settings > General > Save Ghosts; else only in GHOST RACE, the Start Game
// menu's row, src/track2049_menu.cpp, which takes ONE RACE's path) from its release at the start (car + 0x71C, set by
// func_8008D6F8 at GO) to the finish, one sample per physics tick. A finished run among the profile's fastest on that
// track, direction and lap count (Ghosts Kept, 3 by default) is kept, a file per run (<key>_<profile>_<n>.ghost) in
// the save folder's "ghosts" folder in place of Rush 2049's Controller Pak notes, and the slowest beyond them deleted.
// As on Rush 2049's car select, up to three kept ghosts race in GHOST RACE (GHOST 1-3, chosen on the car select with
// the C buttons, the fastest by default), each a computer car (0x800D3E90 = their count):
//
// - Rush 2049 races its ghosts as extra player cars driven by their samples (func_800F6AB8, func_800E5D64). Rush 2's
//   only spare cars are drones: func_800A37F4 gives the drone slots the recorded cars and colors (hooks at 0x800A3B9C,
//   past the check that drones don't share a type, and 0x800A3E7C). func_80075C3C steps drones round robin (schedule
//   0x800CC0E4), so their scheduled steps are skipped and each ghost car is stepped every physics tick after them, as
//   the player's car is, so samples line up tick for tick.
// - Each of a ghost car's ticks (func_80075880) puts back its physics struct and respawn target (0x800D39D8 + car *
//   12, which func_800754B4 steers a wrecked car to) and car state (0x801124A0 + car * 0x354, read by the physics
//   too, but for its own player +0x350, model drawing flags +0xE4, body node +0xF0 (which func_8009D9F4 places at a
//   computer car's position), draw objects +0x108..+0x2FF (its wheels' nodes, run by func_80082D14) and car index,
//   view and camera mode +0x346..+0x349) as its last step left them, then the words the game thread changed on the
//   recorded car before that tick (wrecks, aborts, gear and clutch) and the tick's inputs (+0x728 steering, +0x72C
//   clutch, +0x730 throttle, +0x734 brake, +0x738 gear); its AI (func_80074990) is skipped. The first tick takes the
//   whole recorded struct: the car's setup, damage and position (the ghost waits on the recorded grid spot until GO).
//   Words that point into the game heap (per-race data) stay the ghost car's own, and the car's descriptor (car + 0)
//   is a copy of the recorded one.
// - Rush 2049 steps a ghost on its own clock. Rush 2 takes a car's step from the physics clock (dt = clock - car
//   + 0x714) and moves a wrecked car back by the game clock, so a ghost's step sees both clocks as the recorded
//   step did (the time argument, 0x8010C0E4 and 0x80117488 until its step ends), and between ticks its struct's
//   times (+0x714, +0x6CC, +0x7F8) are moved to this race's clocks for the game thread. With the same world a ghost
//   repeats the recording exactly; every keyframe_ticks ticks the recording keeps the whole struct, and the ghost
//   takes it if it drifted (a breakable knocked over differently, a mover out of phase).
// - Ghosts don't collide with cars (func_8006F3C0, hooks at 0x8006F434 / 0x8006F470), aren't ranked (func_800A1468
//   ranks the first 0x8010C15A cars, the ghosts being the last: 0x800A1674), have no track map or radar dot
//   (func_800B8900 at 0x800B8958, func_800B8CC8 at 0x800B8D28) and are drawn translucent: their models are drawn
//   from copies of their display lists whose render modes blend the second cycle by the fog color's alpha (Rush 2's
//   car modes use the first cycle for fog by shade alpha), between an RT64 push and pop of the other modes and the
//   fog color (set per view at 0x8007C914).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "util/file.h"

#include "data_files.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

#include "car2049.h"
#include "collectibles.h"
#include "ghost.h"
#include "ghost_logic.h"
#include "rush2_hooks.h"
#include "track1.h"
#include "track2049.h"
#include "track2049_convert.h"
#include "wings.h"

extern "C" void physics_car_tick_80075880(uint8_t* rdram, recomp_context* ctx); // A car's physics tick ($a0 car, $a1 clock bits).
extern "C" void car_material_effects_update_800663CC(uint8_t* rdram, recomp_context* ctx); // After a drone's tick, when 0x800D042C is set.
extern "C" void menu_play_sound_80064908(uint8_t* rdram, recomp_context* ctx); // Plays a menu sound.
extern "C" void text_select_font_80088C24(uint8_t* rdram, recomp_context* ctx); // Selects a font.
extern "C" void menu_text_set_scale_from_global_80093FA8(uint8_t* rdram, recomp_context* ctx); // Text setting ($f12), 0 as the car select's.
extern "C" void text_select_style_800737E4(uint8_t* rdram, recomp_context* ctx); // Selects a text style.
extern "C" void text_measure_string_800732AC(uint8_t* rdram, recomp_context* ctx); // Width of a string.
extern "C" void text_print_string_800734E0(uint8_t* rdram, recomp_context* ctx); // Prints a string at (x, y).

using rush2::ghost::Ghost;
using rush2::ghost::Header;
using rush2::ghost::Input;
using rush2::ghost::Patch;

namespace {
    // Game addresses.
    constexpr uint32_t cars = 0x800F5470;            // physics structs, 0x81C each
    constexpr uint32_t car_size = 0x81C;
    constexpr uint32_t car_words = car_size / 4;
    constexpr uint32_t respawn_targets = 0x800D39D8; // per car: 3 floats, where func_800754B4 puts a wrecked car
    constexpr uint32_t car_states = 0x801124A0;      // 0x354 each; +0x342/+0x343 its wreck and respawn state
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t car_state_player = 0x350;     // its human player record, 0 for drones
    constexpr uint32_t car_state_finished = 0xEA;    // s8: 1 once it finished (func_8008E8EC)
    constexpr uint32_t car_state_race_time = 0xEC;   // f32: its race time, set with +0xEA
    constexpr uint32_t car_state_draw_flags = 0xE4;  // how its model is drawn (func_8009E6DC, func_80087290)
    constexpr uint32_t car_state_body_node = 0xF0;   // its body's scene node (func_80087290), placed by func_8009D9F4
    // Its 18 draw objects (0x1C each, +0x18 a callback; func_80082D14 runs them): its wheels' nodes among them.
    constexpr uint32_t car_state_draw_objects = 0x108, car_state_draw_objects_end = 0x108 + 18 * 0x1C;
    constexpr uint32_t car_state_identity = 0x346;   // s8 each: its car's index, its view, the view's camera mode x2
    constexpr uint32_t identity_bytes = 4;
    // The mirrored words: the physics struct, the respawn target, the car state (physics reads it too).
    constexpr uint32_t first_target_word = car_words;
    constexpr uint32_t first_state_word = car_words + 3;
    constexpr uint32_t mirror_words = first_state_word + car_state_size / 4;
    constexpr uint32_t car_table = 0x800D9C10;       // per car slot: 9 bytes (+1 type, +2..+5 colors and stripe)
    constexpr uint32_t players = 0x800C2140;         // player records, 0x28 each; +0 = the player's car, +2 pressed
    constexpr uint32_t player_count = 0x8010C3E2;    // s16
    constexpr uint32_t game_state = 0x8010C0D0;      // 2 = the car select
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t backward_flag = 0x80119848;
    constexpr uint32_t mirror_flag = 0x800D0190;
    constexpr uint32_t race_laps = 0x8010C0E2;       // s16
    constexpr uint32_t race_drones = 0x800D3E90;     // s16
    constexpr uint32_t demo_flag = 0x800FAE6C;       // the attract demo: every car is a drone
    constexpr uint32_t physics_clock = 0x8010C0E4;   // f32, + the tick each physics tick (func_80076578)
    constexpr uint32_t game_clock = 0x80117488;      // f32
    constexpr uint32_t drone_extra = 0x800D042C;     // u8: func_80075C3C calls func_800663CC after each drone's tick
    constexpr uint32_t body_nodes = 0x80219DD0;      // + car * 0x134: the car's body scene node index
    constexpr uint32_t body_node_stride = 0x134;
    constexpr uint32_t node_pool = 0x800D9E90;
    constexpr uint32_t node_size = 0x38;
    constexpr uint32_t max_nodes = 0x400;

    // Car physics struct fields.
    constexpr uint32_t f_release = 0x71C;            // s16: nonzero once the car may drive
    constexpr uint32_t f_laps_done = 0x7FD;          // s8: laps completed (func_8008F220)
    constexpr uint32_t f_kind = 0x7E8;               // u8: 1 drone, 2 human
    constexpr uint32_t f_type = 0x7EA;
    constexpr uint32_t f_steer = 0x728, f_clutch = 0x72C, f_throttle = 0x730, f_brake = 0x734, f_gear = 0x738;
    constexpr uint32_t f_position = 0x224;
    constexpr uint32_t desc_size = 0xEC;

    // Struct words a ghost keeps: the descriptor pointer (its own copy), index, car state index, kind and type,
    // and the collision flag 0x804.
    bool own_word(uint32_t off) {
        return off == 0x000 || off == 0x7E0 || off == 0x7E4 || off == 0x7E8 || off == 0x804;
    }
    bool input_word(uint32_t off) {
        return off == f_steer || off == f_clutch || off == f_throttle || off == f_brake;
    }
    // By mirrored word index: the words a ghost keeps (above, and its car state's player record, model drawing flags,
    // body node and draw objects), and the inputs.
    bool own_index(uint32_t i) {
        if (i < car_words) return own_word(i * 4);
        if (i < first_state_word) return false;
        uint32_t off = (i - first_state_word) * 4;
        return off == car_state_player || off == car_state_draw_flags || off == car_state_body_node ||
               (off >= car_state_draw_objects && off < car_state_draw_objects_end);
    }
    bool input_index(uint32_t i) {
        return i < car_words && input_word(i * 4);
    }
    // Times: +0x714 the physics clock of the car's last step and +0x73C of its drawing snapshot (func_800691CC, which
    // the game thread moves the drawn car on from), +0x6CC (put back after a wreck) and +0x7F8 (became slow; 0 = isn't)
    // the game clock.
    enum class Clock { none, physics, game, game_nonzero };
    Clock clock_of(uint32_t off) {
        if (off == 0x714 || off == 0x73C) return Clock::physics;
        if (off == 0x6CC) return Clock::game;
        if (off == 0x7F8) return Clock::game_nonzero;
        return Clock::none;
    }
    // A pointer into the game heap (per-race allocations).
    bool heap_pointer(uint32_t v) {
        return (v >> 24) == 0x80 && (v & 0xFFFFFF) >= 0x400000;
    }

    // Spare RDRAM (src/controls_menu.cpp lists the other users): translucent display list copies, the side lists
    // that draw them, the ghost cars' descriptors and the drone type check's stand-in table.
    constexpr uint32_t copies_start = 0x80CB0000;
    constexpr uint32_t copies_end = 0x80CE0000;
    constexpr uint32_t side_start = 0x80CE0000;
    constexpr uint32_t side_end = 0x80CEF000;
    constexpr uint32_t ghost_descriptors = 0x80CEF000;  // desc_size each
    constexpr uint32_t no_types = 0x80CEF400;        // 8 x 9 bytes of 0xFF

    constexpr int max_ghosts = 3;                    // Rush 2049's car select: GHOST 1-3
    constexpr uint8_t ghost_alpha = 0x80;
    constexpr uint32_t cutout_alpha = 0x20;         // texels below this alpha aren't drawn (alpha compare)
    constexpr float drift_limit = 0.05f;             // feet, at a keyframe

    // Test aids (environment): RUSH2_GHOST_TEST_TICKS=n keeps a recording after n ticks as if the car had finished;
    // RUSH2_GHOST_TEST_LAST_LAP=1 puts player 1 on the race's last lap at GO (+0x7FD laps done, +0x7FC past halfway)
    // with the lap checkpoint (0x8010BCEC) as its next (+0x800), so the game's own finish (func_8008F220,
    // func_8008E8EC) comes at its next checkpoint;
    // RUSH2_GHOST_DEBUG=1 logs, at every keyframe, how far a ghost is from the recording and the words that differ.
    uint32_t test_ticks() {
        static const uint32_t n = [] {
            const char* v = std::getenv("RUSH2_GHOST_TEST_TICKS");
            return v != nullptr ? (uint32_t)std::strtoul(v, nullptr, 10) : 0u;
        }();
        return n;
    }
    bool test_last_lap() {
        static const bool on = std::getenv("RUSH2_GHOST_TEST_LAST_LAP") != nullptr;
        return on;
    }
    bool debug_log() {
        static const bool on = std::getenv("RUSH2_GHOST_DEBUG") != nullptr;
        return on;
    }

    float get_f32(uint8_t* rdram, uint32_t addr) {
        uint32_t v = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    void put_f32(uint8_t* rdram, uint32_t addr, float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        MEM_W(0, (int32_t)addr) = (int32_t)v;
    }
    uint32_t car_addr(int car) {
        return cars + uint32_t(car) * car_size;
    }
    // Address of mirrored word i of race car `car`.
    uint32_t mirror_addr(int car, uint32_t i) {
        if (i < first_target_word) return car_addr(car) + i * 4;
        if (i < first_state_word) return respawn_targets + uint32_t(car) * 12 + (i - first_target_word) * 4;
        return car_states + uint32_t(car) * car_state_size + (i - first_state_word) * 4;
    }
    void read_mirror(uint8_t* rdram, int car, std::vector<uint32_t>& out) {
        out.resize(mirror_words);
        for (uint32_t i = 0; i < mirror_words; i++) {
            out[i] = (uint32_t)MEM_W(0, (int32_t)mirror_addr(car, i));
        }
    }

    // Calls a game function from a hook, keeping the hooked function's registers. Returns $v0.
    int32_t call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0 = 0,
                 int32_t a1 = 0, int32_t a2 = 0, int32_t a3 = 0, float f12 = 0.0f) {
        recomp_context saved = *ctx;
        ctx->r4 = a0;
        ctx->r5 = a1;
        ctx->r6 = a2;
        ctx->r7 = a3;
        ctx->f12.fl = f12;
        func(rdram, ctx);
        int32_t ret = (int32_t)ctx->r2;
        *ctx = saved;
        return ret;
    }

    struct Key {
        int track = -1;
        int backward = 0;
        int mirror = 0;
        int laps = 0;
        bool operator==(const Key&) const = default;
    };

    std::recursive_mutex mutex;
    std::atomic<bool> chosen_flag = false;
    std::atomic<bool> save_all = true;      // Settings > General > Save Ghosts
    std::atomic<int> ghosts_kept = 3;       // Settings > General > Ghosts Kept

    // The kept ghosts of a key, for the car select's choice: by file, fastest first.
    struct Entry {
        std::filesystem::path path;
        Header header;
    };
    Key list_key;
    std::vector<Entry> entries;
    std::filesystem::path choice[max_ghosts];   // GHOST 1-3 (empty: none)
    int choice_row = 0;
    // Ghosts loaded or recorded this session, by file (a new best is here before its file is written).
    std::map<std::filesystem::path, std::shared_ptr<const Ghost>> cache;

    // Set by the race settings (func_80094698).
    bool race_ghost = false;         // this race is a ghost race
    Key race_key;

    // Per race.
    Ghost recording;
    bool recording_active = false;
    bool recording_started = false;
    bool recording_full = false;
    std::vector<uint32_t> recorder_post;
    int recorder_car = -1;

    struct Racer {
        Ghost ghost;
        int car = -1;
        bool started = false;
        std::vector<uint32_t> post;     // its words after its last step, on the recording's clocks
        bool drift_logged = false;
        Input last_input;               // what it drives with past its last sample
    };
    std::vector<Racer> racers;
    int32_t tick_counter = 0;
    Racer* stepping = nullptr;          // the ghost being stepped from rush2_ghost_tick_end
    // While a ghost's step runs on the recording's clocks: this race's, to put back after it.
    bool clocks_swapped = false;
    uint32_t race_physics_clock = 0;
    uint32_t race_game_clock = 0;
    float physics_offset = 0.0f;        // this race's clocks less the recording's
    float game_offset = 0.0f;

    // Drawing (game thread).
    std::map<uint32_t, uint32_t> copies;      // display list -> translucent copy
    uint32_t copies_cursor = copies_start;
    uint32_t side_cursor = side_start;
    std::atomic<uint32_t> fog_color = 0;

    Racer* racer_of(int car) {
        for (Racer& r : racers) {
            if (r.car >= 0 && r.car == car) return &r;
        }
        return nullptr;
    }

    std::string key_prefix(const Key& k) {
        char name[64];
        std::snprintf(name, sizeof(name), "track%02d_%s%s_%dlap", k.track, k.backward ? "backward" : "forward",
                      k.mirror ? "_mirror" : "", k.laps);
        return name;
    }

    std::filesystem::path ghost_folder() {
        return rush2::data_files::save_folder() / "ghosts";
    }

    // A profile name as it appears in file names.
    std::string profile_tag(const std::string& profile) {
        std::string p;
        for (char c : profile) {
            p += std::isalnum((unsigned char)c) ? (char)std::toupper((unsigned char)c) : '_';
        }
        return p.empty() ? "PLAYER" : p;
    }

    std::string header_profile(const Header& h) {
        return std::string(h.name, strnlen(h.name, sizeof(h.name)));
    }

    // A new file for one of a profile's ghosts of a key: <key>_<profile>_<n>.ghost, n the first unused.
    std::filesystem::path new_ghost_path(const Key& k, const std::string& profile) {
        std::string base = key_prefix(k) + "_" + profile_tag(profile) + "_";
        for (int n = 1;; n++) {
            std::filesystem::path p = ghost_folder() / (base + std::to_string(n) + ".ghost");
            std::error_code ec;
            if (!std::filesystem::exists(p, ec) && cache.find(p) == cache.end()) return p;
        }
    }

    // The race's track select id (0-29): the host slots stand for the added track they hold.
    int menu_track(uint8_t* rdram) {
        int t = (int8_t)MEM_B(0, (int32_t)track_id);
        if (t == rush2::track2049::host_slot) {
            int k = rush2::track2049::race_track();
            if (k >= 1 && k <= rush2::track2049::track_count) return rush2::track2049::first_menu_id + k - 1;
            if (k == rush2::track2049::obstacle) return rush2::track2049::obstacle_menu_id;
            int k1 = rush2::track1::race_track();
            if (k1 >= 1) return rush2::track1::first_menu_id + k1 - 1;
        }
        else if (t == rush2::track2049::stunt_host_slot && rush2::track2049::stunt_arena() > 0) {
            return rush2::track2049::stunt_menu_id + rush2::track2049::stunt_arena() - 1;
        }
        return t;
    }

    Key current_key(uint8_t* rdram) {
        Key k;
        k.track = menu_track(rdram);
        k.backward = MEM_BU(0, (int32_t)backward_flag) != 0;
        k.mirror = MEM_BU(0, (int32_t)mirror_flag) == 1;
        k.laps = (int16_t)MEM_H(0, (int32_t)race_laps);
        return k;
    }

    bool car_available(int type) {
        if (type < rush2::car2049::rush2_types) return true;
        return type >= rush2::car2049::first_type && type < rush2::car2049::types && rush2::car2049::available();
    }

    bool demo(uint8_t* rdram) {
        return MEM_BU(0, (int32_t)demo_flag) != 0;
    }

    // ---- The kept ghosts ------------------------------------------------------------------------------------

    // Lists the kept ghosts of key `k` (files and this session's new ones). A new key starts with the fastest as
    // GHOST 1; the choice stays across races of the same key.
    void refresh_list(const Key& k, bool new_key) {
        std::string prefix = key_prefix(k) + "_";
        std::map<std::filesystem::path, Header> found;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(ghost_folder(), ec)) {
            std::string name = e.path().filename().string();
            if (name.rfind(prefix, 0) != 0 || e.path().extension() != ".ghost") continue;
            Header h;
            if (rush2::ghost::load_header(e.path(), h)) found[e.path()] = h;
        }
        for (const auto& [path, g] : cache) {
            if (path.filename().string().rfind(prefix, 0) == 0) found[path] = g->header;
        }
        entries.clear();
        for (const auto& [path, h] : found) {
            if (car_available(h.car_type) && h.count > 0) entries.push_back({ path, h });
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.header.race_time < b.header.race_time;
        });
        auto listed = [](const std::filesystem::path& p) {
            return std::any_of(entries.begin(), entries.end(), [&](const Entry& e) { return e.path == p; });
        };
        if (new_key) {
            list_key = k;
            choice_row = 0;
            for (auto& c : choice) c.clear();
            if (!entries.empty()) choice[0] = entries[0].path;
        }
        for (auto& c : choice) {
            if (!c.empty() && !listed(c)) c.clear();
        }
    }

    // A profile's kept ghosts of key `k` (files and this session's new ones), fastest first.
    std::vector<Entry> profile_ghosts(const Key& k, const std::string& profile) {
        std::string prefix = key_prefix(k) + "_";
        std::map<std::filesystem::path, Header> found;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(ghost_folder(), ec)) {
            if (e.path().filename().string().rfind(prefix, 0) != 0 || e.path().extension() != ".ghost") continue;
            Header h;
            if (rush2::ghost::load_header(e.path(), h)) found[e.path()] = h;
        }
        for (const auto& [path, g] : cache) {
            if (path.filename().string().rfind(prefix, 0) == 0) found[path] = g->header;
        }
        std::vector<Entry> list;
        for (const auto& [path, h] : found) {
            if (h.count > 0 && profile_tag(header_profile(h)) == profile_tag(profile)) list.push_back({ path, h });
        }
        std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
            return a.header.race_time < b.header.race_time;
        });
        return list;
    }

    void ensure_list(const Key& k) {
        if (!(list_key == k)) refresh_list(k, true);
    }

    int chosen_count() {
        int n = 0;
        for (const auto& c : choice) n += c.empty() ? 0 : 1;
        return n;
    }

    std::shared_ptr<const Ghost> ghost_of(const std::filesystem::path& path) {
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
        auto g = std::make_shared<Ghost>();
        if (!rush2::ghost::load(path, *g)) return nullptr;
        printf("[ghost] Loaded %s: %.2f s\n", path.filename().string().c_str(), g->header.race_time);
        std::fflush(stdout);
        cache[path] = g;
        return g;
    }

    // ---- Recording (physics thread) ------------------------------------------------------------------------

    Input read_input(uint8_t* rdram, recomp_context* ctx, uint32_t car, int car_index) {
        Input in;
        in.steer = get_f32(rdram, car + f_steer);
        in.clutch = get_f32(rdram, car + f_clutch);
        in.throttle = get_f32(rdram, car + f_throttle);
        in.brake = get_f32(rdram, car + f_brake);
        in.gear = (int16_t)MEM_H(0, (int32_t)(car + f_gear));
        rush2::wings::Input w;
        if (rush2::wings::car_input(rdram, car_index, w)) {
            in.wings = (w.held ? 1 : 0) | uint8_t((w.style & 3) << 1);
            in.stick_x = w.stick_x;
            in.stick_y = w.stick_y;
        }
        uint32_t time = (uint32_t)ctx->r5;
        std::memcpy(&in.physics_clock, &time, 4);
        in.game_clock = get_f32(rdram, game_clock);
        return in;
    }

    // `race_time`: the game's time for a finished run (else the recording's length).
    void stop_recording(bool finished, float race_time = 0.0f) {
        if (!recording_active) {
            return;
        }
        recording_active = false;
        struct Flush {
            ~Flush() { std::fflush(stdout); }
        } flush;
        float time = race_time > 0.0f ? race_time : recording.header.position / rush2::ghost::tick_rate;
        if (!rush2::ghost::finish(recording, finished && recording_started, time)) {
            return;
        }
        // The profile keeps its Ghosts Kept fastest runs: a slower run isn't kept, a faster one replaces the slowest.
        std::string profile = header_profile(recording.header);
        std::vector<Entry> kept = profile_ghosts(race_key, profile);
        size_t limit = (size_t)std::max(1, ghosts_kept.load());
        if (kept.size() >= limit && kept[limit - 1].header.race_time <= recording.header.race_time) {
            printf("[ghost] Finished in %.2f s; slower than the %zu kept ghosts\n", time, limit);
            return;
        }
        std::filesystem::path path = new_ghost_path(race_key, profile);
        printf("[ghost] New ghost %s: %.2f s, %zu ticks, %zu patches\n", path.filename().string().c_str(), time,
               recording.inputs.size(), recording.patches.size());
        for (size_t i = limit - 1; i < kept.size(); i++) {
            std::error_code ec;
            std::filesystem::remove(kept[i].path, ec);
            cache.erase(kept[i].path);
            printf("[ghost] Removed %s (%.2f s)\n", kept[i].path.filename().string().c_str(), kept[i].header.race_time);
        }
        auto copy = std::make_shared<Ghost>(std::move(recording));
        recording = Ghost{};
        cache[path] = copy;
        if (list_key == race_key) {
            refresh_list(race_key, false);
            if (chosen_count() == 0) choice[0] = path;
        }
        std::thread([copy, path]() {
            if (!rush2::ghost::save(path, *copy)) {
                printf("[ghost] Failed to save %s\n", path.string().c_str());
                std::fflush(stdout);
            }
        }).detach();
    }

    // func_800E5D64's recording branch for the recorded car, before its step.
    void record_tick(uint8_t* rdram, recomp_context* ctx) {
        uint32_t car = car_addr(recorder_car);
        // Finished: func_8008F220 counts a car's laps (+0x7FD) at the finish line, and once they reach the race's
        // (0x8010C0E2) func_8008E8EC marks its car state finished (+0xEA) with its race time (+0xEC).
        uint32_t state = car_states + uint32_t(recorder_car) * car_state_size;
        int laps = (int16_t)MEM_H(0, (int32_t)race_laps);
        if (recording_started && MEM_B(0, (int32_t)(state + car_state_finished)) != 0 &&
            (int8_t)MEM_B(0, (int32_t)(car + f_laps_done)) >= laps) {
            stop_recording(true, get_f32(rdram, state + car_state_race_time));
            return;
        }
        std::vector<Patch> changed;
        std::vector<uint32_t> now;
        read_mirror(rdram, recorder_car, now);
        if (!recording_started) {
            if ((int16_t)MEM_H(0, (int32_t)(car + f_release)) == 0) {
                return;
            }
            recording_started = true;
            if (test_last_lap()) {
                MEM_B(0, (int32_t)(car + f_laps_done)) = (int8_t)((int16_t)MEM_H(0, (int32_t)race_laps) - 1);
                MEM_B(0, (int32_t)(car + 0x7FC)) = 1;
                MEM_H(0, (int32_t)(car + 0x800)) = MEM_H(0, (int32_t)0x8010BCEC); // next checkpoint: the lap line
                read_mirror(rdram, recorder_car, now);
            }
            recording.start = now;
            uint32_t desc = (uint32_t)MEM_W(0, (int32_t)car);
            recording.descriptor.resize(desc_size / 4);
            for (uint32_t i = 0; i < desc_size / 4; i++) {
                recording.descriptor[i] = (uint32_t)MEM_W(0, (int32_t)(desc + i * 4));
            }
            uint32_t time = (uint32_t)ctx->r5;
            std::memcpy(&recording.physics_time, &time, 4);
            recording.game_time = get_f32(rdram, game_clock);
            uint32_t entry = car_table + uint32_t(recorder_car) * 9;
            for (int i = 0; i < 9; i++) {
                recording.header.car_entry[i] = MEM_BU(0, (int32_t)(entry + i));
            }
            recording.header.car_type = MEM_BU(0, (int32_t)(car + f_type));
            std::string name = rush2::collectibles::player_name(rdram, 0);
            std::snprintf(recording.header.name, sizeof(recording.header.name), "%s", name.c_str());
            printf("[ghost] Recording car %d on track %d\n", recorder_car, race_key.track);
            std::fflush(stdout);
        }
        else {
            for (uint32_t i = 0; i < mirror_words; i++) {
                if (now[i] != recorder_post[i] && !own_index(i) && !input_index(i) && !heap_pointer(now[i])) {
                    changed.push_back({ (uint16_t)i, now[i] });
                }
            }
        }
        if (recording_full) {
            return;
        }
        uint32_t n = recording.header.position;
        if (!rush2::ghost::record(recording, read_input(rdram, ctx, car, recorder_car), changed)) {
            recording_full = true;
            return;
        }
        if (n % rush2::ghost::keyframe_ticks == 0) {
            recording.keyframes.push_back(now);
        }
        if (test_ticks() != 0 && recording.header.position >= test_ticks()) {
            stop_recording(true);
        }
    }

    // ---- Playback (physics thread) -------------------------------------------------------------------------

    uint32_t on_race_clocks(uint32_t off, uint32_t value) {
        float f;
        std::memcpy(&f, &value, 4);
        switch (clock_of(off)) {
        case Clock::physics: f += physics_offset; break;
        case Clock::game: f += game_offset; break;
        case Clock::game_nonzero:
            if (value == 0) return value;
            f += game_offset;
            break;
        default: return value;
        }
        std::memcpy(&value, &f, 4);
        return value;
    }

    // Writes mirrored words into a ghost car, but for the words it keeps.
    void put_words(uint8_t* rdram, int car, const std::vector<uint32_t>& words, bool keep_release, bool race_clocks) {
        for (uint32_t i = 0; i < std::min<uint32_t>(mirror_words, (uint32_t)words.size()); i++) {
            uint32_t off = i * 4;
            if (own_index(i) || (keep_release && off == f_release)) {
                continue;
            }
            int32_t at = (int32_t)mirror_addr(car, i);
            uint32_t v = words[i];
            if (heap_pointer(v) || heap_pointer((uint32_t)MEM_W(0, at))) {
                continue;
            }
            MEM_W(0, at) = (int32_t)(race_clocks && i < car_words ? on_race_clocks(off, v) : v);
        }
    }

    void put_descriptor(uint8_t* rdram, const Racer& r, int index) {
        if (r.ghost.descriptor.size() != desc_size / 4) {
            return;
        }
        uint32_t desc = ghost_descriptors + uint32_t(index) * desc_size;
        for (uint32_t i = 0; i < desc_size / 4; i++) {
            MEM_W(0, (int32_t)(desc + i * 4)) = (int32_t)r.ghost.descriptor[i];
        }
        MEM_W(0, (int32_t)car_addr(r.car)) = (int32_t)desc;
    }

    void put_input(uint8_t* rdram, int car_index, const Input& in) {
        uint32_t car = car_addr(car_index);
        put_f32(rdram, car + f_steer, in.steer);
        put_f32(rdram, car + f_clutch, in.clutch);
        put_f32(rdram, car + f_throttle, in.throttle);
        put_f32(rdram, car + f_brake, in.brake);
        MEM_H(0, (int32_t)(car + f_gear)) = in.gear;
        rush2::wings::Input w;
        w.held = (in.wings & 1) != 0;
        w.style = (in.wings >> 1) & 3;
        w.stick_x = in.stick_x;
        w.stick_y = in.stick_y;
        rush2::wings::set_ghost_input(car_index, true, w);
    }

    void log_drift(uint8_t* rdram, const Racer& r, float drift, const std::vector<uint32_t>& keyframe) {
        printf("[ghost] car %d tick %u: %.4f ft from the recording; at %.1f %.1f %.1f (car 0 at %.1f %.1f %.1f);", r.car,
               r.ghost.header.position, drift, get_f32(rdram, car_addr(r.car) + f_position),
               get_f32(rdram, car_addr(r.car) + f_position + 4), get_f32(rdram, car_addr(r.car) + f_position + 8),
               get_f32(rdram, car_addr(0) + f_position), get_f32(rdram, car_addr(0) + f_position + 4),
               get_f32(rdram, car_addr(0) + f_position + 8));
        int shown = 0;
        for (uint32_t i = 0; i < mirror_words && shown < 12; i++) {
            uint32_t off = i * 4, v = (uint32_t)MEM_W(0, (int32_t)mirror_addr(r.car, i)), k = keyframe[i];
            if (v != k && !own_index(i) && !input_index(i) && !heap_pointer(k) && !heap_pointer(v)) {
                printf(" %03X:%08X/%08X", off, v, k);
                shown++;
            }
        }
        printf("\n");
        std::fflush(stdout);
    }

    // func_800E5D64's playback branch for a ghost car, before its step (`clock`: the step's time argument). True:
    // skip the step.
    bool play_tick(uint8_t* rdram, Racer& r, int index, float& clock) {
        uint32_t car = car_addr(r.car);
        if (!r.started) {
            put_descriptor(rdram, r, index);
            if ((int16_t)MEM_H(0, (int32_t)(car + f_release)) == 0) {
                // Waiting for GO on the recorded grid spot.
                physics_offset = get_f32(rdram, physics_clock) - r.ghost.physics_time;
                game_offset = get_f32(rdram, game_clock) - r.ghost.game_time;
                put_words(rdram, r.car, r.ghost.start, true, true);
                return false;
            }
            r.started = true;
            r.ghost.header.state = 1;
            r.ghost.header.position = 0;
            put_words(rdram, r.car, r.ghost.start, false, false);
        }
        else if (r.post.size() == mirror_words) {
            put_words(rdram, r.car, r.post, false, false);
        }
        Input& in = r.last_input;
        const Patch* patches = nullptr;
        size_t patch_count = 0;
        const std::vector<uint32_t>* keyframe = nullptr;
        int step = rush2::ghost::play(r.ghost, tick_counter, in, patches, patch_count, keyframe);
        if (step < 0) {
            return true;
        }
        for (size_t i = 0; i < patch_count; i++) {
            uint32_t w = patches[i].word;
            if (w < mirror_words && !own_index(w)) {
                MEM_W(0, (int32_t)mirror_addr(r.car, w)) = (int32_t)patches[i].value;
            }
        }
        if (keyframe != nullptr && keyframe->size() == mirror_words) {
            float drift = 0.0f;
            for (int a = 0; a < 3; a++) {
                uint32_t v = (*keyframe)[(f_position >> 2) + a];
                float k;
                std::memcpy(&k, &v, 4);
                drift = std::max(drift, std::fabs(k - get_f32(rdram, car + f_position + a * 4)));
            }
            if (debug_log()) {
                log_drift(rdram, r, drift, *keyframe);
            }
            if (drift > drift_limit) {
                if (!r.drift_logged) {
                    printf("[ghost] Car %d drifted %.3f ft by tick %u; put back on the recording\n", r.car, drift,
                           r.ghost.header.position);
                    std::fflush(stdout);
                    r.drift_logged = true;
                }
                put_words(rdram, r.car, *keyframe, false, false);
            }
        }
        put_input(rdram, r.car, in);
        // The step runs on the recording's clocks.
        race_physics_clock = (uint32_t)MEM_W(0, (int32_t)physics_clock);
        race_game_clock = (uint32_t)MEM_W(0, (int32_t)game_clock);
        physics_offset = get_f32(rdram, physics_clock) - in.physics_clock;
        game_offset = get_f32(rdram, game_clock) - in.game_clock;
        put_f32(rdram, physics_clock, in.physics_clock);
        put_f32(rdram, game_clock, in.game_clock);
        clock = in.physics_clock;
        clocks_swapped = true;
        return false;
    }

    // A ghost car's physics tick, as func_80075C3C steps a drone: func_80075880 ($a1 = the clock's bits), then
    // func_800663CC if 0x800D042C.
    void step_racer(uint8_t* rdram, recomp_context* ctx, Racer& r, int index) {
        // The physics reads the car table's driver byte (func_800715E8, func_8006FC94): the recorded car's.
        int32_t driver = (int32_t)(car_table + uint32_t(r.car) * 9 + 8);
        int8_t own_driver = MEM_B(0, driver);
        MEM_B(0, driver) = (int8_t)r.ghost.header.car_entry[8];
        // And the car's kind (+0x7E8: func_80071D78 and the functions it calls treat a human's car differently from a
        // drone's): the recorded car was a human's.
        int32_t kind = (int32_t)(car_addr(r.car) + f_kind);
        int8_t own_kind = MEM_B(0, kind);
        MEM_B(0, kind) = 2;
        // The car state's identity bytes (func_8009E6DC) stay the ghost car's own: its car's index (+0x346), and its view
        // (+0x347, -1 for none) and that view's camera mode (+0x348, +0x349), by which func_8009D02C puts a view's
        // camera on its car. The recorded car's would put player 1's camera on the ghost.
        int32_t identity = (int32_t)(car_states + uint32_t(r.car) * car_state_size + car_state_identity);
        int8_t own_identity[identity_bytes];
        for (int32_t b = 0; b < (int32_t)identity_bytes; b++) own_identity[b] = MEM_B(b, identity);
        float clock = get_f32(rdram, physics_clock);
        bool skip = play_tick(rdram, r, index, clock);
        for (int32_t b = 0; b < (int32_t)identity_bytes; b++) MEM_B(b, identity) = own_identity[b];
        if (!skip) {
            stepping = &r;
            int32_t time;
            std::memcpy(&time, &clock, 4);
            call(rdram, ctx, physics_car_tick_80075880, r.car, time);
            stepping = nullptr;
        }
        if (r.started && clocks_swapped) {
            read_mirror(rdram, r.car, r.post);
            // Back on this race's clocks for the game thread.
            uint32_t car = car_addr(r.car);
            for (uint32_t off : { 0x714u, 0x73Cu, 0x6CCu, 0x7F8u }) {
                MEM_W(0, (int32_t)(car + off)) = (int32_t)on_race_clocks(off, (uint32_t)MEM_W(0, (int32_t)(car + off)));
            }
        }
        if (clocks_swapped) {
            MEM_W(0, (int32_t)physics_clock) = (int32_t)race_physics_clock;
            MEM_W(0, (int32_t)game_clock) = (int32_t)race_game_clock;
            clocks_swapped = false;
        }
        MEM_B(0, driver) = own_driver;
        MEM_B(0, kind) = own_kind;
        if (MEM_BU(0, (int32_t)drone_extra) != 0) {
            call(rdram, ctx, car_material_effects_update_800663CC, r.car);
        }
    }

    // ---- Translucent drawing (game thread) -----------------------------------------------------------------

    // Render mode (G_SETOTHERMODE_L 0xE200001C) for a ghost: the last blender cycle mixes the pixel with memory by
    // the fog color's alpha. Rush 2's car modes fog in cycle 1 (P = fog, A = shade alpha); other modes get the
    // blend in both cycles. The blender then no longer uses the pixel's own alpha, so alpha compare (threshold
    // against the blend color's alpha, set by draw_model) keeps the shape of textures cut out by their alpha: a
    // ghost's sparks against a wall are such quads (without it they drew as solid yellow squares).
    uint32_t translucent_mode(uint32_t rm) {
        constexpr uint32_t cycle2 = (0u << 28) | (1u << 24) | (1u << 20) | (0u << 16);   // IN * FOG_A + MEM * 1-A
        constexpr uint32_t cycle1 = (0u << 30) | (1u << 26) | (1u << 22) | (0u << 18);
        constexpr uint32_t AA_EN = 0x8, Z_CMP = 0x10, Z_UPD = 0x20, IM_RD = 0x40, CVG_DST_FULL = 0x200;
        constexpr uint32_t ZMODE_MASK = 0xC00, ZMODE_XLU = 0x800, ZMODE_DEC = 0xC00, FORCE_BL = 0x4000;
        constexpr uint32_t ac_threshold = 1;    // G_AC_THRESHOLD
        uint32_t blender = (rm >> 30) == 3 ? (rm & 0xCCCC0000) | cycle2 : cycle1 | cycle2;
        uint32_t zmode = (rm & ZMODE_MASK) == ZMODE_DEC ? ZMODE_DEC : ZMODE_XLU;
        uint32_t flags = (rm & (AA_EN | Z_CMP | Z_UPD)) | IM_RD | CVG_DST_FULL | zmode | FORCE_BL;
        uint32_t alpha_compare = (rm & 0x3) != 0 ? (rm & 0x3) : ac_threshold;
        return blender | flags | (rm & 0x4) | alpha_compare;
    }

    // A display list address the G_DL commands of the game's models use (KSEG0 or physical) as KSEG0, if it is in
    // the game heap, where models are loaded; else 0.
    uint32_t heap_list(uint32_t addr) {
        uint32_t top = addr >> 24;
        if (top != 0x80 && top != 0x00) return 0;
        uint32_t a = 0x80000000u | (addr & 0xFFFFFF);
        return a >= 0x80400000 && a < 0x80B00000 ? a : 0;
    }

    // A copy of display list `dl` (and the lists it calls) with translucent render modes, or 0 if it can't be made.
    uint32_t translucent_copy(uint8_t* rdram, uint32_t dl, int depth = 0) {
        uint32_t src = heap_list(dl);
        if (src == 0 || depth > 8) return 0;
        auto it = copies.find(src);
        if (it != copies.end()) return it->second;
        std::vector<std::pair<uint32_t, uint32_t>> cmds;
        for (uint32_t o = src; cmds.size() < 8192; o += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)o), w1 = (uint32_t)MEM_W(4, (int32_t)o);
            cmds.push_back({ w0, w1 });
            uint8_t op = w0 >> 24;
            if (op == 0xDF || (op == 0xDE && ((w0 >> 16) & 0xFF) == 1)) break;
        }
        uint32_t bytes = (uint32_t)cmds.size() * 8;
        if (copies_cursor + bytes > copies_end) return 0;
        uint32_t copy = copies_cursor;
        copies_cursor += bytes;
        copies[src] = copy;
        for (size_t i = 0; i < cmds.size(); i++) {
            auto [w0, w1] = cmds[i];
            uint8_t op = w0 >> 24;
            if (w0 == 0xE200001C) {
                w1 = translucent_mode(w1);
            }
            else if (op == 0xDE || (op == 0xE0 && ((w0 >> 16) & 0xFF) == 1)) {
                uint32_t sub = translucent_copy(rdram, w1, depth + 1);
                if (sub != 0) w1 = (w1 & 0xFF000000) | (sub & 0xFFFFFF);
            }
            MEM_W(0, (int32_t)(copy + i * 8)) = (int32_t)w0;
            MEM_W(4, (int32_t)(copy + i * 8)) = (int32_t)w1;
        }
        return copy;
    }

    uint32_t side_alloc(uint32_t bytes) {
        if (side_cursor + bytes > side_end) side_cursor = side_start;
        uint32_t a = side_cursor;
        side_cursor += bytes;
        return a;
    }

    // Whether scene node `index` is car `car`'s body or below it.
    bool car_node(uint8_t* rdram, int car, int index) {
        int body = (int32_t)MEM_W(0, (int32_t)(body_nodes + uint32_t(car) * body_node_stride));
        if (body < 0 || body >= (int)max_nodes) return false;
        std::vector<int> stack = { body };
        std::set<int> seen;
        while (!stack.empty()) {
            int n = stack.back();
            stack.pop_back();
            if (n < 0 || n >= (int)max_nodes || !seen.insert(n).second) continue;
            if (n == index) return true;
            uint32_t node = node_pool + uint32_t(n) * node_size;
            stack.push_back((int16_t)MEM_H(0, (int32_t)(node + 0xE)));     // first child
            if (n != body) stack.push_back((int16_t)MEM_H(0, (int32_t)(node + 0x10)));  // next sibling
        }
        return false;
    }

    void reset_race_state() {
        recording_active = false;
        recording_started = false;
        recording_full = false;
        recorder_post.clear();
        recorder_car = -1;
        racers.clear();
        tick_counter = 0;
        stepping = nullptr;
        clocks_swapped = false;
        copies.clear();
        copies_cursor = copies_start;
        for (int c = 0; c < 8; c++) {
            rush2::wings::set_ghost_input(c, false, {});
        }
    }

    // ---- The car select's ghost choice (game thread) -------------------------------------------------------

    constexpr int font_small = 0;
    constexpr int style_title = 0xA;    // green
    constexpr int style_label = 4;      // gray
    constexpr int style_value = 1;      // white
    constexpr int style_flash = 0x14;   // the game's flashing highlight
    constexpr int sound_move = 0x3B;
    constexpr int sound_change = 0x49;
    constexpr uint16_t button_cu = 0x0008, button_cd = 0x0004, button_cl = 0x0002, button_cr = 0x0001;
    // The panel: left of the car's stats, above its name (car select coordinates, 320 x 240).
    constexpr int panel_x = 14, panel_value_x = 62, panel_y = 138, panel_row = 10;

    uint32_t text_scratch = 0;
    uint32_t text_at = 0;
    constexpr uint32_t text_scratch_size = 0x200;

    uint32_t str(uint8_t* rdram, const std::string& s) {
        if (text_scratch == 0) {
            text_scratch = (uint32_t)((uint8_t*)recomp::alloc(rdram, text_scratch_size) - rdram) + 0x80000000u;
        }
        if (text_at + s.size() + 1 > text_scratch_size) {
            text_at = 0;
        }
        uint32_t at = text_scratch + text_at;
        for (size_t i = 0; i <= s.size(); i++) {
            MEM_B(0, (int32_t)(at + i)) = i < s.size() ? s[i] : 0;
        }
        text_at += (uint32_t)((s.size() + 4) & ~3u);
        return at;
    }

    void print(uint8_t* rdram, recomp_context* ctx, int style, int x, int y, const std::string& s) {
        call(rdram, ctx, text_select_style_800737E4, style);
        call(rdram, ctx, text_print_string_800734E0, x, y, (int32_t)str(rdram, s));
    }

    std::string upper(const std::string& s) {
        std::string u;
        for (char c : s) u += (char)std::toupper((unsigned char)c);
        return u;
    }

    std::string format_time(float seconds) {
        int hundredths = (int)std::lround(seconds * 100.0f);
        char t[24];
        std::snprintf(t, sizeof(t), "%d:%02d.%02d", hundredths / 6000, hundredths / 100 % 60, hundredths % 100);
        return t;
    }

    // Moves GHOST `row` to the next (+1) or previous (-1) kept ghost not chosen in another row, through NONE.
    void cycle_choice(int row, int dir) {
        std::vector<std::filesystem::path> options = { {} };
        for (const Entry& e : entries) {
            bool taken = false;
            for (int r = 0; r < max_ghosts; r++) {
                taken |= r != row && choice[r] == e.path;
            }
            if (!taken) options.push_back(e.path);
        }
        auto it = std::find(options.begin(), options.end(), choice[row]);
        int i = it == options.end() ? 0 : (int)(it - options.begin());
        int n = (int)options.size();
        choice[row] = options[((i + dir) % n + n) % n];
    }

    void draw_choice(uint8_t* rdram, recomp_context* ctx) {
        text_at = 0;
        call(rdram, ctx, text_select_font_80088C24, font_small);
        call(rdram, ctx, menu_text_set_scale_from_global_80093FA8, 0, 0, 0, 0, 0.0f);
        print(rdram, ctx, style_title, panel_x, panel_y, entries.empty() ? "NO GHOST AVAILABLE" : "GHOSTS   C BUTTONS");
        if (entries.empty()) {
            return;
        }
        for (int r = 0; r < max_ghosts; r++) {
            int y = panel_y + (r + 1) * panel_row;
            print(rdram, ctx, r == choice_row ? style_value : style_label, panel_x, y, "GHOST " + std::to_string(r + 1));
            std::string value = "NONE";
            for (const Entry& e : entries) {
                if (e.path == choice[r]) {
                    std::string name(e.header.name, strnlen(e.header.name, sizeof(e.header.name)));
                    value = upper(name.empty() ? "PLAYER" : name) + " " + format_time(e.header.race_time);
                }
            }
            print(rdram, ctx, r == choice_row ? style_flash : style_value, panel_value_x, y, value);
        }
    }
}

void rush2::ghost::set_chosen(bool c) {
    chosen_flag = c;
}

bool rush2::ghost::chosen() {
    return chosen_flag;
}

bool rush2::ghost::is_ghost_car(int index) {
    std::lock_guard lock{ mutex };
    return racer_of(index) != nullptr;
}

void rush2::ghost::set_save_all(bool on) {
    save_all = on;
}

void rush2::ghost::set_ghosts_kept(int n) {
    ghosts_kept = std::clamp(n, 1, 20);
}

// End of func_80094698 (0x80094A2C), which copied the race options into the race's settings (also run by the menus).
// A ghost race has a computer car for each chosen ghost.
extern "C" void rush2_ghost_settings(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    race_ghost = chosen_flag && !demo(rdram);
    if (!race_ghost) {
        return;
    }
    race_key = current_key(rdram);
    ensure_list(race_key);
    MEM_H(0, (int32_t)race_drones) = (int16_t)chosen_count();
}

// Start of func_800A5110 (0x800A5114), race setup, also on a restart.
extern "C" void rush2_ghost_race_setup(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    stop_recording(false);
    reset_race_state();
    if (demo(rdram) || !(race_ghost || save_all)) {
        return;
    }
    // Every race records (the Save Ghosts option); the settings only ran for this race's track in a ghost race.
    race_key = current_key(rdram);
    // Stunt tracks (STUNT1 and the 2049 stunt arenas) have no finish to time.
    if (race_key.track == rush2::track2049::stunt_host_slot ||
        (race_key.track >= rush2::track2049::stunt_menu_id && race_key.track < rush2::track2049::obstacle_menu_id)) {
        return;
    }
    Header h;
    h.track = (uint8_t)race_key.track;
    h.backward = (uint8_t)race_key.backward;
    h.mirror = (uint8_t)race_key.mirror;
    h.laps = (uint8_t)race_key.laps;
    rush2::ghost::begin_recording(recording, h, rush2::ghost::capacity(race_key.track, race_key.laps));
    recording_active = true;
    recorder_car = MEM_BU(0, (int32_t)players);
    if (!race_ghost || !(list_key == race_key)) {
        return;
    }
    int drones = (int16_t)MEM_H(0, (int32_t)race_drones);
    for (const auto& path : choice) {
        if (path.empty() || (int)racers.size() >= drones) continue;
        auto g = ghost_of(path);
        if (g == nullptr) continue;
        Racer r;
        r.ghost = *g;
        r.ghost.header.state = 0;
        racers.push_back(std::move(r));
    }
}

// func_800A37F4 at 0x800A3B9C: drone slot $s2's random type ($t7) is about to be checked against the cars before it
// ($v0 walks their table entries). The first drone slots become the ghost cars: the recorded type, and a table with
// no types to check against.
extern "C" void rush2_ghost_drone_type(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    int slot = (int32_t)ctx->r18;
    if (racers.empty() || slot < 0 || slot >= 8 || MEM_BU(0, (int32_t)(car_addr(slot) + f_kind)) != 1) {
        return;
    }
    int k = 0;
    for (int j = 0; j < slot; j++) {
        k += MEM_BU(0, (int32_t)(car_addr(j) + f_kind)) == 1 ? 1 : 0;
    }
    if (k >= (int)racers.size()) {
        return;
    }
    Racer& r = racers[k];
    r.car = slot;
    printf("[ghost] Car %d is the ghost of %.16s (type %d)\n", slot, r.ghost.header.name, r.ghost.header.car_type);
    std::fflush(stdout);
    for (uint32_t i = 0; i < 8 * 9; i++) {
        MEM_B(0, (int32_t)(no_types + i)) = (int8_t)0xFF;
    }
    ctx->r15 = r.ghost.header.car_type;
    ctx->r2 = (uint64_t)(int64_t)(int32_t)no_types;
}

// func_800A37F4 at 0x800A3E7C: drone slot $s2's colors are chosen ($s1 = its table entry). A ghost's colors and
// stripe are the recording's. Its driver byte +8 stays a drone's (8) for the race's setup (func_8009E6DC reads a
// human's player record), and is the recording's only during its steps (step_racer).
extern "C" void rush2_ghost_drone_colors(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    Racer* r = racer_of((int32_t)ctx->r18);
    if (r == nullptr) {
        return;
    }
    uint32_t entry = (uint32_t)ctx->r17;
    for (int i = 2; i < 8; i++) {
        MEM_B(0, (int32_t)(entry + i)) = (int8_t)r->ghost.header.car_entry[i];
    }
}

// Start of func_80075880 (a car's physics tick: $a0 = car index, $a1 = the physics clock's bits): records player 1's
// car; a ghost car's scheduled step is skipped (rush2_ghost_tick_end steps it). Returns 1 to skip the tick.
extern "C" int rush2_ghost_car_tick(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    int car = (int32_t)ctx->r4;
    if (recording_active && car == recorder_car) {
        record_tick(rdram, ctx);
    }
    Racer* r = racer_of(car);
    return r != nullptr && r != stepping ? 1 : 0;
}

// Start of func_80074990 (the drones' driver: $a0 = car). Ghost cars are driven by their samples.
extern "C" int rush2_ghost_drive(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    for (const Racer& r : racers) {
        if (r.started && r.car >= 0 && (uint32_t)ctx->r4 == car_addr(r.car)) return 1;
    }
    return 0;
}

// func_80076578 at 0x8007660C, after the players' and drones' steps of this physics tick: the ghosts' steps.
extern "C" void rush2_ghost_tick_end(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    if (recording_active && recording_started) {
        read_mirror(rdram, recorder_car, recorder_post);
    }
    // func_80075880 comes back through rush2_ghost_car_tick and rush2_ghost_drive (the mutex is recursive).
    for (size_t i = 0; i < racers.size(); i++) {
        if (racers[i].car >= 0) {
            step_racer(rdram, ctx, racers[i], (int)i);
        }
    }
    tick_counter++;
}

// func_8006F3C0 (car against car) at 0x8006F434: $t9 = the testing car's ($s3) collision flag; 0 skips its tests.
extern "C" void rush2_ghost_collide_self(uint8_t* rdram, recomp_context* ctx) {
    for (const Racer& r : racers) {
        if (r.car >= 0 && (uint32_t)ctx->r19 == car_addr(r.car)) ctx->r25 = 0;
    }
}

// func_8006F3C0 at 0x8006F470: $t1 = whether the other car ($s2) is in the race; 0 skips it.
extern "C" void rush2_ghost_collide_other(uint8_t* rdram, recomp_context* ctx) {
    for (const Racer& r : racers) {
        if (r.car >= 0 && (uint32_t)ctx->r18 == car_addr(r.car)) ctx->r9 = 0;
    }
}

// func_800A1468 at 0x800A1674: $ra = the number of race cars (0x8010C15A) about to be ranked. The ghosts, the last
// cars, aren't.
extern "C" void rush2_ghost_standings(uint8_t* rdram, recomp_context* ctx) {
    int count = (int32_t)ctx->r31;
    while (count > 0 && racer_of(count - 1) != nullptr) {
        count--;
    }
    ctx->r31 = (uint64_t)(int64_t)count;
}

// func_800BA608 (the race position HUD) at 0x800BA664 and 0x800BA7A8: `cars` = the race's cars (0x8010C158), of
// which the ghosts aren't counted.
extern "C" int32_t rush2_ghost_hud_cars(int32_t cars) {
    for (const Racer& r : racers) {
        cars -= r.car >= 0 ? 1 : 0;
    }
    return cars;
}

// Start of func_8008B0CC, the car hit test for a breakable ($a0 = the car's index, $a1 = the breakable; $v0 = hit):
// ghost cars hit no breakables (nor coins, which are breakables). Returns 1 to skip the test.
extern "C" int rush2_ghost_breakable_hit(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2::ghost::is_ghost_car((int16_t)ctx->r4)) {
        return 0;
    }
    ctx->r2 = 0;
    return 1;
}

// func_80071FBC (car physics) at 0x800723A8, in its drafting loop over the other cars: `$v1` = the other car, skipped
// when it is the car itself (`$s7`). Ghosts neither draft nor are drafted, so a ghost moves as its recording did
// whoever is around it, and no one gains from it.
extern "C" void rush2_ghost_draft(uint8_t* rdram, recomp_context* ctx) {
    if (racer_of((int32_t)ctx->r23) != nullptr || racer_of((int16_t)ctx->r3) != nullptr) {
        ctx->r3 = ctx->r23;
    }
}

// func_800B8900 at 0x800B8958: $a1 = whether the track map's dot for car $t2 is hidden.
extern "C" void rush2_ghost_map_dot(uint8_t* rdram, recomp_context* ctx) {
    if (racer_of((int32_t)ctx->r10) != nullptr) {
        ctx->r5 = 1;
    }
}

// func_800B8CC8 at 0x800B8D28: the radar dot $s0 (+0x2C: bits 8-11 its view, bits 0-3 its car); $t2 = the track,
// compared with 11 (no radar) next.
extern "C" void rush2_ghost_radar_dot(uint8_t* rdram, recomp_context* ctx) {
    if (racer_of((int32_t)(MEM_W(0x2C, (int32_t)ctx->r16) & 0xF)) != nullptr) {
        ctx->r10 = 11;
    }
}

// func_8007C624 at 0x8007C918: $t6 = the view's fog color, just written as G_SETFOGCOLOR.
extern "C" void rush2_ghost_fog_color(uint8_t* rdram, recomp_context* ctx) {
    fog_color = (uint32_t)ctx->r14;
}

// func_803BC048 (the car select's text callback) at 0x803BC7A8, after its text: in a one player ghost race, the
// ghost choice (Rush 2049's car select GHOST 1-3 panels): C up and down pick a row, C left and right its ghost.
extern "C" void rush2_ghost_car_select_text(uint8_t* rdram, recomp_context* ctx) {
    if (!chosen_flag || demo(rdram) || MEM_W(0, (int32_t)game_state) != 2 ||
        (int16_t)MEM_H(0, (int32_t)player_count) != 1) {
        return;
    }
    std::lock_guard lock{ mutex };
    Key k = current_key(rdram);
    ensure_list(k);
    uint16_t pressed = MEM_HU(0, (int32_t)(players + 2));
    if (!entries.empty()) {
        if (pressed & (button_cu | button_cd)) {
            choice_row = (choice_row + ((pressed & button_cd) ? 1 : max_ghosts - 1)) % max_ghosts;
            call(rdram, ctx, menu_play_sound_80064908, sound_move);
        }
        if (pressed & (button_cl | button_cr)) {
            cycle_choice(choice_row, (pressed & button_cr) ? 1 : -1);
            call(rdram, ctx, menu_play_sound_80064908, sound_change);
            MEM_H(0, (int32_t)race_drones) = (int16_t)chosen_count();
        }
    }
    draw_choice(rdram, ctx);
}

void rush2::ghost::draw_model(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    if (racers.empty() || (uint32_t)ctx->r23 < node_pool) {
        return;
    }
    int node = (int)(((uint32_t)ctx->r23 - node_pool) / node_size);
    bool ghost = false;
    for (const Racer& r : racers) {
        ghost |= r.car >= 0 && car_node(rdram, r.car, node);
    }
    if (!ghost) {
        return;
    }
    uint32_t dl = (uint32_t)ctx->r30;
    uint32_t copy = translucent_copy(rdram, dl);
    GfxCommand ex[6];
    gEXPushOtherMode(&ex[0]);
    gEXPushFogColor(&ex[1]);
    gEXPushBlendColor(&ex[2]);
    gEXPopBlendColor(&ex[3]);
    gEXPopFogColor(&ex[4]);
    gEXPopOtherMode(&ex[5]);
    uint32_t side = side_alloc(11 * 8);
    uint32_t at = side;
    auto cmd = [&](uint32_t w0, uint32_t w1) {
        MEM_W(0, (int32_t)at) = (int32_t)w0;
        MEM_W(4, (int32_t)at) = (int32_t)w1;
        at += 8;
    };
    cmd(ex[0].values.word0, ex[0].values.word1);
    cmd(ex[1].values.word0, ex[1].values.word1);
    cmd(ex[2].values.word0, ex[2].values.word1);
    cmd(0xF8000000, (fog_color.load() & 0xFFFFFF00) | ghost_alpha);
    cmd(0xF9000000, cutout_alpha);  // G_SETBLENDCOLOR: the alpha compare threshold
    cmd(0xDE000000, copy != 0 ? copy : dl);
    cmd(0xE7000000, 0);
    cmd(ex[3].values.word0, ex[3].values.word1);
    cmd(ex[4].values.word0, ex[4].values.word1);
    cmd(ex[5].values.word0, ex[5].values.word1);
    cmd(0xDF000000, 0);
    ctx->r30 = (uint64_t)(int64_t)(int32_t)side;
}
