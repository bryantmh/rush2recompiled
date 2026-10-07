// Track select entries for the Rush 2049 and SF Rush tracks (docs/rush2049_research/menus.md).
//
// Rush 2's track select (menu overlay: func_803AB294 init and draw, func_803ABE0C per frame) cycles the track id byte
// through 0-11 and shows a carousel of dioramas built from 12-entry tables. With Rush 2049 tracks available it
// offers ids 12-17 after them, Rush 2049's stunt arenas as ids 25-28 and its obstacle course as id 29, and with SF
// Rush (Rush 1) tracks available ids 18-24:
// - The per-track tables the screen reads (diorama names 0x803C91E0, scales 0x803C9180, cloud heights 0x803C91B0,
//   logo names 0x803C9668) and its carousel array (0x803D0698, 0x1C bytes per entry) only have room for 12. Copies
//   with 30 entries live in the memory the game heap used before it moved (src/assets.cpp), and us.toml repoints the
//   instructions that address them. The loop bounds and wraps go from 12 to 30; func_803AB01C, which says whether a
//   track is unlocked, offers 12-17, 25-29 and 18-24 only while those tracks are available.
// - The menus test for the stunt track (11) by id: its options other than TRACK, FOG and WIND are greyed and
//   func_80094698 turns backward and mirror off. Hooks make those tests treat the stunt arenas' and the obstacle
//   course's ids as 11, except the test that sets stunt mode: the obstacle course is raced (src/track2049.cpp).
// - The dioramas and logos come from a generated copy of asset 3 (src/track2049_art.cpp, then
//   rush2::track1::extend_menu_container for the Rush 1 ones).
// - The screen saves the chosen track as the low nibble of byte +0x30 of the player's save record, which can't hold
//   12-29, so such a choice leaves the nibble alone and is kept in track2049.json instead, and restored when the
//   screen opens.
// - Car select counts the track's collected keys from 12-entry tables (func_803B1AB0); added tracks have none.
// - The Start Game menu has a GHOST RACE row (src/ghost.cpp) and a STUNT row after PRACTICE. Through STUNT the track
//   select offers STUNT1, the stunt arenas and the obstacle course only, remembered apart (track2049.json "stunt");
//   through the other rows it leaves them out.
//
// When a race starts on id 12-29, the id becomes the host slot's (STUNT1's for a stunt arena) and the added track is
// noted for the track hooks (src/track2049.cpp, src/track1.cpp). The host slot keeps its id through restarts; opening
// the track select clears it again. Circuits never pick a stunt arena or the obstacle course, as they never pick
// STUNT1.

#include <atomic>
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
    constexpr uint32_t new_diorama_names = menu_data + 0x000;
    constexpr uint32_t new_diorama_scales = menu_data + 0x080;
    constexpr uint32_t new_cloud_heights = menu_data + 0x100;
    constexpr uint32_t new_logo_names = menu_data + 0x180;
    // new carousel array at menu_data + 0x400 (30 x 0x1C)
    constexpr uint32_t new_circuit_instances = menu_data + 0x780; // 30 x s32, the circuit screen's dioramas
    constexpr uint32_t new_strings = menu_data + 0x800;           // 32 bytes per added track
    constexpr uint32_t stunt_label = menu_data + 0xC00;           // "STUNT", the Start Game menu's added row
    constexpr uint32_t stunt_label_ptr = menu_data + 0xC10;       // char* to it, read as the row's table entry
    constexpr uint32_t unlocks_label = menu_data + 0xC20;         // "UNLOCKS", the row of the unlock system's shop
    constexpr uint32_t unlocks_label_ptr = menu_data + 0xC30;
    constexpr uint32_t ghost_label = menu_data + 0xC40;           // "GHOST RACE"
    constexpr uint32_t ghost_label_ptr = menu_data + 0xC50;

    // Unlock bytes of PIPE (9) and ATARI (10) (func_803AB01C).
    constexpr uint32_t pipe_unlocked = 0x800E7D50;
    constexpr uint32_t atari_unlocked = 0x800E7D19;

    // Start Game menu rows: ONE RACE, CIRCUIT, PRACTICE, GHOST RACE, STUNT, RECORDS, SETUP. The stock menu has the
    // five without GHOST RACE and STUNT; the rows after them map back to the stock options.
    constexpr int ghost_row = 3;
    constexpr int stunt_row = 4;

    // Circuit mode: the race list func_800A7DCC generates, 4 bytes per race (track, direction bits, fog, wind).
    constexpr uint32_t circuit_races = 0x800D3A60;
    constexpr uint32_t circuit_last = 0x800D3DF0;    // s8: index of the last race
    constexpr int circuit_stock_tracks = 7;          // Rush 2 draws circuits from tracks 0-6
    constexpr int circuit_max_uses = 4;

    constexpr int rush2_tracks = 12;
    constexpr int r1_first = rush2::track1::first_menu_id;               // 18
    constexpr int r1_end = r1_first + rush2::track1::track_count;       // 25
    constexpr int menu_tracks = obstacle_menu_id + 1;                   // 30
    constexpr int added_tracks = menu_tracks - first_menu_id;           // 2049, Rush 1, stunt arena and obstacle entries
    static_assert(first_menu_id + track_count == r1_first);
    static_assert(r1_end == stunt_menu_id);
    static_assert(stunt_menu_id + stunt_count == obstacle_menu_id);

    bool is_arena(int t) {
        return t >= stunt_menu_id && t < stunt_menu_id + stunt_count;
    }

    // A stunt arena or the obstacle course: the entries the STUNT row offers besides STUNT1, whose options the menus
    // grey as STUNT1's.
    bool is_stunt_course(int t) {
        return is_arena(t) || t == obstacle_menu_id;
    }

    // The track select's options (0x803D05D8[row], row = the cursor 0x803D05BC): 0 TRACK, 3 FOG, 4 WIND, 10 DEATHS.
    constexpr uint32_t option_rows = 0x803D05D8;
    constexpr uint32_t option_cursor = 0x803D05BC;
    constexpr int option_deaths = 10;

    // Whether option `option` stays open on track t although STUNT1 greys it: the obstacle course is raced (a car
    // can die on it), so DEATHS stays.
    bool option_open(int t, int option) {
        return t == obstacle_menu_id && option == option_deaths;
    }

    int cursor_option(uint8_t* rdram) {
        int row = (int32_t)MEM_W(0, (int32_t)option_cursor);
        return (int32_t)MEM_W(0, (int32_t)(option_rows + row * 4));
    }

    // Whether added track select entry t (12-29) can be chosen.
    bool entry_available(int t) {
        if (t >= first_menu_id && t < r1_first) return available();
        if (t >= r1_first && t < r1_end) return rush2::track1::available();
        if (is_stunt_course(t)) return available();
        return false;
    }

    bool is_stunt_track(int t) {
        return t == stunt_host_slot || is_stunt_course(t);
    }

    // Whether the track select was reached through the Start Game menu's STUNT row. It then offers STUNT1, the stunt
    // arenas and the obstacle course only, and otherwise leaves them out.
    std::atomic<bool> stunt_select = false;

    // Whether the track select offers track t (0-29): func_803AB01C's unlocks, the added tracks, and the stunt
    // filter.
    bool track_selectable(uint8_t* rdram, int t) {
        bool unlocked;
        if (t < 9) unlocked = true;
        else if (t == 9) unlocked = rush2::unlocks::track_open(rdram, t, MEM_BU(0, (int32_t)pipe_unlocked) != 0);
        else if (t == 10) unlocked = rush2::unlocks::track_open(rdram, t, MEM_BU(0, (int32_t)atari_unlocked) != 0);
        else if (t == stunt_host_slot) unlocked = true;
        else unlocked = entry_available(t) && rush2::unlocks::track_open(rdram, t);
        return unlocked && is_stunt_track(t) == stunt_select.load();
    }

    std::mutex menu_mutex;
    int selection = -1;            // Added track (id - 12) last chosen on the track select, or -1.
    int stunt_selection = -1;      // Track (11 or 25-29) last chosen on the stunt track select, or -1.
    bool selection_loaded = false;
    std::vector<uint8_t> menu_container;
    std::shared_ptr<const std::vector<uint8_t>> menu_container_rom;    // 2049 ROM the container was built with.
    std::shared_ptr<const std::vector<uint8_t>> menu_container_rom1;   // Rush 1 ROM it was built with.
    bool menu_container_built = false;

    std::filesystem::path selection_path() {
        return recompui::file::get_app_folder_path() / "track2049.json";
    }

    void load_selection() {
        if (selection_loaded) {
            return;
        }
        selection_loaded = true;
        std::ifstream f(selection_path());
        std::string text((std::istreambuf_iterator<char>(f)), {});
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
    }

    void write_selection() {
        std::ofstream f(selection_path());
        f << "{ \"selected\": " << selection << ", \"stunt\": " << stunt_selection << " }\n";
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

    void write_string(uint8_t* rdram, uint32_t addr, const std::string& s) {
        for (size_t i = 0; i <= s.size(); i++) {
            MEM_B(0, (int32_t)(addr + i)) = i < s.size() ? s[i] : 0;
        }
    }

    // Fills the 30-entry tables: Rush 2's 12 entries, then the 2049 tracks', the Rush 1 tracks', the stunt arenas' and
    // the obstacle course's.
    void write_tables(uint8_t* rdram) {
        for (int t = 0; t < rush2_tracks; t++) {
            MEM_W(0, (int32_t)(new_diorama_names + t * 4)) = MEM_W(0, (int32_t)(diorama_names + t * 4));
            MEM_W(0, (int32_t)(new_diorama_scales + t * 4)) = MEM_W(0, (int32_t)(diorama_scales + t * 4));
            MEM_W(0, (int32_t)(new_cloud_heights + t * 4)) = MEM_W(0, (int32_t)(cloud_heights + t * 4));
            MEM_W(0, (int32_t)(new_logo_names + t * 4)) = MEM_W(0, (int32_t)(logo_names + t * 4));
        }
        uint32_t s = new_strings;
        // The circuit screen instances every entry: entries that aren't available show stock models it can find.
        for (int t = rush2_tracks; t < menu_tracks; t++) {
            MEM_W(0, (int32_t)(new_diorama_names + t * 4)) = MEM_W(0, (int32_t)diorama_names);
            MEM_W(0, (int32_t)(new_logo_names + t * 4)) = MEM_W(0, (int32_t)logo_names);
            MEM_W(0, (int32_t)(new_diorama_scales + t * 4)) = MEM_W(0, (int32_t)diorama_scales);
            MEM_W(0, (int32_t)(new_cloud_heights + t * 4)) = MEM_W(0, (int32_t)cloud_heights);
        }
        auto rom1 = rush2::track1::get_rom();
        if (rush2::track1::available() && rom1 != nullptr) {
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
        if (!available()) {
            return;
        }
        std::vector<std::pair<int, int>> entries; // (menu id, convert_track's k)
        for (int k = 1; k <= track_count; k++) entries.push_back({ first_menu_id + k - 1, k });
        for (int n = 0; n < stunt_count; n++) entries.push_back({ stunt_menu_id + n, stunt_first + n });
        entries.push_back({ obstacle_menu_id, obstacle });
        for (auto [t, k] : entries) {
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
            std::vector<uint8_t> asset3, with_2049;
            if (!rush2::assets::read_original(rdram, 3, asset3)) {
                printf("[2049] Failed to read the track select art\n");
            }
            else {
                if (rom != nullptr && !build_menu_container(asset3, *rom, with_2049)) {
                    printf("[2049] Failed to build the track select art\n");
                    with_2049.clear();
                }
                const std::vector<uint8_t>& base = with_2049.empty() ? asset3 : with_2049;
                if (rom1 == nullptr || !rush2::track1::extend_menu_container(base, *rom1, menu_container)) {
                    menu_container = with_2049;
                }
            }
        }
        if (menu_container.empty()) {
            rush2::assets::restore(rdram, 3);
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
            MEM_B(0, (int32_t)track_id) = host_slot;
        }
        else if (t >= r1_first && t < r1_end) {
            set_race_track(0);
            rush2::track1::set_race_track(t - r1_first + 1);
            set_stunt_arena(0);
            MEM_B(0, (int32_t)track_id) = host_slot;
        }
        else if (is_arena(t)) {
            set_race_track(0);
            rush2::track1::set_race_track(0);
            set_stunt_arena(t - stunt_menu_id + 1);
            MEM_B(0, (int32_t)track_id) = stunt_host_slot;
        }
        else if (t == obstacle_menu_id) {
            set_race_track(obstacle);
            rush2::track1::set_race_track(0);
            set_stunt_arena(0);
            MEM_B(0, (int32_t)track_id) = host_slot;
        }
        else {
            if (t != host_slot) {
                set_race_track(0);
                rush2::track1::set_race_track(0);
            }
            if (t != stunt_host_slot) {
                set_stunt_arena(0);
            }
        }
    }
}

// func_803AB294 at 0x803AB5C0, once per track select visit: the initial track is chosen and the screen's assets are
// about to load. $s0 = &track id.
void rush2::track2049::prepare_menu_art(uint8_t* rdram) {
    std::lock_guard lock{ menu_mutex };
    write_tables(rdram);
    serve_menu_container(rdram);
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
    restore_host(rdram);
    rush2::track1::restore_host(rdram);
    write_tables(rdram);
    serve_menu_container(rdram);
    int t = -1;
    if (stunt_select) {
        // The game's choice (from the save record's nibble) is a race track, which this select doesn't offer.
        t = stunt_selection >= 0 && track_selectable(rdram, stunt_selection) ? stunt_selection : stunt_host_slot;
    }
    else if (selection >= 0 && entry_available(first_menu_id + selection) && MEM_W(0, (int32_t)game_mode) != 1) {
        t = first_menu_id + selection;
    }
    if (t >= 0) {
        MEM_B(0, (int32_t)track_id) = uint8_t(t);
        MEM_H(0, (int32_t)shown_track) = int16_t(t);
    }
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
    if (stunt_select) {
        ctx->r25 = ctx->r15;
        save_stunt_selection(t);
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
    if (stunt_select || (int32_t)ctx->r25 >= first_menu_id) {
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
}

// Start of func_803B6260, the circuit screen (every frame). It shows the dioramas and logos of the circuit's races
// from the 29-entry tables (us.toml), so they and the 2049 art must be in place.
extern "C" void rush2_track49_circuit_screen(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ menu_mutex };
    write_tables(rdram);
    serve_menu_container(rdram);
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

// The menus' stunt track tests (see the top of this file): a stunt arena's id counts as STUNT1's, and so does the
// obstacle course's except for stunt mode.

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

// func_803C6268, the DEATHS value (option 10's case) at 0x803C6858 ($t4) / 0x803C68AC ($t0): the track, about to be
// compared with $s5 (11, or the stunt course's id, rush2_track49_stunt_options) to grey the value. Not on a course
// where DEATHS stays open.
extern "C" void rush2_track49_deaths_value_t4(uint8_t* rdram, recomp_context* ctx) {
    if (option_open((int32_t)ctx->r12, option_deaths)) ctx->r12 = 0;
}

extern "C" void rush2_track49_deaths_value_t0(uint8_t* rdram, recomp_context* ctx) {
    if (option_open((int32_t)ctx->r8, option_deaths)) ctx->r8 = 0;
}

// func_803C6268 at 0x803C6354: $s5 = 11, which the option list compares with the track to grey options.
extern "C" void rush2_track49_stunt_options(uint8_t* rdram, recomp_context* ctx) {
    int t = (int8_t)MEM_B(0, (int32_t)track_id);
    if (is_stunt_course(t)) ctx->r21 = (uint64_t)(int64_t)t;
}

// func_803C5798 at 0x803C5930 / 0x803C5978: $t9 / $t8 = the track, about to be compared with 11 (an option's value
// is moved off screen on it).
extern "C" void rush2_track49_stunt_option_t9(uint8_t* rdram, recomp_context* ctx) {
    if (is_stunt_course((int32_t)ctx->r25)) ctx->r25 = stunt_host_slot;
}

extern "C" void rush2_track49_stunt_option_t8(uint8_t* rdram, recomp_context* ctx) {
    if (is_stunt_course((int32_t)ctx->r24)) ctx->r24 = stunt_host_slot;
}

// The Start Game menu's GHOST RACE and STUNT rows and, while the unlock system is on, its UNLOCKS row (func_803B12C8,
// labels drawn by func_803C364C): ONE RACE, CIRCUIT, PRACTICE, GHOST RACE, STUNT, RECORDS, UNLOCKS, SETUP. Hooks after
// the cursor's wraps and the label loop's count make the menu 7 or 8 rows long; these hooks give the added rows their
// labels, map the other rows back to the stock options, and make the menu's box and bottom bar longer.
namespace {
    constexpr int stock_rows = 5;
    constexpr int stock_records = 3;

    int mode_menu_rows() {
        return stock_rows + 2 + (rush2::unlocks::menu_row_shown() ? 1 : 0);
    }

    // The UNLOCKS row, or -1 while it is hidden.
    int unlocks_row() {
        return rush2::unlocks::menu_row_shown() ? stunt_row + 2 : -1;
    }

    // The stock option a row stands for (GHOST RACE, STUNT and UNLOCKS: the one whose path they take).
    int stock_row(int row) {
        int unlocks = unlocks_row();
        if (row == ghost_row) return 0;
        if (row == stunt_row) return 0;
        if (row == unlocks) return stock_records;                   // RECORDS: its profile list, then the shop
        if (row > stunt_row) return row - 2 - (unlocks >= 0 && row > unlocks ? 1 : 0);
        return row;
    }
}

// func_803B12C8 at 0x803B14B8: A or START was pressed, $t2 = the cursor, about to pick the stock option's code.
// STUNT takes ONE RACE's (game mode 0; a race on a stunt track becomes stunt mode, see rush2_track49_stunt_mode).
// UNLOCKS takes RECORDS' (its profile list, which then opens the shop: src/unlocks_shop.cpp). GHOST RACE takes ONE RACE's: the race it starts records player 1 and races the ghost.
extern "C" void rush2_mode_menu_choose(uint8_t* rdram, recomp_context* ctx) {
    int row = (int32_t)ctx->r10;
    stunt_select = row == stunt_row;
    rush2::ghost::set_chosen(row == ghost_row);
    rush2::unlocks::set_shop_chosen(row == unlocks_row());
    ctx->r10 = stock_row(row);
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
        if (row == ghost_row) {
            base = (uint64_t)(int64_t)(int32_t)(ghost_label_ptr - (uint32_t)ctx->r17);
        }
        else if (row == stunt_row) {
            base = (uint64_t)(int64_t)(int32_t)(stunt_label_ptr - (uint32_t)ctx->r17);
        }
        else if (row == unlocks_row()) {
            base = (uint64_t)(int64_t)(int32_t)(unlocks_label_ptr - (uint32_t)ctx->r17);
        }
        else if (row > stunt_row) {
            base -= 4 * (row - stock_row(row));
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
