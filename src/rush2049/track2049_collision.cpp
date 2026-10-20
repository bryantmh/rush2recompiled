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
// node's unused byte +2 and putting the leaf section size's bits 16-20 in bits 0-4; these hooks take 2049's behavior
// only for a marked file, so Rush 2's own, SF Rush's and custom tracks keep Rush 2's. California Speed's tracks keep
// Rush 2's walk too; with their extended scenery the leaf fetches also take the leaves it adds (leaf_offset).
// Root +2 bit 5 (rush2::collision::wide_leaf_tag), set by the custom track converter on a collision of more than 8,191
// polygons in either form, says the leaf entries are plain u16 polygon numbers instead of Rush 2's 13-bit number +
// 3-bit run (rush2_collision_leaf_read_wide). On a marked file the size's bits 16-20 are in bits 0-4 (2 MB of leaves).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "calspeed_scenery.h"
#include "collision_polys.h"
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
    // On a California Speed track with its extended scenery (src/calspeed/calspeed_scenery.cpp, root tagged
    // scenery_collision_tag) that bit says the child is an index into the far-leaf table instead: u32 leaf offsets
    // after the vertex lists, 4-byte aligned from the file's start.
    void leaf_offset(uint8_t* rdram, gpr& offset, gpr node, gpr q) {
        uint8_t tag = root_tag(rdram);
        if (!(MEM_BU(3, (int32_t)node) & (0x10 << ((int32_t)q & 3)))) {
            return;
        }
        if (tag & marker) {
            offset = (int32_t)((uint32_t)offset + 0x10000);
        }
        else if (tag & rush2::calspeed::scenery_collision_tag) {
            uint32_t base = (uint32_t)MEM_W(0, (int32_t)file_base);
            uint32_t vlists = (uint32_t)MEM_W(0, (int32_t)vlist_table);
            uint32_t table = base + ((vlists - base + MEM_HU(10, (int32_t)base) + 3) & ~3u);
            offset = MEM_W(0, (int32_t)(table + ((uint32_t)offset & 0xFFFF) * 4));
        }
    }
}

// collision_file_load_800814B0 at 0x80081550: $t3 = the header's u16 leaf section size, $t2 = the leaf section, the
// vertex lists follow it. A marked file keeps the size's upper bits in the root node.
extern "C" void rush2_track49_collision_load(uint8_t* rdram, recomp_context* ctx) {
    uint8_t tag = root_tag(rdram);
    if (tag & marker) {
        ctx->r11 = (int32_t)(((uint32_t)ctx->r11 & 0xFFFF) | ((uint32_t)(tag & 0x1F) << 16));
    }
}

// For code that reads leaves itself (California Speed's wall collision, src/calspeed/calspeed_walls.cpp).
uint32_t rush2::collision::leaf_offset(uint8_t* rdram, uint32_t node, int q) {
    gpr offset = (gpr)MEM_HU(0xC + 2 * (q & 3), (int32_t)node);
    ::leaf_offset(rdram, offset, (gpr)(int32_t)node, (gpr)q);
    return (uint32_t)offset;
}

bool rush2::collision::wide_leaves(uint8_t* rdram) {
    return (root_tag(rdram) & wide_leaf_tag) != 0;
}

// collision_leaf_read_8006CCEC at its entry: $a0 = the leaf's polygon count (its first byte), $a1 = the query's u16
// list, $a2 = the entries after the count, $t0 = the polygon to put first (the car's last one). Rush 2 decodes 13-bit
// numbers with 3-bit runs: polygon $t0 goes to the front of the list, the rest fill it from the back. On a file with
// wide leaves each entry is one u16 polygon number, stored the same way; returns 1 when it did the read.
extern "C" int rush2_collision_leaf_read_wide(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2::collision::wide_leaves(rdram)) {
        return 0;
    }
    int32_t count = (int32_t)ctx->r4;
    uint32_t front = (uint32_t)ctx->r5, back = front + 2 * (uint32_t)count - 2, in = (uint32_t)ctx->r6;
    uint32_t first = (uint32_t)ctx->r8;
    for (int32_t i = 0; i < count; i++, in += 2) {
        uint32_t poly = (uint32_t)MEM_BU(0, (int32_t)in) << 8 | MEM_BU(1, (int32_t)in);   // unaligned after the count
        if (poly == first) {
            MEM_H(0, (int32_t)front) = (int16_t)poly;
            front += 2;
        }
        else {
            MEM_H(0, (int32_t)back) = (int16_t)poly;
            back -= 2;
        }
    }
    ctx->r2 = (int32_t)back;
    ctx->r4 = 0;
    ctx->r5 = (int32_t)front;
    ctx->r6 = (int32_t)in;
    return 1;
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
    // California Speed's extended scenery (src/calspeed/calspeed_scenery.cpp) is checked too, with Rush 2's lookups:
    // the polygons whose probes miss are listed.
    bool scenery = (root_tag(rdram) & rush2::calspeed::scenery_collision_tag) != 0;
    if (!enabled || (!is_2049(rdram) && !scenery)) {
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
    std::vector<int> probe_poly;              // Their polygons.
    std::vector<std::array<float, 3>> probe_up; // Half a foot up from them: straight up, or (scenery) the floor's
                                                // normal, as straight up drifts out of a sloped floor's uphill edge.
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
        std::array<float, 3> up = { 0.0f, 0.5f, 0.0f };
        if (scenery) up = { m[3] * 0.5f, m[4] * 0.5f, m[5] * 0.5f };
        probes.push_back(c);
        probe_poly.push_back(pi);
        probe_up.push_back(up);
        for (auto& v : w) for (int j = 0; j < 3; j++) v[j] = c[j] + (v[j] - c[j]) * 0.97f;
        for (int i = 0; i < count; i++) {
            const auto& a = w[i];
            const auto& b = w[(i + 1) % count];
            int steps = std::max(1, (int)(std::hypot(b[0] - a[0], b[2] - a[2]) * 2.0f));
            for (int s = 0; s <= steps; s++) {
                float t = (float)s / steps;
                probes.push_back({ a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t });
                probe_poly.push_back(pi);
                probe_up.push_back(up);
            }
        }
    }

    // The game's stack is borrowed below its stack pointer, as battle.cpp's ground_at does.
    std::vector<int> found_poly(probes.size(), -1);   // what each probe found (the last pass): polygon, height
    std::vector<float> found_h(probes.size(), 0.0f);
    auto hits = [&]() {
        recomp_context saved = *ctx;
        uint32_t args = ((uint32_t)saved.r29 - 0x100u) & ~0xFu;   // +0 the point, +0x10 the surface's, +0x20 matrix
        std::vector<bool> hit(probes.size());
        for (size_t i = 0; i < probes.size(); i++) {
            const auto& p = probes[i];
            float q[3] = { p[0] + probe_up[i][0], p[1] + probe_up[i][1], p[2] + probe_up[i][2] };
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
            // The floor's height under the lifted point (moved off along a sloped floor's normal).
            const auto& u = probe_up[i];
            float expect = p[1] - (u[1] > 0.0f ? (u[0] * u[0] + u[2] * u[2]) / u[1] : 0.0f);
            hit[i] = poly >= 0x80000000u && std::fabs(read_f(rdram, (int32_t)(args + 0x14)) - expect) <= 0.05f;
            found_poly[i] = poly >= 0x80000000u ? (int)((poly - polys) / 0x18) : -1;
            found_h[i] = read_f(rdram, (int32_t)(args + 0x14));
        }
        return hit;
    };
    auto start = std::chrono::steady_clock::now();
    if (scenery) {
        std::vector<bool> hit = hits();
        std::vector<int> missed;   // polygons with a probe that misses
        int misses = 0;
        for (size_t i = 0; i < probes.size(); i++) {
            if (hit[i]) continue;
            misses++;
            if (missed.empty() || missed.back() != probe_poly[i]) missed.push_back(probe_poly[i]);
        }
        std::sort(missed.begin(), missed.end());
        missed.erase(std::unique(missed.begin(), missed.end()), missed.end());
        printf("[scenery] collision self-test: %zu probes over %d polygons, %d miss (another surface nearer in height, or "
               "none), on %zu polygons:", probes.size(), n_poly, misses, missed.size());
        for (int m : missed) printf(" %d", m);
        printf("\n");
        // RUSH2_TEST_COLLISION_FROM=<polygon>: the misses on that polygon and later ones (the scenery's, from the
        // converter's report) in detail: nothing found, or which polygon at what height.
        if (const char* from_env = std::getenv("RUSH2_TEST_COLLISION_FROM")) {
            int from = std::atoi(from_env), none = 0, above = 0, below = 0, shown = 0, none_before = 0;
            for (size_t i = 0; i < probes.size(); i++) {
                if (!hit[i] && probe_poly[i] < from && found_poly[i] < 0) none_before++;
                if (hit[i] || probe_poly[i] < from) continue;
                float d = found_h[i] - probes[i][1];
                if (found_poly[i] < 0) none++;
                else if (d > 0) above++;
                else below++;
                if (found_poly[i] < 0 && shown++ < 25) {
                    printf("[scenery]   polygon %d at (%.1f %.1f %.1f): found %d, %.2f ft %s\n", probe_poly[i], probes[i][0],
                           probes[i][1], probes[i][2], found_poly[i], std::fabs(d), d > 0 ? "over" : "under");
                }
            }
            printf("[scenery] misses from polygon %d: %d found nothing, %d a surface over the probe, %d one under (before it: "
                   "%d found nothing)\n", from, none, above, below, none_before);
        }
        fflush(stdout);
        return;
    }
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
           (uint32_t)MEM_HU(8, (int32_t)base) | ((uint32_t)(root_tag(rdram) & 0x1F) << 16), fixed, broken, neither, ms);
    fflush(stdout);
}
