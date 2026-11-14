// Closes the cracks in the SF Rush arcade (Rush The Rock) scenery: missing curb faces, near-miss seams and
// T-junctions in the arcade's own models (docs/sfrush_arcade.md#cracks-and-curbs). Run on the TRACK mesh by
// arcade_track.cpp build_one for the Rock tracks only; the Rush 2049 arcade tracks are left as they are.
//
// Steps, on the opaque triangles (cutout and blended textures are left alone: their edges are open on purpose):
//   1. Curbs: two open edges of ground faces (normal within 45 degrees of up) on the same line seen from above, on
//      opposite sides of it, one 0.04-1 ft over the other, get a vertical face between them over the stretch they
//      share, with the upper face's texture (Embarcadero's sidewalks rise 0.5 ft over the road at the overpass with
//      nothing under their edge: the sky showed through).
//   2. Snap: an open corner within `seam` of another open corner moves onto it, unless the two are corners of one
//      triangle (then only within `tjunc`: a thin strip or a pole's open end keeps its width).
//   3. T-junctions: an open edge with an open corner lying on it (within `tjunc`, or within `seam` when the corner is
//      outside the edge's triangle and has an open edge of its own running alongside) is split there, the corner's
//      position taken, so the two sides
//      share their edges. A split triangle is redrawn as a fan around its center.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <numeric>
#include <unordered_map>

#include "arcade_internal.h"

namespace a49 = rush2::arcade2049;
using namespace a49;

namespace {
    constexpr double seam = 0.2;        // ft: gaps closed by snapping or splitting
    constexpr double tjunc = 0.04;      // ft: corners this close are one corner, or on the edge, whatever their faces
    constexpr double curb_min = 0.04;   // ft: smaller height steps are left to the seam closing
    constexpr double curb_max = 1.0;    // ft: taller steps are not curbs
    constexpr double curb_line = 0.05;  // ft: the two edges' distance apart seen from above
    constexpr double curb_len = 0.5;    // ft: shortest shared stretch given a face
    constexpr double cell = 1.0;        // ft: spatial hash cell

    struct Tri {
        int material;
        std::array<V3, 3> p;            // glTF meters
        std::array<V2, 3> uv;
        std::array<std::array<double, 4>, 3> col;
        bool opaque;
    };

    inline double ft(const V3& a) { return norm(a) * feet_per_meter; }

    struct CellKey {
        int64_t x, y, z;
        bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct CellHash {
        size_t operator()(const CellKey& k) const {
            return size_t(k.x * 73856093) ^ size_t(k.y * 19349663) ^ size_t(k.z * 83492791);
        }
    };
    CellKey cell_of(const V3& p) {
        double s = feet_per_meter / cell;
        return { int64_t(std::floor(p[0] * s)), int64_t(std::floor(p[1] * s)), int64_t(std::floor(p[2] * s)) };
    }
    // The cells within one cell of a segment (sampled every half cell).
    template <class F> void cells_along(const V3& a, const V3& b, F&& f) {
        double len = ft(b - a);
        int steps = std::max(1, int(std::ceil(len / (cell * 0.5))));
        std::vector<CellKey> seen;
        for (int s = 0; s <= steps; s++) {
            CellKey c = cell_of(a + (b - a) * (double(s) / steps));
            for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
                CellKey k{ c.x + dx, c.y + dy, c.z + dz };
                if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
                seen.push_back(k);
                if (seen.size() > 64) seen.erase(seen.begin(), seen.begin() + 32);
                f(k);
            }
        }
    }

    // Welded corners (to 1/1000 ft), open edges and corner -> triangles of a triangle list.
    struct Mesh {
        std::vector<V3> pos;                                  // per corner id
        std::vector<std::array<int, 3>> ids;                  // per triangle
        std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> edges;   // (lo, hi) -> (triangle, edge)
        std::vector<std::vector<int>> tris_of;                // per corner id
        std::vector<char> open_corner;                        // per corner id: on an open edge of an opaque triangle
        std::vector<std::pair<int, int>> open;                // (triangle, edge) of opaque triangles' open edges

        explicit Mesh(const std::vector<Tri>& tris) {
            std::map<std::array<int64_t, 3>, int> weld;
            for (const Tri& t : tris) {
                std::array<int, 3> id;
                for (int c = 0; c < 3; c++) {
                    std::array<int64_t, 3> k;
                    for (int a = 0; a < 3; a++) k[a] = std::llround(t.p[c][a] * feet_per_meter * 1000.0);
                    auto [it, added] = weld.emplace(k, int(pos.size()));
                    if (added) pos.push_back(t.p[c]);
                    id[c] = it->second;
                }
                ids.push_back(id);
            }
            tris_of.resize(pos.size());
            for (size_t t = 0; t < ids.size(); t++) {
                for (int e = 0; e < 3; e++) {
                    int a = ids[t][e], b = ids[t][(e + 1) % 3];
                    tris_of[size_t(a)].push_back(int(t));
                    if (a != b) edges[{ std::min(a, b), std::max(a, b) }].push_back({ int(t), e });
                }
            }
            open_corner.assign(pos.size(), 0);
            for (const auto& [key, users] : edges) {
                if (users.size() != 1 || !tris[size_t(users[0].first)].opaque) continue;
                open.push_back(users[0]);
                open_corner[size_t(key.first)] = open_corner[size_t(key.second)] = 1;
            }
        }
        bool share_triangle(int a, int b) const {
            for (int t : tris_of[size_t(a)]) {
                const auto& c = ids[size_t(t)];
                if (c[0] == b || c[1] == b || c[2] == b) return true;
            }
            return false;
        }
        std::pair<int, int> ends(const std::pair<int, int>& te) const {
            return { ids[size_t(te.first)][te.second], ids[size_t(te.first)][(te.second + 1) % 3] };
        }
    };

    V3 face_normal(const Tri& t) { return unit(cross(t.p[1] - t.p[0], t.p[2] - t.p[0])); }

    // Corner attributes at fraction s along edge e of t (position given).
    void along(const Tri& t, int e, double s, V2& uv, std::array<double, 4>& col) {
        int a = e, b = (e + 1) % 3;
        uv = { t.uv[a][0] + (t.uv[b][0] - t.uv[a][0]) * s, t.uv[a][1] + (t.uv[b][1] - t.uv[a][1]) * s };
        for (int i = 0; i < 4; i++) col[i] = t.col[a][i] + (t.col[b][i] - t.col[a][i]) * s;
    }

    // Step 1: faces under raised open ground edges. Returns the faces added.
    size_t add_curbs(std::vector<Tri>& tris) {
        Mesh m(tris);
        struct Edge { int tri, e; V3 a, b; };
        std::vector<Edge> ground;
        for (const auto& te : m.open) {
            const Tri& t = tris[size_t(te.first)];
            if (std::abs(face_normal(t)[1]) < 0.7) continue;
            auto [a, b] = m.ends(te);
            V3 pa = m.pos[size_t(a)], pb = m.pos[size_t(b)];
            if (std::hypot(pb[0] - pa[0], pb[2] - pa[2]) * feet_per_meter < curb_len) continue;
            ground.push_back({ te.first, te.second, pa, pb });
        }
        std::unordered_map<CellKey, std::vector<int>, CellHash> grid;   // xz cells (y = 0)
        auto flat = [](V3 p) { p[1] = 0; return p; };
        for (size_t i = 0; i < ground.size(); i++) {
            std::vector<CellKey> mine;
            cells_along(flat(ground[i].a), flat(ground[i].b), [&](const CellKey& k) {
                if (k.y == 0 && std::find(mine.begin(), mine.end(), k) == mine.end()) mine.push_back(k);
            });
            for (const CellKey& k : mine) grid[k].push_back(int(i));
        }
        size_t added = 0;
        std::vector<Tri> out;
        std::set<std::pair<int, int>> done;
        for (size_t i = 0; i < ground.size(); i++) {
            const Edge& e1 = ground[i];
            V3 d1 = flat(e1.b - e1.a);
            double l1 = norm(d1);
            V3 u = d1 * (1.0 / l1);
            V3 side{ -u[2], 0, u[0] };
            std::vector<int> cand;
            cells_along(flat(e1.a), flat(e1.b), [&](const CellKey& k) {
                auto it = grid.find(k);
                if (it != grid.end()) cand.insert(cand.end(), it->second.begin(), it->second.end());
            });
            std::sort(cand.begin(), cand.end());
            cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
            for (int j : cand) {
                if (j <= int(i) || !done.insert({ int(i), j }).second) continue;
                const Edge& e2 = ground[size_t(j)];
                // On e1's line seen from above.
                double da = dot(flat(e2.a - e1.a), side), db = dot(flat(e2.b - e1.a), side);
                if (std::max(std::abs(da), std::abs(db)) * feet_per_meter > curb_line) continue;
                double ta = dot(flat(e2.a - e1.a), u), tb = dot(flat(e2.b - e1.a), u);
                double s0 = std::max(0.0, std::min(ta, tb)), s1 = std::min(l1, std::max(ta, tb));
                if ((s1 - s0) * feet_per_meter < curb_len) continue;
                // Their faces on opposite sides of the line.
                auto away = [&](const Edge& e) {
                    const Tri& t = tris[size_t(e.tri)];
                    return dot(flat(t.p[(e.e + 2) % 3] - e1.a), side);
                };
                if (away(e1) * away(e2) >= 0) continue;
                // Heights over the shared stretch.
                auto y1 = [&](double s) { return e1.a[1] + (e1.b[1] - e1.a[1]) * (s / l1); };
                auto y2 = [&](double s) { return e2.a[1] + (e2.b[1] - e2.a[1]) * ((s - ta) / (tb - ta)); };
                double h0 = (y1(s0) - y2(s0)) * feet_per_meter, h1 = (y1(s1) - y2(s1)) * feet_per_meter;
                double sign = std::abs(h0) > std::abs(h1) ? (h0 > 0 ? 1 : -1) : (h1 > 0 ? 1 : -1);
                double hi = std::max(std::abs(h0), std::abs(h1));
                if (hi < curb_min || hi > curb_max || std::min(h0 * sign, h1 * sign) < -0.01) continue;
                const Edge& upper = sign > 0 ? e1 : e2;
                const Edge& lower = sign > 0 ? e2 : e1;
                auto at = [&](const Edge& e, double s) {
                    // The point of e over s along e1's line, and its fraction along e's own triangle edge.
                    V3 d = flat(e.b - e.a);
                    double f = dot(flat(e1.a + u * s - e.a), d) / dot(d, d);
                    f = std::clamp(f, 0.0, 1.0);
                    // The triangle's edge e runs from corner e.e to e.e + 1; e.a/e.b are those corners (welded).
                    return std::make_pair(e.a + (e.b - e.a) * f, f);
                };
                const Tri& ut = tris[size_t(upper.tri)];
                auto [U0, f0] = at(upper, s0);
                auto [U1, f1] = at(upper, s1);
                V3 L0 = at(lower, s0).first, L1 = at(lower, s1).first;
                V2 uv0, uv1;
                std::array<double, 4> c0, c1;
                along(ut, upper.e, f0, uv0, c0);
                along(ut, upper.e, f1, uv1, c1);
                // Both windings: the face is seen from the lower side, and the faces under a curb are never seen.
                out.push_back({ ut.material, { U0, U1, L1 }, { uv0, uv1, uv1 }, { c0, c1, c1 }, true });
                out.push_back({ ut.material, { U0, L1, L0 }, { uv0, uv1, uv0 }, { c0, c1, c0 }, true });
                out.push_back({ ut.material, { U0, L1, U1 }, { uv0, uv1, uv1 }, { c0, c1, c1 }, true });
                out.push_back({ ut.material, { U0, L0, L1 }, { uv0, uv0, uv1 }, { c0, c0, c1 }, true });
                added += 4;
            }
        }
        tris.insert(tris.end(), out.begin(), out.end());
        return added;
    }

    // Steps 2 and 3. Returns (corners snapped, edges split).
    std::pair<size_t, size_t> close_seams(std::vector<Tri>& tris) {
        Mesh m(tris);
        std::unordered_map<CellKey, std::vector<int>, CellHash> grid;
        for (size_t v = 0; v < m.pos.size(); v++) if (m.open_corner[v]) grid[cell_of(m.pos[v])].push_back(int(v));
        // Step 2: snap open corners onto open corners within `seam`.
        std::vector<int> moved_to(m.pos.size(), -1);
        std::vector<char> target(m.pos.size(), 0);   // a corner others moved onto stays where it is
        size_t snapped = 0;
        for (size_t v = 0; v < m.pos.size(); v++) {
            if (!m.open_corner[v] || moved_to[v] >= 0) continue;
            CellKey c = cell_of(m.pos[v]);
            for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
                auto it = grid.find({ c.x + dx, c.y + dy, c.z + dz });
                if (it == grid.end()) continue;
                for (int w : it->second) {
                    if (w == int(v) || moved_to[size_t(w)] >= 0 || target[size_t(w)]) continue;
                    double d = ft(m.pos[size_t(w)] - m.pos[v]);
                    if (d > seam || (d > tjunc && m.share_triangle(w, int(v)))) continue;
                    moved_to[size_t(w)] = int(v);
                    target[v] = 1;
                    snapped++;
                }
            }
        }
        for (size_t w = 0; w < m.pos.size(); w++) if (moved_to[w] >= 0) m.pos[w] = m.pos[size_t(moved_to[w])];
        // Open edge directions at each open corner, for the alongside test.
        std::vector<std::vector<V3>> dirs(m.pos.size());
        for (const auto& te : m.open) {
            auto [a, b] = m.ends(te);
            V3 d = unit(m.pos[size_t(b)] - m.pos[size_t(a)]);
            dirs[size_t(a)].push_back(d);
            dirs[size_t(b)].push_back(d);
        }
        // Step 3: corners on open edges.
        std::map<std::pair<int, int>, std::vector<std::pair<double, int>>> inserts;   // (tri, edge) -> (fraction, corner)
        size_t split = 0;
        for (const auto& te : m.open) {
            auto [a, b] = m.ends(te);
            const V3 &pa = m.pos[size_t(a)], &pb = m.pos[size_t(b)];
            V3 d = pb - pa;
            double len = norm(d);
            if (len * feet_per_meter < 0.05) continue;
            V3 u = d * (1.0 / len);
            std::vector<int> cand;
            cells_along(pa, pb, [&](const CellKey& k) {
                auto it = grid.find(k);
                if (it != grid.end()) cand.insert(cand.end(), it->second.begin(), it->second.end());
            });
            std::sort(cand.begin(), cand.end());
            cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
            auto& list = inserts[te];
            for (int v : cand) {
                if (v == a || v == b) continue;
                const V3& pv = m.pos[size_t(v)];
                if (ft(pv - pa) < 0.02 || ft(pv - pb) < 0.02) continue;   // snapped onto an end
                double s = dot(pv - pa, u);
                if (s * feet_per_meter < 0.02 || (len - s) * feet_per_meter < 0.02) continue;
                double off = ft(pv - (pa + u * s));
                if (off > seam) continue;
                if (off > tjunc) {
                    // Outside the edge's triangle (a strip's other side is inside it), with an open edge alongside.
                    const Tri& host = tris[size_t(te.first)];
                    V3 c = host.p[(te.second + 2) % 3] - pa;
                    V3 in = c - u * dot(c, u);
                    if (dot(pv - pa, in) >= 0) continue;
                    bool alongside = std::any_of(dirs[size_t(v)].begin(), dirs[size_t(v)].end(),
                                                 [&](const V3& dv) { return std::abs(dot(dv, u)) > 0.98; });
                    if (!alongside) continue;
                }
                list.push_back({ s / len, v });
            }
            if (list.empty()) inserts.erase(te);
            else split++;
        }
        // Rebuild: every triangle at its (snapped) corners; split ones as a fan around their center.
        std::vector<Tri> out;
        out.reserve(tris.size() + inserts.size() * 4);
        for (size_t t = 0; t < tris.size(); t++) {
            Tri tr = tris[t];
            for (int c = 0; c < 3; c++) tr.p[c] = m.pos[size_t(m.ids[t][c])];
            bool any = false;
            for (int e = 0; e < 3; e++) any = any || inserts.count({ int(t), e });
            if (!any) {
                out.push_back(tr);
                continue;
            }
            struct P { V3 p; V2 uv; std::array<double, 4> col; };
            std::vector<P> ring;
            for (int e = 0; e < 3; e++) {
                ring.push_back({ tr.p[e], tr.uv[e], tr.col[e] });
                auto it = inserts.find({ int(t), e });
                if (it == inserts.end()) continue;
                std::vector<std::pair<double, int>> pts = it->second;
                std::sort(pts.begin(), pts.end());
                for (const auto& [s, v] : pts) {
                    P q;
                    q.p = m.pos[size_t(v)];
                    along(tr, e, s, q.uv, q.col);
                    ring.push_back(q);
                }
            }
            P mid{};
            for (int c = 0; c < 3; c++) {
                mid.p = mid.p + tr.p[c] * (1.0 / 3.0);
                mid.uv = { mid.uv[0] + tr.uv[c][0] / 3.0, mid.uv[1] + tr.uv[c][1] / 3.0 };
                for (int i = 0; i < 4; i++) mid.col[i] += tr.col[c][i] / 3.0;
            }
            for (size_t k = 0; k < ring.size(); k++) {
                const P& p0 = ring[k];
                const P& p1 = ring[(k + 1) % ring.size()];
                out.push_back({ tr.material, { mid.p, p0.p, p1.p }, { mid.uv, p0.uv, p1.uv }, { mid.col, p0.col, p1.col }, tr.opaque });
            }
        }
        tris = std::move(out);
        return { snapped, split };
    }
}

std::vector<Glb::Prim> a49::close_scenery_seams(const std::vector<Glb::Prim>& prims, const std::function<bool(int)>& opaque,
                                                std::vector<std::string>& report) {
    std::vector<Tri> tris;
    std::vector<int> order;
    for (const Glb::Prim& p : prims) {
        order.push_back(p.material);
        bool op = opaque(p.material);
        for (const auto& t : p.tris) {
            Tri tr{ p.material, {}, {}, {}, op };
            for (int c = 0; c < 3; c++) {
                tr.p[c] = p.verts[t[c]];
                tr.uv[c] = p.uvs[t[c]];
                tr.col[c] = p.colors.empty() ? std::array<double, 4>{ 1, 1, 1, 1 } : p.colors[t[c]];
            }
            tris.push_back(tr);
        }
    }
    size_t before = tris.size();
    size_t curbs = add_curbs(tris);
    auto [snapped, split] = close_seams(tris);
    std::map<int, Glb::Prim> by;
    for (const Tri& t : tris) {
        Glb::Prim& p = by[t.material];
        p.material = t.material;
        V3 n = face_normal(t);
        uint32_t base = uint32_t(p.verts.size());
        for (int c = 0; c < 3; c++) {
            p.verts.push_back(t.p[c]);
            p.normals.push_back(n);
            p.uvs.push_back(t.uv[c]);
            p.colors.push_back(t.col[c]);
        }
        p.tris.push_back({ base, base + 1, base + 2 });
    }
    std::vector<Glb::Prim> out;
    for (int mtl : order) if (by.count(mtl)) out.push_back(std::move(by[mtl]));
    char line[200];
    snprintf(line, sizeof(line), "seams: %zu curb faces, %zu corners snapped, %zu edges split at corners on them (%zu -> %zu triangles)",
             curbs, snapped, split, before, tris.size());
    report.push_back(line);
    return out;
}
