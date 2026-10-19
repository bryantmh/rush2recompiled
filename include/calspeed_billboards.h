#ifndef __CALSPEED_BILLBOARDS_H__
#define __CALSPEED_BILLBOARDS_H__

#include <cstdint>
#include <string>
#include <vector>

// Billboard trees for California Speed's tracks (src/calspeed/calspeed_billboards.cpp,
// docs/calspeed_research.md#billboard-trees), behind the Games tab's California Speed Billboard Trees option.
//
// California Speed draws most of its roadside trees as single flat cards built into the track's section models, so
// they show edge on from the side. This finds those cards (a lone upright see-through quad of foliage, not part of a
// row, a wall or a crossed tree) and redraws each one turned to face the camera about its upright axis, inside its
// own section's list (no scene nodes: the node table holds 1,024). It needs nothing of the game, so
// tools/calspeed/billboards_test.cpp runs it outside the game.
namespace rush2::calspeed {
    // RSP segment the per-view camera-facing turn is read through: src/interpolation.cpp points it at the view's
    // yaw-only matrix (view_billboard_basis_8007BF2C's) before each view's scene is drawn.
    constexpr uint32_t billboard_segment = 0x0D;

    struct BillboardCard {
        std::string model;          // the section model it was in
        std::string texture;        // the texture it shows: "T" and its texels' offset in the container, in hex
        bool chosen = false;
        std::string why;            // why it was left flat (empty when chosen)
        double base[3] = {};        // world position of its base's middle (ft)
        double width = 0, height = 0;
        float green = 0, clear = 0; // opaque texels that are green, texels that are see-through (0-1)
        // The texels the card shows (RGBA8, crop_w x crop_h), for the test tool's contact sheet.
        int crop_w = 0, crop_h = 0;
        std::vector<uint8_t> crop;
    };

    struct BillboardResult {
        int cards = 0, models = 0;
        std::string report;
        std::vector<BillboardCard> candidates;  // every lone upright see-through quad looked at (with crops if asked)
    };

    // t: track 0-15 (its tree textures are listed per track). geometry: the track's own model container (asset 62 + t), changed in place: each section model with cards gets
    // a copy of its list without them, drawing them turned to the camera at its end. placement: the converted
    // placement (read only). crops: fill BillboardCard::crop. any_texture: take every card the shape tests pass, whatever
    // it shows (for reviewing the lists with the test tool). False with result.report saying why if nothing changed.
    bool add_billboards(int t, std::vector<uint8_t>& geometry, const std::vector<uint8_t>& placement,
                        BillboardResult& result, bool crops = false, bool any_texture = false);
}

#endif
