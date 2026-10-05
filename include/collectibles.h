#ifndef __COLLECTIBLES_H__
#define __COLLECTIBLES_H__

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// SF Rush's keys and Rush 2049's coins on the added tracks (src/collectibles.cpp, docs/unlocks_plan.md). They are
// picked up like Rush 2's keys and kept per profile name in collectibles.json; nothing unlocks with them yet. Rush 2's
// own keys and Dew cans stay in its save and are only read, for the Progress tab.
namespace rush2::collectibles {
    constexpr int rush2_courses = 12;       // Rush 2's tracks 0-11 (Las Vegas .. Stunt 1).
    constexpr int rush2_keys = 12;          // Per Rush 2 track: keys are bits 0-11, Dew cans bits 12-15.
    constexpr int rush2_cans = 4;
    constexpr uint16_t rush2_key_bits = 0x0FFF;
    constexpr uint16_t rush2_can_bits = 0xF000;
    constexpr int sfrush_courses = 7;       // SF Rush tracks 1-7 (track select ids 18-24).
    constexpr int rush2049_courses = 6;     // Rush 2049 race tracks 1-6 (ids 12-17).
    constexpr int stunt2049_courses = 4;    // Rush 2049 stunt arenas 1-4 (ids 25-28).

    // Keys on each SF Rush track (Rush 1's table 0x800CAD48). Key n is bit n - 1.
    constexpr std::array<int, sfrush_courses> sfrush_keys = { 6, 7, 8, 8, 8, 8, 8 };
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
    };

    // Every profile in the Controller Pak image (in pak order), then other profiles with something collected (by
    // name), then the no-profile players' progress.
    std::vector<Progress> progress();
    // Forgets what profile p (pak * 5 + record) collected: its records were cleared or it was deleted.
    void clear_profile(uint8_t* rdram, int p);
    // The Progress tab (src/progress_tab.cpp).
    void create_tab();
}

#endif
