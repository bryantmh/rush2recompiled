#pragma once
// The loaded track's collision polygons, read from Rush 2's collision file in RDRAM (collision_file_load_800814B0,
// docs/rush2049_research/collision.md): the POLY records (0x18 bytes) and their world vertices. Implemented in
// src/smooth_collision.cpp, which reads the same file for its corner normals.

#include <cstdint>

namespace rush2::collision {
    // How many POLY records the loaded file has (0 without one).
    int polygon_count(uint8_t* rdram);
    // Polygon i's flags (& 0xF the surface type, bits 4-7 the material) and its vertices in the world (at most 16);
    // returns the vertex count.
    int polygon(uint8_t* rdram, int i, uint16_t& flags, float out[16][3]);
    // The leaf section offset of quadrant q's leaf under NODE `node` (its u16 child, 0 = empty), as the game's leaf
    // fetches read it with the converted files' extensions: a Rush 2049 file's 17-bit offsets and a California Speed
    // file's far leaves (src/rush2049/track2049_collision.cpp).
    uint32_t leaf_offset(uint8_t* rdram, uint32_t node, int q);
}
