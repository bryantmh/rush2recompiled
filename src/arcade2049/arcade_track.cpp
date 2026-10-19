// Converts the Rush 2049 arcade (SE) track Alcatraz (TRACKS/TRACK4) into a custom-track mod (docs/custom_tracks.md):
// a port of tools/arcade2049/track_mod.py, whose comments explain each step (docs/arcade2049.md, "What the converter
// makes"). Where the Python leans on shapely, Clipper2 does the polygon booleans (the spline road's footprints, at
// 1/4096 ft, and their union on the 1/32 ft grid set_precision snapped to) and earcut the triangulation (Delaunay
// in the Python: the triangles differ, the area they cover doesn't, and the converter joins them into convex polygons
// again). The logo and the round preview are fixed pictures for this one track, shipped as assets/arcade2049/ (made
// by the Python tool: tools/banner_font.py's lettering and round_preview).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

#include "clipper2/clipper.h"
#include "mapbox/earcut.hpp"

#include "arcade2049.h"
#include "texture_upscale.h"
#include "arcade_internal.h"

namespace a49 = rush2::arcade2049;
using namespace a49;

namespace {
    const char* const sky_textures[] = { "GS_SKY", "GS_APANS", "AL_MOON" };
    constexpr double lane_map = 1.2;           // a Rush 2049 lane speed / 1.2 is the Rush 2 lane speed
    constexpr double sky_radius = 9000.0;      // ft: objects reaching past this from the origin are sky
    constexpr double max_edge = 2500.0;        // ft: longer scenery triangle edges are split
    constexpr double water_edge = 2500.0;      // ft: water collision triangles are split as scenery is
    constexpr size_t max_leaf = 36;            // the game's collision queries hold 36 polygons per index cell
    constexpr double cell_size = 4.0;          // ft: the converter's smallest cell
    constexpr int spline_window = 30;          // segments either side of a polygon's own that can hold part of it
    constexpr double spline_fit = 1.0;         // ft: a piece is on a segment's surface when its facet is this close

    const char* surface_name(int type) {
        switch (type) {
        case 0: return "ROAD";
        case 1: return "DIRT";
        case 2: return "GRASS";
        case 3: return "WATER";
        case 5: case 6: return "WALL";
        case 7: return "KILL";
        default: return nullptr;
        }
    }

    bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
    std::string lower(std::string s) {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    }
    std::string upper(std::string s) {
        for (char& c : s) c = char(std::toupper((unsigned char)c));
        return s;
    }
    std::vector<std::string> split_words(const std::string& s) {
        std::vector<std::string> out;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
            size_t j = i;
            while (j < s.size() && !std::isspace((unsigned char)s[j])) j++;
            if (j > i) out.push_back(s.substr(i, j - i));
            i = j;
        }
        return out;
    }

    struct Corner {
        V3 p;
        V2 uv;
        std::array<double, 4> col;
    };
    using Tri = std::array<Corner, 3>;

    // Halves the longest edge (uv and color interpolated) until every edge is under `limit` ft (0: never).
    std::vector<Tri> split_large(const Tri& tri, double limit) {
        std::vector<Tri> out, todo{ tri };
        while (!todo.empty()) {
            Tri t = todo.back();
            todo.pop_back();
            double lens[3];
            for (int k = 0; k < 3; k++) lens[k] = norm(t[(k + 1) % 3].p - t[k].p);
            int k = int(std::max_element(lens, lens + 3) - lens);
            if (limit == 0 || lens[k] < limit) {
                out.push_back(t);
                continue;
            }
            const Corner &a = t[k], &b = t[(k + 1) % 3], &c = t[(k + 2) % 3];
            Corner m;
            m.p = (a.p + b.p) * 0.5;
            m.uv = { (a.uv[0] + b.uv[0]) / 2, (a.uv[1] + b.uv[1]) / 2 };
            for (int i = 0; i < 4; i++) m.col[i] = (a.col[i] + b.col[i]) / 2;
            todo.push_back({ a, m, c });
            todo.push_back({ m, b, c });
        }
        return out;
    }

    // Primitive lists by material, in first-use order (a Python dict).
    struct PrimSet {
        std::vector<int> order;
        std::map<int, Glb::Prim> by_material;
        Glb::Prim& operator[](int m) {
            auto it = by_material.find(m);
            if (it != by_material.end()) return it->second;
            order.push_back(m);
            Glb::Prim& p = by_material[m];
            p.material = m;
            return p;
        }
        std::vector<Glb::Prim> list() const {
            std::vector<Glb::Prim> out;
            for (int m : order) out.push_back(by_material.at(m));
            return out;
        }
        bool empty() const { return order.empty(); }
    };

    // ------------------------------------------------------------------------------------------ 2D geometry
    using namespace Clipper2Lib;
    constexpr double fine = 4096.0;            // Clipper units per foot for the spline road's cuts
    constexpr double grid = 32.0;              // the 1/32 ft grid footprints are joined on

    Path64 to_path(const std::vector<V2>& pts, double scale) {
        Path64 p;
        for (const V2& q : pts) p.push_back(Point64(int64_t(std::llround(q[0] * scale)), int64_t(std::llround(q[1] * scale))));
        return p;
    }

    // A boolean operation keeping collinear corners, as shapely does: footprints of neighbouring segments then keep
    // the same corners along the border they share, and it snaps to the grid alike on both sides (dropped on one
    // side, the snapped corner opened slivers of road with no collision).
    Paths64 boolean(ClipType op, const Paths64& subject, const Paths64& clip = {}) {
        Clipper64 c;
        c.PreserveCollinear(true);
        c.AddSubject(subject);
        if (!clip.empty()) c.AddClip(clip);
        Paths64 out;
        c.Execute(op, FillRule::NonZero, out);
        return out;
    }

    double paths_area(const Paths64& p, double scale) {
        double a = 0;
        for (const Path64& q : p) a += Area(q);
        return std::abs(a) / (scale * scale);
    }

    // A polygon with its holes (rings in feet).
    struct Shape {
        std::vector<std::vector<V2>> rings;    // [0] the outline, then the holes
    };

    std::vector<Shape> shapes_of(const Paths64& paths, double scale) {
        PolyTree64 tree;
        Clipper64 c;
        c.PreserveCollinear(true);
        c.AddSubject(paths);
        c.Execute(ClipType::Union, FillRule::NonZero, tree);
        std::vector<Shape> out;
        std::function<void(const PolyPath64&)> walk = [&](const PolyPath64& n) {
            for (const auto& child : n) {
                if (child->IsHole()) continue;
                Shape s;
                auto ring = [&](const Path64& p) {
                    std::vector<V2> r;
                    for (const Point64& q : p) r.push_back({ q.x / scale, q.y / scale });
                    return r;
                };
                s.rings.push_back(ring(child->Polygon()));
                for (const auto& hole : *child) {
                    s.rings.push_back(ring(hole->Polygon()));
                    walk(*hole);     // islands inside the hole
                }
                out.push_back(std::move(s));
            }
        };
        walk(tree);
        return out;
    }

    double ring_area(const std::vector<V2>& r) {
        double a = 0;
        for (size_t k = 0; k < r.size(); k++) {
            const V2 &p = r[k], &q = r[(k + 1) % r.size()];
            a += p[0] * q[1] - q[0] * p[1];
        }
        return a / 2;
    }

    double shape_area(const Shape& s) {
        double a = std::abs(ring_area(s.rings[0]));
        for (size_t k = 1; k < s.rings.size(); k++) a -= std::abs(ring_area(s.rings[k]));
        return a;
    }

    // Triangles covering the shape (earcut), as corner triples.
    std::vector<std::array<V2, 3>> triangulate(const Shape& s) {
        std::vector<std::vector<std::array<double, 2>>> rings;
        std::vector<V2> flat;
        for (const auto& r : s.rings) {
            rings.emplace_back();
            for (const V2& p : r) rings.back().push_back({ p[0], p[1] }), flat.push_back(p);
        }
        std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(rings);
        std::vector<std::array<V2, 3>> out;
        for (size_t k = 0; k + 2 < idx.size(); k += 3) out.push_back({ flat[idx[k]], flat[idx[k + 1]], flat[idx[k + 2]] });
        return out;
    }

    double tri_area(const std::array<V2, 3>& t) {
        return std::abs((t[1][0] - t[0][0]) * (t[2][1] - t[0][1]) - (t[2][0] - t[0][0]) * (t[1][1] - t[0][1])) / 2;
    }

    // A point inside the shapes (GEOS's interior point, as shapely's representative_point): per polygon, the line
    // across it at the height midway between the vertex heights nearest its middle; the middle of the widest stretch
    // of that line inside any of them.
    bool interior_point(const std::vector<Shape>& shapes, V2& out) {
        double best = -1;
        for (const Shape& s : shapes) {
            double lo = 1e300, hi = -1e300;
            for (const V2& p : s.rings[0]) lo = std::min(lo, p[1]), hi = std::max(hi, p[1]);
            double centre = (lo + hi) / 2, lo_y = lo, hi_y = hi;
            for (const auto& r : s.rings) {
                for (const V2& p : r) {
                    if (p[1] <= centre && p[1] > lo_y) lo_y = p[1];
                    else if (p[1] > centre && p[1] < hi_y) hi_y = p[1];
                }
            }
            double y = (lo_y + hi_y) / 2;
            std::vector<double> xs;
            for (const auto& r : s.rings) {
                for (size_t k = 0; k < r.size(); k++) {
                    const V2 &a = r[k], &b = r[(k + 1) % r.size()];
                    if ((a[1] > y) != (b[1] > y)) xs.push_back(a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                if (xs[k + 1] - xs[k] > best) {
                    best = xs[k + 1] - xs[k];
                    out = { (xs[k] + xs[k + 1]) / 2, y };
                }
            }
        }
        return best >= 0;
    }

    // ------------------------------------------------------------------------------------- spline segments
    // A SEG record by field (tools/rush2049/collision.py seg_fields).
    struct Seg {
        V3 O, M[3], f30, f3C, f48;
        double f54, f58, f5C, f60, f64, f68, f6C, f70, f74, f78;
        explicit Seg(const std::array<double, 33>& v) {
            O = { v[0], v[1], v[2] };
            for (int r = 0; r < 3; r++) M[r] = { v[3 + 3 * r], v[4 + 3 * r], v[5 + 3 * r] };
            f30 = { v[12], v[13], v[14] };
            f3C = { v[15], v[16], v[17] };
            f48 = { v[18], v[19], v[20] };
            f54 = v[21], f58 = v[22], f5C = v[23], f60 = v[24], f64 = v[25], f68 = v[26], f6C = v[27], f70 = v[28];
            f74 = v[29], f78 = v[30];
        }
    };

    // collision_seg_surface_8006BDA8 (collision.py seg_surface) for one point: its height above segment S's surface
    // along the normal, t along the segment, u across.
    void seg_surface(const Seg& S, const Seg& T, const V3& P, double& h, double& t, double& u) {
        V3 d = P - S.O;
        V3 L{ dot(d, S.M[0]), dot(d, S.M[1]), dot(d, S.M[2]) };
        double length = L[0] * (S.f60 - S.f5C) + S.f58;
        t = (L[2] - L[0] * S.f5C) / length;
        u = L[0] > 0 ? 0.5 + 0.5 * L[0] / (L[2] * S.f70 + S.f6C) : 0.5 - 0.5 * L[0] / (L[2] * S.f68 + S.f64);
        auto lerp = [](const V3& a, const V3& b, double s) { return a * (1 - s) + b * s; };
        V3 along = lerp(S.f3C, S.f48, u) * (1 - t) + lerp(T.f3C, T.f48, u) * t;
        V3 across = lerp(S.f30, T.f30, t);
        V3 normal = cross(along, across);
        V3 ps = S.O + S.f30 * ((u - 0.5) * S.f54);
        V3 pt = T.O + T.f30 * ((u - 0.5) * T.f54);
        double h0 = S.f74 + (S.f78 - S.f74) * u, h1 = T.f74 + (T.f78 - T.f74) * u;
        double lift = (t * t * t * (h0 - h1) - t * t * (2 * h0 - h1) + t * h0) * length;
        V3 q = ps * (1 - t) + pt * t - normal * lift;
        h = dot(P - q, normal);
    }

    struct FlatPoly {
        uint32_t flags;
        std::vector<V3> pts;
        V3 normal;
    };

    struct FootprintTri {
        uint32_t flags;
        std::array<V3, 3> pts;
        int seg;
    };

    // track_mod.py spline_road: each segment gets the part of the spline road over its stretch, joined into flat
    // footprint polygons that take their height from the segment; the rest stays flat.
    void spline_road(const std::vector<std::array<double, 33>>& records, const std::vector<CollisionPoly>& polys,
                     std::vector<FootprintTri>& tris, std::vector<FlatPoly>& flat, std::vector<std::string>& report) {
        std::vector<Seg> segs;
        for (const auto& r : records) segs.emplace_back(r);
        int ns = int(segs.size());
        auto nxt = [&](int k) -> const Seg& { return segs[(k + 1) % ns]; };

        // Segment k's stretch from above: its start line to its next one's, well past the road's edges (u -1.5..2.5).
        std::vector<std::optional<Path64>> bands(ns);
        for (int k = 0; k < ns; k++) {
            const Seg &S = segs[k], &T = nxt(k);
            double h, tt, u;
            seg_surface(S, T, T.O, h, tt, u);
            if (!(std::abs(tt - 1) < 0.05 && std::abs(u - 0.5) < 0.05)) continue;   // a chain's end
            auto line = [](const Seg& G, double u) {
                V3 p = G.O + G.f30 * ((u - 0.5) * G.f54);
                return V2{ p[0], p[2] };
            };
            std::vector<V2> q{ line(S, -1.5), line(S, 2.5), line(T, 2.5), line(T, -1.5) };
            // shapely's is_valid: no edge crosses the opposite one (a bowtie).
            auto crosses = [](V2 a, V2 b, V2 c, V2 d) {
                auto side = [](V2 p, V2 q, V2 r) { return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0]); };
                return side(a, b, c) * side(a, b, d) < 0 && side(c, d, a) * side(c, d, b) < 0;
            };
            if (crosses(q[0], q[1], q[2], q[3]) || crosses(q[1], q[2], q[3], q[0])) continue;
            if (!(std::abs(ring_area(q)) > 0.01)) continue;
            bands[k] = to_path(q, fine);
        }

        auto facet_y = [](const std::vector<V3>& pts, const V3& n, double x, double z) {
            return pts[0][1] - (n[0] * (x - pts[0][0]) + n[2] * (z - pts[0][2])) / n[1];
        };

        std::map<std::pair<int, uint32_t>, Paths64> pieces;     // (segment, flags) -> footprint parts
        int spline_polys = 0;
        for (const CollisionPoly& p : polys) {
            if (p.seg >= 0 && (p.flags & 0x1000)) spline_polys++;
            if (p.seg < 0 || !(p.flags & 0x1000) || (p.flags & 0xF) > 2 || p.normal[1] < 0.2) {
                flat.push_back({ p.flags, p.pts, p.normal });
                continue;
            }
            std::vector<V2> xz;
            for (const V3& q : p.pts) xz.push_back({ q[0], q[2] });
            Paths64 rest = boolean(ClipType::Union, Paths64{ to_path(xz, fine) });
            std::vector<int> ks;
            for (int k = std::max(0, p.seg - spline_window); k < std::min(ns, p.seg + spline_window + 1); k++) ks.push_back(k);
            std::stable_sort(ks.begin(), ks.end(), [&](int a, int b) { return std::abs(a - p.seg) < std::abs(b - p.seg); });
            for (int k : ks) {
                if (!bands[k] || rest.empty()) continue;
                Paths64 part = boolean(ClipType::Intersection, rest, Paths64{ *bands[k] });
                if (paths_area(part, fine) < 0.01) continue;
                V2 c;
                if (!interior_point(shapes_of(part, fine), c)) continue;
                double h, tt, u;
                seg_surface(segs[k], nxt(k), { c[0], facet_y(p.pts, p.normal, c[0], c[1]), c[1] }, h, tt, u);
                if (std::abs(h) < spline_fit && tt >= -0.01 && tt <= 1.01) {
                    Paths64& to = pieces[{ k, p.flags & 0xFF }];
                    to.insert(to.end(), part.begin(), part.end());
                    rest = boolean(ClipType::Difference, rest, Paths64{ *bands[k] });
                }
            }
            for (const Shape& left : shapes_of(rest, fine)) {
                if (shape_area(left) <= 0.05) continue;
                for (const auto& t : triangulate(left)) {
                    if (tri_area(t) <= 1e-9) continue;
                    FlatPoly fp{ p.flags, {}, p.normal };
                    for (const V2& q : t) fp.pts.push_back({ q[0], facet_y(p.pts, p.normal, q[0], q[1]), q[1] });
                    flat.push_back(std::move(fp));
                }
            }
        }

        // The height of segment k's surface over (x, z): the lift along the normal, solved for y.
        auto surface_y = [&](int k, double x, double z, double y) {
            for (int i = 0; i < 4; i++) {
                double h0, h1, t, u;
                seg_surface(segs[k], nxt(k), { x, y, z }, h0, t, u);
                seg_surface(segs[k], nxt(k), { x, y + 1.0, z }, h1, t, u);
                if (std::abs(h1 - h0) < 1e-6) break;
                y -= h0 / (h1 - h0);
            }
            return y;
        };

        std::vector<double> off;
        for (const auto& [key, parts] : pieces) {
            auto [k, flags] = key;
            // Joined, then put on the file's 1/32 ft grid (shapely's unary_union, then set_precision): corners a hair
            // apart become one and shared borders snap alike.
            Paths64 snapped;
            for (const Path64& q : boolean(ClipType::Union, parts)) {
                Path64 s;
                for (const Point64& pt : q) {
                    s.push_back(Point64(int64_t(std::llround(pt.x * grid / fine)), int64_t(std::llround(pt.y * grid / fine))));
                }
                snapped.push_back(std::move(s));
            }
            for (const Shape& part : shapes_of(snapped, grid)) {
                if (shape_area(part) < 0.01) continue;
                // One plane per footprint fitted to the segment's surface at its corners (least squares).
                const auto& ring = part.rings[0];
                double ata[3][3] = {}, aty[3] = {};
                std::vector<double> ys;
                for (const V2& q : ring) {
                    double y = surface_y(k, q[0], q[1], segs[k].O[1]);
                    ys.push_back(y);
                    double row[3] = { q[0], q[1], 1.0 };
                    for (int i = 0; i < 3; i++) {
                        aty[i] += row[i] * y;
                        for (int j = 0; j < 3; j++) ata[i][j] += row[i] * row[j];
                    }
                }
                // Solve ata c = aty (Cramer's rule; a footprint is never a line, so it is well conditioned).
                auto det3 = [](double m[3][3]) {
                    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
                };
                double det = det3(ata), coef[3] = { 0, 0, segs[k].O[1] };
                if (std::abs(det) > 1e-12) {
                    for (int c = 0; c < 3; c++) {
                        double m[3][3];
                        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i][j] = j == c ? aty[i] : ata[i][j];
                        coef[c] = det3(m) / det;
                    }
                }
                double worst = 0;
                for (size_t i = 0; i < ring.size(); i++) {
                    worst = std::max(worst, std::abs(coef[0] * ring[i][0] + coef[1] * ring[i][1] + coef[2] - ys[i]));
                }
                off.push_back(worst);
                for (const auto& t : triangulate(part)) {
                    if (tri_area(t) <= 0) continue;
                    FootprintTri ft{ flags | 0x1000, {}, k };
                    for (int c = 0; c < 3; c++) ft.pts[c] = { t[c][0], coef[0] * t[c][0] + coef[1] * t[c][1] + coef[2], t[c][1] };
                    tris.push_back(ft);
                }
            }
        }
        int leftovers = 0;
        for (const FlatPoly& f : flat) leftovers += (f.flags & 0x1000) ? 1 : 0;
        char line[256];
        snprintf(line, sizeof(line), "spline road: %d segments, %zu footprints from %d spline polygons (planes within "
                 "%.2f ft of the surface), %d flat leftovers", ns, off.size(), spline_polys,
                 off.empty() ? 0.0 : *std::max_element(off.begin(), off.end()), leftovers);
        report.push_back(line);
    }

    // ----------------------------------------------------------------------------------------- crowding
    using P3 = std::array<double, 3>;

    // Snaps corners within `reach` ft to one position (the lowest-numbered of each group, as union-find over
    // cKDTree's query_pairs gives).
    std::vector<FlatPoly> weld(const std::vector<FlatPoly>& polys, double reach = 0.1) {
        std::vector<V3> flat;
        for (const auto& p : polys) flat.insert(flat.end(), p.pts.begin(), p.pts.end());
        std::vector<size_t> parent(flat.size());
        std::iota(parent.begin(), parent.end(), 0);
        std::function<size_t(size_t)> root = [&](size_t a) {
            while (parent[a] != a) parent[a] = parent[parent[a]], a = parent[a];
            return a;
        };
        struct Key {
            int64_t x, y, z;
            bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
        };
        struct KeyHash {
            size_t operator()(const Key& k) const { return size_t(k.x * 73856093 ^ k.y * 19349663 ^ k.z * 83492791); }
        };
        std::unordered_map<Key, std::vector<size_t>, KeyHash> cells;
        auto key = [&](const V3& p) { return Key{ int64_t(std::floor(p[0] / reach)), int64_t(std::floor(p[1] / reach)), int64_t(std::floor(p[2] / reach)) }; };
        for (size_t i = 0; i < flat.size(); i++) cells[key(flat[i])].push_back(i);
        for (size_t i = 0; i < flat.size(); i++) {
            Key k = key(flat[i]);
            for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
                auto it = cells.find({ k.x + dx, k.y + dy, k.z + dz });
                if (it == cells.end()) continue;
                for (size_t j : it->second) {
                    if (j <= i || norm(flat[i] - flat[j]) > reach) continue;
                    size_t ra = root(i), rb = root(j);
                    if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
                }
            }
        }
        std::vector<FlatPoly> out;
        size_t k = 0;
        for (const auto& p : polys) {
            FlatPoly q{ p.flags, {}, p.normal };
            for (size_t c = 0; c < p.pts.size(); c++) q.pts.push_back(flat[root(k + c)]);
            k += p.pts.size();
            out.push_back(std::move(q));
        }
        return out;
    }

    // Whether the polygon's outline (x, z) comes within `m` ft of the square cell.
    bool near_cell(const std::vector<V2>& poly, double x0, double z0, double size, double m) {
        V2 box[4] = { { x0, z0 }, { x0 + size, z0 }, { x0 + size, z0 + size }, { x0, z0 + size } };
        for (const V2& p : poly) {
            if (p[0] >= x0 && p[0] <= x0 + size && p[1] >= z0 && p[1] <= z0 + size) return true;
        }
        auto inside = [&](const V2& q) {
            bool in = false;
            for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                if ((poly[i][1] > q[1]) != (poly[j][1] > q[1]) &&
                    q[0] < (poly[j][0] - poly[i][0]) * (q[1] - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0]) {
                    in = !in;
                }
            }
            return in;
        };
        if (poly.size() >= 3 && inside(box[0])) return true;
        auto seg_dist = [](V2 p, V2 a, V2 b) {
            double dx = b[0] - a[0], dz = b[1] - a[1], l = dx * dx + dz * dz;
            double t = l > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dz) / l, 0.0, 1.0) : 0.0;
            return std::hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dz);
        };
        auto crosses = [](V2 a, V2 b, V2 c, V2 d) {
            auto side = [](V2 p, V2 q, V2 r) { return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0]); };
            return side(a, b, c) * side(a, b, d) <= 0 && side(c, d, a) * side(c, d, b) <= 0;
        };
        for (size_t i = 0; i < poly.size(); i++) {
            V2 a = poly[i], b = poly[(i + 1) % poly.size()];
            for (int e = 0; e < 4; e++) {
                V2 c = box[e], d = box[(e + 1) % 4];
                if (crosses(a, b, c, d)) return true;
                if (std::min({ seg_dist(a, c, d), seg_dist(b, c, d), seg_dist(c, a, b), seg_dist(d, a, b) }) <= m) return true;
            }
        }
        return false;
    }

    // Index cells (4 ft) where more than 36 polygons' outlines (walls widened by 6 ft, the rest by 1) meet:
    // cell -> polygon indices (track_mod.py crowded_cells).
    std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> crowded_cells(const std::vector<FlatPoly>& polys) {
        std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> boxes, out;
        for (size_t i = 0; i < polys.size(); i++) {
            double m = (polys[i].flags & 0xF) == 5 || (polys[i].flags & 0xF) == 6 ? 6.0 : 1.0;
            double xa = 1e300, xb = -1e300, za = 1e300, zb = -1e300;
            for (const V3& p : polys[i].pts) xa = std::min(xa, p[0]), xb = std::max(xb, p[0]), za = std::min(za, p[2]), zb = std::max(zb, p[2]);
            int64_t x0 = int64_t(std::floor((xa - m) / cell_size)), x1 = int64_t(std::floor((xb + m) / cell_size));
            int64_t z0 = int64_t(std::floor((za - m) / cell_size)), z1 = int64_t(std::floor((zb + m) / cell_size));
            if ((x1 - x0 + 1) * (z1 - z0 + 1) > 20000) continue;   // a huge polygon: not counted (as the Python)
            for (int64_t cx = x0; cx <= x1; cx++) for (int64_t cz = z0; cz <= z1; cz++) boxes[{ cx, cz }].push_back(i);
        }
        for (const auto& [cell, ids] : boxes) {
            if (ids.size() <= max_leaf) continue;
            std::vector<size_t> hit;
            for (size_t i : ids) {
                const FlatPoly& f = polys[i];
                std::vector<V2> xz;
                for (const V3& p : f.pts) xz.push_back({ p[0], p[2] });
                double m = (f.flags & 0xF) == 5 || (f.flags & 0xF) == 6 ? 6.0 : 1.0;
                if (std::abs(ring_area(xz)) < 1e-3) m += 0.5;     // shapely buffered a degenerate outline by 0.5 first
                if (near_cell(xz, cell.first * cell_size, cell.second * cell_size, cell_size, m)) hit.push_back(i);
            }
            if (hit.size() > max_leaf) out[cell] = hit;
        }
        return out;
    }

    // The polygon of pa and pb joined across their shared edge (corner lists in order), or empty.
    std::vector<P3> merge_pair(const std::vector<P3>& pa, const std::vector<P3>& pb) {
        auto find = [&](const P3& p) { return std::find(pb.begin(), pb.end(), p); };
        for (size_t k = 0; k < pa.size(); k++) {
            const P3 &u = pa[k], &v = pa[(k + 1) % pa.size()];
            if (find(u) == pb.end() || find(v) == pb.end()) continue;
            size_t m = size_t(find(v) - pb.begin());
            if (pb[(m + 1) % pb.size()] != u) continue;
            std::vector<P3> out(pa.begin(), pa.begin() + k + 1);
            for (size_t q = 0; q + 2 < pb.size(); q++) out.push_back(pb[(m + 2 + q) % pb.size()]);
            out.insert(out.end(), pa.begin() + k + 1, pa.end());
            return out;
        }
        return {};
    }

    bool convex(const std::vector<P3>& P, const V3& nrm) {
        int sign = 0;
        for (size_t k = 0; k < P.size(); k++) {
            const V3 &a = P[k], &b = P[(k + 1) % P.size()], &c = P[(k + 2) % P.size()];
            double v = dot(cross(b - a, c - b), nrm);
            if (std::abs(v) < 1e-6) continue;
            int s = v > 0 ? 1 : -1;
            if (sign && s != sign) return false;
            sign = s;
        }
        return true;
    }

    // track_mod.py relieve_crowding: where a spot has more collision polygons than an index cell holds, joins
    // neighbouring polygons of a surface into flat convex polygons of at most 8 corners.
    std::vector<FlatPoly> relieve_crowding(const std::vector<FlatPoly>& input, std::vector<std::string>& report,
                                           double tolerance = 1.0) {
        std::vector<FlatPoly> polys = weld(input);
        int total = 0;
        double worst = 0.0;
        std::vector<double> steps(6, tolerance);
        for (int i = 0; i < 3; i++) steps.push_back(tolerance * 2);
        for (int i = 0; i < 3; i++) steps.push_back(tolerance * 3);
        for (double tol : steps) {
            auto cells = crowded_cells(polys);
            if (cells.empty()) break;
            std::set<size_t> crowd;
            for (const auto& [c, ids] : cells) crowd.insert(ids.begin(), ids.end());
            // Edges in first-seen order (a Python dict keyed by the corner pair).
            std::map<std::pair<P3, P3>, size_t> edge_index;
            std::vector<std::vector<size_t>> edge_polys;
            std::vector<std::pair<P3, P3>> edge_keys;
            for (size_t i = 0; i < polys.size(); i++) {
                const auto& pts = polys[i].pts;
                for (size_t k = 0; k < pts.size(); k++) {
                    P3 a = pts[k], b = pts[(k + 1) % pts.size()];
                    auto key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
                    auto [it, fresh] = edge_index.emplace(key, edge_polys.size());
                    if (fresh) edge_polys.emplace_back(), edge_keys.push_back(key);
                    edge_polys[it->second].push_back(i);
                }
            }
            struct Cand {
                double d;
                size_t a, b;
                std::vector<std::pair<P3, double>> devs;
                V3 nrm;
            };
            std::vector<Cand> cands;
            for (size_t e = 0; e < edge_polys.size(); e++) {
                const auto& ps = edge_polys[e];
                if (ps.size() != 2 || (polys[ps[0]].flags & 0xF) != (polys[ps[1]].flags & 0xF)) continue;
                if (!crowd.count(ps[0]) && !crowd.count(ps[1])) continue;
                size_t a = ps[0], b = ps[1];
                const auto& A = polys[a].pts;
                V3 nrm = unit(cross(A[1] - A[0], A[2] - A[0]));
                std::vector<std::pair<P3, double>> devs;
                double d = 0;
                for (const P3& p : polys[b].pts) {
                    if (p == edge_keys[e].first || p == edge_keys[e].second) continue;
                    if (std::any_of(devs.begin(), devs.end(), [&](const auto& x) { return x.first == p; })) continue;
                    double v = dot(p - A[0], nrm);
                    devs.push_back({ p, v });
                    d = std::max(d, std::abs(v));
                }
                if (d < tol) cands.push_back({ d, a, b, std::move(devs), nrm });
            }
            std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.d < y.d; });
            std::map<P3, P3> moved;
            std::set<size_t> locked, gone;
            std::map<size_t, std::vector<P3>> joined;
            for (const Cand& c : cands) {
                if (locked.count(c.a) || locked.count(c.b)) continue;
                if (std::any_of(c.devs.begin(), c.devs.end(), [&](const auto& x) { return moved.count(x.first) > 0; })) continue;
                std::map<P3, P3> new_pos;
                for (const auto& [p, dv] : c.devs) new_pos[p] = p - c.nrm * dv;
                std::vector<P3> m = merge_pair(polys[c.a].pts, polys[c.b].pts);
                if (m.empty() || m.size() > 8) continue;
                std::vector<P3> moved_m;
                for (const P3& p : m) moved_m.push_back(new_pos.count(p) ? new_pos[p] : p);
                if (!convex(moved_m, c.nrm)) continue;
                for (const auto& [p, q] : new_pos) moved[p] = q;
                locked.insert(c.a);
                locked.insert(c.b);
                joined[c.a] = m;
                gone.insert(c.b);
                worst = std::max(worst, c.d);
            }
            if (joined.empty()) continue;
            total += int(joined.size());
            std::vector<FlatPoly> next;
            for (size_t i = 0; i < polys.size(); i++) {
                if (gone.count(i)) continue;
                const auto& pts = joined.count(i) ? joined[i] : polys[i].pts;
                FlatPoly q{ polys[i].flags, {}, polys[i].normal };
                for (const P3& p : pts) q.pts.push_back(moved.count(p) ? moved[p] : p);
                next.push_back(std::move(q));
            }
            polys = std::move(next);
        }
        char line[256];
        if (total) {
            snprintf(line, sizeof(line), "collision: %d polygon pairs in spots over the game's %zu polygons per cell "
                     "joined flat (corners moved %.2f ft at most)", total, max_leaf, worst);
            report.push_back(line);
        }
        auto left = crowded_cells(polys);
        if (!left.empty()) {
            size_t most = 0;
            for (const auto& [c, ids] : left) most = std::max(most, ids.size());
            snprintf(line, sizeof(line), "collision: still %zu spots over %zu polygons (worst %zu)", left.size(), max_leaf, most);
            report.push_back(line);
        }
        return polys;
    }

    // Half width (ft) of the drivable surface either side of `point`, from the road/dirt collision seen from above.
    struct Tri2D {
        V2 a, b, c;
        double y;
    };
    std::array<double, 2> road_half_width(const std::vector<const Tri2D*>& tris, V2 point, V2 tangent, double height,
                                          double reach = 25.0) {
        V2 lat{ -tangent[1], tangent[0] };
        auto on_road = [&](V2 q) {
            for (const Tri2D* t : tris) {
                if (std::abs(t->y - height) > reach) continue;
                V2 v0{ t->c[0] - t->a[0], t->c[1] - t->a[1] }, v1{ t->b[0] - t->a[0], t->b[1] - t->a[1] };
                V2 v2{ q[0] - t->a[0], q[1] - t->a[1] };
                double d00 = v0[0] * v0[0] + v0[1] * v0[1], d01 = v0[0] * v1[0] + v0[1] * v1[1];
                double d11 = v1[0] * v1[0] + v1[1] * v1[1], d20 = v2[0] * v0[0] + v2[1] * v0[1];
                double d21 = v2[0] * v1[0] + v2[1] * v1[1];
                double den = d00 * d11 - d01 * d01;
                if (std::abs(den) < 1e-9) continue;
                double u = (d11 * d20 - d01 * d21) / den, v = (d00 * d21 - d01 * d20) / den;
                if (u >= -1e-6 && v >= -1e-6 && u + v <= 1 + 1e-6) return true;
            }
            return false;
        };
        std::array<double, 2> sides{};
        int i = 0;
        for (int s : { 1, -1 }) {
            double d = 0.0;
            while (d < 200 && on_road({ point[0] + s * (d + 2) * lat[0], point[1] + s * (d + 2) * lat[1] })) d += 2;
            sides[i++] = d;
        }
        return sides;
    }
}

bool a49::build_track_mod(const std::filesystem::path& data_dir, const std::filesystem::path& assets_dir, ModFiles& out,
                          std::string& log, const std::function<bool()>& cancelled) {
    const std::string track_id = "alcatraz", name = "Alcatraz";
    std::vector<std::string> report;
    std::string error;
    Track t;
    if (!t.open(data_dir / "TRACKS" / "TRACK4", error)) {
        log += error + "\n";
        return false;
    }
    std::vector<WorldEntry> worlds;
    std::vector<DobsEntry> dobs;
    std::vector<CollisionPoly> collision;
    std::vector<std::array<double, 33>> seg_records;
    std::vector<PathTable> tables;
    if (!t.worlds(worlds, error) || !t.dobs(dobs, error) || !t.collision(collision, error) ||
        !t.segments(seg_records, error) || !t.path_tables(tables, error)) {
        log += error + "\n";
        return false;
    }
    Glb g("rush2-recomp src/arcade2049 (tools/arcade2049/track_mod.py)");

    // Materials: one per arcade texture, made on first use.
    std::map<int, int> mats;
    const std::vector<uint8_t>& texels = t.texels();
    auto material = [&](int ti) {
        auto it = mats.find(ti);
        if (it != mats.end()) return it->second;
        const Texture& tex = t.textures[ti];
        std::vector<uint8_t> rgba;
        if (!tex.rgba(texels, rgba)) rgba.assign(size_t(std::max(tex.w, 1)) * std::max(tex.h, 1) * 4, 255);
        int w = std::max(tex.w, 1), h = std::max(tex.h, 1);
        std::string mode = "OPAQUE";
        if (tex.fmt == fmt_ayiq8422) {
            size_t soft = 0;
            for (size_t k = 3; k < rgba.size(); k += 4) soft += rgba[k] > 16 && rgba[k] < 240;
            mode = double(soft) / (rgba.size() / 4) > 0.05 ? "BLEND" : "MASK";
        }
        std::array<int, 2> wrap{ tex.clamp_s() ? 33071 : 10497, tex.clamp_t() ? 33071 : 10497 };
        int m = g.material(tex.name, g.image(tex.name, png(rgba, w, h)), mode, true, wrap);
        mats[ti] = m;
        return m;
    };

    std::set<std::string> sky_names;
    for (const Object& o : t.objects) {
        for (const auto& tr : o.lod0.tris) {
            if (tr[3] >= t.textures.size()) continue;
            for (const char* s : sky_textures) {
                if (starts_with(t.textures[tr[3]].name, s)) sky_names.insert(o.name);
            }
        }
    }
    auto is_water_texture = [&](int ti) { return ti < int(t.textures.size()) && t.textures[ti].name == "WATER2"; };
    size_t tri_count = 0;

    // lod's triangles at the world points V into target; returns the count.
    auto add_tris = [&](PrimSet& target, const Lod& lod, const std::vector<V3>& V, double limit) {
        size_t count = 0;
        for (const auto& tr : lod.tris) {
            int ti = tr[3];
            if (ti >= int(t.textures.size())) continue;
            Glb::Prim& p = target[material(ti)];
            V2 s = t.textures[ti].uv_scale();
            Tri corner;
            int order[3] = { tr[0], tr[2], tr[1] };   // mirroring x (to_gltf) flips the winding
            bool bad = false;
            for (int c = 0; c < 3; c++) {
                int k = order[c];
                if (k >= int(lod.verts.size())) { bad = true; break; }
                const Vertex& v = lod.verts[k];
                corner[c] = { V[k], { v.st[0] / s[0], v.st[1] / s[1] },
                              { v.rgba[0] / 255.0, v.rgba[1] / 255.0, v.rgba[2] / 255.0, v.rgba[3] / 255.0 } };
            }
            if (bad) continue;
            for (const Tri& tri : split_large(corner, limit)) {
                V3 gl[3];
                for (int c = 0; c < 3; c++) gl[c] = to_gltf(tri[c].p);
                V3 n = unit(cross(gl[1] - gl[0], gl[2] - gl[0]));
                uint32_t base = uint32_t(p.verts.size());
                for (int c = 0; c < 3; c++) {
                    p.verts.push_back(gl[c]);
                    p.normals.push_back(n);
                    p.uvs.push_back(tri[c].uv);
                    p.colors.push_back(tri[c].col);
                }
                p.tris.push_back({ base, base + 1, base + 2 });
                count++;
            }
        }
        return count;
    };
    auto placed = [](const Lod& lod, const M3& M, const V3& pos) {
        std::vector<V3> V;
        for (const Vertex& v : lod.verts) V.push_back(mul(v.pos, M) + pos);
        return V;
    };

    // Every placed object: worlds.rom instances, then dobs.rom objects (their placements are (x, z, y) of world and
    // their matrices' rows where the object's axes go: P M P with P swapping y and z).
    struct Instance {
        const Object* o;
        M3 M;
        V3 pos;
        std::vector<std::string> script;
    };
    std::vector<Instance> instances;
    for (const WorldEntry& w : worlds) {
        const Object* o = t.object_named(w.name);
        if (!o) { report.push_back("worlds.rom: " + w.name + " names no object, left out"); continue; }
        instances.push_back({ o, w.m, w.pos, {} });
    }
    for (const DobsEntry& d : dobs) {
        const Object* o = t.object_named(d.name);
        if (!o) { report.push_back("dobs.rom: " + d.name + " names no object, left out"); continue; }
        const M3& m = d.m;
        M3 M = { m[0], m[2], m[1], m[6], m[8], m[7], m[3], m[5], m[4] };
        instances.push_back({ o, M, to_world_dobs(d.pos), d.script });
    }

    PrimSet scenery, sky;
    int billboards = 0, knocks = 0;
    struct Mover {
        std::string name;
        const Lod* lod;
        std::vector<V3> local;
        V3 pos;
        std::vector<double> times;
        std::vector<std::array<double, 4>> quats;
        std::vector<V3> scales;
    };
    std::vector<Mover> movers;
    std::set<std::string> still;
    for (const Instance& in : instances) {
        if (cancelled && cancelled()) return false;
        std::set<std::string> words;
        for (const std::string& s : in.script) {
            auto w = split_words(s);
            if (!w.empty()) words.insert(lower(w[0]));
        }
        const Lod& lod = in.o->lod0;
        std::vector<V3> local;
        for (const Vertex& v : lod.verts) local.push_back(mul(v.pos, in.M));
        if (words.count("coin") || starts_with(upper(in.o->name), "GOLDCOIN")) {
            still.insert(in.o->name + " (a coin pickup: left out)");
            continue;
        }
        bool knock = words.count("bounce") > 0, card = words.count("frontface") > 0;
        if (knock || card) {
            PrimSet prims;
            add_tris(prims, lod, local, 0.0);
            std::vector<Glb::Prim> list = prims.list();
            if (card) {
                // A card the arcade turns to the camera: kept upright facing glTF +z (half a turn about y if not).
                double nz = 0;
                for (const auto& p : list) for (const V3& n : p.normals) nz += n[2];
                if (nz < 0) {
                    for (auto& p : list) {
                        for (V3& v : p.verts) v = { -v[0], v[1], -v[2] };
                        for (V3& n : p.normals) n = { -n[0], n[1], -n[2] };
                    }
                }
            }
            char suffix[16];
            snprintf(suffix, sizeof(suffix), ".%03d", billboards);
            std::string node_name = (knock ? "PROP_KNOCK_" + in.o->name : "BILLBOARD_" + in.o->name) + suffix;
            Glb::Trs trs;
            trs.translation = to_gltf(in.pos);
            int node = g.textured_mesh(node_name, list, trs);
            if (knock && card) g.js["nodes"][node]["extras"] = { { "billboard", true } };
            billboards++;
            tri_count += lod.tris.size();
            if (words.count("flipbook")) still.insert(in.o->name + " (a flipbook billboard: its first frame)");
            if (knock) knocks++;
            continue;
        }
        if (words.count("hide")) {
            still.insert(in.o->name + " (hidden until a script shows it: left out)");
            continue;
        }
        std::vector<V3> V = placed(lod, in.M, in.pos);
        bool is_water = std::all_of(lod.tris.begin(), lod.tris.end(), [&](const auto& tr) { return is_water_texture(tr[3]); });
        double reach = 0;
        for (const V3& p : V) reach = std::max(reach, std::hypot(p[0], p[2]));
        bool far = reach > sky_radius;
        bool is_sky = sky_names.count(in.o->name) || in.o->name.find("CLOUDS") != std::string::npos || (far && !is_water);
        if (is_water && far) add_tris(sky, lod, V, 0.0);
        std::vector<std::vector<std::string>> spin, flip;
        for (const std::string& s : in.script) {
            std::string l = lower(s);
            if (starts_with(l, "spinx") || starts_with(l, "spiny") || starts_with(l, "spinz")) spin.push_back(split_words(s));
            if (starts_with(l, "flipbook")) flip.push_back(split_words(s));
        }
        if (!is_sky && !spin.empty()) {
            // spin<axis> <degrees per second>: a looping MOVE_ turn about the object's own axis (the script's x, y,
            // z are world x, z, y; the x mirror flips the axis's x and reverses the turn).
            char ax = char(std::tolower((unsigned char)spin[0][0].back()));
            V3 axis = ax == 'x' ? V3{ 1, 0, 0 } : ax == 'y' ? V3{ 0, 0, 1 } : V3{ 0, 1, 0 };
            axis = mul(axis, in.M);
            axis = V3{ -axis[0], axis[1], axis[2] } * (1.0 / norm(axis));
            double rate = spin[0].size() > 1 ? std::strtod(spin[0][1].c_str(), nullptr) : 0.0;
            if (rate != 0) {
                double period = 360.0 / std::abs(rate);
                Mover mv{ in.o->name + "_" + std::to_string(movers.size()), &lod, local, in.pos };
                for (int k = 0; k < 5; k++) {
                    double a = -std::copysign(1.0, rate) * 3.14159265358979323846 * k / 2;
                    mv.times.push_back(period * k / 4);
                    mv.quats.push_back({ axis[0] * std::sin(a / 2), axis[1] * std::sin(a / 2), axis[2] * std::sin(a / 2), std::cos(a / 2) });
                }
                movers.push_back(std::move(mv));
                tri_count += lod.tris.size();
                continue;
            }
        }
        if (!is_sky && !flip.empty() && flip[0].size() > 1 && std::strtod(flip[0][1].c_str(), nullptr) > 0) {
            // flipbook <frames per second>: frames <name>_1..n, each a MOVE_ object at full size only in its slot.
            std::string base = in.o->name;
            size_t us = base.rfind('_');
            if (us != std::string::npos && us + 1 < base.size() &&
                std::all_of(base.begin() + us + 1, base.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
                base = base.substr(0, us);
            }
            std::vector<const Object*> frames;
            while (const Object* f = t.find(base + "_" + std::to_string(frames.size() + 1))) frames.push_back(f);
            if (frames.size() > 1) {
                double fps = std::strtod(flip[0][1].c_str(), nullptr);
                double period = frames.size() / fps, eps = 0.002;
                // One pivot for the whole book, on a whole foot (frames share it and one interpolation ID).
                V3 sum{};
                size_t count = 0;
                for (const Object* f : frames) for (const Vertex& v : f->lod0.verts) sum = sum + mul(v.pos, in.M), count++;
                V3 mean = count ? sum * (1.0 / count) : V3{};
                V3 pivot{ round_even(mean[0] + in.pos[0]), round_even(mean[1] + in.pos[1]), round_even(mean[2] + in.pos[2]) };
                for (size_t j = 0; j < frames.size(); j++) {
                    double s0 = j / fps, s1 = (j + 1) / fps;
                    std::vector<std::pair<double, bool>> keys{ { 0.0, j == 0 } };
                    if (j) keys.push_back({ s0 - eps, false }), keys.push_back({ s0, true });
                    keys.push_back({ s1 - eps, true });
                    keys.push_back({ s1, false });
                    // Every frame's keys end on key 0's pose so the loop closes in exactly `period`.
                    if (j == 0) keys.push_back({ period - eps, false }), keys.push_back({ period, true });
                    else if (s1 < period - eps) keys.push_back({ period, false });
                    Mover mv{ frames[j]->name + "_" + std::to_string(movers.size()), &frames[j]->lod0, {}, pivot };
                    for (const Vertex& v : frames[j]->lod0.verts) mv.local.push_back(mul(v.pos, in.M) + in.pos - pivot);
                    for (const auto& [time, on] : keys) {
                        mv.times.push_back(time);
                        mv.scales.push_back(on ? V3{ 1, 1, 1 } : V3{ 0.001, 0.001, 0.001 });
                    }
                    tri_count += frames[j]->lod0.tris.size();
                    movers.push_back(std::move(mv));
                }
                continue;
            }
        }
        if (!flip.empty() || !spin.empty()) {
            still.insert(in.o->name + (is_sky ? " (sky: it stays still)" : " (an event flipbook: its first frame)"));
        }
        size_t n = add_tris(is_sky ? sky : scenery, lod, V, is_sky ? 0.0 : max_edge);
        tri_count += is_sky ? 0 : n;
    }
    g.textured_mesh("TRACK", scenery.list());
    for (const Mover& mv : movers) {
        PrimSet prims;
        add_tris(prims, *mv.lod, mv.local, 0.0);
        Glb::Trs trs;
        trs.translation = to_gltf(mv.pos);
        int node = g.textured_mesh("MOVE_" + mv.name, prims.list(), trs);
        g.animate(mv.name, node, mv.times, mv.quats.empty() ? nullptr : &mv.quats, mv.scales.empty() ? nullptr : &mv.scales);
    }
    if (!movers.empty()) {
        report.push_back(std::to_string(movers.size()) + " moving objects (MOVE_): spinning lights and lighthouse beams, flipbook frames");
    }
    for (const std::string& s : still) report.push_back("still: " + s);
    if (!sky.empty()) {
        // Drawn without depth in material order: the dome, the far water over it, the land on the water, the clouds.
        std::vector<Glb::Prim> list = sky.list();
        auto layer = [&](const Glb::Prim& p) {
            std::string n = g.js["materials"][p.material]["name"];
            return upper(n).find("CLOUD") != std::string::npos ? 3 : starts_with(n, "GS_APANS") ? 2 : n == "WATER2" ? 1 : 0;
        };
        std::stable_sort(list.begin(), list.end(), [&](const auto& a, const auto& b) { return layer(a) < layer(b); });
        g.textured_mesh("SKY", list);
    }
    if (cancelled && cancelled()) return false;

    // Collision.
    std::vector<FootprintTri> spline_tris;
    std::vector<FlatPoly> flat;
    spline_road(seg_records, collision, spline_tris, flat, report);
    if (cancelled && cancelled()) return false;
    struct ColPoly {
        uint32_t flags;
        std::vector<V3> pts;
        V3 normal;
        int seg;
    };
    std::vector<ColPoly> col_polys;
    for (FlatPoly& f : relieve_crowding(flat, report)) col_polys.push_back({ f.flags, std::move(f.pts), f.normal, -1 });
    for (const FootprintTri& ft : spline_tris) {
        V3 up = cross(ft.pts[1] - ft.pts[0], ft.pts[2] - ft.pts[0]);
        if (up[1] <= 0) up = up * -1.0;     // footprints face up
        col_polys.push_back({ ft.flags, { ft.pts[0], ft.pts[1], ft.pts[2] }, up, ft.seg });
    }
    struct ColMesh {
        std::vector<V3> verts, normals;
        std::vector<std::array<uint32_t, 3>> tris;
        std::vector<double> segs;
    };
    std::map<std::string, ColMesh> by_type;
    std::vector<Tri2D> road2d;
    int up_road = 0, road_count = 0;
    std::set<int> unknown_types;
    auto add_col_tri = [&](const std::string& surface, std::array<V3, 3> tri, const V3& normal, int seg) {
        // The converter's front (game space) is -(v1-v0)x(v2-v0); keep it on the polygon's normal side.
        if (dot(cross(tri[1] - tri[0], tri[2] - tri[0]), normal) > 0) std::swap(tri[1], tri[2]);
        ColMesh& m = by_type[surface];
        uint32_t base = uint32_t(m.verts.size());
        for (const V3& p : tri) {
            m.verts.push_back(to_gltf(p));
            m.normals.push_back({ -normal[0], normal[1], normal[2] });
            m.segs.push_back(seg);
        }
        m.tris.push_back({ base, base + 1, base + 2 });
    };
    for (const ColPoly& cp : col_polys) {
        V3 normal = unit(cp.normal);
        int ty = cp.flags & 0xF;
        const char* surface = surface_name(ty);
        if (!surface) {
            unknown_types.insert(ty);
            surface = "WALL";
        }
        if (ty == 0) road_count++, up_road += normal[1] > 0.3;
        for (size_t k = 1; k + 1 < cp.pts.size(); k++) {
            std::array<V3, 3> tri{ cp.pts[0], cp.pts[k], cp.pts[k + 1] };
            if (ty <= 2) {
                road2d.push_back({ { tri[0][0], tri[0][2] }, { tri[1][0], tri[1][2] }, { tri[2][0], tri[2][2] },
                                   (tri[0][1] + tri[1][1] + tri[2][1]) / 3 });
            }
            add_col_tri(surface, tri, normal, cp.seg);
        }
    }
    for (int ty : unknown_types) report.push_back("collision type " + std::to_string(ty) + ": made WALL");
    // The bay: the water sheets become COL_WATER, which wrecks and resets the car as Rush 2's own Alcatraz water does.
    int sheets = 0;
    for (const WorldEntry& w : worlds) {
        const Object* o = t.object_named(w.name);
        if (!o) continue;
        for (const auto& tr : o->lod0.tris) {
            if (!is_water_texture(tr[3])) continue;
            Tri corners;
            for (int c = 0; c < 3; c++) corners[c] = { o->lod0.verts[tr[c]].pos + w.pos, {}, {} };
            for (const Tri& part : split_large(corners, water_edge)) {
                std::array<V3, 3> p{ part[0].p, part[1].p, part[2].p };
                V3 normal = unit(cross(p[1] - p[0], p[2] - p[0]));
                if (normal[1] < 0) normal = normal * -1.0;
                add_col_tri("WATER", p, normal, -1);
                sheets++;
            }
        }
    }
    if (sheets) report.push_back("water: " + std::to_string(sheets) + " collision triangles from the water sheets");
    size_t collision_tris = 0;
    for (const auto& [surface, m] : by_type) {
        bool segs = std::any_of(m.segs.begin(), m.segs.end(), [](double s) { return s >= 0; });
        g.mesh("COL_" + surface + "_arcade", m.verts, m.normals, m.tris,
               segs ? std::vector<std::pair<std::string, std::vector<double>>>{ { "_SEGMENT", m.segs } }
                    : std::vector<std::pair<std::string, std::vector<double>>>{});
        collision_tris += m.tris.size();
    }
    // The spline road's segments as the game reads them (unused fields of some records are NaN in the arcade data).
    int splines = g.empty("ROAD_SPLINES");
    nlohmann::json segments = nlohmann::json::array();
    for (const auto& r : seg_records) {
        nlohmann::json row = nlohmann::json::array();
        for (double v : r) row.push_back(std::isfinite(v) ? v : 0.0);
        segments.push_back(row);
    }
    g.js["nodes"][splines]["extras"] = { { "segments", segments } };
    report.push_back("road polygon normals: " + std::to_string(up_road) + " of " + std::to_string(road_count) + " point up");

    // The drones' lap and the gates.
    const PathTable& t0 = tables[0];
    size_t n = std::min<size_t>(t0.lap_count, t0.records.size());
    if (n < 3) {
        log += "dpath.rom: no lap\n";
        return false;
    }
    std::vector<V3> lap;
    for (size_t i = 0; i < n; i++) lap.push_back(t0.records[i].pos);
    auto tangent = [&](size_t i) {
        V3 d = lap[(i + 1) % n] - lap[(i + n - 1) % n];
        double l = std::hypot(d[0], d[2]);
        if (l == 0) l = 1.0;
        return V2{ d[0] / l, d[2] / l };
    };
    // Road triangles near each lap point (within 260 ft of their centers), from a grid.
    constexpr double grid_cell = 260.0;
    std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> road_grid;
    std::vector<V2> centers;
    for (size_t k = 0; k < road2d.size(); k++) {
        const Tri2D& r = road2d[k];
        V2 c{ (r.a[0] + r.b[0] + r.c[0]) / 3, (r.a[1] + r.b[1] + r.c[1]) / 3 };
        centers.push_back(c);
        road_grid[{ int64_t(std::floor(c[0] / grid_cell)), int64_t(std::floor(c[1] / grid_cell)) }].push_back(k);
    }
    auto near_tris = [&](size_t i) {
        std::vector<const Tri2D*> out;
        int64_t gx = int64_t(std::floor(lap[i][0] / grid_cell)), gz = int64_t(std::floor(lap[i][2] / grid_cell));
        for (int64_t dx = -1; dx <= 1; dx++) for (int64_t dz = -1; dz <= 1; dz++) {
            auto it = road_grid.find({ gx + dx, gz + dz });
            if (it == road_grid.end()) continue;
            for (size_t k : it->second) {
                if (std::hypot(centers[k][0] - lap[i][0], centers[k][1] - lap[i][2]) <= grid_cell) out.push_back(&road2d[k]);
            }
        }
        return out;
    };
    std::vector<std::array<double, 2>> half(n);
    for (size_t i = 0; i < n; i++) {
        auto tris = near_tris(i);
        half[i] = road_half_width(tris, { lap[i][0], lap[i][2] }, tangent(i), lap[i][1]);
        if (half[i][0] + half[i][1] == 0) {   // over a jump: the road below
            half[i] = road_half_width(tris, { lap[i][0], lap[i][2] }, tangent(i), lap[i][1], 90.0);
        }
    }
    auto nearest = [&](const V3& p) {
        size_t best = 0;
        double bd = 1e300;
        for (size_t i = 0; i < n; i++) {
            double d = norm(lap[i] - p);
            if (d < bd) bd = d, best = i;
        }
        return best;
    };
    std::vector<std::pair<std::string, size_t>> gates;
    auto finish = std::find_if(dobs.begin(), dobs.end(), [](const DobsEntry& d) { return d.name == "FINISHSIGN"; });
    if (finish != dobs.end()) gates.push_back({ "FINISH", nearest(to_world_dobs(finish->pos)) });
    else {
        report.push_back("no FINISHSIGN: the finish is the lap's first point");
        gates.push_back({ "FINISH", 0 });
    }
    std::vector<size_t> banners;
    for (const WorldEntry& w : worlds) {
        const Object* o = t.find(w.name);
        if (!o) continue;
        V3 sum{};
        size_t count = 0;
        for (const auto& tr : o->lod0.tris) {
            if (tr[3] >= t.textures.size() || t.textures[tr[3]].name != "MG_CHECK5") continue;
            for (int c = 0; c < 3; c++) sum = sum + o->lod0.verts[tr[c]].pos, count++;
        }
        if (count) banners.push_back(nearest(sum * (1.0 / count) + w.pos));
    }
    size_t f0 = gates[0].second;
    std::sort(banners.begin(), banners.end());
    banners.erase(std::unique(banners.begin(), banners.end()), banners.end());
    std::stable_sort(banners.begin(), banners.end(), [&](size_t a, size_t b) { return (a + n - f0) % n < (b + n - f0) % n; });
    for (size_t k = 0; k < banners.size(); k++) gates.push_back({ "CHECKPOINT_" + std::to_string(k + 1), banners[k] });
    for (const auto& [gname, i] : gates) {
        V2 tg = tangent(i);
        g.empty(gname, gate_matrix(lap[i], { tg[0], 0.0, tg[1] }, std::max({ half[i][0], half[i][1], 30.0 }) + 10.0));
    }

    // AI: table 0's lap is AIPATH_main; the racing lines that run on this course are AIPATH_lane1-4 left to right,
    // each with the arcade's speed (_SPEED, Rush 2 mph) and steering behaviour (_AI_MODE) per point.
    auto ai_attrs = [](const std::vector<PathRecord>& recs) {
        std::vector<double> speed, mode;
        for (const PathRecord& r : recs) speed.push_back(r.speed * 60.0 / 88.0 / lane_map), mode.push_back(r.kind);
        return std::vector<std::pair<std::string, std::vector<double>>>{ { "_SPEED", speed }, { "_AI_MODE", mode } };
    };
    auto ribbon = [&](const std::string& rname, const std::vector<V3>& pts, const std::vector<double>& widths,
                      const std::vector<std::pair<std::string, std::vector<double>>>& attrs) {
        std::vector<V3> verts, nrms;
        std::vector<std::array<uint32_t, 3>> tris;
        size_t m = pts.size();
        std::vector<std::pair<std::string, std::vector<double>>> at;
        for (const auto& [k, v] : attrs) at.push_back({ k, {} });
        for (size_t i = 0; i < m; i++) {
            V3 d = pts[(i + 1) % m] - pts[(i + m - 1) % m];
            double l = std::hypot(d[0], d[2]);
            if (l == 0) l = 1.0;
            V3 lat{ -d[2] / l, 0.0, d[0] / l };
            verts.push_back(to_gltf(pts[i] - lat * widths[i]));
            verts.push_back(to_gltf(pts[i] + lat * widths[i]));
            nrms.push_back({ 0, 1, 0 });
            nrms.push_back({ 0, 1, 0 });
            for (size_t a = 0; a < attrs.size(); a++) at[a].second.push_back(attrs[a].second[i]), at[a].second.push_back(attrs[a].second[i]);
        }
        for (size_t i = 0; i < m; i++) {
            uint32_t j = uint32_t((i + 1) % m), ii = uint32_t(i);
            tris.push_back({ 2 * ii, 2 * ii + 1, 2 * j });
            tris.push_back({ 2 * ii + 1, 2 * j + 1, 2 * j });
        }
        return g.mesh(rname, verts, nrms, tris, at);
    };
    std::vector<double> widths;
    for (const auto& h : half) widths.push_back(std::max(15.0, std::min({ h[0], h[1], 80.0 })) * 0.9);
    std::vector<PathRecord> lap_recs(t0.records.begin(), t0.records.begin() + n);
    int main_node = ribbon("AIPATH_main", lap, widths, ai_attrs(lap_recs));
    // The arcade lap is at road height except over its jumps, where it is the cars' arc: keep it there.
    g.js["nodes"][main_node]["extras"] = { { "keep_height", true } };
    struct Line {
        size_t table;
        std::vector<V3> pts;
        std::vector<PathRecord> recs;
    };
    std::vector<Line> lines{ { 0, lap, lap_recs } };
    for (size_t k = 1; k < tables.size(); k++) {
        size_t m = std::min<size_t>(tables[k].lap_count, tables[k].records.size());
        std::vector<PathRecord> recs(tables[k].records.begin(), tables[k].records.begin() + m);
        std::vector<V3> pts;
        for (const PathRecord& r : recs) pts.push_back(r.pos);
        if (pts.size() < 10) continue;
        std::vector<double> d;
        for (const V3& p : pts) d.push_back(norm(lap[nearest(p)] - p));
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        double median = d[d.size() / 2];
        if (d.size() % 2 == 0) {
            double lower_mid = *std::max_element(d.begin(), d.begin() + d.size() / 2);
            median = (median + lower_mid) / 2;
        }
        if (median > 60.0) {
            char line[200];
            snprintf(line, sizeof(line), "dpath table %zu: %zu points, %.0f ft from the course (not a racing line here), left out",
                     k, m, median);
            report.push_back(line);
            continue;
        }
        lines.push_back({ k, std::move(pts), std::move(recs) });
    }
    if (t0.records.size() > n) {
        report.push_back("dpath table 0: " + std::to_string(t0.records.size() - n) +
                         " records past the lap (routes off the course, not raced), left out");
    }
    // Left to right in the race direction: the mean offset along the lap's left-hand lateral.
    auto side = [&](const std::vector<V3>& pts) {
        double sum = 0;
        for (const V3& p : pts) {
            size_t i = nearest(p);
            V2 tg = tangent(i);
            sum += (p[0] - lap[i][0]) * tg[1] + (p[2] - lap[i][2]) * -tg[0];
        }
        return sum / pts.size();
    };
    std::vector<double> sides;
    for (const Line& l : lines) sides.push_back(side(l.pts));
    std::vector<size_t> order(lines.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return -sides[a] < -sides[b]; });
    if (lines.size() > 4) {
        report.push_back(std::to_string(lines.size()) + " racing lines: the game has 4 lane slots, so " +
                         std::to_string(lines.size() - 4) + " are left out");
    }
    // Four slots: the lines spread over them, the middle ones repeated when there are fewer.
    for (int s = 0; s < 4; s++) {
        const Line& l = lines[order[size_t(round_even(s * double(lines.size() - 1) / 3))]];
        ribbon("AIPATH_lane" + std::to_string(s + 1), l.pts, std::vector<double>(l.pts.size(), 3.0), ai_attrs(l.recs));
        log += "  AIPATH_lane" + std::to_string(s + 1) + ": dpath table " + std::to_string(l.table) + " (" +
               std::to_string(l.pts.size()) + " points)\n";
    }

    const std::string dir = "tracks/" + track_id + "/";
    out[dir + "track.glb"] = g.bytes();
    out[dir + "track.toml"] = text_bytes(
        "[track]\nname = \"" + name + "\"\nauthor = \"Rush 2049 arcade (converted)\"\ndescription = \"" + name +
        " from San Francisco Rush 2049 Special Edition (arcade)\"\ngravity = \"rush2049\"\n"
        // The arcade's road is built for its own flat-faced collision: no smoothing (docs/custom_tracks.md#smooth-collision).
        "smooth_collision = false\n\n[look]\nfog_color = "
        "\"#0c1424\"\n\n[menu]\nlogo = \"logo.png\"\nafter = \"rush2049\"\npreview = \"preview.png\"\n");
    for (const char* file : { "logo.png", "preview.png" }) {
        std::vector<uint8_t> bytes;
        std::ifstream f(assets_dir / (track_id + "_" + file), std::ios::binary);
        if (!f) {
            log += "missing " + rush2::upscale::path_utf8(assets_dir / (track_id + "_" + file)) + "\n";
            return false;
        }
        bytes.assign(std::istreambuf_iterator<char>(f), {});
        out[dir + file] = std::move(bytes);
    }
    out["mod.json"] = text_bytes(nlohmann::json{
        { "game_id", "rush2" }, { "id", track_mod_id }, { "display_name", "Rush 2049 Arcade Tracks" },
        { "description", "Tracks from San Francisco Rush 2049 Special Edition (arcade), built from the arcade data by the "
                         "Games tab." },
        { "short_description", "Rush 2049 arcade tracks" }, { "version", "1.0.0" }, { "authors", { "rush2-recomp" } },
        { "minimum_recomp_version", "0.0.0" } }.dump(4));

    char line[256];
    snprintf(line, sizeof(line), "wrote %s: %zu scenery triangles, %zu textures, %zu collision triangles, %zu gates, lap of %zu points\n",
             dir.c_str(), tri_count, mats.size(), collision_tris, gates.size(), n);
    log += line;
    for (const auto& [gname, i] : gates) {
        snprintf(line, sizeof(line), "  %s at lap point %zu, road half widths %.0f / %.0f ft\n", gname.c_str(), i, half[i][0], half[i][1]);
        log += line;
    }
    if (knocks) log += "props: KNOCK " + std::to_string(knocks) + "\n";
    for (const std::string& r : report) log += r + "\n";
    return true;
}
