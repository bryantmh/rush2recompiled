// Rush 2049's collision lookups for its converted courses (docs/rush2049_research/collision.md, "Quadtree lookups").
//
// Rush 2 and 2049 share the quadtree format and walk it with near-identical code, but differ in three places that
// change which leaf a point gets, and so which walls and floors it is tested against:
// - The split of a node: Rush 2 takes (min + max) >> 1 (collision_node_find_8006CD60), 2049 rounds toward zero
//   (func_800AC9BC). 2049's data splits toward zero, so with Rush 2's rounding the one-unit strips on split lines with
//   a negative odd min + max go to the wrong child. Walls often sit on those lines; a car coming from the side that
//   doesn't list the wall drives into it.
// - The query cell: Rush 2's four leaf queries take x - 0.99 then truncate for negative x (wrong in a 0.01 band),
//   2049 floors exactly (x - 1.0 unless x is a whole number, then truncate: 0x800C6B50, 0x800C42B8).
// - 17-bit leaf offsets: a node's mask bit 4 + q adds 0x10000 to quadrant q's leaf offset. Only 2049 track 3 (file
//   141, 0x184A4 leaf bytes) needs them.
// The converter (src/rush2049/track2049_convert.cpp, convert_collision) marks its files by setting bit 7 of the root
// node's unused byte +2 and putting the leaf section size's bits 16-22 in bits 0-6; these hooks take 2049's behavior
// only for a marked file, so Rush 2's own, SF Rush's and custom tracks keep Rush 2's.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "recomp.h"
#include "rush2_hooks.h"

namespace {
    constexpr uint32_t node_table = 0x8010D390;  // Pointer to the collision file's NODE records (0x14 bytes).
    constexpr uint8_t marker = 0x80;              // Root node +2: converted from Rush 2049.
    constexpr uint32_t file_base = 0x80112448;   // Pointer to the collision file (its header).
    constexpr uint32_t poly_table = 0x8011001C;  // Pointer to its POLY records (0x18 bytes).
    constexpr uint32_t vert_table = 0x80110050;  // Pointer to its VERT records (8 bytes).
    constexpr uint32_t vlist_table = 0x80111930; // Pointer to its vertex lists.

    bool split_and_cell_off = false;              // Self-test only: Rush 2's split and cell, to count what they miss.

    uint8_t root_tag(uint8_t* rdram) {
        int32_t root = MEM_W(0, (int32_t)node_table);
        return root != 0 ? MEM_BU(2, root) : 0;
    }

    bool is_2049(uint8_t* rdram) {
        return (root_tag(rdram) & marker) != 0;
    }

    float read_f(uint8_t* rdram, int32_t addr) {
        uint32_t w = (uint32_t)MEM_W(0, addr);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    // 2049's cell coordinate: an exact floor done its way, in single precision.
    int32_t cell(float v) {
        if (v < 0.0f && (float)(int32_t)v != v) {
            v = v - 1.0f;
        }
        return (int32_t)(int16_t)(int32_t)v;
    }

    // The point's cell in $a1/$a2, as the query passes them to collision_node_find_cached_8006CE28.
    void set_cell(uint8_t* rdram, recomp_context* ctx, int32_t x_addr, int32_t z_addr) {
        if (split_and_cell_off || !is_2049(rdram)) {
            return;
        }
        ctx->r5 = cell(read_f(rdram, x_addr));
        ctx->r6 = cell(read_f(rdram, z_addr));
    }

    // A leaf offset just read from node->child[q]: + 0x10000 when the node's mask bit 4 + q is set.
    void leaf_offset(uint8_t* rdram, gpr& offset, gpr node, gpr q) {
        if (!is_2049(rdram)) {
            return;
        }
        if (MEM_BU(3, (int32_t)node) & (0x10 << ((int32_t)q & 3))) {
            offset = (int32_t)((uint32_t)offset + 0x10000);
        }
    }
}

// collision_file_load_800814B0 at 0x80081550: $t3 = the header's u16 leaf section size, $t2 = the leaf section, the
// vertex lists follow it. A marked file keeps the size's upper bits in the root node.
extern "C" void rush2_track49_collision_load(uint8_t* rdram, recomp_context* ctx) {
    uint8_t tag = root_tag(rdram);
    if (tag & marker) {
        ctx->r11 = (int32_t)(((uint32_t)ctx->r11 & 0xFFFF) | ((uint32_t)(tag & 0x7F) << 16));
    }
}

// collision_node_find_8006CD60 at 0x8006CDA8, per node: $v0 = xmin + xmax, $v1 = zmin + zmax, $t4 / $t7 their
// halves (the split; x >= split and z < split pick the quadrant). 2049 rounds them toward zero.
extern "C" void rush2_track49_collision_split(uint8_t* rdram, recomp_context* ctx) {
    if (split_and_cell_off || !is_2049(rdram)) {
        return;
    }
    int32_t sx = (int32_t)ctx->r2, sz = (int32_t)ctx->r3;
    ctx->r12 = sx / 2;
    ctx->r15 = sz / 2;
}

// The four leaf queries, after Rush 2 worked out the cell in $a1/$a2 (before they are narrowed to 16 bits).
// collision_ground_query_8006CF00 at 0x8006CFBC, collision_poly_extract_8006F704 at 0x8006F7D0 and
// car_wall_collision_query_8006FC94 at 0x8006FD60: $t0 = the point.
extern "C" void rush2_track49_collision_cell(uint8_t* rdram, recomp_context* ctx) {
    set_cell(rdram, ctx, (int32_t)ctx->r8, (int32_t)ctx->r8 + 8);
}
// collision_ground_query_basis_8008BDC0 at 0x8008BF78: the point's x at sp+0xCC, z at sp+0xD4.
extern "C" void rush2_track49_collision_cell_basis(uint8_t* rdram, recomp_context* ctx) {
    set_cell(rdram, ctx, (int32_t)ctx->r29 + 0xCC, (int32_t)ctx->r29 + 0xD4);
}

// The leaf fetches after each query's collision_node_find_cached_8006CE28: $v0 = the node, the quadrant and the
// offset just read from node +0xC + 2q.
// collision_ground_query_8006CF00 at 0x8006CFEC: quadrant $t8, offset $v1.
extern "C" void rush2_track49_collision_leaf_ground(uint8_t* rdram, recomp_context* ctx) {
    leaf_offset(rdram, ctx->r3, ctx->r2, ctx->r24);
}
// collision_poly_extract_8006F704 at 0x8006F820: quadrant $t7, offset $a3.
extern "C" void rush2_track49_collision_leaf_wheel(uint8_t* rdram, recomp_context* ctx) {
    leaf_offset(rdram, ctx->r7, ctx->r2, ctx->r15);
}
// car_wall_collision_query_8006FC94 at 0x8006FD88: quadrant $t6, offset $v1.
extern "C" void rush2_track49_collision_leaf_wall(uint8_t* rdram, recomp_context* ctx) {
    leaf_offset(rdram, ctx->r3, ctx->r2, ctx->r14);
}
// collision_ground_query_basis_8008BDC0 at 0x8008BFAC: quadrant $t8, offset $v1.
extern "C" void rush2_track49_collision_leaf_basis(uint8_t* rdram, recomp_context* ctx) {
    leaf_offset(rdram, ctx->r3, ctx->r2, ctx->r24);
}

extern "C" void collision_ground_query_8006CF00(uint8_t* rdram, recomp_context* ctx);

// Test aid: RUSH2_TEST_COLLISION=1 checks a 2049 course's lookups once per loaded collision file, on the race's first
// physics tick (called from rush2_track49_movers_tick). Every floor polygon (types 0-2, no spline surface) is probed
// with Rush 2's ground query (collision_ground_query_8006CF00) half a unit above points along its edges (pulled 3 %
// toward its middle, every half unit) and at its middle; a probe that finds no floor at that height is a miss: the
// point's leaf doesn't list the polygon. It runs with 2049's lookups, then with Rush 2's split and cell, and logs both
// counts ("[2049] collision self-test").
extern "C" void rush2_track49_collision_selftest(uint8_t* rdram, recomp_context* ctx) {
    static const bool enabled = std::getenv("RUSH2_TEST_COLLISION") != nullptr;
    static uint32_t tested = 0;
    if (!enabled || !is_2049(rdram)) {
        return;
    }
    uint32_t base = (uint32_t)MEM_W(0, (int32_t)file_base);
    if (base == tested) {
        return;
    }
    tested = base;
    uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table), verts = (uint32_t)MEM_W(0, (int32_t)vert_table);
    uint32_t vlists = (uint32_t)MEM_W(0, (int32_t)vlist_table);
    int n_poly = MEM_HU(4, (int32_t)base);

    auto vertex = [&](int i, float out[3]) {
        int32_t v = (int32_t)(verts + i * 8);
        uint16_t frac = MEM_HU(6, v);
        const int shifts[3] = { 10, 5, 0 };
        for (int k = 0; k < 3; k++) {
            out[k] = (float)((int16_t)MEM_HU(k * 2, v) * 32 + ((frac >> shifts[k]) & 31)) / 32.0f;
        }
    };
    std::vector<std::array<float, 3>> probes; // Points on floor polygons.
    for (int pi = 0; pi < n_poly; pi++) {
        int32_t rec = (int32_t)(polys + pi * 0x18);
        uint16_t flags = MEM_HU(0, rec);
        int count = MEM_HU(2, rec) & 0xF;
        if ((flags & 0xF) > 2 || (flags & 0x3000) || count < 3) {
            continue;
        }
        // Rush 2's vertex list: u16 start, then (if more remain) a byte >= 0xC0 with (byte & 0x3F) more in a run.
        std::vector<int> idx;
        int32_t o = (int32_t)(vlists + MEM_HU(0x16, rec));
        while ((int)idx.size() < count) {
            int v = (MEM_BU(0, o) << 8) | MEM_BU(1, o); // A byte stream: o can be odd.
            o += 2;
            int run = 0;
            if (count - (int)idx.size() >= 2 && MEM_BU(0, o) >= 0xC0) {
                run = MEM_BU(0, o) & 0x3F;
                o += 1;
            }
            for (int r = 0; r <= run; r++) idx.push_back(v + r);
        }
        idx.resize(count);
        // Vertex 0 is the world origin; the others are in the polygon's frame (matrix rows: world -> local).
        float m[9];
        for (int k = 0; k < 9; k++) m[k] = (float)(int16_t)MEM_HU(4 + k * 2, rec) / 16384.0f;
        std::vector<std::array<float, 3>> w(count);
        vertex(idx[0], w[0].data());
        for (int i = 1; i < count; i++) {
            float l[3];
            vertex(idx[i], l);
            for (int j = 0; j < 3; j++) w[i][j] = w[0][j] + m[j] * l[0] + m[3 + j] * l[1] + m[6 + j] * l[2];
        }
        std::array<float, 3> c = {};
        for (const auto& v : w) for (int j = 0; j < 3; j++) c[j] += v[j] / count;
        probes.push_back(c);
        for (auto& v : w) for (int j = 0; j < 3; j++) v[j] = c[j] + (v[j] - c[j]) * 0.97f;
        for (int i = 0; i < count; i++) {
            const auto& a = w[i];
            const auto& b = w[(i + 1) % count];
            int steps = std::max(1, (int)(std::hypot(b[0] - a[0], b[2] - a[2]) * 2.0f));
            for (int s = 0; s <= steps; s++) {
                float t = (float)s / steps;
                probes.push_back({ a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t });
            }
        }
    }

    // The game's stack is borrowed below its stack pointer, as battle.cpp's ground_at does.
    auto hits = [&]() {
        recomp_context saved = *ctx;
        uint32_t args = ((uint32_t)saved.r29 - 0x100u) & ~0xFu;   // +0 the point, +0x10 the surface's, +0x20 matrix
        std::vector<bool> hit(probes.size());
        for (size_t i = 0; i < probes.size(); i++) {
            const auto& p = probes[i];
            float q[3] = { p[0], p[1] + 0.5f, p[2] };
            for (int k = 0; k < 3; k++) {
                uint32_t bits;
                memcpy(&bits, &q[k], 4);
                MEM_W(k * 4, (int32_t)args) = (int32_t)bits;
            }
            ctx->r29 = (uint64_t)(int64_t)(int32_t)(((uint32_t)saved.r29 - 0x1000u) & ~0xFu);
            ctx->r4 = (uint64_t)(int64_t)(int32_t)args;
            ctx->r5 = (uint64_t)(int64_t)(int32_t)(args + 0x10);
            ctx->r6 = (uint64_t)(int64_t)(int32_t)(args + 0x20);
            collision_ground_query_8006CF00(rdram, ctx);
            uint32_t poly = (uint32_t)ctx->r2;
            *ctx = saved;
            hit[i] = poly >= 0x80000000u && std::fabs(read_f(rdram, (int32_t)(args + 0x14)) - p[1]) <= 0.05f;
        }
        return hit;
    };
    auto start = std::chrono::steady_clock::now();
    std::vector<bool> with_2049 = hits();
    split_and_cell_off = true;
    std::vector<bool> with_rush2 = hits();
    split_and_cell_off = false;
    int fixed = 0, broken = 0, neither = 0;
    for (size_t i = 0; i < probes.size(); i++) {
        fixed += with_2049[i] && !with_rush2[i];
        broken += !with_2049[i] && with_rush2[i];
        neither += !with_2049[i] && !with_rush2[i];
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // "neither": the query found another surface nearer in height (or a spline-shaped one) under both lookups.
    printf("[2049] collision self-test: %zu probes over %d polygons, leaf section 0x%X bytes: 2049's lookups find the "
           "floor at %d probes where Rush 2's split and cell don't, and miss it at %d where they find it (%d neither; "
           "%.0f ms)\n", probes.size(), n_poly,
           (uint32_t)MEM_HU(8, (int32_t)base) | ((uint32_t)(root_tag(rdram) & 0x7F) << 16), fixed, broken, neither, ms);
    fflush(stdout);
}
