// Circuit length and courses (General tab: the Circuit Length option and the Circuit Courses section).
//
// A Rush 2 circuit is 28 races (docs/rush2049_research/menus.md, section 9). The profile's circuit block (0x60 bytes at
// player record +0x464 + humans * 0x60, pointer in 0x800D3E40) holds +1 flags (bit 0: a circuit is in progress),
// +2 the races finished and +4 the random seed the races are generated from. func_800B2A58 starts a new circuit: it
// clears the block, takes a seed from the timer and sets bit 0. The length 28 is compared in six places, each hooked
// to compare with the circuit's length L instead:
// - func_800A7DCC at 0x800A7E5C: the current race 0x800D3DF0 = min(finished, 27) -> min(finished, L - 1).
// - func_800A8D1C at 0x800A8F54: finished >= 28 completes the circuit (func_800A8B04, stats and the victory screen).
// - func_800A952C at 0x800A9670: the race just run was not the last (index < 27) -> the settings for the next race.
// - func_803B6CA8 at 0x803B6CB8: NEXT RACE is offered while finished < 28 (0x803C7894).
// - func_803B7018 at 0x803B7158: back from a circuit race with finished >= 28 -> the results (state 0xC).
// - func_803BCFA4 at 0x803BCFE0: the circuit screen's races left, 28 - finished ($s3 = 28).
// The places of each race are kept in 3 bits a race (11 bytes a car), room for 29; the standings loops already stop at
// the races finished. Rush 2's circuit codes (func_800B2BCC) still hold 28-race circuits.
//
// The courses a circuit picks from are chosen per track (Rush 2's 0-6, Rush 2049's 12-17, SF Rush's 18-24); the
// generator hook (rush2_track49_circuit, src/rush2049/track2049_menu.cpp) draws from the chosen ones that are
// available. A circuit keeps the length and courses it started with: the start (end of func_800B2A58) records them
// against its seed in the saves file's "circuits" section. A circuit with no record (started before this setting
// existed, or entered as a code) is 28 races over every course, as before.
//
// Custom tracks (src/track_mod/, track select ids 38-63) are chosen by their folder, since their ids follow the enabled
// mods: the choices keep the folders turned off (so a newly installed track is in), and a circuit records the folders it
// started with. A circuit recorded before custom tracks could be chosen has none.

// librecomp's own nlohmann::json first, so Config::load_config links (see src/rush2049/wings.cpp).
#include "../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/config.hpp"
#include "recompui/config.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "elements/ui_select.h"
#include "elements/ui_toggle.h"

#include "circuit.h"
#include "data_files.h"
#include "npc_cars.h"
#include "online.h"
#include "options_page.h"
#include "rush2_hooks.h"
#include "track1.h"
#include "track2049.h"
#include "track_mod_runtime.h"

namespace {
    using rush2::circuit::course_ids;
    using rush2::circuit::stock_length;

    constexpr uint32_t circuit_block = 0x800D3E40;  // void*: the current profile's circuit block
    constexpr int32_t block_finished = 0x2;         // s8: races finished (offsets signed: MEM_ macros)
    constexpr int32_t block_seed = 0x4;             // u32

    std::atomic<int> next_length = stock_length;
    std::array<std::atomic<bool>, course_ids> next_courses;

    // Track select ids a circuit can pick, by game.
    struct Game {
        const char* name;
        int first;
        int count;
    };
    constexpr Game games[] = {
        { "Rush 2", 0, 7 },
        { "Rush 2049", rush2::track2049::first_menu_id, rush2::track2049::track_count },
        { "SF Rush", rush2::track1::first_menu_id, rush2::track1::track_count },
    };
    // Rush 2's full track names (0x800C4C40).
    const char* const rush2_names[7] = { "Las Vegas", "Lower Manhattan", "Honolulu", "Upper Manhattan", "Alcatraz",
                                         "Los Angeles", "Seattle" };

    bool is_course(int t) {
        for (const Game& g : games) {
            if (t >= g.first && t < g.first + g.count) return true;
        }
        return false;
    }

    std::string course_name(int t) {
        if (t < 7) return rush2_names[t];
        if (t < rush2::track1::first_menu_id) return rush2::track2049::track_names[t - rush2::track2049::first_menu_id];
        return rush2::track1::track_names[t - rush2::track1::first_menu_id];
    }

    // The enabled custom tracks' folders by track select id - mod_menu_id (the ones the track select has room for).
    std::vector<std::string> mod_folders() {
        std::vector<std::string> folders;
        for (const rush2::track_mod::Entry& e : rush2::track_mod::tracks()) {
            if ((int)folders.size() == rush2::track2049::mod_capacity) break;
            folders.push_back(e.folder);
        }
        return folders;
    }

    // Player 1's car in a circuit race: its car table entry's type, MAIN, ACCENT, STRIPE and STRIPE COLOR (0x800D9C10 +
    // slot * 9, +1..+5) and its TIRE RIMS (RIM01 + rim).
    struct Car {
        int type = 0, main = 0, accent = 0, stripe = 0, stripe_color = 0, rim = 0;
    };

    // What a circuit was started with, and the car player 1 raced each race in (by race index).
    struct Setup {
        uint32_t seed = 0;
        int length = stock_length;
        uint32_t courses = 0xFFFFFFFF; // bit t: track t
        std::set<std::string> mods;    // custom tracks by folder
        std::map<int, Car> cars;
    };

    constexpr const char* saves_section = "circuits";
    constexpr size_t setups_kept = 64; // The oldest go first (a profile's circuit block only holds its latest).
    std::mutex setups_mutex;
    std::deque<Setup> setups;
    bool setups_loaded = false;

    void load_setups() {
        if (setups_loaded) return;
        setups_loaded = true;
        std::string text = rush2::data_files::read(rush2::data_files::File::Saves, saves_section);
        if (text.empty()) return;
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object() || !j["list"].is_array()) return;
        for (const auto& e : j["list"]) {
            if (!e.is_object() || !e["seed"].is_number_unsigned() || !e["races"].is_number_integer() ||
                !e["courses"].is_array()) {
                continue;
            }
            Setup s;
            s.seed = e["seed"].get<uint32_t>();
            s.length = std::clamp(e["races"].get<int>(), 1, stock_length);
            s.courses = 0;
            for (const auto& c : e["courses"]) {
                if (c.is_number_integer() && is_course(c.get<int>())) s.courses |= 1u << c.get<int>();
            }
            if (e.contains("mods") && e["mods"].is_array()) {
                for (const auto& m : e["mods"]) {
                    if (m.is_string()) s.mods.insert(m.get<std::string>());
                }
            }
            if (e.contains("cars") && e["cars"].is_array()) {
                for (const auto& c : e["cars"]) {
                    if (!c.is_array() || c.size() != 7) continue;
                    bool ints = true;
                    for (const auto& v : c) ints = ints && v.is_number_integer();
                    if (!ints) continue;
                    s.cars[c[0].get<int>()] = { c[1].get<int>(), c[2].get<int>(), c[3].get<int>(), c[4].get<int>(),
                                                c[5].get<int>(), c[6].get<int>() };
                }
            }
            setups.push_back(s);
        }
    }

    void save_setups() {
        nlohmann::json list = nlohmann::json::array();
        for (const Setup& s : setups) {
            nlohmann::json courses = nlohmann::json::array();
            for (int t = 0; t < course_ids; t++) {
                if (s.courses & (1u << t)) courses.push_back(t);
            }
            // cars: [race, type, main, accent, stripe, stripe color, rim] each.
            nlohmann::json cars = nlohmann::json::array();
            for (const auto& [race, c] : s.cars) {
                cars.push_back({ race, c.type, c.main, c.accent, c.stripe, c.stripe_color, c.rim });
            }
            list.push_back({ { "seed", s.seed }, { "races", s.length }, { "courses", courses },
                             { "mods", nlohmann::json(std::vector<std::string>(s.mods.begin(), s.mods.end())) },
                             { "cars", cars } });
        }
        rush2::data_files::write(rush2::data_files::File::Saves, saves_section,
            nlohmann::json{ { "version", 1 }, { "list", list } }.dump());
    }

    // The current circuit's setup (Rush 2's when it has no record).
    Setup current_setup(uint8_t* rdram) {
        uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block);
        Setup found;
        if (block == 0) return found;
        uint32_t seed = (uint32_t)MEM_W(block_seed, (int32_t)block);
        std::lock_guard lock{ setups_mutex };
        load_setups();
        for (const Setup& s : setups) {
            if (s.seed == seed) found = s;
        }
        return found;
    }

    int finished(uint8_t* rdram) {
        uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block);
        return block == 0 ? 0 : (int8_t)MEM_B(block_finished, (int32_t)block);
    }

    // The choices for the next circuit (circuit.json, hidden options: races, track_<id>).
    const std::string config_id = "circuit";
    recomp::config::Config circuit_config{ "Circuit", config_id, false };

    std::string option_id(int t) {
        return "track_" + std::to_string(t);
    }

    const std::string length_id = "races";

    // The custom tracks left out of the next circuit, by folder ('/' between them: folder names can't hold one).
    const std::string mods_off_id = "mods_off";
    std::mutex mods_mutex;
    std::set<std::string> mods_off;

    void set_course(int t, bool on) {
        next_courses[t].store(on);
        circuit_config.update_option_value(option_id(t), on);
    }

    bool mod_on(const std::string& folder) {
        std::lock_guard lock{ mods_mutex };
        return !mods_off.contains(folder);
    }

    void set_mod(const std::string& folder, bool on) {
        std::lock_guard lock{ mods_mutex };
        if (on) mods_off.erase(folder);
        else mods_off.insert(folder);
        std::string text;
        for (const std::string& f : mods_off) text += (text.empty() ? "" : "/") + f;
        circuit_config.update_option_value(mods_off_id, text);
    }
}

int rush2::circuit::length(uint8_t* rdram) {
    // An online circuit another machine hosts is as long as the host's (src/online/online_game.cpp).
    int online = rush2::online::game::circuit_length();
    if (online > 0) return online;
    return current_setup(rdram).length;
}

bool rush2::circuit::course_chosen(uint8_t* rdram, int t) {
    int m = t - rush2::track2049::mod_menu_id;
    if (m >= 0 && m < rush2::track2049::mod_capacity) {
        std::vector<std::string> folders = mod_folders();
        return m < (int)folders.size() && current_setup(rdram).mods.contains(folders[m]);
    }
    return t >= 0 && t < course_ids && (current_setup(rdram).courses & (1u << t)) != 0;
}

void rush2::circuit::init_config() {
    circuit_config.add_number_option(length_id, "Circuit Length", "", 1, stock_length, 1, 0, false, stock_length, true);
    for (int t = 0; t < course_ids; t++) {
        next_courses[t].store(true);
        if (is_course(t)) {
            circuit_config.add_bool_option(option_id(t), course_name(t), "", true, true);
        }
    }
    circuit_config.add_string_option(mods_off_id, "Custom Tracks Left Out", "", "", true);
}

void rush2::circuit::load_config() {
    circuit_config.load_config();
    next_length.store(std::clamp((int)std::get<double>(circuit_config.get_option_value(length_id)), 1, stock_length));
    for (int t = 0; t < course_ids; t++) {
        if (is_course(t)) {
            next_courses[t].store(std::get<bool>(circuit_config.get_option_value(option_id(t))));
        }
    }
    std::string text = std::get<std::string>(circuit_config.get_option_value(mods_off_id));
    std::lock_guard lock{ mods_mutex };
    mods_off.clear();
    for (size_t from = 0; from < text.size();) {
        size_t to = std::min(text.find('/', from), text.size());
        if (to > from) mods_off.insert(text.substr(from, to - from));
        from = to + 1;
    }
}

// End of func_800B2A58 (a new circuit; also run before a circuit code is entered, which then replaces the seed): the
// circuit keeps the length and courses chosen now.
extern "C" void rush2_circuit_started(uint8_t* rdram, recomp_context* ctx) {
    uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block);
    if (block == 0) return;
    Setup s;
    s.seed = (uint32_t)MEM_W(block_seed, (int32_t)block);
    s.length = next_length.load();
    s.courses = 0;
    for (int t = 0; t < course_ids; t++) {
        if (is_course(t) && next_courses[t].load()) s.courses |= 1u << t;
    }
    for (const std::string& folder : mod_folders()) {
        if (mod_on(folder)) s.mods.insert(folder);
    }
    std::lock_guard lock{ setups_mutex };
    load_setups();
    std::erase_if(setups, [&](const Setup& old) { return old.seed == s.seed; });
    setups.push_back(s);
    while (setups.size() > setups_kept) setups.pop_front();
    save_setups();
    printf("[circuit] New circuit (seed %05X): %d races\n", s.seed, s.length);
    std::fflush(stdout);
}

// func_800A7DCC at 0x800A7E5C: $v0 = races finished, about to give the current race min($v0, 27).
extern "C" void rush2_circuit_race_index(uint8_t* rdram, recomp_context* ctx) {
    int last = rush2::circuit::length(rdram) - 1;
    if ((int32_t)ctx->r2 > last) ctx->r2 = (uint64_t)(int64_t)last;
}

// func_800A8D1C at 0x800A8F54 ($t7) and func_803B7018 at 0x803B7158 ($t9): races finished, about to be compared with
// 28 for a complete circuit. func_803B6CA8 at 0x803B6CB8 ($v0): the same, for NEXT RACE.
static void complete_at_length(uint8_t* rdram, uint64_t& finished_reg) {
    if ((int32_t)finished_reg >= rush2::circuit::length(rdram)) finished_reg = stock_length;
}

extern "C" void rush2_circuit_complete_t7(uint8_t* rdram, recomp_context* ctx) {
    complete_at_length(rdram, ctx->r15);
}

extern "C" void rush2_circuit_complete_t9(uint8_t* rdram, recomp_context* ctx) {
    complete_at_length(rdram, ctx->r25);
}

extern "C" void rush2_circuit_complete_v0(uint8_t* rdram, recomp_context* ctx) {
    complete_at_length(rdram, ctx->r2);
}

// func_800A952C at 0x800A9670: $t9 = the index of the race just run, about to be compared with 27 (the last race).
extern "C" void rush2_circuit_last_race(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r25 >= rush2::circuit::length(rdram) - 1) ctx->r25 = stock_length - 1;
}

// func_803BCFA4 at 0x803BCFE4: $s3 = 28, the races left being 28 - finished.
extern "C" void rush2_circuit_races_left(uint8_t* rdram, recomp_context* ctx) {
    int races = std::max(rush2::circuit::length(rdram), finished(rdram));
    ctx->r19 = (uint64_t)(int64_t)races;
}

// More Circuit Settings. A circuit race takes its DIFFICULTY and DEATHS from the circuit block, not the menu settings
// (func_80094698: 0x8010C211 = block +1 bits 3-5, 0x8010BC98 = block +1 bit 1). func_800B2A58 starts a circuit with
// difficulty 2 and the menu's DEATHS (+0x11); the track select's DEATHS step writes the block too (func_800B29DC).
// func_803ABE0C greys every option in a circuit except DEATHS on the first race and every option after it, in two
// places: the input test (0x803ABF28-0x803ABF74, mode 1 and race 0x800D3DF0 > 0 or option != DEATHS -> 0x803AC000,
// no step) and the flag the option text and arrows grey from (0x803AC554-0x803AC594 -> 0x803D0608 = 0). With the option
// on, both take the track's own tests for DIFFICULTY and DEATHS on any circuit race, a step starts from the circuit's
// values (the menu's may have changed since, in a single race) and DIFFICULTY's is written to the block.
extern "C" void pak_mark_dirty_8005F338(uint8_t* rdram, recomp_context* ctx);     // $a0 address, $a1 length, $t1 = 0

namespace {
    constexpr uint32_t game_mode = 0x8010C3E8;        // s32: 1 = circuit
    constexpr uint32_t menu_settings = 0x800D5760;    // the track select's settings
    constexpr int32_t menu_difficulty = 0xA;          // s8 0..5
    constexpr int32_t menu_deaths = 0x11;             // s8 0/1
    constexpr int32_t block_flags = 0x1;              // u8: bit 0 in progress, bit 1 deaths, bits 3-5 difficulty
    constexpr uint32_t option_cursor = 0x803D05BC;    // s32: the track select's cursor row
    constexpr uint32_t option_rows = 0x803D05D8;      // s32 [rows]: option id per row
    constexpr int option_difficulty = 7;
    constexpr int option_deaths = 10;

    std::atomic<bool> more_settings = false;

    bool more_settings_option(uint8_t* rdram) {
        if (!more_settings.load()) return false;
        int row = (int32_t)MEM_W(0, (int32_t)option_cursor);
        int option = (int32_t)MEM_W(0, (int32_t)(option_rows + row * 4));
        return option == option_difficulty || option == option_deaths;
    }

    bool in_circuit(uint8_t* rdram) {
        return (int32_t)MEM_W(0, (int32_t)game_mode) == 1 && (uint32_t)MEM_W(0, (int32_t)circuit_block) != 0;
    }
}

void rush2::circuit::set_more_settings(bool enabled) {
    more_settings.store(enabled);
}

// func_803ABE0C at 0x803ABF3C: $a0 = whether this is a circuit, about to take the circuit's greying.
extern "C" void rush2_circuit_more_input(uint8_t* rdram, recomp_context* ctx) {
    if (ctx->r4 != 0 && more_settings_option(rdram)) ctx->r4 = 0;
}

// func_803ABE0C at 0x803AC558: $v0 = whether this is not a circuit, about to take the circuit's greyed flag.
extern "C" void rush2_circuit_more_grey(uint8_t* rdram, recomp_context* ctx) {
    if (ctx->r2 == 0 && more_settings_option(rdram)) ctx->r2 = 1;
}

// func_803ABE0C at 0x803AC064, about to step the cursor's option: DIFFICULTY and DEATHS step from the circuit's.
extern "C" void rush2_circuit_more_step(uint8_t* rdram, recomp_context* ctx) {
    if (!more_settings_option(rdram) || !in_circuit(rdram)) return;
    uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block);
    uint8_t flags = MEM_BU(block_flags, (int32_t)block);
    MEM_B(menu_difficulty, (int32_t)menu_settings) = (int8_t)((flags >> 3) & 7);
    MEM_B(menu_deaths, (int32_t)menu_settings) = (int8_t)((flags >> 1) & 1);
}

// func_803ABE0C at 0x803AC410, an option stepped: a circuit keeps the DIFFICULTY chosen (DEATHS the game wrote).
extern "C" void rush2_circuit_more_stepped(uint8_t* rdram, recomp_context* ctx) {
    if (!more_settings_option(rdram) || !in_circuit(rdram)) return;
    uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block);
    uint8_t flags = MEM_BU(block_flags, (int32_t)block);
    int row = (int32_t)MEM_W(0, (int32_t)option_cursor);
    if ((int32_t)MEM_W(0, (int32_t)(option_rows + row * 4)) == option_difficulty) {
        int difficulty = (int8_t)MEM_B(menu_difficulty, (int32_t)menu_settings) & 7;
        uint8_t now = (uint8_t)((flags & ~0x38) | (difficulty << 3));
        if (now != flags) {
            flags = now;
            MEM_B(block_flags, (int32_t)block) = (int8_t)now;
            recomp_context call = *ctx;
            call.r4 = (uint64_t)(int64_t)(int32_t)(block + block_flags);
            call.r5 = 1;
            call.r9 = 0;
            pak_mark_dirty_8005F338(rdram, &call);
        }
    }
    printf("[circuit] Race %d: difficulty %d, deaths %d\n", finished(rdram) + 1, (flags >> 3) & 7, (flags >> 1) & 1);
    std::fflush(stdout);
}

// Fixed Circuit Difficulty. After every circuit race, func_800A8D1C (0x800A8FF8-0x800A9148) steps the circuit's
// difficulty (block +1 bits 3-5) by one toward the standings: $a2 = the best human's place (0 = 1st), $a0 = the leader's
// points over the second car's. It rises below 5 in 1st with a lead of 20 or more, below 4 in 1st with a lead of 6 or
// more, below 3 in 1st or 2nd, below 2 in 3rd or 4th and at 0 in 5th or 6th; otherwise it falls when it is above
// that place's value. With the option on, the hook at 0x800A8FF8 goes straight to the function's end (0x800A914C).
namespace {
    std::atomic<bool> fixed_difficulty = false;
}

void rush2::circuit::set_fixed_difficulty(bool enabled) {
    fixed_difficulty.store(enabled);
}

// func_800A8D1C at 0x800A8FF8, about to step the difficulty: nonzero skips it.
extern "C" int rush2_circuit_fixed_difficulty(uint8_t* rdram, recomp_context* ctx) {
    bool fixed = fixed_difficulty.load();
    printf("[circuit] Race %d done: standing %d, difficulty %d %s\n", finished(rdram), (int)(int32_t)ctx->r6 + 1,
        (MEM_BU(block_flags, (int32_t)ctx->r8) >> 3) & 7, fixed ? "kept" : "follows the standings");
    std::fflush(stdout);
    return fixed ? 1 : 0;
}

// Accurate Circuit Review. When a circuit completes, the game goes on to its victory screen, which is the title's
// attract loop (func_800ABE7C): a card with a track's logo, then a demo race on it in a wreath with a medal, track
// after track (func_800AB824: 0-6, then 10 with ATARI unlocked). The screen's place (1st to 3rd in the circuit, or
// not) picks the medal, the balloons, the music and the demo's car: racing, or wrecked when not in the top 3
// (func_800AB91C sets car 0's flags +0x7F4 |= 0x70 every frame when the place it gets in $s2 is 3, 0x800ABA94-
// 0x800ABAB4). With the option on, the loop shows the circuit's own races, in the order they were run, and each demo's
// car follows player 1's place in that race; the medal, balloons and music keep the circuit's place. That car (car 0,
// the one the camera follows) is also the car player 1 raced there: each circuit race's car is kept with the circuit's
// record (saves "circuits"), and the demo's car draw (func_800A37F4 at 0x800A3E80) gives it to car 0. Each demo runs
// forward, unmirrored, as every attract race does (func_80094698 forces it): tried, a backward demo shows only sky in
// the wreath, and mirror and fog apply to the whole screen, flipping the wreath and coin out of sight or fogging them.
//
// The demo is the attract race (func_800AB91C), which runs the menu track 0x8010C3F0. An added track runs in its host
// slot as a race would (rush2::track2049::host_menu_track). The logo card is a widget the setup makes from Las Vegas's
// logo (texture "VEGAS" of asset 4, loaded for track 0); func_800A4B04 then loads each next track's logo (asset 4 +
// track) in its place. The review keeps asset 4 loaded instead (func_800A4B04 does nothing when the asset asked for is
// loaded) and writes each track's logo over its texels and palette, which works for the added tracks too (their logos
// aren't assets).
namespace {
    constexpr uint32_t menu_track = 0x8010C3F0;    // s8: the track select's (and attract demo's) track
    constexpr uint32_t circuit_races = 0x800D3A60; // 4 bytes a race: track, direction bits, fog, wind
    constexpr int32_t block_places = 0x8;          // 3 bits a race (low bit first), 11 bytes a car, player 1 first
    constexpr uint32_t car_table = 0x800D9C10;     // 9 bytes a car slot: +1 type, +2..+5 paint, +8 driver (8 = player 1)
    constexpr uint32_t rim_table = 0x802019B0;     // u8 [row][36 types]: TIRE RIMS, row 0 drones, row player + 1
    constexpr int car_types = 36;
    constexpr uint32_t attract_mode = 0x800E7BB0;  // u8

    struct ReviewRace {
        int track, place;
        bool has_car = false;
        Car car;
    };

    std::atomic<bool> accurate_review = true;
    // Game thread.
    std::vector<ReviewRace> completed;   // the circuit that just completed, for the victory screen
    std::vector<ReviewRace> review;      // the victory screen's, while it shows them
    size_t review_index = 0, review_next = 0;
    bool reviewing = false;
    bool review_hosted = false;          // a review track may still be hosted

    constexpr int card_logo = 4;                   // Las Vegas's logo, the asset the card's widget is made from

    // Writes track t's logo over the card's (the loaded asset 4).
    void write_logo(uint8_t* rdram, recomp_context* ctx, int t) {
        rush2::track2049::write_menu_logo(rdram, ctx, card_logo, t);
    }
}

void rush2::circuit::set_accurate_review(bool enabled) {
    accurate_review.store(enabled);
}

// Start of func_800A8D1C (a circuit race's result, before the race count goes up): keeps the car player 1 raced in it
// with the circuit's record.
extern "C" void rush2_circuit_race_car(uint8_t* rdram, recomp_context* ctx) {
    uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block);
    if (block == 0) return;
    // The host's circuit followed online is the host's to record (src/online/online_game.cpp).
    if (rush2::online::game::circuit_length() > 0) return;
    int race = finished(rdram);
    if (race < 0 || race >= stock_length) return;
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (MEM_BU(8, (int32_t)(car_table + i * 9)) == 8) {
            slot = i;
            break;
        }
    }
    uint32_t entry = car_table + slot * 9;
    Car c;
    c.type = MEM_BU(1, (int32_t)entry);
    c.main = MEM_BU(2, (int32_t)entry);
    c.accent = MEM_BU(3, (int32_t)entry);
    c.stripe = MEM_BU(4, (int32_t)entry);
    c.stripe_color = MEM_BU(5, (int32_t)entry);
    if (c.type >= car_types) return;
    c.rim = MEM_BU(0, (int32_t)(rim_table + car_types + c.type));
    uint32_t seed = (uint32_t)MEM_W(block_seed, (int32_t)block);
    std::lock_guard lock{ setups_mutex };
    load_setups();
    Setup* found = nullptr;
    for (Setup& s : setups) {
        if (s.seed == seed) found = &s;
    }
    if (found == nullptr) {
        // A circuit with no record (older, or from a code) is Rush 2's: 28 races over every course.
        setups.push_back(Setup{ seed });
        while (setups.size() > setups_kept) setups.pop_front();
        found = &setups.back();
    }
    found->cars[race] = c;
    save_setups();
    printf("[circuit] Race %d: car %d, paint %d/%d, stripe %d/%d, rims %d\n", race + 1, c.type, c.main, c.accent,
           c.stripe, c.stripe_color, c.rim + 1);
    std::fflush(stdout);
}

// Start of func_800A8B04 (the circuit is complete): its races are generated up to the last (0x800D3A60) and player
// 1's place in each is packed into the block.
extern "C" void rush2_circuit_review_snapshot(uint8_t* rdram, recomp_context* ctx) {
    uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block);
    completed.clear();
    if (block == 0) return;
    int races = std::clamp(finished(rdram), 0, stock_length);
    std::map<int, Car> cars = current_setup(rdram).cars;
    for (int r = 0; r < races; r++) {
        ReviewRace race;
        if (auto c = cars.find(r); c != cars.end()) {
            race.has_car = true;
            race.car = c->second;
        }
        uint32_t entry = circuit_races + r * 4;
        race.track = (int8_t)MEM_B(0, (int32_t)entry);
        race.place = 0;
        for (int k = 0; k < 3; k++) {
            int bit = r * 3 + k;
            race.place |= ((MEM_BU(block_places + bit / 8, (int32_t)block) >> (bit % 8)) & 1) << k;
        }
        completed.push_back(race);
    }
}

// func_800ABE7C's setup at 0x800AC0D0: the menu track has just been set to 0 (Las Vegas), about to load its logo
// (asset 4), which the card's widget is made from.
extern "C" void rush2_circuit_review_title(uint8_t* rdram, recomp_context* ctx) {
    reviewing = accurate_review.load() && !completed.empty();
    if (reviewing) {
        review = std::move(completed);
        review_index = review_next = 0;
        std::string order;
        for (const ReviewRace& r : review) order += " " + std::to_string(r.track) + "/" + std::to_string(r.place + 1);
        printf("[circuit] Review (track/place):%s\n", order.c_str());
        std::fflush(stdout);
    }
    else if (review_hosted) {
        // Not left hosted for the title's own demos.
        rush2::track2049::host_menu_track(rdram, (int8_t)MEM_B(0, (int32_t)menu_track));
        review_hosted = false;
    }
    completed.clear();
}

// At 0x800AC0E4, after the setup loaded Las Vegas's logo.
extern "C" void rush2_circuit_review_title_logo(uint8_t* rdram, recomp_context* ctx) {
    if (reviewing) write_logo(rdram, ctx, review[0].track);
}

// At 0x800AC764, after the setup gave the medal its model. The card's widget is made, so the first demo's track can be
// set: nothing else in the setup reads it.
extern "C" void rush2_circuit_review_first(uint8_t* rdram, recomp_context* ctx) {
    if (!reviewing) return;
    rush2::track2049::host_menu_track(rdram, review[0].track);
    review_hosted = true;
}

// At 0x800ACE94: $v0 = func_800AB824's next track, whose logo (asset 4 + $v0) func_800A4B04 is about to load in place
// of the shown one. Track 0's is asset 4, the loaded one, so it loads nothing.
extern "C" void rush2_circuit_review_preload(uint8_t* rdram, recomp_context* ctx) {
    if (!reviewing) return;
    review_next = (review_index + 1) % review.size();
    ctx->r2 = card_logo - 4;
}

// At 0x800ACEA8, after func_800A4B04.
extern "C" void rush2_circuit_review_preload_logo(uint8_t* rdram, recomp_context* ctx) {
    if (reviewing) write_logo(rdram, ctx, review[review_next].track);
}

// At 0x800ACFDC: $v0 = func_800AB824's next track, about to become the menu track the next demo runs.
extern "C" void rush2_circuit_review_next(uint8_t* rdram, recomp_context* ctx) {
    if (!reviewing) return;
    review_index = review_next;
    const ReviewRace& race = review[review_index];
    ctx->r2 = rush2::track2049::host_menu_track(rdram, race.track);
}

// func_800A37F4 at 0x800A3E80 (after the AI Opponents' and the ghosts' hooks): car slot $s2's paint has been drawn
// ($s1 = its car table entry). A review demo's car 0 is the car player 1 raced there.
extern "C" void rush2_circuit_review_car(uint8_t* rdram, recomp_context* ctx) {
    if (!reviewing || (int32_t)ctx->r18 != 0 || MEM_BU(0, (int32_t)attract_mode) == 0) return;
    const ReviewRace& race = review[review_index];
    if (!race.has_car || !rush2::npc_cars::type_available(race.car.type)) return;
    uint32_t entry = (uint32_t)ctx->r17;
    MEM_B(1, (int32_t)entry) = (int8_t)race.car.type;
    MEM_B(2, (int32_t)entry) = (int8_t)race.car.main;
    MEM_B(3, (int32_t)entry) = (int8_t)race.car.accent;
    MEM_B(4, (int32_t)entry) = (int8_t)race.car.stripe;
    MEM_B(5, (int32_t)entry) = (int8_t)race.car.stripe_color;
    rush2::npc_cars::set_slot_rim(0, race.car.rim);
}

// At 0x800ACF94, about to run the demo (func_800AB91C(1), which reads the screen's place in $s2): the place in the
// race shown, top 3 or not.
extern "C" void rush2_circuit_review_demo(uint8_t* rdram, recomp_context* ctx) {
    if (reviewing) ctx->r18 = review[review_index].place < 3 ? 0 : 3;
}

namespace {
    using namespace recompui;

    bool expanded = false; // UI thread: whether the section is open.

    const char* const length_description =
        "Sets how many races a circuit has. Rush 2's circuits have 28. "
        "Applies to the next circuit you start; a circuit in progress keeps its own length.";
    const char* const courses_description =
        "Picks the courses circuits are made from. The toggles at the top turn all of a game's courses, or all the "
        "modded courses, on or off. "
        "A course that isn't unlocked yet is skipped, and with none on, every course is used. "
        "Applies to the next circuit you start; a circuit in progress keeps its own courses.";

    bool game_shown(const Game& g) {
        if (g.first == rush2::track2049::first_menu_id) return rush2::track2049::available();
        if (g.first == rush2::track1::first_menu_id) return rush2::track1::available();
        return true;
    }

    bool all_on(const Game& g) {
        for (int t = g.first; t < g.first + g.count; t++) {
            if (!next_courses[t].load()) return false;
        }
        return true;
    }

    // A custom track: its folder (what the choices keep) and its name.
    struct ModCourse {
        std::string folder, name;
    };

    std::vector<ModCourse> mod_courses() {
        std::vector<ModCourse> out;
        std::vector<rush2::track_mod::Entry> entries = rush2::track_mod::tracks();
        for (size_t i = 0; i < entries.size() && i < size_t(rush2::track2049::mod_capacity); i++) {
            if (rush2::track_mod::is_arena(entries[i].settings)) continue;   // stunt and battle arenas are never in a circuit
            out.push_back({ entries[i].folder, entries[i].settings.name });
        }
        return out;
    }

    bool all_mods_on(const std::vector<ModCourse>& mods) {
        for (const ModCourse& m : mods) {
            if (!mod_on(m.folder)) return false;
        }
        return true;
    }

    // A line of the section: a name on the left, a toggle on the right.
    Toggle* add_toggle_line(Element* parent, const std::string& name, bool bold, bool checked) {
        ContextId context = get_current_context();
        Element* line = context.create_element<rush2::ui::FocusRow>(parent);
        line->set_display(Display::Flex);
        line->set_flex_direction(FlexDirection::Row);
        line->set_align_items(AlignItems::Center);
        line->set_gap(16.0f);
        line->set_as_navigation_container(NavigationType::Horizontal);
        Label* label = context.create_element<Label>(line, name, bold ? theme::Typography::LabelSM : theme::Typography::Body);
        if (!bold) label->set_color(theme::color::TextDim);
        label->set_flex_grow(1.0f);
        Toggle* toggle = context.create_element<Toggle>(line, ToggleSize::Medium);
        toggle->set_flex_shrink(0.0f);
        toggle->set_checked(checked);
        return toggle;
    }

    // The section: a heading with a button that opens and closes the course list below it. At the top, a toggle per
    // game turns all its courses on or off, and one all the modded courses; below, a toggle per course, the modded
    // courses last.
    class Section : public Element {
    public:
        Section(ResourceId rid, Element* parent, const rush2::circuit::TextShower& text_shower)
            : Element(rid, parent, Events(EventType::Update), "div", false) {
            ContextId context = get_current_context();
            set_display(Display::Flex);
            set_flex_direction(FlexDirection::Column);
            set_padding(12.0f);
            set_gap(8.0f);
            set_width(100.0f, Unit::Percent);
            set_as_navigation_container(NavigationType::Vertical);

            // Circuit Length: a select of 4 to 28 races.
            Element* length_row = context.create_element<rush2::ui::FocusRow>(this, text_shower(length_description));
            length_row->set_display(Display::Flex);
            length_row->set_flex_direction(FlexDirection::Row);
            length_row->set_align_items(AlignItems::Center);
            length_row->set_gap(16.0f);
            length_row->set_as_navigation_container(NavigationType::Horizontal);
            Label* length_label = context.create_element<Label>(length_row, "Circuit Length", theme::Typography::LabelMD);
            length_label->set_flex_grow(1.0f);
            length_label->set_flex_shrink(0.0f);
            std::vector<SelectOption> lengths;
            for (int races = 4; races <= stock_length; races += 4) {
                lengths.emplace_back(std::to_string(races) + (races == stock_length ? " Races (Rush 2)" : " Races"),
                                     std::to_string(races));
            }
            int chosen = next_length.load();
            if (chosen % 4 != 0) {
                lengths.emplace_back(std::to_string(chosen) + " Races", std::to_string(chosen));
            }
            Select* select = context.create_element<Select>(length_row, lengths, std::to_string(chosen));
            // Select is focusable but not in the tab order, which the d-pad's navigation (RmlUi's nav: auto) needs.
            select->set_tab_index_auto();
            select->add_change_callback([](SelectOption& option, int) {
                int races = std::clamp(std::stoi(option.value), 1, stock_length);
                next_length.store(races);
                circuit_config.update_option_value(length_id, (double)races);
                circuit_config.save_config();
            });

            Element* head = context.create_element<rush2::ui::FocusRow>(this, text_shower(courses_description));
            head->set_display(Display::Flex);
            head->set_flex_direction(FlexDirection::Row);
            head->set_align_items(AlignItems::Center);
            head->set_gap(16.0f);
            head->set_as_navigation_container(NavigationType::Horizontal);
            Label* title = context.create_element<Label>(head, "Circuit Courses", theme::Typography::LabelMD);
            title->set_flex_grow(1.0f);
            button = context.create_element<Button>(head, expanded ? "Hide" : "Show", ButtonStyle::Secondary);
            button->set_flex_shrink(0.0f);
            button->add_pressed_callback([this]() {
                expanded = !expanded;
                queue_update();
            });

            body = context.create_element<Element>(this, 0, "div", false);
            body->set_display(Display::Flex);
            body->set_flex_direction(FlexDirection::Column);
            body->set_gap(6.0f);
            body->set_as_navigation_container(NavigationType::Vertical);
            build();
        }

    protected:
        std::string_view get_type_name() override { return "CircuitCoursesSection"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Update && built != expanded) {
                button->set_text(expanded ? "Hide" : "Show");
                build();
            }
        }

    private:
        Button* button = nullptr;
        Element* body = nullptr;
        bool built = false;
        std::vector<std::pair<int, Toggle*>> game_toggles;  // (index in games, toggle)
        std::vector<std::pair<int, Toggle*>> course_toggles; // (track, toggle)
        std::vector<ModCourse> mods;                         // the custom tracks when built
        Toggle* all_mods_toggle = nullptr;
        std::vector<std::pair<std::string, Toggle*>> mod_toggles; // (folder, toggle)

        void refresh() {
            for (auto [g, toggle] : game_toggles) toggle->set_checked(all_on(games[g]));
            for (auto [t, toggle] : course_toggles) toggle->set_checked(next_courses[t].load());
            if (all_mods_toggle != nullptr) all_mods_toggle->set_checked(all_mods_on(mods));
            for (const auto& [folder, toggle] : mod_toggles) toggle->set_checked(mod_on(folder));
        }

        void build() {
            body->clear_children();
            game_toggles.clear();
            course_toggles.clear();
            mod_toggles.clear();
            all_mods_toggle = nullptr;
            built = expanded;
            if (!expanded) return;
            ContextId context = get_current_context();
            mods = mod_courses();

            Element* all = context.create_element<Element>(body, 0, "div", false);
            all->set_display(Display::Flex);
            all->set_flex_direction(FlexDirection::Column);
            all->set_gap(6.0f);
            all->set_padding_bottom(8.0f);
            all->set_as_navigation_container(NavigationType::Vertical);
            for (int g = 0; g < (int)std::size(games); g++) {
                if (!game_shown(games[g])) continue;
                Toggle* toggle = add_toggle_line(all, std::string("All ") + games[g].name + " Courses", true,
                                                 all_on(games[g]));
                toggle->add_checked_callback([this, g](bool on) {
                    for (int t = games[g].first; t < games[g].first + games[g].count; t++) set_course(t, on);
                    circuit_config.save_config();
                    refresh();
                });
                game_toggles.emplace_back(g, toggle);
            }
            if (!mods.empty()) {
                all_mods_toggle = add_toggle_line(all, "All Modded Courses", true, all_mods_on(mods));
                all_mods_toggle->add_checked_callback([this](bool on) {
                    for (const ModCourse& m : mods) set_mod(m.folder, on);
                    circuit_config.save_config();
                    refresh();
                });
            }

            for (int g = 0; g < (int)std::size(games); g++) {
                if (!game_shown(games[g])) continue;
                Element* block = context.create_element<Element>(body, 0, "div", false);
                block->set_display(Display::Flex);
                block->set_flex_direction(FlexDirection::Column);
                block->set_gap(6.0f);
                block->set_padding_top(8.0f);
                block->set_as_navigation_container(NavigationType::Vertical);
                context.create_element<Label>(block, games[g].name, theme::Typography::LabelSM);
                for (int t = games[g].first; t < games[g].first + games[g].count; t++) {
                    Toggle* toggle = add_toggle_line(block, course_name(t), false, next_courses[t].load());
                    toggle->add_checked_callback([this, t](bool on) {
                        set_course(t, on);
                        circuit_config.save_config();
                        refresh();
                    });
                    course_toggles.emplace_back(t, toggle);
                }
            }

            if (!mods.empty()) {
                Element* block = context.create_element<Element>(body, 0, "div", false);
                block->set_display(Display::Flex);
                block->set_flex_direction(FlexDirection::Column);
                block->set_gap(6.0f);
                block->set_padding_top(8.0f);
                block->set_as_navigation_container(NavigationType::Vertical);
                context.create_element<Label>(block, "Modded Courses", theme::Typography::LabelSM);
                for (const ModCourse& m : mods) {
                    Toggle* toggle = add_toggle_line(block, m.name, false, mod_on(m.folder));
                    std::string folder = m.folder;
                    toggle->add_checked_callback([this, folder](bool on) {
                        set_mod(folder, on);
                        circuit_config.save_config();
                        refresh();
                    });
                    mod_toggles.emplace_back(m.folder, toggle);
                }
            }
        }
    };
}

void rush2::circuit::add_section(recompui::Element* parent, const TextShower& text_shower) {
    recompui::get_current_context().create_element<Section>(parent, text_shower);
}

