#ifndef __UNLOCKS_H__
#define __UNLOCKS_H__

#include <cstdint>
#include <set>
#include <string>
#include <vector>

// The unlock system (src/unlocks.cpp, src/unlocks_shop.cpp). Keys, Dew cans and coins found in every game earn points
// (src/collectibles.cpp) that each profile spends in the UNLOCKS menu on cars, tracks and parts. It replaces what Rush
// 2's keys unlock; turned off (Progress tab), each game's own rules apply.
namespace rush2::unlocks {
    enum class Kind { Car, Track, Part };

    struct Item {
        const char* id;     // Kept in the save file's "collectibles" section.
        const char* name;   // As the shop shows it.
        const char* game;
        Kind kind;
        int value;          // Car type, track select id, or ENGINE level (0-8).
        int cost;
    };

    // Every item, in the shop's order.
    const std::vector<Item>& items();
    // Whether an item's game is available (its ROM is present and its option is on).
    bool item_available(const Item& item);
    // The points a profile's purchases cost.
    int spent(const std::set<std::string>& purchases);
    // The item that must be bought first (each ENGINE level needs the one before it), or nullptr.
    const Item* prerequisite(const Item& item);

    // The Progress tab's Unlock System option (kept with the Cheats tab's options).
    bool enabled();

    // Game thread.
    // The Start Game menu shows UNLOCKS while the unlock system is on; choosing it (or another row) is noted, so the
    // records screen it leads to becomes the shop.
    bool menu_row_shown();
    void set_shop_chosen(bool chosen);
    // The car type the shop shows in the car select, or -1 when the shop isn't open.
    int shop_car();
    // Player p's car list, once car select built it: the unlock system's cars, or each game's own rules.
    void filter_car_list(uint8_t* rdram, int player);
    // Whether track select may offer track t: PIPE (9) and MIDWAY (10) are passed what the game decided.
    bool track_open(uint8_t* rdram, int track, bool game_unlocked = true);
    // Whether player p may pick a 2049 car's ENGINE level (0-8).
    bool engine_open(uint8_t* rdram, int player, int level);
    // Car select's key count on an added track (func_803B1AB0): with the unlock system off, SF Rush keys and 2049
    // coins count as Rush 2's keys and cans there. Returns false for Rush 2's own tracks.
    bool added_track_keys(uint8_t* rdram, int player, int track, bool dew, int& count);
}

#endif
