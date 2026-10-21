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
    // Root NODE +2 bit set by the custom track converter past 8,191 polygons: each leaf entry is a plain u16 polygon
    // number (no 3-bit run), read by rush2_collision_leaf_read_wide (src/rush2049/track2049_collision.cpp).
    inline constexpr uint8_t wide_leaf_tag = 0x20;
    // Root NODE +2 bit set with the 2049 form's 0x80 by the custom track converter when its cell lists pass the 17-bit
    // offsets (128 KB): a node's mask bit 4 + q then says quadrant q's u16 child is an index k into a table of u32
    // leaf offsets at the leaf section's end, entry k at end - 4 (k + 1) (src/rush2049/track2049_collision.cpp
    // leaf_offset). Without 0x80 the same bit is California Speed's scenery table (calspeed_scenery.h).
    inline constexpr uint8_t far_leaf_tag = 0x40;
    bool wide_leaves(uint8_t* rdram);
    // The four leaf queries' u16 polygon lists (0 collision_ground_query_8006CF00, 1 collision_poly_extract_8006F704,
    // 2 car_wall_collision_query_8006FC94, 3 collision_ground_query_basis_8008BDC0), moved off their u16[36] (40)
    // stack buffers by us.toml hooks (rush2_collision_leaf_list), so a leaf can hold max_leaf_polys: its count is
    // one byte. In spare RDRAM 0x80E1EA00-0x80E1F200 (src/controls_menu.cpp's map).
    inline constexpr uint32_t leaf_lists = 0x80E1EA00;
    inline constexpr uint32_t leaf_list_bytes = 0x200;
    inline constexpr int max_leaf_polys = 255;
}
