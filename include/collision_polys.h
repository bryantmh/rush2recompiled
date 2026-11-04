#pragma once
// The loaded track's collision polygons, read from Rush 2's collision file in RDRAM (collision_file_load_800814B0,
// docs/rush2049_research/collision.md): the POLY records (0x18 bytes) and their world vertices. Implemented in
// src/smooth_collision.cpp, which reads the same file for its corner normals.

#include <array>
#include <cstdint>
#include <vector>

namespace rush2::collision {
    // How far past a cell a wall polygon is listed for it, in ft. Rush 2's body probe (car_wall_collision_query_8006FC94)
    // tests each body point only against its own cell's leaf, and takes a wall from 5 ft behind its plane (0x144($sp))
    // to 3.75 ft in front; Rush 2's own files list walls well past their cells. A wall listed only for the cells it
    // crosses stops nothing that has crossed it (docs/rush2049_research/collision.md#walls-behind-their-cells).
    // The custom track converter (src/track_mod/track_mod_convert.cpp) and the Rush 2049 one
    // (src/rush2049/track2049_convert.cpp) both list walls this far out.
    inline constexpr float wall_cell_reach = 6.0f;

    // Whether a convex x/z outline (x, z pairs) meets the box [x0, x1] x [z0, z1]: separating axes of the box and of
    // the outline's edges, so a long diagonal polygon isn't listed for every cell of its bounding box.
    inline bool outline_meets_box(const std::vector<std::array<float, 2>>& o, float x0, float x1, float z0, float z1) {
        if (o.empty()) return false;
        float ox0 = o[0][0], ox1 = o[0][0], oz0 = o[0][1], oz1 = o[0][1];
        for (const auto& w : o) {
            ox0 = w[0] < ox0 ? w[0] : ox0; ox1 = w[0] > ox1 ? w[0] : ox1;
            oz0 = w[1] < oz0 ? w[1] : oz0; oz1 = w[1] > oz1 ? w[1] : oz1;
        }
        if (ox1 < x0 || ox0 > x1 || oz1 < z0 || oz0 > z1) return false;
        for (size_t k = 0; k < o.size(); k++) {
            const auto &a = o[k], &b = o[(k + 1) % o.size()];
            float nx = b[1] - a[1], nz = a[0] - b[0];
            if (nx * nx + nz * nz < 1e-8f) continue;
            float pmin = 1e30f, pmax = -1e30f;
            for (const auto& w : o) {
                float d = nx * w[0] + nz * w[1];
                pmin = d < pmin ? d : pmin; pmax = d > pmax ? d : pmax;
            }
            float rmin = 1e30f, rmax = -1e30f;
            for (float cx : { x0, x1 }) for (float cz : { z0, z1 }) {
                float d = nx * cx + nz * cz;
                rmin = d < rmin ? d : rmin; rmax = d > rmax ? d : rmax;
            }
            if (rmax < pmin - 1e-3f || rmin > pmax + 1e-3f) return false;
        }
        return true;
    }

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
