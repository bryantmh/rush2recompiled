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
}
