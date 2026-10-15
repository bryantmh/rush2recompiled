#ifndef __PAINT_COLORS_H__
#define __PAINT_COLORS_H__

#include <cstdint>

// Car paint colors (src/paint_colors.cpp, docs/rush2049_research/cars.md "Paint colors"). Rush 2 has 32 (0-31); while
// the Rush 2049 cars are offered, the 2049 shades that no Rush 2 color is close to follow as 32-52, for every car.
namespace rush2::paint_colors {
    constexpr int rush2_colors = 32;
    constexpr int extra_colors = 21;
    constexpr int max_colors = rush2_colors + extra_colors;

    // Allocates the game's color tables (RGBA and RGBA5551, max_colors entries). Call once at boot, before the race
    // scene loads.
    void init(uint8_t* rdram);
    // Fills the 2049 shades from Rush 2049's RGB table (0x801226C0: 32 RGBA words, big-endian), or nullptr when the
    // 2049 Main segment isn't there (the extras then show their stand-in Rush 2 colors and aren't offered).
    void load_2049(uint8_t* rdram, const uint8_t* table);
    // The colors offered now: max_colors while the 2049 cars are offered and their shades are loaded, else 32.
    int count();
    // The color a stored index paints with now: itself while offered, an extra's stand-in Rush 2 color otherwise.
    int usable(int color);
    // RGBA (0xRRGGBBAA) of a color, any index below max_colors (the extras' own shade even while not offered).
    uint32_t rgba(int color);
    // Forgets the 2049 shades kept for the player record at `record` (a deleted player).
    void forget_record(uint8_t* rdram, uint32_t record);
}

#endif
