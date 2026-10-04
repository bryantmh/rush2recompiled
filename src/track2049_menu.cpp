// Track select entries for the Rush 2049 and SF Rush tracks (docs/rush2049_research/menus.md).
//
// Rush 2's track select (menu overlay: func_803AB294 init and draw, func_803ABE0C per frame) cycles the track id byte
// through 0-11 and shows a carousel of dioramas built from 12-entry tables. With Rush 2049 tracks available it
// offers ids 12-17 after them, and with SF Rush (Rush 1) tracks available ids 18-24:
// - The per-track tables the screen reads (diorama names 0x803C91E0, scales 0x803C9180, cloud heights 0x803C91B0,
//   logo names 0x803C9668) and its carousel array (0x803D0698, 0x1C bytes per entry) only have room for 12. Copies
//   with 25 entries live in the memory the game heap used before it moved (src/assets.cpp), and us.toml repoints the
//   instructions that address them. The loop bounds and wraps go from 12 to 25; func_803AB01C, which says whether a
//   track is unlocked, offers 12-17 and 18-24 only while those tracks are available.
// - The dioramas and logos come from a generated copy of asset 3 (src/track2049_art.cpp, then
//   rush2::track1::extend_menu_container for the Rush 1 ones).
// - The screen saves the chosen track as the low nibble of byte +0x30 of the player's save record, which can't hold
//   12-24, so such a choice leaves the nibble alone and is kept in track2049.json instead, and restored when the
//   screen opens.
// - Car select counts the track's collected keys from 12-entry tables (func_803B1AB0); added tracks have none.
//
// When a race starts on id 12-24, the id becomes the host slot's and the added track is noted for the track hooks
// (src/track2049.cpp, src/track1.cpp). The host slot keeps its id through restarts; opening the track select clears
// it again.

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
#include "track1.h"
#include "track2049.h"
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

    // Their 18-entry copies (us.toml points the screen at these).
    constexpr uint32_t menu_data = 0x80300000;
    constexpr uint32_t new_diorama_names = menu_data + 0x000;
    constexpr uint32_t new_diorama_scales = menu_data + 0x080;
    constexpr uint32_t new_cloud_heights = menu_data + 0x100;
    constexpr uint32_t new_logo_names = menu_data + 0x180;
    constexpr uint32_t new_strings = menu_data + 0x200;
    // new carousel array at menu_data + 0x400 (25 x 0x1C)
    constexpr uint32_t new_circuit_instances = menu_data + 0x700; // 25 x s32, the circuit screen's dioramas

    // Circuit mode: the race list func_800A7DCC generates, 4 bytes per race (track, direction bits, fog, wind).
    constexpr uint32_t circuit_races = 0x800D3A60;
    constexpr uint32_t circuit_last = 0x800D3DF0;    // s8: index of the last race
    constexpr int circuit_stock_tracks = 7;          // Rush 2 draws circuits from tracks 0-6
    constexpr int circuit_max_uses = 4;

    constexpr int rush2_tracks = 12;
    constexpr int r1_first = rush2::track1::first_menu_id;               // 18
    constexpr int menu_tracks = r1_first + rush2::track1::track_count;  // 25
    constexpr int added_tracks = menu_tracks - first_menu_id;           // 2049 and Rush 1 entries
    static_assert(first_menu_id + track_count == r1_first);

    // Whether added track select entry t (12-24) can be chosen.
    bool entry_available(int t) {
        if (t >= first_menu_id && t < r1_first) return available();
        if (t >= r1_first && t < menu_tracks) return rush2::track1::available();
        return false;
    }

    std::mutex menu_mutex;
    int selection = -1;            // Added track (id - 12) last chosen on the track select, or -1.
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
        size_t at = text.find("\"selected\"");
        if (at != std::string::npos && (at = text.find(':', at)) != std::string::npos) {
            selection = std::atoi(text.c_str() + at + 1);
            if (selection < -1 || selection >= added_tracks) {
                selection = -1;
            }
        }
    }

    void save_selection(int value) {
        if (value == selection) {
            return;
        }
        selection = value;
        std::ofstream f(selection_path());
        f << "{ \"selected\": " << selection << " }\n";
    }

    void write_string(uint8_t* rdram, uint32_t addr, const std::string& s) {
        for (size_t i = 0; i <= s.size(); i++) {
            MEM_B(0, (int32_t)(addr + i)) = i < s.size() ? s[i] : 0;
        }
    }

    // Fills the 25-entry tables: Rush 2's 12 entries, then the 2049 tracks', then the Rush 1 tracks'.
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
        for (int k = 1; k <= track_count; k++) {
            int t = rush2_tracks + k - 1;
            std::string model = "R49TRACK" + std::to_string(k), logo = "R49LOGO" + std::to_string(k);
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

    // Turns a 2049 menu id into the host slot for the race.
    void enter_race(uint8_t* rdram) {
        int t = (int8_t)MEM_B(0, (int32_t)track_id);
        if (t >= first_menu_id && t < r1_first) {
            set_race_track(t - first_menu_id + 1);
            rush2::track1::set_race_track(0);
            MEM_B(0, (int32_t)track_id) = host_slot;
        }
        else if (t >= r1_first && t < menu_tracks) {
            set_race_track(0);
            rush2::track1::set_race_track(t - r1_first + 1);
            MEM_B(0, (int32_t)track_id) = host_slot;
        }
        else if (t != host_slot) {
            set_race_track(0);
            rush2::track1::set_race_track(0);
        }
    }
}

// func_803AB294 at 0x803AB5C0, once per track select visit: the initial track is chosen and the screen's assets are
// about to load. $s0 = &track id.
extern "C" void rush2_track49_select_init(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ menu_mutex };
    load_selection();
    set_race_track(0);
    rush2::track1::set_race_track(0);
    restore_host(rdram);
    rush2::track1::restore_host(rdram);
    write_tables(rdram);
    serve_menu_container(rdram);
    if (selection >= 0 && entry_available(first_menu_id + selection) && MEM_W(0, (int32_t)game_mode) != 1) {
        MEM_B(0, (int32_t)track_id) = uint8_t(first_menu_id + selection);
        MEM_H(0, (int32_t)shown_track) = int16_t(first_menu_id + selection);
    }
}

// func_803AB294 at 0x803AB6C8: $t7 = the number of carousel entries (10 + unlocked PIPE and ATARI).
extern "C" void rush2_track49_select_count(uint8_t* rdram, recomp_context* ctx) {
    if (available()) {
        ctx->r15 += track_count;
    }
    if (rush2::track1::available()) {
        ctx->r15 += rush2::track1::track_count;
    }
}

// Start of func_803AB01C: whether track $a0 can be chosen. Returns true with $v0 set for the added ids.
extern "C" int rush2_track49_select_available(uint8_t* rdram, recomp_context* ctx) {
    int t = (int32_t)ctx->r4;
    if (t < rush2_tracks) {
        return 0;
    }
    ctx->r2 = entry_available(t) ? 1 : 0;
    return 1;
}

// func_803ABE0C at 0x803AC104: the TRACK option wrapped below 0 and stored 11 at $a3; wrap to the last added track
// instead (the availability check that follows steps back past entries that are off).
extern "C" void rush2_track49_select_wrap(uint8_t* rdram, recomp_context* ctx) {
    MEM_B(0, (int32_t)ctx->r7) = uint8_t(menu_tracks - 1);
}

// func_803ABE0C at 0x803AC670 (player 1) / 0x803AC6B8 (player 2): about to store the save record byte with the
// chosen track ($a2 / $t9) in its low nibble ($t9 / $t6, from the old byte $t7). 2049 ids keep the old byte.
extern "C" void rush2_track49_select_save_p1(uint8_t* rdram, recomp_context* ctx) {
    int t = (int32_t)ctx->r6;
    std::lock_guard lock{ menu_mutex };
    if (t >= first_menu_id) {
        ctx->r25 = ctx->r15;
        save_selection(t - first_menu_id);
    }
    else {
        save_selection(-1);
    }
}

extern "C" void rush2_track49_select_save_p2(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r25 >= first_menu_id) {
        ctx->r14 = ctx->r15;
    }
}

// Start of func_803B1AB0 (car select): stores the number of keys collected on the current track at $a1. Returns
// true for the 2049 tracks, which have none.
extern "C" int rush2_track49_keys(uint8_t* rdram, recomp_context* ctx) {
    if ((int8_t)MEM_B(0, (int32_t)track_id) < rush2_tracks) {
        return 0;
    }
    MEM_B(0, (int32_t)ctx->r5) = 0;
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
}

// Start of func_803B6260, the circuit screen (every frame). It shows the dioramas and logos of the circuit's races
// from the 18-entry tables (us.toml), so they and the 2049 art must be in place.
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
        if (entry_available(t)) pool.push_back(t);
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
