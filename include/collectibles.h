#ifndef __COLLECTIBLES_H__
#define __COLLECTIBLES_H__

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// SF Rush's keys and Rush 2049's coins on the added tracks (src/collectibles.cpp, docs/unlocks_plan.md). They are
// picked up like Rush 2's keys and kept per profile name in the save file, with what each profile bought with
// them (src/unlocks.cpp). Rush 2's own keys and Dew cans stay in its save and are only read.
namespace rush2::collectibles {
    constexpr int rush2_courses = 12;       // Rush 2's tracks 0-11 (Las Vegas .. Stunt 1).
    constexpr int rush2_keys = 12;          // Per Rush 2 track: keys are bits 0-11, Dew cans bits 12-15.
    constexpr int rush2_cans = 4;
    constexpr uint16_t rush2_key_bits = 0x0FFF;
    constexpr uint16_t rush2_can_bits = 0xF000;
    constexpr int sfrush_courses = 7;       // SF Rush tracks 1-7 (track select ids 18-24).
    constexpr int rush2049_courses = 6;     // Rush 2049 race tracks 1-6 (ids 12-17).
    constexpr int stunt2049_courses = 4;    // Rush 2049 stunt arenas 1-4 (ids 25-28).
    constexpr int sfrush_ps1_courses = 1;   // The PS1 SF Rush's own course, Chinatown (the sfrush_ps1 mod).
    // The courses' numbering here and in the save file: SF Rush tracks 0-6, Rush 2049 race tracks 7-12, its stunt
    // arenas 13-16, Chinatown 17.
    constexpr int sfrush_ps1_course = sfrush_courses + rush2049_courses + stunt2049_courses;

    // Keys on each SF Rush track (Rush 1's table 0x800CAD48). Key n is bit n - 1.
    constexpr std::array<int, sfrush_courses> sfrush_keys = { 6, 7, 8, 8, 8, 8, 8 };
    // The PS1 SF Rush's keys (docs/sfrush_ps1.md#keys): 4 per PS1 track, records KEYP1-4, bits 8-11 of their course
    // (SF Rush tracks 0-2, Chinatown). Those at an N64 key's spot are that key instead.
    constexpr int ps1_keys = 4;
    constexpr int ps1_key_bit = 8;
    // Whether a course has keys (an SF Rush one or Chinatown), not coins.
    constexpr bool key_course(int c) { return (c >= 0 && c < sfrush_courses) || c == sfrush_ps1_course; }
    // Coins on each 2049 track and stunt arena, per kind: silver coins are bits 0-7, gold coins bits 8-15.
    constexpr int coins_per_kind = 8;
    constexpr uint16_t silver_bits = 0x00FF;
    constexpr uint16_t gold_bits = 0xFF00;

    struct Progress {
        std::string name;   // The profile's name; empty for players without a profile (this session only).
        std::array<uint16_t, rush2_courses> rush2{};
        std::array<uint16_t, sfrush_courses> sfrush{};
        std::array<uint16_t, rush2049_courses> rush2049{};
        std::array<uint16_t, stunt2049_courses> stunt2049{};
        std::array<uint16_t, sfrush_ps1_courses> sfrush_ps1{};
    };
    // A course's mask (the numbering above) in a profile's progress.
    uint16_t course_mask(const Progress& p, int course);

    // Every profile in the Controller Pak image (in pak order), then other profiles with something collected (by
    // name), then the no-profile players' progress.
    std::vector<Progress> progress();
    // Forgets what profile p (pak * 5 + record) collected and bought: its records were cleared or it was deleted.
    void clear_profile(uint8_t* rdram, int p);
    // Game thread: player p's profile name ("" without one) and what it has collected.
    std::string player_name(uint8_t* rdram, int player);
    Progress player_progress(uint8_t* rdram, int player);
    // Points a profile ("" = players without one) has collected: 1 per key and silver coin, 2 per Dew can and gold
    // coin, over every game. Rush 2's finds are read once per frame. max_points() is everything there is to find.
    int points(const std::string& name);
    int max_points();
    // The unlock system's items a profile bought (by id), and buying one: false if it is already bought or the
    // profile's points less `spent` (what its purchases cost) don't cover `cost`.
    std::set<std::string> purchases(const std::string& name);
    bool purchase(const std::string& name, const std::string& id, int cost, int spent);
    // Forgets what a profile ("" = players without one) bought; its finds stay.
    void reset_purchases(const std::string& name);
    // The Progress tab (src/progress_tab.cpp).
    void create_tab();

    // Arcade versions of the courses and the sfrush_ps1 mod's tracks (src/arcade2049/arcade_collectibles.cpp): the
    // course (the numbering above) whose coins or keys a mod's track has some of, else -1; the raced custom track's.
    int arcade_course(const std::string& mod_id, const std::string& folder);
    int raced_arcade_course();
    // The keys of a key course (key_course()): their bits on every version (the N64 course, its arcade, PS1 and other
    // versions); with installed, only on the versions the player has (the N64 course and the enabled mods' tracks).
    uint16_t key_bits(int course, bool installed);
    // Game thread: the bits of the coins or keys placed in the race being raced (all of a stock course's; some, or
    // none, of an arcade version's), and the course whose arcade version is raced, or -1.
    uint16_t race_pickups();
    int race_arcade_course();
}

#endif
