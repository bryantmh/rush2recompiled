#ifndef __RUSH2_CAR_STRIPES_H__
#define __RUSH2_CAR_STRIPES_H__

// Rush 2's STRIPE values on cars that have no Rush 2 panels to stamp them on: the Rush 2049 N64 cars and the car mods
// painted through a paint mask (the PS1 SF Rush cars). docs/car_mods.md#stripes-on-one-body-cars.
//
// Rush 2's stripes are bands running the car's length, measured out from its centerline. Every stripe tile (asset
// 0x1C, cars/stripes: one per STRIPE value and panel shape) reaches across half the panel, from the centerline to the
// side, so a tile's bands are fractions of the car's half width. Here the body's triangles are cut at each band's
// edges (x = +-edge x the half width), and the pieces inside a band are drawn again over the body through a palette
// that turns the paint into the stripe (rush2::car2049, add_damage_textures): the edges are the geometry's, sharp at
// any resolution, and a texture used at several places on the car is striped only where a band crosses it.

#include <cstdint>
#include <vector>

namespace rush2::car_stripes {
    constexpr int values = 8;                // 0 NONE, 1 SINGLE ... 7 TRIPLE B (the car select's STRIPE row)

    // A band of a STRIPE value: |x| / half width from lo to hi, at a tile alpha level (1-15; 15 = the stripe's color,
    // less = SNGL FADE's steps, the paint blended towards it).
    struct Band {
        float lo, hi;
        int level;
    };
    std::vector<Band> bands(int stripe);

    // The tile alpha levels the stripes use, each drawn through its own palette: 15 (solid) and SNGL FADE's steps.
    constexpr int level_count = 7;
    constexpr int level_values[level_count] = { 15, 12, 11, 9, 7, 5, 2 };
    int level_slot(int level);               // index into level_values, -1 for none

    // The y of a triangle's unit normal turned away from `center` (car space; the car's origin unless given),
    // whichever way it is wound.
    float outward_normal_y(const float a[3], const float b[3], const float c[3], const float center[3] = nullptr);
}

#endif
