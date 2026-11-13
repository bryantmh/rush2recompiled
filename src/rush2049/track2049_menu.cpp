// Track select entries for the Rush 2049 and SF Rush tracks (docs/rush2049_research/menus.md).
//
// Rush 2's track select (menu overlay: func_803AB294 init and draw, func_803ABE0C per frame) cycles the track id byte
// through 0-11 and shows a carousel of dioramas built from 12-entry tables. With Rush 2049 tracks available it
// offers ids 12-17 after them, Rush 2049's stunt arenas as ids 25-28, its obstacle course as id 29 and its battle
// arenas DM1-DM8 as ids 30-37, with SF Rush (Rush 1) tracks available ids 18-24, then the custom tracks as 38-63,
// California Speed's tracks as 64-79 (src/calspeed/), the custom tracks past the 26th as 80-248 (include/track2049.h,
// mod_id) and the race tracks' Dreamcast versions as 249-254 (disc_menu_id). A course with more than one version
// shows as one entry with a version button (see "Track versions" below). The game keeps the id in a signed byte;
// us.toml turns every load of it (and of the carousel's copy) into an unsigned one, so ids past 127 read as themselves
// (tools/byte_reads.py lists them). The carousel lists them in display order (build_order: custom tracks can be
// listed after another game's with track.toml [menu] after, a course's versions follow it):
// - The per-track tables the screen reads (diorama names 0x803C91E0, scales 0x803C9180, cloud heights 0x803C91B0,
//   logo names 0x803C9668) and its carousel array (0x803D0698, 0x1C bytes per entry) only have room for 12. Copies
//   with room for 256 entries live in the memory the game heap used before it moved (src/assets.cpp), and us.toml
//   repoints the instructions that address them. The loop bounds and wraps go from 12 to 255; func_803AB01C, which says
//   whether a track is unlocked, offers the added tracks only while they are available.
// - The menus test for the stunt track (11) by id: its options other than TRACK, FOG and WIND are greyed and
//   func_80094698 turns backward and mirror off. Hooks make those tests treat the stunt arenas' and the obstacle
//   course's ids as 11, except the test that sets stunt mode: the obstacle course is raced (src/rush2049/track2049.cpp).
// - The dioramas and logos come from a generated copy of asset 3 (src/rush2049/track2049_art.cpp, then
//   rush2::track1::extend_menu_container for the Rush 1 ones).
// - The screen saves the chosen track as the low nibble of byte +0x30 of the player's save record, which can't hold
//   12-37, so such a choice leaves the nibble alone and is kept in the save file's "track_select" section
//   (include/data_files.h) instead, and restored when the
//   screen opens.
// - Car select counts the track's collected keys from 12-entry tables (func_803B1AB0); added tracks have none.
// - The Start Game menu has a GHOST RACE row (src/ghost.cpp), a STUNT row and a BATTLE row after PRACTICE. Through
//   STUNT the track select offers STUNT1, the stunt arenas and the obstacle course only, remembered apart ("stunt" in
//   that section); through BATTLE it offers the battle arenas only ("battle"); through the other rows it leaves
//   them all out.
//
// When a race starts on id 12-37, the id becomes the host slot's (STUNT1's for a stunt or battle arena) and the added track is
// noted for the track hooks (src/rush2049/track2049.cpp, src/rush1/track1.cpp). The host slot keeps its id through restarts; opening
// the track select clears it again. Circuits never pick a stunt or battle arena or the obstacle course, as they never pick
// STUNT1.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "json/json.hpp"
#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "util/file.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

#include "arcade2049.h"
#include "sfrush_ps1.h"
#include "data_files.h"
#include "game_filter.h"
#include "rush2.h"
#include "rush2_hooks.h"
#include "assets.h"
#include "battle.h"
#include "calspeed.h"
#include "circuit.h"
#include "explosive.h"
#include "time_sprint.h"
#include "ghost.h"
#include "track1.h"
#include "snowfall.h"
#include "track2049.h"
#include "texture_upscale.h"
#include "track_mod_runtime.h"
#include "unlocks.h"
#include "wings.h"
#include "scene_nodes.h"

using namespace rush2::track2049;

extern "C" void menu_play_sound_80064908(uint8_t* rdram, recomp_context* ctx);   // Plays a menu sound ($a0).

namespace {
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t game_mode = 0x8010C3E8;       // 1 = circuit.
    constexpr uint32_t game_state = 0x8010C0D0;      // 1 = the track select.
    constexpr uint32_t shown_track = 0x803D05AC;     // s16: the track the screen last showed.

    // The screen's 12-entry tables (overlay data).
    constexpr uint32_t diorama_names = 0x803C91E0;
    constexpr uint32_t diorama_scales = 0x803C9180;
    constexpr uint32_t cloud_heights = 0x803C91B0;
    constexpr uint32_t logo_names = 0x803C9668;

    // Their copies, with room for 256 entries (us.toml points the screen at these, by these offsets).
    constexpr uint32_t table_data = 0x80310000;
    constexpr uint32_t new_diorama_names = table_data + 0x000;
    constexpr uint32_t new_diorama_scales = table_data + 0x400;
    constexpr uint32_t new_cloud_heights = table_data + 0x800;
    constexpr uint32_t new_logo_names = table_data + 0xC00;
    constexpr uint32_t new_carousel = table_data + 0x1000;         // 256 x 0x1C
    constexpr uint32_t carousel_entry = 0x1C;
    constexpr uint32_t carousel_count = 0x803D05AE;                 // s16: its entries
    constexpr uint32_t carousel_position = 0x803D05B4;              // s16: the entry shown
    constexpr uint32_t new_circuit_instances = table_data + 0x2C00; // 256 x s32, the circuit screen's dioramas
    constexpr uint32_t mod_strings = table_data + 0x3000;          // 32 bytes per custom track: its model and logo names
    constexpr uint32_t calspeed_strings = table_data + 0x5000;     // 32 bytes per Cal Speed track: the same
    static_assert(new_carousel + 256 * carousel_entry <= new_circuit_instances);
    static_assert(mod_strings + rush2::track2049::mod_capacity * 32 <= calspeed_strings);
    constexpr uint32_t menu_data = 0x80300000;
    constexpr uint32_t new_strings = menu_data + 0x900;           // 32 bytes per added track
    constexpr uint32_t stunt_label = menu_data + 0xD00;           // "STUNT", the Start Game menu's added row
    constexpr uint32_t stunt_label_ptr = menu_data + 0xD10;       // char* to it, read as the row's table entry
    constexpr uint32_t unlocks_label = menu_data + 0xD20;         // "UNLOCKS", the row of the unlock system's shop
    constexpr uint32_t unlocks_label_ptr = menu_data + 0xD30;
    constexpr uint32_t ghost_label = menu_data + 0xD40;           // "GHOST RACE"
    constexpr uint32_t ghost_label_ptr = menu_data + 0xD50;
    constexpr uint32_t battle_label = menu_data + 0xD60;          // "BATTLE"
    constexpr uint32_t battle_label_ptr = menu_data + 0xD70;
    // 0xD80-0xF3F: the car select's WINGS rows (src/rush2049/wings_menu.cpp)
    constexpr uint32_t points_label = menu_data + 0xF40;          // "POINTS", the BATTLE select's LAPS row (16 bytes)
    constexpr uint32_t rule_value = menu_data + 0xF50;            // POINTS' or TIME LIMIT's value text (16 bytes)
    constexpr uint32_t battle_row_label = menu_data + 0xF60;      // "BATTLE", the race select's BATTLE row (16 bytes)
    constexpr uint32_t time_label = menu_data + 0xF70;            // "TIME LIMIT", the BATTLE select's CHECKPOINTS row
    constexpr uint32_t preview_strings = menu_data + 0x1000;      // 16 bytes per 2049 entry: its preview model's name
    constexpr uint32_t disc_preview_strings = menu_data + 0x1200; // 16 bytes per Dreamcast version (249-254): the same
    constexpr uint32_t disc_model_strings = menu_data + 0x1300;   // 16 bytes per Dreamcast version: its diorama's name
    static_assert(preview_strings + 32 * 16 <= disc_preview_strings);

    // Unlock bytes of PIPE (9) and ATARI (10) (func_803AB01C).
    constexpr uint32_t pipe_unlocked = 0x800E7D50;
    constexpr uint32_t atari_unlocked = 0x800E7D19;


    // Circuit mode: the race list func_800A7DCC generates, 4 bytes per race (track, direction bits, fog, wind).
    constexpr uint32_t circuit_races = 0x800D3A60;
    constexpr uint32_t circuit_last = 0x800D3DF0;    // s8: index of the last race
    constexpr int circuit_stock_tracks = 7;          // Rush 2 draws circuits from tracks 0-6
    constexpr int circuit_max_uses = 4;

    constexpr int rush2_tracks = 12;
    constexpr int r1_first = rush2::track1::first_menu_id;               // 18
    constexpr int r1_end = r1_first + rush2::track1::track_count;       // 25
    constexpr int cs_first = rush2::calspeed::first_menu_id;            // 64
    constexpr int cs_end = cs_first + rush2::calspeed::track_count;     // 80
    constexpr int menu_tracks = menu_id_limit;                          // ids 0-254 (the tables have room for 256)
    // Circuits pick from every entry but arenas (the circuit screen's instance array holds 256: us.toml).
    constexpr int circuit_tracks = menu_id_limit;
    static_assert(mod_menu_id == battle_menu_id + battle_count);        // the custom tracks (src/track_mod/) follow
    static_assert(mod_menu_id + mod_low_count == cs_first);             // then the Cal Speed tracks
    static_assert(cs_end == mod_high_menu_id);                          // then the rest of the custom tracks
    static_assert(menu_tracks <= 256);
    constexpr int added_tracks = menu_tracks - first_menu_id;           // 2049, Rush 1, stunt, obstacle, battle, custom, Cal Speed
    static_assert(first_menu_id + track_count == r1_first);
    static_assert(r1_end == stunt_menu_id);
    static_assert(stunt_menu_id + stunt_count == obstacle_menu_id);
    static_assert(obstacle_menu_id + 1 == battle_menu_id);

    // Track select entry t's preview model name (in RDRAM; 2049 entries), or 0 for its diorama.
    uint32_t preview_names[menu_tracks] = {};

    // The custom tracks' types (rush2::track_mod::RaceType) by custom track index (mod_index), from refresh_mods.
    std::vector<int> mod_types;
    // Whether each custom track (by mod_index) has Time Sprint's clocks (rush2::time_sprint::has_clocks), from
    // refresh_mods.
    std::vector<bool> mod_clocks;

    int mod_type(int t) {
        int i = mod_index(t);
        return i >= 0 && size_t(i) < mod_types.size() ? mod_types[size_t(i)] : rush2::track_mod::race_race;
    }

    // Rush 2049's battle arenas and the custom ones (track.toml type = "battle").
    bool is_battle_arena(int t) {
        return (t >= battle_menu_id && t < battle_menu_id + battle_count) || mod_type(t) == rush2::track_mod::race_battle;
    }

    // A stunt or battle arena (hosted by STUNT1, which they are played as), Rush 2049's or a custom one.
    bool is_arena(int t) {
        return (t >= stunt_menu_id && t < stunt_menu_id + stunt_count) || is_battle_arena(t) ||
               mod_type(t) == rush2::track_mod::race_stunt;
    }

    // A stunt or battle arena or the obstacle course: the entries besides STUNT1 whose options the menus grey as
    // STUNT1's.
    bool is_stunt_course(int t) {
        return is_arena(t) || t == obstacle_menu_id;
    }

    // The track select's options (0x803D05D8[row], row = the cursor 0x803D05BC, count 0x803D05CC, top visible row
    // 0x803D05C4; func_803AB294 builds the list): 0 TRACK, 1 BACKWARD, 2 MIRROR, 3 FOG, 4 WIND, 5 LAPS, 6 DRONES,
    // 7 DIFFICULTY, 8 HANDICAP, 9 CHECKPOINTS, 10 DEATHS (labels 0x800C48F8[language * 11 + option]).
    constexpr uint32_t option_rows = 0x803D05D8;
    constexpr uint32_t option_cursor = 0x803D05BC;
    constexpr uint32_t option_count = 0x803D05CC;
    constexpr uint32_t option_top = 0x803D05C4;
    constexpr int option_track = 0;
    constexpr int option_backward = 1;
    constexpr int option_fog = 3;
    constexpr int option_wind = 4;
    constexpr int option_laps = 5;      // POINTS on the BATTLE select (rush2_track49_battle_rule_*)
    constexpr int option_drones = 6;
    constexpr int option_difficulty = 7;
    constexpr int option_checkpoints = 9;   // TIME LIMIT on the BATTLE select
    constexpr int option_deaths = 10;
    constexpr int option_battle = 11;   // Added: the race select's BATTLE row (rush2_track49_battle_*)
    constexpr int option_boxes = 4;     // OPTIONTEXTBOX widgets: the rows on screen

    // Whether option `option` stays open on track t although STUNT1 greys it: the obstacle course is raced (a car
    // can die on it), so DEATHS stays; a battle arena's DRONES is its computer opponents, DIFFICULTY their skill
    // (src/rush2049/battle_ai.cpp), and LAPS and CHECKPOINTS, shown as POINTS and TIME LIMIT, the battle's rules.
    bool option_open(int t, int option) {
        return (t == obstacle_menu_id && option == option_deaths) ||
               (is_battle_arena(t) && (option == option_laps || option == option_checkpoints || option == option_drones ||
                                       option == option_difficulty));
    }

    int cursor_option(uint8_t* rdram) {
        int row = (int32_t)MEM_W(0, (int32_t)option_cursor);
        return (int32_t)MEM_W(0, (int32_t)(option_rows + row * 4));
    }


    // The custom tracks the track select offers: whether each converted (its art built), and the enabled mods they came
    // from (rebuilt when those change).
    std::vector<bool> mod_ok;
    // By custom track index: from one of the app's own arcade mods (by its mod id, not anything a track.toml can say):
    // 1 Rush 2049's, which the game filter files under Rush 2049 (rush2::track2049::mod_from_2049), 2 SF Rush's, under
    // SF Rush (mod_from_sfrush); else 0.
    std::array<std::atomic<int>, rush2::track2049::mod_capacity> mod_arcade{};
    std::vector<rush2::track_mod::MenuArt> mod_arts;
    std::string mod_signature;
    // Each custom track's mod id and folder, by index (rush2::track2049::mod_track_id): set with the custom tracks'
    // list, or read once from the enabled mods before the track select first made it.
    std::mutex mod_keys_mutex;
    std::vector<std::pair<std::string, std::string>> mod_keys;
    bool mod_keys_set = false;
    void set_mod_keys(const std::vector<rush2::track_mod::Entry>& entries) {
        std::lock_guard lock{ mod_keys_mutex };
        mod_keys.clear();
        for (const auto& e : entries) mod_keys.emplace_back(e.mod_id, e.folder);
        mod_keys_set = true;
    }
    std::atomic<bool> mods_reload = false;     // rush2::track2049::mods_changed

    bool is_mod_track(int t) {
        return is_mod_id(t);
    }

    // Whether added track select entry t (12-254) can be chosen.
    bool entry_available(int t) {
        if (is_disc_course(t)) return disc_courses_available();
        if (is_mod_track(t)) {
            // A custom battle arena needs the Rush 2049 ROM as the battle does (its weapons, pickups and HUD).
            bool ok = size_t(mod_index(t)) < mod_ok.size() && mod_ok[size_t(mod_index(t))];
            return ok && (mod_type(t) != rush2::track_mod::race_battle || available());
        }
        if (t >= first_menu_id && t < r1_first) return available();
        if (t >= r1_first && t < r1_end) return rush2::track1::available();
        if (t >= cs_first && t < cs_end) return rush2::calspeed::available();
        if (is_stunt_course(t)) return available();
        return false;
    }

    // Which track select a track belongs to: the Start Game menu's STUNT row offers STUNT1, the stunt arenas and the
    // obstacle course, its BATTLE row the battle arenas, and the other rows everything else.
    enum SelectKind : int { select_race, select_stunt, select_battle };

    int kind_of(int t) {
        if (is_battle_arena(t)) return select_battle;
        return t == stunt_host_slot || is_stunt_course(t) ? select_stunt : select_race;
    }

    bool is_stunt_track(int t) {
        return kind_of(t) == select_stunt;
    }

    // The track select the Start Game menu opened (a SelectKind).
    std::atomic<int> select_kind = select_race;
    // The row kind (StartKind) the Start Game menu last chose, -1 (None) before any: rush2::track2049::chosen_start_kind.
    std::atomic<int> chosen_kind = -1;

    // The option list func_803AB294 built for this visit, before filter_options.
    std::vector<int> stock_options;

    // The STUNT and BATTLE track selects list only the options that do something on their tracks: TRACK, FOG, WIND
    // and the options a course keeps open (option_open). STUNT1 grays out the others; a list with fewer options than the
    // rows on screen needs the hooks below (rush2_track49_option_*). Rows past the list hold TRACK, which shows no
    // slider. After a stunt or battle race the game mode is stunt (2, set by func_800AE670), whose list func_803AB294
    // stops at WIND, so the BATTLE select lists its own: TRACK, FOG, WIND, POINTS (LAPS), TIME LIMIT (CHECKPOINTS),
    // DRONES, DIFFICULTY.
    void filter_options(uint8_t* rdram) {
        int t = MEM_BU(0, (int32_t)track_id);
        std::vector<int> ids = stock_options;
        if (select_kind == select_battle) {
            ids = { option_track, option_fog, option_wind, option_laps, option_checkpoints, option_drones,
                    option_difficulty };
        }
        // The race select's BATTLE row, last (below DEATHS), when the battle's models are there (the Rush 2049
        // tracks) and not for a ghost race. It makes a list of 12 with HANDICAP (2 players or more): the twelfth
        // entry is at 0x803D0604, which is the car select's (it sets it when it opens; the track select's screen is
        // set up again each time it opens).
        if (select_kind == select_race && available() && !rush2::ghost::chosen()) ids.push_back(option_battle);
        int n = 0;
        for (int id : ids) {
            if (select_kind != select_race && id != option_track && id != option_fog && id != option_wind &&
                !option_open(t, id)) {
                continue;
            }
            // California Speed has no backward races (one path per track; rush2_track49_obstacle_settings keeps
            // backward off), so its tracks list no BACKWARD, and its point-to-point ones (raced over one lap) no LAPS.
            if (t >= cs_first && t < cs_end &&
                (id == option_backward || (id == option_laps && rush2::calspeed::point_to_point(t - cs_first)))) {
                continue;
            }
            // A point-to-point custom track is raced one way over one lap, as California Speed's; a run-in one forwards.
            if (is_mod_track(t) && (id == option_backward || id == option_laps) &&
                rush2::track_mod::point_to_point(mod_index(t))) {
                continue;
            }
            if (is_mod_track(t) && id == option_backward && rush2::track_mod::run_in(mod_index(t))) {
                continue;
            }
            MEM_W(0, (int32_t)(option_rows + n++ * 4)) = id;
        }
        MEM_W(0, (int32_t)option_count) = n;
        for (int row = n; row <= option_boxes; row++) {     // the rows on screen start after TRACK's
            MEM_W(0, (int32_t)(option_rows + row * 4)) = option_track;
        }
    }

    int option_list_count(uint8_t* rdram) {
        return (int32_t)MEM_W(0, (int32_t)option_count);
    }

    // rush2::track2049::set_all_tracks_open: an online lobby's track select has every track this machine has.
    std::atomic<bool> all_tracks_open = false;
    // rush2::track2049::set_online_offered: with every track open, only those the host's track select offers (its
    // unlocks and game filter), so both carousels hold the same entries and step alike. Custom tracks go by whether
    // the host offers any (each machine numbers its own). A set without a track of the select's kind (the host's last
    // select was another kind) doesn't apply. offered_rebuild: the set changed while the screen was up.
    // Guarded by menu_mutex.
    bool online_offered_on = false;
    std::array<bool, menu_tracks> online_offered{};
    std::array<bool, 3> offered_kinds{};    // by SelectKind: the set has a track of that kind
    bool offered_rebuild = false;

    // Whether the track select would offer track t (0-254) without the game filter: func_803AB01C's unlocks, the added
    // tracks, and the stunt and battle filter.
    bool base_selectable(uint8_t* rdram, int t) {
        bool unlocked;
        if (all_tracks_open) unlocked = t <= stunt_host_slot || entry_available(t);
        else if (t < 9) unlocked = true;
        else if (t == 9) unlocked = rush2::unlocks::track_open(rdram, t, MEM_BU(0, (int32_t)pipe_unlocked) != 0);
        else if (t == 10) unlocked = rush2::unlocks::track_open(rdram, t, MEM_BU(0, (int32_t)atari_unlocked) != 0);
        else if (t == stunt_host_slot) unlocked = true;
        else unlocked = entry_available(t) && rush2::unlocks::track_open(rdram, course_id(t));
        if (all_tracks_open && online_offered_on && offered_kinds[select_kind.load()] && !online_offered[t]) {
            unlocked = false;
        }
        return unlocked && kind_of(t) == select_kind.load();
    }

    // The game filter (src/game_filter.cpp), as of the screen's last setup (rush2_track49_select_init): the games
    // with tracks on this select (0 where the filter can't apply), whether the Game Filter option was on (a change
    // rebuilds the screen), and whether the filter would leave no track (then it is ignored).
    unsigned filter_present = 0;
    bool filter_on = false;
    bool filter_leaves_none = false;
    int filter_keep_track = -1;     // The track shown when a changed filter rebuilt the screen.

    // The carousel's slide, kept across the rebuild a version switch makes (rush2_track49_filter_frame): by entry (in
    // display order) how far its x (+0x4) still is from its target (+0x10), and the highlight's x (0x803CB40C). The
    // build puts every entry at its target, so without this a switch mid-slide jumped to the end of the slide.
    struct CarouselSlide {
        bool valid = false;
        std::vector<float> offsets;
        float highlight_x = 0.0f;
    };
    CarouselSlide kept_slide;
    constexpr uint32_t carousel_highlight_x = 0x803CB40C;   // f32: the highlight's x (init: track's table x - 5)

    float slide_get(uint8_t* rdram, uint32_t addr) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
    void slide_set(uint8_t* rdram, uint32_t addr, float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        MEM_W(0, (int32_t)addr) = (int32_t)bits;
    }

    // Whether the track select would offer track t (0-254) without track versions: base_selectable and the game
    // filter.
    bool member_ok(uint8_t* rdram, int t) {
        using namespace rush2::game_filter;
        return base_selectable(rdram, t) &&
               (filter_present == 0 || !filter_on || filter_leaves_none || MEM_W(0, (int32_t)game_state) != 1 ||
                !hidden(Screen::Tracks, track_game(t)));
    }

    // Track versions (docs/custom_tracks.md#versions). A course with more than one version (the Rush 2049 race
    // tracks' N64 version, 12-17, and Dreamcast version, 249-254; any race track and the custom tracks that name it
    // with track.toml [menu] variant_of, as the arcade mod's versions of the six do) is one carousel entry showing one
    // of them, the one picked last with the version button: the menus' C-left (X, Square; J on the keyboard), with
    // its prompt under the game filter's (rush2::game_filter::draw). That version is what is raced. The others are left
    // out of the list the carousel is built from (track_selectable), as the game filter leaves out a game's tracks; a
    // version the select can't offer (locked, filtered out, not loaded) leaves the others to it. On a circuit's select
    // (the circuit picked the track) and outside the track select every version is offered. The picks are kept per
    // course in the saves ("track_select" "versions", by the course's key and the version's label), so a version
    // that comes back (a mod turned on again) is picked again.
    struct VersionGroup {
        std::string key;            // a race track's "game:name" ("rush2049:marina"), or a custom track's folder
        std::vector<int> members;   // track select ids, the course first
    };
    std::vector<VersionGroup> version_groups;
    std::array<int, menu_tracks> group_of = [] {
        std::array<int, menu_tracks> a;
        a.fill(-1);
        return a;
    }();
    std::vector<std::string> mod_version_labels;    // by custom track index: [menu] variant, else its name (upper case)
    std::map<std::string, std::string> version_picks;   // course key -> the picked version's label
    // Characters: what fits right of the prompt's X before the text drawer's clip, with FILTER where it always was
    // (rush2::game_filter::draw). The Dreamcast versions show as DC for it.
    constexpr size_t version_label_max = 6;

    std::string upper_label(const std::string& text) {
        std::string out;
        for (char c : text) out.push_back((char)std::toupper((unsigned char)c));
        return out.size() > version_label_max ? out.substr(0, version_label_max) : out;
    }

    // A name as a key: lower case, each run of other characters than letters and digits one _ ("Mt. Shasta" ->
    // "mt_shasta").
    std::string key_name(const std::string& name) {
        std::string out;
        for (char c : name) {
            if (std::isalnum((unsigned char)c)) out.push_back((char)std::tolower((unsigned char)c));
            else if (!out.empty() && out.back() != '_') out.push_back('_');
        }
        while (!out.empty() && out.back() == '_') out.pop_back();
        return out;
    }

    // Race track t's key ("rush2049:marina"), "" for an id that isn't another game's race track.
    std::string stock_key(int t) {
        using rush2::game_filter::Game;
        std::string name = rush2::circuit::track_name(t);
        if (name.empty()) return {};
        switch (rush2::game_filter::track_game(t)) {
            case Game::Rush2: return "rush2:" + key_name(name);
            case Game::SFRush: return "sfrush:" + key_name(name);
            case Game::Rush2049: return "rush2049:" + key_name(name);
            case Game::CalSpeed: return "calspeed:" + key_name(name);
            default: return {};
        }
    }

    // What the version button shows for version t: the stock courses of Rush 2049 and SF Rush (both from N64 ROMs) as
    // N64 (or DC), Rush 2's and California Speed's as STOCK.
    std::string version_label(int t) {
        if (t >= first_menu_id && t < first_menu_id + track_count) return courses_from_disc() ? "DC" : "N64";
        if (is_disc_course(t)) return "DC";
        if (t >= r1_first && t < r1_end) return "N64";
        int i = mod_index(t);
        if (i >= 0 && size_t(i) < mod_version_labels.size()) return mod_version_labels[size_t(i)];
        return "STOCK";
    }

    // Groups the courses' versions: the 2049 race tracks' two, then the custom tracks that name a course (through
    // another custom track that names one too, if they chain), each in mod order after the course's own.
    void build_versions(const std::vector<rush2::track_mod::Entry>& entries) {
        version_groups.clear();
        group_of.fill(-1);
        mod_version_labels.clear();
        size_t n = std::min(entries.size(), size_t(mod_capacity));
        for (size_t i = 0; i < n; i++) {
            const rush2::track_mod::Settings& st = entries[i].settings;
            mod_version_labels.push_back(upper_label(st.variant.empty() ? (st.name.empty() ? entries[i].folder : st.name)
                                                                        : st.variant));
        }
        auto add = [&](int course, const std::string& key, int member) {
            int g = group_of[size_t(course)];
            if (g < 0) {
                g = int(version_groups.size());
                version_groups.push_back({ key, { course } });
                group_of[size_t(course)] = g;
            }
            version_groups[size_t(g)].members.push_back(member);
            group_of[size_t(member)] = g;
        };
        for (int k = 1; k <= track_count; k++) {
            int course = first_menu_id + k - 1;
            add(course, stock_key(course), disc_menu_id + k - 1);
        }
        // The track a [menu] variant_of names: a race track's key or a custom track's folder (-1: none).
        auto named = [&](const std::string& name) {
            if (name.find(':') != std::string::npos) {
                for (int t = 0; t < menu_tracks; t++) {
                    if (!is_mod_track(t) && !is_disc_course(t) && stock_key(t) == name) return t;
                }
                return -1;
            }
            for (size_t j = 0; j < n; j++) {
                if (entries[j].folder == name) return mod_id(int(j));
            }
            return -1;
        };
        for (size_t i = 0; i < n; i++) {
            const std::string& of = entries[i].settings.variant_of;
            if (of.empty()) continue;
            int self = mod_id(int(i));
            // The course at the end of the chain of variant_of.
            int course = named(of);
            for (size_t step = 0; course >= 0 && step <= n; step++) {
                int j = mod_index(course);
                if (j < 0 || entries[size_t(j)].settings.variant_of.empty()) break;
                course = named(entries[size_t(j)].settings.variant_of);
            }
            if (course < 0 || course == self || (mod_index(course) >= 0 &&
                                                 !entries[size_t(mod_index(course))].settings.variant_of.empty())) {
                printf("[trackmod] %s: [menu] variant_of = \"%s\" names no race track (\"rush2049:marina\", ...) or "
                       "custom track, or they name each other; it is a track of its own\n", entries[i].folder.c_str(),
                       of.c_str());
                continue;
            }
            if (kind_of(course) != kind_of(self)) {
                printf("[trackmod] %s: [menu] variant_of = \"%s\" is another kind of track (race, stunt or battle); it "
                       "is a track of its own\n", entries[i].folder.c_str(), of.c_str());
                continue;
            }
            int g = group_of[size_t(course)];
            std::string key = g >= 0 ? version_groups[size_t(g)].key
                              : is_mod_track(course) ? entries[size_t(mod_index(course))].folder : stock_key(course);
            add(course, key, self);
        }
    }

    // Whether the select shows one version per course: the track select, not a circuit's.
    bool versions_apply(uint8_t* rdram) {
        return MEM_W(0, (int32_t)game_state) == 1 && MEM_W(0, (int32_t)game_mode) != 1;
    }

    // An online joiner's select, which offers the host's (set_online_offered).
    bool online_joiner() {
        return all_tracks_open && online_offered_on && offered_kinds[size_t(select_kind.load())];
    }

    // The version of group g the select shows: the one picked last, else the first it can offer; on an online
    // joiner's the host's (the one it offers that isn't a custom track, which each machine numbers its own). -1: none.
    int shown_version(uint8_t* rdram, int g) {
        const VersionGroup& group = version_groups[size_t(g)];
        auto pick = version_picks.find(group.key);
        int first = -1, picked = -1, first_stock = -1;
        for (int t : group.members) {
            if (!member_ok(rdram, t)) continue;
            if (first < 0) first = t;
            if (first_stock < 0 && !is_mod_track(t)) first_stock = t;
            if (picked < 0 && pick != version_picks.end() && version_label(t) == pick->second) picked = t;
        }
        if (online_joiner()) return first_stock >= 0 ? first_stock : first;
        return picked >= 0 ? picked : first;
    }

    // Whether the track select offers track t (0-254): member_ok, and the version shown of a course with several.
    bool track_selectable(uint8_t* rdram, int t) {
        if (!member_ok(rdram, t)) return false;
        int g = t >= 0 && t < menu_tracks ? group_of[size_t(t)] : -1;
        return g < 0 || !versions_apply(rdram) || shown_version(rdram, g) == t;
    }

    // The version of t's course the select shows (t when it has no other or none can be offered).
    int shown_of(uint8_t* rdram, int t) {
        int g = t >= 0 && t < menu_tracks ? group_of[size_t(t)] : -1;
        if (g < 0 || !versions_apply(rdram)) return t;
        int shown = shown_version(rdram, g);
        return shown >= 0 ? shown : t;
    }

    // Whether t's course has another version installed, which the prompt names t's version for: even one the
    // select can't offer now (locked), so a course showing its arcade version until the others unlock says so.
    bool other_version(uint8_t* rdram, int t) {
        int g = t >= 0 && t < menu_tracks ? group_of[size_t(t)] : -1;
        if (g < 0 || !versions_apply(rdram) || online_joiner()) return false;
        for (int other : version_groups[size_t(g)].members) {
            if (other != t && entry_available(other)) return true;
        }
        return false;
    }

    // The version the button switches t's course to: the next the select can offer after t, or -1 (no other).
    int next_version(uint8_t* rdram, int t) {
        int g = t >= 0 && t < menu_tracks ? group_of[size_t(t)] : -1;
        if (g < 0 || !versions_apply(rdram) || online_joiner()) return -1;
        const std::vector<int>& members = version_groups[size_t(g)].members;
        auto at = std::find(members.begin(), members.end(), t);
        if (at == members.end()) return -1;
        size_t i = size_t(at - members.begin());
        for (size_t step = 1; step < members.size(); step++) {
            int other = members[(i + step) % members.size()];
            if (member_ok(rdram, other)) return other;
        }
        return -1;
    }


    // Notes which games' tracks the select offers and whether the filter applies to it: not to a circuit's (its TRACK
    // row is greyed) or an online joiner's (it offers the host's select's tracks, filtered by the host's filter).
    void update_filter(uint8_t* rdram) {
        using namespace rush2::game_filter;
        screen_built(Screen::Tracks);
        filter_present = 0;
        filter_on = enabled();
        filter_leaves_none = true;
        if (all_tracks_open || MEM_W(0, (int32_t)game_mode) == 1) {
            return;
        }
        unsigned present = 0;
        bool any_shown = false;
        for (int t = 0; t < menu_tracks; t++) {
            if (base_selectable(rdram, t)) {
                present |= bit(track_game(t));
                any_shown |= !hidden(Screen::Tracks, track_game(t));
            }
        }
        if (std::popcount(present) >= 2) {
            filter_present = present;
            filter_leaves_none = !any_shown;
        }
    }

    std::mutex menu_mutex;
    int selection = -1;            // Added track (id - 12) last chosen on the track select, or -1.
    int stunt_selection = -1;      // Track (11 or 25-29) last chosen on the stunt track select, or -1.
    int battle_selection = -1;     // Track (30-37) last chosen on the battle track select, or -1.
    bool race_battle_selection = false;    // The BATTLE row out of a circuit (a circuit keeps its own: rush2::circuit).
    bool selection_loaded = false;
    std::vector<uint8_t> menu_container;
    // The unlock shop's asset 3 (rush2::track2049::prepare_menu_art): Rush 2's art with the Rush 2049 and SF Rush
    // tracks' only, which is all the shop shows. The shop loads asset 3 into the car select's scene, where the heap has far less room than at
    // the track select: the full container (custom, Dreamcast version and Cal Speed tracks' art too) ran the heap past
    // its top (0x80AA0000) there once the PlayStation SF Rush mod's Chinatown diorama was added (issue #59).
    std::vector<uint8_t> shop_container;
    bool shop_served = false;      // Asset 3 is the shop's: the next serve_menu_container puts the full one back.
    // The custom tracks' dioramas, by mod index, as the served build made them (MenuBuild::streamed), and their copies
    // above 16 MB (stream_diorama).
    std::vector<rush2::track2049::MenuModel> stream_models;
    void stream_forget_all();
    std::shared_ptr<const rush2::rom2049::Source> menu_container_rom; // 2049 ROM the container was built with.
    std::shared_ptr<const std::vector<uint8_t>> menu_container_rom1;   // Rush 1 ROM it was built with.
    std::shared_ptr<const std::vector<uint8_t>> menu_container_rom_cs; // Cal Speed ROM it was built with.
    bool menu_container_built = false;
    // What the container has: the tables name only art that is in it (a missing logo is a null texture the track
    // select reads through).
    bool menu_has_2049 = false;
    bool menu_has_rush1 = false;
    bool menu_has_mods = false;
    bool menu_has_cs = false;
    bool menu_has_disc = false;           // the Dreamcast versions' own previews (disc_preview_name)
    std::string menu_container_mods;      // mod_signature the container was built with
    std::shared_ptr<const rush2::rom2049::Source> menu_container_disc;

    // The Dreamcast disc, for the Dreamcast versions' previews when they are offered beside the N64 ones; else null.
    std::shared_ptr<const rush2::rom2049::Source> disc_versions_source() {
        return disc_courses_available() ? rush2::wings::track_source(true) : nullptr;
    }

    // By custom track index: the Rush 2049 race track (1-6) it is a version of ([menu] variant_of, as the arcade
    // mod's are), else 0. Its preview shows that course's route and color (build_menu).
    std::vector<int> mod_courses() {
        std::vector<int> out(mod_ok.size(), 0);
        for (size_t i = 0; i < out.size(); i++) {
            int g = group_of[size_t(mod_id(int(i)))];
            if (g < 0) continue;
            int course = version_groups[size_t(g)].members[0];
            if (course >= first_menu_id && course < first_menu_id + track_count) out[i] = course - first_menu_id + 1;
        }
        return out;
    }

    // Where a preview's spare buffer room starts in its model blob (just past its tag, preview_buffer_tag after the
    // list's end), or 0 when the blob has none.
    size_t preview_room(const std::vector<uint8_t>& blob, uint32_t list) {
        auto be32 = [&](size_t o) {
            return uint32_t(blob[o] << 24 | blob[o + 1] << 16 | blob[o + 2] << 8 | blob[o + 3]);
        };
        for (size_t at = list; at + 8 <= blob.size(); at += 8) {
            if (blob[at] != 0xDF) continue;
            size_t tag = at + 8;
            return tag + 16 <= blob.size() && be32(tag) == preview_buffer_tag ? tag + 16 : 0;
        }
        return 0;
    }

    // The custom tracks' dioramas aren't in the track select's container (asset 3, loaded into the game heap below
    // 16 MB, which had about 3.8 MB left at the track select with the other games' art in it): an authored diorama can
    // be over a megabyte and a route preview's picture 128 KB, so with many custom tracks they didn't fit and the later
    // ones showed no diorama. The container holds a stub model per custom track (its name, an empty list, the model's
    // radius) and the logos (4.5 KB each, under 1 MB for the most custom tracks the ids allow). Each diorama is copied
    // above 16 MB while its track is near the carousel's shown entry (stream_diorama, after the tables) and the stub's
    // record pointed at the copy; the model draw (rush2::track2049::extended_model_list) has RT64 read lists there with
    // gEXSetRDRAMExtended.
    constexpr size_t stub_bytes = 24;   // G_ENDDL, then 16 zero bytes where a preview's tag would be (find_model_vertices)

    // Asset 3 with the 2049, Rush 1, custom and Cal Speed tracks' dioramas and logos, and what it holds.
    struct MenuBuild {
        std::shared_ptr<const rush2::rom2049::Source> rom;
        std::shared_ptr<const std::vector<uint8_t>> rom1, rom_cs;
        std::string mods;     // mod_signature
        std::shared_ptr<const rush2::rom2049::Source> disc;     // disc_versions_source
        std::vector<uint8_t> container;
        std::vector<uint8_t> shop;     // shop_container: the container before the custom, disc and Cal Speed art
        std::vector<MenuModel> streamed;    // by custom track (mod index): its diorama, streamed in (empty blob: none)
        bool has_2049 = false, has_rush1 = false, has_mods = false, has_cs = false, has_disc = false;
    };

    // The container the background builds made for the track select's next visit (prepare_menu_container), and a
    // count of the custom tracks' file reloads, so a build from art read before one is dropped.
    std::mutex prebuilt_mutex;
    std::optional<MenuBuild> prebuilt;
    int menu_reloads = 0;

    // The save file's section (include/data_files.h): { "selected": n, "stunt": n, "battle": n, "points": n,
    // "minutes": n, "race_battle": 0/1, "versions": { course key: version label } }. "race_battle" is the race select's BATTLE row (rush2::battle::race_battle), off without it.
    // "points" and "minutes" are the BATTLE select's POINTS and TIME LIMIT (rush2::battle::points_to_win,
    // time_limit_minutes, 0 off); a save without "minutes" takes the old Games tab time limit (src/rush2049/wings.cpp,
    // hidden now) if it was changed from its default, 3 minutes.
    const std::string selection_section = "track_select";

    void load_selection() {
        if (selection_loaded) {
            return;
        }
        selection_loaded = true;
        std::string text = rush2::data_files::read(rush2::data_files::File::Saves, selection_section);
        auto read = [&](const char* key) {
            size_t at = text.find(key);
            if (at != std::string::npos && (at = text.find(':', at)) != std::string::npos) {
                return std::atoi(text.c_str() + at + 1);
            }
            return -1;
        };
        selection = read("\"selected\"");
        if (selection < -1 || selection >= added_tracks) {
            selection = -1;
        }
        stunt_selection = read("\"stunt\"");
        if (!is_stunt_track(stunt_selection)) {
            stunt_selection = -1;
        }
        battle_selection = read("\"battle\"");
        if (!is_battle_arena(battle_selection)) {
            battle_selection = -1;
        }
        int points = read("\"points\"");
        rush2::battle::set_points_to_win(points < 0 ? rush2::battle::default_points : points);
        int minutes = read("\"minutes\"");
        if (minutes < 0) {
            int legacy = rush2::battle::legacy_time_limit_minutes();
            minutes = legacy == 3 ? rush2::battle::default_minutes : legacy;
        }
        rush2::battle::set_time_limit_minutes(minutes);
        race_battle_selection = read("\"race_battle\"") == 1;
        rush2::battle::set_race_battle(race_battle_selection);
        // Saves from before track versions have none: each course shows its first version (the N64 one), except the
        // course of the saved choice, which shows that version (an arcade track was an entry of its own then).
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_object() && j.contains("versions") && j["versions"].is_object()) {
            for (const auto& [key, label] : j["versions"].items()) {
                // A build of 2026-11-05 named the Dreamcast versions DREAMCAST and allowed 12 characters.
                std::string l = label.is_string() ? label.get<std::string>() : std::string();
                if (l == "DREAMCAST") l = "DC";
                // Until 2026-11-06 the SF Rush courses' own version showed as STOCK (now N64, beside ARCADE).
                if (l == "STOCK" && key.rfind("sfrush:", 0) == 0) l = "N64";
                if (!l.empty()) version_picks[key] = upper_label(l);
            }
        }
        else if (selection >= 0 && first_menu_id + selection < menu_tracks) {
            int g = group_of[size_t(first_menu_id + selection)];
            if (g >= 0) version_picks[version_groups[size_t(g)].key] = version_label(first_menu_id + selection);
        }
    }

    // The BATTLE row's value as the track select opens: a circuit's own, which it takes from the row out of a circuit
    // when it has none yet (it started, or was recorded before circuits kept one).
    void apply_race_battle(uint8_t* rdram) {
        bool on = race_battle_selection;
        int circuit = rush2::circuit::race_battle(rdram);
        if (circuit >= 0) on = circuit != 0;
        else rush2::circuit::set_race_battle(rdram, on);
        rush2::battle::set_race_battle(on);
    }

    void write_selection() {
        nlohmann::json versions = nlohmann::json::object();
        for (const auto& [key, label] : version_picks) versions[key] = label;
        rush2::data_files::write(rush2::data_files::File::Saves, selection_section,
            "{ \"selected\": " + std::to_string(selection) + ", \"stunt\": " + std::to_string(stunt_selection) +
            ", \"battle\": " + std::to_string(battle_selection) +
            ", \"points\": " + std::to_string(rush2::battle::points_to_win()) +
            ", \"minutes\": " + std::to_string(rush2::battle::time_limit_minutes()) +
            ", \"race_battle\": " + std::to_string(race_battle_selection ? 1 : 0) +
            ", \"versions\": " + versions.dump() + " }");
    }

    void save_selection(int value) {
        if (value == selection) {
            return;
        }
        selection = value;
        write_selection();
    }

    void save_stunt_selection(int value) {
        if (value == stunt_selection) {
            return;
        }
        stunt_selection = value;
        write_selection();
    }

    void save_battle_selection(int value) {
        if (value == battle_selection) {
            return;
        }
        battle_selection = value;
        write_selection();
    }

    void write_string(uint8_t* rdram, uint32_t addr, const std::string& s) {
        for (size_t i = 0; i <= s.size(); i++) {
            MEM_B(0, (int32_t)(addr + i)) = i < s.size() ? s[i] : 0;
        }
    }

    // The track select ids in use, 0 to this (the circuit screen instances these entries' dioramas): up to the Cal
    // Speed tracks, or the last custom track's past them. Set by write_tables.
    std::atomic<int> table_entries = cs_end;

    // Fills the 255-entry tables: Rush 2's 12 entries, then the 2049 tracks', the Rush 1 tracks', the stunt arenas', the
    // obstacle course's, the battle arenas', the custom tracks' and the Cal Speed tracks'.
    void write_tables(uint8_t* rdram) {
        size_t mods = std::min(mod_ok.size(), size_t(mod_capacity));
        // A circuit can pick the Dreamcast versions (249-254), the last ids.
        table_entries = disc_courses_available() ? menu_tracks
                        : mods > size_t(mod_low_count) ? mod_id(int(mods) - 1) + 1 : cs_end;
        for (int t = 0; t < rush2_tracks; t++) {
            MEM_W(0, (int32_t)(new_diorama_names + t * 4)) = MEM_W(0, (int32_t)(diorama_names + t * 4));
            MEM_W(0, (int32_t)(new_diorama_scales + t * 4)) = MEM_W(0, (int32_t)(diorama_scales + t * 4));
            MEM_W(0, (int32_t)(new_cloud_heights + t * 4)) = MEM_W(0, (int32_t)(cloud_heights + t * 4));
            MEM_W(0, (int32_t)(new_logo_names + t * 4)) = MEM_W(0, (int32_t)(logo_names + t * 4));
        }
        uint32_t s = new_strings;
        std::fill(std::begin(preview_names), std::end(preview_names), 0u);
        // The circuit screen instances every entry: entries that aren't available show stock models it can find.
        for (int t = rush2_tracks; t < menu_tracks; t++) {
            MEM_W(0, (int32_t)(new_diorama_names + t * 4)) = MEM_W(0, (int32_t)diorama_names);
            MEM_W(0, (int32_t)(new_logo_names + t * 4)) = MEM_W(0, (int32_t)logo_names);
            MEM_W(0, (int32_t)(new_diorama_scales + t * 4)) = MEM_W(0, (int32_t)diorama_scales);
            MEM_W(0, (int32_t)(new_cloud_heights + t * 4)) = MEM_W(0, (int32_t)cloud_heights);
        }
        auto rom1 = rush2::track1::get_rom();
        if (rush2::track1::available() && rom1 != nullptr && menu_has_rush1) {
            for (int k = 1; k <= rush2::track1::track_count; k++) {
                int t = r1_first + k - 1;
                write_string(rdram, s, "R1TRACK" + std::to_string(k));
                MEM_W(0, (int32_t)(new_diorama_names + t * 4)) = s;
                s += 16;
                write_string(rdram, s, "R1LOGO" + std::to_string(k));
                MEM_W(0, (int32_t)(new_logo_names + t * 4)) = s;
                s += 16;
                float scale = rush2::track1::diorama_scale(*rom1, k - 1);
                uint32_t bits;
                memcpy(&bits, &scale, 4);
                MEM_W(0, (int32_t)(new_diorama_scales + t * 4)) = bits;
                MEM_W(0, (int32_t)(new_cloud_heights + t * 4)) = 0x41700000;  // 15.0, as most stock dioramas
            }
        }
        auto rom_cs = rush2::calspeed::get_rom();
        if (rush2::calspeed::available() && rom_cs != nullptr && menu_has_cs) {
            for (int i = 0; i < rush2::calspeed::track_count; i++) {
                int t = cs_first + i;
                uint32_t m = calspeed_strings + uint32_t(i) * 32;
                write_string(rdram, m, rush2::calspeed::menu_model_name(i));
                MEM_W(0, (int32_t)(new_diorama_names + t * 4)) = m;
                write_string(rdram, m + 16, rush2::calspeed::menu_logo_name(i));
                MEM_W(0, (int32_t)(new_logo_names + t * 4)) = m + 16;
                float scale = rush2::calspeed::diorama_scale(*rom_cs, i);
                uint32_t bits;
                memcpy(&bits, &scale, 4);
                MEM_W(0, (int32_t)(new_diorama_scales + t * 4)) = bits;
                MEM_W(0, (int32_t)(new_cloud_heights + t * 4)) = 0x41700000;  // 15.0, as most stock dioramas
            }
        }
        if (menu_has_mods) {
            uint32_t m = mod_strings;
            for (size_t i = 0; i < mod_ok.size(); i++, m += 32) {
                if (!mod_ok[i]) continue;
                int t = mod_id(int(i));
                // The app's SF Rush arcade versions show their course's own diorama and banner, as the Dreamcast
                // versions do (only one version of a course is on the carousel): the arcade data has no picture of
                // its own, and SF Rush's dioramas are models of the courses.
                int g = group_of[size_t(t)];
                int course = g >= 0 ? version_groups[size_t(g)].members[0] : -1;
                if (mod_arcade[i].load() == 2 && menu_has_rush1 && course >= r1_first && course < r1_end) {
                    for (uint32_t table : { new_diorama_names, new_logo_names, new_diorama_scales, new_cloud_heights }) {
                        MEM_W(0, (int32_t)(table + t * 4)) = MEM_W(0, (int32_t)(table + course * 4));
                    }
                    // With the version bands (src/rush1/track1_convert.cpp add_band_model), as the Rush 2049 versions'
                    // rings: red round this one (ice white for a SNOW version, violet for a NIGHT one, green for a
                    // STUNT one: the PlayStation SF Rush's), blue round the course's N64 one. The names go in this entry's own string slots (its
                    // logo name is the course's).
                    std::string model = "R1TRACK" + std::to_string(course - r1_first + 1);
                    const std::string& label = i < mod_version_labels.size() ? mod_version_labels[i] : std::string();
                    write_string(rdram, m, model + (label == "SNOW" ? "S" : label == "NIGHT" ? "K" : label == "STUNT" ? "T" : "A"));
                    MEM_W(0, (int32_t)(new_diorama_names + t * 4)) = m;
                    write_string(rdram, m + 16, model + "N");
                    MEM_W(0, (int32_t)(new_diorama_names + course * 4)) = m + 16;
                    continue;
                }
                // The author's diorama, or Rush 2049's preview of the route, posed and animated as the 2049 tracks'
                // previews are (rush2_track49_select_pose); either is streamed in (stream_diorama).
                write_string(rdram, m, rush2::track_mod::diorama_model_name(int(i)));
                MEM_W(0, (int32_t)(new_diorama_names + t * 4)) = m;
                if (mod_arts[i].route_preview) preview_names[t] = m;
                // The stock dioramas show about 800 model units on their longer side (VEGASTRACK 1000 x 0.8,
                // HAWAIITRACK 1161 x 0.7, CRASHTRACK 1283 x 0.6); the converter makes an authored one 1200.
                MEM_W(0, (int32_t)(new_diorama_scales + t * 4)) = 0x3F2AAAAB;    // 0.667
                write_string(rdram, m + 16, rush2::track_mod::logo_texture_name(int(i)));
                MEM_W(0, (int32_t)(new_logo_names + t * 4)) = m + 16;
                MEM_W(0, (int32_t)(new_cloud_heights + t * 4)) = 0x41700000;  // 15.0, as most stock dioramas
            }
        }
        if (!available() || !menu_has_2049) {
            return;
        }
        std::vector<std::pair<int, int>> entries; // (menu id, convert_track's k)
        for (int k = 1; k <= track_count; k++) entries.push_back({ first_menu_id + k - 1, k });
        for (int n = 0; n < stunt_count; n++) entries.push_back({ stunt_menu_id + n, stunt_first + n });
        entries.push_back({ obstacle_menu_id, obstacle });
        for (int n = 0; n < battle_count; n++) entries.push_back({ battle_menu_id + n, battle_first + n });
        uint32_t p = preview_strings;
        for (auto [t, k] : entries) {
            write_string(rdram, p, menu_preview_name(k));
            preview_names[t] = p;
            p += 16;
            std::string model = menu_model_name(k), logo = menu_logo_name(k);
            write_string(rdram, s, model);
            MEM_W(0, (int32_t)(new_diorama_names + t * 4)) = s;
            s += 16;
            write_string(rdram, s, logo);
            MEM_W(0, (int32_t)(new_logo_names + t * 4)) = s;
            s += 16;
            MEM_W(0, (int32_t)(new_diorama_scales + t * 4)) = 0x3F0CCCCD; // 0.55: the miniatures are larger than the stock models
            MEM_W(0, (int32_t)(new_cloud_heights + t * 4)) = 0x41700000;  // 15.0, as most stock dioramas
        }
        // The Dreamcast versions show the course's art (only one version of a course is on the carousel).
        for (int k = 1; k <= track_count; k++) {
            int from = first_menu_id + k - 1, t = disc_menu_id + k - 1;
            for (uint32_t table : { new_diorama_names, new_logo_names, new_diorama_scales, new_cloud_heights }) {
                MEM_W(0, (int32_t)(table + t * 4)) = MEM_W(0, (int32_t)(table + from * 4));
            }
            preview_names[t] = preview_names[from];
            // Their own preview when the container has it: the disc's picture with the Dreamcast version ring.
            if (menu_has_disc) {
                uint32_t name = disc_preview_strings + uint32_t(k - 1) * 16;
                write_string(rdram, name, disc_preview_name(k));
                preview_names[t] = name;
                uint32_t model = disc_model_strings + uint32_t(k - 1) * 16;
                write_string(rdram, model, disc_model_name(k));
                MEM_W(0, (int32_t)(new_diorama_names + t * 4)) = model;
            }
        }
    }

    // The order the track select lists its entries in: every id 0-254 once. Ids stay what saves and races use; only the
    // carousel follows this order (rush2_track49_select_count, _carousel_order, _step). Custom tracks with a track.toml
    // [menu] after are moved behind the last entry of that game on their own select (race, stunt or battle), or behind
    // the custom track of that folder; several behind one anchor keep the mod order. The others go by game, then id.
    std::vector<int> display_order;

    bool anchor_game(const std::string& name, rush2::game_filter::Game& out) {
        using rush2::game_filter::Game;
        if (name == "rush2") out = Game::Rush2;
        else if (name == "sfrush") out = Game::SFRush;
        else if (name == "rush2049") out = Game::Rush2049;
        else if (name == "calspeed") out = Game::CalSpeed;
        else return false;
        return true;
    }

    void build_order(const std::vector<rush2::track_mod::Entry>& entries) {
        display_order.clear();
        for (int t = 0; t < menu_tracks; t++) display_order.push_back(t);
        // Grouped by game (rush2::game_filter::display_rank: SF Rush, Rush 2, Rush 2049, California Speed, custom), id
        // order within a game. The app's arcade and PlayStation tracks count as their game's (mod_arcade, set first).
        std::stable_sort(display_order.begin(), display_order.end(), [](int a, int b) {
            return rush2::game_filter::display_rank(rush2::game_filter::track_game(a)) <
                   rush2::game_filter::display_rank(rush2::game_filter::track_game(b));
        });
        size_t n = std::min(entries.size(), size_t(mod_capacity));
        std::vector<bool> placed(n);
        for (size_t i = 0; i < n; i++) placed[i] = entries[i].settings.menu_after.empty();
        std::map<int, int> behind;      // anchor -> the custom track last put behind it
        auto place = [&](int self, int anchor) {
            display_order.erase(std::find(display_order.begin(), display_order.end(), self));
            int after = behind.count(anchor) ? behind[anchor] : anchor;
            display_order.insert(std::find(display_order.begin(), display_order.end(), after) + 1, self);
            behind[anchor] = self;
        };
        // An anchor on another custom track waits for that one to be placed (a loop of them stays put).
        for (size_t pass = 0; pass <= n; pass++) {
            for (size_t i = 0; i < n; i++) {
                if (placed[i]) continue;
                const std::string& after = entries[i].settings.menu_after;
                int self = mod_id(int(i));
                rush2::game_filter::Game game;
                int anchor = -1;
                if (anchor_game(after, game)) {
                    for (int t : display_order) {
                        // A course's other versions (the Dreamcast ones, 249-254) don't count: they move next to
                        // their course below, so the last of the game is the last course (Presidio for "rush2049").
                        int g = group_of[size_t(t)];
                        if (g >= 0 && version_groups[size_t(g)].members[0] != t) continue;
                        if (!is_mod_track(t) && rush2::game_filter::track_game(t) == game && kind_of(t) == kind_of(self)) {
                            anchor = t;
                        }
                    }
                }
                else {
                    size_t j = 0;
                    while (j < n && entries[j].folder != after) j++;
                    if (j < n && j != i && !placed[j]) continue;
                    if (j < n && j != i) anchor = mod_id(int(j));
                }
                placed[i] = true;
                if (anchor < 0) {
                    printf("[trackmod] %s: [menu] after = \"%s\" names no game (rush2, sfrush, rush2049, calspeed) or "
                           "custom track; it stays at the end\n", entries[i].folder.c_str(), after.c_str());
                    continue;
                }
                place(self, anchor);
            }
        }
        // A course's other versions right after it, so the one shown takes the course's place.
        for (const VersionGroup& group : version_groups) {
            int previous = group.members[0];
            for (size_t m = 1; m < group.members.size(); m++) {
                int t = group.members[m];
                display_order.erase(std::find(display_order.begin(), display_order.end(), t));
                display_order.insert(std::find(display_order.begin(), display_order.end(), previous) + 1, t);
                previous = t;
            }
        }
    }

    // The tracks the carousel shows, in display order.
    std::vector<int> shown_tracks(uint8_t* rdram) {
        std::vector<int> shown;
        for (int t : display_order) {
            if (track_selectable(rdram, t)) shown.push_back(t);
        }
        return shown;
    }

    // The enabled mods' custom tracks: converted (or read from the cache) when the enabled mods change, for their track
    // select art. A track that doesn't convert isn't offered; its report (<config>/track_reports) says why.
    void refresh_mods() {
        std::vector<rush2::track_mod::Entry> entries = rush2::track_mod::tracks();
        std::string signature;
        for (const auto& e : entries) {
            signature += e.mod_id + "/" + e.folder + ">" + e.settings.menu_after + "~" + e.settings.variant_of + "=" +
                         e.settings.variant + ";";
        }
        bool reload = mods_reload.exchange(false);
        set_mod_keys(entries);
        if (signature == mod_signature && !display_order.empty() && !reload) return;
        if (reload) {
            menu_container_built = false;   // the art is built again from the new files
            std::lock_guard lock{ prebuilt_mutex };
            prebuilt.reset();
            menu_reloads++;
        }
        mod_signature = signature;
        mod_ok.clear();
        mod_arts.clear();
        mod_types.clear();
        mod_clocks.clear();
        for (size_t i = 0; i < size_t(mod_capacity); i++) {
            const std::string id = i < entries.size() ? entries[i].mod_id : std::string();
            // The PlayStation SF Rush mod's track (its track 4, Chinatown) is SF Rush content too.
            mod_arcade[i] = id == rush2::arcade2049::track_mod_id ? 1
                            : id == rush2::arcade2049::sfrush_track_mod_id || id == rush2::sfrush_ps1::mod_id ? 2
                                                                                                         : 0;
        }
        for (size_t i = 0; i < entries.size() && i < size_t(mod_capacity); i++) {
            mod_types.push_back(rush2::track_mod::type_of(entries[i].settings));
            mod_clocks.push_back(rush2::time_sprint::has_clocks(entries[i].mod_id, entries[i].folder));
        }
        build_versions(entries);
        build_order(entries);
        if (entries.size() > size_t(mod_capacity)) {
            printf("[trackmod] The track select has room for %d custom tracks; %zu are left out\n", mod_capacity,
                   entries.size() - size_t(mod_capacity));
        }
        for (size_t i = 0; i < entries.size() && i < size_t(mod_capacity); i++) {
            rush2::track_mod::MenuArt art;
            mod_ok.push_back(rush2::track_mod::menu_art(int(i), art));
            // The logo drawn in full color, as the 2049 banners are (rush2::upscale::add_exact_image).
            if (mod_ok.back()) {
                rush2::upscale::Image image;
                image.width = 128;
                image.height = 32;
                image.rgba = art.logo_rgba;
                rush2::upscale::add_exact_image(art.logo, std::move(image));
            }
            mod_arts.push_back(std::move(art));
        }
        rush2::track_mod::publish_textures();   // the dioramas' (and tracks') full-size textures
    }

    // Builds it from the ROMs and the custom tracks' art (any thread: Rush 2's own asset 3 comes from the ROM).
    MenuBuild build_menu(std::shared_ptr<const rush2::rom2049::Source> rom, std::shared_ptr<const std::vector<uint8_t>> rom1,
                         std::shared_ptr<const std::vector<uint8_t>> rom_cs, const std::string& mods_signature,
                         const std::vector<bool>& ok, const std::vector<rush2::track_mod::MenuArt>& arts,
                         std::shared_ptr<const rush2::rom2049::Source> disc, const std::vector<int>& courses) {
        MenuBuild b{ rom, rom1, rom_cs, mods_signature, disc };
        std::vector<uint8_t> asset3, with_2049, with_rush1;
        if (!rush2::assets::rom_original(3, asset3)) {
            printf("[2049] Failed to read the track select art\n");
            return b;
        }
        if (rom != nullptr && !build_menu_container(asset3, *rom, with_2049)) {
            printf("[2049] Failed to build the track select art\n");
            with_2049.clear();
        }
        b.has_2049 = !with_2049.empty();
        const std::vector<uint8_t>& base = with_2049.empty() ? asset3 : with_2049;
        if (rom1 != nullptr && rush2::track1::extend_menu_container(base, *rom1, with_rush1)) {
            b.has_rush1 = true;
        }
        const std::vector<uint8_t>& base2 = b.has_rush1 ? with_rush1 : base;
        b.shop = base2;
        std::vector<MenuModel> models;
        std::vector<MenuLogo> logos;
        b.streamed.resize(std::min(ok.size(), arts.size()));
        for (size_t i = 0; i < ok.size() && i < arts.size(); i++) {
            if (!ok[i]) continue;
            const rush2::track_mod::MenuArt& art = arts[i];
            MenuModel model{ rush2::track_mod::diorama_model_name(int(i)), art.diorama, art.diorama_list,
                             art.diorama_pointers, std::max(art.diorama_radius, 1.0f) };
            // A version of a Rush 2049 race track (the arcade mod's) shows the course's own route and color, as its
            // N64 and Dreamcast versions do, with its own picture and the arcade's version ring; the route of its AI
            // path stays for a course the 2049 source can't give.
            if (rom != nullptr && art.route_preview && i < courses.size() && courses[i] > 0) {
                MenuModel course{ model.name };
                if (course_preview(*rom, courses[i], &art.diorama, art.diorama_list, version_ring_arcade, course.blob,
                                   course.list, course.pointers, course.radius)) {
                    course.radius = std::max(course.radius, 1.0f);
                    model = std::move(course);
                }
            }
            // The diorama is streamed in while the track is near the carousel's shown entry (stream_diorama): the
            // container holds a stub of the same name, an empty list with the model's radius.
            MenuModel stub{ model.name, std::vector<uint8_t>(stub_bytes, 0), 0, {}, model.radius };
            stub.blob[0] = 0xDF;
            if (!model.blob.empty()) b.streamed[i] = std::move(model);
            models.push_back(std::move(stub));
            logos.push_back({ rush2::track_mod::logo_texture_name(int(i)), art.logo, art.logo_palette });
        }
        bool with_mods = !models.empty();
        // The Dreamcast versions' previews (the disc's picture and the Dreamcast ring), with their spare buffers lent
        // from the pool as the custom tracks' are, so they cost the heap no more than the screenshot and the tube.
        bool with_disc = false;
        if (b.has_2049 && disc != nullptr) {
            std::vector<MenuModel> disc_models;
            for (int k = 1; k <= track_count; k++) {
                MenuModel model{ disc_preview_name(k) };
                if (!course_preview(*disc, k, nullptr, 0, version_ring_dc, model.blob, model.list, model.pointers,
                                    model.radius)) {
                    printf("[2049] The Dreamcast version of track %d has no track select preview of its own\n", k);
                    disc_models.clear();
                    break;
                }
                model.radius = std::max(model.radius, 1.0f);
                // Its diorama (the circuit screen's): the disc's route with the Dreamcast ring.
                MenuModel diorama{ disc_model_name(k) };
                if (!course_diorama(*disc, k, version_ring_dc, diorama.blob, diorama.list, diorama.pointers,
                                    diorama.radius)) {
                    printf("[2049] The Dreamcast version of track %d has no diorama of its own\n", k);
                    disc_models.clear();
                    break;
                }
                diorama.radius = std::max(diorama.radius, 1.0f);
                disc_models.push_back(std::move(diorama));
                if (size_t room = preview_room(model.blob, model.list); room != 0) {
                    model.blob.resize(room);
                    uint32_t pooled = preview_pooled_tag;
                    for (int i = 0; i < 4; i++) model.blob[room - 16 + size_t(i)] = uint8_t(pooled >> (24 - 8 * i));
                }
                disc_models.push_back(std::move(model));
            }
            with_disc = !disc_models.empty();
            models.insert(models.end(), disc_models.begin(), disc_models.end());
        }
        if (rom_cs != nullptr) {
            std::vector<MenuModel> cs_models;
            std::vector<MenuLogo> cs_logos;
            if (rush2::calspeed::menu_art(*rom_cs, cs_models, cs_logos)) {
                models.insert(models.end(), cs_models.begin(), cs_models.end());
                logos.insert(logos.end(), cs_logos.begin(), cs_logos.end());
            }
        }
        if (!models.empty() || !logos.empty()) {
            if (extend_menu_container(base2, models, logos, b.container)) {
                b.has_mods = with_mods;
                b.has_cs = rom_cs != nullptr;
                b.has_disc = with_disc;
            }
            else {
                printf("[trackmod] Couldn't add the custom, Dreamcast version and Cal Speed tracks' track select art\n");
                b.container.clear();
            }
        }
        if (b.container.empty() && (b.has_2049 || b.has_rush1)) {
            b.container = base2;
        }
        return b;
    }

    // Asset 3 with the 2049, Rush 1, custom and Cal Speed tracks' dioramas and logos, rebuilt when a ROM, option or mod
    // changes.
    // Whether the container in hand was built from the ROMs, mods and disc in use now.
    bool menu_container_matches() {
        auto rom = available() ? rush2::wings::get_rom() : nullptr;
        auto rom1 = rush2::track1::available() ? rush2::track1::get_rom() : nullptr;
        auto rom_cs = rush2::calspeed::available() ? rush2::calspeed::get_rom() : nullptr;
        auto disc = rom != nullptr ? disc_versions_source() : nullptr;
        return menu_container_built && menu_container_rom == rom && menu_container_rom1 == rom1 &&
               menu_container_rom_cs == rom_cs && menu_container_mods == mod_signature && menu_container_disc == disc;
    }

    void serve_menu_container(uint8_t* rdram) {
        auto rom = available() ? rush2::wings::get_rom() : nullptr;
        auto rom1 = rush2::track1::available() ? rush2::track1::get_rom() : nullptr;
        auto rom_cs = rush2::calspeed::available() ? rush2::calspeed::get_rom() : nullptr;
        auto disc = rom != nullptr ? disc_versions_source() : nullptr;
        bool mods = std::find(mod_ok.begin(), mod_ok.end(), true) != mod_ok.end();
        if (rom == nullptr && rom1 == nullptr && rom_cs == nullptr && !mods) {
            rush2::assets::restore(rdram, 3);
            menu_has_2049 = menu_has_rush1 = menu_has_mods = menu_has_cs = menu_has_disc = false;
            shop_container.clear();
            shop_served = false;
            return;
        }
        bool same = menu_container_matches();
        if (same && !menu_container.empty() && rush2::assets::is_replaced(3) && !shop_served) {
            return;
        }
        shop_served = false;
        if (!same) {
            menu_container_rom = rom;
            menu_container_rom1 = rom1;
            menu_container_rom_cs = rom_cs;
            menu_container_mods = mod_signature;
            menu_container_disc = disc;
            menu_container_built = true;
            // Built ahead by the background builds (prepare_menu_container) when nothing changed since; built here
            // only after a change they haven't caught up with.
            std::optional<MenuBuild> ready;
            {
                std::lock_guard lock{ prebuilt_mutex };
                if (prebuilt && prebuilt->rom == rom && prebuilt->rom1 == rom1 && prebuilt->rom_cs == rom_cs &&
                    prebuilt->mods == mod_signature && prebuilt->disc == disc) {
                    ready = std::move(prebuilt);
                    prebuilt.reset();
                }
            }
            MenuBuild build = ready ? std::move(*ready) : build_menu(rom, rom1, rom_cs, mod_signature, mod_ok, mod_arts,
                                                                   disc, mod_courses());
            menu_container = std::move(build.container);
            shop_container = std::move(build.shop);
            stream_forget_all();
            stream_models = std::move(build.streamed);
            menu_has_2049 = build.has_2049;
            menu_has_rush1 = build.has_rush1;
            menu_has_mods = build.has_mods;
            menu_has_cs = build.has_cs;
            menu_has_disc = build.has_disc;
        }
        if (menu_container.empty()) {
            rush2::assets::restore(rdram, 3);
            menu_has_2049 = menu_has_rush1 = menu_has_mods = menu_has_cs = menu_has_disc = false;
        }
        else {
            rush2::assets::replace(rdram, 3, menu_container);
        }
    }

    std::mutex catalog_menu_mutex;
    std::shared_ptr<const rush2::rom2049::Source> catalog_menu_rom;
    std::shared_ptr<const std::vector<uint8_t>> catalog_menu_rom1;
    std::shared_ptr<const std::vector<uint8_t>> catalog_menu_rom_cs;
    std::vector<uint8_t> catalog_menu;

    // Turns an added track's menu id into its host slot for the race.
    void enter_race(uint8_t* rdram) {
        int t = MEM_BU(0, (int32_t)track_id);
        if ((t >= first_menu_id && t < mod_menu_id) || is_disc_course(t)) {
            rush2::track_mod::set_race_track(-1);
        }
        if (t >= first_menu_id && t < menu_tracks && (t < cs_first || t >= cs_end)) {
            rush2::calspeed::set_race_track(0);
        }
        if (t >= cs_first && t < cs_end) {
            set_race_track(0);
            rush2::track1::set_race_track(0);
            set_stunt_arena(0);
            set_battle_arena(0);
            rush2::track_mod::set_race_track(-1);
            rush2::calspeed::set_race_track(t - cs_first + 1);
            MEM_B(0, (int32_t)track_id) = host_slot;
        }
        else if (is_mod_track(t)) {
            set_race_track(0);
            rush2::track1::set_race_track(0);
            set_stunt_arena(0);
            set_battle_arena(0);
            rush2::track_mod::set_race_track(mod_index(t));
            // A custom arena races in STUNT1's slot (stunt mode), as Rush 2049's arenas.
            MEM_B(0, (int32_t)track_id) = is_arena(t) ? stunt_host_slot : host_slot;
        }
        else if (is_disc_course(t)) {
            set_race_track(t - disc_menu_id + 1, true);
            rush2::track1::set_race_track(0);
            set_stunt_arena(0);
            set_battle_arena(0);
            MEM_B(0, (int32_t)track_id) = host_slot;
        }
        else if (t >= first_menu_id && t < r1_first) {
            set_race_track(t - first_menu_id + 1);
            rush2::track1::set_race_track(0);
            set_stunt_arena(0);
            set_battle_arena(0);
            MEM_B(0, (int32_t)track_id) = host_slot;
        }
        else if (t >= r1_first && t < r1_end) {
            set_race_track(0);
            rush2::track1::set_race_track(t - r1_first + 1);
            set_stunt_arena(0);
            set_battle_arena(0);
            MEM_B(0, (int32_t)track_id) = host_slot;
        }
        else if (is_battle_arena(t)) {
            set_race_track(0);
            rush2::track1::set_race_track(0);
            set_stunt_arena(0);
            set_battle_arena(t - battle_menu_id + 1);
            MEM_B(0, (int32_t)track_id) = stunt_host_slot;
        }
        else if (is_arena(t)) {
            set_race_track(0);
            rush2::track1::set_race_track(0);
            set_stunt_arena(t - stunt_menu_id + 1);
            set_battle_arena(0);
            MEM_B(0, (int32_t)track_id) = stunt_host_slot;
        }
        else if (t == obstacle_menu_id) {
            set_race_track(obstacle);
            rush2::track1::set_race_track(0);
            set_stunt_arena(0);
            set_battle_arena(0);
            MEM_B(0, (int32_t)track_id) = host_slot;
        }
        else {
            // Race start runs this again on the host slot the first call chose: a custom track stays in HAWAII, a
            // custom arena in STUNT1 (the track select's start clears it, so a stock STUNT1 race is never one).
            bool mod_arena = t == stunt_host_slot && rush2::track_mod::race_track() >= 0 &&
                             rush2::track_mod::race_type() != rush2::track_mod::race_race;
            if (t != host_slot && !mod_arena) {
                set_race_track(0);
                rush2::track1::set_race_track(0);
                rush2::track_mod::set_race_track(-1);
                rush2::calspeed::set_race_track(0);
            }
            if (t != stunt_host_slot) {
                set_stunt_arena(0);
                set_battle_arena(0);
            }
        }
    }
}

// func_803AB294 at 0x803AB5C0, once per track select visit: the initial track is chosen and the screen's assets are
// about to load. $s0 = &track id.
bool rush2::track2049::catalog_menu_container(std::vector<uint8_t>& out) {
    auto rom = available() ? rush2::wings::get_rom() : nullptr;
    auto rom1 = rush2::track1::available() ? rush2::track1::get_rom() : nullptr;
    auto rom_cs = rush2::calspeed::available() ? rush2::calspeed::get_rom() : nullptr;
    if (rom == nullptr && rom1 == nullptr && rom_cs == nullptr) return false;
    std::lock_guard lock{ catalog_menu_mutex };
    if (catalog_menu.empty() || catalog_menu_rom != rom || catalog_menu_rom1 != rom1 || catalog_menu_rom_cs != rom_cs) {
        // As serve_menu_container builds it, into a copy of our own (the game's may be in use).
        catalog_menu.clear();
        catalog_menu_rom = rom;
        catalog_menu_rom1 = rom1;
        catalog_menu_rom_cs = rom_cs;
        uint8_t* rdram = rush2::assets::rdram();
        std::vector<uint8_t> asset3, with_2049, with_rush1;
        if (rdram == nullptr || !rush2::assets::read_original(rdram, 3, asset3)) return false;
        if (rom != nullptr && !build_menu_container(asset3, *rom, with_2049)) with_2049.clear();
        const std::vector<uint8_t>& base = with_2049.empty() ? asset3 : with_2049;
        if (rom1 == nullptr || !rush2::track1::extend_menu_container(base, *rom1, with_rush1)) with_rush1 = with_2049;
        catalog_menu = with_rush1;
        std::vector<MenuModel> cs_models;
        std::vector<MenuLogo> cs_logos;
        if (rom_cs != nullptr && rush2::calspeed::menu_art(*rom_cs, cs_models, cs_logos)) {
            const std::vector<uint8_t>& base2 = with_rush1.empty() ? asset3 : with_rush1;
            if (!extend_menu_container(base2, cs_models, cs_logos, catalog_menu)) catalog_menu = with_rush1;
        }
    }
    out = catalog_menu;
    return !out.empty();
}

void rush2::track2049::prepare_menu_container() {
    auto rom = available() ? rush2::wings::get_rom() : nullptr;
    auto rom1 = rush2::track1::available() ? rush2::track1::get_rom() : nullptr;
    auto rom_cs = rush2::calspeed::available() ? rush2::calspeed::get_rom() : nullptr;
    auto disc = rom != nullptr ? disc_versions_source() : nullptr;
    std::vector<bool> ok;
    std::vector<rush2::track_mod::MenuArt> arts;
    std::vector<int> courses;
    std::string signature;
    int reloads;
    {
        std::lock_guard lock{ menu_mutex };
        refresh_mods();
        bool same = menu_container_built && menu_container_rom == rom && menu_container_rom1 == rom1 &&
                    menu_container_rom_cs == rom_cs && menu_container_mods == mod_signature &&
                    menu_container_disc == disc;
        if (same) return;       // the game has this one
        ok = mod_ok;
        arts = mod_arts;
        courses = mod_courses();
        signature = mod_signature;
        std::lock_guard pre_lock{ prebuilt_mutex };
        reloads = menu_reloads;
    }
    {
        std::lock_guard lock{ prebuilt_mutex };
        if (prebuilt && prebuilt->rom == rom && prebuilt->rom1 == rom1 && prebuilt->rom_cs == rom_cs &&
            prebuilt->mods == signature && prebuilt->disc == disc) {
            return;
        }
    }
    MenuBuild b = build_menu(rom, rom1, rom_cs, signature, ok, arts, disc, courses);
    std::lock_guard lock{ prebuilt_mutex };
    if (reloads == menu_reloads) prebuilt = std::move(b);
}

void rush2::track2049::prepare_menu_art(uint8_t* rdram) {
    std::lock_guard lock{ menu_mutex };
    refresh_mods();
    // The shop calls this for each track it shows while its scene is built: once its art is in place (and was built
    // from the ROMs and mods in use), the tables are written and there's nothing to install again.
    if (shop_served && menu_container_matches()) return;
    serve_menu_container(rdram);
    write_tables(rdram);
    // The shop's own container (shop_container), until the track select or circuit screen serves the full one again.
    if (!shop_container.empty()) {
        rush2::assets::replace(rdram, 3, shop_container);
        shop_served = true;
    }
}

std::string rush2::track2049::shop_diorama_name(uint8_t* rdram, int t) {
    // An SF Rush course with an arcade version names its banded copy, which only the full container holds.
    if (shop_served && t >= r1_first && t < r1_end) return "R1TRACK" + std::to_string(t - r1_first + 1);
    std::string name;
    uint32_t at = diorama_name(rdram, t);
    for (int i = 0; i < 31 && rush2::in_game_ram(at + uint32_t(i)) && MEM_B(i, (int32_t)at) != 0; i++) {
        name += (char)MEM_B(i, (int32_t)at);
    }
    return name;
}

extern "C" uint32_t rush2_interp_side_dl(uint8_t* rdram, const GfxCommand* cmds, uint32_t count);

uint32_t rush2::track2049::extended_model_list(uint8_t* rdram, uint32_t model_list, uint32_t call) {
    if (model_list < 0x81000000u || model_list - 0x80000000u >= recomp::mem_size) return call;
    GfxCommand cmds[3];
    gEXSetRDRAMExtended(&cmds[0], 1);
    cmds[1].values.word0 = 0xDE000000;     // G_DL, pushed
    cmds[1].values.word1 = call;
    gEXSetRDRAMExtended(&cmds[2], 0);
    return rush2_interp_side_dl(rdram, cmds, 3);
}

bool rush2::track2049::mod_from_2049(int t) {
    int i = mod_index(t);
    return i >= 0 && i < mod_capacity && mod_arcade[size_t(i)].load() == 1;
}

bool rush2::track2049::mod_from_sfrush(int t) {
    int i = mod_index(t);
    return i >= 0 && i < mod_capacity && mod_arcade[size_t(i)].load() == 2;
}

std::vector<int> rush2::track2049::course_versions(int t) {
    // The track select builds the groups when it reads the mods; a race started some other way (a test) builds them now.
    if (version_groups.empty()) build_versions(rush2::track_mod::tracks());
    if (t < 0 || t >= menu_tracks || group_of[size_t(t)] < 0) return { t };
    return version_groups[size_t(group_of[size_t(t)])].members;
}

uint32_t rush2::track2049::diorama_name(uint8_t* rdram, int t) {
    return (uint32_t)MEM_W(0, (int32_t)(new_diorama_names + t * 4));
}

float rush2::track2049::diorama_scale(uint8_t* rdram, int t) {
    uint32_t bits = (uint32_t)MEM_W(0, (int32_t)(new_diorama_scales + t * 4));
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

extern "C" void rush2_track49_select_init(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ menu_mutex };
    refresh_mods();     // the custom tracks' types, which say which select a saved choice belongs to
    load_selection();
    apply_race_battle(rdram);
    set_race_track(0);
    rush2::track1::set_race_track(0);
    set_stunt_arena(0);
    set_battle_arena(0);
    rush2::track_mod::set_race_track(-1);
    rush2::calspeed::set_race_track(0);
    restore_host(rdram);
    rush2::track1::restore_host(rdram);
    serve_menu_container(rdram);
    write_tables(rdram);
    update_filter(rdram);
    offered_rebuild = false;
    int t = -1;
    // A saved choice that is a course's other version stands for the course: the version shown now.
    if (select_kind == select_stunt) {
        // The game's choice (from the save record's nibble) is a race track, which this select doesn't offer.
        int c = stunt_selection >= 0 ? shown_of(rdram, stunt_selection) : -1;
        t = c >= 0 && track_selectable(rdram, c) ? c : stunt_host_slot;
    }
    else if (select_kind == select_battle) {
        int c = battle_selection >= 0 ? shown_of(rdram, battle_selection) : -1;
        t = c >= 0 && track_selectable(rdram, c) ? c : battle_menu_id;
    }
    else if (selection >= 0 && MEM_W(0, (int32_t)game_mode) != 1) {
        int c = shown_of(rdram, first_menu_id + selection);
        if (track_selectable(rdram, c)) t = c;
    }
    // Test aid: RUSH2_TEST_TRACK=<track select id> opens the screen on that track, whatever the save chose (a course's
    // other version is picked for this visit). Not on a rebuild (filter_keep_track), which keeps the track shown.
    static const char* test_track = std::getenv("RUSH2_TEST_TRACK");
    if (test_track != nullptr && filter_keep_track < 0) {
        int want = std::atoi(test_track);
        int g = want >= 0 && want < menu_tracks ? group_of[size_t(want)] : -1;
        if (g >= 0 && member_ok(rdram, want)) version_picks[version_groups[size_t(g)].key] = version_label(want);
        if (track_selectable(rdram, want)) t = want;
    }
    // RUSH2_TEST_MOD_FOLDER=<folder> does the same for the custom track of that folder (tools/trackview's AI runs).
    static const char* test_folder = std::getenv("RUSH2_TEST_MOD_FOLDER");
    if (test_folder != nullptr && filter_keep_track < 0) {
        std::vector<rush2::track_mod::Entry> mods = rush2::track_mod::tracks();
        for (size_t i = 0; i < mods.size(); i++) {
            if (i < size_t(mod_capacity) && mods[i].folder == test_folder && track_selectable(rdram, mod_id(int(i)))) {
                t = mod_id(int(i));
            }
        }
    }
    // The game's own choice of a course's other version: the version shown. After the game filter or the version
    // button rebuilt the screen, the track to show; a choice the filter hides gives the first track it offers.
    {
        int chosen = shown_of(rdram, t >= 0 ? t : MEM_BU(0, (int32_t)track_id));
        if (filter_keep_track >= 0 && track_selectable(rdram, filter_keep_track)) chosen = filter_keep_track;
        if (filter_present != 0 && filter_on) {
            for (int i = 0; i < menu_tracks && !track_selectable(rdram, chosen); i++) {
                if (track_selectable(rdram, i)) chosen = i;
            }
        }
        if (chosen != MEM_BU(0, (int32_t)track_id)) t = chosen;
    }
    filter_keep_track = -1;
    if (t >= 0) {
        MEM_B(0, (int32_t)track_id) = uint8_t(t);
        MEM_H(0, (int32_t)shown_track) = int16_t(t);
    }
    stock_options.clear();
    for (int row = 0; row < option_list_count(rdram) && row < 11; row++) {
        stock_options.push_back((int32_t)MEM_W(0, (int32_t)(option_rows + row * 4)));
    }
    filter_options(rdram);
}

// func_803ABE0C at 0x803ABEDC, before the screen sets itself up for the frame (func_803AB294(1)): the game filter's
// prompt and panel (src/game_filter.cpp). A changed filter tears the screen down and skips the rest of the frame (goto
// its end); the next frame builds it again, on the track that was shown.
extern "C" void trackselect_init_draw_803AB294(uint8_t* rdram, recomp_context* ctx);
extern "C" int rush2_track49_filter_frame(uint8_t* rdram, recomp_context* ctx) {
    // An online joiner's carousel holds the host's offered tracks (set_online_offered): a change rebuilds it the same way.
    bool offered_changed;
    int switched = -1;
    {
        std::lock_guard lock{ menu_mutex };
        offered_changed = std::exchange(offered_rebuild, false);
        // The version button: the course's next version, picked (and saved) and shown.
        constexpr uint32_t menu_pressed = 0x8011962A;   // u16: the menus' presses this frame
        constexpr uint16_t c_left = 0x0002;
        if (!offered_changed && !rush2::game_filter::input_captured() &&
            (MEM_HU(0, (int32_t)menu_pressed) & c_left) != 0) {
            int t = MEM_BU(0, (int32_t)track_id);
            switched = next_version(rdram, t);
            if (switched >= 0) {
                version_picks[version_groups[size_t(group_of[size_t(t)])].key] = version_label(switched);
                if (select_kind == select_stunt) stunt_selection = switched;
                else if (select_kind == select_battle) battle_selection = switched;
                else selection = switched - first_menu_id;
                write_selection();
            }
        }
    }
    if (switched >= 0) {
        constexpr int sound_change = 0x49;
        recomp_context saved = *ctx;
        ctx->r4 = sound_change;
        menu_play_sound_80064908(rdram, ctx);
        *ctx = saved;
        filter_keep_track = switched;
        // The switch swaps one entry for its other version and keeps the rest: the screen is rebuilt in this frame
        // (the game's own func_803AB294(1) after the hook builds it again and draws it), so no frame goes by without
        // the screen, and the slide in progress carries on (rush2_track49_carousel_restore).
        {
            std::lock_guard lock{ menu_mutex };
            int count = MEM_H(0, (int32_t)carousel_count);
            kept_slide.valid = count > 0 && count <= menu_tracks;
            kept_slide.offsets.clear();
            for (int i = 0; kept_slide.valid && i < count; i++) {
                uint32_t entry = new_carousel + i * carousel_entry;
                kept_slide.offsets.push_back(slide_get(rdram, entry + 0x4) - slide_get(rdram, entry + 0x10));
            }
            kept_slide.highlight_x = slide_get(rdram, carousel_highlight_x);
        }
        ctx->r4 = 0;
        trackselect_init_draw_803AB294(rdram, ctx);
        *ctx = saved;
        return 0;
    }
    else if (!offered_changed &&
             !rush2::game_filter::frame(rdram, ctx, rush2::game_filter::Screen::Tracks, filter_present)) {
        return 0;
    }
    else {
        filter_keep_track = MEM_BU(0, (int32_t)track_id);
    }
    recomp_context saved = *ctx;
    ctx->r4 = 0;
    trackselect_init_draw_803AB294(rdram, ctx);
    *ctx = saved;
    return 1;
}

// End of the track select's option text callback (func_803C6268 at 0x803C6940): the game filter's prompt and panel.
extern "C" void rush2_track49_filter_text(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_W(0, (int32_t)game_state) == 1) {
        rush2::game_filter::VersionPrompt version;
        {
            std::lock_guard lock{ menu_mutex };
            int t = MEM_BU(0, (int32_t)track_id);
            if (other_version(rdram, t)) version.label = version_label(t);
        }
        rush2::game_filter::draw(rdram, ctx, rush2::game_filter::Screen::Tracks, filter_present, &version);
    }
}

// func_803AB294 at 0x803AB6C8: $t7 = the number of carousel entries (10 + unlocked PIPE and ATARI), which must be
// the number of tracks the build loop finds selectable.
// The position loop before it has set the carousel position (s16 0x803D05B4) to the track's place in id order; it
// becomes its place in display order (build_order).
extern "C" void rush2_track49_select_count(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ menu_mutex };
    std::vector<int> shown = shown_tracks(rdram);
    ctx->r15 = int(shown.size());
    auto at = std::find(shown.begin(), shown.end(), MEM_BU(0, (int32_t)track_id));
    if (at != shown.end()) MEM_H(0, (int32_t)carousel_position) = int16_t(at - shown.begin());
}

// func_803AB294 at 0x803AB878, the carousel built: its entries hold the tracks in id order, each entry's place (+0x4 -
// +0x17) from its index. The tracks (+0x0) and their model instances (+0x18) are put in display order.
extern "C" void rush2_track49_carousel_order(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ menu_mutex };
    std::vector<int> shown = shown_tracks(rdram);
    int count = std::min<int>(MEM_H(0, (int32_t)carousel_count), int(shown.size()));
    for (int i = 0; i < count; i++) {
        uint32_t entry = new_carousel + i * carousel_entry;
        for (int j = i; j < count; j++) {
            uint32_t other = new_carousel + j * carousel_entry;
            if (MEM_BU(0, (int32_t)other) != shown[i]) continue;
            if (j != i) {
                uint8_t track = MEM_BU(0, (int32_t)entry);
                uint32_t node = (uint32_t)MEM_W(0, (int32_t)(entry + 0x18));
                MEM_B(0, (int32_t)entry) = MEM_BU(0, (int32_t)other);
                MEM_W(0, (int32_t)(entry + 0x18)) = MEM_W(0, (int32_t)(other + 0x18));
                MEM_B(0, (int32_t)other) = track;
                MEM_W(0, (int32_t)(other + 0x18)) = node;
            }
            break;
        }
    }
}

namespace {
    // The last TRACK left / right step's direction ($a1 of func_803ABE0C: -1 or +1), for the carousel slide.
    std::atomic<int> last_step_direction{ -1 };
}

// func_803ABE0C at 0x803AC12C: TRACK left / right moved the carousel position by one (wrapped) and the track id to
// the next selectable id ($a2, at $a3); the track is the one at the new position, in display order.
extern "C" void rush2_track49_select_step(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r5 != 0) last_step_direction = (int32_t)ctx->r5 < 0 ? -1 : 1;
    int count = MEM_H(0, (int32_t)carousel_count);
    int position = MEM_H(0, (int32_t)carousel_position);
    if (position < 0 || position >= count || count > menu_tracks) return;
    uint8_t t = MEM_BU(0, (int32_t)(new_carousel + position * carousel_entry));
    MEM_B(0, (int32_t)ctx->r7) = t;
    ctx->r6 = t;
}

// func_803AB294 at 0x803ABB58, each frame per carousel entry $s0: its slot (place relative to the position, $v0 =
// (index - position) wrapped to [0, count)) changed from the old slot $v1 (+0x14). The game takes the step's direction
// from the slots: $a1 = $v0 - 1 (wrapped) == $v1 means the position went down (left), and the entry's target x (+0x10)
// drops by the spacing; otherwise it rises. With two entries both directions give the same slots, so every step
// slid the same way. There the button's direction decides.
extern "C" void rush2_track49_carousel_slide(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_H(0, (int32_t)carousel_count) != 2) return;
    int old_slot = (int32_t)ctx->r3;
    ctx->r5 = last_step_direction < 0 ? (uint64_t)(int64_t)old_slot : (uint64_t)(int64_t)(old_slot + 1);
}

// func_803AB294 at 0x803AB718, building the carousel: entry $s0's place (index $s3 - position, $t6) was just stored at
// +0x14 unwrapped; the game then read it back signed (lb) and added the count ($s5) when negative. Places are read
// unsigned now (us.toml: lbu here and at 0x803ABB30/0x803ABB38), as a carousel of more than 128 tracks has places past
// 127, so the place is wrapped to [0, count) here and the game's own wrap is skipped.
extern "C" void rush2_track49_carousel_place(uint8_t* rdram, recomp_context* ctx) {
    int place = (int32_t)ctx->r14;
    int count = (int16_t)MEM_H(0, (int32_t)ctx->r21);
    if (place < 0 && count > 0) place += count;
    MEM_B(0x14, (int32_t)ctx->r16) = (int8_t)place;
}

// func_803AB294 at 0x803AB9B8, the screen built (entries at their targets, the highlight at its track's x), before the
// first frame's slide: after a version switch the slide that was going on carries on (kept_slide, saved by
// rush2_track49_filter_frame). The switch keeps the carousel's entries and order, swapping one for its other version.
extern "C" void rush2_track49_carousel_restore(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ menu_mutex };
    if (!std::exchange(kept_slide.valid, false)) return;
    int count = MEM_H(0, (int32_t)carousel_count);
    if (count != int(kept_slide.offsets.size())) return;
    for (int i = 0; i < count; i++) {
        uint32_t entry = new_carousel + i * carousel_entry;
        slide_set(rdram, entry + 0x4, slide_get(rdram, entry + 0x10) + kept_slide.offsets[size_t(i)]);
    }
    slide_set(rdram, carousel_highlight_x, kept_slide.highlight_x);
}

// Rush 2049's own preview for the 2049 entries (docs/rush2049_research/menus.md §7.5): the carousel instances the
// track's preview model (src/rush2049/track2049_art.cpp, menu_preview_name) in place of its diorama, and each frame the preview
// is posed so its model space, Rush 2049's track select camera space, lands in front of Rush 2's camera with 2049's
// screen layout moved to the middle of the diorama area: the screenshot centred between the carousel arrows, the model
// in front of it as 2049 shows it, still (no carousel spin). Race tracks' tubes get 2049's moving white highlight.
extern "C" void model_find_by_name_8005BE3C(uint8_t* rdram, recomp_context* ctx);
extern "C" void asset_find_slot_8008696C(uint8_t* rdram, recomp_context* ctx);           // $a0 asset -> slot or -1

namespace {
    constexpr uint32_t camera = 0x800E79D0;       // view 0's camera: rows -right, up, forward, then +0x24 position
    constexpr uint32_t view = 0x802401F0;         // view 0's projection (func_80054A50): +0x14 tan(hfov / 2),
                                                  // +0x24 width, +0x2C/+0x30 the screen point straight ahead
    constexpr uint32_t node_poses = rush2::scene_nodes::table + 4;   // per scene node, 56 bytes: -> {matrix[9], position[3]}
    constexpr uint32_t model_tables = 0x80118D78; // per model slot, 8 bytes: -> its 0x34-byte model records
    constexpr uint32_t model_slots = 0x800D5788;  // u8: loaded model slots
    constexpr uint32_t frame_seconds = 0x80023028;

    // Where the preview's screenshot is centred on Rush 2's 320 x 240 screen (2049 draws it at (176, 32) - (304, 160)).
    constexpr float preview_disc_center_x = 160.0f, preview_disc_center_y = 108.0f;
    constexpr float preview_depth = 110.0f;       // 2049 camera units: about the preview's middle, put at the diorama's depth

    float preview_phase = 0.0f;                   // the tube highlight's run along the route (0-1, one per 5 s)
    float preview_angle = 0.0f;                   // 0x803B8350: the model's turn (radians, one turn per 10 s)
    // Per entry, looked up once: the model's vertices (the tube's or the outline's) in own-space positions
    // (preview_unplace), and the buffers they are turned into. The renderer reads a frame's list while the game is
    // already on the next frame, so vertices rewritten where that list loads them tear (a sliver opens where two loads
    // of a ring straddle the write). Each frame is turned into the next of three buffers, the model's own and the two
    // spare ones the container holds after its list (preview_buffer_tag), each with a copy of the model's commands
    // that loads it, and the model record's list is pointed at that buffer's.
    constexpr int preview_buffers = 1 + rush2::track2049::preview_spare_buffers;
    struct PreviewModel {
        bool looked_up = false;
        uint32_t record = 0;                        // the model record (+0xC: its LOD 0 list)
        uint32_t vertices[preview_buffers] = {};    // [0]: the model's own
        uint32_t lists[preview_buffers] = {};
        int buffers = 0;                            // 1: no spare ones, turned in place
        int turn = 0;                               // the buffer last drawn
        std::vector<std::array<float, 3>> local;
        // A spare buffer's list ends with a branch at +commands x 8 to the rest of the model's list (rest): the
        // screenshot, then the state the stock dioramas leave and the end (finish, 0 when not found).
        uint32_t commands = 0, rest = 0, finish = 0;
        uint32_t count = 0;                         // the vertices in a buffer
        bool still = false;                         // spare buffer 1 holds the route unturned, its plain color
        // A custom track's preview (preview_pooled_tag): its spare buffers are lent from the pool while it is on
        // screen (lend_buffers), pool slot `slot`; without them it is turned in place.
        bool pooled = false;
        int slot = -1;
        // The version ring's G_NOOP (track2049.h version_ring_start) and the command after its end marker, in the
        // rest of the list (0: none); ring_shown is what the start command holds now.
        uint32_t ring = 0, ring_after = 0;
        bool ring_shown = true;
    };
    PreviewModel preview_models[menu_tracks];
    bool has_other_version(int t);
    bool find_circuit_ring(uint8_t* rdram, uint32_t list, uint32_t& start, uint32_t& after);
    void show_circuit_ring(uint8_t* rdram, uint32_t start, uint32_t after, bool show);

    // Display list addresses. The game's are 24 bits (0x80000000 or 0 above them); a streamed diorama's
    // (stream_diorama) are full RDRAM addresses above 16 MB, which RT64 takes as they are while gEXSetRDRAMExtended is
    // on (lib/rt64 rt64_rsp.cpp maskPhysicalAddress: the address less 0x80000000).
    constexpr uint32_t extended_start = 0x81000000u;
    bool in_rdram(uint32_t address) {
        return address >= 0x80000000u && address - 0x80000000u < recomp::mem_size;
    }
    uint32_t dl_address(uint32_t w1) {
        return w1 >= extended_start && in_rdram(w1) ? w1 : 0x80000000u | (w1 & 0x00FFFFFF);
    }

    // The record of the loaded model named by the string at `name` (0: none).
    uint32_t model_record(uint8_t* rdram, recomp_context* ctx, uint32_t name) {
        recomp_context saved = *ctx;
        ctx->r4 = (int32_t)name;
        ctx->r5 = 0;
        ctx->r6 = MEM_BU(0, (int32_t)model_slots) - 1;
        ctx->r7 = 1;
        model_find_by_name_8005BE3C(rdram, ctx);
        int32_t handle = (int32_t)ctx->r2;
        *ctx = saved;
        if (handle < 0 || (handle & 0xFFFF) == 0xFFFF) return 0;
        uint32_t table = (uint32_t)MEM_W(0, (int32_t)(model_tables + (uint32_t)(handle >> 10) * 8));
        return table + (uint32_t)(handle & 0x3FF) * 0x34;
    }

    // The custom tracks' dioramas streamed in (stub_bytes): while a custom track's carousel entry is within
    // stream_window places of the shown one, or it is the circuit screen's shown race, its model (stream_models) is
    // copied above 16 MB (recomp::alloc, relocated there: its pointers are offsets from its start) and its record in
    // the container, a stub, pointed at the copy's list. This runs in the frame's pose hooks, before the frame draws,
    // so an entry never shows without its diorama however fast the carousel turns; the window loads the ones the next
    // presses bring in ahead of time. Copies stay for the next visit (the container is loaded again with stubs; the
    // records are pointed again) up to stream_budget, past which the least recently wanted ones out of the window go;
    // a dropped copy is freed a few frames later, the renderer may still read lists of the frames before.
    struct StreamCopy {
        uint8_t* block = nullptr;   // recomp::alloc's
        uint32_t base = 0;          // the copy (16-byte aligned in block)
        uint32_t size = 0;
        uint32_t stub = 0;          // the stub's list the record held before it was pointed at the copy
        uint32_t record = 0;        // the record last pointed at it (checked before use: the container may be gone)
        uint32_t parked = 0;        // while the game's mirror walk runs: the list the record is put back to after
        bool mirrored = false;      // its triangles are wound for a mirrored view (stream_sync_mirror)
        uint32_t used = 0;          // the stream_frame it was last wanted
    };
    std::vector<StreamCopy> stream_copies;     // by mod index
    struct StreamFreed {
        uint8_t* block;
        uint32_t frame;
    };
    std::vector<StreamFreed> stream_freed;     // dropped copies, freed stream_keep_frames after
    std::vector<uint8_t*> stream_orphans;      // copies of an older build, freed once a container is loaded again
    size_t stream_bytes = 0;
    uint32_t stream_frame = 0;
    constexpr int stream_window = 6;
    constexpr size_t stream_budget = size_t(64) << 20;
    constexpr uint32_t stream_keep_frames = 6;
    constexpr uint32_t stream_idle_frames = 120;

    // The name entry t's model is looked up by (the carousel's: its preview's, else its diorama's).
    uint32_t entry_model_name(uint8_t* rdram, int t) {
        return preview_names[t] != 0 ? preview_names[t] : (uint32_t)MEM_W(0, (int32_t)(new_diorama_names + t * 4));
    }

    bool rdram_string_is(uint8_t* rdram, uint32_t at, const std::string& s) {
        if (!in_rdram(at)) return false;
        for (size_t k = 0; k <= s.size(); k++) {
            if ((uint8_t)MEM_B((int32_t)k, (int32_t)at) != (k < s.size() ? (uint8_t)s[k] : 0)) return false;
        }
        return true;
    }

    // Copy i's record back on the stub (when it is on the copy in the loaded container), and the copy dropped.
    void stream_drop(uint8_t* rdram, recomp_context* ctx, size_t i) {
        StreamCopy& c = stream_copies[i];
        if (c.block == nullptr) return;
        int t = mod_id(int(i));
        if (t >= 0 && t < menu_tracks) {
            uint32_t record = model_record(rdram, ctx, entry_model_name(rdram, t));
            uint32_t list = record != 0 ? (uint32_t)MEM_W(0xC, (int32_t)record) : 0;
            if (list >= c.base && list < c.base + c.size && c.stub != 0) MEM_W(0xC, (int32_t)record) = (int32_t)c.stub;
            preview_models[t] = PreviewModel();
        }
        stream_freed.push_back({ c.block, stream_frame });
        stream_bytes -= c.size;
        c = StreamCopy();
    }

    // The build changed (mods turned on or off, a ROM): the copies are of the old one. The loaded container may still
    // point at them until the next one is loaded (stream_container_loaded).
    void stream_forget_all() {
        for (size_t i = 0; i < stream_copies.size(); i++) {
            if (stream_copies[i].block == nullptr) continue;
            stream_orphans.push_back(stream_copies[i].block);
            int t = mod_id(int(i));
            if (t >= 0 && t < menu_tracks) preview_models[t] = PreviewModel();
        }
        stream_copies.clear();
        stream_bytes = 0;
    }

    // A container was just loaded (the track select's carousel or the circuit screen is being built): no record
    // points at an orphan any more.
    void stream_container_loaded() {
        for (uint8_t* block : stream_orphans) stream_freed.push_back({ block, stream_frame });
        stream_orphans.clear();
    }

    // Once a frame on the screens that show the dioramas: frees what the renderer is done with.
    void stream_tick(uint8_t* rdram) {
        stream_frame++;
        auto done = [&](const StreamFreed& f) {
            if (stream_frame - f.frame <= stream_keep_frames) return false;
            recomp::free(rdram, f.block);
            return true;
        };
        stream_freed.erase(std::remove_if(stream_freed.begin(), stream_freed.end(), done), stream_freed.end());
    }

    // Mirror mode. The game draws a mirrored view through a mirrored camera and keeps front faces by winding every
    // triangle the other way: when the mirror state (race setup byte 0x800D0190) differs from the one a model slot was
    // wound for, mirror_state_update_800776DC walks every list of the slot's models (mirror_winding_walk_80077518,
    // following G_DL) and swaps each G_TRI1 / G_TRI2 / G_QUAD's second and third vertex
    // (gfx_tri_winding_swap_cb_8006121C), then keeps the state at slot table entry +2 (0x8010C258, 12 bytes a slot: +0
    // the asset, +1 the model table). That walk reads 24-bit addresses, so the records pointed at copies are put back on
    // their stubs while it runs on asset 3's slot (rush2_track49_stream_mirror_begin / _end), and each copy is wound
    // here to match asset 3's slot instead.
    constexpr uint32_t slot_table = 0x8010C258;
    constexpr uint32_t slot_entry = 12;
    constexpr int menu_art_asset = 3;

    // Asset 3's slot's mirror state: 1 mirrored, 0 not, -1 not loaded.
    int menu_art_mirrored(uint8_t* rdram) {
        int slots = MEM_BU(0, (int32_t)model_slots);
        for (int s = 0; s < slots; s++) {
            uint32_t e = slot_table + uint32_t(s) * slot_entry;
            if (MEM_BU(0, (int32_t)e) == menu_art_asset) return MEM_BU(2, (int32_t)e) != 0 ? 1 : 0;
        }
        return -1;
    }

    // gfx_tri_winding_swap_cb_8006121C on the copy's commands from `at` (in [lo, hi)), following its calls and branches;
    // `seen` keeps a command reached twice (a preview's lists share their rest) from being swapped back.
    void swap_winding(uint8_t* rdram, uint32_t at, uint32_t lo, uint32_t hi, std::set<uint32_t>& seen) {
        auto swapped = [](uint32_t w) { return (w & 0xFFFF0000u) | ((w & 0xFF00u) >> 8) | ((w & 0xFFu) << 8); };
        for (int i = 0; i < 0x10000 && at >= lo && at + 8 <= hi && seen.insert(at).second; i++, at += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)at), w1 = (uint32_t)MEM_W(4, (int32_t)at);
            uint32_t op = w0 >> 24;
            if (op == 0xDF) return;
            if (op == 0x05 || op == 0x06 || op == 0x07) {
                MEM_W(0, (int32_t)at) = (int32_t)swapped(w0);
                if (op != 0x05) MEM_W(4, (int32_t)at) = (int32_t)swapped(w1);
            }
            else if (op == 0xDE) {
                uint32_t to = dl_address(w1);
                if ((w0 >> 16) == 0xDE01) {
                    at = to - 8;
                    continue;
                }
                swap_winding(rdram, to, lo, hi, seen);
            }
        }
    }

    // Copy c of model m wound as `want` (asset 3's slot state, menu_art_mirrored; -1 leaves it): its list and, for a
    // route preview whose spare buffers are made (find_model_vertices), their lists.
    void stream_sync_mirror(uint8_t* rdram, StreamCopy& c, const rush2::track2049::MenuModel& m, int want) {
        if (want < 0 || (want != 0) == c.mirrored) return;
        std::set<uint32_t> seen;
        const uint32_t lo = c.base, hi = c.base + c.size;
        uint32_t own = c.base + m.list;
        swap_winding(rdram, own, lo, hi, seen);
        uint32_t tag = 0;
        for (uint32_t at = own, i = 0; i < 0x10000 && at >= lo && at + 8 <= hi; i++, at += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)at);
            if ((w0 >> 16) == 0xDE01) at = dl_address((uint32_t)MEM_W(4, (int32_t)at)) - 8;
            else if ((w0 >> 24) == 0xDF) { tag = at + 8; break; }
        }
        if (tag != 0 && tag + 16 <= hi && (uint32_t)MEM_W(0, (int32_t)tag) == preview_buffer_tag &&
            MEM_W(12, (int32_t)tag) != 0) {
            uint32_t commands = (uint32_t)MEM_W(4, (int32_t)tag), count = (uint32_t)MEM_W(8, (int32_t)tag);
            uint32_t at = tag + 16;
            for (int b = 1; b < preview_buffers && commands < 4096 && count < 4096; b++) {
                uint32_t list = at + count * 16;
                swap_winding(rdram, list, lo, hi, seen);
                at = list + (commands + 2) * 8;
            }
        }
        c.mirrored = want != 0;
    }

    // Entry t's diorama, when t is a custom track: copied above 16 MB if it isn't, and the record pointed at it.
    void stream_diorama(uint8_t* rdram, recomp_context* ctx, int t) {
        int i = mod_index(t);
        if (i < 0 || size_t(i) >= stream_models.size() || stream_models[size_t(i)].blob.empty()) return;
        const rush2::track2049::MenuModel& m = stream_models[size_t(i)];
        uint32_t name = entry_model_name(rdram, t);
        // Only the track's own model: an SF Rush arcade version shows its course's diorama (write_tables).
        if (!rdram_string_is(rdram, name, m.name)) return;
        if (stream_copies.size() < stream_models.size()) stream_copies.resize(stream_models.size());
        StreamCopy& c = stream_copies[size_t(i)];
        c.used = stream_frame;
        if (c.block == nullptr) {
            uint32_t size = (uint32_t(m.blob.size()) + 15) & ~15u;
            // Over the budget, the copies longest unwanted (out of the window) go first.
            while (stream_bytes + size > stream_budget) {
                size_t pick = SIZE_MAX;
                for (size_t k = 0; k < stream_copies.size(); k++) {
                    const StreamCopy& o = stream_copies[k];
                    if (o.block == nullptr || stream_frame - o.used <= stream_idle_frames) continue;
                    if (pick == SIZE_MAX || o.used < stream_copies[pick].used) pick = k;
                }
                if (pick == SIZE_MAX) break;
                stream_drop(rdram, ctx, pick);
            }
            uint8_t* block = (uint8_t*)recomp::alloc(rdram, size + 16);
            if (block == nullptr) {
                printf("[trackmod] No memory to show %s's diorama (%u bytes)\n", m.name.c_str(), size);
                fflush(stdout);
                c.used = 0;
                return;
            }
            uint32_t base = (uint32_t(block - rdram) + 0x80000000u + 15) & ~15u;
            auto be32 = [&](size_t o) {
                uint32_t w = 0;
                for (size_t b = 0; b < 4; b++) w = w << 8 | (o + b < m.blob.size() ? m.blob[o + b] : 0);
                return w;
            };
            for (uint32_t o = 0; o < size; o += 4) MEM_W(0, (int32_t)(base + o)) = (int32_t)be32(o);
            for (uint32_t p : m.pointers) {
                if (p + 4 > m.blob.size()) continue;
                MEM_W(0, (int32_t)(base + p)) = (int32_t)(base + (be32(p) & 0x00FFFFFF));
            }
            c.block = block;
            c.base = base;
            c.size = size;
            c.stub = 0;
            c.mirrored = false;     // as built
            stream_bytes += size;
        }
        uint32_t record = model_record(rdram, ctx, name);
        if (record == 0) return;
        stream_sync_mirror(rdram, c, m, menu_art_mirrored(rdram));
        c.record = record;
        uint32_t list = (uint32_t)MEM_W(0xC, (int32_t)record);
        if (list >= c.base && list < c.base + c.size) return;
        c.stub = list;
        MEM_W(0xC, (int32_t)record) = (int32_t)(c.base + m.list);
        preview_models[t].looked_up = false;
    }
}

// mirror_state_update_800776DC at its start ($a0 = the model slot) and at 0x800777E8, its exit: while it walks asset
// 3's slot, the records pointed at streamed copies are back on their stubs (the walk reads 24-bit addresses); after it,
// they are pointed again and the copies wound as the slot now is (stream_sync_mirror).
extern "C" void rush2_track49_stream_mirror_begin(uint8_t* rdram, recomp_context* ctx) {
    uint32_t slot = (uint32_t)ctx->r4 & 0xFF;
    if (MEM_BU(0, (int32_t)(slot_table + slot * slot_entry)) != menu_art_asset) return;
    for (StreamCopy& c : stream_copies) {
        if (c.block == nullptr || c.record == 0 || c.stub == 0) continue;
        uint32_t list = (uint32_t)MEM_W(0xC, (int32_t)c.record);
        if (list < c.base || list >= c.base + c.size) continue;
        c.parked = list;
        MEM_W(0xC, (int32_t)c.record) = (int32_t)c.stub;
    }
}

extern "C" void rush2_track49_stream_mirror_end(uint8_t* rdram, recomp_context* ctx) {
    int want = -2;
    for (size_t i = 0; i < stream_copies.size() && i < stream_models.size(); i++) {
        StreamCopy& c = stream_copies[i];
        if (c.parked == 0) continue;
        if ((uint32_t)MEM_W(0xC, (int32_t)c.record) == c.stub) MEM_W(0xC, (int32_t)c.record) = (int32_t)c.parked;
        c.parked = 0;
        if (want == -2) want = menu_art_mirrored(rdram);
        stream_sync_mirror(rdram, c, stream_models[i], want);
    }
}

namespace {

    // The custom tracks' previews' spare buffers: up to 201 previews with two each would take 4 MB of the game heap
    // the track select's container is loaded into (as the stock 2049 previews' are), so the container leaves them out
    // (strip_spare_buffers) and the few previews on screen borrow a slot of this pool, in RDRAM the game heap used
    // before it moved (src/assets.cpp). A slot goes to another preview once its own has been off screen for a few
    // frames (the renderer may still be reading lists from the frames before).
    constexpr uint32_t preview_pool = 0x80320000;
    constexpr uint32_t preview_pool_slot = 0x8000;     // two buffers: vertices, the commands loading them, a branch, an end
    constexpr int preview_pool_slots = 8;              // 0x80320000-0x80360000
    constexpr uint32_t pool_idle_frames = 6;
    std::array<int, preview_pool_slots> pool_owner = [] { std::array<int, preview_pool_slots> a; a.fill(-1); return a; }();
    std::array<uint32_t, preview_pool_slots> pool_used{};
    uint32_t pool_frame = 0;

    // The pool forgets its previews without touching their records: the container they were in was loaded again.
    void reset_pool() {
        for (int& owner : pool_owner) {
            if (owner >= 0) preview_models[owner].slot = -1;
            owner = -1;
        }
    }

    // Model m's spare buffers from `at`: each the vertices, then the model's commands loading them, a branch to the
    // rest of the model's list and an end. `copy`: fill them from the model's own (else they were made before).
    void make_spares(uint8_t* rdram, PreviewModel& m, uint32_t at, bool copy) {
        for (int b = 1; b < preview_buffers; b++) {
            m.vertices[b] = at;
            m.lists[b] = at + m.count * 16;
            at = m.lists[b] + (m.commands + 2) * 8;
            if (!copy) continue;
            for (uint32_t o = 0; o < m.count * 16; o += 4) {
                MEM_W(0, (int32_t)(m.vertices[b] + o)) = MEM_W(0, (int32_t)(m.vertices[0] + o));
            }
            for (uint32_t i = 0; i < m.commands; i++) {
                uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)(m.lists[0] + i * 8));
                uint32_t w1 = (uint32_t)MEM_W(4, (int32_t)(m.lists[0] + i * 8));
                if ((w0 >> 24) == 0x01) w1 += m.vertices[b] - m.vertices[0];
                MEM_W(0, (int32_t)(m.lists[b] + i * 8)) = (int32_t)w0;
                MEM_W(4, (int32_t)(m.lists[b] + i * 8)) = (int32_t)w1;
            }
            uint32_t rest = m.lists[b] + m.commands * 8;
            MEM_W(0, (int32_t)rest) = (int32_t)0xDE010000;
            MEM_W(4, (int32_t)rest) = (int32_t)m.rest;
            MEM_W(8, (int32_t)rest) = (int32_t)0xDF000000;
            MEM_W(12, (int32_t)rest) = 0;
        }
    }

    // A pooled preview gives its slot back: drawn from its own list again.
    void return_buffers(uint8_t* rdram, PreviewModel& m) {
        if (m.slot < 0) return;
        MEM_W(0xC, (int32_t)m.record) = (int32_t)m.lists[0];
        pool_owner[size_t(m.slot)] = -1;
        m.slot = -1;
        m.buffers = 1;
        m.turn = 0;
        m.still = false;
    }

    // Entry t's pooled preview, on screen this frame, keeps or borrows a pool slot (none free: it is turned in place).
    void lend_buffers(uint8_t* rdram, int t, PreviewModel& m) {
        if (!m.pooled || m.record == 0) return;
        if (m.slot >= 0) {
            pool_used[size_t(m.slot)] = pool_frame;
            return;
        }
        if (2 * (m.count * 16 + (m.commands + 2) * 8) > preview_pool_slot) return;
        int pick = -1;
        for (int s = 0; s < preview_pool_slots && pick < 0; s++) {
            if (pool_owner[size_t(s)] < 0) pick = s;
        }
        for (int s = 0; s < preview_pool_slots && pick < 0; s++) {
            if (pool_frame - pool_used[size_t(s)] > pool_idle_frames) pick = s;
        }
        if (pick < 0) return;
        if (pool_owner[size_t(pick)] >= 0) return_buffers(rdram, preview_models[pool_owner[size_t(pick)]]);
        pool_owner[size_t(pick)] = t;
        pool_used[size_t(pick)] = pool_frame;
        m.slot = pick;
        make_spares(rdram, m, preview_pool + uint32_t(pick) * preview_pool_slot, true);
        m.buffers = preview_buffers;
        m.turn = 0;
        m.still = false;
    }

    float rdram_float(uint8_t* rdram, uint32_t address) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)address);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    void write_float(uint8_t* rdram, uint32_t address, float f) {
        uint32_t w;
        memcpy(&w, &f, 4);
        MEM_W(0, (int32_t)address) = (int32_t)w;
    }

    // Entry t's preview model: the vertices its list loads before its screenshot, and its buffers (made on the first
    // lookup after the container loads). `out` holds the last lookup's result.
    void find_model_vertices(uint8_t* rdram, recomp_context* ctx, int t, PreviewModel& out) {
        const PreviewModel last = std::move(out);
        if (last.slot >= 0) pool_owner[size_t(last.slot)] = -1;    // its record is set below
        out = PreviewModel();
        out.looked_up = true;
        out.turn = last.turn;
        uint32_t record = model_record(rdram, ctx, preview_names[t]);
        if (record == 0) return;
        uint32_t own = (uint32_t)MEM_W(0xC, (int32_t)record);

        // The end of the model's list, where the buffers' tag is: {tag, the model's commands, its vertices, the
        // model's own list once the buffers are made}. A container still loaded from an earlier visit may have the
        // record on a buffer's list, which ends with a branch into the model's own.
        using namespace rush2::track2049;
        uint32_t tag = 0;
        for (uint32_t i = 0, at = own; i < 8192 && in_rdram(at); i++, at += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)at), w1 = (uint32_t)MEM_W(4, (int32_t)at);
            if ((w0 >> 16) == 0xDE01) at = dl_address(w1) - 8;
            else if ((w0 >> 24) == 0xDF) { tag = at + 8; break; }
        }
        uint32_t commands = 0, count = 0;
        bool made = false, pooled = false;
        uint32_t tag_word = tag != 0 ? (uint32_t)MEM_W(0, (int32_t)tag) : 0;
        if (tag_word == preview_buffer_tag || tag_word == preview_pooled_tag) {
            pooled = tag_word == preview_pooled_tag;
            commands = (uint32_t)MEM_W(4, (int32_t)tag);
            count = (uint32_t)MEM_W(8, (int32_t)tag);
            uint32_t list = (uint32_t)MEM_W(12, (int32_t)tag);
            made = list != 0;
            if (made) own = list;
        }

        uint32_t first = 0xFFFFFFFF, end = 0;
        uint32_t list = own;
        for (uint32_t i = 0; i < 4096 && (commands == 0 || i < commands) && in_rdram(list); i++, list += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)list), w1 = (uint32_t)MEM_W(4, (int32_t)list);
            uint32_t op = w0 >> 24;
            if (op == 0xFD || op == 0xDF) break;
            if (op != 0x01) continue;
            uint32_t at = dl_address(w1);
            first = std::min(first, at);
            end = std::max(end, at + ((w0 >> 12) & 0xFF) * 16);
        }
        if (end <= first || end - first > 0x10000) return;
        out.record = record;
        out.vertices[0] = first;
        out.lists[0] = own;
        out.buffers = 1;
        if (commands != 0 && commands < 4096 && count == (end - first) / 16) {
            out.commands = commands;
            out.count = count;
            out.rest = own + commands * 8;
            if (pooled) {
                // Its spare buffers come from the pool while it is on screen; off a pool buffer an earlier visit
                // left it on (the pool is reset with each container).
                out.pooled = true;
                MEM_W(0xC, (int32_t)record) = (int32_t)own;
            }
            else {
                // In the container after the tag.
                make_spares(rdram, out, tag + 16, !made);
                out.buffers = preview_buffers;
            }
            MEM_W(12, (int32_t)tag) = (int32_t)own;
            // The rest ends with the state the stock dioramas leave (track2049_art.cpp preview_finish: a pipe sync,
            // the render mode and the combine) before its end.
            for (uint32_t at = out.rest, i = 0; i < 4096 && in_rdram(at); i++, at += 8) {
                if ((uint32_t)MEM_W(0, (int32_t)at) != 0xDF000000u) continue;
                uint32_t sync = at - 24;
                if (sync >= out.rest && (uint32_t)MEM_W(0, (int32_t)sync) == 0xE7000000u &&
                    (uint32_t)MEM_W(8, (int32_t)sync) == 0xE200001Cu) {
                    out.finish = sync;
                }
                break;
            }
            for (uint32_t at = out.rest, i = 0; i < 4096 && in_rdram(at); i++, at += 8) {
                uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)at), w1 = (uint32_t)MEM_W(4, (int32_t)at);
                if ((w0 >> 24) == 0xDF) break;
                if (out.ring == 0 && (w0 >> 16) == 0xDE01) {
                    // The start, branched over on an earlier visit: its branch goes just past the end marker.
                    uint32_t end = dl_address(w1) - 8;
                    if ((uint32_t)MEM_W(0, (int32_t)end) == 0 && (uint32_t)MEM_W(4, (int32_t)end) == version_ring_end) {
                        out.ring = at;
                        out.ring_after = end + 8;
                        out.ring_shown = false;
                        break;
                    }
                }
                if (w0 != 0) continue;
                if (w1 == version_ring_start) out.ring = at;
                if (w1 == version_ring_end && out.ring != 0) { out.ring_after = at + 8; break; }
            }
            if (out.ring_after == 0) out.ring = 0;
            // The circuit screen's version ring stays off here (a circuit screen may have shown it in this container).
            uint32_t circuit_start = 0, circuit_after = 0;
            if (find_circuit_ring(rdram, out.rest, circuit_start, circuit_after)) {
                show_circuit_ring(rdram, circuit_start, circuit_after, false);
            }
        }
        // The own-space positions: read while the vertices are as loaded, and kept across lookups from then on (the
        // model's own vertices are a buffer too, left turned).
        if (made && last.vertices[0] == first && last.local.size() == (end - first) / 16) {
            out.local = last.local;
            return;
        }
        for (uint32_t at = first; at < end; at += 16) {
            float placed[3] = { (float)(int16_t)MEM_H(0, (int32_t)at), (float)(int16_t)MEM_H(2, (int32_t)at),
                                (float)(int16_t)MEM_H(4, (int32_t)at) };
            std::array<float, 3> local;
            rush2::track2049::preview_unplace(placed, local.data());
            out.local.push_back(local);
        }
    }
}

// func_803AB294 at 0x803AB7DC, building the carousel: $a0 = the diorama name of entry $s0 about to be looked up.
extern "C" void rush2_track49_select_model(uint8_t* rdram, recomp_context* ctx) {
    reset_pool();
    stream_container_loaded();
    int t = MEM_BU(0, (int32_t)ctx->r16);
    if (t < menu_tracks && preview_names[t] != 0) {
        ctx->r4 = (int32_t)preview_names[t];
        preview_models[t].looked_up = false;
    }
}

namespace {
    // The node matrix that shows Rush 2049's track select camera space in front of the camera at `camera_base` (view
    // `view_base`'s projection) `depth` ahead, with 2049's screen layout scaled by `size` about its point (anchor_x,
    // anchor_y) and that point at (target_x, target_y) on the view's screen: rows m (a model point v lands at
    // position + sum v_i / 16 x m[i], model units being 1/16 of 2049's) and the position, the camera's.
    bool preview_matrix(uint8_t* rdram, uint32_t camera_base, uint32_t view_base, float depth, float anchor_x,
                        float anchor_y, float target_x, float target_y, float size, float m[3][3], float position[3]) {
        float rows[3][3];
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) rows[r][c] = rdram_float(rdram, camera_base + uint32_t(r * 3 + c) * 4);
            position[r] = rdram_float(rdram, camera_base + 0x24 + uint32_t(r) * 4);
        }
        const float right[3] = { -rows[0][0], -rows[0][1], -rows[0][2] };
        const float* up = rows[1];
        const float* forward = rows[2];
        float tan_h = rdram_float(rdram, view_base + 0x14), width = rdram_float(rdram, view_base + 0x24);
        float cx = rdram_float(rdram, view_base + 0x2C), cy = rdram_float(rdram, view_base + 0x30);
        if (!(tan_h > 0.01f) || !(width > 0.0f) || depth <= 1.0f) return false;
        const float focal = width * 0.5f / tan_h;

        // 2049 camera space (X, Y, Z) to Rush 2's (right, up, forward): Z' = a Z, X' = s a f1 / f2 X + b Z, Y' =
        // s a f1 / f2 Y + c Z, so Rush 2's projection shows each point at 2049's screen point, scaled by s about the
        // anchor, plus a constant offset that puts the anchor at the target.
        using namespace rush2::track2049;
        const float a = depth / preview_depth;
        const float scale = size * a * preview_focal / focal;
        const float b = a * (target_x - cx + size * (preview_center_x - anchor_x)) / focal;
        const float c = a * (cy - target_y - size * (preview_center_y - anchor_y)) / focal;
        for (int i = 0; i < 3; i++) {
            m[0][i] = scale * right[i];
            m[1][i] = scale * up[i];
            m[2][i] = b * right[i] + c * up[i] + a * forward[i];
        }
        return true;
    }

    void write_pose(uint8_t* rdram, int16_t node, const float m[3][3], const float position[3]) {
        uint32_t pose = (uint32_t)MEM_W(0, (int32_t)(node_poses + node * 56));
        for (int r = 0; r < 3; r++) {
            for (int i = 0; i < 3; i++) write_float(rdram, pose + uint32_t(r * 3 + i) * 4, m[r][i]);
        }
        for (int i = 0; i < 3; i++) write_float(rdram, pose + 0x24 + uint32_t(i) * 4, position[i]);
    }

    // Writes entry t's route turned by `angle` into `vertices`, its tube colored with the highlight at `phase` (no
    // highlight below 0).
    void write_route(uint8_t* rdram, int t, const PreviewModel& model, uint32_t vertices, float angle, float phase) {
        using namespace rush2::track2049;
        for (size_t v = 0; v < model.local.size(); v++) {
            float placed[3];
            preview_place(model.local[v].data(), angle, placed);
            uint32_t at = vertices + uint32_t(v) * 16;
            for (int i = 0; i < 3; i++) {
                MEM_H(i * 2, (int32_t)at) = (int16_t)std::clamp(std::lround(placed[i]), -32767L, 32767L);
            }
        }
        // The tube's color: race track k's; a custom track's route race track 4's red (src/track_mod/track_mod_menu.cpp),
        // or its course's color when it is a version of a 2049 race track (the arcade mod's, mod_courses).
        int k = course_id(t) - first_menu_id + 1;
        if (is_mod_track(t)) {
            int g = group_of[size_t(t)];
            int course = g >= 0 ? version_groups[size_t(g)].members[0] : -1;
            k = course >= first_menu_id && course < first_menu_id + track_count ? course - first_menu_id + 1 : 4;
        }
        else if (k < 1 || k > track_count) return;
        if (model.local.size() < size_t(preview_tube_rings) * 4) return;
        for (int ring = 0; ring < preview_tube_rings; ring++) {
            uint8_t rgb[3];
            preview_tube_color(k, ring, phase, rgb);
            for (int v = 0; v < 2; v++) {
                uint32_t at = vertices + uint32_t(ring * 4 + v) * 16 + 12;
                MEM_B(0, (int32_t)at) = rgb[0];
                MEM_B(1, (int32_t)at) = rgb[1];
                MEM_B(2, (int32_t)at) = rgb[2];
            }
        }
    }

    // The version ring shows only on a course with another version, as the X prompt naming the version does
    // (other_version): the ring's start is a G_NOOP, or a branch past its end.
    void show_ring(uint8_t* rdram, int t, PreviewModel& model) {
        if (model.ring == 0) return;
        // A circuit's select (the circuit picked the version, no X prompt) shows it too when the course has another.
        bool show = versions_apply(rdram) ? other_version(rdram, t) : has_other_version(t);
        if (show == model.ring_shown) return;
        model.ring_shown = show;
        MEM_W(0, (int32_t)model.ring) = show ? 0 : (int32_t)0xDE010000;
        MEM_W(4, (int32_t)model.ring) = (int32_t)(show ? rush2::track2049::version_ring_start : model.ring_after);
    }

    // Poses entry t's preview model (node `node`) for the track select: the screenshot's middle (240, 96 on 2049's
    // screen) at (target_x, target_y), `slide` along world x (the carousel's entries). Then turns the model and runs
    // the tube's highlight (`advance`: this frame's step), each frame in the next buffer.
    void pose_preview(uint8_t* rdram, recomp_context* ctx, int t, int16_t node, uint32_t camera_base, uint32_t view_base,
                      float depth, float target_x, float target_y, float slide, bool advance, bool on_screen) {
        float m[3][3], position[3];
        if (!preview_matrix(rdram, camera_base, view_base, depth, 240.0f, 96.0f, target_x, target_y, 1.0f, m, position)) {
            return;
        }
        position[0] += slide;
        write_pose(rdram, node, m, position);

        // The highlight runs once along the route every 5 seconds (0x803B7CE0 += dt / 5) and the model turns once
        // every 10 (0x803B8350 += dt x 2 pi / 10).
        if (advance) {
            float dt = rdram_float(rdram, frame_seconds);
            preview_phase += dt / 5.0f;
            preview_phase -= std::floor(preview_phase);
            preview_angle = std::fmod(preview_angle + dt * 6.2831853f / 10.0f, 6.2831853f);
        }
        PreviewModel& model = preview_models[t];
        if (!model.looked_up) find_model_vertices(rdram, ctx, t, model);
        if (model.buffers == 0) return;
        show_ring(rdram, t, model);
        if (on_screen) lend_buffers(rdram, t, model);
        model.turn = (model.turn + 1) % model.buffers;
        if (model.buffers > 1) {
            MEM_W(0xC, (int32_t)model.record) = (int32_t)model.lists[model.turn];
            // Its branch on to the screenshot (the circuit screen may have pointed it past, rush2_track49_circuit_pose).
            if (model.turn > 0) MEM_W(4, (int32_t)(model.lists[model.turn] + model.commands * 8)) = (int32_t)model.rest;
            if (model.turn == 1) model.still = false;
        }
        write_route(rdram, t, model, model.vertices[model.turn], preview_angle, preview_phase);
    }

    // Depth along a camera's forward axis (row 2) from the camera (+0x24) to `point`.
    float camera_depth(uint8_t* rdram, uint32_t camera_base, const float point[3]) {
        float depth = 0;
        for (int i = 0; i < 3; i++) {
            depth += rdram_float(rdram, camera_base + uint32_t(6 + i) * 4) *
                     (point[i] - rdram_float(rdram, camera_base + 0x24 + uint32_t(i) * 4));
        }
        return depth;
    }

    // Graphics > Falling Snow over a diorama (include/snowfall.h): its snowflakes sit between G_NOOPs tagged "SNOW"
    // and "SNOE" (the N64 SF Rush SNOW models, a custom diorama's SNOWFLAKES material). While the setting is off the
    // first is a branch (G_DL, no return) to the second, so the flakes aren't drawn; on, the G_NOOP again. Each list
    // is scanned once (by the address of the name the entry draws, checked against its record's list and the tag
    // still being there), and written only when the setting changes.
    struct SnowSpan {
        uint32_t record = 0, list = 0;
        uint32_t start = 0, after = 0;   // the "SNOW" and "SNOE" commands, 0: the list has none
        int shown = -1;                  // what is written: 1 the flakes, 0 the branch past them
    };
    std::map<uint32_t, SnowSpan> snow_spans;   // by name address

    void show_diorama_snow(uint8_t* rdram, recomp_context* ctx, uint32_t name) {
        if (name == 0 || !in_rdram(name)) return;
        SnowSpan& sp = snow_spans[name];
        const bool want = rush2::snowfall::enabled();
        auto tag_at = [&](uint32_t at, uint32_t tag) {
            return at != 0 && MEM_W(0, (int32_t)at) == 0 && (uint32_t)MEM_W(4, (int32_t)at) == tag;
        };
        auto branch_at = [&](uint32_t at, uint32_t to) {
            return at != 0 && (uint32_t)MEM_W(0, (int32_t)at) == 0xDE010000u && (uint32_t)MEM_W(4, (int32_t)at) == to;
        };
        bool valid = sp.record != 0 && in_rdram(sp.record) &&
                     dl_address((uint32_t)MEM_W(0xC, (int32_t)sp.record)) == sp.list &&
                     (sp.start == 0 || ((tag_at(sp.start, rush2::snowfall::flakes_start_tag) || branch_at(sp.start, sp.after)) &&
                                        tag_at(sp.after, rush2::snowfall::flakes_end_tag)));
        if (!valid) {
            sp = SnowSpan{};
            sp.record = model_record(rdram, ctx, name);
            if (sp.record == 0) return;
            sp.list = dl_address((uint32_t)MEM_W(0xC, (int32_t)sp.record));
            if (!in_rdram(sp.list)) return;
            // The list straight through (following branches), to its end.
            uint32_t at = sp.list;
            for (int i = 0; i < 200000 && in_rdram(at); i++) {
                const uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)at), w1 = (uint32_t)MEM_W(4, (int32_t)at);
                if (w0 >> 24 == 0xDF) break;
                if (w0 == 0 && w1 == rush2::snowfall::flakes_start_tag) sp.start = at;
                if (w0 == 0 && w1 == rush2::snowfall::flakes_end_tag && sp.start != 0) { sp.after = at; break; }
                // Left off by an earlier visit: the branch to the end tag.
                if (w0 == 0xDE010000u && tag_at(dl_address(w1), rush2::snowfall::flakes_end_tag)) {
                    sp.start = at;
                    sp.after = dl_address(w1);
                    sp.shown = 0;
                    break;
                }
                at = w0 == 0xDE010000u ? dl_address(w1) : at + 8;
            }
            if (sp.after == 0) sp.start = 0;
        }
        if (sp.start == 0 || sp.shown == (int)want) return;
        MEM_W(0, (int32_t)sp.start) = want ? 0 : (int32_t)0xDE010000;
        MEM_W(4, (int32_t)sp.start) = (int32_t)(want ? rush2::snowfall::flakes_start_tag : sp.after);
        sp.shown = (int)want;
    }
}

// func_803AB294 at 0x803ABCA0, each frame: carousel entry $s0's node was just given its place and spun matrix.
extern "C" void rush2_track49_select_pose(uint8_t* rdram, recomp_context* ctx) {
    uint32_t entry = (uint32_t)ctx->r16;
    int t = MEM_BU(0, (int32_t)entry);
    if (t >= menu_tracks) return;
    int slot = MEM_BU(0, (int32_t)(entry + 0x14));   // 0 to count - 1 (us.toml reads it unsigned)
    int count = MEM_H(0, (int32_t)carousel_count);
    bool shown = slot == 0;
    if (shown) stream_tick(rdram);
    // A custom track's diorama, while its entry is near the shown one (stream_diorama).
    if (is_mod_track(t) && (slot <= stream_window || slot >= count - stream_window)) stream_diorama(rdram, ctx, t);
    // Its snowflakes, by Graphics > Falling Snow (a SNOW version's diorama), while it can be on screen.
    if (slot <= 2 || slot >= count - 2) show_diorama_snow(rdram, ctx, (uint32_t)MEM_W(0, (int32_t)(new_diorama_names + t * 4)));
    if (preview_names[t] == 0) return;

    // The carousel's centre entry sits at (0, y, z) and the others slide along x; the centre one advances the
    // highlight and the turn.
    float place[3] = { rdram_float(rdram, entry + 4), rdram_float(rdram, entry + 8), rdram_float(rdram, entry + 12) };
    const float centre[3] = { 0.0f, place[1], place[2] };
    int16_t node = (int16_t)MEM_H(0, (int32_t)(entry + 0x1A));
    if (shown) pool_frame++;
    // The centre entry and the two each side of it (sliding in or out) can be on screen.
    bool on_screen = slot <= 2 || slot >= count - 2;
    pose_preview(rdram, ctx, t, node, camera, view, camera_depth(rdram, camera, centre), preview_disc_center_x,
                 preview_disc_center_y, place[0], shown, on_screen);
}

// The circuit screen (func_803B6260) shows the race's diorama in view 1, a window at the bottom right with its own
// camera (0x800E7A10) and projection (func_80054A50(1, ...): 113 wide, straight ahead at (239, 195); the scissor shows
// (183, 185)-(296, 229)), turned on its turntable by 0x803C7A68. A custom track without an authored diorama has Rush
// 2049's route preview as its model (diorama_model_name), drawn in 2049's track select camera space. Its route is shown
// there as a diorama: in the middle of the window, turned by the turntable's angle through the node's matrix (a still
// copy of the route in spare buffer 1, so nothing is rewritten while the renderer may read it), and without the
// screenshot, which drew black in this view (its quads ran, with the track select's state and data, and still came out
// dark). Buffer 1's branch on to the rest of the list is pointed past the screenshot to the state the stock dioramas
// leave; the track select points it back each time it draws that buffer.
namespace {
    constexpr uint32_t circuit_camera = 0x800E7A10;          // view 1's camera (0x40 a view)
    constexpr uint32_t circuit_view = view + 0x54;           // view 1's projection (0x54 a view)
    constexpr uint32_t circuit_shown = 0x803CB414;           // s32: the track whose diorama is shown
    constexpr uint32_t circuit_place = 0x803CB440;           // float[3]: the diorama's position
    constexpr uint32_t circuit_turn = 0x803C7A68;            // f32: the turntable's angle (radians)
    constexpr uint32_t circuit_set_up = 0x803C7A70;          // u8: the screen's setup has run
    constexpr float circuit_window_drop = 12.0f;             // the window's middle (207) below straight ahead (195)
    // The route's turning centre on 2049's screen: preview_pos (50, -15 raised by 10 pixels, 100) projected.
    constexpr float route_centre_x = rush2::track2049::preview_center_x + rush2::track2049::preview_focal * 0.5f;
    constexpr float route_centre_y = rush2::track2049::preview_center_y + 15.0f * rush2::track2049::preview_focal / 100.0f - 10.0f;
    constexpr float circuit_route_size = 0.45f;              // the turning route (up to about 80 of 2049's pixels) in the window
}

// func_803B6260 at 0x803B64B4, the circuit screen's setup instancing a diorama per name table entry ($s1, from
// 0x80310000) into the instance array (0x80312C00 + track * 4): $t8 is where the loop ends, which us.toml makes the
// end of all 255; the entries past the ones in use are skipped (their instances stay -1, as the screen's teardown
// leaves them).
extern "C" void rush2_track49_circuit_entries(uint8_t* rdram, recomp_context* ctx) {
    stream_container_loaded();
    ctx->r24 = (uint64_t)(int64_t)(int32_t)(new_diorama_names + uint32_t(table_entries.load()) * 4);
}

// func_803B6260 at 0x803B6AFC, each frame: the shown diorama's node (0x80312C00 + track * 4, +2) was just placed.
namespace {
    // The circuit screen's version ring (track2049.h circuit_ring_start) in the list from `list` up to its end: its
    // start command (a G_NOOP when shown, a branch past its end marker when not) and the command after the end; false
    // when the model has none.
    bool find_circuit_ring(uint8_t* rdram, uint32_t list, uint32_t& start, uint32_t& after) {
        using namespace rush2::track2049;
        for (uint32_t at = list, i = 0; i < 8192 && in_rdram(at); i++, at += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)at), w1 = (uint32_t)MEM_W(4, (int32_t)at);
            if ((w0 >> 24) == 0xDF) return false;
            if ((w0 >> 16) == 0xDE01) {
                uint32_t end = dl_address(w1) - 8;
                if (in_rdram(end) && (uint32_t)MEM_W(0, (int32_t)end) == 0 &&
                    (uint32_t)MEM_W(4, (int32_t)end) == circuit_ring_end) {
                    start = at;
                    after = end + 8;
                    return true;
                }
            }
            if (w0 == 0 && w1 == circuit_ring_start) {
                for (uint32_t e = at + 8, j = 0; j < 8192 && in_rdram(e); j++, e += 8) {
                    uint32_t v0 = (uint32_t)MEM_W(0, (int32_t)e), v1 = (uint32_t)MEM_W(4, (int32_t)e);
                    if ((v0 >> 24) == 0xDF) return false;
                    if (v0 == 0 && v1 == circuit_ring_end) {
                        start = at;
                        after = e + 8;
                        return true;
                    }
                }
                return false;
            }
        }
        return false;
    }

    void show_circuit_ring(uint8_t* rdram, uint32_t start, uint32_t after, bool show) {
        MEM_W(0, (int32_t)start) = show ? 0 : (int32_t)0xDE010000;
        MEM_W(4, (int32_t)start) = (int32_t)(show ? rush2::track2049::circuit_ring_start : after);
    }

    // Whether t's course has another version installed (the circuit may race any of them; locked ones count, as the
    // track select's prompt counts them).
    bool has_other_version(int t) {
        int g = t >= 0 && t < menu_tracks ? group_of[size_t(t)] : -1;
        if (g < 0) return false;
        for (int other : version_groups[size_t(g)].members) {
            if (other != t && entry_available(other)) return true;
        }
        return false;
    }

    // A 2049 race track's diorama on the circuit screen: its version ring shown when the course has another version.
    void circuit_diorama_ring(uint8_t* rdram, recomp_context* ctx, int t) {
        uint32_t name = (uint32_t)MEM_W(0, (int32_t)(new_diorama_names + t * 4));
        if (name == 0) return;
        uint32_t record = model_record(rdram, ctx, name);
        if (record == 0) return;
        uint32_t list = (uint32_t)MEM_W(0xC, (int32_t)record);
        uint32_t start = 0, after = 0;
        if (find_circuit_ring(rdram, list, start, after)) show_circuit_ring(rdram, start, after, has_other_version(t));
    }
}

extern "C" void rush2_track49_circuit_pose(uint8_t* rdram, recomp_context* ctx) {
    int t = (int32_t)MEM_W(0, (int32_t)circuit_shown);
    stream_tick(rdram);
    if (t >= 0 && t < menu_tracks && is_mod_track(t)) stream_diorama(rdram, ctx, t);
    if (t >= 0 && t < menu_tracks) show_diorama_snow(rdram, ctx, (uint32_t)MEM_W(0, (int32_t)(new_diorama_names + t * 4)));
    if (t >= 0 && t < menu_tracks && !is_mod_track(t) && course_id(t) >= first_menu_id &&
        course_id(t) < first_menu_id + track_count) {
        circuit_diorama_ring(rdram, ctx, t);
        return;
    }
    if (!is_mod_track(t) || preview_names[t] == 0 ||
        (uint32_t)MEM_W(0, (int32_t)(new_diorama_names + t * 4)) != preview_names[t]) {
        return;
    }
    int16_t node = (int16_t)MEM_H(2, (int32_t)(new_circuit_instances + t * 4));
    if (node < 0) return;
    PreviewModel& model = preview_models[t];
    if (!model.looked_up) find_model_vertices(rdram, ctx, t, model);
    pool_frame++;
    lend_buffers(rdram, t, model);
    if (model.buffers < 2 || model.finish == 0) return;

    float place[3];
    for (int i = 0; i < 3; i++) place[i] = rdram_float(rdram, circuit_place + uint32_t(i) * 4);
    float cx = rdram_float(rdram, circuit_view + 0x2C), cy = rdram_float(rdram, circuit_view + 0x30);
    float m[3][3], position[3];
    if (!preview_matrix(rdram, circuit_camera, circuit_view, camera_depth(rdram, circuit_camera, place), route_centre_x,
                        route_centre_y, cx, cy + circuit_window_drop, circuit_route_size, m, position)) {
        return;
    }
    // The turn as an affine map of the still route's model points: p -> R p + o (preview_place of preview_unplace),
    // folded into the matrix (rows m'[j] = sum R[i][j] m[i]) and the position (+ sum o_i / 16 m[i]).
    float angle = rdram_float(rdram, circuit_turn);
    auto turned = [&](const float p[3], float out[3]) {
        float local[3];
        rush2::track2049::preview_unplace(p, local);
        rush2::track2049::preview_place(local, angle, out);
    };
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    float o[3];
    turned(zero, o);
    float r[3][3];   // r[j]: the turned unit vector j, less o
    for (int j = 0; j < 3; j++) {
        float e[3] = { 0.0f, 0.0f, 0.0f }, out[3];
        e[j] = 1000.0f;
        turned(e, out);
        for (int i = 0; i < 3; i++) r[j][i] = (out[i] - o[i]) / 1000.0f;
    }
    float turned_m[3][3];
    for (int j = 0; j < 3; j++) {
        for (int k = 0; k < 3; k++) turned_m[j][k] = r[j][0] * m[0][k] + r[j][1] * m[1][k] + r[j][2] * m[2][k];
    }
    for (int k = 0; k < 3; k++) position[k] += (o[0] * m[0][k] + o[1] * m[1][k] + o[2] * m[2][k]) / 16.0f;
    write_pose(rdram, node, turned_m, position);

    if (!model.still) {
        write_route(rdram, t, model, model.vertices[1], 0.0f, -1.0f);
        model.still = true;
    }
    // Past the screenshot to the version ring (shown for a course with another version), else to the end state.
    uint32_t target = model.finish, start = 0, after = 0;
    if (find_circuit_ring(rdram, model.rest, start, after) && after <= model.finish) {
        show_circuit_ring(rdram, start, after, has_other_version(t));
        target = start;
    }
    MEM_W(4, (int32_t)(model.lists[1] + model.commands * 8)) = (int32_t)target;
    MEM_W(0xC, (int32_t)model.record) = (int32_t)model.lists[1];
}

// Start of func_803AB01C: whether track $a0 can be chosen. Replaces the function.
extern "C" int rush2_track49_select_available(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = track_selectable(rdram, (int32_t)ctx->r4) ? 1 : 0;
    return 1;
}

// func_803ABE0C at 0x803AC118: the TRACK option's step loop (0x803AC0EC-0x803AC124: id byte at $a3 += step $a1 until
// func_803AB01C accepts it) just stored 0 because the id passed 254. The game read the id signed (lb) and wrapped
// below 0 to 11 at 0x803AC100; read unsigned (us.toml lbu) 0 - 1 is 255, which the patched slti at 0x803AC0FC sends
// to 0 as well, so stepping left from 0 went 255 -> 0 -> 255 forever whenever track 0 is off (the stunt and battle
// selects, or Rush 2's tracks filtered out): the game hung on that select. Left (step < 0) wraps to the last id
// instead; the loop then steps back past entries that are off. Either way rush2_track49_select_step then takes the
// track at the carousel's new position.
extern "C" void rush2_track49_select_wrap(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r5 < 0) MEM_B(0, (int32_t)ctx->r7) = uint8_t(menu_tracks - 1);
}

// func_803ABE0C at 0x803AC670 (player 1) / 0x803AC6B8 (player 2): about to store the save record byte with the
// chosen track ($a2 / $t9) in its low nibble ($t9 / $t6, from the old byte $t7). 2049 ids keep the old byte, and so
// does the stunt select, which keeps its choice apart so the race select's isn't lost to it.
extern "C" void rush2_track49_select_save_p1(uint8_t* rdram, recomp_context* ctx) {
    int t = (int32_t)ctx->r6;
    std::lock_guard lock{ menu_mutex };
    // The TRACK option is the one that changed the track, so the cursor is on the first row, which every list keeps.
    filter_options(rdram);
    if (select_kind == select_stunt) {
        ctx->r25 = ctx->r15;
        save_stunt_selection(t);
    }
    else if (select_kind == select_battle) {
        ctx->r25 = ctx->r15;
        save_battle_selection(t);
    }
    else if (t >= first_menu_id) {
        ctx->r25 = ctx->r15;
        save_selection(t - first_menu_id);
    }
    else {
        save_selection(-1);
    }
}

extern "C" void rush2_track49_select_save_p2(uint8_t* rdram, recomp_context* ctx) {
    if (select_kind != select_race || (int32_t)ctx->r25 >= first_menu_id) {
        ctx->r14 = ctx->r15;
    }
}

// Start of func_803B1AB0 (car select, $a0 = player, $a2 = count Dew cans instead): stores the number of keys collected
// on the current track at $a1. On the added tracks the unlock system decides (rush2::unlocks::added_track_keys).
extern "C" int rush2_track49_keys(uint8_t* rdram, recomp_context* ctx) {
    int count;
    if (!rush2::unlocks::added_track_keys(rdram, (int32_t)ctx->r4, course_id(MEM_BU(0, (int32_t)track_id)),
                                          (ctx->r6 & 0xFF) != 0, count)) {
        return 0;
    }
    MEM_B(0, (int32_t)ctx->r5) = (int8_t)count;
    return 1;
}

// func_800A5ADC at 0x800A5B2C (race start, before the race's cars and objects are set up) and the start of
// func_800A5110 (race setup, also reached directly by func_800A57C8). A RESTART runs func_800A5110 but skips its track
// file queue (func_800A4C98, only when 0x8010C3F8 is 0), so the 2049 objects are reset here too.
extern "C" void rush2_track49_race_start(uint8_t* rdram, recomp_context* ctx) {
    enter_race(rdram);
    rush2::track2049::reset_race_objects();
}

int rush2::track2049::host_menu_track(uint8_t* rdram, int t) {
    // As the track select's start does: nothing hosted, so a Rush 2 track in the host slot is itself.
    set_race_track(0);
    rush2::track1::set_race_track(0);
    set_stunt_arena(0);
    set_battle_arena(0);
    rush2::track_mod::set_race_track(-1);
    rush2::calspeed::set_race_track(0);
    MEM_B(0, (int32_t)track_id) = (int8_t)t;
    enter_race(rdram);
    return MEM_BU(0, (int32_t)track_id);
}

bool rush2::track2049::menu_track_logo(uint8_t* rdram, int t, std::vector<uint8_t>& out) {
    t = course_id(t);   // a Dreamcast version: its course's banner
    if (t >= 0 && t < rush2_tracks) {
        return rush2::assets::read_original(rdram, 4 + t, out);
    }
    std::vector<uint8_t> logo;
    if (!rush2::assets::read_original(rdram, 4 + host_slot, logo)) return false;
    if ((t >= first_menu_id && t < r1_first) || t == obstacle_menu_id) {
        auto rom = rush2::wings::get_rom();
        int k = t == obstacle_menu_id ? obstacle : t - first_menu_id + 1;
        return available() && rom != nullptr && build_race_logo(logo, *rom, k, out);
    }
    if (t >= r1_first && t < r1_end) {
        auto rom = rush2::track1::get_rom();
        return rush2::track1::available() && rom != nullptr && rush2::track1::build_race_logo(logo, *rom, t - r1_first, out);
    }
    if (t >= stunt_menu_id && t < stunt_menu_id + stunt_count) {
        // A stunt arena's, in its own host's container (STUNT1's).
        auto rom = rush2::wings::get_rom();
        return available() && rom != nullptr && rush2::assets::read_original(rdram, 4 + stunt_host_slot, logo) &&
               build_race_logo(logo, *rom, stunt_first + (t - stunt_menu_id), out);
    }
    if (t >= cs_first && t < cs_end) {
        auto rom = rush2::calspeed::get_rom();
        return rush2::calspeed::available() && rom != nullptr &&
               rush2::calspeed::build_race_logo(logo, *rom, t - cs_first, out);
    }
    if (is_mod_track(t)) {
        // The custom track's own logo, as its race shows it (mod_race_logo).
        rush2::track_mod::MenuArt art;
        {
            std::lock_guard lock{ menu_mutex };
            size_t i = size_t(mod_index(t));
            if (i < mod_ok.size() && mod_ok[i]) art.logo = mod_arts[i].logo, art.logo_palette = mod_arts[i].logo_palette;
        }
        if (art.logo.empty() && !rush2::track_mod::menu_art(mod_index(t), art)) return false;
        return set_race_logo(logo, art.logo, art.logo_palette, out);
    }
    return false;
}

// Called by the race load under track_mutex, so it doesn't take menu_mutex (the select's init holds that one while it
// takes track_mutex); the track was just converted, so menu_art has its art without reading files. Without its art the
// logo is left clear (as a track without a [menu] logo has it), never the host's.
bool rush2::track2049::mod_race_logo(uint8_t* rdram, int index, int container, std::vector<uint8_t>& out) {
    std::vector<uint8_t> logo;
    if (index < 0 || !rush2::assets::read_original(rdram, container, logo)) return false;
    rush2::track_mod::MenuArt art;
    if (!rush2::track_mod::menu_art(index, art) || art.logo.size() != 128 * 32 || art.logo_palette.size() != 512) {
        art.logo.assign(128 * 32, 255);     // index 255 clear
        art.logo_palette.assign(512, 0);
    }
    return set_race_logo(logo, art.logo, art.logo_palette, out);
}

namespace {
    // Where loaded asset `asset` is in RDRAM, or 0.
    uint32_t loaded_asset(uint8_t* rdram, recomp_context* ctx, int asset) {
        constexpr uint32_t asset_slots = 0x8010C258;   // 12 bytes a slot: +0 asset, +4 its data
        recomp_context call = *ctx;
        call.r4 = asset;
        asset_find_slot_8008696C(rdram, &call);
        int slot = (int32_t)call.r2;
        return slot < 0 ? 0 : (uint32_t)MEM_W(slot * 12 + 4, (int32_t)asset_slots);
    }
}

bool rush2::track2049::write_menu_logo(uint8_t* rdram, recomp_context* ctx, int asset, int t) {
    std::vector<uint8_t> logo;
    uint32_t texels, palette;
    if (!menu_track_logo(rdram, t, logo) || !logo_data_offsets(logo, texels, palette)) {
        return false;
    }
    uint32_t data = loaded_asset(rdram, ctx, asset);
    if (data == 0) return false;
    for (uint32_t i = 0; i < 128 * 32; i++) MEM_B(0, (int32_t)(data + texels + i)) = logo[texels + i];
    for (uint32_t i = 0; i < 512; i++) MEM_B(0, (int32_t)(data + palette + i)) = logo[palette + i];
    return true;
}

bool rush2::track2049::write_menu_texture(uint8_t* rdram, recomp_context* ctx, int asset, const std::string& name,
                                          const std::vector<uint8_t>& texels, const std::vector<uint8_t>& palette) {
    // Rush 2 model container: +8 texture records (0x20: name[16], u16 w, u16 h, u32, u32 texels), +12 palette
    // records (0x18: name[16], u32, u32 colors), +20 / +24 their counts.
    std::vector<uint8_t> file;
    if (!rush2::assets::read_original(rdram, asset, file) || file.size() < 0x28) return false;
    auto be32 = [&](size_t o) { return uint32_t(file[o] << 24 | file[o + 1] << 16 | file[o + 2] << 8 | file[o + 3]); };
    auto named = [&](size_t rec) { return strncmp((const char*)&file[rec], name.c_str(), 16) == 0; };
    uint32_t tex_at = 0, pal_at = 0, size = 0;
    for (uint32_t i = 0; i < be32(20); i++) {
        size_t rec = be32(8) + size_t(i) * 0x20;
        if (rec + 0x20 <= file.size() && named(rec)) {
            tex_at = be32(rec + 24);
            size = uint32_t((file[rec + 16] << 8 | file[rec + 17]) * (file[rec + 18] << 8 | file[rec + 19]));
        }
    }
    for (uint32_t i = 0; i < be32(24); i++) {
        size_t rec = be32(12) + size_t(i) * 0x18;
        if (rec + 0x18 <= file.size() && named(rec)) pal_at = be32(rec + 20);
    }
    if (tex_at == 0 || pal_at == 0 || tex_at + size > file.size() || pal_at + 512 > file.size()) return false;
    if (!texels.empty() && (texels.size() != size || palette.size() != 512)) return false;
    uint32_t data = loaded_asset(rdram, ctx, asset);
    if (data == 0) return false;
    const uint8_t* t = texels.empty() ? &file[tex_at] : texels.data();
    const uint8_t* p = texels.empty() ? &file[pal_at] : palette.data();
    for (uint32_t i = 0; i < size; i++) MEM_B(0, (int32_t)(data + tex_at + i)) = (int8_t)t[i];
    for (uint32_t i = 0; i < 512; i++) MEM_B(0, (int32_t)(data + pal_at + i)) = (int8_t)p[i];
    return true;
}

// func_800A62C0 after the menu overlay is loaded: the overlay's own data starts fresh, and so must the circuit
// screen's diorama instances that moved out of it.
extern "C" void rush2_track49_overlay_loaded(uint8_t* rdram, recomp_context* ctx) {
    for (int t = 0; t < circuit_tracks; t++) {
        MEM_W(0, (int32_t)(new_circuit_instances + t * 4)) = 0xFFFFFFFF;
    }
    write_string(rdram, stunt_label, "STUNT");
    MEM_W(0, (int32_t)stunt_label_ptr) = stunt_label;
    write_string(rdram, unlocks_label, "UNLOCKS");
    MEM_W(0, (int32_t)unlocks_label_ptr) = unlocks_label;
    write_string(rdram, ghost_label, "GHOST RACE");
    MEM_W(0, (int32_t)ghost_label_ptr) = ghost_label;
    write_string(rdram, battle_label, "BATTLE");
    MEM_W(0, (int32_t)battle_label_ptr) = battle_label;
}

// Start of func_803B6260, the circuit screen (every frame). It shows the dioramas and logos of the circuit's races
// from the 29-entry tables (us.toml), so they and the 2049 art must be in place.
extern "C" void rush2_track49_circuit_screen(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ menu_mutex };
    refresh_mods();
    serve_menu_container(rdram);
    write_tables(rdram);
    // The setup (a nonzero $a0 and the screen not set up yet) instances the dioramas from the container just served,
    // so the route previews' models are looked up again (rush2_track49_circuit_pose).
    if ((ctx->r4 & 0xFF) != 0 && MEM_BU(0, (int32_t)circuit_set_up) == 0) {
        for (PreviewModel& model : preview_models) model.looked_up = false;
        reset_pool();
    }
}

// func_800A7DCC after it generated a circuit's races (tracks 0-6). With added tracks available, or only some courses
// chosen for the circuit (src/circuit.cpp), the tracks are picked again from the chosen ones among Rush 2's 7, HALFPIPE,
// CRASH, PIPE and MIDWAY (7-10, PIPE and MIDWAY once unlocked; Rush 2's circuits never have them) and the added tracks, custom tracks included (all of them if none is chosen), with Rush 2's rules: no track twice in a row, at most 4 times in all,
// and no track in the same direction twice. The game generates races only up to the one to come, so the picks are
// seeded from its random state before its race loop (which moves on with every race run; seeding from the state after
// it re-picked the races already run, so a circuit's review showed other tracks than the ones raced). The generator
// runs on every visit to the circuit screens, and the custom
// tracks it can pick follow the enabled mods (missing for a moment while an arcade mod is rebuilt at launch), so the
// races are kept with the circuit's record (rush2::circuit::record_races, custom tracks by folder) and taken from it
// when they were generated before: a circuit's races stay the ones it raced, as with Rush 2's own tracks.
extern "C" void rush2_track49_circuit(uint8_t* rdram, recomp_context* ctx) {
    {
        std::lock_guard lock{ menu_mutex };
        refresh_mods();     // the custom tracks that can be raced (entry_available), as the track select has them
    }
    std::vector<int> all, pool;
    for (int t = 0; t < circuit_stock_tracks; t++) all.push_back(t);
    all.push_back(7);   // HALFPIPE
    all.push_back(8);   // CRASH
    if (rush2::unlocks::track_open(rdram, 9, MEM_BU(0, (int32_t)pipe_unlocked) != 0)) all.push_back(9);
    if (rush2::unlocks::track_open(rdram, 10, MEM_BU(0, (int32_t)atari_unlocked) != 0)) all.push_back(10);   // MIDWAY
    for (int t = first_menu_id; t < circuit_tracks; t++) {
        if (entry_available(t) && !is_stunt_course(t) && rush2::unlocks::track_open(rdram, course_id(t))) all.push_back(t);
    }
    for (int t : all) {
        if (rush2::circuit::course_chosen(rdram, t)) pool.push_back(t);
    }
    if (pool.empty()) {
        pool = all;
    }
    int kept_track, kept_direction;
    if (pool.size() == circuit_stock_tracks && pool.back() == circuit_stock_tracks - 1 &&
        !rush2::circuit::recorded_race(rdram, 0, kept_track, kept_direction)) {
        return; // Rush 2's own tracks: the game's picks stand.
    }
    int last = (int8_t)MEM_B(0, (int32_t)circuit_last);
    // Seeded from the game's random state ($s0 points at it) as it was before the race loop, so the same circuit seed
    // gives the same races. The loop (0x800A8374-0x800A857C) draws 4 numbers a race (track, direction, fog, wind) for
    // races 0 to `last`, the race to come: the state at this hook moves on with every race run, so it is wound back.
    constexpr uint32_t lcg_mul = 0x41C64E6D, lcg_add = 0x3039, draws_per_race = 4;
    constexpr uint32_t lcg_inverse = [] {
        uint32_t x = lcg_mul;                           // Newton's iteration for the inverse mod 2^32
        for (int k = 0; k < 5; k++) x *= 2 - lcg_mul * x;
        return x;
    }();
    static_assert(lcg_mul * lcg_inverse == 1);
    uint32_t state = (uint32_t)MEM_W(0, (int32_t)ctx->r16);
    for (int n = 0; n < (last + 1) * int(draws_per_race); n++) state = (state - lcg_add) * lcg_inverse;
    std::mt19937 rng{ state };
    std::map<int, int> uses;
    std::set<std::pair<int, int>> seen; // (track, direction bits)
    int previous = -1;
    std::vector<std::pair<int, int>> generated;
    for (int i = 0; i <= last; i++) {
        uint32_t race = circuit_races + i * 4;
        // The pick is made even for a kept race, so the races after it draw the same numbers as they did.
        std::vector<int> choices;
        for (int t : pool) {
            if (t != previous && uses[t] < circuit_max_uses) choices.push_back(t);
        }
        if (choices.empty()) {
            choices = pool;
        }
        int t = choices[std::uniform_int_distribution<size_t>(0, choices.size() - 1)(rng)];
        int dir = MEM_B(0, (int32_t)(race + 1)) & 3;
        // A race run (before `last`, the race to come) keeps its track; the race to come only one it can be raced on.
        if (rush2::circuit::recorded_race(rdram, i, kept_track, kept_direction) &&
            (i < last || std::find(all.begin(), all.end(), kept_track) != all.end())) {
            t = kept_track;
            dir = kept_direction;
        }
        else {
            for (int k = 0; k < 4 && seen.contains({ t, dir }); k++) {
                dir = (dir + 1) & 3;
            }
        }
        generated.push_back({ t, dir });
        seen.insert({ t, dir });
        uses[t]++;
        previous = t;
        MEM_B(0, (int32_t)race) = uint8_t(t);
        MEM_B(0, (int32_t)(race + 1)) = uint8_t(dir);
    }
    rush2::circuit::record_races(rdram, generated);
    // The races, once per circuit (the generator runs on every visit to the circuit screen).
    static std::string logged;
    std::string races;
    for (int i = 0; i <= last; i++) races += " " + std::to_string(MEM_BU(0, (int32_t)(circuit_races + i * 4)));
    if (races != logged) {
        logged = races;
        printf("[circuit] Tracks:%s\n", races.c_str());
        std::fflush(stdout);
    }
}

// The menus' stunt track tests (see the top of this file): a stunt or battle arena's id counts as STUNT1's, and so
// does the obstacle course's except for stunt mode.

// func_800AE670 at 0x800AE78C: $t7 = the track, about to be compared with 11 to set stunt mode (game mode 2, in which
// func_80094698 sets no drones and the race scores stunts).
extern "C" void rush2_track49_stunt_mode(uint8_t* rdram, recomp_context* ctx) {
    if (is_arena((int32_t)ctx->r15)) ctx->r15 = stunt_host_slot;
}

// func_80094698 at 0x80094754 / 0x800947F0: $t8 / $t6 = the track, about to be compared with 11 (backward and mirror
// are forced off on it).
extern "C" void rush2_track49_stunt_settings_t8(uint8_t* rdram, recomp_context* ctx) {
    if (is_stunt_course((int32_t)ctx->r24)) ctx->r24 = stunt_host_slot;
}

extern "C" void rush2_track49_stunt_settings_t6(uint8_t* rdram, recomp_context* ctx) {
    if (is_stunt_course((int32_t)ctx->r14)) ctx->r14 = stunt_host_slot;
}

// func_803ABE0C at 0x803ABFBC and 0x803AC554: $a2 = the track and $t0 = 11, about to be compared to grey the options
// other than TRACK, FOG and WIND. $a2 is used as the track afterwards, so $t0 takes the arena's id instead ($t0 is
// set again before its next use). Options a course keeps open (option_open) aren't greyed.
extern "C" void rush2_track49_stunt_select(uint8_t* rdram, recomp_context* ctx) {
    int t = (int32_t)ctx->r6;
    if (is_stunt_course(t) && !option_open(t, cursor_option(rdram))) ctx->r8 = ctx->r6;
}

// A list shorter than the rows on screen (filter_options). TRACK, the first option, has no row of its own (the
// carousel's arrows): the rows on screen start at the top row 0x803D05C4, 1 when the screen opens, and show the 4
// options after TRACK. func_803ABE0C at 0x803AC4FC, the cursor moved down: $v0 = the top row, about to be scrolled
// unless $v0 + 4 is the list's count ($a0, reloaded before its next use). A list with no more than the rows on
// screen after TRACK never scrolls.
extern "C" void rush2_track49_option_scroll(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r4 <= option_boxes) ctx->r4 = ctx->r2 + option_boxes;
}

// func_803C5710 (an OPTIONTEXTBOX widget) at 0x803C5774: $a2 = its row, $a1 = whether the box is hidden (under a
// slider). Rows past the list are hidden.
extern "C" void rush2_track49_option_box(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r6 >= option_list_count(rdram)) ctx->r5 = 1;
}

// func_803C6268 at 0x803C6934: $t0 = the y of the row just drawn (0xBC, then 0xB apart), about to step to the next
// row until 0xE8. The rows wrap around the list (to TRACK's, which has no row), so a short list stops at its last.
extern "C" void rush2_track49_option_text_rows(uint8_t* rdram, recomp_context* ctx) {
    constexpr int first_y = 0xBC, row_height = 0xB;
    int drawn = ((int32_t)ctx->r8 - first_y) / row_height + 1;
    int rows = option_list_count(rdram) - (int32_t)MEM_W(0, (int32_t)option_top);
    if (drawn >= rows) ctx->r8 = first_y + (option_boxes - 1) * row_height;
}

// func_803C6268, the DEATHS value (option 10's case) at 0x803C6858 ($t4) / 0x803C68AC ($t0): the track, about to be
// compared with $s5 (11, or the stunt course's id, rush2_track49_stunt_options) to grey the value. Not on a course
// where DEATHS stays open.
extern "C" void rush2_track49_deaths_value_t4(uint8_t* rdram, recomp_context* ctx) {
    if (option_open((int32_t)ctx->r12, option_deaths)) ctx->r12 = 0;
}

extern "C" void rush2_track49_deaths_value_t0(uint8_t* rdram, recomp_context* ctx) {
    if (option_open((int32_t)ctx->r8, option_deaths)) ctx->r8 = 0;
}

// func_803C6268, DRONES' digits at 0x803C6720: $t4 = the track, about to be compared with $s5 to grey the digit of
// the drone count (green otherwise). Not on a battle arena, where DRONES is its computer opponents.
extern "C" void rush2_track49_drones_value(uint8_t* rdram, recomp_context* ctx) {
    if (option_open((int32_t)ctx->r12, option_drones)) ctx->r12 = 0;
}

// The BATTLE select's POINTS and TIME LIMIT rows, Rush 2049's battle rules (rush2::battle::points_to_win and
// time_limit_minutes): its LAPS and CHECKPOINTS rows, which a battle doesn't use, with other labels, values and steps.
namespace {
    // The label of option row `option` on the BATTLE select, or 0 for the game's own.
    uint32_t battle_rule_label(uint8_t* rdram, int option) {
        if (select_kind != select_battle) return 0;
        if (option == option_laps) {
            write_string(rdram, points_label, "POINTS");
            return points_label;
        }
        if (option == option_checkpoints) {
            write_string(rdram, time_label, "TIME LIMIT");
            return time_label;
        }
        return 0;
    }

    int32_t call_game(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0 = 0,
                      int32_t a1 = 0, int32_t a2 = 0) {
        recomp_context saved = *ctx;
        ctx->r4 = a0;
        ctx->r5 = a1;
        ctx->r6 = a2;
        func(rdram, ctx);
        int32_t ret = (int32_t)ctx->r2;
        *ctx = saved;
        return ret;
    }
}

extern "C" void text_select_style_800737E4(uint8_t* rdram, recomp_context* ctx);
extern "C" void text_measure_string_800732AC(uint8_t* rdram, recomp_context* ctx);
extern "C" void text_print_string_800734E0(uint8_t* rdram, recomp_context* ctx);

// func_803C6268 at 0x803C6414: $s0 = the row's label, about to be placed and printed; $s1 = the row's entry in the
// option list.
extern "C" void rush2_track49_battle_rule_label(uint8_t* rdram, recomp_context* ctx) {
    uint32_t label = battle_rule_label(rdram, (int32_t)MEM_W(0, (int32_t)ctx->r17));
    if (label != 0) ctx->r16 = (uint64_t)(int64_t)(int32_t)label;
}

// func_803C59A0 (the arrows either side of the cursor row's label) at 0x803C5C18: $a3 = the label, about to be
// measured to place the left arrow.
extern "C" void rush2_track49_battle_rule_arrows(uint8_t* rdram, recomp_context* ctx) {
    uint32_t label = battle_rule_label(rdram, cursor_option(rdram));
    if (label != 0) ctx->r7 = (uint64_t)(int64_t)(int32_t)label;
}

// func_803C6268, LAPS' value (case 5) at 0x803C666C, the digits 1-8 from x 0xFF, and CHECKPOINTS' (case 9) at
// 0x803C6770, OFF / ON; the chosen value in style 0xA, at y $s2, $s1 = the row's entry. On the BATTLE select the
// points ("10") or the time limit ("OFF", "8 MIN") is printed instead, centered on the values' column, and the case
// is skipped (to 0x803C6908, the next row).
extern "C" int rush2_track49_battle_rule_value(uint8_t* rdram, recomp_context* ctx) {
    if (select_kind != select_battle) return 0;
    constexpr int value_center_x = 0x112, style_chosen = 0xA;
    int option = (int32_t)MEM_W(0, (int32_t)ctx->r17);
    int minutes = rush2::battle::time_limit_minutes();
    std::string text = option == option_laps ? std::to_string(rush2::battle::points_to_win())
                     : minutes == 0 ? "OFF" : std::to_string(minutes) + " MIN";
    write_string(rdram, rule_value, text);
    int width = call_game(rdram, ctx, text_measure_string_800732AC, (int32_t)rule_value, -1);
    call_game(rdram, ctx, text_select_style_800737E4, style_chosen);
    call_game(rdram, ctx, text_print_string_800734E0, value_center_x - width / 2, (int16_t)ctx->r18,
              (int32_t)rule_value);
    return 1;
}

// func_803ABE0C, LAPS' left / right (case 5) at 0x803AC278 and CHECKPOINTS' (case 9) at 0x803AC3A4: $a1 = the step
// (-1, 1, or 0 for the default), about to change the menu settings. On the BATTLE select the rule steps as in 2049
// (setup overlay 0x8038ECBC / 0x8038ED20): the points by 5 from 5 to 50, the time limit off, then 1 to 20 minutes,
// both wrapping; step 0 gives the default. The case goes to its end (0x803AC410: the menu settings are copied to
// the race's).
extern "C" int rush2_track49_battle_rule_step(uint8_t* rdram, recomp_context* ctx) {
    if (select_kind != select_battle) return 0;
    int step = (int32_t)ctx->r5;
    std::lock_guard lock{ menu_mutex };
    if (cursor_option(rdram) == option_laps) {
        using namespace rush2::battle;
        int points = step == 0 ? default_points : points_to_win() + step * points_step;
        if (points < min_points) points = max_points;
        if (points > max_points) points = min_points;
        set_points_to_win(points);
    }
    else {
        using namespace rush2::battle;
        int minutes = step == 0 ? default_minutes : time_limit_minutes() + step;
        if (minutes < 0) minutes = max_minutes;
        if (minutes > max_minutes) minutes = 0;
        set_time_limit_minutes(minutes);
    }
    write_selection();
    return 1;
}

// The race select's BATTLE row (rush2::battle::race_battle): option 11, past the game's 0-10. Each place that indexes
// a table by the option is hooked: its label (0x800C48F8 + language x 44, which an 11 would read past) in the text
// (0x803C6410) and for the arrows' width (0x803C59A0 at 0x803C5C14); its value (the value jump table 0x803CAF38 has
// cases for 1-10; an 11 skips to the next row at 0x803C644C, so 0x803C6440 draws it first); and left / right (the
// jump table 0x803CAC10 has 0-10; an 11 goes to 0x803AC410, which copies the menu settings, so 0x803AC078 toggles it
// first). The value is drawn as BACKWARD's (case 1 at 0x803C6468): the language's two words (0x800C4A90 + language x 8,
// off then on) at x 0x100 and centered on 0x124, the chosen one in style 0xA (green) and the other in style 4.
namespace {
    constexpr uint32_t language = 0x800C4618;        // s32
    constexpr uint32_t onoff_words = 0x800C4A90;     // char*[2] per language: BACKWARD's values, off then on

    uint32_t battle_row_text(uint8_t* rdram) {
        write_string(rdram, battle_row_label, "BATTLE");
        return battle_row_label;
    }
}

extern "C" void text_center_x_variant_8007347C(uint8_t* rdram, recomp_context* ctx);

// func_803C6268 at 0x803C6410: about to load the row's label into $s0 ($s1 = the row's entry in the option list).
// Returns 1 with $s0 set for the BATTLE row (the caller skips the load).
extern "C" int rush2_track49_battle_label(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)MEM_W(0, (int32_t)ctx->r17) != option_battle) return 0;
    ctx->r16 = (uint64_t)(int64_t)(int32_t)battle_row_text(rdram);
    return 1;
}

// func_803C59A0 at 0x803C5C14: about to load the cursor row's label into $a3 to measure it. Returns 1 with $a3 set
// for the BATTLE row.
extern "C" int rush2_track49_battle_arrow_label(uint8_t* rdram, recomp_context* ctx) {
    if (cursor_option(rdram) != option_battle) return 0;
    ctx->r7 = (uint64_t)(int64_t)(int32_t)battle_row_text(rdram);
    return 1;
}

// func_803C6268 at 0x803C6440, after the row's label: the BATTLE row's value (at y $s2), as BACKWARD's. Returns 1 to
// go on to the next row.
extern "C" int rush2_track49_battle_value(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)MEM_W(0, (int32_t)ctx->r17) != option_battle) return 0;
    constexpr int style_chosen = 0xA, style_other = 4;
    bool on = rush2::battle::race_battle();
    int lang = (int32_t)MEM_W(0, (int32_t)language);
    int32_t off_word = MEM_W(0, (int32_t)(onoff_words + lang * 8)), on_word = MEM_W(0, (int32_t)(onoff_words + lang * 8 + 4));
    int32_t y = (int16_t)ctx->r18;
    call_game(rdram, ctx, text_select_style_800737E4, on ? style_other : style_chosen);
    call_game(rdram, ctx, text_print_string_800734E0, 0x100, y, off_word);
    call_game(rdram, ctx, text_select_style_800737E4, on ? style_chosen : style_other);
    int x = call_game(rdram, ctx, text_center_x_variant_8007347C, on_word, 0x124);
    call_game(rdram, ctx, text_print_string_800734E0, (int16_t)x, y, on_word);
    return 1;
}

// func_803ABE0C at 0x803AC078: $t9 = the cursor row's option, about to be bounded for the left / right jump table;
// $a1 = the step (-1, 1, or 0 for the default). The BATTLE row toggles (the default is off) and is saved: in a circuit
// as the circuit's (rush2::circuit::race_battle; src/circuit.cpp says when a circuit lets it step). Returns 1 to go on
// to the case's end (0x803AC410).
extern "C" int rush2_track49_battle_step(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r25 != option_battle) return 0;
    bool on = (int32_t)ctx->r5 != 0 && !rush2::battle::race_battle();
    if (on != rush2::battle::race_battle()) {
        std::lock_guard lock{ menu_mutex };
        rush2::battle::set_race_battle(on);
        if (!rush2::circuit::set_race_battle(rdram, on)) {
            race_battle_selection = on;
            write_selection();
        }
    }
    return 1;
}

// DEATHS' third value, EXPLOSIVE (rush2::explosive, src/explosive.cpp): the row steps OFF, ON, EXPLOSIVE. The game's
// step (case 10 at 0x803AC3D0) flips menu +0x11 (xor 1, or 0 for the default) and in a circuit writes the circuit's
// DEATHS bit (circuit_set_deaths_800B29DC); func_80094698 then copies it to 0x8010BC98, which the value is drawn from.
namespace {
    constexpr uint32_t race_deaths = 0x8010BC98;     // s8: 0, 1, 2 (EXPLOSIVE)
    constexpr uint32_t menu_deaths = 0x800D5771;     // s8: the track select's DEATHS (0x800D5760 + 0x11)
    constexpr uint32_t explosive_label = menu_data + 0xF80;   // "EXPLOSIVE" in the menu's language (16 bytes)
}

extern "C" void circuit_set_deaths_800B29DC(uint8_t* rdram, recomp_context* ctx);

// func_803ABE0C at 0x803AC078 (as rush2_track49_battle_step): DEATHS steps through its three values from the one shown
// (0x8010BC98: a circuit's comes from its block and record, not menu +0x11), $a1 = -1, 1 or 0 for the default (off).
// Returns 1 to go on to the case's end (0x803AC410, which copies the settings to the race globals).
extern "C" int rush2_track49_deaths_step(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r25 != option_deaths) return 0;
    int step = (int32_t)ctx->r5;
    int now = std::min<int>(MEM_BU(0, (int32_t)race_deaths), rush2::explosive::deaths_value);
    int next = step == 0 ? 0 : (now + (step > 0 ? 1 : 2)) % 3;
    MEM_B(0, (int32_t)menu_deaths) = (int8_t)next;
    constexpr uint32_t game_mode = 0x8010C3E8;    // s32: 1 = circuit
    if ((int32_t)MEM_W(0, (int32_t)game_mode) == 1) {
        call_game(rdram, ctx, circuit_set_deaths_800B29DC, next != 0 ? 1 : 0);
        rush2::circuit::set_explosive(rdram, next == rush2::explosive::deaths_value);
    }
    return 1;
}

// func_803C6268 at 0x803C6840, DEATHS' value (at y $s2; $s4 -> the track, $s5 the id that greys it): with EXPLOSIVE the
// one word in the value box (centered where the game centers ON, which puts it in the box's middle), green or grey as
// the game draws the chosen one. Returns 1 to go on to the next row.
extern "C" int rush2_track49_deaths_value(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_BU(0, (int32_t)race_deaths) != rush2::explosive::deaths_value) return 0;
    static const char* const words[] = { "EXPLOSIVE", "EXPLOSIV", "EXPLOSIF" };    // English, German, French
    int lang = std::clamp((int32_t)MEM_W(0, (int32_t)language), 0, 2);
    write_string(rdram, explosive_label, words[lang]);
    int t = MEM_BU(0, (int32_t)ctx->r20);
    if (option_open(t, option_deaths)) t = 0;
    bool grey = (int32_t)ctx->r21 == t;
    int32_t y = (int16_t)ctx->r18;
    call_game(rdram, ctx, text_select_style_800737E4, grey ? 4 : 0xA);
    int x = call_game(rdram, ctx, text_center_x_variant_8007347C, (int32_t)explosive_label, 0x124);
    call_game(rdram, ctx, text_print_string_800734E0, (int16_t)x, y, (int32_t)explosive_label);
    return 1;
}

// CHECKPOINTS' third value, CLOCKS (rush2::time_sprint, src/time_sprint.cpp): on a track select entry with Time
// Sprint's clocks (SF Rush N64 courses 1-3, and the arcade and PS1 versions of them and of Chinatown) the row steps
// OFF, ON, CLOCKS. The game's step (case 9 at 0x803AC3A4) flips menu +0x19 (xor 1, or 1 for the default);
// func_80094698 copies it to 0x8010C17B (1 in a circuit), which the value is drawn from. Elsewhere the row is the
// game's: a CLOCKS kept from another track races as ON there and is drawn as ON.
namespace {
    constexpr uint32_t race_checkpoints = 0x8010C17B;   // u8: 0, 1, 2 (CLOCKS)
    constexpr uint32_t menu_checkpoints = 0x800D5779;   // s8: the track select's CHECKPOINTS (0x800D5760 + 0x19)
    constexpr uint32_t clocks_label = menu_data + 0xF90;   // "CLOCKS" in the menu's language (16 bytes)

    // Time Sprint draws the PS1's own clock, so it needs the sfrush_ps1 mod (rush2::time_sprint::available).
    bool clocks_offered(int t) {
        if (!rush2::time_sprint::available()) return false;
        if (t >= r1_first && t < r1_end) return rush2::time_sprint::has_clocks_n64(t - r1_first);
        int i = is_mod_track(t) ? mod_index(t) : -1;
        return i >= 0 && size_t(i) < mod_clocks.size() && mod_clocks[size_t(i)];
    }
}

// func_803ABE0C at 0x803AC078 (as rush2_track49_battle_step): CHECKPOINTS on a track with clocks steps through its
// three values from the one shown, $a1 = -1, 1 or 0 for the default (on). Returns 1 to go on to the case's end.
extern "C" int rush2_track49_clocks_step(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r25 != option_checkpoints || select_kind != select_race) return 0;
    if (!clocks_offered(MEM_BU(0, (int32_t)track_id))) return 0;
    int step = (int32_t)ctx->r5;
    int now = std::min<int>(MEM_BU(0, (int32_t)race_checkpoints), rush2::time_sprint::clocks_value);
    int next = step == 0 ? 1 : (now + (step > 0 ? 1 : 2)) % 3;
    MEM_B(0, (int32_t)menu_checkpoints) = (int8_t)next;
    return 1;
}

// func_803C6268 at 0x803C6770, CHECKPOINTS' value (at y $s2; $s4 -> the track, $s5 the id that greys it): with CLOCKS
// on a track with clocks, the one word in the value box as DEATHS' EXPLOSIVE. Returns 1 to go on to the next row.
extern "C" int rush2_track49_clocks_value(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_BU(0, (int32_t)race_checkpoints) != rush2::time_sprint::clocks_value) return 0;
    int t = MEM_BU(0, (int32_t)ctx->r20);
    if (!clocks_offered(t)) return 0;
    static const char* const words[] = { "CLOCKS", "UHREN", "HORLOGES" };    // English, German, French
    int lang = std::clamp((int32_t)MEM_W(0, (int32_t)language), 0, 2);
    write_string(rdram, clocks_label, words[lang]);
    bool grey = (int32_t)ctx->r21 == t;
    int32_t y = (int16_t)ctx->r18;
    call_game(rdram, ctx, text_select_style_800737E4, grey ? 4 : 0xA);
    int x = call_game(rdram, ctx, text_center_x_variant_8007347C, (int32_t)clocks_label, 0x124);
    call_game(rdram, ctx, text_print_string_800734E0, (int16_t)x, y, (int32_t)clocks_label);
    return 1;
}

// func_803C6268 at 0x803C6354: $s5 = 11, which the option list compares with the track to grey options.
extern "C" void rush2_track49_stunt_options(uint8_t* rdram, recomp_context* ctx) {
    int t = MEM_BU(0, (int32_t)track_id);
    if (is_stunt_course(t)) ctx->r21 = (uint64_t)(int64_t)t;
}

// func_803C5798 at 0x803C5930 / 0x803C5978: $t9 / $t8 = the track, about to be compared with 11 (an option's value
// is moved off screen on it): the DIFFICULTY slider (0x803C5930, at 0x8010C211 x 36 / 5) and the HANDICAP one. A battle
// arena keeps its DIFFICULTY slider.
extern "C" void rush2_track49_stunt_option_t9(uint8_t* rdram, recomp_context* ctx) {
    int t = (int32_t)ctx->r25;
    if (is_stunt_course(t) && !option_open(t, option_difficulty)) ctx->r25 = stunt_host_slot;
}

extern "C" void rush2_track49_stunt_option_t8(uint8_t* rdram, recomp_context* ctx) {
    if (is_stunt_course((int32_t)ctx->r24)) ctx->r24 = stunt_host_slot;
}

// The Start Game menu's GHOST RACE, STUNT and BATTLE rows and, while the unlock system is on, its UNLOCKS row
// (func_803B12C8, labels drawn by func_803C364C): ONE RACE, CIRCUIT, PRACTICE, GHOST RACE, STUNT, BATTLE, RECORDS,
// UNLOCKS, SETUP. BATTLE is left out without the Rush 2049 tracks (its arenas are theirs). Hooks after the cursor's
// wraps and the label loop's count make the menu 7 to 9 rows long; these hooks give the added rows their labels, map
// the other rows back to the stock options, and make the menu's box and bottom bar longer.
namespace {
    constexpr int stock_rows = 5;

    enum ModeRow : int { row_one_race, row_circuit, row_practice, row_ghost, row_stunt, row_battle, row_records,
                         row_unlocks, row_setup };

    // The menu's rows, top to bottom.
    std::vector<int> mode_rows() {
        std::vector<int> rows{ row_one_race, row_circuit, row_practice, row_ghost, row_stunt };
        if (available()) rows.push_back(row_battle);
        rows.push_back(row_records);
        if (rush2::unlocks::menu_row_shown()) rows.push_back(row_unlocks);
        rows.push_back(row_setup);
        return rows;
    }

    int mode_menu_rows() {
        return (int)mode_rows().size();
    }

    // The ModeRow on the menu's row `row`, or -1.
    int row_kind(int row) {
        std::vector<int> rows = mode_rows();
        return row >= 0 && row < (int)rows.size() ? rows[row] : -1;
    }

    // The stock option a row stands for (GHOST RACE, STUNT, BATTLE and UNLOCKS: the one whose path they take).
    int stock_row(int kind) {
        switch (kind) {
            case row_circuit: return 1;
            case row_practice: return 2;
            case row_records: return 3;
            case row_unlocks: return 3;     // RECORDS: its profile list, then the shop
            case row_setup: return 4;
            default: return 0;              // ONE RACE, and GHOST RACE, STUNT and BATTLE through it
        }
    }
}

// func_803B12C8 at 0x803B14B8: A or START was pressed, $t2 = the cursor, about to pick the stock option's code.
// STUNT and BATTLE take ONE RACE's (game mode 0; a race on a stunt track or battle arena becomes stunt mode, see
// rush2_track49_stunt_mode).
// UNLOCKS takes RECORDS' (its profile list, which then opens the shop: src/unlocks_shop.cpp). GHOST RACE takes ONE RACE's: the race it starts records player 1 and races the ghost.
extern "C" void rush2_mode_menu_choose(uint8_t* rdram, recomp_context* ctx) {
    int kind = row_kind((int32_t)ctx->r10);
    chosen_kind = kind;
    select_kind = kind == row_stunt ? select_stunt : kind == row_battle ? select_battle : select_race;
    rush2::ghost::set_chosen(kind == row_ghost);
    rush2::unlocks::set_shop_chosen(kind == row_unlocks);
    ctx->r10 = stock_row(kind);
}

static_assert((int)rush2::track2049::StartKind::Setup == row_setup);

rush2::track2049::StartKind rush2::track2049::start_menu_kind(int row) {
    return (StartKind)row_kind(row);
}

int rush2::track2049::start_menu_row(StartKind kind) {
    std::vector<int> rows = mode_rows();
    for (int row = 0; row < (int)rows.size(); row++) {
        if (rows[row] == (int)kind) return row;
    }
    return -1;
}

void rush2::track2049::set_all_tracks_open(bool on) {
    all_tracks_open = on;
}

std::vector<int> rush2::track2049::offered_tracks(uint8_t* rdram) {
    std::lock_guard lock{ menu_mutex };
    return shown_tracks(rdram);
}

void rush2::track2049::set_online_offered(uint8_t* rdram, const std::vector<int>& tracks) {
    std::array<bool, menu_tracks> offered{};
    std::array<bool, 3> kinds{};
    bool any_mod = false;
    for (int t : tracks) {
        if (t < 0 || t >= menu_tracks) continue;
        offered[t] = true;
        any_mod |= is_mod_track(t);
    }
    std::lock_guard lock{ menu_mutex };
    for (int t = 0; t < menu_tracks; t++) {
        if (is_mod_track(t)) offered[t] = any_mod;
        if (offered[t]) kinds[kind_of(t)] = true;
    }
    bool on = !tracks.empty();
    if (on == online_offered_on && (!on || offered == online_offered)) return;
    online_offered_on = on;
    online_offered = offered;
    offered_kinds = kinds;
    offered_rebuild = offered_rebuild || MEM_W(0, (int32_t)game_state) == 1;
}

rush2::track2049::StartKind rush2::track2049::chosen_start_kind() {
    return StartKind(chosen_kind.load());
}

std::string rush2::track2049::online_track_key(int t) {
    std::lock_guard lock{ menu_mutex };
    if (!is_mod_track(t) || !entry_available(t)) {
        return {};
    }
    return rush2::track_mod::identity(mod_index(t));
}

int rush2::track2049::track_for_online_key(const std::string& key) {
    if (key.empty()) {
        return -1;
    }
    std::lock_guard lock{ menu_mutex };
    for (size_t i = 0; i < mod_ok.size(); i++) {
        if (mod_ok[i] && rush2::track_mod::identity(int(i)) == key) {
            return mod_id(int(i));
        }
    }
    return -1;
}

int rush2::track2049::mod_track_id(const std::string& mod, const std::string& folder) {
    bool set;
    {
        std::lock_guard lock{ mod_keys_mutex };
        set = mod_keys_set;
    }
    if (!set) set_mod_keys(rush2::track_mod::tracks());
    std::lock_guard lock{ mod_keys_mutex };
    for (size_t i = 0; i < mod_keys.size() && i < size_t(mod_capacity); i++) {
        if (mod_keys[i].first == mod && mod_keys[i].second == folder) return mod_id(int(i));
    }
    return -1;
}

std::string rush2::track2049::mod_track_name(int t) {
    if (!is_mod_track(t)) {
        return {};
    }
    std::vector<rush2::track_mod::Entry> entries = rush2::track_mod::tracks();
    size_t i = size_t(mod_index(t));
    return i < entries.size() ? entries[i].settings.name : std::string();
}

void rush2::track2049::mods_changed() {
    rush2::track_mod::rescan();   // the mod's track list too: a rebuilt mod keeps its id and file name
    mods_reload = true;
}

// The carousel array (new_carousel, 0x1C bytes per entry, +0 the track (u8); func_803AB294 builds it) and its
// count (s16 0x803D05AE).
int rush2::track2049::carousel_index(uint8_t* rdram, int t) {
    constexpr uint32_t carousel = new_carousel;
    int count = std::min<int>(MEM_H(0, (int32_t)carousel_count), menu_tracks);
    for (int i = 0; i < count; i++) {
        if (MEM_BU(0, (int32_t)(carousel + i * carousel_entry)) == t) return i;
    }
    return -1;
}

// func_803B12C8 at 0x803B15FC (the cursor wrapped above the first row; $t8 is the row it goes to) and 0x803B1654
// ($at = whether the cursor, $t3, is still within the rows after moving down).
extern "C" void rush2_mode_menu_last_row(uint8_t* rdram, recomp_context* ctx) {
    ctx->r24 = mode_menu_rows() - 1;
}

extern "C" void rush2_mode_menu_below(uint8_t* rdram, recomp_context* ctx) {
    ctx->r1 = (int32_t)ctx->r11 < mode_menu_rows() ? 1 : 0;
}

// func_803C364C at 0x803C3754, the top of its label loop: $s7 = the number of labels drawn.
extern "C" void rush2_mode_menu_label_count(uint8_t* rdram, recomp_context* ctx) {
    ctx->r23 = mode_menu_rows();
}

namespace {
    // $s0 = the row; *base + $s1 (row x 4) is about to be read as the label of language table base.
    void label_table(uint8_t* rdram, recomp_context* ctx, uint64_t& base) {
        int row = (int32_t)ctx->r16;
        auto own = [&](uint32_t label_ptr) { base = (uint64_t)(int64_t)(int32_t)(label_ptr - (uint32_t)ctx->r17); };
        switch (int kind = row_kind(row)) {
            case row_ghost: own(ghost_label_ptr); break;
            case row_stunt: own(stunt_label_ptr); break;
            case row_battle: own(battle_label_ptr); break;
            case row_unlocks: own(unlocks_label_ptr); break;
            default:
                if (kind >= 0) base -= 4 * (row - stock_row(kind));
                break;
        }
    }
}

// func_803C364C at 0x803C37C4 / 0x803C380C: $t3 / $t1 = the language's label table (0x800C4B30 + language x 20),
// about to be indexed by $s1 for the label's width / drawing.
extern "C" void rush2_mode_menu_label_t3(uint8_t* rdram, recomp_context* ctx) {
    label_table(rdram, ctx, ctx->r11);
}

extern "C" void rush2_mode_menu_label_t1(uint8_t* rdram, recomp_context* ctx) {
    label_table(rdram, ctx, ctx->r9);
}

// func_803C3560 at 0x803C35D0: $t8 = 10 x the font height ($v0), the menu box's height less 20 (5 rows 2 lines
// apart). The added rows.
extern "C" void rush2_mode_menu_box(uint8_t* rdram, recomp_context* ctx) {
    ctx->r24 += 2 * ctx->r2 * (mode_menu_rows() - stock_rows);
}

// func_803C3560 at 0x803C3624: $t7 = the bottom bar's y less 0x42, from 10 x the font height ($t0). Lower by the
// added rows.
extern "C" void rush2_mode_menu_bar(uint8_t* rdram, recomp_context* ctx) {
    ctx->r15 += 2 * ctx->r8 * (mode_menu_rows() - stock_rows);
}
