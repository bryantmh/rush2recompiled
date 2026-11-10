#ifndef __SCENERY_FILL_H__
#define __SCENERY_FILL_H__

// Fill Scenery Gaps (Settings > Graphics > Effects, docs/graphics_effects.md#fill-scenery-gaps): the tracks were built
// for a 4:3 view from the road, so a wide screen, a high camera or the wings see past where the scenery ends: sky
// under the edges of the ground and through open-backed buildings. Two parts:
// - Extended ground: a ground underlay made from the raced track's own models (src/scenery_fill_build.cpp, nothing of
//   the game, so tools/car_probes_test.cpp --fill builds it offline): a height grid over the track and out past it,
//   filled from the ground's edges, drawn a little under the real ground where the scenery ends and out to the
//   distance, with the ground textures and colors nearest each spot.
// - Two-sided walls: the track's near-vertical faces also show from behind, with their front's texture (RT64's
//   gEXSetTwoSidedWalls, around each track model's draw).
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
        size_t quads = 0, vertices = 0;
        double seconds = 0;
    };

    // Builds the underlay of a track: the placements draw the models of `containers` (the track's first, then the
    // shared ones), by name as the game looks them up.
    Mesh build(const std::vector<const rush2::catalog::Container*>& containers,
               const std::vector<rush2::catalog::Placement>& placements, BuildStats* stats = nullptr);
    // Bumped when the build changes, so cached underlays are built again.
    constexpr uint32_t build_version = 3;
    // A texture's image as RT64 decodes it once the underlay loads it.
    rush2::upscale::Image image_of(const Texture& t);

    // The game side (src/scenery_fill.cpp).
    void set_enabled(bool on);
    bool enabled();
    // At the start of each view (func_8007C624, from rush2_interp_view_begin): starts the raced track's underlay when
    // the track changes, and lets the view draw it again.
    void view_begin(uint8_t* rdram, uint32_t view);
    // From rush2_model_draw (model_draw_8007AA48 about to call a node's model: $s7 node, $fp list): around a track
    // model, the two-sided walls, and before the view's first one, the underlay.
    void model_draw(uint8_t* rdram, recomp_context* ctx);
}

#endif
