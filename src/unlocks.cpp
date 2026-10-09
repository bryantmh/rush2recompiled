// The unlock system: what keys, Dew cans and coins unlock.
//
// Every key and silver coin found is worth 1 point, every Dew can and gold coin 2 (src/collectibles.cpp counts them
// per profile, over Rush 2, SF Rush and Rush 2049). Each profile spends its points in the UNLOCKS menu
// (src/unlocks_shop.cpp) on the cars, tracks and parts below; what it bought is kept with its finds. Everything to
// find is worth 533 points and everything to buy costs about half of that.
//
// With the unlock system on (the Progress tab's option, on by default):
// - Car select lists cars 0-15, the Rush 2049 cars 1-6 and whatever the player's profile bought, on every track. Rush
//   2's rules (mystery cars for keys found on the current track, the Rocket for winning a circuit) don't apply.
// - Track select offers PIPE, MIDWAY and the locked SF Rush and Rush 2049 tracks once any player in the game bought
//   them.
// - A Rush 2049 car's ENGINE row offers the three engines Rush 2049 starts with (3.2L HP V6, TURBO 350, 6.2L V8)
//   and the ones the player's profile bought, which are bought in Rush 2049's unlock order (5.0L HP V6 first,
//   8.0L V10 last; each needs the one before it).
// - Every car's TIRES row offers Rush 2049's SLICKS and PRO SLICKS once the player's profile bought them (2049
//   unlocks its tires by miles driven, its table 0x80150F00; its other three match tires Rush 2 already has).
// With it off, each game's own rules apply where the port can follow them:
// - Rush 2's mystery cars, PIPE and MIDWAY as in Rush 2.
// - On an SF Rush track, half its keys unlock the Taxi and all of them the Hot Rod there, as in SF Rush; all of them
//   also count as Rush 2's 12 keys and 4 cans, so every mystery car is offered there. All 16 coins of a Rush 2049
//   track or stunt arena do the same.
// - The Rush 2049 cars from coin totals, as Rush 2049 sets them (func_800F2A28): cars 1-6 always, LOCUST LX for all
//   48 race silver coins, GX-2 / MINI XS for 24 / 36 race gold coins, VENOM for all 32 stunt silver coins, CRUSHER /
//   EURO LX for 16 / 24 stunt gold coins and PANTHER for every coin.
// - Rush 2049 unlocks its tracks and parts by circuit places, stunt points, battle kills and miles, which the port
//   doesn't keep, so the added tracks, every ENGINE level and every tire are open.
// The Cheats tab's Unlock All Cars, Unlock All Tracks and Unlock All Parts open everything either way.

#include <algorithm>
#include <cstdio>

#include "recomp.h"

#include "rush2.h"
#include "rush2_hooks.h"
#include "car2049.h"
#include "collectibles.h"
#include "track1.h"
#include "track2049.h"
#include "unlocks.h"

namespace {
    using rush2::unlocks::Item;
    using rush2::unlocks::Kind;
    namespace collect = rush2::collectibles;

    constexpr int pipe_track = 9;
    constexpr int midway_track = 10;
    constexpr int sfrush_track7 = rush2::track1::first_menu_id + 6;
    constexpr int car2049_type = rush2::car2049::first_type;
    constexpr int free_engines = 3;     // Rush 2049's: levels 0-2 always open (its table 0x80150ED8).

    const std::vector<Item> catalog = {
        { "car_taxi",      "TAXI",         "RUSH 2",    Kind::Car,   16, 5 },
        { "car_hotrod",    "HOT ROD",      "RUSH 2",    Kind::Car,   17, 5 },
        { "car_formula",   "FORMULA",      "RUSH 2",    Kind::Car,   18, 10 },
        { "car_prototype", "PROTOTYPE",    "RUSH 2",    Kind::Car,   19, 15 },
        { "car_dew",       "MOUNTAIN DEW", "RUSH 2",    Kind::Car,   21, 10 },
        { "car_rocket",    "ROCKET",       "RUSH 2",    Kind::Car,   20, 15 },
        { "car_locust",    "LOCUST LX",    "RUSH 2049", Kind::Car,   car2049_type + 6, 10 },
        { "car_gx2",       "GX-2",         "RUSH 2049", Kind::Car,   car2049_type + 7, 10 },
        { "car_minixs",    "MINI XS",      "RUSH 2049", Kind::Car,   car2049_type + 8, 15 },
        { "car_venom",     "VENOM",        "RUSH 2049", Kind::Car,   car2049_type + 9, 10 },
        { "car_crusher",   "CRUSHER",      "RUSH 2049", Kind::Car,   car2049_type + 10, 10 },
        { "car_eurolx",    "EURO LX",      "RUSH 2049", Kind::Car,   car2049_type + 11, 10 },
        { "car_panther",   "PANTHER",      "RUSH 2049", Kind::Car,   car2049_type + 12, 25 },
        { "track_midway",  "MIDWAY",       "RUSH 2",    Kind::Track, midway_track, 10 },
        { "track_pipe",    "PIPE",         "RUSH 2",    Kind::Track, pipe_track, 20 },
        { "track_sf7",     "TRACK 7",      "SF RUSH",   Kind::Track, sfrush_track7, 10 },
        { "track_49_4",    "TRACK 4",      "RUSH 2049", Kind::Track, rush2::track2049::first_menu_id + 3, 5 },
        { "track_49_5",    "TRACK 5",      "RUSH 2049", Kind::Track, rush2::track2049::first_menu_id + 4, 5 },
        { "track_49_6",    "TRACK 6",      "RUSH 2049", Kind::Track, rush2::track2049::first_menu_id + 5, 10 },
        { "stunt_2",       "STUNT 2",      "RUSH 2049", Kind::Track, rush2::track2049::stunt_menu_id + 1, 5 },
        { "stunt_3",       "STUNT 3",      "RUSH 2049", Kind::Track, rush2::track2049::stunt_menu_id + 2, 5 },
        { "stunt_4",       "STUNT 4",      "RUSH 2049", Kind::Track, rush2::track2049::stunt_menu_id + 3, 10 },
        { "obstacle",      "OBSTACLE",     "RUSH 2049", Kind::Track, rush2::track2049::obstacle_menu_id, 15 },
        // Rush 2049 unlocks four battle arenas by battle stats (docs/unlocks_plan.md); DM1-DM4 are open from the start.
        { "battle_5",      "BATTLE 5",     "RUSH 2049", Kind::Track, rush2::track2049::battle_menu_id + 4, 5 },
        { "battle_6",      "BATTLE 6",     "RUSH 2049", Kind::Track, rush2::track2049::battle_menu_id + 5, 5 },
        { "battle_7",      "BATTLE 7",     "RUSH 2049", Kind::Track, rush2::track2049::battle_menu_id + 6, 10 },
        { "battle_8",      "BATTLE 8",     "RUSH 2049", Kind::Track, rush2::track2049::battle_menu_id + 7, 15 },
        { "engine_4",      "5.0L HP V6",   "RUSH 2049", Kind::Part,  3, 2 },
        { "engine_5",      "TURBO 400",    "RUSH 2049", Kind::Part,  4, 3 },
        { "engine_6",      "7.0L V8",      "RUSH 2049", Kind::Part,  5, 5 },
        { "engine_7",      "6.5L HP V8",   "RUSH 2049", Kind::Part,  6, 6 },
        { "engine_8",      "TURBO 500",    "RUSH 2049", Kind::Part,  7, 8 },
        { "engine_9",      "8.0L V10",     "RUSH 2049", Kind::Part,  8, 10 },
        { "tires_slicks",  "SLICKS",       "RUSH 2049", Kind::Part,  rush2::unlocks::tire_part + 1, 4 },
        { "tires_pro",     "PRO SLICKS",   "RUSH 2049", Kind::Part,  rush2::unlocks::tire_part + 2, 8 },
    };

    constexpr uint32_t player_count = 0x8010C3E2;   // s16
    constexpr uint32_t car_list = rush2::car2049::car_list;    // u8: car i of player p at + 2 * i + p.
    constexpr uint32_t car_list_size = 0x803CB398;  // s16 per player
    constexpr int rush2_base_cars = 16;

    const Item* find(Kind kind, int value) {
        for (const Item& item : catalog) {
            if (item.kind == kind && item.value == value) {
                return &item;
            }
        }
        return nullptr;
    }

    bool owns(const std::string& name, const Item& item) {
        return collect::purchases(name).contains(item.id);
    }

    int player_total(uint8_t* rdram) {
        return std::clamp<int>((int16_t)MEM_H(0, (int32_t)player_count), 1, 4);
    }

    int bits(uint32_t v) {
        int n = 0;
        for (; v != 0; v &= v - 1) n++;
        return n;
    }

    // Rush 2049's coin rules for its cars 7-13 (func_800F2A28), from a player's coins.
    bool earned_2049_car(const collect::Progress& p, int k) {
        int race_silver = 0, race_gold = 0, stunt_silver = 0, stunt_gold = 0;
        for (uint16_t m : p.rush2049) {
            race_silver += bits(m & collect::silver_bits);
            race_gold += bits(m & collect::gold_bits);
        }
        for (uint16_t m : p.stunt2049) {
            stunt_silver += bits(m & collect::silver_bits);
            stunt_gold += bits(m & collect::gold_bits);
        }
        constexpr int race_all = collect::rush2049_courses * collect::coins_per_kind;
        constexpr int stunt_all = collect::stunt2049_courses * collect::coins_per_kind;
        switch (k) {
            case 6: return race_silver >= race_all;
            case 7: return race_gold >= 24;
            case 8: return race_gold >= 36;
            case 9: return stunt_silver >= stunt_all;
            case 10: return stunt_gold >= 16;
            case 11: return stunt_gold >= 24;
            case 12: return race_silver >= race_all && race_gold >= race_all && stunt_silver >= stunt_all &&
                            stunt_gold >= stunt_all;
            default: return k < 6;
        }
    }

    void set_list(uint8_t* rdram, int player, const std::vector<int>& types) {
        int n = std::min<int>((int)types.size(), rush2::car2049::types);
        for (int i = 0; i < n; i++) {
            MEM_B(0, (int32_t)(car_list + i * 2 + player)) = (int8_t)types[i];
        }
        MEM_H(0, (int32_t)(car_list_size + player * 2)) = (int16_t)n;
    }

    std::vector<int> get_list(uint8_t* rdram, int player) {
        std::vector<int> types;
        int n = (int16_t)MEM_H(0, (int32_t)(car_list_size + player * 2));
        for (int i = 0; i < n && i < rush2::car2049::types; i++) {
            types.push_back((int8_t)MEM_B(0, (int32_t)(car_list + i * 2 + player)));
        }
        return types;
    }
}

const std::vector<Item>& rush2::unlocks::items() {
    return catalog;
}

bool rush2::unlocks::item_available(const Item& item) {
    if (item.kind == Kind::Car) {
        return item.value < rush2::car2049::rush2_types || rush2::car2049::available();
    }
    if (item.kind == Kind::Part) {
        return rush2::car2049::available();
    }
    if (item.value >= rush2::track1::first_menu_id && item.value < rush2::track2049::stunt_menu_id) {
        return rush2::track1::available();
    }
    return item.value < rush2::track2049::first_menu_id || rush2::track2049::available();
}

const Item* rush2::unlocks::prerequisite(const Item& item) {
    return item.kind == Kind::Part && item.value < tire_part ? find(Kind::Part, item.value - 1) : nullptr;
}

int rush2::unlocks::spent(const std::set<std::string>& purchases) {
    int total = 0;
    for (const Item& item : catalog) {
        if (purchases.contains(item.id)) {
            total += item.cost;
        }
    }
    return total;
}

bool rush2::unlocks::enabled() {
    return rush2::cheats::unlock_system();
}

bool rush2::unlocks::menu_row_shown() {
    return enabled();
}

void rush2::unlocks::filter_car_list(uint8_t* rdram, int player) {
    // The shop's carousel: its cars, in its order (src/unlocks_shop.cpp moves through them by index).
    if (shop_car() >= 0) {
        std::vector<int> types;
        for (const Item& item : catalog) {
            if (item.kind == Kind::Car && item_available(item)) {
                types.push_back(item.value);
            }
        }
        set_list(rdram, player, types);
        return;
    }
    if (!rush2::cheats::unlock_all_parts()) {
        rush2::car2049::limit_engines(rdram, player);
        rush2::car2049::limit_tires(rdram, player);
    }
    if (rush2::cheats::unlock_all_cars()) {
        return;
    }
    if (enabled()) {
        std::set<std::string> bought = collect::purchases(collect::player_name(rdram, player));
        std::vector<int> types;
        for (int t = 0; t < rush2::car2049::types; t++) {
            if (t == rush2::car2049::rush2_types || (t >= car2049_type && !rush2::car2049::available())) {
                continue;
            }
            const Item* item = find(Kind::Car, t);
            if (item == nullptr || bought.contains(item->id)) {
                types.push_back(t);
            }
        }
        set_list(rdram, player, types);
        return;
    }
    // The game listed Rush 2's cars by its rules (and the added tracks' finds, added_track_keys); the 2049 cars
    // follow Rush 2049's.
    collect::Progress p = collect::player_progress(rdram, player);
    std::vector<int> types;
    for (int t : get_list(rdram, player)) {
        if (t < car2049_type || earned_2049_car(p, t - car2049_type)) {
            types.push_back(t);
        }
    }
    set_list(rdram, player, types);
}

bool rush2::unlocks::track_open(uint8_t* rdram, int track, bool game_unlocked) {
    if (rush2::cheats::unlock_all_tracks()) {
        return true;
    }
    const Item* item = find(Kind::Track, track);
    if (!enabled()) {
        return item == nullptr || track > midway_track || game_unlocked;
    }
    if (item == nullptr) {
        return game_unlocked;
    }
    for (int p = 0; p < player_total(rdram); p++) {
        if (owns(collect::player_name(rdram, p), *item)) {
            return true;
        }
    }
    return false;
}

bool rush2::unlocks::engine_open(uint8_t* rdram, int player, int level) {
    if (level < free_engines || !enabled() || rush2::cheats::unlock_all_parts()) {
        return true;
    }
    const Item* item = find(Kind::Part, level);
    return item == nullptr || owns(collect::player_name(rdram, player), *item);
}

bool rush2::unlocks::tires_open(uint8_t* rdram, int player, int tire) {
    if (!enabled() || rush2::cheats::unlock_all_parts()) {
        return true;
    }
    const Item* item = find(Kind::Part, tire_part + tire);
    return item == nullptr || owns(collect::player_name(rdram, player), *item);
}

bool rush2::unlocks::added_track_keys(uint8_t* rdram, int player, int track, bool dew, int& count) {
    count = 0;
    if (track < rush2::track2049::first_menu_id) {
        return false;
    }
    if (enabled()) {
        return true;
    }
    collect::Progress p = collect::player_progress(rdram, player);
    bool all = false, half = false;
    if (track >= rush2::track1::first_menu_id && track < rush2::track2049::stunt_menu_id) {
        int c = track - rush2::track1::first_menu_id;
        int total = collect::sfrush_keys[c];
        int found = bits(p.sfrush[c] & ((1u << total) - 1));
        all = found >= total;
        half = 2 * found >= total;
    }
    else if (track < rush2::track1::first_menu_id) {
        all = p.rush2049[track - rush2::track2049::first_menu_id] == 0xFFFF;
    }
    else if (track < rush2::track2049::obstacle_menu_id) {
        all = p.stunt2049[track - rush2::track2049::stunt_menu_id] == 0xFFFF;
    }
    // Rush 2's car select gives a mystery car per 3 keys and the Dew car for 4 cans.
    if (dew) {
        count = all ? collect::rush2_cans : 0;
    }
    else {
        count = all ? collect::rush2_keys : half ? 3 : 0;
    }
    return true;
}
