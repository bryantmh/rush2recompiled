// Smooth-shaded collision: a road surface whose tilt changes smoothly across polygon edges, for California Speed's
// tracks and custom mod tracks (docs/custom_tracks.md#smooth-collision).
//
// Rush 2's road is flat polygons. A wheel takes the polygon under it: its height above that polygon's plane along
// the plane's normal, and the polygon's frame (rows across, normal, along; car +0x2F0 + 0x24 x wheel) that the
// suspension and tire forces work in. Two flat polygons meeting at a crease share their edge, so the height is
// continuous across it, but the normal jumps: the wheel's velocity along the new normal jumps with it, the damper
// and the tire see a spike, and the car is kicked. The stock tracks hide the creases of their main roads under the
// spline road (poly flag 0x1000, collision_seg_surface_8006BDA8), whose frame turns smoothly along a segment. A
// converted track has no splines (California Speed's are only on parts of its roads, a mod's only where its author
// built them), so every edge of its road is a crease.
//
// This gives those tracks' floor polygons a smooth normal instead: every vertex gets the mean normal of the floor
// polygons that meet at its position (the same position, to 1/32 ft, since the files repeat vertices), leaving out
// the neighbors that fold away by more than the fold angle so a curb, a ramp lip or a jump edge stays sharp; under
// a wheel the normal is the barycentric blend of the polygon's corner normals, and the wheel's frame is the flat
// frame turned onto it. Two polygons sharing an edge blend the same two corner normals along it, so the normal is
// continuous across the edge. The height stays the flat polygon's (the distance to the flat face, taken along the
// blended normal), so the surface a car rests on is exactly the mesh its author built and the probe's flat-face
// polygon pick still applies; only the tilt the physics sees is smoothed. On California Speed's tracks the height is
// smoothed too: crease fillets round the flat floor's creases (crease_fillet), and the spline road rides its cubic
// raised by a smooth lift instead of the faces (spline_lift); docs/calspeed_research.md, "Spline lift".
//
// Hooked in collision_poly_extract_8006F704 at 0x8006FA70, after the surface dispatch (flat plane, or the spline or
// arc surface for flags 0x1000 / 0x2000, which are left alone): 0x94($sp) = the POLY found, 0x13C($sp) = the wheel's
// point, $s1 = the result vector whose [1] is the height, *0x148($sp) = the wheel's frame (f32[3][3], world -> the
// polygon's frame). The corner normals are built once per loaded collision file (keyed on the POLY and VERT table
// pointers), on the first wheel probe of a race, from the file in RDRAM.
//
// RUSH2_SMOOTH_FOLD=<degrees> overrides the fold angle (default 20); RUSH2_SMOOTH_COLLISION=0 turns the surface off;
// RUSH2_SMOOTH_LOG=1 prints the table's statistics when it is built.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "rush2_hooks.h"
#include "collision_polys.h"
#include "calspeed.h"
#include "track2049.h"

namespace {
    // Rush 2's collision file pointers (collision_file_load_800814B0, docs/rush2049_research/collision.md).
    constexpr uint32_t poly_table_ptr = 0x8011001C;   // POLY records (0x18 bytes)
    constexpr uint32_t vert_table_ptr = 0x80110050;   // VERT records (8 bytes), right after the POLY records
    constexpr uint32_t vlist_table_ptr = 0x80111930;  // vertex index lists
    constexpr uint32_t poly_size = 0x18, vert_size = 8;
    constexpr int vlist_run_mark = 0xC0, vlist_run_mask = 0x3F;
    constexpr uint32_t flag_spline = 0x1000, flag_arc = 0x2000;
    constexpr int max_corners = 15;                   // a POLY's vertex count is its info's low nibble

    // The probe's frame at the hook.
    // Stack slot offsets are int: MEM_W adds the offset to the (int32) base before widening it to the 64-bit address, so an
    // unsigned offset wraps the sum to 32 bits and the access lands 4 GB away.
    constexpr int32_t sp_poly = 0x94, sp_point = 0x13C, sp_frame = 0x148;

    struct Corner {
        float p[3];   // world position
        float n[3];   // smooth normal
    };
    struct Smooth {
        int count = 0;                  // corners, 0 = not a floor polygon
        int first = 0;                  // into corners
        float face[3] = {};             // the flat normal (the frame's row 1)
    };
    // A spline segment's lift (spline_lift): nodes at its start and end (t 0, 1) by 5 across (u -0.5, 0 .. 1.5: the
    // segment's polygons span u -0.5 to 1.5, its half-width parameter covering the middle half), in ft along its normal; lifted = false for a segment with no spline floor polygon.
    struct Lift {
        bool lifted = false;
        float node[2][5] = {};
    };
    struct Table {
        uint32_t polys = 0, verts = 0;  // the file it was built for
        float fold_cos = 0.0f;
        std::vector<Smooth> smooth;
        std::vector<Corner> corners;
        std::vector<Lift> lifts;        // per SEG record, California Speed's tracks only
        // Crease fillets (crease_fillet), California Speed's tracks only: the smoothed floor polygons by 64 ft cell.
        bool fillets = false;
        std::unordered_map<uint64_t, std::vector<int>> cells;
        std::vector<bool> in_cells;     // the polygon is in cells (both sides of an edge must agree)
    };
    Table table;
    constexpr float fillet_cell = 64.0f;
    uint64_t cell_key(int gx, int gz) {
        return (uint64_t)(uint32_t)gx << 32 | (uint32_t)gz;
    }

    // A ledge is a floor standing above the road: a sidewalk's or platform's top, its plane well above the spot the
    // wheel last rested on. A ramp rising from the road starts at the road's height, so its plane, carried back to that
    // spot, is at or under it; but a car driving onto it fast puts its wheel more than ledge_step (below) into it in one
    // step (San Francisco's start ramps, California's crest ramp, at California Speed's 150-250 ft/s), and the ledge
    // rule held the wheel at ledge_rise: the suspension barely pushed, the ramp rose through the car and stood it on
    // end. So a floor whose plane is no more than ramp_rise above the wheel's last resting spot is the road going on
    // and keeps its full depth; the ledge rule is for the others. (Neighbors by shared vertices can't tell them apart:
    // California Speed's road polygons are 1/32 ft apart at their cracks, and platform tops taper to the road at their
    // ends, so they touch it.)
    // support: per car and wheel, the wheel's point on its surface at its last probe that was not held as a ledge.
    struct Rest {
        float p[3];
    };
    std::unordered_map<uint32_t, Rest> support;
    constexpr float ramp_rise = 0.5f;
    bool held_as_ledge = false;           // the current probe's height was clamped by the ledge rule

    const bool enabled = [] {
        const char* v = getenv("RUSH2_SMOOTH_COLLISION");
        return v == nullptr || strcmp(v, "0") != 0;
    }();
    const bool log_on = getenv("RUSH2_SMOOTH_LOG") != nullptr;
    // RUSH2_SMOOTH_LOG=2: every probe of the player's car whose height is more than 1 ft off the usual rest (a kick),
    // with the polygon found and what the dispatch left (before the smooth surface).
    const bool log_probes = log_on && atoi(getenv("RUSH2_SMOOTH_LOG")) >= 2;
    constexpr uint32_t player_car = 0x800F5470;
    constexpr int32_t sp_car = 0x138;
    // The fold angle: the raced custom track's smooth_fold, else 20 degrees; RUSH2_SMOOTH_FOLD overrides both.
    float fold_degrees(uint8_t* rdram) {
        if (const char* v = getenv("RUSH2_SMOOTH_FOLD")) return (float)atof(v);
        float fold = 20.0f;
        if (!rush2::calspeed::racing(rdram)) rush2::track2049::mod_smooth_collision(rdram, &fold);
        return fold;
    }

    float f32(uint8_t* rdram, uint32_t at) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
    void put_f32(uint8_t* rdram, uint32_t at, float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        MEM_W(0, (int32_t)at) = (int32_t)bits;
    }
    float vert_coord(uint8_t* rdram, uint32_t vert, int c) {
        int frac = MEM_HU(6, (int32_t)vert) >> (10 - 5 * c) & 31;
        return (float)((int32_t)(int16_t)MEM_H(2 * c, (int32_t)vert) * 32 + frac) / 32.0f;
    }
    void poly_matrix(uint8_t* rdram, uint32_t poly, float m[9]) {
        for (int k = 0; k < 9; k++) m[k] = (int16_t)MEM_H(0, (int32_t)(poly + 4 + 2 * k)) / 16384.0f;
    }
    int poly_vertices(uint8_t* rdram, uint32_t poly, uint32_t out[16], uint32_t* end = nullptr) {
        int count = MEM_HU(2, (int32_t)poly) & 0xF, n = 0;
        uint32_t p = (uint32_t)MEM_W(0, (int32_t)vlist_table_ptr) + MEM_HU(0x16, (int32_t)poly);
        while (n < count) {
            uint32_t first = (uint32_t)MEM_BU(0, (int32_t)p) << 8 | MEM_BU(1, (int32_t)p);
            p += 2;
            int run = 0;
            if (count - n >= 2 && MEM_BU(0, (int32_t)p) >= vlist_run_mark) {
                run = MEM_BU(0, (int32_t)p) & vlist_run_mask;
                p++;
            }
            for (int k = 0; k <= run && n < count && n < 16; k++) out[n++] = first + k;
        }
        if (end != nullptr) *end = p;   // a spline or arc polygon's u16 segment number follows
        return n;
    }
    void normalize(float v[3]) {
        float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 1e-6f) {
            v[0] /= len;
            v[1] /= len;
            v[2] /= len;
        }
    }
    uint64_t position_key(const float p[3]) {
        // 1/32 ft cells: the files' vertices are 1/32 ft fixed point, so repeated vertices land in the same cell.
        uint64_t k = 0;
        for (int c = 0; c < 3; c++) k = k * 0x100000 + ((uint64_t)(int64_t)std::lround(p[c] * 32.0f) & 0xFFFFF);
        return k;
    }

    // A polygon's vertices in the world: vertex 0 is the origin, the others are in its frame (rows = world -> frame).
    int world_vertices(uint8_t* rdram, uint32_t poly, float out[16][3], float m[9]) {
        uint32_t idx[16];
        int count = poly_vertices(rdram, poly, idx);
        uint32_t verts = (uint32_t)MEM_W(0, (int32_t)vert_table_ptr);
        poly_matrix(rdram, poly, m);
        float o[3];
        for (int c = 0; c < 3; c++) o[c] = vert_coord(rdram, verts + vert_size * idx[0], c);
        for (int k = 0; k < count; k++) {
            float v[3];
            for (int c = 0; c < 3; c++) v[c] = vert_coord(rdram, verts + vert_size * idx[k], c);
            for (int c = 0; c < 3; c++) out[k][c] = k == 0 ? o[c] : o[c] + m[c] * v[0] + m[3 + c] * v[1] + m[6 + c] * v[2];
        }
        return count;
    }

    // Spline road segments (SEG records, 0x84 bytes; docs/rush2049_research/collision.md), for spline_lift.
    constexpr uint32_t seg_table_ptr = 0x80110BD8;    // SEG records
    constexpr uint32_t file_base_ptr = 0x80112448;    // the collision file: u16 SEG count first
    struct Seg {
        float o[3], m[9], across[3], along_l[3], along_r[3];
        float s54, s58, s5C, s60, s64, s68, s6C, s70, s74, s78;
    };
    int seg_count(uint8_t* rdram) {
        uint32_t file = (uint32_t)MEM_W(0, (int32_t)file_base_ptr);
        return file != 0 ? MEM_HU(0, (int32_t)file) : 0;
    }
    Seg read_seg(uint8_t* rdram, int index) {
        uint32_t s = (uint32_t)MEM_W(0, (int32_t)seg_table_ptr) + 0x84u * (uint32_t)index;
        Seg g;
        for (int k = 0; k < 3; k++) {
            g.o[k] = f32(rdram, s + 4 * k);
            g.across[k] = f32(rdram, s + 0x30 + 4 * k);
            g.along_l[k] = f32(rdram, s + 0x3C + 4 * k);
            g.along_r[k] = f32(rdram, s + 0x48 + 4 * k);
        }
        for (int k = 0; k < 9; k++) g.m[k] = f32(rdram, s + 0xC + 4 * k);
        float* scalars[] = { &g.s54, &g.s58, &g.s5C, &g.s60, &g.s64, &g.s68, &g.s6C, &g.s70, &g.s74, &g.s78 };
        for (int k = 0; k < 10; k++) *scalars[k] = f32(rdram, s + 0x54 + 4 * k);
        return g;
    }
    // collision_seg_surface_8006BDA8 for point p on segment s (next segment n): the point's height above the cubic
    // along its normal, t along the segment (0-1) and u across it (0-1). Same as tools/rush2049/collision.py seg_surface.
    void seg_surface(const Seg& s, const Seg& n, const float p[3], float* h, float* t, float* u) {
        float d[3] = { p[0] - s.o[0], p[1] - s.o[1], p[2] - s.o[2] }, l[3];
        for (int r = 0; r < 3; r++) l[r] = s.m[3 * r] * d[0] + s.m[3 * r + 1] * d[1] + s.m[3 * r + 2] * d[2];
        float length = l[0] * (s.s60 - s.s5C) + s.s58;
        float tt = (l[2] - l[0] * s.s5C) / length;
        float uu = l[0] > 0.0f ? 0.5f + 0.5f * l[0] / (l[2] * s.s70 + s.s6C) : 0.5f - 0.5f * l[0] / (l[2] * s.s68 + s.s64);
        float along[3], across[3], normal[3], q[3];
        for (int k = 0; k < 3; k++) {
            along[k] = (s.along_l[k] * (1 - uu) + s.along_r[k] * uu) * (1 - tt) + (n.along_l[k] * (1 - uu) + n.along_r[k] * uu) * tt;
            across[k] = s.across[k] * (1 - tt) + n.across[k] * tt;
        }
        normal[0] = along[1] * across[2] - along[2] * across[1];
        normal[1] = along[2] * across[0] - along[0] * across[2];
        normal[2] = along[0] * across[1] - along[1] * across[0];
        float h0 = s.s74 + (s.s78 - s.s74) * uu, h1 = n.s74 + (n.s78 - n.s74) * uu;
        float lift = (tt * tt * tt * (h0 - h1) - tt * tt * (2 * h0 - h1) + tt * h0) * length;
        float height = 0.0f;
        for (int k = 0; k < 3; k++) {
            float ps = s.o[k] + s.across[k] * (uu - 0.5f) * s.s54, pt = n.o[k] + n.across[k] * (uu - 0.5f) * n.s54;
            q[k] = ps * (1 - tt) + pt * tt - lift * normal[k];
            height += (p[k] - q[k]) * normal[k];
        }
        *h = height;
        *t = tt;
        *u = uu;
    }
    float smoothstep(float f) {
        return f * f * (3.0f - 2.0f * f);
    }
    // The lift at (t, u) on a segment: its nodes blended with smoothstep along and across.
    float lift_at(const Lift& g, float t, float u) {
        float ft = smoothstep(std::clamp(t, 0.0f, 1.0f));
        float uu = std::clamp((u + 0.5f) * 0.5f, 0.0f, 1.0f) * 4.0f;   // the road spans u -0.5 to 1.5
        int j = std::min((int)uu, 3);
        float fu = smoothstep(uu - (float)j);
        float a = g.node[0][j] * (1 - fu) + g.node[0][j + 1] * fu, b = g.node[1][j] * (1 - fu) + g.node[1][j + 1] * fu;
        return a * (1 - ft) + b * ft;
    }
    // A spline polygon's segment number (after its vertex list; unaligned, so read by bytes), or -1.
    int poly_seg(uint8_t* rdram, uint32_t poly) {
        uint32_t idx[16], end = 0;
        poly_vertices(rdram, poly, idx, &end);
        if ((MEM_HU(0, (int32_t)poly) & (flag_spline | flag_arc)) == 0) return -1;
        return (int)((uint32_t)MEM_BU(0, (int32_t)end) << 8 | MEM_BU(1, (int32_t)end));
    }

    // Builds the spline lifts (spline_lift) for the loaded file: per segment and quarter of its width, the most any
    // of its spline floor polygons' faces stands above the cubic (sampled on a 9 x 9 grid per triangle); a node at a
    // segment's end takes the most of the cells either side of it in both directions, so the lifted surface is not
    // under the drawn road (but near joins to flat road, below).
    void build_lifts(uint8_t* rdram, int n) {
        int nseg = seg_count(rdram);
        if (nseg <= 0 || nseg > 0x4000) return;
        std::vector<Seg> segs(nseg);
        for (int s = 0; s < nseg; s++) segs[s] = read_seg(rdram, s);
        std::vector<std::array<float, 4>> cell(nseg, std::array<float, 4>{});
        std::vector<bool> has(nseg, false);
        uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr);
        constexpr int grid = 8;
        for (int i = 0; i < n; i++) {
            uint32_t poly = polys + poly_size * (uint32_t)i;
            uint32_t flags = MEM_HU(0, (int32_t)poly);
            if ((flags & 0xF) >= 4 || !(flags & flag_spline)) continue;
            int s = poly_seg(rdram, poly);
            if (s < 0 || s >= nseg) continue;
            float w[16][3], m[9];
            int count = world_vertices(rdram, poly, w, m);
            if (count < 3) continue;
            has[s] = true;
            const Seg& next = segs[s + 1 < nseg ? s + 1 : 0];
            for (int k = 1; k + 1 < count; k++) {
                for (int a = 0; a <= grid; a++) {
                    for (int b = 0; a + b <= grid; b++) {
                        float fa = (float)a / grid, fb = (float)b / grid, fc = 1.0f - fa - fb, p[3], h, t, u;
                        for (int c = 0; c < 3; c++) p[c] = fa * w[0][c] + fb * w[k][c] + fc * w[k + 1][c];
                        seg_surface(segs[s], next, p, &h, &t, &u);
                        if (!std::isfinite(h) || !std::isfinite(u) || h <= 0.0f) continue;
                        int band = std::clamp((int)((u + 0.5f) * 2.0f), 0, 3);
                        cell[s][band] = std::max(cell[s][band], h);
                    }
                }
            }
        }
        table.lifts.assign(nseg, Lift{});
        int lifted = 0;
        float most = 0.0f;
        for (int s = 0; s < nseg; s++) {
            if (!has[s]) continue;
            int prev = s > 0 ? s - 1 : nseg - 1, next = s + 1 < nseg ? s + 1 : 0;
            Lift& g = table.lifts[s];
            g.lifted = true;
            for (int r = 0; r < 2; r++) {
                int other = r == 0 ? prev : next;
                float row[4] = {};
                // A join to road with no spline (flat polygons) gets no lift: a step there would be a bump. The cubic
                // meets the faces along the join, so the car only sinks a little near it (up to about 1 ft on 1% of
                // the spline road).
                if (has[other]) {
                    for (int y = 0; y < 4; y++) row[y] = std::max(cell[s][y], cell[other][y]);
                }
                for (int y = 0; y < 5; y++) {
                    g.node[r][y] = std::max(row[std::max(y - 1, 0)], row[std::min(y, 3)]);
                    most = std::max(most, g.node[r][y]);
                }
            }
            lifted++;
        }
        if (log_on) {
            printf("[smooth] spline lifts: %d of %d segments, most %.2f ft\n", lifted, nseg, most);
            fflush(stdout);
        }
    }

    // Builds the corner normals for the loaded file.
    void build(uint8_t* rdram, float fold_cos) {
        uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr), verts = (uint32_t)MEM_W(0, (int32_t)vert_table_ptr);
        table = Table{};
        table.polys = polys;
        table.verts = verts;
        table.fold_cos = fold_cos;
        if (polys == 0 || verts <= polys) return;
        int n = (int)((verts - polys) / poly_size);
        if (n <= 0 || n > 0x4000) return;
        table.smooth.resize(n);
        struct Use {
            int poly;
            float n[3];
        };
        std::unordered_map<uint64_t, std::vector<Use>> at;
        at.reserve((size_t)n * 3);
        std::vector<std::vector<Corner>> raw(n);
        for (int i = 0; i < n; i++) {
            uint32_t poly = polys + poly_size * (uint32_t)i;
            uint32_t flags = MEM_HU(0, (int32_t)poly);
            if ((flags & 0xF) >= 4 || (flags & (flag_spline | flag_arc))) continue;   // walls; spline and arc roads
            float w[16][3], m[9];
            int count = world_vertices(rdram, poly, w, m);
            if (count < 3 || count > max_corners) continue;
            Smooth& s = table.smooth[i];
            s.count = count;
            for (int c = 0; c < 3; c++) s.face[c] = m[3 + c];
            normalize(s.face);
            raw[i].resize(count);
            for (int k = 0; k < count; k++) {
                memcpy(raw[i][k].p, w[k], sizeof w[k]);
                at[position_key(w[k])].push_back(Use{ i, { s.face[0], s.face[1], s.face[2] } });
            }
        }
        support.clear();
        int smoothed = 0, sharp = 0;
        for (int i = 0; i < n; i++) {
            Smooth& s = table.smooth[i];
            if (s.count == 0) continue;
            s.first = (int)table.corners.size();
            for (int k = 0; k < s.count; k++) {
                Corner c = raw[i][k];
                float sum[3] = {};
                int used = 0;
                for (const Use& u : at[position_key(c.p)]) {
                    float d = u.n[0] * s.face[0] + u.n[1] * s.face[1] + u.n[2] * s.face[2];
                    if (d < fold_cos) continue;   // folds away more than the fold angle: a crease to keep
                    // Weighted by how near it lies to the face's own plane, falling to nothing at the fold angle: a
                    // neighbor just under the fold counted in full had tilted the road's normal by 8 degrees where a
                    // 17 degree shoulder met Clover's straight (corner -1488, 5, 245), the wheel's frame with it, and
                    // under throttle the tilted drive force lifted the rear 0.3 ft for half a second at each such
                    // corner, every few seconds round the track. The weight's 4th power: coplanar neighbors count 1, a
                    // 10 degree facet (the subway tunnel's curved floor) 0.31, a 17 degree shoulder 0.006 (squared,
                    // 0.56 and 0.076, still tilted that corner 1.3 degrees and the rear still dipped 0.2 ft).
                    // RUSH2_SMOOTH_WEIGHT=0: every neighbor under the fold counts 1 (the old averaging, for comparing).
                    static const bool equal_weights = getenv("RUSH2_SMOOTH_WEIGHT") != nullptr && strcmp(getenv("RUSH2_SMOOTH_WEIGHT"), "0") == 0;
                    float w = fold_cos < 0.9999f && !equal_weights ? (d - fold_cos) / (1.0f - fold_cos) : 1.0f;
                    w *= w;
                    w *= w;
                    for (int a = 0; a < 3; a++) sum[a] += w * u.n[a];
                    used++;
                }
                normalize(sum);
                if (used <= 1 || sum[0] * s.face[0] + sum[1] * s.face[1] + sum[2] * s.face[2] < 0.5f) {
                    memcpy(c.n, s.face, sizeof c.n);
                    sharp++;
                }
                else {
                    memcpy(c.n, sum, sizeof c.n);
                    smoothed++;
                }
                table.corners.push_back(c);
            }
        }
        if (log_on) {
            printf("[smooth] collision file %08X: %d polygons, %zu floor polygons, %d corners smoothed, %d sharp, fold cos %.3f\n",
                   polys, n, table.corners.size(), smoothed, sharp, fold_cos);
            fflush(stdout);
        }
        if (rush2::calspeed::racing(rdram)) {
            auto start = std::chrono::steady_clock::now();
            // RUSH2_SMOOTH_FILLET=0: no crease fillets (a test aid for comparing).
            static const bool fillets_off = getenv("RUSH2_SMOOTH_FILLET") != nullptr && strcmp(getenv("RUSH2_SMOOTH_FILLET"), "0") == 0;
            table.fillets = !fillets_off;
            table.in_cells.assign(n, false);
            for (int i = 0; i < n; i++) {
                const Smooth& s = table.smooth[i];
                if (s.count == 0 || s.face[1] < 0.5f) continue;
                float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
                for (int k = 0; k < s.count; k++) {
                    const float* p = table.corners[s.first + k].p;
                    lo[0] = std::min(lo[0], p[0]);
                    hi[0] = std::max(hi[0], p[0]);
                    lo[1] = std::min(lo[1], p[2]);
                    hi[1] = std::max(hi[1], p[2]);
                }
                int gx0 = (int)std::floor(lo[0] / fillet_cell), gx1 = (int)std::floor(hi[0] / fillet_cell);
                int gz0 = (int)std::floor(lo[1] / fillet_cell), gz1 = (int)std::floor(hi[1] / fillet_cell);
                if ((gx1 - gx0 + 1) * (gz1 - gz0 + 1) > 1024) continue;   // a ground sheet miles across: no fillets
                table.in_cells[i] = true;
                for (int gx = gx0; gx <= gx1; gx++) {
                    for (int gz = gz0; gz <= gz1; gz++) {
                        table.cells[cell_key(gx, gz)].push_back(i);
                    }
                }
            }
            build_lifts(rdram, n);
            if (log_on) {
                printf("[smooth] spline lifts built in %.2f ms\n",
                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                fflush(stdout);
            }
        }
    }

    bool wanted(uint8_t* rdram) {
        if (!enabled) return false;
        // California Speed's tracks, with either physics (src/calspeed/calspeed_physics.cpp).
        if (rush2::calspeed::racing(rdram)) return true;
        // A custom track's: its track.toml smooth_collision (the author's opt-out).
        return rush2::track2049::mod_smooth_collision(rdram);
    }

    // Cracks and ledges. California Speed's floors (and converted custom tracks) have hairline gaps between neighboring
    // polygons, and raised sidewalks and subway platforms a couple of feet above the road's edge whose tops are plain
    // floor polygons with no wall face on their edge, overlapping the road's edge polygons. Rush 2's probe takes only
    // the polygons whose footprint holds the wheel's point exactly, and of those the highest within 5 ft above the
    // wheel: at a crack a wheel finds no floor for a frame, and at a ledge the wheel point that strays under the
    // sidewalk's top is lifted onto it in one step; either way the car is kicked. Here the surface nearest the wheel
    // wins instead, a floor polygon whose footprint is within gap_tol of the point counting too: a ledge more than
    // ledge_step above a wheel that is on a surface is left alone, as a wall face would have stopped it.
    constexpr float gap_tol = 1.0f;       // ft outside a footprint, in the polygon's plane
    constexpr float ledge_step = 0.75f;   // a found surface this far above the wheel's point gives way to one it is on
    constexpr float ledge_rise = 0.1f;    // ... and with none to give way to, the wheel climbs it this much per probe
    // The leaf's polygon indices (u16), their count noted by the leaf hook: the probe's list, moved off its stack (sp+0xA4)
    // to spare RDRAM by rush2_collision_leaf_list (query 1) so a leaf can hold 255.
    int leaf_count = 0;                   // the current probe's leaf polygon count (0 when it found no leaf)

    uint32_t support_key(uint8_t* rdram, uint32_t sp) {
        return (uint32_t)MEM_W(sp_car, (int32_t)sp) + (uint32_t)MEM_W(0x14C, (int32_t)sp);
    }
    // The found polygon's plane is no more than ramp_rise above the wheel's last resting spot: a ramp, not a ledge.
    bool joined_to_support(uint8_t* rdram, uint32_t sp, uint32_t poly) {
        auto it = support.find(support_key(rdram, sp));
        if (it == support.end()) return false;
        float w[16][3], m[9];
        if (world_vertices(rdram, poly, w, m) < 3) return false;
        const float* r = it->second.p;
        float above = m[3] * (r[0] - w[0][0]) + m[4] * (r[1] - w[0][1]) + m[5] * (r[2] - w[0][2]);   // the spot over the plane
        return above > -ramp_rise;
    }

    // A ledge with nothing under it to keep (a sidewalk's or platform's top the wheel's point has fully entered): the
    // height is clamped so the wheel climbs it at ledge_rise per probe (a 1.9 ft platform over about a car length at
    // speed) instead of being lifted onto it in one step, which launches the car.
    // A car coming down fast (a landing: car-frame vertical velocity +0x38 under -10 ft/s) is not on a ledge; it keeps
    // the probe's full height so the floor holds it.
    constexpr float falling_speed = -10.0f;
    bool falling(uint8_t* rdram, uint32_t sp) {
        return f32(rdram, (uint32_t)MEM_W(sp_car, (int32_t)sp) + 0x38) < falling_speed;
    }
    void ledge_clamp(uint8_t* rdram, recomp_context* ctx, bool joined) {
        float h = f32(rdram, (uint32_t)ctx->r17 + 4);
        if (h < -ledge_step && !joined && !falling(rdram, (uint32_t)ctx->r29)) {
            put_f32(rdram, (uint32_t)ctx->r17 + 4, -ledge_rise);
            held_as_ledge = true;
        }
    }

    // Distance from (lx, lz) to a polygon's footprint in its local plane, 0 inside.
    float footprint_distance(const float (*lv)[3], int count, float lx, float lz) {
        bool inside = true;
        int sign = 0;
        float best = 1e30f;
        for (int k = 0; k < count; k++) {
            const float* a = lv[k];
            const float* b = lv[(k + 1) % count];
            float ex = b[0] - a[0], ez = b[2] - a[2], len2 = ex * ex + ez * ez;
            float cross = ex * (lz - a[2]) - ez * (lx - a[0]);
            if (std::fabs(cross) > 1e-9f) {
                int s = cross > 0 ? 1 : -1;
                if (sign == 0) sign = s;
                else if (s != sign) inside = false;
            }
            float t = len2 > 0 ? std::clamp(((lx - a[0]) * ex + (lz - a[2]) * ez) / len2, 0.0f, 1.0f) : 0.0f;
            float dx = lx - a[0] - t * ex, dz = lz - a[2] - t * ez;
            best = std::min(best, std::sqrt(dx * dx + dz * dz));
        }
        return inside ? 0.0f : best;
    }

    // The tolerant search over the probe's leaf: `have` says a polygon was found (its height in $s1[1]); it is kept
    // unless it is a ledge (more than ledge_step above the point). Replaces the probe's result (POLY slot, local point,
    // frame, the car's wheel polygon index) with the floor polygon within gap_tol of the point whose surface is nearest
    // the point. Returns the polygon taken, 0 for none.
    uint32_t fill_gap(uint8_t* rdram, recomp_context* ctx, bool have) {
        uint32_t sp = (uint32_t)ctx->r29;
        uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr);
        if (polys == 0 || leaf_count <= 0) return 0;
        uint32_t pointp = (uint32_t)MEM_W(sp_point, (int32_t)sp);
        float p[3] = { f32(rdram, pointp), f32(rdram, pointp + 4), f32(rdram, pointp + 8) };
        float held = have ? f32(rdram, (uint32_t)ctx->r17 + 4) : 250.0f;
        if (have && (held >= -ledge_step || falling(rdram, sp))) return 0;   // on or above the surface, or landing
        float best = have ? -held - 0.05f : 250.0f;       // the nearest |height| so far
        uint32_t taken = 0;
        float tl[3] = {}, tm[9] = {};
        int tindex = 0;
        for (int i = 0; i < leaf_count; i++) {
            int index = MEM_HU(2 * i, (int32_t)rush2_collision_leaf_list_at(1));
            uint32_t poly = polys + poly_size * (uint32_t)index;
            uint32_t flags = MEM_HU(0, (int32_t)poly);
            if ((flags & 0xF) >= 4) continue;             // walls and the like: floors only
            float w[16][3], m[9];
            int count = world_vertices(rdram, poly, w, m);
            if (count < 3 || m[4] < 0.5f) continue;       // a floor faces up
            float d[3] = { p[0] - w[0][0], p[1] - w[0][1], p[2] - w[0][2] };
            float l[3];
            for (int r = 0; r < 3; r++) l[r] = m[3 * r] * d[0] + m[3 * r + 1] * d[1] + m[3 * r + 2] * d[2];
            if (l[1] <= -5.0f || std::fabs(l[1]) >= best) continue;   // the probe's window, nearer than what is held
            float lv[16][3];
            for (int k = 0; k < count; k++) {
                float e[3] = { w[k][0] - w[0][0], w[k][1] - w[0][1], w[k][2] - w[0][2] };
                for (int r = 0; r < 3; r++) lv[k][r] = m[3 * r] * e[0] + m[3 * r + 1] * e[1] + m[3 * r + 2] * e[2];
            }
            float outside = footprint_distance(lv, count, l[0], l[2]);
            if (outside > gap_tol) continue;
            if (outside > 0.0f && m[4] < 0.94f) continue;  // a slope's plane extended past its edge is not the road
            best = std::fabs(l[1]);
            taken = poly;
            tindex = index;
            memcpy(tl, l, sizeof tl);
            memcpy(tm, m, sizeof tm);
        }
        if (taken == 0) return 0;
        MEM_W(sp_poly, (int32_t)sp) = (int32_t)taken;
        for (int r = 0; r < 3; r++) put_f32(rdram, (uint32_t)ctx->r17 + 4 * (uint32_t)r, tl[r]);
        uint32_t frame = (uint32_t)MEM_W(sp_frame, (int32_t)sp);
        for (int k = 0; k < 9; k++) put_f32(rdram, frame + 4 * (uint32_t)k, tm[k]);
        uint32_t car = (uint32_t)MEM_W(sp_car, (int32_t)sp);
        int wheel = (int)MEM_W(0x14C, (int32_t)sp);
        MEM_H(0x594 + 2 * wheel, (int32_t)car) = (int16_t)tindex;
        ctx->r2 = MEM_HU(0, (int32_t)taken);              // $v0 = the flags, as the code at 0x8006FA70 expects
        if (log_probes && car == player_car) {
            printf("[smooth] crack wheel %d: %s poly %d h %.2f at (%.1f %.1f %.1f)\n", wheel, have ? "ledge" : "crack",
                   tindex, tl[1], p[0], p[1], p[2]);
            fflush(stdout);
        }
        return taken;
    }

    // A spline road polygon (flag 0x1000) holds the wheel on the segment's cubic surface (collision_seg_surface_8006BDA8,
    // California Speed's 0x800AEE54 the same), which meets the polygon at its corners and bows away between them. The
    // road drawn is the flat polygons, and on California Speed's coarse spline triangles the cubic sags up to 1.4 ft
    // under them where the road dips (San Francisco's x 2300-2550, z 1000-1980 and its walled loop, polygons
    // 7120-7288), so the car sank into the road it showed. The wheel is held on the higher of the two: the height to
    // the flat face, measured along the spline's normal (the probe's frame row 1), when the face is above the cubic.
    // California Speed only: a custom track's ROAD_SPLINES road may be drawn finer than its polygons, along the cubic.
    // Replaced by spline_lift (2026-10-27): the higher of the two switched between the cubic and the flat faces at
    // every triangle, so the faces' edges kicked the car (15-40 g at 120 ft/s on Clover, Mojave, Highway 1); kept for
    // an easy revert, and RUSH2_SPLINE_FACE=1 runs it instead.
    void spline_on_face(uint8_t* rdram, recomp_context* ctx, uint32_t poly) {
        float w[16][3], m[9];
        if (world_vertices(rdram, poly, w, m) < 3) return;
        uint32_t pointp = (uint32_t)MEM_W(sp_point, (int32_t)ctx->r29), frame = (uint32_t)MEM_W(sp_frame, (int32_t)ctx->r29);
        float face = 0.0f, dot = 0.0f;
        for (int a = 0; a < 3; a++) {
            face += m[3 + a] * (f32(rdram, pointp + 4 * (uint32_t)a) - w[0][a]);
            dot += m[3 + a] * f32(rdram, frame + 12 + 4 * (uint32_t)a);
        }
        if (dot < 0.5f) return;
        float h = f32(rdram, (uint32_t)ctx->r17 + 4);
        if (face / dot < h) put_f32(rdram, (uint32_t)ctx->r17 + 4, face / dot);
    }

    // California Speed's spline road: the wheel rides the cubic (smooth, as California Speed's own probe does) raised
    // by the segment's lift (build_lifts), so it is never under the drawn faces, which sag up to 2.4 ft above the cubic
    // on Clover's turns. The lift changes smoothly along and across the road (a node per segment end and quarter of
    // its width), so the ride keeps the cubic's smoothness: at 120 ft/s the road's vertical kicks peak at 5-10 g against
    // 15-23 g for the higher of cubic and face (tmp/cs_bumpy/lift.py). The cost is hover: median 0.2-0.35 ft over the
    // drawn road, up to about 3 ft on Clover's worst turn.
    void spline_lift(uint8_t* rdram, recomp_context* ctx, uint32_t poly) {
        int s = poly_seg(rdram, poly), nseg = (int)table.lifts.size();
        if (s < 0 || s >= nseg || !table.lifts[s].lifted || seg_count(rdram) != nseg) return;
        uint32_t pointp = (uint32_t)MEM_W(sp_point, (int32_t)ctx->r29);
        float p[3] = { f32(rdram, pointp), f32(rdram, pointp + 4), f32(rdram, pointp + 8) }, h, t, u;
        seg_surface(read_seg(rdram, s), read_seg(rdram, s + 1 < nseg ? s + 1 : 0), p, &h, &t, &u);
        if (!std::isfinite(t) || !std::isfinite(u)) return;
        float lift = lift_at(table.lifts[s], t, u);
        float lifted = f32(rdram, (uint32_t)ctx->r17 + 4) - lift;
        // At the road's edges (u -0.5 and 1.5) the lifted cubic stood up to 0.8 ft off the flat floor beside it, a step
        // each time a car cut a corner over it (Clover's segments 52-55, 60-154 g in a trace). In the last 0.1 of u on
        // each side (u below -0.4, above 1.4; about 3 ft) the wheel goes over to the polygon's own face, which meets
        // the floor beside it. Narrow on purpose: cars cross it sideways, slowly, but Clover's computer cars drive the
        // inside of its turns (a quarter of their steps below u -0.25), and a wider band (from -0.25) brought the
        // faces' creases back there (spline kicks over 10 g 0.9% -> 3.3%).
        float outside = std::max(-0.4f - u, u - 1.4f);
        if (outside > 0.0f) {
            float w[16][3], m[9];
            uint32_t frame = (uint32_t)MEM_W(sp_frame, (int32_t)ctx->r29);
            if (world_vertices(rdram, poly, w, m) >= 3) {
                float face = 0.0f, dot = 0.0f;
                for (int a = 0; a < 3; a++) {
                    face += m[3 + a] * (p[a] - w[0][a]);
                    dot += m[3 + a] * f32(rdram, frame + 12 + 4 * (uint32_t)a);
                }
                if (dot >= 0.5f) {
                    float k = smoothstep(std::min(outside / 0.1f, 1.0f));
                    lifted = lifted * (1.0f - k) + face / dot * k;
                }
            }
        }
        put_f32(rdram, (uint32_t)ctx->r17 + 4, lifted);
    }
}

// collision_poly_extract_8006F704 at 0x8006F748 (entry, count 0) and 0x8006F860 (the leaf decoded, $t1 = its polygon
// count): remembers the probe's leaf size for the crack search.
extern "C" void rush2_smooth_collision_leaf(uint8_t* rdram, recomp_context* ctx, int count) {
    (void)rdram;
    (void)ctx;
    leaf_count = count;
}

// collision_poly_extract_8006F704 at 0x8006FBD0: no polygon held the wheel's point. On a California Speed or custom
// track a floor polygon within gap_tol of it is taken instead; true means the result is in place and the hook text
// continues at 0x8006FA70 (type, material, frame kept flat: a spline polygon's edge is on its plane).
extern "C" bool rush2_smooth_collision_gap(uint8_t* rdram, recomp_context* ctx) {
    if (!wanted(rdram)) return false;
    return fill_gap(rdram, ctx, false) != 0;
}

// Crease fillets on California Speed's flat floor. The smooth normals turn the wheel's frame smoothly across an edge,
// but its height stayed the flat face's, so the road's slope still changed in one step at every crease: a computer
// car's traced wheels on Clover (polygons about 128 ft across, creases of 3-7 degrees) met kicks over 10 g on 10% of
// their steps on flat road, against 0.4% on Silicon Valley's finer floor. Each edge within fillet_reach of the wheel
// rounds its crease with the quadratic fillet between the two planes: (s_self - s_other) x (r - d)^2 / (4 r) added to
// the height, d the wheel's distance in from the edge, s the planes' slopes across it, r = fillet_reach or half of
// either polygon's width across the edge. The other plane is the floor found just across the edge by position, not by
// shared vertices (the road's polygons miss each other's vertices by 1/32 ft and meet at T-junctions), so both sides
// round the same crease alike and the surface stays continuous. Creases past the fold angle and steps (a curb, a
// ledge) stay sharp. Offline on the traced path (tmp/cs_bumpy/fillet.py): 10.6% of steps over 10 g -> 1.8%, worst 67 g
// -> 18 g, the road moved at most 0.44 ft (95% within 0.2 ft); 15 ft beat 10, 20, 25 and 30.
namespace {
    constexpr float fillet_reach = 15.0f;
    float plane_y(int i, float x, float z) {
        const Smooth& s = table.smooth[i];
        const float* o = table.corners[s.first].p;
        return o[1] - (s.face[0] * (x - o[0]) + s.face[2] * (z - o[2])) / s.face[1];
    }
    bool inside_xz(int i, float x, float z) {
        const Smooth& s = table.smooth[i];
        const Corner* c = &table.corners[s.first];
        for (int k = 1; k + 1 < s.count; k++) {
            const float *a = c[0].p, *b = c[k].p, *e = c[k + 1].p;
            float d = (b[2] - e[2]) * (a[0] - e[0]) + (e[0] - b[0]) * (a[2] - e[2]);
            if (std::fabs(d) < 1e-9f) continue;
            float l1 = ((b[2] - e[2]) * (x - e[0]) + (e[0] - b[0]) * (z - e[2])) / d;
            float l2 = ((e[2] - a[2]) * (x - e[0]) + (a[0] - e[0]) * (z - e[2])) / d;
            if (std::min(std::min(l1, l2), 1.0f - l1 - l2) >= -1e-3f) return true;
        }
        return false;
    }
    // The floor polygon holding (x, z) whose plane there is nearest y_ref, within tol; -1 for none.
    int floor_at(float x, float z, float y_ref, float tol) {
        auto it = table.cells.find(cell_key((int)std::floor(x / fillet_cell), (int)std::floor(z / fillet_cell)));
        if (it == table.cells.end()) return -1;
        int best = -1;
        float best_d = tol;
        for (int i : it->second) {
            if (!inside_xz(i, x, z)) continue;
            float d = std::fabs(plane_y(i, x, z) - y_ref);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        return best;
    }
    // The fillets' height added at (x, z) on floor polygon i, and the slope they add (d/dx, d/dz) when asked.
    float crease_fillet(int i, float x, float z, float* slope_x = nullptr, float* slope_z = nullptr) {
        if (!table.in_cells[i]) return 0.0f;
        const Smooth& s = table.smooth[i];
        const Corner* c = &table.corners[s.first];
        float cx = 0.0f, cz = 0.0f;
        for (int k = 0; k < s.count; k++) {
            cx += c[k].p[0] / (float)s.count;
            cz += c[k].p[2] / (float)s.count;
        }
        float gx = -s.face[0] / s.face[1], gz = -s.face[2] / s.face[1], add = 0.0f;
        for (int k = 0; k < s.count; k++) {
            const float *e0 = c[k].p, *e1 = c[(k + 1) % s.count].p;
            float ex = e1[0] - e0[0], ez = e1[2] - e0[2], len = std::sqrt(ex * ex + ez * ez);
            if (len < 1e-3f) continue;
            ex /= len;
            ez /= len;
            float nx = -ez, nz = ex;   // the edge's inward normal across the floor
            if ((cx - e0[0]) * nx + (cz - e0[2]) * nz < 0.0f) {
                nx = -nx;
                nz = -nz;
            }
            float d = (x - e0[0]) * nx + (z - e0[2]) * nz, along = (x - e0[0]) * ex + (z - e0[2]) * ez;
            if (d < 0.0f || d >= fillet_reach || along < 0.0f || along > len) continue;
            float px = e0[0] + ex * along, pz = e0[2] + ez * along;
            int j = floor_at(px - nx * 0.25f, pz - nz * 0.25f, plane_y(i, px, pz), 0.25f);
            if (j < 0 || j == i || !table.in_cells[j]) continue;
            const Smooth& o = table.smooth[j];
            if (o.face[0] * s.face[0] + o.face[1] * s.face[1] + o.face[2] * s.face[2] < table.fold_cos) continue;
            // Half of each polygon's width across the edge.
            float wi = 0.0f, wj = 0.0f;
            for (int m = 0; m < s.count; m++) wi = std::max(wi, (c[m].p[0] - e0[0]) * nx + (c[m].p[2] - e0[2]) * nz);
            const Corner* co = &table.corners[o.first];
            for (int m = 0; m < o.count; m++) wj = std::max(wj, -((co[m].p[0] - e0[0]) * nx + (co[m].p[2] - e0[2]) * nz));
            float r = std::min(fillet_reach, 0.5f * std::min(wi, wj));
            if (d >= r) continue;
            float ox = -o.face[0] / o.face[1], oz = -o.face[2] / o.face[1];
            float bend = (gx - ox) * nx + (gz - oz) * nz;   // the two planes' slopes across the edge, apart
            add += bend * (r - d) * (r - d) / (4.0f * r);
            if (slope_x != nullptr) {
                *slope_x -= bend * (r - d) / (2.0f * r) * nx;
                *slope_z -= bend * (r - d) / (2.0f * r) * nz;
            }
        }
        return add;
    }
}

// The smooth surface for the found polygon (rush2_smooth_collision_surface).
static void smooth_surface(uint8_t* rdram, recomp_context* ctx) {
    uint32_t sp = (uint32_t)ctx->r29;
    uint32_t poly = (uint32_t)MEM_W(sp_poly, (int32_t)sp);
    uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr), verts = (uint32_t)MEM_W(0, (int32_t)vert_table_ptr);
    static int calls = 0;
    if (log_on && (calls++ % 4000) == 0) {
        printf("[smooth] probe call %d: wanted %d poly %08X polys %08X verts %08X sp %08X s1 %08X car %08X point %08X type %08X frame %08X wheel %d h %.2f\n",
               calls, (int)wanted(rdram), poly, polys, verts, sp, (uint32_t)ctx->r17, (uint32_t)MEM_W(0x138, (int32_t)sp),
               (uint32_t)MEM_W(0x13C, (int32_t)sp), (uint32_t)MEM_W(0x144, (int32_t)sp), (uint32_t)MEM_W(0x148, (int32_t)sp),
               (int)MEM_W(0x14C, (int32_t)sp), f32(rdram, (uint32_t)ctx->r17 + 4));
        fflush(stdout);
    }
    if (polys == 0 || poly < polys) return;
    float fold_cos = (float)std::cos(fold_degrees(rdram) * 3.14159265358979 / 180.0);
    if (table.polys != polys || table.verts != verts || table.fold_cos != fold_cos) build(rdram, fold_cos);
    // A floor rising from where the wheel last rested is the road going on (a ramp): no ledge rule for it.
    bool joined = f32(rdram, (uint32_t)ctx->r17 + 4) < -ledge_step && joined_to_support(rdram, sp, poly);
    if (joined && log_probes && (uint32_t)MEM_W(sp_car, (int32_t)sp) == player_car) {
        uint32_t pointp = (uint32_t)MEM_W(sp_point, (int32_t)sp);
        printf("[smooth] ramp wheel %d: poly %u h %.2f at (%.1f %.1f %.1f)\n", (int)MEM_W(0x14C, (int32_t)sp),
               (poly - polys) / poly_size, f32(rdram, (uint32_t)ctx->r17 + 4), f32(rdram, pointp), f32(rdram, pointp + 4),
               f32(rdram, pointp + 8));
        fflush(stdout);
    }
    if (!joined) {
        if (uint32_t nearer = fill_gap(rdram, ctx, true)) poly = nearer;   // a ledge gives way to the road under it
    }
    if (log_probes && (uint32_t)MEM_W(sp_car, (int32_t)sp) == player_car) {
        float h0 = f32(rdram, (uint32_t)ctx->r17 + 4);
        if (h0 < -1.0f || h0 > 2.0f) {
            uint32_t pointp0 = (uint32_t)MEM_W(sp_point, (int32_t)sp), frame0 = (uint32_t)MEM_W(sp_frame, (int32_t)sp);
            printf("[smooth] probe car %08X wheel %d poly %u flags %04X h %.2f point (%.1f %.1f %.1f) normal (%.2f %.2f %.2f)\n",
                   (uint32_t)MEM_W(sp_car, (int32_t)sp), (int)MEM_W(0x14C, (int32_t)sp), (poly - polys) / poly_size, MEM_HU(0, (int32_t)poly), h0,
                   f32(rdram, pointp0), f32(rdram, pointp0 + 4), f32(rdram, pointp0 + 8),
                   f32(rdram, frame0 + 12), f32(rdram, frame0 + 16), f32(rdram, frame0 + 20));
            fflush(stdout);
        }
    }
    uint32_t index = (poly - polys) / poly_size;
    if (index >= table.smooth.size()) {
        ledge_clamp(rdram, ctx, joined);
        return;
    }
    const Smooth& s = table.smooth[index];
    if (s.count == 0) {
        if ((MEM_HU(0, (int32_t)poly) & flag_spline) && rush2::calspeed::racing(rdram)) {
            // RUSH2_SPLINE_FACE=1: the replaced rule (higher of cubic and face), a test aid for comparing the two.
            static const bool face_rule = getenv("RUSH2_SPLINE_FACE") != nullptr;
            if (face_rule) spline_on_face(rdram, ctx, poly);
            else spline_lift(rdram, ctx, poly);
        }
        ledge_clamp(rdram, ctx, joined);   // a spline or arc polygon
        return;
    }
    const Corner* c = &table.corners[s.first];

    // The wheel's point, flat on the polygon's plane (its height along the flat normal taken off), then the triangle of
    // the polygon's fan it lies in and its barycentric weights there (clamped to the triangle, so a point just outside
    // the footprint, which the probe allows, still blends its nearest edge).
    uint32_t pointp = (uint32_t)MEM_W(sp_point, (int32_t)sp);
    float p[3] = { f32(rdram, pointp), f32(rdram, pointp + 4), f32(rdram, pointp + 8) };
    float d = (p[0] - c[0].p[0]) * s.face[0] + (p[1] - c[0].p[1]) * s.face[1] + (p[2] - c[0].p[2]) * s.face[2];
    float q[3] = { p[0] - d * s.face[0], p[1] - d * s.face[1], p[2] - d * s.face[2] };
    int best = 1;
    float bw[3] = { 1.0f, 0.0f, 0.0f }, best_out = 1e30f;
    for (int k = 1; k + 1 < s.count; k++) {
        // Barycentric weights in the plane: solve with the two edge vectors.
        float e1[3], e2[3], r[3];
        for (int a = 0; a < 3; a++) {
            e1[a] = c[k].p[a] - c[0].p[a];
            e2[a] = c[k + 1].p[a] - c[0].p[a];
            r[a] = q[a] - c[0].p[a];
        }
        float d11 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2], d12 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
        float d22 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
        float r1 = r[0] * e1[0] + r[1] * e1[1] + r[2] * e1[2], r2 = r[0] * e2[0] + r[1] * e2[1] + r[2] * e2[2];
        float det = d11 * d22 - d12 * d12;
        if (std::fabs(det) < 1e-9f) continue;
        float v = (d22 * r1 - d12 * r2) / det, w = (d11 * r2 - d12 * r1) / det, u = 1.0f - v - w;
        float out = std::max(0.0f, -u) + std::max(0.0f, -v) + std::max(0.0f, -w);   // how far outside, in weights
        if (out < best_out) {
            best_out = out;
            best = k;
            bw[0] = u;
            bw[1] = v;
            bw[2] = w;
        }
        if (out == 0.0f) break;
    }
    for (int a = 0; a < 3; a++) bw[a] = std::max(0.0f, bw[a]);
    float sum = bw[0] + bw[1] + bw[2];
    if (sum <= 0.0f) {
        ledge_clamp(rdram, ctx, joined);
        return;
    }
    for (int a = 0; a < 3; a++) bw[a] /= sum;
    const Corner* tri[3] = { &c[0], &c[best], &c[best + 1] };
    float normal[3] = {};
    for (int t = 0; t < 3; t++) {
        for (int a = 0; a < 3; a++) normal[a] += bw[t] * tri[t]->n[a];
    }
    normalize(normal);
    if (normal[0] * s.face[0] + normal[1] * s.face[1] + normal[2] * s.face[2] < 0.5f) {   // nothing sensible
        ledge_clamp(rdram, ctx, joined);
        return;
    }
    // California Speed's tracks: the frame is the filleted surface's own normal (the face's slope plus the fillets'),
    // and the height is taken to that surface. The blended corner normals tilt across a whole polygon (128 ft on
    // Clover) toward each neighbor while the fillets bend the surface only within 15 ft of the edge, so mid-polygon
    // the wheel's frame leaned off the flat road under it; the drive force along that frame then lifted a player's
    // rear 0.1-0.3 ft at each crease, the bump every few seconds round Clover. The fillets' surface is smooth (its
    // slope continuous across every rounded crease), so its normal is too, and flat wherever the road is flat.
    bool on_fillets = table.fillets && s.face[1] >= 0.5f;
    float surface_y = 0.0f;
    if (on_fillets) {
        float sx = -s.face[0] / s.face[1], sz = -s.face[2] / s.face[1];
        surface_y = plane_y((int)index, p[0], p[2]) + crease_fillet((int)index, p[0], p[2], &sx, &sz);
        normal[0] = -sx;
        normal[1] = 1.0f;
        normal[2] = -sz;
        normalize(normal);
    }

    // The frame: row 1 the smooth normal, row 0 the flat across turned onto it, row 2 closing the right-handed set
    // (checked against the flat along so the frame keeps its handedness).
    uint32_t frame = (uint32_t)MEM_W(sp_frame, (int32_t)sp);
    float across[3] = { f32(rdram, frame), f32(rdram, frame + 4), f32(rdram, frame + 8) };
    float along_flat[3] = { f32(rdram, frame + 24), f32(rdram, frame + 28), f32(rdram, frame + 32) };
    float an = across[0] * normal[0] + across[1] * normal[1] + across[2] * normal[2];
    for (int a = 0; a < 3; a++) across[a] -= an * normal[a];
    normalize(across);
    float along[3] = { across[1] * normal[2] - across[2] * normal[1], across[2] * normal[0] - across[0] * normal[2],
                       across[0] * normal[1] - across[1] * normal[0] };
    if (along[0] * along_flat[0] + along[1] * along_flat[1] + along[2] * along_flat[2] < 0.0f) {
        for (int a = 0; a < 3; a++) along[a] = -along[a];
    }
    for (int a = 0; a < 3; a++) {
        put_f32(rdram, frame + 4 * a, across[a]);
        put_f32(rdram, frame + 12 + 4 * a, normal[a]);
        put_f32(rdram, frame + 24 + 4 * a, along[a]);
    }
    // The height: the wheel's distance to the flat face along the smooth normal; on the fillets' surface, to it.
    float h = (p[0] - q[0]) * normal[0] + (p[1] - q[1]) * normal[1] + (p[2] - q[2]) * normal[2];
    if (on_fillets) h = (p[1] - surface_y) * normal[1];
    // A ledge is climbed, not jumped onto (a landing keeps its depth).
    if (h < -ledge_step && !joined && !falling(rdram, sp)) {
        h = -ledge_rise;
        held_as_ledge = true;
    }
    put_f32(rdram, (uint32_t)ctx->r17 + 4, h);
}

// collision_poly_extract_8006F704 at 0x8006FA70: the found polygon's surface is in place (flat, spline or arc). On a
// California Speed or custom track, a flat floor polygon's frame and height are replaced by the smooth surface's, and
// the wheel's point on the surface it ends on becomes its resting spot unless it was held as a ledge.
extern "C" void rush2_smooth_collision_surface(uint8_t* rdram, recomp_context* ctx) {
    if (!wanted(rdram)) return;
    held_as_ledge = false;
    smooth_surface(rdram, ctx);
    uint32_t sp = (uint32_t)ctx->r29;
    uint32_t poly = (uint32_t)MEM_W(sp_poly, (int32_t)sp), polys = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr);
    if (held_as_ledge || polys == 0 || poly < polys) return;
    uint32_t pointp = (uint32_t)MEM_W(sp_point, (int32_t)sp), frame = (uint32_t)MEM_W(sp_frame, (int32_t)sp);
    float h = f32(rdram, (uint32_t)ctx->r17 + 4);
    Rest& rest = support[support_key(rdram, sp)];
    for (int a = 0; a < 3; a++) rest.p[a] = f32(rdram, pointp + 4 * (uint32_t)a) - h * f32(rdram, frame + 12 + 4 * (uint32_t)a);
    // RUSH2_SMOOTH_TRACE=<car address, hex>: that car's every wheel probe, the road point under the wheel and the
    // polygon's flags (tools/calspeed/road_trace.py turns it into the road's vertical kicks the car felt).
    static const uint32_t trace_car = getenv("RUSH2_SMOOTH_TRACE") ? (uint32_t)strtoul(getenv("RUSH2_SMOOTH_TRACE"), nullptr, 16) : 0;
    if (trace_car != 0 && (uint32_t)MEM_W(sp_car, (int32_t)sp) == trace_car) {
        int wheel = (int)MEM_W(0x14C, (int32_t)sp);
        printf("[trace] %d %.3f %.3f %.3f %04X %.3f %.3f\n", wheel, rest.p[0], rest.p[1], rest.p[2], MEM_HU(0, (int32_t)poly), h,
               f32(rdram, trace_car + 0x5CC + 4 * (uint32_t)wheel));   // the wheel's last compression
        fflush(stdout);
    }
}

// Debug (RUSH2_SMOOTH_LOG=2): car_ground_contact_800703EC at 0x8007057C, right after a wheel's probe: $s1 = the car,
// $s3 = the wheel, 0xB8($sp) = the height the probe returned, +0x5CC the wheel's last compression. Every probe of the
// player's car whose height is more than half a foot off is printed (a kick in the making, or a missed floor).
extern "C" void rush2_smooth_debug_contact(uint8_t* rdram, recomp_context* ctx) {
    if (!log_probes) return;
    uint32_t car = (uint32_t)ctx->r17;
    if (car != player_car) return;
    float h = f32(rdram, (uint32_t)ctx->r29 + 0xB8);
    if (h > -0.5f && h < 0.5f) return;
    int wheel = (int)ctx->r19;
    uint32_t pointp = car + 0x230 + 12 * (uint32_t)wheel;
    printf("[smooth] contact wheel %d h %.2f last %.2f type %d point (%.1f %.1f %.1f) normal (%.2f %.2f %.2f)\n", wheel, h,
           f32(rdram, car + 0x5CC + 4 * (uint32_t)wheel), (int)MEM_W(0x5FC + 4 * wheel, (int32_t)car),
           f32(rdram, pointp), f32(rdram, pointp + 4), f32(rdram, pointp + 8),
           f32(rdram, car + 0x2F0 + 0x24 * (uint32_t)wheel + 12), f32(rdram, car + 0x2F0 + 0x24 * (uint32_t)wheel + 16),
           f32(rdram, car + 0x2F0 + 0x24 * (uint32_t)wheel + 20));
    fflush(stdout);
}

int rush2::collision::polygon_count(uint8_t* rdram) {
    uint32_t polys = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr), verts = (uint32_t)MEM_W(0, (int32_t)vert_table_ptr);
    if (polys == 0 || verts <= polys) return 0;
    return (int)((verts - polys) / poly_size);
}

int rush2::collision::polygon(uint8_t* rdram, int i, uint16_t& flags, float out[16][3]) {
    uint32_t poly = (uint32_t)MEM_W(0, (int32_t)poly_table_ptr) + poly_size * (uint32_t)i;
    flags = MEM_HU(0, (int32_t)poly);
    float m[9];
    return world_vertices(rdram, poly, out, m);
}
