#ifndef __CALSPEED_SCENERY_H__
#define __CALSPEED_SCENERY_H__

#include <cstdint>
#include <string>
#include <vector>

// Extended scenery for California Speed's tracks (src/calspeed/calspeed_scenery.cpp,
// docs/calspeed_research.md#extended-scenery), behind the Games tab's California Speed Extended Scenery option.
//
// California Speed's tracks are built only as far as its camera looked: a point-to-point track's road stops a few
// feet behind the start line (California Speed doesn't let a car turn round), and its seas are patches that end in
// the void. This adds to a track's converted files, before they are merged and loaded:
// - behind the start of a point-to-point track, the track's own cross-section at the road's end swept back along a
//   road that bends and drops out of sight, with the track's own textures, and a row of road-closed barricades across
//   it (with collision up to them);
// - on the tracks by the sea, the sea out to the horizon at the level of the track's water.
// It needs nothing of the game, so tools/calspeed/scenery_test.cpp runs it outside the game.
namespace rush2::calspeed {
    struct SceneryResult {
        int models = 0, records = 0, polygons = 0;
        std::string report;     // one line per step, for the log and the test tool
    };
    // t: track 0-15. geometry: the track's own model container (asset 62 + t), changed in place: new models are
    // appended to it (they draw with its texture-load lists). placement: the converted placement file, records
    // appended. collision: the converted collision file (surfaces remapped), new polygons and quadtree leaves added.
    // path: the converted path file (read only). False with result.report saying why if nothing could be added;
    // the files are then unchanged.
    bool add_scenery(int t, std::vector<uint8_t>& geometry, std::vector<uint8_t>& placement,
                     std::vector<uint8_t>& collision, const std::vector<uint8_t>& path, SceneryResult& result);

    // Root node byte +2 of a collision file add_scenery extended (bit 6; Rush 2049's converted files use bit 7):
    // a node's mask bit 4 + q then says quadrant q's u16 child is an index into the far-leaf table, whose u32 entries
    // (leaf offsets from the leaf section, past its 16-bit reach) follow the vertex lists, 4-byte aligned.
    constexpr uint8_t scenery_collision_tag = 0x40;
}

#endif
