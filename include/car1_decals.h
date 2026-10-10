#ifndef __CAR1_DECALS_H__
#define __CAR1_DECALS_H__

#include <cstdint>
#include <vector>

// Rush 1's unique car decals (the Camaro's flames, the Taxi's checker band, the VW Bus's swirls, the VW Bug's sunburst) as a
// stripe pattern for the same cars in Rush 2 (src/rush1/car1_decals.cpp, port of tools/rush1/cardecal.py; method and
// evidence in docs/rush1_research.md section 11; the game side is src/rush1/car1_stripes.cpp).
//
// Rush 2's cars are the Rush 1 cars re-textured, so the Rush 1 art is projected onto Rush 2's panel textures through
// the car's 3D shape. The decal keeps Rush 1's colours: the result is one colour map per panel texture (D0_1 ..
// D0_6; Rush 2 car palette indices, 0 = no decal) plus its quarter-size mip.
namespace rush2::car1decals {
    constexpr int car_count = 5;
    constexpr int max_ranges = 2;

    struct Range {
        int first = 0, last = 0;    // inclusive; last = 0 marks an unused entry
    };

    // A hand-made cut: a texel rectangle (inclusive) of panel D0_<panel> where the decal is dropped.
    struct Cut {
        int panel, x0, y0, x1, y1;
    };

    struct Car {
        const char* name;               // tools/rush1/cartex.py name
        int rush2_type;                 // Rush 2 car type (its car asset is 0x1D + type)
        int rush1_asset;                // Rush 1 asset table index of the car file
        Range primary[max_ranges];      // Rush 1 palette indices of the decal
        Range companion[max_ranges];    // indices kept only next to a decal texel (the Taxi's black checks)
        int panels;                     // bit n: the decal goes on panel D0_n
        std::vector<Cut> cuts;
    };
    extern const Car cars[car_count];

    // A panel's decal: the Rush 2 car palette index to paint, 0 = none. `lod` is the _4 texture (a quarter of the size
    // in each direction).
    struct Panel {
        int w = 0, h = 0;
        std::vector<uint8_t> full, lod;
    };

    struct Pattern {
        Panel panel[7];         // by panel number 1-6 (the digit after "D0_" in Rush 2's texture names)
    };

    // r1_car / r2_car: the car's asset in each game (Rush 1 asset 25+, Rush 2 asset 0x1D + type), stripe_asset: Rush 2
    // asset 0x1C (its CARPALETTE). `car` indexes `cars`.
    // On failure, `why` (if given) says which step failed.
    bool build(int car, const std::vector<uint8_t>& r1_car, const std::vector<uint8_t>& r2_car,
               const std::vector<uint8_t>& stripe_asset, Pattern& out, const char** why = nullptr);
}

#endif
