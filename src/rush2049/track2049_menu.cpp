// Track select entries for the Rush 2049 and SF Rush tracks (docs/rush2049_research/menus.md).
//
// Rush 2's track select (menu overlay: func_803AB294 init and draw, func_803ABE0C per frame) cycles the track id byte
// through 0-11 and shows a carousel of dioramas built from 12-entry tables. With Rush 2049 tracks available it
// offers ids 12-17 after them, Rush 2049's stunt arenas as ids 25-28, its obstacle course as id 29 and its battle
// arenas DM1-DM8 as ids 30-37, and with SF Rush (Rush 1) tracks available ids 18-24:
// - The per-track tables the screen reads (diorama names 0x803C91E0, scales 0x803C9180, cloud heights 0x803C91B0,
//   logo names 0x803C9668) and its carousel array (0x803D0698, 0x1C bytes per entry) only have room for 12. Copies
//   with 38 entries live in the memory the game heap used before it moved (src/assets.cpp), and us.toml repoints the
//   instructions that address them. The loop bounds and wraps go from 12 to 38; func_803AB01C, which says whether a
//   track is unlocked, offers 12-17, 25-37 and 18-24 only while those tracks are available.
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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "util/file.h"

#include "data_files.h"
#include "rush2_hooks.h"
#include "assets.h"
#include "ghost.h"
#include "track1.h"
#include "track2049.h"
#include "unlocks.h"
#include "wings.h"

using namespace rush2::track2049;

namespace {
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t game_mode = 0x8010C3E8;       // 1 = circuit.
    constexpr uint32_t shown_track = 0x803D05AC;     // s16: the track the screen last showed.

    // The screen's 12-entry tables (overlay data).
    constexpr uint32_t diorama_names = 0x803C91E0;
    constexpr uint32_t diorama_scales = 0x803C9180;
    constexpr uint32_t cloud_heights = 0x803C91B0;
    constexpr uint32_t logo_names = 0x803C9668;

    // Their 30-entry copies (us.toml points the screen at these).
    constexpr uint32_t menu_data = 0x80300000;
    // The tables have room for 40 entries each (us.toml addresses them by these offsets).
    constexpr uint32_t new_diorama_names = menu_data + 0x000;
    constexpr uint32_t new_diorama_scales = menu_data + 0x0A0;
    constexpr uint32_t new_cloud_heights = menu_data + 0x140;
    constexpr uint32_t new_logo_names = menu_data + 0x1E0;
    // new carousel array at menu_data + 0x400 (38 x 0x1C)
    constexpr uint32_t new_circuit_instances = menu_data + 0x840; // 38 x s32, the circuit screen's dioramas
    constexpr uint32_t new_strings = menu_data + 0x900;           // 32 bytes per added track
    constexpr uint32_t stunt_label = menu_data + 0xD00;           // "STUNT", the Start Game menu's added row
    constexpr uint32_t stunt_label_ptr = menu_data + 0xD10;       // char* to it, read as the row's table entry
    constexpr uint32_t unlocks_label = menu_data + 0xD20;         // "UNLOCKS", the row of the unlock system's shop
    constexpr uint32_t unlocks_label_ptr = menu_data + 0xD30;
    constexpr uint32_t ghost_label = menu_data + 0xD40;           // "GHOST RACE"
    constexpr uint32_t ghost_label_ptr = menu_data + 0xD50;
    constexpr uint32_t battle_label = menu_data + 0xD60;          // "BATTLE"
    constexpr uint32_t battle_label_ptr = menu_data + 0xD70;
    constexpr uint32_t preview_strings = menu_data + 0x1000;      // 16 bytes per 2049 entry: its preview model's name

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
    constexpr int menu_tracks = battle_menu_id + battle_count;          // 38
    constexpr int added_tracks = menu_tracks - first_menu_id;           // 2049, Rush 1, stunt, obstacle and battle entries
    static_assert(first_menu_id + track_count == r1_first);
    static_assert(r1_end == stunt_menu_id);
    static_assert(stunt_menu_id + stunt_count == obstacle_menu_id);
    static_assert(obstacle_menu_id + 1 == battle_menu_id);

    // Track select entry t's preview model name (in RDRAM; 2049 entries), or 0 for its diorama.
    uint32_t preview_names[menu_tracks] = {};

    bool is_battle_arena(int t) {
        return t >= battle_menu_id && t < battle_menu_id + battle_count;
    }

    // A stunt or battle arena (hosted by STUNT1, which they are played as).
    bool is_arena(int t) {
        return (t >= stunt_menu_id && t < stunt_menu_id + stunt_count) || is_battle_arena(t);
    }

    // A stunt or battle arena or the obstacle course: the entries besides STUNT1 whose options the menus grey as
    // STUNT1's.
    bool is_stunt_course(int t) {
        return is_arena(t) || t == obstacle_menu_id;
    }

    // The track select's options (0x803D05D8[row], row = the cursor 0x803D05BC, count 0x803D05CC, top visible row
    // 0x803D05C4; func_803AB294 builds the list): 0 TRACK, 3 FOG, 4 WIND, 10 DEATHS.
    constexpr uint32_t option_rows = 0x803D05D8;
    constexpr uint32_t option_cursor = 0x803D05BC;
    constexpr uint32_t option_count = 0x803D05CC;
    constexpr uint32_t option_top = 0x803D05C4;
    constexpr int option_track = 0;
    constexpr int option_fog = 3;
    constexpr int option_wind = 4;
    constexpr int option_drones = 6;
    constexpr int option_difficulty = 7;
    constexpr int option_deaths = 10;
    constexpr int option_boxes = 4;     // OPTIONTEXTBOX widgets: the rows on screen

    // Whether option `option` stays open on track t although STUNT1 greys it: the obstacle course is raced (a car
    // can die on it), so DEATHS stays; a battle arena's DRONES is its computer opponents and DIFFICULTY their skill
    // (src/rush2049/battle_ai.cpp).
    bool option_open(int t, int option) {
        return (t == obstacle_menu_id && option == option_deaths) ||
               (is_battle_arena(t) && (option == option_drones || option == option_difficulty));
    }

    int cursor_option(uint8_t* rdram) {
        int row = (int32_t)MEM_W(0, (int32_t)option_cursor);
        return (int32_t)MEM_W(0, (int32_t)(option_rows + row * 4));
    }


    // Whether added track select entry t (12-37) can be chosen.
    bool entry_available(int t) {
        if (t >= first_menu_id && t < r1_first) return available();
        if (t >= r1_first && t < r1_end) return rush2::track1::available();
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

    // The option list func_803AB294 built for this visit, before filter_options.
    std::vector<int> stock_options;

    // The STUNT and BATTLE track selects list only the options that do something on their tracks: TRACK, FOG, WIND
    // and the options a course keeps open (option_open). STUNT1 grays out the others; a list with fewer options than the
    // rows on screen needs the hooks below (rush2_track49_option_*). Rows past the list hold TRACK, which shows no
    // slider. After a stunt or battle race the game mode is stunt (2, set by func_800AE670), whose list func_803AB294
    // stops at WIND, so the BATTLE select adds DRONES and DIFFICULTY back.
    void filter_options(uint8_t* rdram) {
        int t = (int8_t)MEM_B(0, (int32_t)track_id);
        std::vector<int> ids = stock_options;
        for (int added : { option_drones, option_difficulty }) {
            if (select_kind == select_battle && std::find(ids.begin(), ids.end(), added) == ids.end()) {
                ids.insert(std::upper_bound(ids.begin(), ids.end(), added), added);
            }
        }
        int n = 0;
        for (int id : ids) {
            if (select_kind != select_race && id != option_track && id != option_fog && id != option_wind &&
                !option_open(t, id)) {
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

    // Whether the track select offers track t (0-37): func_803AB01C's unlocks, the added tracks, and the stunt and
    // battle filter.
    bool track_selectable(uint8_t* rdram, int t) {
        bool unlocked;
        if (t < 9) unlocked = true;
        else if (t == 9) unlocked = rush2::unlocks::track_open(rdram, t, MEM_BU(0, (int32_t)pipe_unlocked) != 0);
        else if (t == 10) unlocked = rush2::unlocks::track_open(rdram, t, MEM_BU(0, (int32_t)atari_unlocked) != 0);
        else if (t == stunt_host_slot) unlocked = true;
        else unlocked = entry_available(t) && rush2::unlocks::track_open(rdram, t);
        return unlocked && kind_of(t) == select_kind.load();
    }

    std::mutex menu_mutex;
    int selection = -1;            // Added track (id - 12) last chosen on the track select, or -1.
    int stunt_selection = -1;      // Track (11 or 25-29) last chosen on the stunt track select, or -1.
    int battle_selection = -1;     // Track (30-37) last chosen on the battle track select, or -1.
    bool selection_loaded = false;
    std::vector<uint8_t> menu_container;
    std::shared_ptr<const rush2::rom2049::Source> menu_container_rom; // 2049 ROM the container was built with.
    std::shared_ptr<const std::vector<uint8_t>> menu_container_rom1;   // Rush 1 ROM it was built with.
    bool menu_container_built = false;
    // What the container has: the tables name only art that is in it (a missing logo is a null texture the track
    // select reads through).
    bool menu_has_2049 = false;
    bool menu_has_rush1 = false;

    // The save file's section (include/data_files.h): { "selected": n, "stunt": n, "battle": n }.
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
    }

    void write_selection() {
        rush2::data_files::write(rush2::data_files::File::Saves, selection_section,
            "{ \"selected\": " + std::to_string(selection) + ", \"stunt\": " + std::to_string(stunt_selection) +
            ", \"battle\": " + std::to_string(battle_selection) + " }");
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

    // Fills the 38-entry tables: Rush 2's 12 entries, then the 2049 tracks', the Rush 1 tracks', the stunt arenas', the
    // obstacle course's and the battle arenas'.
    void write_tables(uint8_t* rdram) {
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
    }

    // Asset 3 with the 2049 and Rush 1 dioramas and logos, rebuilt when a ROM or option changes.
    void serve_menu_container(uint8_t* rdram) {
        auto rom = available() ? rush2::wings::get_rom() : nullptr;
        auto rom1 = rush2::track1::available() ? rush2::track1::get_rom() : nullptr;
        if (rom == nullptr && rom1 == nullptr) {
            rush2::assets::restore(rdram, 3);
            menu_has_2049 = menu_has_rush1 = false;
            return;
        }
        if (menu_container_built && menu_container_rom == rom && menu_container_rom1 == rom1 && !menu_container.empty() &&
            rush2::assets::is_replaced(3)) {
            return;
        }
        if (!menu_container_built || menu_container_rom != rom || menu_container_rom1 != rom1) {
            menu_container.clear();
            menu_container_rom = rom;
            menu_container_rom1 = rom1;
            menu_container_built = true;
            menu_has_2049 = menu_has_rush1 = false;
            std::vector<uint8_t> asset3, with_2049;
            if (!rush2::assets::read_original(rdram, 3, asset3)) {
                printf("[2049] Failed to read the track select art\n");
            }
            else {
                if (rom != nullptr && !build_menu_container(asset3, *rom, with_2049)) {
                    printf("[2049] Failed to build the track select art\n");
                    with_2049.clear();
                }
                menu_has_2049 = !with_2049.empty();
                const std::vector<uint8_t>& base = with_2049.empty() ? asset3 : with_2049;
                if (rom1 == nullptr || !rush2::track1::extend_menu_container(base, *rom1, menu_container)) {
                    menu_container = with_2049;
                }
                else {
                    menu_has_rush1 = true;
                }
            }
        }
        if (menu_container.empty()) {
            rush2::assets::restore(rdram, 3);
            menu_has_2049 = menu_has_rush1 = false;
        }
        else {
            rush2::assets::replace(rdram, 3, menu_container);
        }
    }

    // Turns an added track's menu id into its host slot for the race.
    void enter_race(uint8_t* rdram) {
        int t = (int8_t)MEM_B(0, (int32_t)track_id);
        if (t >= first_menu_id && t < r1_first) {
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
            if (t != host_slot) {
                set_race_track(0);
                rush2::track1::set_race_track(0);
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
void rush2::track2049::prepare_menu_art(uint8_t* rdram) {
    std::lock_guard lock{ menu_mutex };
    serve_menu_container(rdram);
    write_tables(rdram);
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
    load_selection();
    set_race_track(0);
    rush2::track1::set_race_track(0);
    set_stunt_arena(0);
    set_battle_arena(0);
    restore_host(rdram);
    rush2::track1::restore_host(rdram);
    serve_menu_container(rdram);
    write_tables(rdram);
    int t = -1;
    if (select_kind == select_stunt) {
        // The game's choice (from the save record's nibble) is a race track, which this select doesn't offer.
        t = stunt_selection >= 0 && track_selectable(rdram, stunt_selection) ? stunt_selection : stunt_host_slot;
    }
    else if (select_kind == select_battle) {
        t = battle_selection >= 0 && track_selectable(rdram, battle_selection) ? battle_selection : battle_menu_id;
    }
    else if (selection >= 0 && track_selectable(rdram, first_menu_id + selection) && MEM_W(0, (int32_t)game_mode) != 1) {
        t = first_menu_id + selection;
    }
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

// func_803AB294 at 0x803AB6C8: $t7 = the number of carousel entries (10 + unlocked PIPE and ATARI), which must be
// the number of tracks the build loop finds selectable.
extern "C" void rush2_track49_select_count(uint8_t* rdram, recomp_context* ctx) {
    int count = 0;
    for (int t = 0; t < menu_tracks; t++) {
        if (track_selectable(rdram, t)) count++;
    }
    ctx->r15 = count;
}

// Rush 2049's own preview for the 2049 entries (docs/rush2049_research/menus.md §7.5): the carousel instances the
// track's preview model (src/rush2049/track2049_art.cpp, menu_preview_name) in place of its diorama, and each frame the preview
// is posed so its model space, Rush 2049's track select camera space, lands in front of Rush 2's camera with 2049's
// screen layout moved to the middle of the diorama area: the screenshot centred between the carousel arrows, the model
// in front of it as 2049 shows it, still (no carousel spin). Race tracks' tubes get 2049's moving white highlight.
extern "C" void model_find_by_name_8005BE3C(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t camera = 0x800E79D0;       // view 0's camera: rows -right, up, forward, then +0x24 position
    constexpr uint32_t view = 0x802401F0;         // view 0's projection (func_80054A50): +0x14 tan(hfov / 2),
                                                  // +0x24 width, +0x2C/+0x30 the screen point straight ahead
    constexpr uint32_t node_poses = 0x800D9E94;   // per scene node, 56 bytes: -> {matrix[9], position[3]}
    constexpr uint32_t model_tables = 0x80118D78; // per model slot, 8 bytes: -> its 0x34-byte model records
    constexpr uint32_t model_slots = 0x800D5788;  // u8: loaded model slots
    constexpr uint32_t frame_seconds = 0x80023028;

    // Where the preview's screenshot is centred on Rush 2's 320 x 240 screen (2049 draws it at (176, 32) - (304, 160)).
    constexpr float preview_disc_center_x = 160.0f, preview_disc_center_y = 108.0f;
    constexpr float preview_depth = 110.0f;       // 2049 camera units: about the preview's middle, put at the diorama's depth

    float preview_phase = 0.0f;                   // the tube highlight's run along the route (0-1, one per 5 s)
    float preview_angle = 0.0f;                   // 0x803B8350: the model's turn (radians, one turn per 10 s)
    // Per entry: the RDRAM address and own-space positions (preview_unplace) of the model's vertices, looked up once.
    struct PreviewModel {
        bool looked_up = false;
        uint32_t vertices = 0;
        std::vector<std::array<float, 3>> local;
    };
    PreviewModel preview_models[menu_tracks];

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

    // The vertices the preview model's list loads before its screenshot (the tube's or the outline's).
    void find_model_vertices(uint8_t* rdram, recomp_context* ctx, int t, PreviewModel& out) {
        out = PreviewModel();
        out.looked_up = true;
        recomp_context saved = *ctx;
        ctx->r4 = (int32_t)preview_names[t];
        ctx->r5 = 0;
        ctx->r6 = MEM_BU(0, (int32_t)model_slots) - 1;
        ctx->r7 = 1;
        model_find_by_name_8005BE3C(rdram, ctx);
        int32_t handle = (int32_t)ctx->r2;
        *ctx = saved;
        if (handle < 0 || (handle & 0xFFFF) == 0xFFFF) return;
        uint32_t table = (uint32_t)MEM_W(0, (int32_t)(model_tables + (uint32_t)(handle >> 10) * 8));
        uint32_t list = (uint32_t)MEM_W(0, (int32_t)(table + (uint32_t)(handle & 0x3FF) * 0x34 + 0xC));
        uint32_t first = 0xFFFFFFFF, end = 0;
        for (int i = 0; i < 4096 && (list >> 24) == 0x80; i++, list += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)list), w1 = (uint32_t)MEM_W(4, (int32_t)list);
            uint32_t op = w0 >> 24;
            if (op == 0xFD || op == 0xDF) break;
            if (op != 0x01) continue;
            uint32_t at = 0x80000000 | (w1 & 0x00FFFFFF);
            first = std::min(first, at);
            end = std::max(end, at + ((w0 >> 12) & 0xFF) * 16);
        }
        if (end <= first || end - first > 0x10000) return;
        out.vertices = first;
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
    int t = (int8_t)MEM_B(0, (int32_t)ctx->r16);
    if (t >= 0 && t < menu_tracks && preview_names[t] != 0) {
        ctx->r4 = (int32_t)preview_names[t];
        preview_models[t] = PreviewModel();
    }
}

// func_803AB294 at 0x803ABCA0, each frame: carousel entry $s0's node was just given its place and spun matrix.
extern "C" void rush2_track49_select_pose(uint8_t* rdram, recomp_context* ctx) {
    uint32_t entry = (uint32_t)ctx->r16;
    int t = (int8_t)MEM_B(0, (int32_t)entry);
    if (t < 0 || t >= menu_tracks || preview_names[t] == 0) return;

    float rows[3][3], cam[3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) rows[r][c] = rdram_float(rdram, camera + uint32_t(r * 3 + c) * 4);
        cam[r] = rdram_float(rdram, camera + 0x24 + uint32_t(r) * 4);
    }
    const float right[3] = { -rows[0][0], -rows[0][1], -rows[0][2] };
    const float* up = rows[1];
    const float* forward = rows[2];
    float tan_h = rdram_float(rdram, view + 0x14), width = rdram_float(rdram, view + 0x24);
    float cx = rdram_float(rdram, view + 0x2C), cy = rdram_float(rdram, view + 0x30);
    if (!(tan_h > 0.01f) || !(width > 0.0f)) return;
    const float focal = width * 0.5f / tan_h;

    // The carousel's centre entry sits at (0, y, z) and the others slide along x.
    float place[3] = { rdram_float(rdram, entry + 4), rdram_float(rdram, entry + 8), rdram_float(rdram, entry + 12) };
    float depth = 0;
    for (int i = 0; i < 3; i++) depth += forward[i] * ((i == 0 ? 0.0f : place[i]) - cam[i]);
    if (depth <= 1.0f) return;

    // 2049 camera space (X, Y, Z) to Rush 2's (right, up, forward): Z' = a Z, X' = a f1 / f2 X + b Z, Y' = a f1 / f2 Y
    // + c Z, so Rush 2's projection shows each point at 2049's screen point plus a constant offset.
    using namespace rush2::track2049;
    const float a = depth / preview_depth;
    const float scale = a * preview_focal / focal;
    const float dx = preview_disc_center_x - 240.0f, dy = preview_disc_center_y - 96.0f;
    const float b = a * (preview_center_x + dx - cx) / focal;
    const float c = a * (cy - preview_center_y - dy) / focal;
    float m[3][3];
    for (int i = 0; i < 3; i++) {
        m[0][i] = scale * right[i];
        m[1][i] = scale * up[i];
        m[2][i] = b * right[i] + c * up[i] + a * forward[i];
    }
    int16_t node = (int16_t)MEM_H(0, (int32_t)(entry + 0x1A));
    uint32_t pose = (uint32_t)MEM_W(0, (int32_t)(node_poses + node * 56));
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 3; i++) write_float(rdram, pose + uint32_t(r * 3 + i) * 4, m[r][i]);
    }
    write_float(rdram, pose + 0x24, cam[0] + place[0]);
    write_float(rdram, pose + 0x28, cam[1]);
    write_float(rdram, pose + 0x2C, cam[2]);

    // The highlight runs once along the route every 5 seconds (0x803B7CE0 += dt / 5) and the model turns once
    // every 10 (0x803B8350 += dt x 2 pi / 10).
    if ((int8_t)MEM_B(0, (int32_t)(entry + 0x14)) == 0) {
        float dt = rdram_float(rdram, frame_seconds);
        preview_phase += dt / 5.0f;
        preview_phase -= std::floor(preview_phase);
        preview_angle = std::fmod(preview_angle + dt * 6.2831853f / 10.0f, 6.2831853f);
    }
    PreviewModel& model = preview_models[t];
    if (!model.looked_up) find_model_vertices(rdram, ctx, t, model);
    if (model.vertices == 0) return;
    for (size_t v = 0; v < model.local.size(); v++) {
        float placed[3];
        preview_place(model.local[v].data(), preview_angle, placed);
        uint32_t at = model.vertices + uint32_t(v) * 16;
        for (int i = 0; i < 3; i++) {
            MEM_H(i * 2, (int32_t)at) = (int16_t)std::clamp(std::lround(placed[i]), -32767L, 32767L);
        }
    }
    int k = t - first_menu_id + 1;
    if (k < 1 || k > track_count || model.local.size() < size_t(preview_tube_rings) * 4) return;
    for (int ring = 0; ring < preview_tube_rings; ring++) {
        uint8_t rgb[3];
        preview_tube_color(k, ring, preview_phase, rgb);
        for (int v = 0; v < 2; v++) {
            uint32_t at = model.vertices + uint32_t(ring * 4 + v) * 16 + 12;
            MEM_B(0, (int32_t)at) = rgb[0];
            MEM_B(1, (int32_t)at) = rgb[1];
            MEM_B(2, (int32_t)at) = rgb[2];
        }
    }
}

// Start of func_803AB01C: whether track $a0 can be chosen. Replaces the function.
extern "C" int rush2_track49_select_available(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = track_selectable(rdram, (int32_t)ctx->r4) ? 1 : 0;
    return 1;
}

// func_803ABE0C at 0x803AC104: the TRACK option wrapped below 0 and stored 11 at $a3; wrap to the last added track
// instead (the availability check that follows steps back past entries that are off).
extern "C" void rush2_track49_select_wrap(uint8_t* rdram, recomp_context* ctx) {
    MEM_B(0, (int32_t)ctx->r7) = uint8_t(menu_tracks - 1);
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
    if (!rush2::unlocks::added_track_keys(rdram, (int32_t)ctx->r4, (int8_t)MEM_B(0, (int32_t)track_id),
                                          (ctx->r6 & 0xFF) != 0, count)) {
        return 0;
    }
    MEM_B(0, (int32_t)ctx->r5) = (int8_t)count;
    return 1;
}

// func_800A5ADC at 0x800A5B2C (race start, before the race's cars and objects are set up) and the start of
// func_800A5110 (race setup, also reached directly by func_800A57C8).
extern "C" void rush2_track49_race_start(uint8_t* rdram, recomp_context* ctx) {
    enter_race(rdram);
}

// func_800A62C0 after the menu overlay is loaded: the overlay's own data starts fresh, and so must the circuit
// screen's diorama instances that moved out of it.
extern "C" void rush2_track49_overlay_loaded(uint8_t* rdram, recomp_context* ctx) {
    for (int t = 0; t < menu_tracks; t++) {
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
    serve_menu_container(rdram);
    write_tables(rdram);
}

// func_800A7DCC after it generated a circuit's races (tracks 0-6). With added tracks available, the tracks are
// picked again from Rush 2's 7 and the added tracks, with Rush 2's rules: no track twice in a row, at most 4 times in
// all, and no track in the same direction twice.
extern "C" void rush2_track49_circuit(uint8_t* rdram, recomp_context* ctx) {
    if (!available() && !rush2::track1::available()) {
        return;
    }
    int last = (int8_t)MEM_B(0, (int32_t)circuit_last);
    std::vector<int> pool;
    for (int t = 0; t < circuit_stock_tracks; t++) pool.push_back(t);
    for (int t = first_menu_id; t < menu_tracks; t++) {
        if (entry_available(t) && !is_stunt_course(t) && rush2::unlocks::track_open(rdram, t)) pool.push_back(t);
    }
    // Seeded from the game's random state ($s0 points at it), so the same circuit seed gives the same races.
    std::mt19937 rng{ (uint32_t)MEM_W(0, (int32_t)ctx->r16) };
    std::map<int, int> uses;
    std::set<std::pair<int, int>> seen; // (track, direction bits)
    int previous = -1;
    for (int i = 0; i <= last; i++) {
        uint32_t race = circuit_races + i * 4;
        std::vector<int> choices;
        for (int t : pool) {
            if (t != previous && uses[t] < circuit_max_uses) choices.push_back(t);
        }
        if (choices.empty()) {
            choices = pool;
        }
        int t = choices[std::uniform_int_distribution<size_t>(0, choices.size() - 1)(rng)];
        int dir = MEM_B(0, (int32_t)(race + 1)) & 3;
        for (int k = 0; k < 4 && seen.contains({ t, dir }); k++) {
            dir = (dir + 1) & 3;
        }
        seen.insert({ t, dir });
        uses[t]++;
        previous = t;
        MEM_B(0, (int32_t)race) = uint8_t(t);
        MEM_B(0, (int32_t)(race + 1)) = uint8_t(dir);
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

// func_803C6268 at 0x803C6354: $s5 = 11, which the option list compares with the track to grey options.
extern "C" void rush2_track49_stunt_options(uint8_t* rdram, recomp_context* ctx) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
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
    select_kind = kind == row_stunt ? select_stunt : kind == row_battle ? select_battle : select_race;
    rush2::ghost::set_chosen(kind == row_ghost);
    rush2::unlocks::set_shop_chosen(kind == row_unlocks);
    ctx->r10 = stock_row(kind);
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
