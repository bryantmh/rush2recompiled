// Custom track converter (include/track_mod.h, docs/custom_tracks.md): track.glb + track.toml -> the five files of a
// Rush 2 track slot (geometry, placement, collision, forward and backward AI path) and its per-track table values.
//
// Spaces: glTF is meters, y up, right handed. The game is feet, y up, with x the glTF x negated (the model dump's
// convention, docs/asset_catalog.md); model vertices are 1/16 ft relative to their placement record.
//
// Objects are recognized by name (a Blender ".001" suffix is ignored):
//   COL_<SURFACE>[_anything]  collision only, not drawn. ROAD, DIRT, GRASS, WALL, WATER, KILL or a [surfaces] name.
//   AIPATH_main               the AI path: a curve with Extrude, exported as a ribbon (read_ribbon).
//   AIPATH_lane1..4           optional: one of the four racing lines, its ribbon's center line (paint_lane); 1 is
//                             the leftmost in the race's direction. A lane left out is offset from AIPATH_main.
//   FINISH, CHECKPOINT_n      gates: Empty, green (Blender Y = glTF -Z) arrow forward, X scale = half the width (m).
//   START                     a point-to-point or run-in race's start line ([track] point_to_point / run_in; FINISH is
//                             then the finish (lap) line only and AIPATH_main is open).
//   START_n                   ignored: Rush 2 lines the grid up behind FINISH itself (said in the report).
//   AISPEED_<mph>             caps the AI's target speed from the nearest path point on (0 lifts the cap).
//   SKY                       the sky: its own model SKYO1, drawn around the camera without depth or fog.
//   BILLBOARD_<name>          a card that turns about its vertical axis to face the camera (Rush 2's node flag
//                             0x8000): its own model, shared by billboards of the same mesh and scale.
//   PROP_KNOCK_*, PROP_TIP_*  props a car knocks flying (gone after 5 s) or tips over, as Rush 2049's cone and signs
//                             (src/rush2049/track2049_props.cpp): its own model, shared by props of the same mesh and
//                             scale, at the node's position and rotation.
//   anything else with a mesh the visible track.
//
// Formats: geometry docs/rush2049_research/geometry.md section 2, placement docs/rush2049_research/placement.md,
// collision docs/rush2049_research/collision.md, AI path docs/rush2049_research/race.md section 3.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#define TOML_EXCEPTIONS 1
#include "toml.hpp"

#include "gltf_scene.h"
#include "texture_upscale.h"
#include "track_mod.h"

namespace {
    using rush2::track_mod::Report;
    using Vec3 = std::array<float, 3>;

    constexpr float feet_per_meter = 3.2808399f;
    constexpr float section_size = 1024.0f;     // ft: visual sections (models) on a grid
    constexpr float vertex_reach = 2000.0f;     // ft: s16 vertices at 1/16 ft reach 2047 ft from their record
    constexpr float spine_step = 20.0f;         // ft between AI path points (the stock files' spacing)
    constexpr int max_checkpoints = 9;          // 10 checkpoint records, the finish line one of them
    constexpr int leaf_limit = 12;              // polygons per quadtree leaf before it splits
    constexpr int min_cell = 32;                // ft: smallest quadtree cell for leaf_limit
    // The game's collision queries copy a leaf into an unchecked u16[36] stack buffer (func_8006CF00, func_8006F704,
    // func_8008BDC0; func_8006FC94 has 40): a longer leaf overwrites the callers' locals, e.g. func_8006FC94's saved
    // point pointer at sp+0x15C, crashing in func_8006C670 (docs/rush2049_research/collision.md). Rush 2's own tracks
    // reach 34. A leaf past this splits below min_cell until it fits.
    constexpr int max_leaf = 36;

    // Render state of Rush 2's own track sections (most common in HAWAII's lists).
    constexpr uint32_t mode_opaque = 0xC8112230;   // fog, then AA z-buffered opaque surface
    constexpr uint32_t mode_cutout = 0xC8113278;   // fog, then AA z-buffered alpha-tested (texture edge)
    constexpr uint32_t mode_blend = 0xC8104A50;    // fog, then AA z-buffered translucent (no depth write)
    constexpr uint32_t late_pass = 0x40000;        // placement/node flag: drawn after the opaque nodes (transparent)
    constexpr uint32_t camera_card = 0x8000;       // node flag: an upright card turned to the camera (view_billboard_basis)
    constexpr uint32_t combine[2] = { 0xFC127FFF, 0xFFFFF238 };

    Vec3 sub(const Vec3& a, const Vec3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    Vec3 add(const Vec3& a, const Vec3& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
    Vec3 mul(const Vec3& a, float s) { return { a[0] * s, a[1] * s, a[2] * s }; }
    float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    Vec3 cross(const Vec3& a, const Vec3& b) {
        return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    }
    float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
    Vec3 normalize(const Vec3& a) { float l = length(a); return l > 1e-12f ? mul(a, 1.0f / l) : Vec3{ 0, 0, 0 }; }
    float dist_xz(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[2] - b[2]); }

    // glTF point / direction -> game.
    Vec3 to_game(const Vec3& p) { return { -p[0] * feet_per_meter, p[1] * feet_per_meter, p[2] * feet_per_meter }; }
    Vec3 dir_to_game(const Vec3& d) { return { -d[0], d[1], d[2] }; }

    void add8(std::vector<uint8_t>& d, uint8_t v) { d.push_back(v); }
    void add16(std::vector<uint8_t>& d, uint16_t v) { d.push_back(uint8_t(v >> 8)); d.push_back(uint8_t(v)); }
    void add32(std::vector<uint8_t>& d, uint32_t v) { for (int s = 24; s >= 0; s -= 8) d.push_back(uint8_t(v >> s)); }
    void addf(std::vector<uint8_t>& d, float f) { uint32_t u; memcpy(&u, &f, 4); add32(d, u); }
    void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) d[o + i] = uint8_t(v >> (24 - 8 * i)); }
    void align8(std::vector<uint8_t>& d) { while (d.size() % 8) d.push_back(0); }
    void add_name(std::vector<uint8_t>& d, const std::string& n) {
        for (int i = 0; i < 16; i++) d.push_back(i < (int)n.size() && i < 15 ? uint8_t(n[i]) : 0);
    }

    // "COL_ROAD_main.001" -> "COL_ROAD_main".
    std::string base_name(const std::string& n) {
        size_t dot_at = n.rfind('.');
        if (dot_at != std::string::npos && dot_at + 1 < n.size() &&
            std::all_of(n.begin() + dot_at + 1, n.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
            return n.substr(0, dot_at);
        }
        return n;
    }
    std::string upper(std::string s) { for (char& c : s) c = char(std::toupper((unsigned char)c)); return s; }
    bool starts(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
    // "CHECKPOINT_12" with prefix "CHECKPOINT_" -> 12, else -1.
    int number_after(const std::string& s, const std::string& p) {
        if (!starts(s, p) || s.size() == p.size() || s.size() - p.size() > 6) return -1;
        for (size_t i = p.size(); i < s.size(); i++) if (!std::isdigit((unsigned char)s[i])) return -1;
        return std::stoi(s.substr(p.size()));
    }

    std::string fmt_ft(float ft) {
        char b[64];
        snprintf(b, sizeof(b), "%.0f m (%.0f ft)", ft / feet_per_meter, ft);
        return b;
    }
    std::string fmt_pos(const Vec3& g) { // a game point as the author's Blender coordinates (meters, Z up)
        char b[96];
        snprintf(b, sizeof(b), "(%.1f, %.1f, %.1f)", -g[0] / feet_per_meter, -g[2] / feet_per_meter, g[1] / feet_per_meter);
        return b;
    }

    // ---- AI path ribbon ------------------------------------------------------------------------------------------
    // AIPATH_main's ribbon as rungs across it (one per pair of facing edge vertices, game space), in order along the
    // curve. Built from the mesh's shape, not the order its vertices are stored in: an exporter splits a vertex
    // wherever its UV or normal changes (a cyclic curve's UVs wrap from 1 to 0 at its start, so Blender writes the
    // first rung's vertices twice; a flat-shaded ribbon gets a copy per face) and orders them as it likes. Vertices in
    // the same place are merged unless an edge joins them (a rung pinched to nothing, as one of Atari's is, stays two
    // vertices rather than becoming a bow tie); the ribbon is then a strip whose boundary is one loop round both edges and its two
    // ends (an open curve) or two loops, one per edge (a cyclic curve). The two edges are matched up where the rungs
    // across them come out shortest in all. The first rung, the direction and which edge is a are the ones the stored
    // order gives (lowest vertex index first), so a plain export reads as it always has.
    struct Ribbon {
        std::vector<std::pair<Vec3, Vec3>> rungs;
        // Per rung, the authored AI values (the mean of its two vertices' _SPEED in mph, the first's _AI_MODE); NaN
        // where the mesh has no such attribute.
        std::vector<float> speed, mode;
        bool closed = false;
        std::string problem;   // why it isn't a ribbon (rungs empty)
    };

    Ribbon read_ribbon(const rush2::gltf::Node& node) {
        Ribbon r;
        constexpr float weld = 0.01f;   // ft
        std::vector<Vec3> pos;          // merged vertices
        std::vector<uint32_t> first;    // each one's lowest stored index (across primitives in order)
        std::vector<std::vector<uint32_t>> members;   // each one's stored indices
        std::set<std::pair<uint32_t, uint32_t>> joined;   // stored index pairs an edge joins
        std::map<std::array<int64_t, 3>, std::vector<uint32_t>> cells;
        auto cell = [&](const Vec3& p) {
            return std::array<int64_t, 3>{ int64_t(std::floor(p[0] / weld)), int64_t(std::floor(p[1] / weld)),
                                           int64_t(std::floor(p[2] / weld)) };
        };
        auto merged = [&](const Vec3& p, uint32_t stored) -> uint32_t {
            auto c = cell(p);
            for (int64_t dx = -1; dx <= 1; dx++) for (int64_t dy = -1; dy <= 1; dy++) for (int64_t dz = -1; dz <= 1; dz++) {
                auto it = cells.find({ c[0] + dx, c[1] + dy, c[2] + dz });
                if (it == cells.end()) continue;
                for (uint32_t v : it->second) {
                    if (length(sub(pos[v], p)) > weld) continue;
                    bool edge = false;
                    for (uint32_t m : members[v]) edge = edge || joined.count({ std::min(m, stored), std::max(m, stored) });
                    if (edge) continue;
                    first[v] = std::min(first[v], stored);
                    members[v].push_back(stored);
                    return v;
                }
            }
            pos.push_back(p);
            first.push_back(stored);
            members.push_back({ stored });
            cells[c].push_back(uint32_t(pos.size() - 1));
            return uint32_t(pos.size() - 1);
        };
        std::set<std::array<uint32_t, 3>> tris;
        std::vector<float> stored_speed, stored_mode;   // by stored index
        uint32_t base = 0;
        for (const auto& prim : node.primitives) {
            std::vector<uint32_t> id(prim.pos.size());
            auto attr = [&](const char* name, size_t i) {
                auto it = prim.attrs.find(name);
                return it == prim.attrs.end() ? std::nanf("") : it->second[i];
            };
            for (size_t i = 0; i < prim.pos.size(); i++) {
                stored_speed.push_back(attr("_SPEED", i));
                stored_mode.push_back(attr("_AI_MODE", i));
            }
            for (size_t t = 0; t + 2 < prim.tris.size(); t += 3) {
                for (int k = 0; k < 3; k++) {
                    uint32_t i = base + prim.tris[t + k], j = base + prim.tris[t + (k + 1) % 3];
                    joined.insert({ std::min(i, j), std::max(i, j) });
                }
            }
            for (size_t i = 0; i < prim.pos.size(); i++) id[i] = merged(to_game(prim.pos[i]), base + uint32_t(i));
            for (size_t t = 0; t + 2 < prim.tris.size(); t += 3) {
                std::array<uint32_t, 3> v = { id[prim.tris[t]], id[prim.tris[t + 1]], id[prim.tris[t + 2]] };
                if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;   // collapsed by the merge
                std::sort(v.begin(), v.end());
                tris.insert(v);
            }
            base += uint32_t(prim.pos.size());
        }
        if (tris.empty()) {
            r.problem = "it has no faces";
            return r;
        }
        std::map<std::pair<uint32_t, uint32_t>, int> edges;
        for (const auto& t : tris) {
            for (int k = 0; k < 3; k++) edges[{ std::min(t[k], t[(k + 1) % 3]), std::max(t[k], t[(k + 1) % 3]) }]++;
        }
        std::map<uint32_t, std::vector<uint32_t>> rim;   // boundary neighbors
        for (const auto& [e, n] : edges) {
            if (n > 2) {
                r.problem = "an edge at " + fmt_pos(pos[e.first]) + " is shared by " + std::to_string(n) + " faces";
                return r;
            }
            if (n == 1) { rim[e.first].push_back(e.second); rim[e.second].push_back(e.first); }
        }
        for (const auto& [v, nb] : rim) {
            if (nb.size() != 2) {
                r.problem = "its edges meet at " + fmt_pos(pos[v]) + " (is it more than one strip, or does it touch itself?)";
                return r;
            }
        }
        if (rim.empty()) {
            r.problem = "it is a closed tube with no edges (is Bevel Depth set?)";
            return r;
        }
        std::vector<std::vector<uint32_t>> loops;
        std::set<uint32_t> seen;
        for (const auto& [v0, nb0] : rim) {
            if (seen.count(v0)) continue;
            std::vector<uint32_t> loop{ v0 };
            seen.insert(v0);
            uint32_t prev = v0, cur = nb0[0];
            while (cur != v0) {
                loop.push_back(cur);
                seen.insert(cur);
                const auto& nb = rim[cur];
                uint32_t next = nb[0] == prev ? nb[1] : nb[0];
                prev = cur;
                cur = next;
            }
            loops.push_back(std::move(loop));
        }
        // Side a's vertex k and side b's facing it; the matching with the shortest rungs in all.
        std::vector<uint32_t> a, b;
        auto best_match = [&](size_t n, const std::function<uint32_t(size_t, size_t, int)>& pa,
                              const std::function<uint32_t(size_t, size_t, int)>& pb, size_t shifts, bool both_ways) {
            size_t stride = std::max<size_t>(1, n / 256);
            double best = 1e300;
            size_t best_s = 0;
            int best_d = 1;
            for (int d = 1; d >= (both_ways ? -1 : 1); d -= 2) {
                for (size_t s = 0; s < shifts; s++) {
                    double sum = 0.0;
                    for (size_t k = 0; k < n && sum < best; k += stride) sum += length(sub(pos[pa(s, k, d)], pos[pb(s, k, d)]));
                    if (sum < best) { best = sum; best_s = s; best_d = d; }
                }
            }
            a.resize(n);
            b.resize(n);
            for (size_t k = 0; k < n; k++) { a[k] = pa(best_s, k, best_d); b[k] = pb(best_s, k, best_d); }
        };
        if (loops.size() == 1) {
            // Open: a(0..n-1) then b(n-1..0) round the one loop.
            const auto& L = loops[0];
            size_t m = L.size();
            if (m % 2 || m < 4) {
                r.problem = "its two edges have different numbers of points (is it a curve with Extrude?)";
                return r;
            }
            size_t n = m / 2;
            best_match(n, [&](size_t s, size_t k, int) { return L[(s + k) % m]; },
                       [&](size_t s, size_t k, int) { return L[(s + 2 * m - 1 - k) % m]; }, m, false);
        }
        else if (loops.size() == 2) {
            // Cyclic: one loop per edge.
            const auto& P = loops[0];
            const auto& Q = loops[1];
            if (P.size() != Q.size() || P.size() < 3) {
                r.problem = "its two edges have different numbers of points (is it a curve with Extrude?)";
                return r;
            }
            size_t n = P.size();
            best_match(n, [&](size_t, size_t k, int) { return P[k]; },
                       [&](size_t s, size_t k, int d) { return Q[d > 0 ? (s + k) % n : (s + n - k) % n]; }, n, true);
            r.closed = true;
        }
        else {
            r.problem = "it has " + std::to_string(loops.size()) + " separate edges (is it more than one strip, or has it holes?)";
            return r;
        }
        // Stored order: start at the rung with the lowest vertex index (an open ribbon at the end nearer it), run
        // toward lower indices, side a holding the start's lowest.
        size_t n = a.size();
        auto key = [&](size_t k) { return std::min(first[a[k]], first[b[k]]); };
        size_t start = 0;
        bool forward = true;
        if (r.closed) {
            for (size_t k = 1; k < n; k++) if (key(k) < key(start)) start = k;
            forward = key((start + 1) % n) <= key((start + n - 1) % n);
        }
        else if (key(n - 1) < key(0)) {
            start = n - 1;
            forward = false;
        }
        bool swap = first[b[start]] < first[a[start]];
        for (size_t i = 0; i < n; i++) {
            size_t k = forward ? (start + i) % n : (start + n - i) % n;
            Vec3 pa = pos[a[k]], pb = pos[b[k]];
            r.rungs.push_back(swap ? std::make_pair(pb, pa) : std::make_pair(pa, pb));
            float sa = stored_speed[first[a[k]]], sb = stored_speed[first[b[k]]];
            r.speed.push_back(std::isnan(sa) ? sb : std::isnan(sb) ? sa : (sa + sb) * 0.5f);
            float ma = stored_mode[first[a[k]]];
            r.mode.push_back(std::isnan(ma) ? stored_mode[first[b[k]]] : ma);
        }
        return r;
    }

    // ---- collision -----------------------------------------------------------------------------------------------
    struct CollisionTri {
        Vec3 v[3];          // game space
        Vec3 front;         // the author's front-face normal, game space
        uint16_t flags;     // type | material << 4
        std::string object;
        int group = 0;      // moving object (MOVE_*) it belongs to, 1-31; 0 for the fixed track
        int seg = -1;       // spline road segment (ROAD_SPLINES, the face's _SEGMENT) giving its height; -1 flat
    };

    constexpr int max_poly_verts = 8;       // stock polygons: 3 and 4 almost always, at most 9

    struct BuiltPoly {
        uint16_t flags;
        int16_t m[9];
        Vec3 v0;                            // origin (world)
        std::vector<Vec3> l;                // the other vertices, local (y = 0)
        float x0, x1, z0, z1;
        std::vector<std::array<float, 2>> xz;   // world x/z outline (convex), for the quadtree's cell test
        int group = 0;                      // CollisionTri::group
        int seg = -1;                       // CollisionTri::seg (flag 0x1000 and the vertex list's trailer)
        // A moving polygon's x/z boxes (x0, x1, z0, z1) along its whole travel: the cells it is listed in.
        std::vector<std::array<float, 4>> sweep;
    };

    void encode_vert(std::vector<uint8_t>& d, const Vec3& v) {
        int32_t q[3];
        for (int k = 0; k < 3; k++) q[k] = int32_t(std::floor(v[k] * 32.0f + 0.5f));
        for (int k = 0; k < 3; k++) add16(d, uint16_t(int16_t(q[k] >> 5)));
        add16(d, uint16_t(((q[0] & 31) << 10) | ((q[1] & 31) << 5) | (q[2] & 31)));
    }

    // sweep(p): fills a moving polygon's (group > 0) BuiltPoly::sweep, before the index is built. segs: the spline
    // road segments (SEG records, 0x84 bytes each, as the file holds them) that faces' CollisionTri::seg name.
    bool build_collision(const std::vector<CollisionTri>& tris, std::vector<uint8_t>& out, Report& report,
                         std::vector<BuiltPoly>* polys_out, const std::function<void(BuiltPoly&)>& sweep = {},
                         const std::vector<uint8_t>& segs = {}) {
        // Each face keeps the side its normal gives it, down-facing drivable ones included: the stock tracks have up to
        // 294 per track, nearly all tunnel roofs and bridge undersides above other road (tools/stock_track_glb.py).
        // The vertices wind so that, with x along the first edge and y the normal, every later vertex has local z > 0
        // (z = x cross y; every stock polygon does: func_8006C670's edge tests).
        struct Face { uint16_t flags; Vec3 n; std::vector<Vec3> v; bool dead = false; int group = 0; int seg = -1; };
        std::vector<Face> faces;
        size_t degenerate = 0;
        auto same = [](const Vec3& a, const Vec3& b) {     // one point at the file's 1/32 ft
            for (int k = 0; k < 3; k++) if (std::lround(a[k] * 32.0f) != std::lround(b[k] * 32.0f)) return false;
            return true;
        };
        for (const CollisionTri& t : tris) {
            Vec3 n = normalize(cross(sub(t.v[1], t.v[0]), sub(t.v[2], t.v[0])));
            // A sliver whose corners meet at the file's precision would merge into a polygon with a repeated corner,
            // which has no local frame and collapses to a point (a hole).
            if (length(n) < 0.5f || same(t.v[0], t.v[1]) || same(t.v[1], t.v[2]) || same(t.v[0], t.v[2])) { degenerate++; continue; }
            if (dot(n, t.front) < 0) n = mul(n, -1.0f);       // the face's own front
            Face f{ t.flags, n, { t.v[0], t.v[1], t.v[2] }, false, t.group, t.seg };
            if (dot(cross(sub(f.v[1], f.v[0]), sub(f.v[2], f.v[0])), n) > 0) std::swap(f.v[1], f.v[2]);
            faces.push_back(std::move(f));
        }
        if (degenerate) report.info(std::to_string(degenerate) + " collision triangles have no area and were skipped.");

        // Flat neighbors of the same surface merge into convex polygons, as the stock collision is built (mostly
        // quads): a modeller's triangles would otherwise take one of the game's 8191 polygons each.
        auto key = [](const Vec3& p) {
            return std::array<int32_t, 3>{ int32_t(std::lround(p[0] * 32.0f)), int32_t(std::lround(p[1] * 32.0f)),
                                           int32_t(std::lround(p[2] * 32.0f)) };
        };
        using Edge = std::pair<std::array<int32_t, 3>, std::array<int32_t, 3>>;
        std::map<Edge, size_t> edge_of;
        for (size_t i = 0; i < faces.size(); i++) {
            const auto& v = faces[i].v;
            for (size_t k = 0; k < v.size(); k++) edge_of[{ key(v[k]), key(v[(k + 1) % v.size()]) }] = i;
        }
        auto convex = [](const std::vector<Vec3>& v, const Vec3& n) {
            for (size_t k = 0; k < v.size(); k++) {
                const Vec3 &a = v[k], &b = v[(k + 1) % v.size()], &c = v[(k + 2) % v.size()];
                if (dot(cross(sub(b, a), sub(c, b)), n) > 0.01f) return false;
            }
            return true;
        };
        for (bool merged = true; merged;) {
            merged = false;
            for (size_t i = 0; i < faces.size(); i++) {
                Face& a = faces[i];
                if (a.dead) continue;
                for (size_t k = 0; k < a.v.size() && a.v.size() < size_t(max_poly_verts); k++) {
                    const Vec3 &p = a.v[k], &q = a.v[(k + 1) % a.v.size()];
                    auto it = edge_of.find({ key(q), key(p) });
                    if (it == edge_of.end() || it->second == i) continue;
                    Face& b = faces[it->second];
                    if (b.dead || b.flags != a.flags || b.group != a.group || b.seg != a.seg || dot(a.n, b.n) < 0.9995f) continue;
                    if (a.v.size() + b.v.size() - 2 > size_t(max_poly_verts)) continue;
                    bool flat = true;
                    for (const Vec3& w : b.v) flat = flat && std::fabs(dot(sub(w, a.v[0]), a.n)) < 0.05f;
                    if (!flat) continue;
                    // b runs q, p, then its own vertices: those are spliced between p and q.
                    size_t j = 0;
                    while (j < b.v.size() && key(b.v[j]) != key(q)) j++;
                    if (j == b.v.size()) continue;
                    std::vector<Vec3> joined(a.v.begin(), a.v.begin() + k + 1);
                    for (size_t s = 2; s < b.v.size(); s++) joined.push_back(b.v[(j + s) % b.v.size()]);
                    joined.insert(joined.end(), a.v.begin() + k + 1, a.v.end());
                    if (!convex(joined, a.n)) continue;
                    for (size_t s = 0; s < b.v.size(); s++) edge_of.erase({ key(b.v[s]), key(b.v[(s + 1) % b.v.size()]) });
                    for (size_t s = 0; s < a.v.size(); s++) edge_of.erase({ key(a.v[s]), key(a.v[(s + 1) % a.v.size()]) });
                    a.v = std::move(joined);
                    for (size_t s = 0; s < a.v.size(); s++) edge_of[{ key(a.v[s]), key(a.v[(s + 1) % a.v.size()]) }] = i;
                    b.dead = true;
                    merged = true;
                    k = size_t(-1);     // start this face's edges over
                }
            }
        }

        std::vector<BuiltPoly> polys;
        for (const Face& f : faces) {
            if (f.dead) continue;
            const std::vector<Vec3>& v = f.v;
            Vec3 x = normalize(sub(v[1], v[0]));
            Vec3 z = normalize(cross(x, f.n));
            Vec3 y = normalize(cross(z, x));
            BuiltPoly p{};
            p.flags = f.seg >= 0 ? uint16_t(f.flags | 0x1000) : f.flags;   // 0x1000: height from SEG f.seg
            p.group = f.group;
            p.seg = f.seg;
            const Vec3* rows[3] = { &x, &y, &z };
            float mq[9];
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
                p.m[r * 3 + c] = int16_t(std::clamp(std::lround((*rows[r])[c] * 16384.0f), -32767l, 32767l));
                mq[r * 3 + c] = p.m[r * 3 + c] / 16384.0f;
            }
            p.v0 = v[0];
            p.x0 = p.x1 = v[0][0];
            p.z0 = p.z1 = v[0][2];
            for (const Vec3& w : v) p.xz.push_back({ w[0], w[2] });
            for (size_t k = 1; k < v.size(); k++) {
                Vec3 d = sub(v[k], v[0]);
                p.l.push_back(Vec3{ mq[0] * d[0] + mq[1] * d[1] + mq[2] * d[2], 0.0f, mq[6] * d[0] + mq[7] * d[1] + mq[8] * d[2] });
                p.x0 = std::min(p.x0, v[k][0]); p.x1 = std::max(p.x1, v[k][0]);
                p.z0 = std::min(p.z0, v[k][2]); p.z1 = std::max(p.z1, v[k][2]);
            }
            if (p.group > 0 && sweep) {
                sweep(p);
                for (const auto& b : p.sweep) {
                    p.x0 = std::min(p.x0, b[0]); p.x1 = std::max(p.x1, b[1]);
                    p.z0 = std::min(p.z0, b[2]); p.z1 = std::max(p.z1, b[3]);
                }
            }
            polys.push_back(std::move(p));
        }
        if (polys.empty()) { report.error("No collision: add COL_ROAD_* meshes (or a visible track mesh)."); return false; }
        // Past the game's limits the track still loads, with the polygons it can't hold left out (holes).
        size_t far = 0;
        Vec3 far_at{};
        polys.erase(std::remove_if(polys.begin(), polys.end(), [&](const BuiltPoly& p) {
            bool out = std::fabs(p.v0[1]) > 32000.0f;
            for (float c : { p.x0, p.x1, p.z0, p.z1 }) out = out || std::fabs(c) > 32000.0f;
            if (out && far++ == 0) far_at = p.v0;
            return out;
        }), polys.end());
        if (far) {
            report.error("Collision reaches " + fmt_pos(far_at) + ", farther than the game allows (about 9,750 m from "
                         "the origin); " + std::to_string(far) + " polygon(s) out there are left out.");
        }
        if (polys.empty()) { report.error("No collision: add COL_ROAD_* meshes (or a visible track mesh)."); return false; }
        if (polys.size() > 8191) {
            report.error("Collision has " + std::to_string(polys.size()) + " polygons after merging flat neighbors; the "
                         "game's limit is 8191, so the last " + std::to_string(polys.size() - 8191) + " are left out "
                         "(holes). Simplify the COL_* meshes.");
            polys.resize(8191);
        }

        // Quadtree over x/z (func_8006CD60): q = (x >= mid) + 2 * (z < mid); the halves share the mid line. Built
        // Rush 2's way first; when the cell lists pass Rush 2's 64 KB (as Rush 2049's track 3 does), again in the form
        // the port of 2049's tracks reads (src/rush2049/track2049_collision.cpp): splits rounded toward zero, leaf
        // offsets past 16 bits flagged by mask bit 4 + q, and the root node's byte +2 = 0x80 | the size's bits 16-22.
        struct Node { int16_t parent; uint8_t mask; int16_t b[4]; uint16_t child[4]; };
        std::vector<Node> nodes;
        std::vector<uint8_t> leaves;
        std::map<std::vector<uint8_t>, uint32_t> leaf_at;
        bool overflow = false, tagged = false, crowded = false;
        auto encode_leaf = [&](const std::vector<int>& list) -> uint32_t {
            if (list.empty()) return 0;
            std::vector<uint8_t> raw;
            raw.push_back(uint8_t(list.size()));
            for (size_t i = 0; i < list.size();) {
                size_t j = i + 1;
                while (j < list.size() && list[j] == list[j - 1] + 1 && j - i < 8) j++;
                add16(raw, uint16_t(((j - i - 1) << 13) | list[i]));
                i = j;
            }
            auto it = leaf_at.find(raw);
            if (it != leaf_at.end()) return it->second;
            if (leaves.size() + raw.size() > (tagged ? 0x7FFFFFu : 0xFFFFu)) { overflow = true; return 0; }
            uint32_t off = uint32_t(leaves.size());
            leaves.insert(leaves.end(), raw.begin(), raw.end());
            leaf_at[raw] = off;
            return off;
        };
        auto overlaps = [&](const BuiltPoly& p, int x0, int x1, int z0, int z1, bool wide = true) {
            // Inclusive and a foot wide: the game's cell is floor(x) (and x - 0.99 for negatives). Walls also reach
            // the wall query's 5 ft (func_8006FC94's 0x144($sp)) past the cell, so a car at its edge still meets them
            // (the stock files list polygons well past their cells), unless that crowds a cell past max_leaf.
            float m = wide && ((p.flags & 0xF) == 5 || (p.flags & 0xF) == 6) ? 6.0f : 1.0f;
            float rx0 = float(x0) - m, rx1 = float(x1) + m, rz0 = float(z0) - m, rz1 = float(z1) + m;
            if (p.x1 < rx0 || p.x0 > rx1 || p.z1 < rz0 || p.z0 > rz1) return false;
            if (!p.sweep.empty()) {
                for (const auto& b : p.sweep) if (!(b[1] < rx0 || b[0] > rx1 || b[3] < rz0 || b[2] > rz1)) return true;
                return false;
            }
            // The polygon itself, not just its box (separating axes of its x/z outline): a long diagonal strip's box
            // covers far more cells than the strip, and the stacked boxes of a pipe or a terraced slope would
            // otherwise crowd one cell past max_leaf where the stock files, which list polygons this way, stay under.
            const auto& o = p.xz;
            for (size_t k = 0; k < o.size(); k++) {
                const auto &a = o[k], &b = o[(k + 1) % o.size()];
                float nx = b[1] - a[1], nz = a[0] - b[0];
                if (nx * nx + nz * nz < 1e-8f) continue;
                float pmin = 1e30f, pmax = -1e30f;
                for (const auto& w : o) { float d = nx * w[0] + nz * w[1]; pmin = std::min(pmin, d); pmax = std::max(pmax, d); }
                float rmin = 1e30f, rmax = -1e30f;
                for (float cx : { rx0, rx1 }) for (float cz : { rz0, rz1 }) {
                    float d = nx * cx + nz * cz; rmin = std::min(rmin, d); rmax = std::max(rmax, d);
                }
                if (rmax < pmin - 1e-3f || rmin > pmax + 1e-3f) return false;
            }
            return true;
        };
        size_t biggest = 0;
        std::function<int(int, int, int, int, int, const std::vector<int>&)> make_node =
            [&](int parent, int x0, int x1, int z0, int z1, const std::vector<int>& list) -> int {
            int me = int(nodes.size());
            nodes.push_back(Node{ int16_t(parent), 0, { int16_t(x0), int16_t(x1), int16_t(z0), int16_t(z1) }, { 0, 0, 0, 0 } });
            int mx = tagged ? (x0 + x1) / 2 : (x0 + x1) >> 1, mz = tagged ? (z0 + z1) / 2 : (z0 + z1) >> 1;
            for (int q = 0; q < 4; q++) {
                int cx0 = (q & 1) ? mx : x0, cx1 = (q & 1) ? x1 : mx;
                int cz0 = (q & 2) ? z0 : mz, cz1 = (q & 2) ? mz : z1;
                std::vector<int> sub_list;
                for (int i : list) if (overlaps(polys[i], cx0, cx1, cz0, cz1)) sub_list.push_back(i);
                bool split = ((int)sub_list.size() > leaf_limit && cx1 - cx0 > min_cell && cz1 - cz0 > min_cell) ||
                             ((int)sub_list.size() > max_leaf && cx1 - cx0 > 4 && cz1 - cz0 > 4);
                if (split) {
                    int child = make_node(me, cx0, cx1, cz0, cz1, sub_list);
                    nodes[me].mask |= uint8_t(1 << q);
                    nodes[me].child[q] = uint16_t(child);
                }
                else {
                    if ((int)sub_list.size() > max_leaf) {
                        sub_list.clear();
                        for (int i : list) if (overlaps(polys[i], cx0, cx1, cz0, cz1, false)) sub_list.push_back(i);
                    }
                    if ((int)sub_list.size() > max_leaf) sub_list.resize(max_leaf), overflow = crowded = true;
                    biggest = std::max(biggest, sub_list.size());
                    uint32_t off = encode_leaf(sub_list);
                    nodes[me].child[q] = uint16_t(off);
                    if (off > 0xFFFF) nodes[me].mask |= uint8_t(1 << (4 + q));
                }
            }
            return me;
        };
        float bx0 = 1e30f, bx1 = -1e30f, bz0 = 1e30f, bz1 = -1e30f;
        for (const BuiltPoly& p : polys) {
            bx0 = std::min(bx0, p.x0); bx1 = std::max(bx1, p.x1); bz0 = std::min(bz0, p.z0); bz1 = std::max(bz1, p.z1);
        }
        std::vector<int> all(polys.size());
        for (size_t i = 0; i < polys.size(); i++) all[i] = int(i);
        for (int attempt = 0; attempt < 2; attempt++) {
            nodes.clear();
            leaves.assign(1, 0);                        // byte 0 pads so offset 0 means empty
            leaf_at.clear();
            overflow = false;
            biggest = 0;
            make_node(-1, int(std::floor(bx0)) - 2, int(std::ceil(bx1)) + 2, int(std::floor(bz0)) - 2, int(std::ceil(bz1)) + 2, all);
            if (!overflow || tagged || crowded) break;   // the tagged form only helps with size
            tagged = true;
        }
        if (overflow || nodes.size() > 0x7FFF) {
            report.error("The collision is too dense for the game's collision index (over 8 MB of cell lists, or over 36 "
                         "polygons in one small spot). Simplify the COL_* meshes.");
            return false;
        }

        // Each polygon's vertices are consecutive: a 3-byte run (first index, 0xC0 | how many more follow), then a
        // spline polygon's segment index (func_8006C670 reads it when flags & 0x3000).
        std::vector<uint8_t> vlist;
        std::vector<uint16_t> vlist_at;
        size_t vert_count = 0, spline = 0;
        for (const BuiltPoly& p : polys) {
            vlist_at.push_back(uint16_t(vlist.size()));
            add16(vlist, uint16_t(vert_count));
            add8(vlist, uint8_t(0xC0 | p.l.size()));
            if (p.seg >= 0) add16(vlist, uint16_t(p.seg)), spline++;
            vert_count += 1 + p.l.size();
        }
        if (vlist.size() > 0xFFFF) {
            report.error("The collision's vertex lists pass the game's 64 KB. Simplify the COL_* meshes.");
            return false;
        }
        out.clear();
        for (uint16_t v : { uint16_t(segs.size() / 0x84), uint16_t(nodes.size()), uint16_t(polys.size()), uint16_t(vert_count),
                            uint16_t(leaves.size() & 0xFFFF), uint16_t(vlist.size()) }) {
            add16(out, v);
        }
        out.insert(out.end(), segs.begin(), segs.end());
        for (size_t i = 0; i < nodes.size(); i++) {
            const Node& n = nodes[i];
            add16(out, uint16_t(n.parent));
            add8(out, i == 0 && tagged ? uint8_t(0x80 | (leaves.size() >> 16)) : 0);
            add8(out, n.mask);
            for (int16_t b : n.b) add16(out, uint16_t(b));
            for (uint16_t c : n.child) add16(out, c);
        }
        for (size_t i = 0; i < polys.size(); i++) {
            add16(out, polys[i].flags);
            add16(out, uint16_t(1 + polys[i].l.size()));   // info: vertex count
            for (int16_t m : polys[i].m) add16(out, uint16_t(m));
            add16(out, vlist_at[i]);                  // its run in the vertex list
        }
        for (const BuiltPoly& p : polys) {
            encode_vert(out, p.v0);
            for (const Vec3& l : p.l) encode_vert(out, l);
        }
        out.insert(out.end(), leaves.begin(), leaves.end());
        out.insert(out.end(), vlist.begin(), vlist.end());
        char b[192];
        snprintf(b, sizeof(b), "Collision: %zu polygons from %zu triangles, %zu index cells (largest %zu), %zu bytes.",
                 polys.size(), tris.size(), nodes.size(), biggest, out.size());
        report.info(b);
        if (spline) {
            snprintf(b, sizeof(b), "Spline road: %zu segments, %zu polygons take their height from them.", segs.size() / 0x84, spline);
            report.info(b);
        }
        if (polys_out) *polys_out = std::move(polys);
        return true;
    }

    // Height of the drivable ground under (x, z) within `above`/`below` ft of y, or NaN.
    float ground_under(const std::vector<BuiltPoly>& polys, const Vec3& p, float reach) {
        float best = NAN, best_d = 1e30f;
        for (const BuiltPoly& q : polys) {
            int type = q.flags & 0xF;
            if (type == 5 || type == 6 || type == 7) continue;
            if (p[0] < q.x0 - 0.5f || p[0] > q.x1 + 0.5f || p[2] < q.z0 - 0.5f || p[2] > q.z1 + 0.5f) continue;
            float m[9];
            for (int i = 0; i < 9; i++) m[i] = q.m[i] / 16384.0f;
            Vec3 d = sub(p, q.v0);
            float lx = m[0] * d[0] + m[1] * d[1] + m[2] * d[2], ly = m[3] * d[0] + m[4] * d[1] + m[5] * d[2];
            float lz = m[6] * d[0] + m[7] * d[1] + m[8] * d[2];
            // point in the convex polygon (0,0), l... in local x/z
            auto side = [](float x0, float z0, float x1, float z1, float px, float pz) { return (x1 - x0) * (pz - z0) - (z1 - z0) * (px - x0); };
            bool pos = true, neg = true;
            for (size_t k = 0; k <= q.l.size(); k++) {
                float ax = k ? q.l[k - 1][0] : 0.0f, az = k ? q.l[k - 1][2] : 0.0f;
                float bx = k < q.l.size() ? q.l[k][0] : 0.0f, bz = k < q.l.size() ? q.l[k][2] : 0.0f;
                float s = side(ax, az, bx, bz, lx, lz);
                pos = pos && s >= -0.5f;
                neg = neg && s <= 0.5f;
            }
            if (!pos && !neg) continue;
            // height of the plane at (x, z): move along world y until local y = 0
            float h = std::fabs(m[4]) > 1e-4f ? p[1] - ly / m[4] : NAN;
            if (std::isnan(h)) continue;
            float dd = std::fabs(h - p[1]);
            if (dd < reach && dd < best_d) { best_d = dd; best = h; }
        }
        return best;
    }

    // Whether a drivable surface lies within `reach` ft of p along its own normal, walls of type 6 included: Rush 2's
    // Pipe drives on near-vertical tube walls that a straight-down probe (ground_under) can't see.
    bool surface_near(const std::vector<BuiltPoly>& polys, const Vec3& p, float reach) {
        for (const BuiltPoly& q : polys) {
            int type = q.flags & 0xF;
            if (type == 5) continue;
            if (p[0] < q.x0 - reach || p[0] > q.x1 + reach || p[2] < q.z0 - reach || p[2] > q.z1 + reach) continue;
            float m[9];
            for (int i = 0; i < 9; i++) m[i] = q.m[i] / 16384.0f;
            Vec3 d = sub(p, q.v0);
            float lx = m[0] * d[0] + m[1] * d[1] + m[2] * d[2], ly = m[3] * d[0] + m[4] * d[1] + m[5] * d[2];
            float lz = m[6] * d[0] + m[7] * d[1] + m[8] * d[2];
            if (std::fabs(ly) > reach) continue;
            bool pos = true, neg = true;
            for (size_t k = 0; k <= q.l.size(); k++) {
                float ax = k ? q.l[k - 1][0] : 0.0f, az = k ? q.l[k - 1][2] : 0.0f;
                float bx = k < q.l.size() ? q.l[k][0] : 0.0f, bz = k < q.l.size() ? q.l[k][2] : 0.0f;
                float s = (bx - ax) * (lz - az) - (bz - az) * (lx - ax);
                pos = pos && s >= -0.5f;
                neg = neg && s <= 0.5f;
            }
            if (pos || neg) return true;
        }
        return false;
    }

    // ---- textures ------------------------------------------------------------------------------------------------
    // An RGBA16 stand-in (4 KB of TMEM at most) of a material's image tinted by its color. The game draws the full-size
    // image in its place (Texture::key, src/track_mod/track_mod.cpp publish_textures).
    rush2::track_mod::Texture make_texture(const rush2::gltf::Scene& scene, const rush2::gltf::Material* mat, float cutoff) {
        rush2::track_mod::Texture t;
        t.cutoff = cutoff;
        t.blend = mat && mat->alpha_mode == "BLEND";
        rush2::upscale::Image image;
        if (mat && mat->image >= 0) {
            t.image = mat->image;
            rush2::upscale::decode_image(scene.images[mat->image].data, image);
        }
        if (image.empty()) {
            image.width = image.height = 1;
            image.rgba = { 255, 255, 255, 255 };
        }
        float aspect = float(image.width) / float(image.height);
        t.width = aspect >= 1.5f ? 64 : 32;
        t.height = aspect <= 0.67f ? 64 : 32;
        if (image.width <= t.width && image.height <= t.height && image.width >= 4 && image.height >= 4 &&
            (image.width & (image.width - 1)) == 0 && (image.height & (image.height - 1)) == 0) {
            t.width = image.width; t.height = image.height;
        }
        rush2::upscale::Image small = (image.width == t.width && image.height == t.height) ? image : rush2::upscale::shrink(image, t.width, t.height);
        std::array<float, 4> tint = mat ? mat->color : std::array<float, 4>{ 1, 1, 1, 1 };
        t.tint = tint;
        t.texels.resize(size_t(t.width) * t.height * 2);
        for (size_t i = 0; i < size_t(t.width) * t.height; i++) {
            uint8_t q[4];
            for (int k = 0; k < 4; k++) q[k] = uint8_t(std::clamp(small.rgba[i * 4 + k] * tint[k], 0.0f, 255.0f));
            uint16_t c = uint16_t(((q[0] >> 3) << 11) | ((q[1] >> 3) << 6) | ((q[2] >> 3) << 1) | (q[3] >= cutoff * 255.0f ? 1 : 0));
            t.texels[i * 2] = uint8_t(c >> 8);
            t.texels[i * 2 + 1] = uint8_t(c);
        }
        t.key = rush2::upscale::rgba16_key(t.texels, t.width, t.height);
        return t;
    }

    // ---- moving objects ------------------------------------------------------------------------------------------
    // MOVE_<name> objects play their animation through Rush 2049's path followers (src/rush2049/track2049_movers*.cpp):
    // each becomes a 2049 path (PTHD) whose nodes are the animation's keys, a placement record of its own model, and
    // a collision group of its COL_* parts, written in the 2049 layouts the movers code reads.
    using Mat4 = std::array<float, 16>;     // column-major, as glTF
    using Mat3 = std::array<float, 9>;      // row-major [r * 3 + c]

    Mat4 mat4_mul(const Mat4& a, const Mat4& b) {
        Mat4 o{};
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) o[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
        return o;
    }
    Mat4 mat4_trs(const float t[3], const float q[4], const float s[3]) {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float n = std::sqrt(x * x + y * y + z * z + w * w);
        if (n > 1e-12f) { x /= n; y /= n; z /= n; w /= n; }
        float r[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                       2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                       2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) };   // columns
        Mat4 m{};
        for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) m[c * 4 + k] = r[c * 3 + k] * s[c];
        for (int k = 0; k < 3; k++) m[12 + k] = t[k];
        m[15] = 1.0f;
        return m;
    }
    Mat3 mat3_inverse(const Mat3& m) {
        Mat3 o{};
        float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
        if (std::fabs(det) < 1e-20f) return o;
        float k = 1.0f / det;
        o[0] = (m[4] * m[8] - m[5] * m[7]) * k; o[1] = (m[2] * m[7] - m[1] * m[8]) * k; o[2] = (m[1] * m[5] - m[2] * m[4]) * k;
        o[3] = (m[5] * m[6] - m[3] * m[8]) * k; o[4] = (m[0] * m[8] - m[2] * m[6]) * k; o[5] = (m[2] * m[3] - m[0] * m[5]) * k;
        o[6] = (m[3] * m[7] - m[4] * m[6]) * k; o[7] = (m[1] * m[6] - m[0] * m[7]) * k; o[8] = (m[0] * m[4] - m[1] * m[3]) * k;
        return o;
    }
    Vec3 mat3_apply(const Mat3& m, const Vec3& v) {
        return { m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                 m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
    }
    // An affine glTF matrix's inverse (rotation, scale and translation).
    Mat4 mat4_affine_inverse(const Mat4& m) {
        Mat3 a = { m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10] };
        Mat3 i = mat3_inverse(a);
        Vec3 t = mat3_apply(i, { m[12], m[13], m[14] });
        Mat4 o{};
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) o[c * 4 + r] = i[r * 3 + c];
        for (int r = 0; r < 3; r++) o[12 + r] = -t[r];
        o[15] = 1.0f;
        return o;
    }
    Vec3 mat4_point(const Mat4& m, const std::array<float, 3>& v) {
        return { m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12], m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13],
                 m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] };
    }

    // A glTF animation channel's value at time t (clamped to its keys).
    void sample_channel(const rush2::gltf::Channel& c, float t, float* out) {
        int width = c.path == "rotation" ? 4 : 3;
        bool cubic = c.interpolation == "CUBICSPLINE";
        size_t stride = cubic ? width * 3 : width, at = cubic ? width : 0;
        const auto& k = c.times;
        auto value = [&](size_t i, int j) { return c.values[i * stride + at + j]; };
        if (t <= k.front() || k.size() == 1) { for (int j = 0; j < width; j++) out[j] = value(0, j); return; }
        if (t >= k.back()) { for (int j = 0; j < width; j++) out[j] = value(k.size() - 1, j); return; }
        size_t i = size_t(std::upper_bound(k.begin(), k.end(), t) - k.begin()) - 1;
        float dt = k[i + 1] - k[i], u = dt > 0 ? (t - k[i]) / dt : 0.0f;
        if (c.interpolation == "STEP") { for (int j = 0; j < width; j++) out[j] = value(i, j); return; }
        if (cubic) {
            float u2 = u * u, u3 = u2 * u;
            for (int j = 0; j < width; j++) {
                float out_tan = c.values[i * stride + 2 * width + j], in_tan = c.values[(i + 1) * stride + j];
                out[j] = (2 * u3 - 3 * u2 + 1) * value(i, j) + (u3 - 2 * u2 + u) * dt * out_tan +
                         (-2 * u3 + 3 * u2) * value(i + 1, j) + (u3 - u2) * dt * in_tan;
            }
        }
        else if (width == 4) {
            float a[4], b[4], d = 0;
            for (int j = 0; j < 4; j++) { a[j] = value(i, j); b[j] = value(i + 1, j); d += a[j] * b[j]; }
            if (d < 0) { for (float& f : b) f = -f; d = -d; }
            float wa = 1 - u, wb = u;
            if (d < 0.9995f) {
                float th = std::acos(std::min(d, 1.0f)), s = std::sin(th);
                wa = std::sin((1 - u) * th) / s;
                wb = std::sin(u * th) / s;
            }
            for (int j = 0; j < 4; j++) out[j] = a[j] * wa + b[j] * wb;
        }
        else {
            for (int j = 0; j < width; j++) out[j] = value(i, j) + (value(i + 1, j) - value(i, j)) * u;
        }
        if (width == 4) {
            float n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
            if (n > 1e-12f) for (int j = 0; j < 4; j++) out[j] /= n;
        }
    }

    // The quaternion 2049's func_800BFBE8 (movers::quat_to_matrix) turns into row-major matrix rt = R^T, R a rotation
    // in the column convention. That function builds D R(q)^T D (D = diag(1, -1, 1): its elements 1, 3, 5 and 7 are
    // negated), so q is the standard quaternion of D R D.
    std::array<float, 4> quat_2049(const Mat3& r) {
        Mat3 q = r;
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if ((i == 1) != (j == 1)) q[i * 3 + j] = -q[i * 3 + j];
        float tr = q[0] + q[4] + q[8];
        std::array<float, 4> o;
        if (tr > 0) {
            float s = std::sqrt(tr + 1.0f) * 2;
            o = { (q[7] - q[5]) / s, (q[2] - q[6]) / s, (q[3] - q[1]) / s, 0.25f * s };
        }
        else if (q[0] > q[4] && q[0] > q[8]) {
            float s = std::sqrt(1.0f + q[0] - q[4] - q[8]) * 2;
            o = { 0.25f * s, (q[1] + q[3]) / s, (q[2] + q[6]) / s, (q[7] - q[5]) / s };
        }
        else if (q[4] > q[8]) {
            float s = std::sqrt(1.0f + q[4] - q[0] - q[8]) * 2;
            o = { (q[1] + q[3]) / s, 0.25f * s, (q[5] + q[7]) / s, (q[2] - q[6]) / s };
        }
        else {
            float s = std::sqrt(1.0f + q[8] - q[0] - q[4]) * 2;
            o = { (q[2] + q[6]) / s, (q[5] + q[7]) / s, 0.25f * s, (q[3] - q[1]) / s };
        }
        return o;
    }

    // ---- visual geometry -----------------------------------------------------------------------------------------
    struct VisualTri {
        Vec3 p[3];          // game space, ft
        Vec3 n[3];          // game space
        std::array<float, 2> uv[3];
        std::array<float, 4> col[3];
        bool has_col = false;
        int material = -1;  // scene material
    };

    struct Section {
        std::string name;
        Vec3 origin{};
        std::vector<int> tris;
        Vec3 lo{}, hi{};
        float radius = 0.0f;
        uint32_t list = 0;
        bool late = false;      // translucent triangles: the record is drawn in the late pass
    };

    // A material as the lists draw it.
    struct DrawMaterial {
        const rush2::track_mod::Texture* texture = nullptr;
        bool cutout = false;
        bool blend = false;     // translucent (glTF BLEND), drawn in the late pass
        bool wraps = true;      // repeats (not clamped) both ways: coordinates may be shifted by whole repeats
    };

    // A list being built: its commands, the vertices they load (offsets into `vertices` in the G_VTX commands listed
    // in vtx_commands), and the extent of what it draws (relative to its origin, ft).
    struct MeshList {
        std::vector<uint8_t> vertices;
        std::vector<std::pair<uint32_t, uint32_t>> list;
        std::vector<size_t> vtx_commands;
        Vec3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
        float radius = 0.0f;
        void cmd(uint32_t w0, uint32_t w1) { list.emplace_back(w0, w1); }
    };

    // ---- stock sky: a model copied from another container ----------------------------------------------------------
    // A model of another Rush 2 container (a stock sky: Las Vegas's SKYO1, the stunt arena's STUNTSKYO1, a Rush 2049
    // track's SKYSKY as convert_file makes it), copied into the container being built. A model list may point at
    // vertices (G_VTX), texel and palette data (SETTIMG), texture-load lists (G_DL into the header's [7]..[8] range,
    // which hold only SETTIMGs) and other lists without pointers (geometry.md section 2.5); each piece is copied where
    // that kind goes in the new container and the pointers moved with it.
    struct ModelCopy {
        const std::vector<uint8_t>* src = nullptr;
        uint32_t list = 0, list_bytes = 0;                      // the model's list (with its G_ENDDL)
        float radius = 0.0f;
        std::map<uint32_t, uint32_t> blocks;                    // texel / palette data: offset -> bytes
        std::vector<std::pair<uint32_t, uint32_t>> loads, subs; // texture-load lists and other lists: offset, bytes
        uint32_t vtx0 = 0xFFFFFFFF, vtx1 = 0;                   // the vertices' span
        std::map<uint32_t, uint32_t> moved;                     // a copied piece's offset -> its new offset
    };

    uint32_t be32_at(const std::vector<uint8_t>& d, size_t o) {
        return o + 4 <= d.size() ? (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3] : 0;
    }

    // Bytes of the list at o up to and including its G_ENDDL, or 0 if it runs off the data.
    uint32_t list_size(const std::vector<uint8_t>& d, uint32_t o) {
        for (uint32_t at = o; at + 8 <= d.size() && at - o < 0x40000; at += 8) {
            if ((be32_at(d, at) >> 24) == 0xDF) return at + 8 - o;
        }
        return 0;
    }

    // The data each SETTIMG of the list [o, o + bytes) loads, from the load command after it.
    bool texel_blocks(const std::vector<uint8_t>& d, uint32_t o, uint32_t bytes, ModelCopy& c, std::string& why) {
        for (uint32_t at = o; at < o + bytes; at += 8) {
            uint32_t w0 = be32_at(d, at), w1 = be32_at(d, at + 4);
            uint8_t op = uint8_t(w0 >> 24);
            if (op != 0xFD && op != 0xFE && op != 0xFF) continue;
            if (op != 0xFD) { why = "it sets a color or depth image"; return false; }
            uint32_t siz = (w0 >> 19) & 3, width = (w0 & 0xFFF) + 1, size = 0;
            for (uint32_t n = at + 8; n < o + bytes && !size; n += 8) {
                uint32_t v0 = be32_at(d, n), v1 = be32_at(d, n + 4);
                uint8_t k = uint8_t(v0 >> 24);
                if (k == 0xFD || k == 0xDF) break;
                if (k == 0xF3) size = ((((v1 >> 12) & 0xFFF) + 1) << siz) >> 1;                  // LOADBLOCK
                else if (k == 0xF0) size = (((v1 >> 14) & 0x3FF) + 1) * 2;                     // LOADTLUT
                else if (k == 0xF4) size = ((((v1 & 0xFFF) >> 2) + 1) * width << siz) >> 1;   // LOADTILE
            }
            uint32_t addr = w1 & 0xFFFFFF;
            if (!size || addr + size > d.size()) { why = "a texture load couldn't be read"; return false; }
            c.blocks[addr] = std::max(c.blocks[addr], size);
        }
        return true;
    }

    bool plan_copy(const std::vector<uint8_t>& d, const std::string& name, ModelCopy& c, std::string& why) {
        c = ModelCopy{};
        c.src = &d;
        uint32_t models = be32_at(d, 0), names = be32_at(d, 4), count = be32_at(d, 16);
        uint32_t txld0 = be32_at(d, 28), txld1 = be32_at(d, 32);
        if (d.size() < 0x28 || names + count * 0x18 > d.size() || models + count * 0x34 > d.size()) {
            why = "it isn't a model container";
            return false;
        }
        int index = -1;
        for (uint32_t i = 0; i < count && index < 0; i++) {
            if (strncmp(reinterpret_cast<const char*>(&d[names + i * 0x18]), name.c_str(), 15) == 0) index = int(i);
        }
        if (index < 0) { why = "it has no model " + name; return false; }
        uint32_t r = be32_at(d, names + index * 0x18 + 0x10);
        memcpy(&c.radius, &r, 4);
        c.list = be32_at(d, models + index * 0x34 + 4 + 8);
        c.list_bytes = list_size(d, c.list);
        if (!c.list_bytes) { why = "its list couldn't be read"; return false; }
        for (uint32_t at = c.list; at < c.list + c.list_bytes; at += 8) {
            uint32_t w0 = be32_at(d, at), w1 = be32_at(d, at + 4);
            uint8_t op = uint8_t(w0 >> 24);
            uint32_t addr = w1 & 0xFFFFFF;
            if (op == 0x01) {
                uint32_t n = (w0 >> 12) & 0xFF;
                if (addr + n * 16 > d.size()) { why = "its vertices couldn't be read"; return false; }
                c.vtx0 = std::min(c.vtx0, addr);
                c.vtx1 = std::max(c.vtx1, addr + n * 16);
            }
            else if (op == 0xDE) {
                if (((w0 >> 16) & 0xFF) == 1) { why = "it branches"; return false; }
                uint32_t bytes = list_size(d, addr);
                if (!bytes) { why = "a list it calls couldn't be read"; return false; }
                bool load = addr >= txld0 && addr < txld1;
                auto& to = load ? c.loads : c.subs;
                if (std::none_of(to.begin(), to.end(), [&](const auto& p) { return p.first == addr; })) to.push_back({ addr, bytes });
                for (uint32_t s = addr; s < addr + bytes && !load; s += 8) {
                    uint8_t k = uint8_t(be32_at(d, s) >> 24);
                    if (k == 0x01 || k == 0xDE || k == 0xDA || k == 0xDC || k == 0xFD || k == 0xFE || k == 0xFF || k == 0xE1) {
                        why = "a list it calls holds pointers";
                        return false;
                    }
                }
                if (load && !texel_blocks(d, addr, bytes, c, why)) return false;
            }
            else if (op == 0xDA || op == 0xDC || op == 0xDD || op == 0xE1) {
                why = "it uses a matrix, a move or a branch";
                return false;
            }
            else if (op == 0xE0 && ((w0 >> 16) & 0xFF) == 1 && (addr < c.list || addr > c.list + c.list_bytes)) {
                why = "a condition jumps out of its list";
                return false;
            }
        }
        return texel_blocks(d, c.list, c.list_bytes, c, why);
    }

    // The copied piece holding src offset addr (exactly a piece's start).
    uint32_t moved_to(const ModelCopy& c, uint32_t w1) {
        auto it = c.moved.find(w1 & 0xFFFFFF);
        return it == c.moved.end() ? w1 : (w1 & 0x0F000000) | it->second;
    }

    // A list copied with its pointers moved (load lists: their SETTIMGs).
    void copy_list(ModelCopy& c, uint32_t at, uint32_t bytes, std::vector<uint8_t>& g, uint32_t vtx_base, uint32_t list_base) {
        const std::vector<uint8_t>& d = *c.src;
        for (uint32_t o = at; o < at + bytes; o += 8) {
            uint32_t w0 = be32_at(d, o), w1 = be32_at(d, o + 4);
            uint8_t op = uint8_t(w0 >> 24);
            if (op == 0x01) w1 = (w1 & 0x0F000000) | (vtx_base + ((w1 & 0xFFFFFF) - c.vtx0));
            else if (op == 0xDE || op == 0xFD) w1 = moved_to(c, w1);
            else if (op == 0xE0 && ((w0 >> 16) & 0xFF) == 1) w1 = (w1 & 0x0F000000) | (list_base + ((w1 & 0xFFFFFF) - at));
            add32(g, w0);
            add32(g, w1);
        }
    }

    // Appends triangles `tris` of vtris, grouped by scene material in `order` (drawn as materials[i]), as batches of
    // G_VTX (at most 32 vertices) and G_TRI2. Vertices are (p - origin) * unit; load(m, i) emits material i's texture
    // load. Meshes without vertex colors get the shading Rush 2 bakes into its tracks: a fixed light from above.
    void emit_mesh(const std::vector<VisualTri>& vtris, const std::vector<int>& tris, const std::vector<int>& order,
                   const std::vector<DrawMaterial>& materials, const Vec3& origin, float unit,
                   const std::function<void(MeshList&, size_t)>& load, MeshList& m,
                   uint32_t opaque = mode_opaque, uint32_t cutout = mode_cutout, uint32_t blend = mode_blend) {
        const float light[3] = { 0.30f, 0.80f, 0.52f };
        int current_mode = -1;
        // Translucent materials last, over what is behind them.
        std::vector<size_t> passes;
        for (int pass = 0; pass < 2; pass++) {
            for (size_t mi = 0; mi < order.size(); mi++) if (materials[mi].blend == (pass == 1)) passes.push_back(mi);
        }
        for (size_t mi : passes) {
            std::vector<int> mine;
            for (int t : tris) if (vtris[t].material == order[mi]) mine.push_back(t);
            if (mine.empty()) continue;
            const DrawMaterial& dm = materials[mi];
            const rush2::track_mod::Texture& tex = *dm.texture;
            int mode = dm.blend ? 2 : dm.cutout ? 1 : 0;
            if (mode != current_mode) { m.cmd(0xE200001C, mode == 2 ? blend : mode ? cutout : opaque); current_mode = mode; }
            load(m, mi);
            // Each triangle's texture coordinates are shifted by whole repeats to stay inside the s16 range.
            std::vector<uint8_t> batch;
            std::vector<std::array<int, 3>> batch_tris;
            int slots = 0;
            auto flush = [&]() {
                if (slots == 0) return;
                uint32_t at = uint32_t(m.vertices.size());
                m.vertices.insert(m.vertices.end(), batch.begin(), batch.end());
                m.vtx_commands.push_back(m.list.size());
                m.cmd(0x01000000 | (uint32_t(slots) << 12) | (uint32_t(slots) << 1), at);
                for (size_t t = 0; t < batch_tris.size(); t += 2) {
                    uint32_t a = (batch_tris[t][0] * 2 << 16) | (batch_tris[t][1] * 2 << 8) | (batch_tris[t][2] * 2);
                    if (t + 1 < batch_tris.size()) {
                        uint32_t b = (batch_tris[t + 1][0] * 2 << 16) | (batch_tris[t + 1][1] * 2 << 8) | (batch_tris[t + 1][2] * 2);
                        m.cmd(0x06000000 | a, b);
                    }
                    else m.cmd(0x05000000 | a, 0);
                }
                batch.clear();
                batch_tris.clear();
                slots = 0;
            };
            for (int ti : mine) {
                const VisualTri& t = vtris[ti];
                if (slots + 3 > 32) flush();
                float su = 0, sv = 0;
                if (dm.wraps) {
                    su = std::floor(std::min({ t.uv[0][0], t.uv[1][0], t.uv[2][0] }));
                    sv = std::floor(std::min({ t.uv[0][1], t.uv[1][1], t.uv[2][1] }));
                }
                std::array<int, 3> tri;
                for (int k = 0; k < 3; k++) {
                    Vec3 rel = sub(t.p[k], origin);
                    for (int c = 0; c < 3; c++) { m.lo[c] = std::min(m.lo[c], rel[c]); m.hi[c] = std::max(m.hi[c], rel[c]); }
                    m.radius = std::max(m.radius, length(rel));
                    int16_t xyz[3];
                    for (int c = 0; c < 3; c++) xyz[c] = int16_t(std::clamp(std::lround(rel[c] * unit), -32767l, 32767l));
                    uint8_t rgba[4];
                    if (t.has_col) {
                        for (int c = 0; c < 4; c++) rgba[c] = uint8_t(std::clamp(t.col[k][c] * 255.0f + 0.5f, 0.0f, 255.0f));
                    }
                    else {
                        const Vec3& nn = t.n[k];
                        float shade = 0.72f + 0.28f * std::max(0.0f, nn[0] * light[0] + nn[1] * light[1] + nn[2] * light[2]);
                        rgba[0] = rgba[1] = rgba[2] = uint8_t(std::clamp(shade * 255.0f, 0.0f, 255.0f));
                        rgba[3] = 255;
                    }
                    // N64 texel centers are at whole coordinates (src/car_mods.cpp emit_body).
                    int32_t st[2] = { int32_t(std::lround(((t.uv[k][0] - su) * tex.width - 0.5f) * 32.0f)),
                                      int32_t(std::lround(((t.uv[k][1] - sv) * tex.height - 0.5f) * 32.0f)) };
                    for (int c = 0; c < 3; c++) add16(batch, uint16_t(xyz[c]));
                    add16(batch, 0);
                    for (int c = 0; c < 2; c++) add16(batch, uint16_t(int16_t(std::clamp(st[c], -32767, 32767))));
                    batch.insert(batch.end(), rgba, rgba + 4);
                    tri[k] = slots++;
                }
                batch_tris.push_back(tri);
            }
            flush();
        }
    }

    // The commands loading RGBA16 stand-in texture t from `texels` (the pointer is the first command's second word),
    // with the material's wrap modes.
    std::vector<std::pair<uint32_t, uint32_t>> texture_load(const rush2::track_mod::Texture& t,
                                                           const rush2::gltf::Material* mat, uint32_t texels) {
        auto mode = [](int wrap) { return wrap == 33648 ? 1u : wrap == 33071 ? 2u : 0u; }; // mirror / clamp
        auto mask = [](uint32_t n) { return n == 64 ? 6u : n == 32 ? 5u : n == 16 ? 4u : n == 8 ? 3u : 2u; };
        uint32_t cms = mat ? mode(mat->wrap_s) : 0, cmt = mat ? mode(mat->wrap_t) : 0;
        uint32_t tile = (cmt << 18) | (mask(t.height) << 14) | (cms << 8) | (mask(t.width) << 4);
        uint32_t line = t.width * 2 / 8;
        return { { 0xFD100000u, texels }, { 0xF5100000u, 0x07000000u | tile }, { 0xE6000000u, 0u },
                 { 0xF3000000u, 0x07000000u | ((t.width * t.height - 1) << 12) | (2048 / line) }, { 0xE7000000u, 0u },
                 { 0xF5100000u | (line << 9), tile }, { 0xF2000000u, (((t.width - 1) * 4) << 12) | ((t.height - 1) * 4) } };
    }

    // ---- AI path ---------------------------------------------------------------------------------------------------
    // air: over a gap (a jump), kept at the curve's height with its lanes, not dropped onto a road below.
    // speed / mode: the authored AI target speed (mph) and steering behaviour (AIPATH _SPEED / _AI_MODE), NaN if none.
    struct PathPoint { Vec3 p; Vec3 lateral; float half_width; bool air = false; float speed = std::nanf(""), mode = std::nanf(""); };

    // A node's triangles in game space.
    void append_tris(const rush2::gltf::Node& n, std::vector<VisualTri>& out) {
        for (const auto& prim : n.primitives) {
            for (size_t t = 0; t + 2 < prim.tris.size(); t += 3) {
                VisualTri v;
                for (int k = 0; k < 3; k++) {
                    uint32_t i = prim.tris[t + k];
                    v.p[k] = to_game(prim.pos[i]);
                    v.n[k] = dir_to_game(prim.nrm[i]);
                    v.uv[k] = prim.uv[i];
                    if (!prim.col.empty()) { v.col[k] = prim.col[i]; v.has_col = true; }
                }
                v.material = prim.material;
                out.push_back(v);
            }
        }
    }

    // ---- track select diorama ----------------------------------------------------------------------------------------
    // The author's diorama.glb (dioramas are authored: a miniature generated from the track doesn't read well, see
    // src/rush2049/track2049_art.cpp use_miniature), fitted the way the stock dioramas in asset 3 are built (measured:
    // VEGASTRACK..STUNTTRACK span 1000-1280 model units on their longer side, centered on x and z, from y -80 (the
    // base's bottom) up). The stock ones are shown at scales that make about 800 units of their longer side; the
    // track select shows these at 2/3 (src/rush2049/track2049_menu.cpp).
    constexpr float diorama_side = 1200.0f;
    constexpr float diorama_floor = -80.0f;
    constexpr float diorama_triangle_budget = 4000;     // stock dioramas: 750-2400 vertices

    // Builds art.diorama from `tris` (materials of `scene`); its stand-in textures are added to kept.
    bool build_diorama(std::vector<rush2::track_mod::Texture>& kept, const rush2::gltf::Scene& scene,
                       const std::vector<VisualTri>& tris, const std::string& what,
                       rush2::track_mod::MenuArt& art, Report& report) {
        if (tris.empty()) {
            report.warning(what + " has no triangles; the track select shows no diorama.");
            return false;
        }
        Vec3 lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f };
        for (const VisualTri& t : tris) for (const Vec3& p : t.p) for (int c = 0; c < 3; c++) {
            lo[c] = std::min(lo[c], p[c]);
            hi[c] = std::max(hi[c], p[c]);
        }
        float side = std::max({ hi[0] - lo[0], hi[2] - lo[2], 1.0f });
        float unit = diorama_side / side;                   // model units per ft
        Vec3 origin = { (lo[0] + hi[0]) * 0.5f, lo[1] - diorama_floor / unit, (lo[2] + hi[2]) * 0.5f };
        if ((hi[1] - origin[1]) * unit > 32000.0f) {
            report.warning(what + " is too tall for its footprint to make a diorama.");
            return false;
        }

        if (tris.size() > diorama_triangle_budget) {
            report.warning(what + " makes a diorama of " + std::to_string(tris.size()) + " triangles (the stock ones have "
                           "at most about 2400); the track select may slow down.");
        }

        // Textures: one stand-in per material.
        std::vector<int> order;
        for (const VisualTri& t : tris) if (std::find(order.begin(), order.end(), t.material) == order.end()) order.push_back(t.material);
        std::vector<rush2::track_mod::Texture> textures;
        std::vector<const rush2::gltf::Material*> mats;
        textures.reserve(order.size());
        for (int m : order) {
            const rush2::gltf::Material* mat = m >= 0 && m < (int)scene.materials.size() ? &scene.materials[m] : nullptr;
            mats.push_back(mat);
            bool cutout = mat && mat->alpha_mode != "OPAQUE";
            textures.push_back(make_texture(scene, mat, cutout ? (mat->alpha_mode == "MASK" ? mat->alpha_cutoff : 0.5f) : 0.0f));
        }
        std::vector<DrawMaterial> draw;
        for (size_t i = 0; i < order.size(); i++) {
            const rush2::gltf::Material* mat = mats[i];
            draw.push_back({ &textures[i], mat && mat->alpha_mode != "OPAQUE",
                             !mat || (mat->wrap_s != 33071 && mat->wrap_t != 33071) });
        }

        // Blob: texels, vertices, list.
        std::vector<uint8_t>& d = art.diorama;
        d.clear();
        art.diorama_pointers.clear();
        std::vector<uint32_t> texel_at;
        for (rush2::track_mod::Texture& t : textures) {
            align8(d);
            texel_at.push_back(uint32_t(d.size()));
            d.insert(d.end(), t.texels.begin(), t.texels.end());
        }
        MeshList m;
        std::vector<size_t> settimg;
        m.cmd(0xE7000000, 0);
        m.cmd(0xE3001001, 0x00000000);          // no TLUT: true color
        m.cmd(0xE3000F00, 0x00000000);          // no mipmaps
        m.cmd(combine[0], combine[1]);
        m.cmd(0xD7000002, 0xFFFFFFFF);          // G_TEXTURE on, tile 0, scale 1
        std::vector<int> all(tris.size());
        for (size_t i = 0; i < tris.size(); i++) all[i] = int(i);
        // Inline loads: the loader doesn't relocate inside called lists (func_8007786C doesn't enter G_DL targets).
        emit_mesh(tris, all, order, draw, origin, unit, [&](MeshList& ml, size_t mi) {
            bool first = true;
            for (const auto& [w0, w1] : texture_load(textures[mi], mats[mi], texel_at[mi])) {
                if (first) settimg.push_back(ml.list.size());
                first = false;
                ml.cmd(w0, w1);
            }
        }, m);
        m.cmd(0xE3001001, 0x00008000);          // the TLUT mode Rush 2's lists leave set (RGBA16)
        m.cmd(0xDF000000, 0);
        align8(d);
        uint32_t base = uint32_t(d.size());
        d.insert(d.end(), m.vertices.begin(), m.vertices.end());
        align8(d);
        art.diorama_list = uint32_t(d.size());
        for (size_t i : m.vtx_commands) m.list[i].second += base;
        std::set<size_t> pointers(m.vtx_commands.begin(), m.vtx_commands.end());
        pointers.insert(settimg.begin(), settimg.end());
        for (size_t i = 0; i < m.list.size(); i++) {
            const auto& [w0, w1] = m.list[i];
            if (pointers.count(i)) {
                art.diorama_pointers.push_back(uint32_t(d.size() + 4));
            }
            add32(d, w0);
            add32(d, w1);
        }
        art.diorama_radius = m.radius * unit / 16.0f;
        char b[160];
        snprintf(b, sizeof(b), "Diorama: %zu triangles from %s, %zu textures, %zu bytes.", tris.size(), what.c_str(),
                 textures.size(), d.size());
        report.info(b);
        for (rush2::track_mod::Texture& t : textures) {
            t.source = rush2::track_mod::Texture::diorama;
            kept.push_back(std::move(t));
        }
        return true;
    }
}

std::string rush2::track_mod::Report::text() const {
    std::string s;
    for (const Message& m : messages) {
        s += m.level == Message::error ? "ERROR: " : m.level == Message::warning ? "Warning: " : "";
        s += m.text;
        s += "\n";
    }
    return s;
}

bool rush2::track_mod::parse_settings(const std::string& text, Settings& s, Report& report) {
    toml::table t;
    try {
        t = toml::parse(text);
    }
    catch (const toml::parse_error& e) {
        std::ostringstream o;
        o << "track.toml line " << e.source().begin.line << ": " << e.description();
        report.error(o.str());
        return false;
    }
    auto str = [&](const char* sect, const char* key, std::string& dst) {
        if (auto v = t[sect][key].value<std::string>()) dst = *v;
    };
    str("track", "name", s.name);
    str("track", "author", s.author);
    str("track", "version", s.version);
    str("track", "description", s.description);
    if (s.name.empty()) report.error("track.toml: [track] name is required.");
    str("track", "type", s.type);
    str("track", "gravity", s.gravity);
    if (auto v = t["track"]["smooth_collision"].value<bool>()) s.smooth_collision = *v;
    if (auto v = t["track"]["smooth_fold"].value<double>()) s.smooth_fold = float(std::clamp(*v, 0.0, 90.0));
    if (auto v = t["track"]["road_hold"].value<bool>()) s.road_hold = *v;
    if (auto v = t["track"]["point_to_point"].value<bool>()) s.point_to_point = *v;
    if (auto v = t["track"]["run_in"].value<bool>()) s.run_in = *v;
    if (s.gravity != "rush2" && s.gravity != "rush2049") {
        report.error("track.toml: [track] gravity must be \"rush2\" or \"rush2049\"; \"" + s.gravity + "\" races on Rush 2's.");
        s.gravity = "rush2";
    }
    if (s.type != "race" && s.type != "stunt" && s.type != "battle") {
        report.error("track.toml: [track] type must be \"race\", \"stunt\" or \"battle\"; \"" + s.type + "\" is raced as a race.");
        s.type = "race";
    }
    // Not track settings: the players pick the lap count and mirror mode, which every track has.
    if (t["race"]["laps"]) report.info("track.toml: laps is ignored (players pick the lap count).");
    if (t["race"]["mirror"]) report.info("track.toml: mirror is ignored (every track can be raced mirrored).");
    if (auto v = t["race"]["start_time"].value<double>()) s.start_time = float(*v);
    if (auto* arr = t["race"]["checkpoint_time"].as_array()) {
        for (auto& e : *arr) {
            std::array<float, 3> times{};
            auto* inner = e.as_array();
            // [first lap, later laps]: Rush 2 keeps those two per checkpoint. A third number (the old [lap 1, lap 2,
            // lap 3+] form) is read and ignored.
            if (!inner || inner->size() < 2 || inner->size() > 3) {
                report.error("track.toml: each checkpoint_time entry needs two numbers [first lap, later laps].");
                continue;
            }
            if (inner->size() == 3 && s.checkpoint_time.empty()) {
                report.info("track.toml: checkpoint_time's third number is ignored (Rush 2 has one time for every lap "
                            "after the first).");
            }
            for (size_t i = 0; i < inner->size(); i++) times[i] = float((*inner)[i].value<double>().value_or(0.0));
            s.checkpoint_time.push_back(times);
        }
    }
    if (auto* arr = t["race"]["best_times"].as_array()) {
        for (auto& e : *arr) s.best_times.push_back(float(e.value<double>().value_or(0.0)));
    }
    if (auto v = t["look"]["fog_color"].value<std::string>()) {
        unsigned r, g, b;
        if (v->size() == 7 && (*v)[0] == '#' && sscanf(v->c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            s.fog = { uint8_t(r), uint8_t(g), uint8_t(b) };
        }
        else {
            report.error("track.toml: fog_color must look like \"#a0b8d0\".");
        }
    }
    if (t["look"]["fog_colour"]) report.warning("track.toml: the key is fog_color (\"fog_colour\" is ignored).");
    str("look", "sky", s.sky);
    if (s.sky != "none" && !stock_sky(s.sky)) {
        std::string names;
        for (const StockSky& k : stock_skies) names += std::string(names.empty() ? "" : ", ") + k.name;
        report.error("track.toml: sky = \"" + s.sky + "\" isn't a stock sky; use none or one of " + names + ".");
        s.sky = "none";
    }
    if (auto v = t["look"]["cloud_scroll"].value<double>()) s.cloud_scroll = float(*v);
    if (auto v = t["look"]["sky_parallax"].value<double>()) s.sky_parallax = float(*v);
    str("audio", "music", s.music);
    str("menu", "preview", s.preview);
    str("menu", "logo", s.logo);
    str("menu", "diorama", s.diorama);
    str("menu", "after", s.menu_after);
    if (auto* surf = t["surfaces"].as_table()) {
        int next_slot = 3;
        for (auto& [key, node] : *surf) {
            std::string name = upper(std::string(key.str()));
            Surface sf;
            auto* tbl = node.as_table();
            if (!tbl) { report.error("track.toml: [surfaces." + std::string(key.str()) + "] must be a table."); continue; }
            sf.like = (*tbl)["like"].value_or(std::string());
            if (sf.like != "road" && sf.like != "dirt" && sf.like != "grass") {
                report.error("track.toml: surfaces." + std::string(key.str()) + ".like must be \"road\", \"dirt\" or \"grass\".");
                continue;
            }
            sf.drag = float((*tbl)["drag"].value_or(0.0));
            if (name == "ROAD" || name == "DIRT" || name == "GRASS" || name == "WALL" || name == "WATER" || name == "KILL") {
                report.error("track.toml: surfaces." + std::string(key.str()) + " uses a built-in surface's name.");
                continue;
            }
            if (next_slot == 8) next_slot++;        // 8 is the game's "airborne" marker
            if (next_slot > 15) { report.error("track.toml: at most 12 custom surfaces."); break; }
            sf.material = next_slot++;
            s.surfaces[name] = sf;
        }
    }
    if (auto* movers = t["movers"].as_table()) {
        for (auto& [key, node] : *movers) {
            std::string name(key.str());
            MoverSettings m;
            auto* tbl = node.as_table();
            if (!tbl) { report.error("track.toml: [movers." + name + "] must be a table."); continue; }
            m.mode = (*tbl)["mode"].value_or(m.mode);
            m.races = (*tbl)["races"].value_or(m.races);
            if (m.mode != "loop" && m.mode != "pingpong" && m.mode != "ride") {
                report.error("track.toml: movers." + name + ".mode must be \"loop\", \"pingpong\" or \"ride\".");
                m.mode = "loop";
            }
            if (m.races != "both" && m.races != "forward" && m.races != "backward") {
                report.error("track.toml: movers." + name + ".races must be \"both\", \"forward\" or \"backward\".");
                m.races = "both";
            }
            s.movers[name] = m;
        }
    }
    return report.ok();
}

const rush2::track_mod::StockSky* rush2::track_mod::stock_sky(const std::string& name) {
    for (const StockSky& k : stock_skies) if (name == k.name) return &k;
    return nullptr;
}

bool rush2::track_mod::convert(const std::vector<uint8_t>& glb, const std::string& toml_text, const std::string& prefix,
                               Converted& out, Report& report, const Extras& extras) {
    out = Converted{};
    parse_settings(toml_text, out.settings, report);
    const Settings& settings = out.settings;

    rush2::gltf::Scene scene;
    std::string error;
    if (!rush2::gltf::read_glb(glb, scene, error)) { report.error("track.glb: " + error); return false; }

    // ---- sort the objects ----
    const rush2::gltf::Node* finish = nullptr;
    const rush2::gltf::Node* start_line = nullptr;      // START: a point-to-point race's start line
    const rush2::gltf::Node* aipath = nullptr;
    std::array<const rush2::gltf::Node*, 4> lane_nodes{};   // AIPATH_lane1..4
    std::map<int, const rush2::gltf::Node*> checkpoints, grid;
    std::vector<std::pair<float, const rush2::gltf::Node*>> aispeeds;
    std::vector<std::pair<int, const rush2::gltf::Node*>> pickup_nodes;   // PICKUP_<kind>: kind as PickupRecord::kind
    std::vector<const rush2::gltf::Node*> visual, sky, billboard_nodes;
    std::vector<std::pair<int, const rush2::gltf::Node*>> prop_nodes;    // PROP_: PropRecord::type
    const bool arena = is_arena(settings);
    // A point-to-point race ([track] point_to_point): run once from START to FINISH along an open AIPATH_main, as
    // California Speed's point-to-point courses are (checkpoint 0 the start line, flags 2; the last the finish, flags 1).
    const bool p2p = settings.point_to_point && !arena;
    if (settings.point_to_point && arena) report.info("track.toml: point_to_point is for races; an arena ignores it.");
    // A circuit with a run-in ([track] run_in), as Rush 2's New York One, Rush 2049's race 6 and San Francisco Rush's
    // Golden Gate: the race starts at START on a stretch of its own that leads into the loop, and the laps go round
    // the loop. AIPATH_main is open, from behind START along the run-in and once round the loop, ending where it first
    // met the loop, at the loop's checkpoint (flags 4): Rush 2 carries a car's progress past the spine's last point on
    // from that checkpoint's crossing (func_8008CCF8, docs/rush2049_research/race.md).
    const bool run_in = settings.run_in && !arena && !p2p;
    if (settings.run_in && settings.point_to_point) report.error("track.toml: run_in and point_to_point can't both be set; it is raced point to point.");
    if (settings.run_in && arena) report.info("track.toml: run_in is for races; an arena ignores it.");
    const bool open_path = p2p || run_in;   // AIPATH_main isn't a loop; gates START, ..., FINISH
    std::vector<std::pair<uint16_t, const rush2::gltf::Node*>> collision_nodes;
    const rush2::gltf::Node* road_splines = nullptr;    // ROAD_SPLINES: the spline road's segment table
    std::set<std::string> seen;
    // Moving objects: every node under an animated MOVE_<name> (the outermost) moves with it.
    std::vector<int> mover_of(scene.nodes.size(), -1);
    std::map<int, std::vector<const rush2::gltf::Node*>> mover_visual;     // MOVE_ node -> its visible meshes
    {
        std::set<int> animated;
        for (const auto& a : scene.animations) for (const auto& c : a.channels) animated.insert(c.node);
        for (size_t i = 0; i < scene.nodes.size(); i++) {
            for (int a = int(i); a >= 0; a = scene.nodes[a].parent) {
                if (starts(base_name(scene.nodes[a].name), "MOVE_")) mover_of[i] = a;
            }
        }
        for (size_t i = 0; i < scene.nodes.size(); i++) {
            int root = mover_of[i];
            if (root < 0) continue;
            if (root == int(i) && !animated.contains(root)) {
                report.warning(scene.nodes[i].name + " has no animation, so it doesn't move: keyframe its Location, "
                               "Rotation or Scale and export with Animation on. It is used as ordinary track.");
            }
            if (root != int(i) && starts(base_name(scene.nodes[i].name), "MOVE_")) {
                report.warning(scene.nodes[i].name + " is inside " + scene.nodes[root].name + ": it moves with it, "
                               "and its own animation is ignored.");
            }
        }
        for (size_t i = 0; i < scene.nodes.size(); i++) if (mover_of[i] >= 0 && !animated.contains(mover_of[i])) mover_of[i] = -1;
    }
    for (const rush2::gltf::Node& n : scene.nodes) {
        std::string name = base_name(n.name);
        bool mesh = !n.primitives.empty();
        int mover = mover_of[size_t(&n - scene.nodes.data())];
        if (!seen.insert(name).second && (name == "FINISH" || starts(name, "AIPATH_") || starts(name, "CHECKPOINT_"))) {
            report.error("There are two objects named " + name + ".");
            continue;
        }
        if (name.find('*') != std::string::npos) {
            report.warning("\"" + n.name + "\": a * in a name stands for any text (COL_ROAD_main), it isn't typed "
                           "literally. Read as \"" + name.substr(0, name.find('*')) + "...\".");
        }
        if (starts(name, "COL_")) {
            std::string rest = name.substr(4);
            // The longest surface name that the rest starts with (followed by its end or "_").
            struct Known { std::string name; uint16_t flags; };
            std::vector<Known> known = { { "ROAD", 0x00 }, { "DIRT", 0x11 }, { "GRASS", 0x22 }, { "WALL", 0x05 },
                                         { "WATER", 0x03 }, { "KILL", 0x07 } };
            for (const auto& [sname, sf] : settings.surfaces) {
                uint16_t type = sf.like == "road" ? 0 : sf.like == "dirt" ? 1 : 2;
                known.push_back({ sname, uint16_t(type | (sf.material << 4)) });
            }
            const Known* best = nullptr;
            std::string urest = upper(rest);
            for (const Known& k : known) {
                if (starts(urest, k.name) && (urest.size() == k.name.size() || urest[k.name.size()] == '_' || urest[k.name.size()] == '*') &&
                    (!best || k.name.size() > best->name.size())) {
                    best = &k;
                }
            }
            if (!best) {
                report.error(n.name + ": unknown surface. Use COL_ROAD, COL_DIRT, COL_GRASS, COL_WALL, COL_WATER, COL_KILL "
                             "or a surface named in track.toml's [surfaces].");
                continue;
            }
            if (!starts(rest, best->name)) report.warning(n.name + ": surface names are upper case (COL_" + best->name + "_...).");
            if (!mesh) { report.warning(n.name + " has no mesh; ignored."); continue; }
            collision_nodes.push_back({ best->flags, &n });
            continue;
        }
        if (name == "ROAD_SPLINES") { road_splines = &n; continue; }
        if (mover >= 0) {
            if (mesh) mover_visual[mover].push_back(&n);
            else if (&n != &scene.nodes[mover] && !n.name.empty()) {
                report.warning(n.name + " is inside " + scene.nodes[mover].name + ": only meshes and COL_* parts move; "
                               "ignored.");
            }
            continue;
        }
        if (name == "AIPATH_main") { aipath = &n; continue; }
        if (int k = number_after(name, "AIPATH_lane"); k >= 1 && k <= 4) { lane_nodes[size_t(k - 1)] = &n; continue; }
        if (starts(name, "AIPATH")) {
            report.warning(n.name + ": the AI path is AIPATH_main, with optional lanes AIPATH_lane1 to AIPATH_lane4; "
                           "ignored.");
            continue;
        }
        if (name == "FINISH") { finish = &n; continue; }
        if (name == "START") {
            if (open_path) start_line = &n;
            else report.info("START is a point-to-point or run-in race's start line ([track] point_to_point or run_in); ignored.");
            continue;
        }
        if (int k = number_after(name, "CHECKPOINT_"); k >= 0) { checkpoints[k] = &n; continue; }
        if (int k = number_after(name, "START_"); k >= 0) { grid[k] = &n; continue; }
        if (int k = number_after(name, "AISPEED_"); k >= 0) { aispeeds.push_back({ float(k), &n }); continue; }
        if (name == "SKY") { if (mesh) sky.push_back(&n); else report.warning(n.name + " has no mesh; ignored."); continue; }
        if (starts(name, "PROP_")) {
            std::string kind = upper(name.substr(5));
            int type = starts(kind, "KNOCK") ? rush2::track2049::prop_knock : starts(kind, "TIP") ? rush2::track2049::prop_tip : 0;
            if (!type) report.warning(n.name + ": unknown prop; use PROP_KNOCK_<name> or PROP_TIP_<name>. Ignored.");
            else if (!mesh) report.warning(n.name + " has no mesh; ignored.");
            else prop_nodes.push_back({ type, &n });
            continue;
        }
        if (starts(name, "PICKUP_")) {
            // A battle arena's weapon or power-up pickup, by Rush 2049's weapons (PickupRecord::kind).
            static const std::pair<const char*, int> kinds[] = { { "CANNON", 0 }, { "GATLING", 1 }, { "GRENADE", 2 },
                { "MINE", 3 }, { "MISSILE", 4 }, { "RAM", 5 }, { "ROCKET", 6 }, { "SONIC", 7 }, { "POWERUP", 11 } };
            std::string kind = upper(name.substr(7));
            int found = -1;
            for (const auto& [kn, kv] : kinds) if (kind == kn) found = kv;
            if (found < 0) {
                report.warning(n.name + ": unknown pickup; use PICKUP_CANNON, PICKUP_GATLING, PICKUP_GRENADE, PICKUP_MINE, "
                               "PICKUP_MISSILE, PICKUP_RAM, PICKUP_ROCKET, PICKUP_SONIC or PICKUP_POWERUP. Ignored.");
            }
            else if (settings.type != "battle") {
                report.info(n.name + ": pickups are only placed in a battle arena (track.toml type = \"battle\"); ignored.");
            }
            else {
                pickup_nodes.push_back({ found, &n });
            }
            continue;
        }
        if (starts(name, "BILLBOARD_")) {
            if (mesh) billboard_nodes.push_back(&n);
            else report.warning(n.name + " has no mesh; ignored.");
            continue;
        }
        if (mesh) { visual.push_back(&n); continue; }
        if (n.loose_edges) { report.warning(n.name + " is only lines (loose edges); ignored."); continue; }
        if (!n.name.empty()) report.info(n.name + " isn't a recognized marker; ignored.");
    }
    // A [surfaces] entry no COL_<NAME>_* object uses changes nothing: say so, or its drag silently never applies.
    for (const auto& [sname, sf] : settings.surfaces) {
        bool used = std::any_of(collision_nodes.begin(), collision_nodes.end(), [&](const auto& c) { return (c.first >> 4) == sf.material; });
        if (!used) {
            report.warning("track.toml: the surface " + sname + " isn't used: no object is named COL_" + sname + "_*, so "
                           "its drag applies nowhere (COL_DIRT and COL_GRASS keep the stock drag).");
        }
    }
    if (!grid.empty() && !arena) {
        report.info("START_1-" + std::to_string(grid.rbegin()->first) + " are ignored: Rush 2 lines the 8 cars up "
                    "itself, two per row, rows 20 ft (6 m) apart, from the finish line back.");
    }

    // ---- markers ----
    auto forward = [](const rush2::gltf::Node* n) {   // the green (Blender Y = glTF -Z) arrow, flat
        Vec3 f = dir_to_game(mul(n->axis(2), -1.0f));
        f[1] = 0.0f;
        return normalize(f);
    };
    auto half_width = [](const rush2::gltf::Node* n) { return length(n->axis(0)) * feet_per_meter; };
    if (arena) {
        // An arena has no lap: its starts are START_n (FINISH stands in for START_1 without them).
        if (!checkpoints.empty() || (finish && !grid.empty())) {
            report.info("A " + settings.type + " arena has no lap: " + std::string(checkpoints.empty() ? "FINISH is" :
                        "FINISH and the CHECKPOINT_n are") + " ignored (the cars start at START_1, START_2, ...).");
        }
        checkpoints.clear();
        if (!grid.empty()) finish = nullptr;
    }
    else {
        if (!finish) report.error(p2p ? "No FINISH marker: add an Empty named FINISH on the finish line."
                                      : "No FINISH marker: add an Empty named FINISH on the start/finish line.");
        if (checkpoints.empty() && !open_path) report.error("No checkpoints: add Empties CHECKPOINT_1, CHECKPOINT_2, ... around the lap.");
    }
    int expected = 1;
    for (const auto& [k, n] : checkpoints) {
        if (k != expected) report.error("Checkpoints must be numbered 1, 2, 3, ... without gaps; CHECKPOINT_" +
                                        std::to_string(expected) + " is missing.");
        expected = k + 1;
    }
    // A point-to-point race's start line takes one of the 10 gates.
    const int max_cps = open_path ? max_checkpoints - 1 : max_checkpoints;
    if ((int)checkpoints.size() > max_cps) {
        report.error("At most " + std::to_string(max_cps) + " checkpoints (the game has 10 gates including the finish" +
                     std::string(open_path ? " and the start" : "") + ").");
    }
    // at: the forward spine point where the AI path crosses it (the first past it), -1 for none.
    // A circuit's gates are FINISH and then CHECKPOINT_1, 2, ...; a point-to-point race's START, the checkpoints and
    // FINISH last (START stands in at the path's start when there is none: see the AI path below).
    // loop: a run-in circuit's loop start (flags 4), where the path's end comes back onto it.
    struct Gate { std::string name; Vec3 pos, dir; float radius; int at = -1; bool loop = false; };
    std::vector<Gate> gates;
    if (finish && !open_path) gates.push_back({ "FINISH", to_game(finish->position()), forward(finish), half_width(finish) });
    if (start_line) gates.push_back({ "START", to_game(start_line->position()), forward(start_line), half_width(start_line) });
    if (arena) {
        // Start spots: Rush 2049's battle starts player p at checkpoint [0, 2, 1, 3][p] (rush2_track49_battle_start), the
        // stunt mode at the path's first point, which is START_1's.
        constexpr size_t max_starts = 4;
        for (const auto& [k, n] : grid) {
            if (gates.size() >= max_starts) {
                report.info("START_" + std::to_string(k) + " and up are ignored: an arena has 4 starts (the other cars "
                            "start on the route, as far from the others as it goes).");
                break;
            }
            gates.push_back({ "START_" + std::to_string(k), to_game(n->position()), forward(n), 20.0f });
        }
    }
    int cps_added = 0;
    for (const auto& [k, n] : checkpoints) {
        if (cps_added++ >= max_cps) break;   // the extra ones were reported above
        gates.push_back({ "CHECKPOINT_" + std::to_string(k), to_game(n->position()), forward(n), half_width(n) });
    }
    if (finish && open_path) gates.push_back({ "FINISH", to_game(finish->position()), forward(finish), half_width(finish) });
    for (size_t i = 0; i < gates.size(); i++) {
        const Gate& g = gates[i];
        if (length(g.dir) < 0.5f) report.error(g.name + " points straight up or down; turn it so its green arrow points down the track.");
        if (g.radius < 3.0f && !arena) report.error(g.name + " is " + fmt_ft(g.radius * 2) + " wide; scale its X to half the road's width.");
        for (size_t j = 0; j < i; j++) {
            if (length(sub(gates[j].pos, g.pos)) < 1.0f) {   // 3D: markers on stacked roads share x/z
                report.error(g.name + " and " + gates[j].name + " are in the same place " + fmt_pos(g.pos) +
                             (dist_xz(g.pos, { 0, 0, 0 }) < 1.0f ? " (the origin: was it moved into place?)" : "") + ".");
            }
        }
    }
    if (!settings.checkpoint_time.empty() && settings.checkpoint_time.size() != checkpoints.size()) {
        report.error("track.toml: checkpoint_time has " + std::to_string(settings.checkpoint_time.size()) +
                     " entries but there are " + std::to_string(checkpoints.size()) + " checkpoints (one entry each).");
    }

    // ---- moving objects ----
    // Each animated MOVE_<name> plays its own animation (the first one that animates it): its keys become the nodes
    // of a 2049 path, sampled from every channel at the union of their key times. Poses are in game space: a point p
    // of the object (its own frame, game axes, ft) is at pos + lin p, and lin = rotation x scale per column.
    struct Mover {
        int node = -1;                          // the MOVE_ node
        std::string name;                       // after MOVE_
        rush2::track_mod::MoverSettings set;
        std::vector<float> times;
        std::vector<Vec3> pos;
        std::vector<Mat3> lin, rot;             // column convention, row-major
        std::vector<Vec3> scale;
        std::vector<std::array<float, 4>> quat; // for movers::quat_to_matrix (quat_2049)
        Mat4 to_local{};                        // glTF world, as exported -> the MOVE_ node's own frame
        Mat3 rest_inv{};                        // lin[0] inverted
        bool closed = false;                    // the last key is the first one's pose (a loop's end)
        int group = 0;                          // collision group, 0 = no collision
        std::string model;                      // its model and path name, empty if nothing to draw
        std::vector<VisualTri> tris;            // its own frame
        Section section;                        // its model's list
        float unit = 16.0f;                     // model vertex units per ft: under 16 for a mesh past vertex_reach,
                                                // made up by the path's scale (no collision) as the sky is drawn
    };
    std::vector<Mover> movers;
    std::map<int, int> mover_at;                // MOVE_ node -> movers
    auto from_game = [](const Vec3& g) { return std::array<float, 3>{ -g[0] / feet_per_meter, g[1] / feet_per_meter, g[2] / feet_per_meter }; };
    {
        std::set<int> roots(mover_of.begin(), mover_of.end());
        roots.erase(-1);
        for (int r : roots) {
            const rush2::gltf::Node& nd = scene.nodes[r];
            Mover m;
            m.node = r;
            m.name = base_name(nd.name).substr(5);
            if (auto it = settings.movers.find(m.name); it != settings.movers.end()) m.set = it->second;
            std::vector<const rush2::gltf::Channel*> channels;
            std::string anim;
            int anims = 0;
            bool child_anim = false;
            for (const auto& a : scene.animations) {
                bool mine = false;
                for (const auto& c : a.channels) {
                    mine = mine || c.node == r;
                    child_anim = child_anim || (c.node != r && mover_of[c.node] == r);
                }
                if (!mine) continue;
                if (anims++ == 0) {
                    anim = a.name;
                    for (const auto& c : a.channels) if (c.node == r) channels.push_back(&c);
                }
            }
            if (anims > 1) {
                report.warning(nd.name + " has " + std::to_string(anims) + " animations; only the first (" + anim +
                               ") plays. Keep one action per moving object.");
            }
            if (child_anim) {
                report.warning("Parts of " + nd.name + " have animations of their own; only " + nd.name +
                               "'s own animation moves them.");
            }
            if (nd.has_matrix) {
                report.error(nd.name + " is exported as a matrix, not location / rotation / scale; it doesn't move.");
                continue;
            }
            std::set<float> keys;
            for (const auto* c : channels) keys.insert(c->times.begin(), c->times.end());
            for (float t : keys) if (m.times.empty() || t - m.times.back() > 0.0005f) m.times.push_back(t);
            constexpr size_t max_keys = 2000;
            if (m.times.size() > max_keys) {
                report.warning(nd.name + "'s animation has " + std::to_string(m.times.size()) + " keys; it is sampled at " +
                               std::to_string(max_keys) + " evenly spaced times.");
                float t0 = m.times.front(), t1 = m.times.back();
                m.times.resize(max_keys);
                for (size_t i = 0; i < max_keys; i++) m.times[i] = t0 + (t1 - t0) * float(i) / float(max_keys - 1);
            }
            Mat4 parent = nd.parent >= 0 ? scene.nodes[nd.parent].world : Mat4{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            m.to_local = mat4_affine_inverse(nd.world);
            bool mirrored = false;
            for (float t : m.times) {
                float tr[3], rq[4], sc[3];
                for (int k = 0; k < 3; k++) { tr[k] = nd.translation[k]; sc[k] = nd.scale[k]; }
                for (int k = 0; k < 4; k++) rq[k] = nd.rotation[k];
                for (const auto* c : channels) sample_channel(*c, t, c->path == "translation" ? tr : c->path == "rotation" ? rq : sc);
                Mat4 w = mat4_mul(parent, mat4_trs(tr, rq, sc));
                Mat3 l, rt;
                for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) {
                    float sign = ((a == 0) != (b == 0)) ? -1.0f : 1.0f;     // diag(-1, 1, 1) on both sides
                    l[a * 3 + b] = sign * w[b * 4 + a];
                }
                Vec3 s3;
                for (int b = 0; b < 3; b++) {
                    s3[b] = std::sqrt(l[b] * l[b] + l[3 + b] * l[3 + b] + l[6 + b] * l[6 + b]);
                    for (int a = 0; a < 3; a++) rt[a * 3 + b] = s3[b] > 1e-9f ? l[a * 3 + b] / s3[b] : (a == b ? 1.0f : 0.0f);
                }
                float det = rt[0] * (rt[4] * rt[8] - rt[5] * rt[7]) - rt[1] * (rt[3] * rt[8] - rt[5] * rt[6]) +
                            rt[2] * (rt[3] * rt[7] - rt[4] * rt[6]);
                mirrored = mirrored || det < 0;
                auto q = quat_2049(rt);
                if (!m.quat.empty()) {
                    const auto& p = m.quat.back();
                    if (p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3] < 0) for (float& f : q) f = -f;
                }
                m.pos.push_back(to_game({ w[12], w[13], w[14] }));
                m.lin.push_back(l);
                m.rot.push_back(rt);
                m.scale.push_back(s3);
                m.quat.push_back(q);
            }
            if (m.times.empty()) continue;
            if (mirrored) report.warning(nd.name + " is mirrored (a negative scale) in its animation; it is drawn turned instead.");
            m.rest_inv = mat3_inverse(m.lin[0]);
            if (m.times.size() >= 2) {
                const auto &q0 = m.quat.front(), &q1 = m.quat.back();
                float d = std::fabs(q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3]);
                m.closed = length(sub(m.pos.back(), m.pos.front())) < 0.05f && d > 0.99999f &&
                           length(sub(m.scale.back(), m.scale.front())) < 0.001f;
            }
            mover_at[r] = int(movers.size());
            movers.push_back(std::move(m));
        }
        for (const auto& [name, ms] : settings.movers) {
            bool found = std::any_of(movers.begin(), movers.end(), [&](const Mover& m) { return m.name == name; });
            if (!found) report.warning("track.toml: [movers." + name + "] has no animated object named MOVE_" + name + ".");
        }
        // Collision groups (the 5 bits of 2049's polygon info), for movers with COL_* parts.
        int next = 1;
        for (Mover& m : movers) {
            bool has = std::any_of(collision_nodes.begin(), collision_nodes.end(),
                                   [&](const auto& c) { return mover_of[size_t(c.second - scene.nodes.data())] == m.node; });
            if (!has) {
                if (m.set.mode == "ride") {
                    report.warning("MOVE_" + m.name + " is a ride (track.toml) but has no COL_* floor for a car to start it from.");
                }
                continue;
            }
            if (next > 31) {
                report.error("At most 31 moving objects can have collision; MOVE_" + m.name + "'s COL_* parts are left out.");
                continue;
            }
            m.group = next++;
        }
    }
    // A point of mover m's object as exported (glTF world) in its own frame, game axes, and that point at a key.
    auto mover_local = [&](const Mover& m, const std::array<float, 3>& p) { return to_game(mat4_point(m.to_local, p)); };
    auto mover_pose = [&](const Mover& m, size_t key, const Vec3& local) { return add(m.pos[key], mat3_apply(m.lin[key], local)); };

    // ---- collision ----
    // Spline road (ROAD_SPLINES' "segments"): the game's SEG records (collision_seg_surface_8006BDA8), 33 numbers
    // each as the file holds them, feet and game axes. A COL_* face whose first vertex's _SEGMENT names one takes its
    // height from that segment and the next (the table wraps), not from its own plane.
    std::vector<uint8_t> seg_bytes;
    size_t seg_count = 0;
    if (road_splines) {
        const auto& j = road_splines->extras;
        bool ok = j.contains("segments") && j["segments"].is_array() && j["segments"].size() <= 0xFFFF;
        if (ok) {
            for (const auto& sg : j["segments"]) {
                if (!sg.is_array() || sg.size() != 33) { ok = false; break; }
                for (const auto& v : sg) {
                    if (!v.is_number()) { ok = false; break; }
                    float f = v.get<float>();
                    uint32_t u;
                    memcpy(&u, &f, 4);
                    add32(seg_bytes, u);
                }
                if (!ok) break;
            }
        }
        if (!ok) {
            report.error(road_splines->name + ": \"segments\" must be a list of segments of 33 numbers each; the road "
                         "is left flat.");
            seg_bytes.clear();
        }
        seg_count = seg_bytes.size() / 0x84;
    }
    size_t bad_segment = 0;
    std::vector<CollisionTri> ctris;
    auto add_collision = [&](const rush2::gltf::Node* n, uint16_t flags) {
        int root = mover_of[size_t(n - scene.nodes.data())];
        const Mover* m = root >= 0 && mover_at.contains(root) ? &movers[mover_at[root]] : nullptr;
        if (root >= 0 && (!m || m->group == 0)) return;
        for (const auto& prim : n->primitives) {
            for (size_t t = 0; t + 2 < prim.tris.size(); t += 3) {
                CollisionTri c;
                // A moving part is placed as its animation's first key has it (the pose its path starts from).
                for (int k = 0; k < 3; k++) {
                    const auto& p = prim.pos[prim.tris[t + k]];
                    c.v[k] = m ? mover_pose(*m, 0, mover_local(*m, p)) : to_game(p);
                }
                // glTF front = counter-clockwise; x negated mirrors it, so the front normal is the game cross's negation.
                c.front = mul(cross(sub(c.v[1], c.v[0]), sub(c.v[2], c.v[0])), -1.0f);
                c.flags = flags;
                c.object = n->name;
                c.group = m ? m->group : 0;
                if (auto it = prim.attrs.find("_SEGMENT"); it != prim.attrs.end() && !m) {
                    float sv = it->second[prim.tris[t]];
                    if (sv >= 0.0f) {
                        if (sv < float(seg_count)) c.seg = int(std::lround(sv));
                        else bad_segment++;
                    }
                }
                ctris.push_back(c);
            }
        }
    };
    for (const auto& [flags, n] : collision_nodes) add_collision(n, flags);
    if (collision_nodes.empty()) {
        report.warning("No COL_* meshes: the visible track is used as road collision (walls included).");
        for (const auto* n : visual) add_collision(n, 0x00);
    }
    // A moving polygon is listed in every cell it passes over: boxes of its pose between each key and the next.
    auto sweep = [&](BuiltPoly& p) {
        const Mover* m = nullptr;
        for (const Mover& mv : movers) if (mv.group == p.group) m = &mv;
        if (!m) return;
        float mq[9];
        for (int k = 0; k < 9; k++) mq[k] = p.m[k] / 16384.0f;
        std::vector<Vec3> local{ mat3_apply(m->rest_inv, sub(p.v0, m->pos[0])) };
        for (const Vec3& l : p.l) {
            Vec3 w = add(p.v0, { mq[0] * l[0] + mq[6] * l[2], mq[1] * l[0] + mq[7] * l[2], mq[2] * l[0] + mq[8] * l[2] });
            local.push_back(mat3_apply(m->rest_inv, sub(w, m->pos[0])));
        }
        auto box_at = [&](size_t key) {
            std::array<float, 4> b{ 1e30f, -1e30f, 1e30f, -1e30f };
            for (const Vec3& l : local) {
                Vec3 w = mover_pose(*m, key, l);
                b[0] = std::min(b[0], w[0] - 1); b[1] = std::max(b[1], w[0] + 1);
                b[2] = std::min(b[2], w[2] - 1); b[3] = std::max(b[3], w[2] + 1);
            }
            return b;
        };
        auto unite = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
            return std::array<float, 4>{ std::min(a[0], b[0]), std::max(a[1], b[1]), std::min(a[2], b[2]), std::max(a[3], b[3]) };
        };
        auto area = [](const std::array<float, 4>& b) { return (b[1] - b[0]) * (b[3] - b[2]); };
        size_t n = m->times.size();
        std::vector<std::array<float, 4>> boxes;
        for (size_t k = 0; k < n; k++) boxes.push_back(n == 1 ? box_at(0) : unite(box_at(k), box_at(k + 1 < n ? k + 1 : 0)));
        if (m->set.mode != "loop" || (!m->closed && n > 1)) {
            if (boxes.size() > 1) boxes.pop_back();     // no travel from the last key back to the first
        }
        std::array<float, 4> cur = boxes[0];
        for (size_t k = 1; k < boxes.size(); k++) {
            auto u = unite(cur, boxes[k]);
            if (area(u) <= 1.25f * std::max(area(cur), area(boxes[k])) + 400.0f) cur = u;
            else { p.sweep.push_back(cur); cur = boxes[k]; }
        }
        p.sweep.push_back(cur);
    };
    std::vector<BuiltPoly> cpolys;
    if (bad_segment) {
        report.warning(std::to_string(bad_segment) + " collision triangles name a _SEGMENT that ROAD_SPLINES doesn't "
                       "have; they are left flat.");
    }
    bool collision_ok = build_collision(ctris, out.track.collision, report, &cpolys, sweep, seg_bytes);
    // The moving objects' polygon info (2049's layout: group << 11, and 0x20 on the floors cars ride and start rides
    // from) and MOVER records (a polygon's rest matrix and origin vertex, which the movers code moves), in a 2049
    // collision file holding only those (src/rush2049/track2049_movers.cpp parse_collision).
    {
        std::vector<uint8_t> records, polys;
        size_t vert = 0, count = 0;
        for (size_t i = 0; i < cpolys.size(); i++) {
            const BuiltPoly& p = cpolys[i];
            uint16_t word = 0;
            if (p.group > 0) {
                word = uint16_t((p.group << 11) | (p.m[4] > 8192 ? 0x20 : 0));
                add16(records, uint16_t(p.group));
                add16(records, uint16_t(i));
                for (int16_t v : p.m) add16(records, uint16_t(v));
                add16(records, uint16_t(vert));
                encode_vert(records, p.v0);
                count++;
            }
            add16(polys, 0);
            add16(polys, word);
            polys.resize(polys.size() + 0x18 - 4, 0);
            vert += 1 + p.l.size();
        }
        // Written whenever there are moving objects, with or without collision: the movers code takes an empty file
        // for a track without any and never starts (no lighthouse beam, light or flipbook of a scenery-only set moved).
        if (count || !movers.empty()) {
            std::vector<uint8_t>& c = out.track.mover_collision;
            for (uint16_t v : { uint16_t(0), uint16_t(0), uint16_t(cpolys.size()), uint16_t(0), uint16_t(count) }) add16(c, v);
            c.resize(0x10, 0);
            c.insert(c.end(), polys.begin(), polys.end());
            c.insert(c.end(), records.begin(), records.end());
        }
        // Rush 2049's weapons stop on the track's solid triangles (src/rush2049/battle.cpp; 9 floats a triangle), the fixed
        // track's: a moving part is somewhere else by then.
        for (const CollisionTri& c : ctris) {
            if (c.group != 0) continue;
            for (const Vec3& v : c.v) for (float f : v) out.track.solid_triangles.push_back(f);
        }
        // The AI path follows the fixed track only.
        cpolys.erase(std::remove_if(cpolys.begin(), cpolys.end(), [](const BuiltPoly& p) { return p.group > 0; }), cpolys.end());
    }

    // ---- AI path ----
    std::vector<PathPoint> rungs;
    if (!aipath) {
        if (!arena) {
            report.error("No AIPATH_main: draw the racing line as a curve named AIPATH_main with Geometry > Extrude set to "
                         "about half the road's width (a curve with no Extrude isn't exported).");
        }
    }
    else if (aipath->primitives.empty()) {
        report.error("AIPATH_main has no surface: give the curve Geometry > Extrude (about half the road's width) so it is "
                     "exported.");
    }
    else {
        Ribbon ribbon = read_ribbon(*aipath);
        if (!ribbon.problem.empty() || ribbon.rungs.size() < 4) {
            report.error("AIPATH_main isn't a ribbon: " + (ribbon.problem.empty() ? std::string("it is too short") : ribbon.problem) +
                         ". Use a curve with Geometry > Extrude (no Bevel Depth), not a mesh.");
        }
        else {
            // Width: the rung seen from above (its height difference is the road's banking). A ribbon standing on
            // edge (a curve drawn flat in Blender extrudes straight up until its Tilt is 90 degrees) has almost none.
            // A point or two pinched to nothing is let through: Atari's stock path has one (1 of 827).
            size_t thin = 0, thin_at = 0;
            float min_w = 1e30f, max_w = 0.0f, thin_tall = 0.0f;
            for (size_t i = 0; i < ribbon.rungs.size(); i++) {
                const auto& [a, b] = ribbon.rungs[i];
                Vec3 lat = sub(b, a);
                float tall = std::fabs(lat[1]);
                lat[1] = 0.0f;
                rungs.push_back({ mul(add(a, b), 0.5f), normalize(lat), length(lat) * 0.5f });
                rungs.back().speed = ribbon.speed[i];
                rungs.back().mode = ribbon.mode[i];
                float w = rungs.back().half_width * 2.0f;
                min_w = std::min(min_w, w);
                max_w = std::max(max_w, w);
                if (rungs.back().half_width < 1.0f && thin++ == 0) { thin_at = i; thin_tall = tall; }
            }
            auto fmt_w = [](float ft) {
                char b[64];
                snprintf(b, sizeof(b), "%.1f m (%.0f ft)", ft / feet_per_meter, ft);
                return std::string(b);
            };
            std::vector<float> gaps;
            for (size_t i = 1; i < rungs.size(); i++) gaps.push_back(length(sub(rungs[i].p, rungs[i - 1].p)));
            std::vector<float> sorted_gaps = gaps;
            std::sort(sorted_gaps.begin(), sorted_gaps.end());
            float median = sorted_gaps[sorted_gaps.size() / 2];
            float worst = sorted_gaps.back();
            if (thin * 50 > rungs.size()) {
                std::string where = std::to_string(thin) + " of its " + std::to_string(rungs.size()) + " cross sections, "
                                    "the first at " + fmt_pos(rungs[thin_at].p) + ", ";
                if (thin_tall >= 2.0f) {
                    report.error("AIPATH_main's ribbon stands on edge at " + where + "(" + fmt_w(thin_tall) + " tall, " +
                                 fmt_w(rungs[thin_at].half_width * 2) + " across): set the curve's Tilt to 90 degrees "
                                 "there (Ctrl+T) so the ribbon lies flat on the road.");
                }
                else {
                    report.error("AIPATH_main's ribbon is only " + fmt_w(rungs[thin_at].half_width * 2) + " wide at " + where +
                                 "it needs about the road's width: set Extrude to about half of it (and check the "
                                 "point's Radius, Alt+S).");
                }
                report.warning("Until then the computer cars all drive AIPATH_main's center line.");
            }
            else if (median < 0.01f || worst > median * 20.0f + 50.0f) {
                report.error("AIPATH_main's points aren't in order along the path (was it converted to a mesh and edited?). "
                             "Use the curve itself, with Extrude.");
                rungs.clear();
            }
            else {
                report.info("AIPATH_main: " + std::string(ribbon.closed ? "a closed" : "an open") + " ribbon, " +
                            std::to_string(rungs.size()) + " cross sections, " + fmt_w(min_w) +
                            (max_w - min_w > 0.5f ? " to " + fmt_w(max_w) : std::string()) + " wide.");
                float closing = length(sub(rungs.front().p, rungs.back().p));
                if (open_path && ribbon.closed) {
                    report.warning(std::string("AIPATH_main is a closed loop, but the track is ") +
                                   (p2p ? "point-to-point: it is raced once from its start to its end."
                                        : "a run-in circuit: it is read from its start round to its end.") +
                                   " In Blender: Curve > Toggle Cyclic to open it.");
                }
                if (!ribbon.closed && closing > median * 3.0f + 10.0f && !arena && !open_path) {
                    report.warning("AIPATH_main isn't a closed loop (its ends are " + fmt_ft(closing) +
                                   " apart); it is closed with a straight line. In Blender: Curve > Toggle Cyclic.");
                }
            }
        }
    }

    // ---- race direction, start and checkpoint crossings ----
    // Each gate is a line across the road: through its marker, square to its arrow, its half-width to either side. The
    // AI path crosses it where a step of the path goes from one side of that line to the other within that width and
    // within gate_height of the marker, so a stretch running alongside, a bridge over it or a road under it isn't taken
    // for it. One loop crosses each gate once. The race direction is the one most arrows agree with (FINISH breaks a
    // tie), the lap starts at FINISH's crossing, and the crossings are written into the path files, which the game
    // uses as they stand for a custom track (rush2_mod_checkpoint_crossing) in place of its own search at load (the
    // first change of side within the radius seen from above, func_80092D6C), which a compact track's other stretches
    // can fool.
    // ---- stand-ins: a track with an unusable AI path or no FINISH still loads (the errors above stay) ----
    // No FINISH: one across the path near its start; with no path either, just before CHECKPOINT_1 along its arrow
    // (markers sit on the road), or with no markers at all, on the road nearest the middle of the track. No usable AI
    // path: one straight through the markers (along each one's arrow) in number order and back to FINISH, or with
    // FINISH alone, a circle from it along its arrow, as wide as the markers. The race, the grid, respawns and the
    // computer cars follow it as best they can; the path's own warnings are left out, the error already says what's
    // wrong.
    bool stand_in = false;
    // A point-to-point race runs along AIPATH_main to FINISH: the curve is turned round when its start is the end
    // nearer FINISH. With no START, the start line stands p2p_run_up ft along the path from its start, room for the
    // grid Rush 2 lines up behind it (four rows 20 ft apart; California Speed's move_start keeps 75 ft); with no
    // FINISH, the finish line is at the path's end. A run-in circuit's curve starts at its free end: the other end is
    // the one that comes back onto the path, where the loop begins.
    constexpr float p2p_run_up = 100.0f;
    if (open_path && rungs.size() >= 4) {
        if (p2p && finish && dist_xz(rungs.front().p, to_game(finish->position())) < dist_xz(rungs.back().p, to_game(finish->position()))) {
            std::reverse(rungs.begin(), rungs.end());
            for (PathPoint& r : rungs) r.lateral = mul(r.lateral, -1.0f);
            report.info("AIPATH_main starts at FINISH's end; it was turned round to run to FINISH.");
        }
        if (run_in) {
            // How close each end comes to the rest of the path (more than a quarter of it away along the curve).
            size_t n = rungs.size(), far = n / 4;
            auto nearest = [&](const Vec3& p, size_t from, size_t to) {
                float best = 1e30f;
                for (size_t i = from; i < to; i++) best = std::min(best, length(sub(rungs[i].p, p)));
                return best;
            };
            if (nearest(rungs.front().p, far, n) < nearest(rungs.back().p, 0, n - far)) {
                std::reverse(rungs.begin(), rungs.end());
                for (PathPoint& r : rungs) r.lateral = mul(r.lateral, -1.0f);
                report.info("AIPATH_main starts at the end that joins the loop; it was turned round to start on the run-in.");
            }
        }
        auto line_at = [&](const char* name, size_t i, size_t j) {
            Gate g{ name, mul(add(rungs[i].p, rungs[j].p), 0.5f), sub(rungs[j].p, rungs[i].p), 40.0f };
            g.dir[1] = 0.0f;
            g.dir = normalize(g.dir);
            g.radius = std::max(rungs[i].half_width * 1.5f, 40.0f);
            return g;
        };
        if (!start_line) {
            size_t i = 0;
            for (float s = 0.0f; i + 2 < rungs.size() && s < p2p_run_up; i++) s += length(sub(rungs[i + 1].p, rungs[i].p));
            Gate g = line_at("START", i, i + 1);
            report.info("No START: the start line is " + fmt_ft(p2p_run_up) + " along AIPATH_main from its start, at " +
                        fmt_pos(g.pos) + ".");
            gates.insert(gates.begin(), g);
        }
        if (!finish) {
            Gate g = line_at("FINISH", rungs.size() - 2, rungs.size() - 1);
            report.warning("With no FINISH, a stand-in finish line is used at the end of AIPATH_main, " + fmt_pos(g.pos) + ".");
            gates.push_back(g);
        }
    }
    else if (open_path && !start_line && !gates.empty()) {
        Gate g = gates.front();
        g.name = "START";
        g.pos = sub(g.pos, mul(g.dir, p2p_run_up));
        report.warning("With no START and no usable AI path, a stand-in start line is used at " + fmt_pos(g.pos) + ".");
        gates.insert(gates.begin(), g);
    }
    if (!arena && !finish && !open_path) {
        Gate f{ "FINISH", {}, { 0.0f, 0.0f, 1.0f }, 40.0f };
        if (rungs.size() >= 2) {
            f.pos = mul(add(rungs[0].p, rungs[1].p), 0.5f);
            f.dir = sub(rungs[1].p, rungs[0].p);
            f.dir[1] = 0.0f;
            f.dir = normalize(f.dir);
            f.radius = std::max(rungs[0].half_width * 1.5f, 40.0f);
        }
        else if (!gates.empty()) {
            const Gate& c1 = gates.front();
            float room = gates.size() >= 2 ? dist_xz(c1.pos, gates.back().pos) * 0.25f : 100.0f;
            f.pos = sub(c1.pos, mul(c1.dir, std::min(100.0f, room)));
            f.dir = c1.dir;
            f.radius = c1.radius;
        }
        else {
            Vec3 lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f };
            for (const CollisionTri& c : ctris) for (const Vec3& v : c.v) for (int k = 0; k < 3; k++) {
                lo[k] = std::min(lo[k], v[k]);
                hi[k] = std::max(hi[k], v[k]);
            }
            Vec3 mid = mul(add(lo, hi), 0.5f);
            float best = 1e30f;
            for (int pass = 0; pass < 2 && best == 1e30f; pass++) {   // road first, then any surface
                for (const CollisionTri& c : ctris) {
                    if (pass == 0 && (c.flags & 0xF) != 0) continue;
                    Vec3 at = mul(add(add(c.v[0], c.v[1]), c.v[2]), 1.0f / 3.0f);
                    float d = dist_xz(at, mid);
                    if (d < best) { best = d; f.pos = at; }
                }
            }
        }
        if (f.dir == Vec3{ 0.0f, 0.0f, 0.0f }) f.dir = { 0.0f, 0.0f, 1.0f };
        report.warning("With no FINISH, a stand-in finish line is used at " + fmt_pos(f.pos) + ".");
        gates.insert(gates.begin(), f);
    }
    if (!arena && rungs.size() < 4) {
        stand_in = true;
        auto add_run = [&](const Vec3& a, const Vec3& b, float wa, float wb) {
            Vec3 d = sub(b, a);
            Vec3 flat = { d[0], 0.0f, d[2] };
            Vec3 lat = normalize(Vec3{ -flat[2], 0.0f, flat[0] });
            int steps = std::max(2, int(std::ceil(length(d) / 10.0f)));
            for (int k = 0; k < steps; k++) {
                float t = float(k) / float(steps);
                rungs.push_back({ add(a, mul(d, t)), lat, (wa + (wb - wa) * t) * 0.8f });
            }
        };
        if (gates.size() >= 2) {
            // Straight through each marker along its arrow (a corner at the marker might not cross its line), then on
            // to the next.
            std::vector<std::pair<Vec3, float>> corners;
            size_t ng = gates.size();
            for (size_t g = 0; g < ng; g++) {
                const Gate& m = gates[g];
                float room = std::min(dist_xz(m.pos, gates[(g + 1) % ng].pos), dist_xz(m.pos, gates[(g + ng - 1) % ng].pos));
                float d = std::max(1.0f, std::min(30.0f, room * 0.25f));
                corners.push_back({ sub(m.pos, mul(m.dir, d)), m.radius });
                corners.push_back({ add(m.pos, mul(m.dir, d)), m.radius });
            }
            for (size_t k = 0; k + (open_path ? 1 : 0) < corners.size(); k++) {   // an open path stops at its last gate
                const auto& [a, wa] = corners[k];
                const auto& [b, wb] = corners[(k + 1) % corners.size()];
                add_run(a, b, wa, wb);
            }
        }
        else {
            constexpr float r = 150.0f;   // ft
            const Gate& f = gates[0];
            Vec3 left = { -f.dir[2], 0.0f, f.dir[0] };
            Vec3 center = add(f.pos, mul(left, r));
            for (int k = 0; k < 96; k++) {
                float t = float(k) / 96.0f * 6.2831853f;
                Vec3 out = add(mul(left, -std::cos(t)), mul(f.dir, std::sin(t)));
                Vec3 tangent = add(mul(left, std::sin(t)), mul(f.dir, std::cos(t)));
                rungs.push_back({ add(center, mul(out, r)), normalize(Vec3{ -tangent[2], 0.0f, tangent[0] }), f.radius * 0.8f });
            }
        }
        report.warning("The track uses a stand-in AI path straight " +
                       std::string(gates.size() >= 2 ? "through the markers in order" : "round a circle from FINISH") +
                       ": the computer cars and respawns follow it, through walls and off the road where it goes there. "
                       "Fix AIPATH_main for a real one.");
    }

    std::vector<PathPoint> spine;   // every spine_step ft, starting at the finish line (a race's)
    if (!arena && rungs.size() >= 4) {
        constexpr float gate_height = 30.0f;   // ft above or below the marker a crossing may be
        struct Crossing { float s; bool along; Vec3 at; };   // s: distance along rungs; along: the arrow's way
        size_t nr = rungs.size();
        std::vector<float> at(nr + 1, 0.0f);   // distance along the closed loop to rung i (at[nr]: the lap)
        // A point-to-point or run-in path is open: no step from its last rung back to its first.
        const size_t steps = open_path ? nr - 1 : nr;
        std::vector<std::vector<Crossing>> found(gates.size());
        auto trace = [&]() {
            for (size_t i = 0; i < nr; i++) at[i + 1] = at[i] + length(sub(rungs[(i + 1) % nr].p, rungs[i].p));
            for (size_t g = 0; g < gates.size(); g++) {
                const Gate& gate = gates[g];
                Vec3 side_dir = { -gate.dir[2], 0.0f, gate.dir[0] };
                found[g].clear();
                for (size_t i = 0; i < steps; i++) {
                    const Vec3& a = rungs[i].p;
                    const Vec3& b = rungs[(i + 1) % nr].p;
                    float sa = dot(sub(a, gate.pos), gate.dir), sb = dot(sub(b, gate.pos), gate.dir);
                    if ((sa < 0) == (sb < 0)) continue;
                    float f = sa / (sa - sb);
                    Vec3 x = add(a, mul(sub(b, a), f));
                    if (std::fabs(dot(sub(x, gate.pos), side_dir)) > gate.radius) continue;
                    if (std::fabs(x[1] - gate.pos[1]) > gate_height) continue;
                    found[g].push_back({ at[i] + (at[i + 1] - at[i]) * f, sb > sa, x });
                }
                // A path that steps back and forth over the line (the curve's ends overlapping at the gate, a kink)
                // crosses it there an odd number of times, in turn each way: cancel each pair of neighboring
                // crossings, the lap's ends included, that go opposite ways within jitter ft of each other.
                constexpr float jitter = 50.0f;
                auto& c = found[g];
                for (bool again = true; again && c.size() >= 2;) {
                    again = false;
                    for (size_t k = 0; k < c.size() && !again; k++) {
                        size_t j = (k + 1) % c.size();
                        if (j == k || c[k].along == c[j].along || (open_path && j == 0)) continue;
                        float apart = open_path ? c[j].s - c[k].s : std::fmod(c[j].s - c[k].s + at[nr], at[nr]);
                        if (apart > jitter) continue;
                        c.erase(c.begin() + long(std::max(k, j)));
                        c.erase(c.begin() + long(std::min(k, j)));
                        again = true;
                    }
                }
            }
        };
        trace();
        // Gates the path passes once vote; a gate that reaches another stretch of road counts only when none does.
        int along = 0, against = 0;
        for (const auto& c : found) if (c.size() == 1) (c[0].along ? along : against)++;
        if (along == 0 && against == 0) for (const auto& c : found) for (const Crossing& k : c) (k.along ? along : against)++;
        if (against > along || (against == along && found[0].size() == 1 && !found[0][0].along)) {
            std::reverse(rungs.begin(), rungs.end());
            for (PathPoint& r : rungs) r.lateral = mul(r.lateral, -1.0f);
            report.info("AIPATH_main runs against the gates' arrows; it was reversed.");
            trace();
        }
        float lap = at[steps];   // a point-to-point race's: start to end
        // A run-in circuit's loop start: the gate (not START) whose first crossing is nearest where the path ends. A
        // path drawn a little past it crosses it again there: the path is cut at that second crossing, so it ends on
        // the loop start as the stock run-in paths do (New York One's spine and lanes end 7-45 ft before it).
        if (run_in) {
            constexpr float join_reach = 300.0f;   // ft from the path's end to the loop start's crossing
            const Vec3 end = rungs.back().p;
            int best_g = -1;
            float best_d = 1e30f;
            for (size_t g = 1; g < gates.size(); g++) {
                if (found[g].empty()) continue;
                float d = length(sub(found[g].front().at, end));
                if (d < best_d) { best_d = d; best_g = int(g); }
            }
            if (best_g < 0) {
                report.error("A run-in circuit needs a checkpoint where AIPATH_main's end comes back onto it: the loop "
                             "starts there.");
            }
            else {
                auto& c = found[size_t(best_g)];
                for (size_t k = 1; k < c.size(); k++) {
                    if (c[k].s > c[0].s + join_reach && length(sub(c[k].at, end)) < join_reach) { lap = c[k].s; break; }
                }
                if (best_d > join_reach && lap == at[steps]) {
                    report.error("AIPATH_main ends " + fmt_ft(best_d) + " from " + gates[size_t(best_g)].name + ", the "
                                 "nearest checkpoint: a run-in circuit's path ends where it comes back onto itself, at "
                                 "the checkpoint where the loop starts.");
                }
                gates[size_t(best_g)].loop = true;
                for (auto& cg : found) cg.erase(std::remove_if(cg.begin(), cg.end(), [&](const Crossing& k) { return k.s > lap + 1.0f; }), cg.end());
                report.info("Run-in circuit: the loop starts at " + gates[size_t(best_g)].name + "; the laps go round from there.");
            }
        }
        out.lap_feet = lap;
        // Each gate's crossing: the one going the way its arrow points. A gate wide enough to reach another stretch of
        // road (a compact track's next leg) is told apart by its arrow when that stretch runs the other way; the game
        // also checks a car against a gate's width during the race, so that's still worth a warning. Two passes the
        // same way can't be told apart.
        std::vector<float> gate_s(gates.size(), -1.0f);
        for (size_t g = 0; g < gates.size(); g++) {
            Gate& gate = gates[g];
            const auto& c = found[g];
            if (c.empty()) {
                report.error(gate.name + " at " + fmt_pos(gate.pos) + " isn't crossed by the AI path: place it on the "
                             "road across the path, within " + fmt_ft(gate_height) + " of its height, X scaled to half "
                             "the road's width.");
                continue;
            }
            size_t aligned = 0;
            const Crossing* pick = &c[0];
            for (const Crossing& k : c) {
                if (k.along && aligned++ == 0) pick = &k;
            }
            if (c.size() > 1) {
                std::string where;
                for (const Crossing& k : c) where += (where.empty() ? "" : ", ") + fmt_pos(k.at);
                if (aligned == 1) {
                    std::string others;
                    for (const Crossing& k : c) if (&k != pick) others += (others.empty() ? "" : ", ") + fmt_pos(k.at);
                    report.warning(gate.name + " is wide enough to reach another stretch of road (at " + others + "), "
                                   "running the other way; its arrow picks the stretch at " + fmt_pos(pick->at) +
                                   ". In the race a car on the other stretch may still pass it there: narrow it "
                                   "(X scale) to just this road.");
                }
                else {
                    report.error(gate.name + " is crossed by the AI path " + std::to_string(c.size()) + " times (at " +
                                 where + "): the road passes it more than once. Move it to where the road passes only "
                                 "once, or narrow it (X scale).");
                }
            }
            gate_s[g] = pick->s;
            if (!pick->along) {
                report.warning(gate.name + "'s green arrow points against the race direction the other gates agree on; "
                               "it is used turned round.");
                gate.dir = mul(gate.dir, -1.0f);
            }
        }
        // The lap starts at FINISH's crossing (else, already reported, at the rung nearest FINISH); a point-to-point
        // race's spine at the path's start, its run-up to START included (the grid lines up behind START on it).
        float start = open_path ? 0.0f : gate_s[0];
        if (open_path && gate_s[0] >= 0.0f && gate_s[0] < 60.0f) {
            report.warning("START is only " + fmt_ft(gate_s[0]) + " along AIPATH_main from its start: the grid lines up "
                           "behind START, four rows 20 ft (6 m) apart, so begin the curve about 30 m (100 ft) further back.");
        }
        if (start < 0.0f) {
            size_t near = 0;
            float best = 1e30f;
            for (size_t i = 0; i < nr; i++) {
                float d = dist_xz(rungs[i].p, gates[0].pos);
                if (d < best) { best = d; near = i; }
            }
            start = at[near];
        }
        for (float s = 0.0f; open_path ? s <= lap : s < lap - spine_step * 0.5f; s += spine_step) {
            float u = open_path ? s : std::fmod(start + s, lap);
            size_t seg = size_t(std::upper_bound(at.begin(), at.end(), u) - at.begin());
            seg = std::clamp<size_t>(seg, 1, steps) - 1;
            float span = std::max(1e-6f, at[seg + 1] - at[seg]);
            float f = std::clamp((u - at[seg]) / span, 0.0f, 1.0f);
            const PathPoint& a = rungs[seg];
            const PathPoint& b = rungs[(seg + 1) % nr];
            PathPoint p;
            p.p = add(a.p, mul(sub(b.p, a.p), f));
            p.lateral = normalize(add(mul(a.lateral, 1 - f), mul(b.lateral, f)));
            p.half_width = a.half_width + (b.half_width - a.half_width) * f;
            p.speed = std::isnan(a.speed) || std::isnan(b.speed) ? (f < 0.5f ? a.speed : b.speed)
                                                                 : a.speed + (b.speed - a.speed) * f;
            p.mode = f < 0.5f ? a.mode : b.mode;
            spine.push_back(p);
        }
        // Each gate's crossing as the first spine point past it (the finish line: point 0, on it; a point-to-point
        // race's gates all where the path meets them, in order START, CHECKPOINT_1, ..., FINISH).
        std::vector<std::pair<float, size_t>> order;
        for (size_t g = 0; g < gates.size(); g++) {
            if (gate_s[g] < 0.0f || spine.empty()) continue;
            if (open_path) {
                gates[g].at = int(std::min(size_t(gate_s[g] / spine_step) + 1, spine.size() - 1));
                order.push_back({ gate_s[g], g });
                continue;
            }
            float q = std::fmod(gate_s[g] - start + lap, lap);
            gates[g].at = g == 0 ? 0 : int((size_t(q / spine_step) + 1) % spine.size());
            if (g > 0) order.push_back({ q, g });
        }
        std::sort(order.begin(), order.end());
        if (run_in) {
            // A run-in circuit's gates go in the file as the path meets them, FINISH among them (San Francisco Rush's
            // Golden Gate has a checkpoint after its finish line): START first, the checkpoints in number order.
            bool ok = !order.empty() && order[0].second == 0;
            int last = 0;
            for (const auto& [q, g] : order) {
                int k = number_after(gates[g].name, "CHECKPOINT_");
                if (k < 0) continue;
                ok = ok && k > last;
                last = k;
            }
            if (!ok) {
                std::string seen;
                for (const auto& [q, g] : order) seen += ", " + gates[g].name;
                report.error("The gates aren't in race order: along the AI path from its start it meets them as " +
                             seen.substr(2) + ". START comes first and the checkpoints are numbered 1, 2, 3, ... in "
                             "that order (FINISH may be anywhere among them).");
            }
            std::vector<Gate> sorted;
            std::vector<bool> taken(gates.size(), false);
            for (const auto& [q, g] : order) { sorted.push_back(gates[g]); taken[g] = true; }
            for (size_t g = 0; g < gates.size(); g++) if (!taken[g]) sorted.push_back(gates[g]);
            gates = std::move(sorted);
        }
        bool in_order = true;
        size_t first = p2p ? 0 : 1;
        for (size_t k = 0; k < order.size(); k++) in_order = in_order && order[k].second == k + first;
        if (!run_in && !in_order && order.size() + first == gates.size()) {
            std::string seen;
            for (const auto& [q, g] : order) seen += ", " + gates[g].name;
            report.error(p2p ? "The gates aren't in race order: along the AI path from its start it meets them as " +
                               seen.substr(2) + ". Number the checkpoints 1, 2, 3, ... in that order, START before them "
                               "and FINISH after."
                             : "The checkpoints aren't numbered in race order: from FINISH the AI path meets them as " +
                               seen.substr(2) + ". Number them 1, 2, 3, ... in that order.");
        }
        if (spine.size() > 4000) { report.error("The AI path is too long for the game (over 80 km)."); spine.clear(); }
        // The path's height: the road under it (the game's nearest-point searches are 3D).
        // Over a jump the road nearest the curve's height can be another one below it (a stretch the jump passes
        // over): a run of points off the road whose road lies below the curve (or isn't there) and steps down by
        // more than air_step from the road before or after the run is in the air and keeps the curve's height, as
        // the stock paths do over their jumps. A curve drawn flat over a dip or a hill meets road that changes height smoothly, and is dropped
        // onto it as before.
        constexpr float off_road = 10.0f, air_step = 15.0f;
        // keep_height (AIPATH_main's custom property): the author's curve is at the road's height everywhere it is
        // on the road, so every stretch above the road is in the air, step or not (a converted path, such as the
        // Rush 2049 arcade's, whose jumps are its own arcs).
        bool keep_height = false;
        if (aipath && aipath->extras.contains("keep_height")) {
            const auto& v = aipath->extras["keep_height"];
            keep_height = v.is_boolean() ? v.get<bool>() : v.is_number() && v.get<double>() != 0.0;
        }
        size_t n = spine.size();
        std::vector<float> road(n, NAN);
        for (size_t i = 0; i < n && !cpolys.empty(); i++) road[i] = ground_under(cpolys, spine[i].p, 400.0f);
        auto off = [&](size_t i) { return std::isnan(road[i]) || std::fabs(road[i] - spine[i].p[1]) > off_road; };
        size_t first_on = 0;
        while (first_on < n && off(first_on)) first_on++;
        size_t in_air = 0;
        for (size_t k = 0; first_on < n && k < n;) {
            size_t a = (first_on + k) % n;
            if (!off(a)) { k++; continue; }
            size_t len = 0;
            while (k + len < n && off((first_on + k + len) % n)) len++;
            size_t before = (a + n - 1) % n, after = (first_on + k + len) % n;
            // Points with no road under them at all are part of the gap; the step is taken at the first and last
            // points of the run that have road under them.
            bool below = true;
            long first = -1, last = -1;
            for (size_t j = 0; j < len; j++) {
                size_t i = (a + j) % n;
                if (std::isnan(road[i])) continue;
                below = below && road[i] < spine[i].p[1];
                if (first < 0) first = long(i);
                last = long(i);
            }
            bool step = first >= 0 && (road[before] - road[size_t(first)] > air_step ||
                                       road[after] - road[size_t(last)] > air_step);
            // Or the road under it is where the lap itself passes elsewhere (over 300 ft of path away, within 30 ft
            // across and air_step of that point's height): another stretch of the race.
            for (size_t j = 0; j < len && first >= 0 && !step; j++) {
                size_t i = (a + j) % n;
                if (std::isnan(road[i])) continue;
                for (size_t o = 0; o < n && !step; o++) {
                    size_t apart = std::min((o + n - i) % n, (i + n - o) % n);
                    step = apart > 15 && dist_xz(spine[o].p, spine[i].p) < 30.0f &&
                           std::fabs(spine[o].p[1] - road[i]) < air_step;
                }
            }
            if (below && (step || keep_height)) {
                for (size_t j = 0; j < len; j++) spine[(a + j) % n].air = true;
                in_air += len;
            }
            k += len;
        }
        if (in_air) {
            report.info("AIPATH_main is over a gap for " + std::to_string(in_air) + " point(s) (a jump): kept at the "
                        "curve's height.");
        }
        size_t off_height = 0, no_road = 0;
        float worst = 0.0f;
        Vec3 worst_at{};
        for (size_t i = 0; i < n; i++) {
            PathPoint& p = spine[i];
            float h = road[i];
            if (p.air) continue;
            if (std::isnan(h)) { no_road++; continue; }
            float dy = std::fabs(h - p.p[1]);
            if (dy > off_road) {
                off_height++;
                if (dy > worst) { worst = dy; worst_at = p.p; }
            }
            p.p[1] = h;
        }
        if (off_height && !stand_in) {
            report.warning("AIPATH_main is up to " + fmt_ft(worst) + " above or below the road (near " + fmt_pos(worst_at) +
                           ", " + std::to_string(off_height) + " of " + std::to_string(spine.size()) + " points). It was "
                           "dropped onto the road; shape the curve to follow the road's height so bridges and tunnels "
                           "pick the right level.");
        }
        // Stock paths pass over road everywhere but their jumps (0-3 points a track; Pipe's tube 32 of 738), so only
        // more than 2% is worth a warning.
        if (no_road * 50 > spine.size() && !stand_in) {
            report.warning(std::to_string(no_road) + " of " + std::to_string(spine.size()) + " AI path points have no "
                           "road under them at all.");
        }
    }

    // ---- target speeds ----
    // Per point of sp, for a race along it: the backward race brakes for the same corners from the other side, and an
    // AISPEED_ marker caps the speed from it on in either direction. forward: the race the report speaks of.
    // Past a run-in circuit's last spine point the race goes on from the loop start's crossing.
    long loop_at = -1;
    for (const Gate& g : gates) if (g.loop && g.at >= 0) loop_at = g.at;
    auto target_speeds = [&](const std::vector<PathPoint>& sp, bool forward) {
        size_t n = sp.size();
        // Point i + k along the race: a circuit wraps to its start, a run-in circuit to its loop start, a
        // point-to-point path stops at its end.
        auto ahead_of = [&](size_t i, size_t k) -> size_t {
            if (!open_path) return (i + k) % n;
            size_t j = i + k;
            if (j < n) return j;
            if (run_in && loop_at >= 0 && size_t(loop_at) < n) return std::min(size_t(loop_at) + (j - n), n - 1);
            return n - 1;
        };
        std::vector<float> v(n), mph(n), radius(n);
        for (size_t i = 0; i < n; i++) {
            // A point-to-point path doesn't wrap: its ends look only along the path.
            const Vec3& a = sp[open_path ? (i < 3 ? 0 : i - 3) : (i + n - 3) % n].p;
            const Vec3& b = sp[i].p;
            const Vec3& c = sp[ahead_of(i, 3)].p;
            Vec3 ab = sub(b, a), bc = sub(c, b), ac = sub(c, a);
            ab[1] = bc[1] = ac[1] = 0.0f;
            float area2 = std::fabs(ab[0] * bc[2] - ab[2] * bc[0]);
            radius[i] = area2 > 1e-3f ? length(ab) * length(bc) * length(ac) / (2.0f * area2) : 1e9f;
        }
        // The stock lanes' speeds hardly follow the corner a point is in (a grip model rates their tightest corners at
        // 37-51 mph where they hold 72-100). They follow the tightest corner in the next 500 ft, and only loosely: this
        // table matches that radius's quantiles to the lane speeds' over every lane of Rush 2's ten circuit tracks and
        // Rush 2049's six race tracks (2049's / 1.2), so the speeds spread as the stock ones do (tools/stock_track_glb.py).
        static const float table[][2] = { { 91, 83 }, { 114, 97 }, { 154, 107 }, { 205, 118 }, { 270, 124 },
                                          { 347, 131 }, { 430, 136 }, { 538, 142 }, { 725, 149 }, { 1088, 153 },
                                          { 1800, 158 } };
        const size_t ahead = size_t(500.0f / spine_step);
        for (size_t i = 0; i < n; i++) {
            float r = 1e9f;
            for (size_t k = 0; k <= ahead; k++) r = std::min(r, radius[ahead_of(i, k)]);
            float s = table[0][1];
            if (r >= table[10][0]) s = table[10][1];
            else for (int t = 0; t < 10; t++) {
                if (r > table[t][0] && r <= table[t + 1][0]) {
                    float f = std::log(r / table[t][0]) / std::log(table[t + 1][0] / table[t][0]);
                    s = table[t][1] + (table[t + 1][1] - table[t][1]) * f;
                }
            }
            v[i] = s * 1.4667f;
        }
        // Authored speeds (AIPATH_main's _SPEED, forward races only) replace the computed ones and keep their own
        // braking; AISPEED caps still apply.
        std::vector<bool> authored(n, false);
        if (forward) {
            for (size_t i = 0; i < n; i++) {
                if (std::isnan(sp[i].speed)) continue;
                v[i] = std::clamp(sp[i].speed, 1.0f, 255.0f) * 1.4667f;
                authored[i] = true;
            }
        }
        // AISPEED caps: from the nearest point on, until the next marker (0 lifts the cap), in the forward race's order
        // in both directions, so a cap covers the same stretch of road backward (backward point i is forward point
        // n - i): in backward order "until the next marker" would cap the stretches between the zones instead.
        std::vector<std::pair<size_t, float>> caps;
        for (const auto& [cap, node] : aispeeds) {
            Vec3 p = to_game(node->position());
            size_t best_i = 0;
            float best = 1e30f;
            for (size_t i = 0; i < n; i++) { float d = dist_xz(sp[i].p, p); if (d < best) { best = d; best_i = i; } }
            if (forward && best > 200.0f) {
                report.warning(base_name(node->name) + " is " + fmt_ft(best) + " from the AI path; it applies at the nearest point.");
            }
            caps.push_back({ forward ? best_i : (n - best_i) % n, cap });
        }
        std::sort(caps.begin(), caps.end());
        for (size_t c = 0; c < caps.size(); c++) {
            if (caps[c].second <= 0.0f) continue;
            // From this marker up to the next one (all the way round when it is the only one).
            size_t from = caps[c].first, to = caps[(c + 1) % caps.size()].first;
            size_t count = caps.size() == 1 ? n : (to + n - from) % n;
            if (open_path && (c + 1 == caps.size() || caps.size() == 1)) count = n - from;   // the last one: to the end
            for (size_t k = 0; k < count; k++) {
                size_t f = (from + k) % n;
                size_t i = forward ? f : (n - f) % n;
                v[i] = std::min(v[i], caps[c].second * 1.4667f);
            }
        }
        // Brake ahead of slow points: v^2 <= v_next^2 + 2 * decel * step, twice around for the wrap.
        const float decel = 28.0f;
        for (int pass = 0; pass < 2; pass++) {
            for (size_t k = 0; k < n; k++) {
                size_t i = (n - 1 - k), j = ahead_of(i, 1);
                if (authored[i] || (p2p && i + 1 == n)) continue;
                v[i] = std::min(v[i], std::sqrt(v[j] * v[j] + 2.0f * decel * spine_step));
            }
        }
        for (size_t i = 0; i < n; i++) mph[i] = std::clamp(v[i] / 1.4667f, authored[i] ? 1.0f : 25.0f, authored[i] ? 255.0f : 200.0f);
        return mph;
    };
    std::vector<float> speed_mph;
    if (!spine.empty()) {
        speed_mph = target_speeds(spine, true);
        float lo = *std::min_element(speed_mph.begin(), speed_mph.end());
        float hi = *std::max_element(speed_mph.begin(), speed_mph.end());
        char b[160];
        size_t authored = 0;
        for (const PathPoint& q : spine) authored += !std::isnan(q.speed);
        snprintf(b, sizeof(b), "AI path: %zu points over %s, target speeds %.0f-%.0f mph%s.", spine.size(),
                 fmt_ft(out.lap_feet).c_str(), lo, hi,
                 authored == spine.size() ? " (all from _SPEED)" : authored ? " (some from _SPEED)" : "");
        report.info(b);
        // No warning for slow corners: the drone driver's floor (0x800C3FFC[gravity cheat], 61 mph at Normal) sits
        // below every stock lane point (72 mph and up), and this 1.1 g model rates the stock tracks' tightest corners
        // at 37-51 mph, so it would warn about every stock track.
    }

    // ---- lanes ----
    // The game's four racing lines run point for point beside the spine (lanes[k][i] beside spine point i), each
    // dropped onto the road under it unless the spine point is in the air. A lane is offset across AIPATH_main's
    // ribbon (lane_offsets of its half width) unless the author painted it as AIPATH_lane1..4 (paint_lane).
    static const float lane_offsets[4] = { -0.6f, -0.2f, 0.2f, 0.6f };
    using Lanes = std::array<std::vector<Vec3>, 4>;
    // Per lane slot and spine point, a painted lane's authored speed (mph) and steering behaviour (its curve's _SPEED
    // and _AI_MODE); NaN takes the spine's.
    struct LaneAI { std::array<std::vector<float>, 4> speed, mode; };
    LaneAI lane_ai;
    auto on_road = [&](Vec3 p, bool air) {
        float h = cpolys.empty() || air ? NAN : ground_under(cpolys, p, 40.0f);
        if (!std::isnan(h)) p[1] = h;
        return p;
    };
    auto offset_lanes = [&](const std::vector<PathPoint>& sp) {
        Lanes l;
        for (size_t k = 0; k < 4; k++) {
            for (const PathPoint& q : sp) l[k].push_back(on_road(add(q.p, mul(q.lateral, lane_offsets[k] * q.half_width)), q.air));
        }
        return l;
    };
    // A painted lane: for each spine point, where the lane's center line crosses the line across the road through it
    // (square to the spine, seen from above), nearest the offset lane's place there, within reach of the spine and
    // gate_height of its height, on a stretch of the lane running along the road (drawn either way round). Points it
    // doesn't pass keep the offset lane's place. The curve's ends are joined when they are close (a loop left open).
    auto paint_lane = [&](const rush2::gltf::Node& node, const std::string& name, size_t slot, std::vector<Vec3>& lane) {
        if (node.primitives.empty()) {
            report.warning(name + " has no surface: give the curve Geometry > Extrude as for AIPATH_main (any width; "
                           "its center line is the lane) so it is exported. The lane is offset from AIPATH_main.");
            return;
        }
        Ribbon ribbon = read_ribbon(node);
        if (!ribbon.problem.empty() || ribbon.rungs.size() < 2) {
            report.warning(name + " isn't a ribbon: " + (ribbon.problem.empty() ? std::string("it is too short") : ribbon.problem) +
                           ". Use a curve with Geometry > Extrude, not a mesh. The lane is offset from AIPATH_main.");
            return;
        }
        std::vector<Vec3> line;
        for (const auto& [a, b] : ribbon.rungs) line.push_back(mul(add(a, b), 0.5f));
        auto& lane_speed = lane_ai.speed[slot];
        auto& lane_mode = lane_ai.mode[slot];
        size_t segments = line.size() - 1;
        if (ribbon.closed || length(sub(line.front(), line.back())) < 60.0f) segments = line.size();
        constexpr float gate_height = 30.0f;
        size_t n = spine.size(), missed = 0;
        Vec3 first_missed{};
        for (size_t i = 0; i < n; i++) {
            const PathPoint& q = spine[i];
            Vec3 fwd = sub(spine[(i + 1) % n].p, spine[(i + n - 1) % n].p);
            fwd[1] = 0.0f;
            fwd = normalize(fwd);
            Vec3 lat = { q.lateral[0], 0.0f, q.lateral[2] };
            float want = lane_offsets[slot] * q.half_width;
            float reach = std::max(q.half_width * 2.5f, 60.0f);
            float best = 1e30f;
            Vec3 pick{};
            float pick_speed = std::nanf(""), pick_mode = std::nanf("");
            for (size_t j = 0; j < segments; j++) {
                const Vec3& a = line[j];
                const Vec3& b = line[(j + 1) % line.size()];
                float sa = dot(sub(a, q.p), fwd), sb = dot(sub(b, q.p), fwd);
                if ((sa < 0) == (sb < 0)) continue;
                Vec3 ab = sub(b, a);
                if (std::fabs(dot(normalize(Vec3{ ab[0], 0.0f, ab[2] }), fwd)) < 0.5f) continue;
                Vec3 x = add(a, mul(ab, sa / (sa - sb)));
                float off = dot(sub(x, q.p), lat);
                if (std::fabs(off) > reach || std::fabs(x[1] - q.p[1]) > gate_height) continue;
                if (std::fabs(off - want) < best) {
                    best = std::fabs(off - want);
                    pick = x;
                    float f = sa / (sa - sb);
                    size_t jb = (j + 1) % line.size();
                    float s0 = ribbon.speed[j], s1 = ribbon.speed[jb];
                    pick_speed = std::isnan(s0) || std::isnan(s1) ? (f < 0.5f ? s0 : s1) : s0 + (s1 - s0) * f;
                    pick_mode = f < 0.5f ? ribbon.mode[j] : ribbon.mode[jb];
                }
            }
            if (best == 1e30f) {
                if (!missed++) first_missed = add(q.p, mul(lat, want));
                continue;
            }
            lane[i] = on_road(pick, q.air);
            lane_speed[i] = pick_speed;
            lane_mode[i] = pick_mode;
        }
        if (missed == n) {
            report.warning(name + " doesn't run beside AIPATH_main anywhere (within " + fmt_ft(gate_height) + " of its "
                           "height and the road's width of it); the lane is offset from AIPATH_main.");
        }
        else if (missed * 50 > n) {
            report.warning(name + " doesn't run beside AIPATH_main at " + std::to_string(missed) + " of " +
                           std::to_string(n) + " points (first near " + fmt_pos(first_missed) + "); there the lane is "
                           "offset from AIPATH_main.");
        }
        else {
            report.info(name + ": painted lane, " + std::to_string(n - missed) + " of " + std::to_string(n) + " points.");
        }
    };
    Lanes lanes;
    if (!spine.empty()) {
        lanes = offset_lanes(spine);
        for (size_t k = 0; k < 4; k++) {
            lane_ai.speed[k].assign(spine.size(), std::nanf(""));
            lane_ai.mode[k].assign(spine.size(), std::nanf(""));
        }
        // AIPATH_lane1 is the leftmost in the race's direction: slot 3 (+0.6) when the lateral points left.
        float lean = 0.0f;
        size_t n = spine.size();
        for (size_t i = 0; i < n; i++) {
            Vec3 fwd = sub(spine[(i + 1) % n].p, spine[i].p);
            lean += dot(spine[i].lateral, normalize(Vec3{ -fwd[2], 0.0f, fwd[0] }));
        }
        for (size_t k = 0; k < 4; k++) {
            if (!lane_nodes[k]) continue;
            size_t slot = lean > 0.0f ? 3 - k : k;
            paint_lane(*lane_nodes[k], "AIPATH_lane" + std::to_string(k + 1), slot, lanes[slot]);
        }
    }
    else if (arena) {
        for (const auto* node : lane_nodes) {
            if (node) report.info(base_name(node->name) + ": an arena's lanes are its route; ignored.");
        }
    }

    // Lanes off the road (the modder's question: must the ribbon stay on the road?)
    if (!spine.empty() && !cpolys.empty()) {
        size_t off_road = 0;
        Vec3 first{};
        for (size_t i = 0; i < spine.size(); i++) {
            if (spine[i].air) continue;              // over a jump: nothing under it is the point
            for (const auto& lane : lanes) {
                if (!surface_near(cpolys, lane[i], 40.0f)) {
                    if (!off_road) first = lane[i];
                    off_road++;
                }
            }
        }
        // Every stock track has a few such points over its jumps: at most 0.7% of them (Crash), Pipe's tube aside
        // (tools/stock_track_glb.py), so only more than that is worth a warning.
        if (off_road * 50 > spine.size() * 4 && !stand_in) {
            report.warning(std::to_string(off_road) + " of " + std::to_string(spine.size() * 4) + " AI racing-line points "
                           "have no road under them (first near " + fmt_pos(first) + "). Keep AIPATH_main's ribbon (and "
                           "any AIPATH_lane curves) on the road; narrow its Extrude or move the curve.");
        }
    }

    // ---- path file (docs/rush2049_research/race.md section 3) ----
    // Written for each direction: Rush 2 races BACKWARD on its own path file (asset 0x63 + slot, docs/rush2049_research/
    // race.md section 5): the same road the other way round, the finish line first and the checkpoints in reverse.
    // ai: the forward race's authored lane values (painted lanes' own, else the spine's _SPEED / _AI_MODE through mph
    // and sp), or null.
    auto write_path = [&](const std::vector<PathPoint>& sp, const Lanes& lanes, const std::vector<float>& mph,
                          const std::vector<Gate>& order, std::vector<uint8_t>& d, const LaneAI* ai = nullptr) {
        add16(d, 0);                               // base time (the game uses 90)
        // An open path's flags per gate: the start line arms lap counting (2), FINISH is the lap line (1), a run-in
        // circuit's loop start is where its laps go round from (4); a point-to-point race's loop start is the start.
        auto open_flags = [&](size_t c) -> uint16_t {
            const Gate& g = order[c];
            if (c == 0) return 2;
            return uint16_t((g.name == "FINISH" || (p2p && c + 1 == order.size()) ? 1 : 0) | (g.loop ? 4 : 0));
        };
        if (open_path && !order.empty()) {
            // The header's loop, finish and arm checkpoints, as California Speed's point-to-point files and Rush 2's
            // run-in ones (New York One: 1, 5, 0) have them.
            size_t loop = 0, fin = order.size() - 1;
            for (size_t c = 1; c < order.size(); c++) {
                if ((open_flags(c) & 4) && loop == 0) loop = c;
                if (open_flags(c) & 1) fin = c;
            }
            add16(d, uint16_t(loop));
            add16(d, uint16_t(fin));
            add16(d, 0);
        }
        else {
            for (int i = 0; i < 3; i++) add16(d, 0xFFFF); // loop / finish / arm checkpoints: recomputed at load
        }
        add16(d, uint16_t(order.size()));
        add16(d, 0);
        for (size_t c = 0; c < 10; c++) {
            if (c < order.size()) {
                const Gate& g = order[c];
                for (float f : g.pos) addf(d, f);
                // Pointing back against the race, as in every stock path file (the gate test 0x800A15BC takes a
                // side change either way).
                for (float f : g.dir) addf(d, -f);
                add32(d, uint32_t(std::min(65535.0f, g.radius) * std::min(65535.0f, g.radius)));
                // Circuit: the finish is the lap line, arms lap counting and starts the loop (7). An open path:
                // open_flags.
                add16(d, open_path ? open_flags(c) : c == 0 ? 7 : 0);
                add16(d, 45);
                add16(d, 45);
            }
            else {
                for (int i = 0; i < 7; i++) add32(d, 0);
                add16(d, 0); add16(d, 0); add16(d, 0);
            }
            // Crossing indices: the spine, then the 4 lanes, no branches. The game keeps a custom track's
            // (rush2_mod_checkpoint_crossing); -1 leaves that gate to its own search. A lane runs point for point
            // beside the spine, so it crosses a gate at a slant a few points from the spine's crossing: its own is
            // the first lane point past the gate's line within 15 points of that.
            for (int i = 0; i < 20; i++) {
                int at = c < order.size() && i < 5 ? order[c].at : -1;
                if (at > 0 && i > 0) {
                    const Gate& g = order[c];
                    long n = long(sp.size());
                    auto side = [&](long k) {
                        return dot(sub(lanes[size_t(i - 1)][size_t((k % n + n) % n)], g.pos), g.dir) >= 0.0f;
                    };
                    long best = -1;
                    for (long k = at - 15; k <= at + 15; k++) {
                        if (side(k) && !side(k - 1) && (best < 0 || std::labs(k - at) < std::labs(best - at))) best = k;
                    }
                    if (best >= 0) at = int((best % n + n) % n);
                }
                add16(d, uint16_t(at));
            }
            add16(d, 0);
            addf(d, 0.0f);
        }
        add16(d, uint16_t(sp.size()));
        add16(d, 0);
        add32(d, 0);
        add8(d, 0);                                 // no branches
        add8(d, 0); add8(d, 0); add8(d, 0);
        add32(d, 0);
        add16(d, uint16_t(sp.size()));
        auto s16 = [](float f) { return uint16_t(int16_t(std::clamp(std::lround(f), -32767l, 32767l))); };
        for (const PathPoint& p : sp) for (float f : p.p) add16(d, s16(f));
        for (size_t k = 0; k < 4; k++) {
            const auto& lane = lanes[k];
            add16(d, uint16_t(sp.size()));
            add8(d, 0); add8(d, 0);
            add32(d, 0);
            for (size_t i = 0; i < sp.size(); i++) {
                for (float c : lane[i]) add16(d, s16(c));
                float speed = mph[i], mode = sp[i].mode;
                if (ai && i < ai->speed[k].size()) {
                    if (!std::isnan(ai->speed[k][i])) speed = ai->speed[k][i];
                    if (!std::isnan(ai->mode[k][i])) mode = ai->mode[k][i];
                }
                add8(d, uint8_t(std::clamp(std::lround(speed), 1l, 255l)));
                // Steering behaviour (func_80071FBC): 2 follows the line, the default (stock lanes use 2 and 5).
                add8(d, std::isnan(mode) ? 2 : uint8_t(std::clamp(std::lround(mode), 0l, 255l)));
            }
        }
    };
    // Problems the report lists don't stop the track: it is built as it stands so its author can drive it and see
    // them. Only what leaves nothing to race (no AI path through FINISH, no collision, nothing to draw, over the game's
    // hard limits) stops it.
    if (arena) {
        // ---- an arena's path: its starts as the checkpoints and a route over its floor ----
        // Rush 2's stunt mode starts the car at spine point 0 facing point 1 (func_800A34A8); Rush 2049's battle starts
        // the players at the checkpoints, and the other cars and every respawn on the route's points
        // (rush2_track49_battle_start, the respawn hook at 0x800918E8); a crashed car otherwise comes back on the
        // nearest point of the lanes (func_80090A40). So the route covers the floor the cars can reach from START_1,
        // and its lanes are the route itself, as the Rush 2049 arenas' are (track2049_convert.cpp spine_lanes).
        auto on_ground = [&](Vec3 p) {
            float h = cpolys.empty() ? NAN : ground_under(cpolys, p, 60.0f);
            if (!std::isnan(h)) p[1] = h;
            return p;
        };
        std::vector<const CollisionTri*> floors;
        for (const CollisionTri& c : ctris) {
            int type = c.flags & 0xF;
            if (c.group == 0 && type <= 2 && normalize(c.front)[1] > 0.6f) floors.push_back(&c);
        }
        if (gates.empty()) {
            Vec3 lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f };
            for (const CollisionTri* c : floors) for (const Vec3& v : c->v) for (int k = 0; k < 3; k++) {
                lo[k] = std::min(lo[k], v[k]);
                hi[k] = std::max(hi[k], v[k]);
            }
            Gate g{ "START_1", {}, { 0.0f, 0.0f, 1.0f }, 20.0f };
            float best = 1e30f;
            for (const CollisionTri* c : floors) {
                Vec3 at = mul(add(add(c->v[0], c->v[1]), c->v[2]), 1.0f / 3.0f);
                float d = dist_xz(at, mul(add(lo, hi), 0.5f));
                if (d < best) { best = d; g.pos = at; }
            }
            report.warning("No START_1 (or FINISH): the cars start at a stand-in spot on the floor, " + fmt_pos(g.pos) +
                           ". Add Empties START_1, START_2, ... where the cars start, green arrow forward.");
            gates.push_back(g);
        }
        for (Gate& g : gates) g.pos = on_ground(g.pos);
        if (settings.type == "battle" && gates.size() < 4) {
            report.info(std::to_string(gates.size()) + " start(s): players past them start on the route, as far from the "
                        "others as it goes (Rush 2049's rule for the cars past its 4 starts).");
        }
        std::vector<PathPoint> route;
        auto add_point = [&](const Vec3& p) { route.push_back({ p, { 1.0f, 0.0f, 0.0f }, 0.0f }); };
        add_point(gates[0].pos);
        add_point(on_ground(add(gates[0].pos, mul(gates[0].dir, spine_step))));
        if (rungs.size() >= 4) {
            // AIPATH_main: its points every spine_step ft, from the one nearest START_1 on.
            std::vector<Vec3> pts;
            for (size_t i = 0; i + 1 < rungs.size(); i++) {
                float len = length(sub(rungs[i + 1].p, rungs[i].p));
                for (float t = 0.0f; t < len; t += spine_step) pts.push_back(add(rungs[i].p, mul(sub(rungs[i + 1].p, rungs[i].p), t / len)));
            }
            size_t near = 0;
            for (size_t i = 0; i < pts.size(); i++) if (length(sub(pts[i], gates[0].pos)) < length(sub(pts[near], gates[0].pos))) near = i;
            for (size_t k = 0; k < pts.size() && route.size() < 3900; k++) add_point(on_ground(pts[(near + k) % pts.size()]));
            report.info("AIPATH_main is the arena's route: " + std::to_string(route.size()) + " points.");
        }
        else {
            // A grid over the floor: a point per cell and floor level, those reachable from START_1 by steps to a
            // neighboring cell that climb no more than a ramp would, each next one the nearest left.
            float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
            for (const CollisionTri* c : floors) for (const Vec3& v : c->v) {
                x0 = std::min(x0, v[0]); x1 = std::max(x1, v[0]);
                z0 = std::min(z0, v[2]); z1 = std::max(z1, v[2]);
            }
            float cell = std::max(30.0f, std::sqrt(std::max(1.0f, (x1 - x0) * (z1 - z0)) / 2500.0f));
            std::map<std::pair<int, int>, std::vector<float>> levels;
            for (const CollisionTri* c : floors) {
                const Vec3 &a = c->v[0], &b = c->v[1], &q = c->v[2];
                float d = (b[0] - a[0]) * (q[2] - a[2]) - (q[0] - a[0]) * (b[2] - a[2]);
                if (std::fabs(d) < 1e-6f) continue;
                int i0 = int(std::floor((std::min({ a[0], b[0], q[0] }) - x0) / cell)), i1 = int(std::floor((std::max({ a[0], b[0], q[0] }) - x0) / cell));
                int j0 = int(std::floor((std::min({ a[2], b[2], q[2] }) - z0) / cell)), j1 = int(std::floor((std::max({ a[2], b[2], q[2] }) - z0) / cell));
                for (int i = i0; i <= i1; i++) for (int j = j0; j <= j1; j++) {
                    float x = x0 + (float(i) + 0.5f) * cell, z = z0 + (float(j) + 0.5f) * cell;
                    float u = ((x - a[0]) * (q[2] - a[2]) - (q[0] - a[0]) * (z - a[2])) / d;
                    float w = ((b[0] - a[0]) * (z - a[2]) - (x - a[0]) * (b[2] - a[2])) / d;
                    if (u < 0.0f || w < 0.0f || u + w > 1.0f) continue;
                    float h = a[1] + u * (b[1] - a[1]) + w * (q[1] - a[1]);
                    auto& l = levels[{ i, j }];
                    if (std::none_of(l.begin(), l.end(), [&](float e) { return std::fabs(e - h) < 6.0f; })) l.push_back(h);
                }
            }
            struct Node { int i, j; Vec3 p; };
            std::vector<Node> nodes;
            std::map<std::pair<int, int>, std::vector<size_t>> at_cell;
            for (const auto& [ij, hs] : levels) {
                for (float h : hs) {
                    at_cell[ij].push_back(nodes.size());
                    nodes.push_back({ ij.first, ij.second, { x0 + (float(ij.first) + 0.5f) * cell, h, z0 + (float(ij.second) + 0.5f) * cell } });
                }
            }
            size_t start = 0;
            for (size_t k = 0; k < nodes.size(); k++) {
                if (length(sub(nodes[k].p, gates[0].pos)) < length(sub(nodes[start].p, gates[0].pos))) start = k;
            }
            std::vector<bool> reach(nodes.size(), false);
            std::vector<size_t> todo;
            if (!nodes.empty()) { reach[start] = true; todo.push_back(start); }
            const float climb = cell * 0.7f;       // a 35 degree ramp
            while (!todo.empty()) {
                size_t k = todo.back();
                todo.pop_back();
                for (int di = -1; di <= 1; di++) for (int dj = -1; dj <= 1; dj++) {
                    auto it = at_cell.find({ nodes[k].i + di, nodes[k].j + dj });
                    if (it == at_cell.end()) continue;
                    for (size_t o : it->second) {
                        if (!reach[o] && std::fabs(nodes[o].p[1] - nodes[k].p[1]) <= climb) { reach[o] = true; todo.push_back(o); }
                    }
                }
            }
            std::vector<size_t> left;
            for (size_t k = 0; k < nodes.size(); k++) if (reach[k]) left.push_back(k);
            Vec3 cur = route.back().p;
            while (!left.empty() && route.size() < 3900) {
                size_t best = 0;
                float best_d = 1e30f;
                for (size_t k = 0; k < left.size(); k++) {
                    Vec3 d = sub(nodes[left[k]].p, cur);
                    float dd = d[0] * d[0] + d[2] * d[2] + 4.0f * d[1] * d[1];
                    if (dd < best_d) { best_d = dd; best = k; }
                }
                cur = nodes[left[best]].p;
                add_point(cur);
                left[best] = left.back();
                left.pop_back();
            }
            report.info("The arena's route: " + std::to_string(route.size()) + " points on the floor reachable from " +
                        gates[0].name + ", " + fmt_ft(cell) + " apart (draw AIPATH_main to choose them yourself).");
            if (route.size() <= 2) {
                report.warning("The arena has no floor (COL_ROAD, COL_DIRT, COL_GRASS or a custom surface) under " +
                               gates[0].name + ": respawns come back to the start.");
            }
        }
        std::vector<float> mph(route.size(), 100.0f);     // the Rush 2049 arenas' lanes: 100 mph, flags 2
        write_path(route, offset_lanes(route), mph, gates, out.track.path);
        out.track.path_backward = out.track.path;
        out.checkpoints = 0;
        out.lap_feet = 0.0f;
        // Battle pickups (src/rush2049/battle.cpp draws them with Rush 2049's models), at least 2.5 ft over the floor.
        if (pickup_nodes.size() > size_t(rush2::track2049::max_custom_pickups)) {
            report.warning("An arena can have " + std::to_string(rush2::track2049::max_custom_pickups) + " PICKUP_ markers; the other " +
                           std::to_string(pickup_nodes.size() - size_t(rush2::track2049::max_custom_pickups)) + " are left out.");
            pickup_nodes.resize(size_t(rush2::track2049::max_custom_pickups));
        }
        for (const auto& [kind, node] : pickup_nodes) {
            rush2::track2049::PickupRecord r{ -1, kind, {} };
            Vec3 p = to_game(node->position());
            float h = cpolys.empty() ? NAN : ground_under(cpolys, p, 30.0f);
            if (!std::isnan(h) && p[1] - h < 2.5f) p[1] = h + 2.5f;
            for (int k = 0; k < 3; k++) r.pos[k] = p[k];
            out.track.pickup_records.push_back(r);
        }
        if (settings.type == "battle") {
            int weapons = 0, powerups = 0;
            for (const auto& [kind, node] : pickup_nodes) (kind == 11 ? powerups : weapons)++;
            if (pickup_nodes.empty()) {
                report.warning("No PICKUP_ markers: the cars only have their guns. Add Empties such as PICKUP_MISSILE "
                               "and PICKUP_POWERUP where the pickups float.");
            }
            else {
                report.info("Pickups: " + std::to_string(weapons) + " weapon(s), " + std::to_string(powerups) + " power-up spot(s).");
            }
        }
    }
    else if (!spine.empty() && open_path) {
        // Raced one way only (the track select lists no BACKWARD for it): the backward file is the forward one. The
        // race behind the records screen starts its cars at START, not partway down a road that ends or on the loop.
        write_path(spine, lanes, speed_mph, gates, out.track.path, &lane_ai);
        out.track.path_backward = out.track.path;
        int16_t at = int16_t(std::max(0, gates[0].at));
        for (int b = 0; b < 2; b++) out.track.demo_starts[b].assign(4, at);
        out.checkpoints = int(gates.size()) - 2;
    }
    else if (!spine.empty()) {
        write_path(spine, lanes, speed_mph, gates, out.track.path, &lane_ai);
        // Backward: from the finish line the other way round (point i is forward point n - i), lanes mirrored with
        // the direction, gates turned round, the finish first and then CHECKPOINT_n down to CHECKPOINT_1.
        size_t n = spine.size();
        std::vector<PathPoint> back(n);
        Lanes back_lanes;
        for (size_t i = 0; i < n; i++) {
            back[i] = spine[(n - i) % n];
            back[i].lateral = mul(back[i].lateral, -1.0f);
            back[i].speed = back[i].mode = std::nanf("");   // authored AI values are the forward race's
            // The turned lateral puts each offset lane in the opposite slot (+f across -lateral is -f across the
            // forward one); painted lanes go along with theirs.
            for (size_t k = 0; k < 4; k++) back_lanes[k].push_back(lanes[3 - k][(n - i) % n]);
        }
        std::vector<Gate> back_gates = { gates[0] };
        for (size_t g = gates.size() - 1; g >= 1; g--) back_gates.push_back(gates[g]);
        // A forward crossing between points k - 1 and k is first passed at backward point n - k + 1 (forward k - 1).
        for (Gate& g : back_gates) {
            g.dir = mul(g.dir, -1.0f);
            if (g.at > 0) g.at = int((n - size_t(g.at) + 1) % n);
        }
        write_path(back, back_lanes, target_speeds(back, false), back_gates, out.track.path_backward);
        for (size_t k = 0; k < 4; k++) out.track.demo_starts[0].push_back(int16_t(k * n / 4));
        for (size_t k = 0; k < 4; k++) out.track.demo_starts[1].push_back(int16_t(k * n / 4));
        out.checkpoints = int(gates.size()) - 1;
    }

    // ---- visual geometry ----
    std::vector<VisualTri> vtris;
    for (const auto* n : visual) append_tris(*n, vtris);
    // Moving objects' meshes, in their own frames: each is its own model, drawn by a placement record the movers code
    // moves.
    for (size_t i = 0; i < movers.size(); i++) {
        Mover& m = movers[i];
        auto it = mover_visual.find(m.node);
        size_t far = 0;
        if (it != mover_visual.end()) {
            for (const auto* n : it->second) {
                std::vector<VisualTri> world;
                append_tris(*n, world);
                for (VisualTri t : world) {
                    bool fits = true;
                    for (int k = 0; k < 3; k++) {
                        t.p[k] = mover_local(m, from_game(t.p[k]));
                        Vec3 d = dir_to_game(t.n[k]);
                        const Mat4& a = m.to_local;
                        t.n[k] = normalize(dir_to_game({ a[0] * d[0] + a[4] * d[1] + a[8] * d[2], a[1] * d[0] + a[5] * d[1] + a[9] * d[2],
                                                         a[2] * d[0] + a[6] * d[1] + a[10] * d[2] }));
                        for (int c = 0; c < 3; c++) fits = fits && std::fabs(t.p[k][c]) <= vertex_reach;
                    }
                    if (!fits && m.group) { far++; continue; }
                    m.tris.push_back(t);
                }
            }
        }
        if (far) {
            report.error("MOVE_" + m.name + " reaches more than " + fmt_ft(vertex_reach) + " from its origin; " +
                         std::to_string(far) + " of its triangles aren't drawn. Keep a moving object with collision near its origin.");
        }
        // A mesh reaching past vertex_reach is stored in coarser units and drawn scaled back up by its path.
        float extent = 0.0f;
        for (const VisualTri& t : m.tris) for (const Vec3& p : t.p) for (float c : p) extent = std::max(extent, std::fabs(c));
        if (extent > vertex_reach) m.unit = 16.0f * vertex_reach / extent;
        if (!m.tris.empty()) {
            char nm[32];
            snprintf(nm, sizeof(nm), "%sM%03zu", prefix.c_str(), i);
            m.model = nm;
        }
        if (m.model.empty() && m.group == 0) report.warning("MOVE_" + m.name + " has nothing to draw or collide with; ignored.");
        char b[200];
        snprintf(b, sizeof(b), "MOVE_%s: %zu keys over %.2f s, %s%s, %zu triangles, %s.", m.name.c_str(), m.times.size(),
                 m.times.back() - m.times.front(), m.set.mode.c_str(), m.set.mode == "loop" && !m.closed && m.times.size() > 1 ?
                 " (jumps back to its first key)" : "", m.tris.size(), m.group ? "collision moves with it" : "no collision");
        report.info(b);
    }
    // Billboards: each mesh in its node's frame with the node's scale (Rush 2 draws a camera card at its pose's position
    // only), turned half a turn so the face toward Blender's front view (-Y) is the one the camera sees: the game shows
    // the model's -z side. Billboards of the same mesh and scale share a model.
    // Props likewise, in their node's frame without the half turn, drawn at the node's rotation.
    struct BillboardModel { std::string name; std::vector<VisualTri> tris; Section section; };
    // prop: PropRecord::type, 0 = a plain billboard; card: drawn as a camera card (a billboard, or a prop with the
    // custom property billboard = true, which flies off still facing the camera).
    struct Billboard { size_t model; Vec3 pos; Vec3 axes[3]; int prop = 0; bool card = true; };
    std::vector<BillboardModel> billboard_models;
    std::vector<Billboard> billboards;
    std::vector<std::pair<int, const rush2::gltf::Node*>> placed_nodes;
    for (const auto* n : billboard_nodes) placed_nodes.push_back({ 0, n });
    placed_nodes.insert(placed_nodes.end(), prop_nodes.begin(), prop_nodes.end());
    size_t billboard_count = 0;
    {
        std::map<std::string, size_t> model_of;     // the model's triangles as bytes -> billboard_models
        size_t far = 0;
        for (const auto& [prop, n] : placed_nodes) {
            std::vector<VisualTri> world;
            append_tris(*n, world);
            Vec3 o = to_game(n->position());
            Vec3 ax[3];     // the node's axes in the game frame (its x mirrored with the frame)
            for (int a = 0; a < 3; a++) ax[a] = normalize(mul(dir_to_game(n->axis(a)), a == 0 ? -1.0f : 1.0f));
            bool card = !prop;
            if (prop && n->extras.contains("billboard")) {
                const auto& v = n->extras["billboard"];
                card = v.is_boolean() ? v.get<bool>() : v.is_number() && v.get<double>() != 0.0;
            }
            float turn = card ? -1.0f : 1.0f;
            auto local = [&](const Vec3& d) { return Vec3{ turn * dot(ax[0], d), dot(ax[1], d), turn * dot(ax[2], d) }; };
            std::vector<VisualTri> tris;
            bool fits = true;
            for (VisualTri t : world) {
                for (int k = 0; k < 3; k++) {
                    t.p[k] = local(sub(t.p[k], o));
                    t.n[k] = normalize(local(t.n[k]));
                    for (int c = 0; c < 3; c++) fits = fits && std::fabs(t.p[k][c]) <= vertex_reach;
                }
                tris.push_back(t);
            }
            if (!fits) { far++; continue; }
            if (tris.empty()) continue;
            // Keyed on what the model stores (1/16 ft vertices, uv and color to a 1/1024), not the floats, which
            // differ in their last bits from one placement to the next.
            std::string key;
            auto put = [&](float f, float unit) { long v = std::lround(f * unit); key.append((const char*)&v, sizeof(v)); };
            for (const VisualTri& t : tris) {
                for (int k = 0; k < 3; k++) {
                    for (float f : t.p[k]) put(f, 16.0f);
                    for (float f : t.uv[k]) put(f, 1024.0f);
                    if (t.has_col) for (float f : t.col[k]) put(f, 1024.0f);
                }
                key.append((const char*)&t.material, sizeof(t.material));
            }
            key.push_back(card ? 'B' : 'P');
            auto [it, added] = model_of.try_emplace(key, billboard_models.size());
            if (added) {
                char nm[32];
                snprintf(nm, sizeof(nm), "%s%c%03zu", prefix.c_str(), prop ? 'P' : 'B', billboard_models.size());
                billboard_models.push_back({ nm, std::move(tris), {} });
            }
            billboards.push_back({ it->second, o, { ax[0], ax[1], ax[2] }, prop, card });
            billboard_count += !prop;
        }
        if (far) {
            report.error(std::to_string(far) + " BILLBOARD_ or PROP_ object(s) reach more than " + fmt_ft(vertex_reach) +
                         " from their origin; they aren't drawn.");
        }
        if (billboard_count) {
            report.info("Billboards: " + std::to_string(billboard_count) + " BILLBOARD_ objects; they turn to face the camera.");
        }
        if (billboards.size() > billboard_count) {
            report.info("Props: " + std::to_string(billboards.size() - billboard_count) + " PROP_ objects (they react to "
                        "cars when the Rush 2049 ROM is set in the Games tab).");
        }
    }
    if (vtris.empty() && !collision_nodes.empty()) {
        report.error("Nothing to draw: the track has no visible meshes (only COL_* and markers); its COL_* meshes are "
                     "drawn instead.");
        for (const auto& [flags, n] : collision_nodes) append_tris(*n, vtris);
    }
    else if (vtris.empty()) {
        report.error("Nothing to draw: the track has no visible meshes (only markers).");
    }
    // Only a track with nothing to stand on or see, or a collision index the game can't hold, isn't loaded.
    if ((spine.empty() && out.track.path.empty()) || !collision_ok || vtris.empty()) return false;

    // The sky (SKY meshes): one model, SKYO1, which Rush 2's sky code (sky_setup_800A45A8's Las Vegas branch, taken for
    // custom tracks in src/rush2049/track2049.cpp) draws around the camera in every view, before the track, with depth
    // and fog off as Rush 2049's skies are. It is authored around the origin at any size and scaled to fit the model's
    // s16 vertices: drawn without depth, its size doesn't change how it looks.
    std::vector<VisualTri> stris;
    for (const auto* n : sky) append_tris(*n, stris);
    float sky_extent = 0.0f;
    for (const VisualTri& t : stris) for (const Vec3& p : t.p) sky_extent = std::max({ sky_extent, std::fabs(p[0]), std::fabs(p[1]), std::fabs(p[2]) });
    if (!stris.empty()) {
        char b[160];
        snprintf(b, sizeof(b), "SKY: %zu triangles, %s from the origin at most; drawn around the camera, behind everything.",
                 stris.size(), fmt_ft(sky_extent).c_str());
        report.info(b);
    }

    // Materials -> stand-in textures (one per material).
    std::vector<int> used_materials;
    for (const VisualTri& t : stris) if (std::find(used_materials.begin(), used_materials.end(), t.material) == used_materials.end()) used_materials.push_back(t.material);
    for (const VisualTri& t : vtris) if (std::find(used_materials.begin(), used_materials.end(), t.material) == used_materials.end()) used_materials.push_back(t.material);
    for (const Mover& m : movers) {
        for (const VisualTri& t : m.tris) if (std::find(used_materials.begin(), used_materials.end(), t.material) == used_materials.end()) used_materials.push_back(t.material);
    }
    for (const BillboardModel& b : billboard_models) {
        for (const VisualTri& t : b.tris) if (std::find(used_materials.begin(), used_materials.end(), t.material) == used_materials.end()) used_materials.push_back(t.material);
    }
    auto is_blend = [&](int m) { return m >= 0 && m < (int)scene.materials.size() && scene.materials[m].alpha_mode == "BLEND"; };
    std::map<int, int> texture_of;      // material -> out.textures
    std::map<int, bool> cutout_of;
    for (int m : used_materials) {
        const rush2::gltf::Material* mat = m >= 0 && m < (int)scene.materials.size() ? &scene.materials[m] : nullptr;
        bool cutout = mat && mat->alpha_mode != "OPAQUE";
        texture_of[m] = int(out.textures.size());
        cutout_of[m] = cutout;
        out.textures.push_back(make_texture(scene, mat, cutout ? (mat->alpha_mode == "MASK" ? mat->alpha_cutoff : 0.5f) : 0.0f));
    }

    // Sections on a grid, by triangle center. A triangle wider than a cell, or too big for its cell, joins the first
    // such section whose origin reaches all its corners, else gets a section of its own there (Alcatraz's bay, 10,000
    // ft out in pieces up to 2,500 ft, made 242 sections, one per piece, past what the game's scene holds: the race
    // crashed).
    std::map<std::tuple<int, int, bool>, int> cell_section;    // (cell x, cell z, translucent) -> section
    std::vector<int> big_sections;
    std::vector<Section> sections;
    size_t too_big = 0;
    Vec3 too_big_at{};
    for (size_t t = 0; t < vtris.size(); t++) {
        Vec3 c = mul(add(add(vtris[t].p[0], vtris[t].p[1]), vtris[t].p[2]), 1.0f / 3.0f);
        std::pair<int, int> cell = { int(std::floor(c[0] / section_size)), int(std::floor(c[2] / section_size)) };
        Vec3 origin = { (cell.first + 0.5f) * section_size, 0.0f, (cell.second + 0.5f) * section_size };
        bool fits = true;
        for (const Vec3& p : vtris[t].p) for (int k = 0; k < 3; k++) if (std::fabs(p[k] - origin[k]) > vertex_reach) fits = false;
        for (int k = 0; k < 3; k += 2) {
            float lo = std::min({ vtris[t].p[0][k], vtris[t].p[1][k], vtris[t].p[2][k] });
            float hi = std::max({ vtris[t].p[0][k], vtris[t].p[1][k], vtris[t].p[2][k] });
            if (hi - lo > section_size) fits = false;
        }
        int s;
        bool late = is_blend(vtris[t].material);
        if (fits) {
            auto key = std::make_tuple(cell.first, cell.second, late);
            auto it = cell_section.find(key);
            if (it == cell_section.end()) {
                s = int(sections.size());
                sections.push_back(Section{});
                sections[s].origin = origin;
                sections[s].late = late;
                cell_section[key] = s;
            }
            else s = it->second;
        }
        else {
            bool own = true;
            for (const Vec3& p : vtris[t].p) for (int k = 0; k < 3; k++) own = own && std::fabs(p[k] - c[k]) <= vertex_reach;
            if (!own) {   // left out: the track loads without it
                if (too_big++ == 0) too_big_at = c;
                continue;
            }
            s = -1;
            for (int b : big_sections) {
                if (sections[b].late != late) continue;
                bool reach = true;
                for (const Vec3& p : vtris[t].p) for (int k = 0; k < 3; k++) reach = reach && std::fabs(p[k] - sections[b].origin[k]) <= vertex_reach;
                if (reach) { s = b; break; }
            }
            if (s < 0) {
                s = int(sections.size());
                sections.push_back(Section{});
                sections[s].origin = c;
                sections[s].late = late;
                big_sections.push_back(s);
            }
        }
        sections[s].tris.push_back(int(t));
    }
    if (too_big) {
        report.error(std::to_string(too_big) + " visible triangle(s) are over " + fmt_ft(vertex_reach * 2) +
                     " across (the first near " + fmt_pos(too_big_at) + "); they aren't drawn. Subdivide them.");
    }
    if (sections.size() > 900) {
        report.error("The track is too big: " + std::to_string(sections.size()) + " sections of 312 m, the game holds "
                     "900; the rest aren't drawn.");
        sections.resize(900);
    }
    for (size_t s = 0; s < sections.size(); s++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "%sS%03zu", prefix.c_str(), s);
        sections[s].name = nm;
    }

    // A stock sky (track.toml sky): Rush 2's cloud dome is drawn by Rush 2's own sky code (src/rush2049/track2049.cpp
    // mod_cloud_dome); the others are a model copied in from the game as SKYO1, as a SKY mesh would be.
    ModelCopy sky_copy;
    bool stock_sky_model = false;
    if (settings.sky != "none") {
        const auto* k = stock_sky(settings.sky);
        if (!stris.empty()) {
            report.warning("track.toml sets sky = \"" + settings.sky + "\" and the track has a SKY mesh: the SKY mesh is used.");
        }
        else if (k && k->model[0]) {
            std::string why;
            if (extras.sky_container.empty()) {
                report.warning("The stock sky " + settings.sky + " couldn't be read" +
                               (k->game == 2049 ? " (it comes from the Rush 2049 ROM: set one in the Games tab)" : "") +
                               "; the sky is plain fog_color.");
            }
            else if (!plan_copy(extras.sky_container, extras.sky_model, sky_copy, why)) {
                report.error("The stock sky " + settings.sky + " couldn't be copied: " + why + ".");
            }
            else {
                stock_sky_model = true;
                report.info("Sky: the stock sky " + settings.sky + ".");
            }
        }
        else if (k) {
            report.info("Sky: Rush 2's cloud dome, in one-player races (cloud_scroll and sky_parallax move it).");
        }
    }

    // ---- geometry container (docs/rush2049_research/geometry.md section 2) ----
    std::vector<uint8_t>& g = out.track.geometry;
    g.assign(0x28, 0);
    for (Texture& t : out.textures) {
        align8(g);
        t.offset = uint32_t(g.size());
        g.insert(g.end(), t.texels.begin(), t.texels.end());
    }
    if (stock_sky_model) {
        for (const auto& [at, bytes] : sky_copy.blocks) {
            align8(g);
            sky_copy.moved[at] = uint32_t(g.size());
            g.insert(g.end(), sky_copy.src->begin() + at, sky_copy.src->begin() + at + bytes);
        }
    }
    align8(g);
    uint32_t txld_start = uint32_t(g.size());
    std::vector<uint32_t> load_lists;
    std::map<int, uint32_t> tile_of;
    std::vector<DrawMaterial> draw_materials;
    for (int m : used_materials) {
        const Texture& t = out.textures[texture_of[m]];
        const rush2::gltf::Material* mat = m >= 0 && m < (int)scene.materials.size() ? &scene.materials[m] : nullptr;
        draw_materials.push_back({ &t, cutout_of[m], is_blend(m), !mat || (mat->wrap_s != 33071 && mat->wrap_t != 33071) });
        load_lists.push_back(uint32_t(g.size()));
        for (const auto& [w0, w1] : texture_load(t, mat, t.offset)) { add32(g, w0); add32(g, w1); }
        add32(g, 0xDF000000);
        add32(g, 0);
    }
    if (stock_sky_model) {
        for (const auto& [at, bytes] : sky_copy.loads) {
            sky_copy.moved[at] = uint32_t(g.size());
            copy_list(sky_copy, at, bytes, g, 0, uint32_t(g.size()));
        }
    }
    uint32_t empty_list = uint32_t(g.size());
    add32(g, 0xDF000000);
    add32(g, 0);
    uint32_t txld_end = uint32_t(g.size());

    // Names the game looks up: every section, the finish models (unchecked handles, func_8008F080) and the
    // CHKPNT / FINISH textures; sorted for func_8005BCB4's binary search.
    std::vector<std::string> names;
    for (const Section& s : sections) names.push_back(s.name);
    if (!stris.empty() || stock_sky_model) names.push_back("SKYO1");
    for (const Mover& m : movers) if (!m.model.empty()) names.push_back(m.model);
    for (const BillboardModel& b : billboard_models) names.push_back(b.name);
    names.push_back(prefix + "FINISH");
    names.push_back(prefix + "FINISHB");
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return strncmp(a.c_str(), b.c_str(), 15) < 0; });
    uint32_t model_table = uint32_t(g.size());
    g.resize(g.size() + names.size() * 0x34, 0);
    uint32_t texture_table = uint32_t(g.size());
    for (const char* tn : { "CHKPNT", "FINISH" }) {
        add_name(g, tn);
        add16(g, 0); add16(g, 0);
        add32(g, 0xFFFF);
        add32(g, empty_list);
        add32(g, 0);
    }
    uint32_t palette_table = uint32_t(g.size());
    align8(g);

    for (Section& s : sections) {
        MeshList m;
        m.cmd(0xE3001001, 0x00000000);          // no TLUT: true color
        m.cmd(0xE3000F00, 0x00000000);          // no mipmaps
        m.cmd(combine[0], combine[1]);
        m.cmd(0xD7000002, 0xFFFFFFFF);          // G_TEXTURE on, tile 0, scale 1
        emit_mesh(vtris, s.tris, used_materials, draw_materials, s.origin, 16.0f,
                  [&](MeshList& ml, size_t mi) { ml.cmd(0xDE000000, load_lists[mi]); }, m);
        s.lo = m.lo;
        s.hi = m.hi;
        s.radius = m.radius;
        m.cmd(0xE3001001, 0x00008000); // the TLUT mode Rush 2's lists leave set (RGBA16)
        m.cmd(0xDF000000, 0);
        align8(g);
        uint32_t base = uint32_t(g.size());
        g.insert(g.end(), m.vertices.begin(), m.vertices.end());
        align8(g);
        s.list = uint32_t(g.size());
        for (size_t i : m.vtx_commands) m.list[i].second += base;
        for (const auto& [w0, w1] : m.list) { add32(g, w0); add32(g, w1); }
    }
    Section sky_model;
    if (!stris.empty()) {
        // As Rush 2049's SKYSKY lists (docs/rush2049_research/dreamcast.md, Sky): 1-cycle, TEXEL0 x SHADE in both
        // cycles, opaque, alpha-tested or translucent without fog, G_ZBUFFER and G_FOG off; then the track's state put back.
        constexpr uint32_t sky_opaque = 0x0F0A4000;   // G_RM_OPA_SURF, G_RM_OPA_SURF2
        constexpr uint32_t sky_cutout = 0x0F0A3000;   // G_RM_TEX_EDGE, G_RM_TEX_EDGE2
        constexpr uint32_t sky_blend = 0x00504240;    // G_RM_XLU_SURF, G_RM_XLU_SURF2
        std::vector<int> all(stris.size());
        for (size_t i = 0; i < all.size(); i++) all[i] = int(i);
        float unit = std::min(16.0f, 32000.0f / std::max(sky_extent, 1.0f));
        out.track.sky_scale = unit / 16.0f;
        MeshList m;
        m.cmd(0xE3001001, 0x00000000);
        m.cmd(0xE3000F00, 0x00000000);
        m.cmd(0xE3000A01, 0x00000000);          // 1-cycle
        m.cmd(0xFC121824, 0xFF33FFFF);          // TEXEL0 x SHADE
        m.cmd(0xD9FEFFFE, 0x00000000);          // G_ZBUFFER, G_FOG off
        m.cmd(0xD7000002, 0xFFFFFFFF);
        emit_mesh(stris, all, used_materials, draw_materials, Vec3{}, unit,
                  [&](MeshList& ml, size_t mi) { ml.cmd(0xDE000000, load_lists[mi]); }, m, sky_opaque, sky_cutout, sky_blend);
        m.cmd(0xD9FFFFFF, 0x00010001);          // G_ZBUFFER, G_FOG back on
        m.cmd(0xE3000A01, 0x00100000);          // 2-cycle
        m.cmd(combine[0], combine[1]);
        m.cmd(0xE3001001, 0x00008000);
        m.cmd(0xDF000000, 0);
        align8(g);
        uint32_t base = uint32_t(g.size());
        g.insert(g.end(), m.vertices.begin(), m.vertices.end());
        align8(g);
        sky_model.name = "SKYO1";
        sky_model.list = uint32_t(g.size());
        sky_model.radius = m.radius * unit / 16.0f;
        for (size_t i : m.vtx_commands) m.list[i].second += base;
        for (const auto& [w0, w1] : m.list) { add32(g, w0); add32(g, w1); }
    }
    if (stock_sky_model) {
        const std::vector<uint8_t>& d = *sky_copy.src;
        align8(g);
        uint32_t vtx_base = uint32_t(g.size());
        if (sky_copy.vtx1 > sky_copy.vtx0) {
            g.insert(g.end(), d.begin() + sky_copy.vtx0, d.begin() + sky_copy.vtx1);
            // The relocator's mirror pass follows the vertices from the list's first G_VTX while bit 15 of their +6
            // halfword says another follows (geometry.md 4.2): end the chain at the copy's last vertex.
            g[g.size() - 16 + 6] &= 0x7F;
            // Sky models keep their world height (StockSky::ground): raised as far as this track's start is above
            // the source track's, in 1/16 ft.
            float rise = ((gates.empty() ? 0.0f : gates[0].pos[1]) - stock_sky(settings.sky)->ground) * 16.0f;
            bool clipped = false;
            for (size_t at = vtx_base; at + 16 <= g.size(); at += 16) {
                long y = std::lround(int16_t(uint16_t(g[at + 2] << 8 | g[at + 3])) + rise);
                clipped |= y < -32768 || y > 32767;
                y = std::clamp(y, -32768l, 32767l);
                g[at + 2] = uint8_t(uint16_t(y) >> 8);
                g[at + 3] = uint8_t(y);
            }
            if (clipped) {
                report.warning("The stock sky " + settings.sky + " is squashed at the game's height limit: FINISH (at " +
                               fmt_ft(gates[0].pos[1]) + " high) is too far above or below the sky's own track.");
            }
        }
        for (const auto& [at, bytes] : sky_copy.subs) {
            align8(g);
            sky_copy.moved[at] = uint32_t(g.size());
            g.insert(g.end(), d.begin() + at, d.begin() + at + bytes);
        }
        align8(g);
        sky_model.name = "SKYO1";
        sky_model.list = uint32_t(g.size());
        sky_model.radius = sky_copy.radius;
        copy_list(sky_copy, sky_copy.list, sky_copy.list_bytes, g, vtx_base, sky_model.list);
    }
    for (Mover& mv : movers) {
        if (mv.model.empty()) continue;
        std::vector<int> all(mv.tris.size());
        for (size_t i = 0; i < all.size(); i++) all[i] = int(i);
        MeshList m;
        m.cmd(0xE3001001, 0x00000000);
        m.cmd(0xE3000F00, 0x00000000);
        m.cmd(combine[0], combine[1]);
        m.cmd(0xD7000002, 0xFFFFFFFF);
        emit_mesh(mv.tris, all, used_materials, draw_materials, Vec3{}, mv.unit,
                  [&](MeshList& ml, size_t mi) { ml.cmd(0xDE000000, load_lists[mi]); }, m);
        m.cmd(0xE3001001, 0x00008000);
        m.cmd(0xDF000000, 0);
        align8(g);
        uint32_t base = uint32_t(g.size());
        g.insert(g.end(), m.vertices.begin(), m.vertices.end());
        align8(g);
        mv.section.name = mv.model;
        mv.section.list = uint32_t(g.size());
        mv.section.radius = m.radius;     // ft: generous for a coarser unit (the path scales it back up)
        for (size_t i : m.vtx_commands) m.list[i].second += base;
        for (const auto& [w0, w1] : m.list) { add32(g, w0); add32(g, w1); }
    }
    for (BillboardModel& bm : billboard_models) {
        std::vector<int> all(bm.tris.size());
        for (size_t i = 0; i < all.size(); i++) all[i] = int(i);
        MeshList m;
        m.cmd(0xE3001001, 0x00000000);
        m.cmd(0xE3000F00, 0x00000000);
        m.cmd(combine[0], combine[1]);
        m.cmd(0xD7000002, 0xFFFFFFFF);
        emit_mesh(bm.tris, all, used_materials, draw_materials, Vec3{}, 16.0f,
                  [&](MeshList& ml, size_t mi) { ml.cmd(0xDE000000, load_lists[mi]); }, m);
        m.cmd(0xE3001001, 0x00008000);
        m.cmd(0xDF000000, 0);
        align8(g);
        uint32_t base = uint32_t(g.size());
        g.insert(g.end(), m.vertices.begin(), m.vertices.end());
        align8(g);
        bm.section.name = bm.name;
        bm.section.list = uint32_t(g.size());
        bm.section.radius = m.radius;
        bm.section.late = std::any_of(bm.tris.begin(), bm.tris.end(), [&](const VisualTri& t) { return is_blend(t.material); });
        for (size_t i : m.vtx_commands) m.list[i].second += base;
        for (const auto& [w0, w1] : m.list) { add32(g, w0); add32(g, w1); }
    }
    align8(g);
    uint32_t name_table = uint32_t(g.size());
    for (size_t i = 0; i < names.size(); i++) {
        const Section* sec = names[i] == "SKYO1" && (!stris.empty() || stock_sky_model) ? &sky_model : nullptr;
        for (const Section& s : sections) if (s.name == names[i]) sec = &s;
        for (const Mover& mv : movers) if (mv.model == names[i]) sec = &mv.section;
        for (const BillboardModel& bm : billboard_models) if (bm.name == names[i]) sec = &bm.section;
        uint32_t r = model_table + uint32_t(i) * 0x34;
        put32(g, r, 1);                              // one LOD
        put32(g, r + 4 + 8, sec ? sec->list : empty_list);
        add_name(g, names[i]);
        addf(g, sec ? sec->radius : 1.0f);
        add16(g, 0);                                 // behaviour: plain model
        add16(g, 0);
    }
    const uint32_t header[10] = { model_table, name_table, texture_table, palette_table, uint32_t(names.size()), 2, 0,
                                  txld_start, txld_end, 0 };
    for (int i = 0; i < 10; i++) put32(g, i * 4, header[i]);

    // ---- placement (docs/rush2049_research/placement.md): the sections, the finish model, the moving objects (world
    // space at their first key, no culling box; the movers code rewrites their matrices), then the billboards (camera
    // cards at their position, no culling box). Translucent sections and billboards are drawn in the late pass. ----
    std::vector<uint8_t>& pl = out.track.placement;
    add32(pl, 1);
    add32(pl, 0x18);
    add_name(pl, prefix);
    std::vector<int> mover_records;     // movers with a model, in record order
    for (size_t j = 0; j < movers.size(); j++) if (!movers[j].model.empty()) mover_records.push_back(int(j));
    size_t first_billboard = sections.size() + 1 + mover_records.size();
    size_t records = first_billboard + billboards.size();
    for (size_t i = 0; i < records; i++) {
        bool is_finish = i == sections.size();
        const Mover* mv = i > sections.size() && i < first_billboard ? &movers[mover_records[i - sections.size() - 1]] : nullptr;
        const Billboard* bb = i >= first_billboard ? &billboards[i - first_billboard] : nullptr;
        const BillboardModel* bm = bb ? &billboard_models[bb->model] : nullptr;
        add_name(pl, bm ? bm->name : mv ? mv->model : is_finish ? prefix + "FINISH" : sections[i].name);
        // Row-major, a point times the matrix (movers.md): lin transposed. A prop's rows are its axes.
        float rm[9];
        for (int k = 0; k < 9; k++) {
            rm[k] = bb && !bb->card ? bb->axes[k / 3][k % 3] : mv ? mv->lin[0][(k % 3) * 3 + k / 3] * 16.0f / mv->unit : (k % 4) == 0 ? 1.0f : 0.0f;
            addf(pl, rm[k]);
        }
        Vec3 pos = bb ? bb->pos : mv ? mv->pos[0] : is_finish ? (gates.empty() ? Vec3{} : gates[0].pos) : sections[i].origin;
        for (float f : pos) addf(pl, f);
        bool late = bm ? bm->section.late : !mv && !is_finish && sections[i].late;
        add32(pl, 0x40 | (late ? late_pass : 0) | (bb && bb->card ? camera_card : 0));
        if (bb && bb->prop) {
            rush2::track2049::PropRecord r{};
            r.record = int(i);
            r.type = bb->prop;
            r.direction = 0;
            memcpy(r.m, rm, sizeof(r.m));
            memcpy(r.pos, pos.data(), sizeof(r.pos));
            for (int k = 0; k < 9; k++) r.parent_m[k] = (k % 4) == 0 ? 1.0f : 0.0f;
            out.track.prop_records.push_back(r);
        }
        add16(pl, uint16_t(i + 1 < records ? int16_t(i + 1) : int16_t(-1)));
        add16(pl, 0xFFFF);
        add32(pl, 0);
        for (int k = 0; k < 6; k++) addf(pl, is_finish || mv || bb ? 0.0f : (k < 3 ? sections[i].lo[k] : sections[i].hi[k - 3]));
        if (mv) out.track.path_records.push_back({ int(mv - movers.data()), 0, int(i) });
    }

    // ---- moving objects' paths: a 2049 file with one PTHD chunk (movers.md; movers::parse_paths reads it) ----
    if (!movers.empty()) {
        std::vector<uint8_t>& pb = out.track.mover_paths;
        uint32_t headers = 4 + 24;      // directory at 4: PTHD's entry and an empty one ending it
        add32(pb, 4);
        for (char c : std::string("PTHD")) add8(pb, uint8_t(c));
        add32(pb, headers);
        add32(pb, uint32_t(movers.size()));
        pb.resize(headers, 0);
        uint32_t nodes_at = headers + uint32_t(movers.size()) * 36;
        std::vector<uint8_t> node_bytes;
        for (size_t j = 0; j < movers.size(); j++) {
            const Mover& m = movers[j];
            // Path name = type name = model name (the movers code makes a type for each custom path).
            char nm[32];
            snprintf(nm, sizeof(nm), "%sM%03zu", prefix.c_str(), j);
            bool loop = m.set.mode == "loop";
            size_t keys = m.times.size(), count = loop && m.closed ? keys - 1 : keys;
            uint32_t flags = 0x8000;    // no TRIGGERON/OFF models or sounds
            flags |= loop ? 0x2 : m.set.mode == "pingpong" ? 0x1 : 0x40 | 0x4000;
            flags |= m.set.races == "forward" ? 0x4 : m.set.races == "backward" ? 0x8 : 0xC;
            if (m.group) flags |= 0x20 | (m.set.races != "both" ? 0x800 : 0);
            add_name(pb, nm);
            add32(pb, flags);
            add16(pb, uint16_t(count));
            add16(pb, uint16_t(m.group));
            add32(pb, 0);
            add32(pb, nodes_at + uint32_t(node_bytes.size()));
            add32(pb, 0xFFFFFFFFu);
            for (size_t i = 0; i < count; i++) {
                // Each node runs at a steady speed to the next key (the last: back to the first for a loop, in a
                // moment if the animation doesn't end where it began).
                size_t next = i + 1 < keys ? i + 1 : 0;
                bool last = i + 1 == count;
                float time = last && loop ? (m.closed ? m.times[keys - 1] - m.times[i] : 0.001f)
                           : last ? 1.0f : m.times[next] - m.times[i];
                Vec3 d = last && !loop ? Vec3{} : sub(last && loop ? m.pos[0] : m.pos[next], m.pos[i]);
                float dist = length(d);
                if (keys == 1) time = 1.0f;
                time = std::max(time, 0.0001f);
                for (float f : m.pos[i]) addf(node_bytes, f);
                for (float f : normalize(d)) addf(node_bytes, f);
                for (float f : m.scale[i]) addf(node_bytes, f * 16.0f / m.unit);
                for (float f : m.quat[i]) addf(node_bytes, f);
                addf(node_bytes, dist);
                addf(node_bytes, time);
                addf(node_bytes, dist / time);
                add32(node_bytes, 0x8 | (i == 0 ? 0x1 : 0));    // steady speed; the first node spawns the object
            }
        }
        pb.insert(pb.end(), node_bytes.begin(), node_bytes.end());
    }

    // ---- tables ----
    out.track.fog[0] = settings.fog[0];
    out.track.fog[1] = settings.fog[1];
    out.track.fog[2] = settings.fog[2];
    out.track.pvs_count = 0;                         // no visibility regions: every section is drawn
    char b[200];
    size_t tri_count = vtris.size();
    snprintf(b, sizeof(b), "Track: %zu visible triangles in %zu sections, %zu textures; geometry %zu bytes.", tri_count,
             sections.size(), out.textures.size(), g.size());
    report.info(b);
    for (const auto& [name, sf] : settings.surfaces) {
        out.custom_drag[sf.material] = sf.drag;
        snprintf(b, sizeof(b), "Surface COL_%s: like %s, drag %g (slot %d).", name.c_str(), sf.like.c_str(), sf.drag, sf.material);
        report.info(b);
    }

    // ---- track select art ----
    if (extras.diorama.empty()) {
        report.info("No diorama ([menu] diorama): the track select shows Rush 2049's preview of the AI path.");
    }
    else {
        rush2::gltf::Scene d;
        std::string why;
        if (!rush2::gltf::read_glb(extras.diorama, d, why)) {
            report.warning(settings.diorama + ": " + why + " The track select shows no diorama.");
        }
        else {
            std::vector<VisualTri> dtris;
            for (const rush2::gltf::Node& n : d.nodes) append_tris(n, dtris);
            build_diorama(out.textures, d, dtris, settings.diorama, out.menu, report);
        }
    }
    return true;
}
