#ifndef __SCENERY_FILL_H__
#define __SCENERY_FILL_H__

// Fill Scenery Gaps (Settings > Graphics > Effects, docs/graphics_effects.md#fill-scenery-gaps): the tracks were built
// for a 4:3 view from the road, so a wide screen, a high camera or the wings see past where the scenery ends: sky
// under the edges of the ground. It extends the ground: a ground underlay made from the raced track's own models
// (src/scenery_fill_build.cpp, nothing of the game, so tools/car_probes_test.cpp --fill builds it offline): a height
// grid over the track and out past it, filled from the ground's edges, drawn a little under the real ground where the
// scenery ends and out to the distance, with the ground textures and colors nearest each spot. (Its two-sided walls
// were removed 2026-11-13: docs/graphics_effects.md#fill-scenery-gaps.)
// The game side (src/scenery_fill.cpp) builds the raced track's underlay on a worker thread (cached on disk) and draws
// it before the track in each view.

#include <cstdint>
#include <string>
#include <vector>

#include "recomp.h"
#include "texture_upscale.h"

namespace rush2::catalog {
    struct Container;
    struct Placement;
}

namespace rush2::scenery_fill {
    // A texture of the track as the underlay loads it: the texels in the track's own format (so the texture packs'
    // replacements find it by its content, rush2::derive), and the palette of a paletted one.
    struct Texture {
        std::string name;
        uint8_t fmt = 0, siz = 0;          // G_IM_FMT_*, G_IM_SIZ_*
        uint32_t width = 0, height = 0;    // Powers of two
        uint32_t tlut = 0;                 // G_TT_* (0: not paletted)
        std::vector<uint8_t> texels;       // Big-endian, as RDRAM holds them, rows of width texels
        std::vector<uint16_t> palette;     // Paletted: its colors (RGBA5551 or IA88, as tlut says)
    };

    struct Vertex {
        float pos[3];                      // World, feet
        float st[2];                       // Texels
        uint8_t rgb[3];
    };

    // A row of quads: top[i], top[i + 1], bottom[i + 1], bottom[i] for i < cells; at most 15 cells (32 vertices).
    struct Strip {
        int texture = 0;
        std::vector<Vertex> top, bottom;
    };

    // The strips near each other, drawn under one matrix (vertices within 2048 ft of center, the s16 range).
    struct Chunk {
        float center[3] = {};
        float radius = 0;
        std::vector<Strip> strips;         // Sorted by texture
    };

    struct Mesh {
        std::vector<Texture> textures;
        std::vector<Chunk> chunks;
        std::vector<uint8_t> serialize() const;
        bool deserialize(const std::vector<uint8_t>& data);
    };

    struct BuildStats {
        size_t triangles = 0;              // Upward-facing ground triangles read
        size_t covered = 0, sea = 0, fine = 0, coarse = 0;   // Grid cells: with ground, sea, drawn fine, drawn coarse
        size_t roofs = 0;                  // Cells left out as roofs with no floor under them
        size_t guided = 0;                 // Cells under the diorama
        float lie = 0;                     // The diorama's heights' factor fitted to the track's (0 without a guide)
        size_t quads = 0, vertices = 0;
        double seconds = 0;
    };

    // A track's track select diorama laid over it (src/scenery_fill_guide.cpp, docs/graphics_effects.md#diorama-guides):
    // what the land past the track's own ground is (sea, park, city, beach) and its lie (hills and valleys), as the
    // diorama shows them.
    enum class Kind : uint8_t { none, water, green, city, sand, route };
    struct Guide {
        struct Triangle {
            float v[3][3];                 // The builder's frame (the track's models as placed), feet
            Kind kind;
            uint8_t rgb[3];                // Its color in the diorama
        };
        std::vector<Triangle> triangles;   // Facing up (the red route line too, as Kind::route)
        uint8_t water[3] = { 40, 80, 150 };   // The sea's color where the track has no water of its own
        std::string key;                   // The table entry it came from
        uint64_t hash() const;             // For the underlay's cache key
    };
    // A table entry: a track version and its diorama, and the fit (tools/diorama_fit.py) that lays the diorama on it:
    // (x, z) = s * turn(deg) * (mirror ? (-dx, dz) : (dx, dz)) + (tx, tz) in tools/diorama_fit.py's frame (the dumped
    // glbs': the diorama's x and the builder's negated), heights s * dy.
    struct Fit {
        const char* key;                   // "rush2:<SLOT>" (VEGAS ... ATARI), "sfrush:<1-7>", or a custom track's folder
        const char* model;                 // The diorama: a Rush 2 model (asset 3) for rush2:, else SF Rush's (asset 10)
        float s, deg;
        bool mirror;
        float tx, tz;
        uint32_t water;                    // 0xRRGGBB, 0 for the default day blue
    };
    const Fit* find_fit(const std::string& key);
    bool sfrush_diorama(const Fit& fit);   // Whether its diorama is SF Rush's
    // The guide from the container holding fit.model (Rush 2's asset 3 as read by rush2::catalog::read_container, or
    // SF Rush's asset 10 through rush2::track1::convert_container). False when the model isn't there.
    bool make_guide(const rush2::catalog::Container& dioramas, const Fit& fit, Guide& out);

    // Builds the underlay of a track: the placements draw the models of `containers` (the track's first, then the
    // shared ones), by name as the game looks them up. `guide`: the track's diorama, or null.
    Mesh build(const std::vector<const rush2::catalog::Container*>& containers,
               const std::vector<rush2::catalog::Placement>& placements, const Guide* guide = nullptr,
               BuildStats* stats = nullptr);
    // Bumped when the build changes, so cached underlays are built again.
    constexpr uint32_t build_version = 7;
    // A texture's image as RT64 decodes it once the underlay loads it.
    rush2::upscale::Image image_of(const Texture& t);

    // The game side (src/scenery_fill.cpp).
    void set_enabled(bool on);
    bool enabled();
    // At the start of each view (func_8007C624, from rush2_interp_view_begin): starts the raced track's underlay when
    // the track changes, and lets the view draw it again.
    void view_begin(uint8_t* rdram, uint32_t view);
    // From rush2_model_draw (model_draw_8007AA48 about to call a node's model: $s7 node, $fp list): before the view's
    // first track model, the underlay.
    void model_draw(uint8_t* rdram, recomp_context* ctx);
}

#endif
