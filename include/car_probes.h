#ifndef __CAR_PROBES_H__
#define __CAR_PROBES_H__

// Car lighting from light probes (Graphics > Car Lighting = Light Probes, docs/graphics_effects.md#car-light-probes).
//
// The bake (src/car_probes_bake.cpp, nothing of the game, so tools/car_probes_test.cpp runs it on its own) traces the
// raced track's models as placed: probes sit a car's height above every upward-facing surface, and each keeps six
// colors (an ambient cube), the light reaching it from each side: the baked vertex colors times the texture colors of
// what its rays hit, white sky for what they miss. The colors are scaled so an open road is 1 on every side.
// The game side (src/car_probes.cpp) bakes the raced track once (cached on disk), samples the probes at each race car
// and draws the car's parts from copies of their display lists whose vertex colors are relit with the cube, per
// vertex by its normal (rush2::car2049::light_model).

#include <cstdint>
#include <memory>
#include <vector>

namespace rush2::catalog {
    struct Container;
    struct Placement;
}

namespace rush2::probes {
    // Light arriving from +x, -x, +y (above), -y, +z, -z; 1 = an open road's.
    struct Cube {
        float c[6][3];
        // The light on a surface facing the unit direction n (an ambient cube: the faces weighted by n squared).
        void eval(const float n[3], float out[3]) const;
        static Cube neutral();
    };

    // A Rush 2049 track's painted lighting colour (rush2::track2049::car_light): `amount` of the cube takes the
    // colour, keeping the cube's shape but no side brighter than the colour.
    struct Painted {
        float rgb[3];
        float amount;
    };

    // Where a Rush 2049 track's artists painted lighting colours (rush2::track2049::car_light_paint): world triangles,
    // 9 floats each, and each one's colour (0xRRGGBBAA).
    struct PaintSource {
        std::vector<float> tris;
        std::vector<uint32_t> colours;
    };

    // One probe per cell of a grid (BakeParams::spacing), sorted by cell.
    struct ProbeSet {
        float spacing = 10.0f;
        std::vector<uint64_t> keys;        // The cells, sorted (21-bit x, y, z).
        std::vector<uint8_t> offsets;      // 3 per probe: where it is in its cell, 255 = the far side
        std::vector<uint8_t> faces;        // 18 per probe: Cube::c as light * 128
        std::vector<uint8_t> paint;        // 4 per probe: the painted colour under it (r, g, b, 255 = painted, 0 =
                                           // not), or empty: a track without painted colours
        size_t count() const { return keys.size(); }
        void position(size_t i, float out[3]) const;
        // The probes within `radius` of a world position (height counting 3x), blended with weights falling smoothly
        // to 0 at the radius, so the light doesn't step from probe to probe. With none that near (a car in the air),
        // the probes up to 80 ft under it, height counting a quarter. False if none.
        // paint_out: also the painted colour, mostly over `paint_radius` (amount: the painted share); the probes out to
        // `radius` count with a small weight, so the colour never drops out between probes. Left as it is when no probe
        // is near (in the air) or the set has no paint.
        bool sample(const float pos[3], Cube& out, float radius, Painted* paint_out = nullptr, float paint_radius = 0) const;
        std::vector<uint8_t> serialize() const;
        bool deserialize(const std::vector<uint8_t>& data);
    };

    struct BakeStats {
        size_t triangles = 0, probes = 0, open = 0;
        float reference[6] = {};           // The open road's luminance per face the colors were divided by.
        double seconds = 0;
    };

    // How a track is baked (tools/car_probes_test.cpp compares them).
    struct BakeParams {
        float spacing = 5.0f;              // Feet between probes
        int rays = 256;                    // Rays per probe
        bool grid = false;                 // Probes at the cell centers over each upward triangle, rather than at random
                                           // points on it (small triangles then get one only by chance)
        bool rotate = true;                // Each probe's rays turned at random, so neighbours don't share blind spots
        float smooth = 12.0f;              // Feet: each probe averaged with its neighbours this near (height 3x), 0 none
    };

    // Bakes the probes of a track: the placements draw the models of `containers` (the track's first, then the shared
    // ones), by name as the game looks them up.
    std::shared_ptr<ProbeSet> bake(const std::vector<const rush2::catalog::Container*>& containers,
                                   const std::vector<rush2::catalog::Placement>& placements, BakeStats* stats = nullptr,
                                   const BakeParams& params = {}, const PaintSource* paint = nullptr);
    // Bumped when the bake changes, so cached probes are baked again.
    constexpr uint32_t bake_version = 7;

    // A race car's light as it drives (src/car_probes_bake.cpp; the game's update and tools/car_probes_test.cpp's path
    // replay share it): the probes over a radius, eased toward over a short time so the light doesn't flicker from
    // probe to probe, and sampled that same time ahead along the car's motion so a change lands where it happens
    // instead of trailing the car. A Rush 2049 track's painted colour comes from the probes too (blended over a
    // shorter radius and eased quicker), applied after the easing.
    struct CarLight {
        struct Params {
            float radius = 20.0f;          // ProbeSet::sample radius, feet
            float ease_time = 0.08f;       // Seconds for the light to move most (63%) of the way
            float lookahead = 0.06f;       // Seconds of motion ahead the probes are sampled at...
            float max_lookahead = 30.0f;   // ...at most this many feet
            float contrast = 0.6f;         // How much of the cube's difference between sides is kept (soften())
            float paint_radius = 20.0f;    // ProbeSet::sample radius of the painted colour, feet: a painted patch
                                           // fades in over this much road instead of a wheel at a time (10 left
                                           // Marina's tunnel lamp colors about as sharp; wider flattens Presidio's
                                           // lamp pools)
            float paint_ease = 0.04f;      // Seconds for the painted colour to move most of the way
        };
        using Painted = rush2::probes::Painted;
        Cube cube = Cube::neutral();       // The eased probe light
        Painted painted = { { 1, 1, 1 }, 0 }; // The eased painted colour (amount 0 off painted polygons)
        bool valid = false;
        float last[3] = {};
        // Moves the light on by `dt` seconds with the car at `pos`. The painted colour comes from the probes when the
        // set has paint and one is in reach, else from `under` (the colour under the wheels, null: none).
        void step(const ProbeSet& set, const float pos[3], const Painted* under, float dt, const Params& params);
        // The light the car is drawn with: the eased probe light, softened, then painted.
        Cube lit(const Painted* painted, const Params& params) const;
        // The probe light at a place (no easing).
        static Cube target(const ProbeSet& set, const float pos[3], float radius);
        // Moves every side `1 - contrast` of the way to the sides' mean, so the light turns gradually around the car
        // (the top and bottom no longer meet at a hard line on its beltline).
        static void soften(Cube& c, float contrast);
        static void paint(Cube& c, const Painted& painted);
    };

    // The game side (src/car_probes.cpp).
    enum class Mode : uint32_t { Original, Probes };
    void set_mode(Mode mode);
    Mode mode();
    // Once per view in a race (game thread): starts the raced track's bake when it changes and moves each race car's
    // cube toward the probes at the car. False until the track's probes are ready (draw the cars as Original then).
    bool update(uint8_t* rdram);
    // Out of a race: drops the copies (models load again for the next race).
    void reset();
    // Race car `car`'s part list `dl`, drawn by scene node `node` whose world rotation is `rot` (rows): a copy with its
    // vertex colors relit by the car's cube, or 0 when it can't be copied.
    uint32_t relight(uint8_t* rdram, int car, uint32_t dl, const float rot[9]);
}

#endif
