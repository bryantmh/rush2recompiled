// Half-body sheets (include/body_sheet.h, docs/texture_upscaling.md "Half-body sheets").
//
// A car body's tiles each have their own texture space: the model switches texture between runs of triangles,
// and where two tiles meet on the body nothing lines them up (the artists often did, but as often turned or shifted a
// neighbor). The sheet is made from the mesh: a piece is a run of connected triangles drawing one tile, laid on the
// sheet as its tile's texels, turned and flipped so it continues the pieces it shares body edges with. Every tile is
// drawn on both sides of the car (mirrored), so only one side's pieces go in: those of the side first, then any from
// the other side that show tile texels the first didn't. A Rush 2049 car's body is its FRAME1 model; a Rush 2 car's is
// five parts (body_part) whose edges meet in the car's space, so pieces join across them as within one model.

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

#include "body_sheet.h"

namespace rush2::sheet {

namespace {
    // The eight turns and flips as integer matrices (a, b, c, d): x' = a x + b y, y' = c x + d y.
    constexpr int d8[8][4] = { { 1, 0, 0, 1 }, { 0, -1, 1, 0 }, { -1, 0, 0, -1 }, { 0, 1, -1, 0 },
                               { -1, 0, 0, 1 }, { 1, 0, 0, -1 }, { 0, 1, 1, 0 }, { 0, -1, -1, 0 } };

    int find_d8(int a, int b, int c, int d) {
        for (int k = 0; k < 8; k++) {
            if (d8[k][0] == a && d8[k][1] == b && d8[k][2] == c && d8[k][3] == d) return k;
        }
        return 0;
    }
    int compose(int p, int q) { // p * q
        const int* m = d8[p];
        const int* n = d8[q];
        return find_d8(m[0] * n[0] + m[1] * n[2], m[0] * n[1] + m[1] * n[3], m[2] * n[0] + m[3] * n[2],
                       m[2] * n[1] + m[3] * n[3]);
    }
    int inverse(int p) { // The transpose: they're orthogonal.
        return find_d8(d8[p][0], d8[p][2], d8[p][1], d8[p][3]);
    }
    void apply(int m, double x, double y, double& ox, double& oy) {
        ox = d8[m][0] * x + d8[m][1] * y;
        oy = d8[m][2] * x + d8[m][3] * y;
    }
    // The sheet texel a tile texel lands on.
    void cell(int m, int tx, int ty, int x, int y, int& cx, int& cy) {
        double ox, oy;
        apply(m, x + 0.5, y + 0.5, ox, oy);
        cx = int(std::floor(ox + tx));
        cy = int(std::floor(oy + ty));
    }

    struct Tri {
        int tex;
        float pos[3][3];
        float uv[3][2]; // In the tile's texels.
        bool extra = false; // From a model other than the body's.
    };

    struct Frag {
        int tex = -1;
        std::vector<int> tris;
        std::vector<uint8_t> core, shown, owned;
        int core_count = 0;
        double side = 0;
        bool extra = false;
        bool kept = false;
        bool placed = false;
        int orient = 0, x = 0, y = 0;
        double fx = 0, fy = 0;  // The exact offset (x, y is it rounded once, so errors don't add up along a chain).
        int island = -1;
    };

    using PosKey = std::array<int32_t, 3>;
    PosKey pos_key(const float* p) {
        return { int32_t(std::lround(p[0] * 4)), int32_t(std::lround(p[1] * 4)), int32_t(std::lround(p[2] * 4)) };
    }
    struct PosKeyHash {
        size_t operator()(const PosKey& k) const {
            return std::hash<int64_t>()((int64_t(k[0]) * 73856093) ^ (int64_t(k[1]) * 19349663) ^ (int64_t(k[2]) * 83492791));
        }
    };
    using EdgeKey = std::pair<PosKey, PosKey>;
    struct EdgeKeyHash {
        size_t operator()(const EdgeKey& k) const { return PosKeyHash()(k.first) * 31 + PosKeyHash()(k.second); }
    };

    // Marks the tile texels a triangle covers: those whose centers are inside it, and those its edges cross (thin
    // triangles cover no centers).
    void rasterize(const Tri& t, uint32_t w, uint32_t h, std::vector<uint8_t>& mask) {
        auto mark = [&](double x, double y) {
            int ix = std::clamp(int(std::floor(x)), 0, int(w) - 1), iy = std::clamp(int(std::floor(y)), 0, int(h) - 1);
            mask[size_t(iy) * w + ix] = 1;
        };
        const float(*v)[2] = t.uv;
        double minx = std::min({ v[0][0], v[1][0], v[2][0] }), maxx = std::max({ v[0][0], v[1][0], v[2][0] });
        double miny = std::min({ v[0][1], v[1][1], v[2][1] }), maxy = std::max({ v[0][1], v[1][1], v[2][1] });
        double area = (v[1][0] - v[0][0]) * (v[2][1] - v[0][1]) - (v[2][0] - v[0][0]) * (v[1][1] - v[0][1]);
        if (std::fabs(area) > 1e-9) {
            for (int y = std::max(0, int(std::floor(miny))); y <= std::min(int(h) - 1, int(std::ceil(maxy))); y++) {
                for (int x = std::max(0, int(std::floor(minx))); x <= std::min(int(w) - 1, int(std::ceil(maxx))); x++) {
                    double px = x + 0.5, py = y + 0.5;
                    double w0 = (v[1][0] - px) * (v[2][1] - py) - (v[2][0] - px) * (v[1][1] - py);
                    double w1 = (v[2][0] - px) * (v[0][1] - py) - (v[0][0] - px) * (v[2][1] - py);
                    double w2 = (v[0][0] - px) * (v[1][1] - py) - (v[1][0] - px) * (v[0][1] - py);
                    if ((w0 >= -1e-6 && w1 >= -1e-6 && w2 >= -1e-6) || (w0 <= 1e-6 && w1 <= 1e-6 && w2 <= 1e-6)) {
                        mask[size_t(y) * w + x] = 1;
                    }
                }
            }
        }
        for (int e = 0; e < 3; e++) {
            const float* a = v[e];
            const float* b = v[(e + 1) % 3];
            double len = std::hypot(b[0] - a[0], b[1] - a[1]);
            int steps = std::max(1, int(std::ceil(len * 4)));
            for (int s = 0; s <= steps; s++) {
                double f = double(s) / steps;
                mark(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f);
            }
        }
    }

    std::vector<uint8_t> dilate(const std::vector<uint8_t>& m, uint32_t w, uint32_t h) {
        std::vector<uint8_t> out = m;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                if (!m[size_t(y) * w + x]) continue;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = int(x) + dx, ny = int(y) + dy;
                        if (nx >= 0 && ny >= 0 && nx < int(w) && ny < int(h)) out[size_t(ny) * w + nx] = 1;
                    }
                }
            }
        }
        return out;
    }

    std::string rle(const std::vector<uint8_t>& m) {
        std::string out;
        uint8_t value = 0;
        size_t run = 0;
        for (uint8_t v : m) {
            if ((v != 0) == (value != 0)) {
                run++;
                continue;
            }
            out += std::to_string(run) + ",";
            value = v ? 1 : 0;
            run = 1;
        }
        return out + std::to_string(run);
    }

    std::vector<uint8_t> unrle(const std::string& s, size_t n) {
        std::vector<uint8_t> out;
        out.reserve(n);
        uint8_t value = 0;
        std::stringstream ss(s);
        std::string part;
        while (std::getline(ss, part, ',')) {
            size_t run = size_t(std::strtoull(part.c_str(), nullptr, 10));
            out.insert(out.end(), std::min(run, n - out.size()), value);
            value ^= 1;
        }
        out.resize(n, 0);
        return out;
    }
}

// A Rush 2 car's undamaged, nearest body part: <car>D0_FL1, FR1, RL1, RR1 and TOP1 (D0_DFL1... are the crumpled
// ones, D2_ the farther set; FRAME1 is the whole car at a distance).
static bool body_part(const std::string& name) {
    size_t at = name.rfind("D0_");
    if (at == std::string::npos) return false;
    std::string part = name.substr(at + 3);
    return part == "FL1" || part == "FR1" || part == "RL1" || part == "RR1" || part == "TOP1";
}

bool build(const rush2::catalog::Container& container, const std::vector<std::string>& paths,
           const std::string& sheet_path, Sheet& out, std::map<std::string, std::vector<uint8_t>>* drawn_masks) {
    out = Sheet{};
    out.path = sheet_path;
    std::vector<const rush2::catalog::Mesh*> meshes;
    for (const rush2::catalog::Model& m : container.models) {
        if (body_part(m.name) && !m.lods.empty()) {
            for (const rush2::catalog::Mesh& mesh : m.lods[0].meshes) meshes.push_back(&mesh);
        }
    }
    if (meshes.empty()) {
        for (const rush2::catalog::Model& m : container.models) {
            if (m.name.size() >= 6 && m.name.compare(m.name.size() - 6, 6, "FRAME1") == 0 && !m.lods.empty()) {
                for (const rush2::catalog::Mesh& mesh : m.lods[0].meshes) meshes.push_back(&mesh);
                break;
            }
        }
    }
    if (meshes.empty()) return false;
    std::vector<const rush2::catalog::Mesh*> all_meshes; // Every model's, at every distance.
    for (const rush2::catalog::Model& m : container.models) {
        for (const rush2::catalog::Lod& lod : m.lods) {
            for (const rush2::catalog::Mesh& mesh : lod.meshes) all_meshes.push_back(&mesh);
        }
    }

    // Some Rush 2 cars' parts (GT90's) draw small stand-ins in the model, a quarter of the size, which the game draws
    // as the car's full-size panels (GT_D0_1... in the texture table, as the census shows): each stand-in is the
    // panel it's a point sample of, among the panels no model draws itself (BMW's BMD0_1_4 is a real far texture: its
    // BMD0_1 is drawn). UVs are in tile widths, so they carry over.
    std::vector<int> as_drawn(container.textures.size());
    for (size_t i = 0; i < as_drawn.size(); i++) as_drawn[i] = int(i);
    std::vector<uint8_t> referenced(container.textures.size(), 0);
    for (const rush2::catalog::Mesh* mp : all_meshes) {
        if (mp->texture >= 0 && size_t(mp->texture) < referenced.size()) referenced[size_t(mp->texture)] = 1;
    }
    std::vector<uint8_t> matched(container.textures.size(), 0);
    for (const rush2::catalog::Mesh* mp : all_meshes) {
        int small = mp->texture;
        if (small < 0 || size_t(small) >= container.textures.size() || matched[size_t(small)]) continue;
        matched[size_t(small)] = 1;
        const rush2::catalog::Texture& t = container.textures[size_t(small)];
        if (t.image.rgba.size() != size_t(t.width) * t.height * 4) continue;
        double best = 0.3; // At most this share of texels unlike.
        for (size_t u = 0; u < container.textures.size(); u++) {
            const rush2::catalog::Texture& big = container.textures[u];
            if (referenced[u] || big.width != t.width * 4 || big.height != t.height * 4 || u >= paths.size() ||
                paths[u].empty() || big.image.rgba.size() != size_t(big.width) * big.height * 4) {
                continue;
            }
            for (uint32_t o = 0; o < 4; o++) {
                size_t unlike = 0;
                for (uint32_t y = 0; y < t.height; y++) {
                    for (uint32_t x = 0; x < t.width; x++) {
                        const uint8_t* a = &t.image.rgba[(size_t(y) * t.width + x) * 4];
                        const uint8_t* c = &big.image.rgba[((size_t(y) * 4 + o) * big.width + x * 4 + o) * 4];
                        unlike += std::abs(a[0] - c[0]) > 8 || std::abs(a[1] - c[1]) > 8 || std::abs(a[2] - c[2]) > 8;
                    }
                }
                double share = double(unlike) / double(size_t(t.width) * t.height);
                if (share < best) {
                    best = share;
                    as_drawn[size_t(small)] = int(u);
                }
            }
        }
    }

    // A mesh's triangles drawn with a dumped tile, in its texels. One whose texels run past the tile's edges (a
    // repeating texture) isn't one place of the tile (inside false; a clamped one is). The RDP samples a texel at its whole coordinate
    // and RT64 shifts a replacement's coordinates by half a texel to match (TextureSampler.hlsli), so texel k spans
    // UV k - 0.5 to k + 0.5: the UVs move by half a texel into the sheet's space, where texel k spans k to k + 1.
    // Without it every turned or flipped join was a texel off.
    const float uv_shift = 0.5f;
    auto triangles_of = [&](const rush2::catalog::Mesh& mesh, const std::function<void(const Tri&, bool)>& use) {
        if (mesh.texture < 0 || size_t(mesh.texture) >= container.textures.size()) return;
        const int drawn = as_drawn[size_t(mesh.texture)];
        if (size_t(drawn) >= paths.size() || paths[size_t(drawn)].empty()) return;
        const rush2::catalog::Texture& tex = container.textures[size_t(drawn)];
        if (tex.width == 0 || tex.height == 0) return;
        for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
            Tri t;
            t.tex = drawn;
            bool inside = true;
            for (int k = 0; k < 3; k++) {
                const rush2::catalog::Vertex& v = mesh.vertices[i + k];
                for (int c = 0; c < 3; c++) t.pos[k][c] = v.position[c];
                inside = inside && v.uv[0] > -0.05f && v.uv[0] < 1.05f && v.uv[1] > -0.05f && v.uv[1] < 1.05f;
                t.uv[k][0] = v.uv[0] * float(tex.width) + uv_shift;
                t.uv[k][1] = v.uv[1] * float(tex.height) + uv_shift;
            }
            // Past a clamped tile's edges a triangle draws the edge texels: still one place of the tile.
            const bool clamped =
                tex.edge_s == rush2::upscale::Edge::Clamp && tex.edge_t == rush2::upscale::Edge::Clamp;
            use(t, inside || clamped);
        }
    };

    // What the game draws of each tile, from every model at every distance: Dump Textures leaves a tile out of the
    // folders to upscale when the sheet gives all of that back. A triangle running past a clamped tile's edges draws
    // the edge texels there (rasterize keeps to the tile); past a repeating one, all of it (inside false).
    if (drawn_masks) {
        for (const rush2::catalog::Mesh* mp : all_meshes) {
            triangles_of(*mp, [&](const Tri& t, bool inside) {
                const rush2::catalog::Texture& tex = container.textures[size_t(t.tex)];
                std::vector<uint8_t>& mask = (*drawn_masks)[paths[size_t(t.tex)]];
                if (mask.empty()) mask.assign(size_t(tex.width) * tex.height, 0);
                if (inside) rasterize(t, tex.width, tex.height, mask);
                else std::fill(mask.begin(), mask.end(), uint8_t(1));
            });
        }
    }

    // The sheet's triangles: the body's, then every other model's at every distance drawn with the body's tiles (Rush
    // 2's hood and light parts, the far car), which add a piece only where they show texels the body doesn't.
    std::vector<Tri> tris;
    std::set<int> body_tiles;
    for (const rush2::catalog::Mesh* mp : meshes) {
        triangles_of(*mp, [&](const Tri& t, bool inside) {
            if (!inside) return;
            tris.push_back(t);
            body_tiles.insert(t.tex);
        });
    }
    for (const rush2::catalog::Mesh* mp : all_meshes) {
        if (std::find(meshes.begin(), meshes.end(), mp) != meshes.end()) continue;
        triangles_of(*mp, [&](const Tri& t, bool inside) {
            if (!inside || !body_tiles.count(t.tex)) return;
            Tri e = t;
            e.extra = true;
            tris.push_back(e);
        });
    }
    if (tris.empty()) return false;

    // The axis the body mirrors across: the one most vertices have a mirror image across.
    std::unordered_map<PosKey, int, PosKeyHash> positions;
    for (const Tri& t : tris) {
        for (int k = 0; k < 3; k++) positions[pos_key(t.pos[k])]++;
    }
    int axis = 0;
    size_t best_mirrored = 0;
    for (int a = 0; a < 3; a++) {
        size_t mirrored = 0;
        for (const auto& [p, n] : positions) {
            PosKey q = p;
            q[a] = -q[a];
            mirrored += positions.count(q);
        }
        if (mirrored > best_mirrored) {
            best_mirrored = mirrored;
            axis = a;
        }
    }

    // Pieces: triangles of one tile joined by body edges whose ends have the same texel coordinates on both.
    std::vector<int> parent(tris.size());
    for (size_t i = 0; i < parent.size(); i++) parent[i] = int(i);
    std::function<int(int)> root = [&](int i) { return parent[size_t(i)] == i ? i : parent[size_t(i)] = root(parent[size_t(i)]); };
    struct EdgeUse {
        int tri;
        const float* uv_first;  // At the edge key's first position.
        const float* uv_second;
        const float* uv_third;  // The triangle's other corner: which side of the edge it lies on.
    };
    std::unordered_map<EdgeKey, std::vector<EdgeUse>, EdgeKeyHash> edges;
    for (size_t i = 0; i < tris.size(); i++) {
        for (int e = 0; e < 3; e++) {
            int a = e, b = (e + 1) % 3;
            PosKey ka = pos_key(tris[i].pos[a]), kb = pos_key(tris[i].pos[b]);
            if (ka == kb) continue;
            if (kb < ka) {
                std::swap(ka, kb);
                std::swap(a, b);
            }
            edges[{ ka, kb }].push_back({ int(i), tris[i].uv[a], tris[i].uv[b], tris[i].uv[3 - a - b] });
        }
    }
    auto same_uv = [](const float* p, const float* q) { return std::fabs(p[0] - q[0]) < 0.05f && std::fabs(p[1] - q[1]) < 0.05f; };
    for (const auto& [key, uses] : edges) {
        for (size_t i = 0; i < uses.size(); i++) {
            for (size_t j = i + 1; j < uses.size(); j++) {
                const EdgeUse& p = uses[i];
                const EdgeUse& q = uses[j];
                if (tris[size_t(p.tri)].tex == tris[size_t(q.tri)].tex && same_uv(p.uv_first, q.uv_first) &&
                    same_uv(p.uv_second, q.uv_second)) {
                    parent[size_t(root(p.tri))] = root(q.tri);
                }
            }
        }
    }
    std::map<int, int> frag_of_root;
    std::vector<Frag> frags;
    std::vector<int> frag_of(tris.size());
    for (size_t i = 0; i < tris.size(); i++) {
        auto [it, added] = frag_of_root.emplace(root(int(i)), int(frags.size()));
        if (added) {
            frags.emplace_back();
            frags.back().tex = tris[i].tex;
            frags.back().extra = tris[i].extra;
        }
        frag_of[i] = it->second;
        frags[size_t(it->second)].tris.push_back(int(i));
    }
    for (Frag& f : frags) {
        const rush2::catalog::Texture& tex = container.textures[size_t(f.tex)];
        f.core.assign(size_t(tex.width) * tex.height, 0);
        double side = 0;
        for (int ti : f.tris) {
            rasterize(tris[size_t(ti)], tex.width, tex.height, f.core);
            for (int k = 0; k < 3; k++) side += tris[size_t(ti)].pos[k][axis];
        }
        f.side = side / (3.0 * f.tris.size());
        f.shown = dilate(f.core, tex.width, tex.height);
        f.core_count = int(std::count(f.core.begin(), f.core.end(), 1));
    }

    // One side: which side has more of the body drawn is as good as any (the pieces straddling the middle count as
    // both). Each piece is kept if it shows a quarter of its texels or more that no kept piece of its tile shows.
    double mean_side = 0;
    for (const Frag& f : frags) mean_side += f.side * f.core_count;
    std::vector<int> order(frags.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
    const double first_sign = mean_side >= 0 ? 1.0 : -1.0;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        bool sa = frags[size_t(a)].side * first_sign >= -0.5, sb = frags[size_t(b)].side * first_sign >= -0.5;
        if (sa != sb) return sa;
        if (frags[size_t(a)].extra != frags[size_t(b)].extra) return !frags[size_t(a)].extra;
        return frags[size_t(a)].core_count > frags[size_t(b)].core_count;
    });
    std::map<int, std::vector<uint8_t>> tile_owned;
    for (int fi : order) {
        Frag& f = frags[size_t(fi)];
        std::vector<uint8_t>& owned = tile_owned[f.tex];
        if (owned.empty()) owned.assign(f.core.size(), 0);
        int fresh = 0;
        for (size_t i = 0; i < f.core.size(); i++) fresh += f.core[i] && !owned[i];
        // A body piece showing a quarter of its texels fresh or a few, another model's showing any: every drawn texel
        // the sheet gives back is one less reason to upscale the tile alone.
        if (fresh == 0 || (!f.extra && fresh * 4 < f.core_count && fresh < 6)) continue;
        f.kept = true;
        f.owned.assign(f.shown.size(), 0);
        for (size_t i = 0; i < f.shown.size(); i++) {
            if (f.shown[i] && !owned[i]) {
                f.owned[i] = 1;
                owned[i] = 1;
            }
        }
    }

    // Joins: for each body edge two kept pieces share, the turn and offset taking one's texel coordinates to the
    // other's. A straight edge fits a turn and its mirror alike: the right one puts the two triangles on opposite sides
    // of the edge, as they are on the body. For each pair the turn and offset most edges agree on wins (two pieces can
    // touch in two places that disagree), the offset the mean of the edges within a texel of it (the game's UVs often
    // put neighbors a fraction of a texel apart).
    struct Join {
        int orient = 0;
        double x = 0, y = 0;
    };
    struct Tally {
        int count = 0;
        double x = 0, y = 0;
    };
    struct Vote {
        int orient;
        double x, y;
    };
    std::map<std::pair<int, int>, std::vector<Vote>> votes;
    auto side_of = [](const double* p, const double* q, const double* c) {
        return (q[0] - p[0]) * (c[1] - p[1]) - (q[1] - p[1]) * (c[0] - p[0]);
    };
    for (const auto& [key, uses] : edges) {
        for (size_t i = 0; i < uses.size(); i++) {
            for (size_t j = 0; j < uses.size(); j++) {
                int fa = frag_of[size_t(uses[i].tri)], fb = frag_of[size_t(uses[j].tri)];
                if (fa == fb) continue;
                if (!frags[size_t(fa)].kept || !frags[size_t(fb)].kept) continue;
                const float* ap = uses[i].uv_first;
                const float* aq = uses[i].uv_second;
                const float* ac = uses[i].uv_third;
                const double bp[2] = { uses[j].uv_first[0], uses[j].uv_first[1] };
                const double bq[2] = { uses[j].uv_second[0], uses[j].uv_second[1] };
                const double bc[2] = { uses[j].uv_third[0], uses[j].uv_third[1] };
                double dax = aq[0] - ap[0], day = aq[1] - ap[1], dbx = bq[0] - bp[0], dby = bq[1] - bp[1];
                if (std::hypot(dax, day) < 0.5) continue;
                for (int m = 0; m < 8; m++) {
                    double rx, ry;
                    apply(m, dax, day, rx, ry);
                    // An edge of another length in the two tiles' texels (20 to 110 a car: the art changes scale at
                    // the seam) can't be joined without resampling.
                    if (std::hypot(rx - dbx, ry - dby) > 0.75) continue;
                    double px, py, cx, cy;
                    apply(m, ap[0], ap[1], px, py);
                    const double tx = bp[0] - px, ty = bp[1] - py;
                    apply(m, ac[0], ac[1], cx, cy);
                    const double c_mapped[2] = { cx + tx, cy + ty };
                    if (side_of(bp, bq, c_mapped) * side_of(bp, bq, bc) > 0) continue; // Folded onto b's triangle.
                    votes[{ fa, fb }].push_back({ m, tx, ty });
                }
            }
        }
    }
    struct Link {
        int to;
        Join join; // to's coordinates = M * from's + (x, y)
        int weight;
    };
    std::vector<std::vector<Link>> links(frags.size());
    for (const auto& [pair, list] : votes) {
        auto near = [](const Vote& a, const Vote& b) {
            return a.orient == b.orient && std::fabs(a.x - b.x) <= 1.0 && std::fabs(a.y - b.y) <= 1.0;
        };
        Tally best;
        int best_orient = 0;
        for (const Vote& v : list) {
            Tally t;
            for (const Vote& w : list) {
                if (!near(v, w)) continue;
                t.count++;
                t.x += w.x;
                t.y += w.y;
            }
            if (t.count > best.count) {
                best = t;
                best_orient = v.orient;
            }
        }
        if (best.count == 0) continue;
        links[size_t(pair.first)].push_back({ pair.second, { best_orient, best.x / best.count, best.y / best.count }, best.count });
    }

    // Islands: from the biggest piece not yet placed, the pieces joined to the placed ones, most shared edges first,
    // each turned and moved by its join, unless its covered texels would land on another piece's.
    struct CellHash {
        size_t operator()(const std::pair<int, int>& c) const { return std::hash<int64_t>()((int64_t(c.first) << 32) ^ uint32_t(c.second)); }
    };
    std::vector<std::vector<int>> islands;
    for (;;) {
        int start = -1;
        for (size_t i = 0; i < frags.size(); i++) {
            if (frags[i].kept && !frags[i].placed && (start < 0 || frags[i].core_count > frags[size_t(start)].core_count)) start = int(i);
        }
        if (start < 0) break;
        std::unordered_map<std::pair<int, int>, int, CellHash> cores; // sheet texel -> piece
        std::vector<int> members;
        auto cells_of = [&](const Frag& f, int orient, int x, int y, std::vector<std::pair<int, int>>& out_cells) {
            const rush2::catalog::Texture& tex = container.textures[size_t(f.tex)];
            out_cells.clear();
            for (uint32_t ty = 0; ty < tex.height; ty++) {
                for (uint32_t tx = 0; tx < tex.width; tx++) {
                    if (!f.core[size_t(ty) * tex.width + tx]) continue;
                    int cx, cy;
                    cell(orient, x, y, int(tx), int(ty), cx, cy);
                    out_cells.push_back({ cx, cy });
                }
            }
        };
        std::vector<std::pair<int, int>> scratch;
        auto place = [&](int fi, int orient, double fx, double fy, bool force) {
            Frag& f = frags[size_t(fi)];
            const int x = int(std::lround(fx)), y = int(std::lround(fy));
            cells_of(f, orient, x, y, scratch);
            int clashes = 0;
            for (const auto& c : scratch) clashes += cores.count(c) != 0;
            // A shared edge's texels can land on each other by half a texel's rounding: a few clashes are allowed.
            if (!force && clashes > std::max(2, int(scratch.size()) / 12)) return false;
            for (const auto& c : scratch) cores.emplace(c, fi);
            f.placed = true;
            f.orient = orient;
            f.x = x;
            f.y = y;
            f.fx = fx;
            f.fy = fy;
            f.island = int(islands.size());
            members.push_back(fi);
            return true;
        };
        place(start, 0, 0, 0, true);
        struct Candidate {
            int weight, from, link;
            bool operator<(const Candidate& o) const { return weight < o.weight; }
        };
        std::priority_queue<Candidate> queue;
        std::set<int> refused;
        auto offer = [&](int fi) {
            for (size_t l = 0; l < links[size_t(fi)].size(); l++) {
                queue.push({ links[size_t(fi)][l].weight, fi, int(l) });
            }
        };
        offer(start);
        while (!queue.empty()) {
            Candidate c = queue.top();
            queue.pop();
            const Link& link = links[size_t(c.from)][size_t(c.link)];
            Frag& to = frags[size_t(link.to)];
            if (to.placed || refused.count(link.to)) continue;
            const Frag& from = frags[size_t(c.from)];
            // from's sheet = M_from * a + t_from and b = M_link * a + t_link, so b's sheet = M_from M_link^-1 (b - t_link)
            // + t_from.
            int orient = compose(from.orient, inverse(link.join.orient));
            double ox, oy;
            apply(orient, link.join.x, link.join.y, ox, oy);
            const double fx = from.fx - ox, fy = from.fy - oy;
            // Every other placed neighbor of the island must agree within a texel: where joins run round a loop the
            // flat sheet can't close, the piece starts its own island rather than landing texels off a neighbor.
            bool agrees = true;
            for (const Link& other : links[size_t(link.to)]) {
                const Frag& q = frags[size_t(other.to)];
                if (other.to == c.from || !q.placed || q.island != int(islands.size())) continue;
                // q's coordinates = M_other * to's + t_other, so to's sheet = M_q M_other * to's + M_q t_other + t_q.
                double qx, qy;
                apply(q.orient, other.join.x, other.join.y, qx, qy);
                if (compose(q.orient, other.join.orient) != orient || std::hypot(qx + q.fx - fx, qy + q.fy - fy) > 1.0) {
                    agrees = false;
                    break;
                }
            }
            if (agrees && place(link.to, orient, fx, fy, false)) offer(link.to);
            else refused.insert(link.to);
        }
        islands.push_back(members);
    }

    // How far apart the two sides of each body edge two joinable pieces of one island share land on the sheet: what an
    // upscale shows as a step there. The game's own UVs leave some (neighbors a fraction of a texel apart), rounding
    // each piece once adds up to half a texel.
    {
        double sum = 0, worst = 0;
        size_t count = 0, over = 0;
        auto on_sheet = [](const Frag& f, const float* uv, double& x, double& y) {
            apply(f.orient, uv[0], uv[1], x, y);
            x += f.x;
            y += f.y;
        };
        for (const auto& [key, uses] : edges) {
            for (size_t i = 0; i < uses.size(); i++) {
                for (size_t j = i + 1; j < uses.size(); j++) {
                    const int fa = frag_of[size_t(uses[i].tri)], fb = frag_of[size_t(uses[j].tri)];
                    const Frag& a = frags[size_t(fa)];
                    const Frag& b = frags[size_t(fb)];
                    if (fa == fb || !a.placed || !b.placed || a.island != b.island) continue;
                    // Only pieces that could join (an edge of another scale never lines up).
                    if (std::none_of(links[size_t(fa)].begin(), links[size_t(fa)].end(), [&](const Link& l) { return l.to == fb; })) continue;
                    double ax, ay, bx, by, cx, cy, dx, dy;
                    on_sheet(a, uses[i].uv_first, ax, ay);
                    on_sheet(b, uses[j].uv_first, bx, by);
                    on_sheet(a, uses[i].uv_second, cx, cy);
                    on_sheet(b, uses[j].uv_second, dx, dy);
                    double e = std::max(std::hypot(ax - bx, ay - by), std::hypot(cx - dx, cy - dy));
                    sum += e;
                    worst = std::max(worst, e);
                    over += e > 0.75;
                    count++;
                }
            }
        }
        printf("[sheet] %s: %zu joined edges, mean step %.2f texels, worst %.2f, %zu over 0.75\n", sheet_path.c_str(),
               count, count ? sum / count : 0.0, worst, over);
    }

    // Each island's box (of the shown texels), then rows of islands, tallest first, 2 texels apart.
    struct Box {
        int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    };
    std::vector<Box> boxes(islands.size());
    for (size_t i = 0; i < islands.size(); i++) {
        for (int fi : islands[i]) {
            const Frag& f = frags[size_t(fi)];
            const rush2::catalog::Texture& tex = container.textures[size_t(f.tex)];
            for (uint32_t ty = 0; ty < tex.height; ty++) {
                for (uint32_t tx = 0; tx < tex.width; tx++) {
                    if (!f.shown[size_t(ty) * tex.width + tx]) continue;
                    int cx, cy;
                    cell(f.orient, f.x, f.y, int(tx), int(ty), cx, cy);
                    Box& b = boxes[i];
                    b.x0 = std::min(b.x0, cx);
                    b.y0 = std::min(b.y0, cy);
                    b.x1 = std::max(b.x1, cx + 1);
                    b.y1 = std::max(b.y1, cy + 1);
                }
            }
        }
    }
    constexpr int gutter = 2;
    std::vector<size_t> by_height(islands.size());
    for (size_t i = 0; i < by_height.size(); i++) by_height[i] = i;
    std::sort(by_height.begin(), by_height.end(), [&](size_t a, size_t b) {
        return boxes[a].y1 - boxes[a].y0 > boxes[b].y1 - boxes[b].y0;
    });
    int widest = 0;
    double area = 0;
    for (const Box& b : boxes) {
        widest = std::max(widest, b.x1 - b.x0);
        area += double(b.x1 - b.x0 + gutter) * (b.y1 - b.y0 + gutter);
    }
    const int row_width = std::max(widest, int(std::ceil(std::sqrt(area * 1.5))));
    int pen_x = 0, pen_y = 0, row_height = 0, sheet_width = 0;
    std::vector<std::pair<int, int>> shift(islands.size());
    for (size_t i : by_height) {
        int w = boxes[i].x1 - boxes[i].x0, h = boxes[i].y1 - boxes[i].y0;
        if (pen_x > 0 && pen_x + w > row_width) {
            pen_x = 0;
            pen_y += row_height + gutter;
            row_height = 0;
        }
        shift[i] = { pen_x - boxes[i].x0, pen_y - boxes[i].y0 };
        pen_x += w + gutter;
        row_height = std::max(row_height, h);
        sheet_width = std::max(sheet_width, pen_x - gutter);
    }
    out.width = uint32_t(std::max(1, sheet_width));
    out.height = uint32_t(std::max(1, pen_y + row_height));

    for (size_t i = 0; i < islands.size(); i++) {
        for (int fi : islands[i]) {
            const Frag& f = frags[size_t(fi)];
            const rush2::catalog::Texture& tex = container.textures[size_t(f.tex)];
            Piece p;
            p.tile = paths[size_t(f.tex)];
            p.tile_width = tex.width;
            p.tile_height = tex.height;
            p.orient = f.orient;
            p.x = f.x + shift[i].first;
            p.y = f.y + shift[i].second;
            p.shown = f.shown;
            p.core = f.core;
            p.owned = f.owned;
            out.pieces.push_back(std::move(p));
        }
    }
    return !out.pieces.empty();
}

Image render(const Sheet& sheet, const std::function<const Image*(const std::string&)>& image_of) {
    Image out;
    out.width = sheet.width;
    out.height = sheet.height;
    out.rgba.assign(size_t(out.width) * out.height * 4, 0);
    std::vector<uint8_t> filled(size_t(out.width) * out.height, 0);
    // The rims first, then the covered texels over them: where pieces touch, a piece's own texels win.
    for (int pass = 0; pass < 2; pass++) {
        for (const Piece& p : sheet.pieces) {
            const Image* tile = image_of(p.tile);
            if (tile == nullptr || tile->width != p.tile_width || tile->height != p.tile_height) continue;
            for (uint32_t ty = 0; ty < p.tile_height; ty++) {
                for (uint32_t tx = 0; tx < p.tile_width; tx++) {
                    size_t i = size_t(ty) * p.tile_width + tx;
                    if (!p.shown[i] || (pass == 1) != (p.core[i] != 0)) continue;
                    int cx, cy;
                    cell(p.orient, p.x, p.y, int(tx), int(ty), cx, cy);
                    if (cx < 0 || cy < 0 || cx >= int(out.width) || cy >= int(out.height)) continue;
                    size_t o = size_t(cy) * out.width + cx;
                    memcpy(&out.rgba[o * 4], &tile->rgba[i * 4], 4);
                    out.rgba[o * 4 + 3] = 255;
                    filled[o] = 1;
                }
            }
        }
    }
    // The space between: each empty texel the average of its filled neighbors, ring by ring.
    for (bool any = true; any;) {
        any = false;
        std::vector<uint8_t> next = filled;
        for (uint32_t y = 0; y < out.height; y++) {
            for (uint32_t x = 0; x < out.width; x++) {
                size_t o = size_t(y) * out.width + x;
                if (filled[o]) continue;
                int sum[3] = {}, n = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = int(x) + dx, ny = int(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= int(out.width) || ny >= int(out.height)) continue;
                        size_t q = size_t(ny) * out.width + nx;
                        if (!filled[q]) continue;
                        for (int c = 0; c < 3; c++) sum[c] += out.rgba[q * 4 + c];
                        n++;
                    }
                }
                if (n == 0) {
                    any = true;
                    continue;
                }
                for (int c = 0; c < 3; c++) out.rgba[o * 4 + c] = uint8_t(sum[c] / n);
                out.rgba[o * 4 + 3] = 255;
                next[o] = 1;
            }
        }
        if (next == filled) break; // Nothing left to grow from.
        filled.swap(next);
    }
    return out;
}

bool cut(const Sheet& sheet, const Image& sheet_up, const std::string& tile, const Image& tile_image,
         const Image& fallback, Image& out) {
    if (sheet.width == 0 || sheet_up.width % sheet.width != 0) return false;
    const uint32_t s = sheet_up.width / sheet.width;
    if (s == 0 || sheet_up.height != sheet.height * s) return false;
    bool any = false;
    for (const Piece& p : sheet.pieces) {
        if (p.tile != tile) continue;
        if (!any) {
            const uint32_t w = p.tile_width * s, h = p.tile_height * s;
            if (fallback.width == w && fallback.height == h) out = fallback;
            else if (!fallback.empty()) out = rush2::upscale::resize_bilinear(fallback, w, h);
            else if (tile_image.width == p.tile_width && tile_image.height == p.tile_height) {
                out.width = w;
                out.height = h;
                out.rgba.resize(size_t(w) * h * 4);
                for (uint32_t y = 0; y < h; y++) {
                    for (uint32_t x = 0; x < w; x++) {
                        memcpy(&out.rgba[(size_t(y) * w + x) * 4], &tile_image.rgba[(size_t(y / s) * p.tile_width + x / s) * 4], 4);
                    }
                }
            }
            else return false;
            any = true;
        }
        for (uint32_t ty = 0; ty < p.tile_height; ty++) {
            for (uint32_t tx = 0; tx < p.tile_width; tx++) {
                if (!p.owned[size_t(ty) * p.tile_width + tx]) continue;
                for (uint32_t j = 0; j < s; j++) {
                    for (uint32_t i = 0; i < s; i++) {
                        // The upscaled pixel's center in tile texels, turned onto the sheet, in the sheet upscale's pixels.
                        double ux = tx + (i + 0.5) / s, uy = ty + (j + 0.5) / s, sx, sy;
                        apply(p.orient, ux, uy, sx, sy);
                        int px = int(std::floor((sx + p.x) * s)), py = int(std::floor((sy + p.y) * s));
                        if (px < 0 || py < 0 || px >= int(sheet_up.width) || py >= int(sheet_up.height)) continue;
                        size_t o = (size_t(ty * s + j) * out.width + tx * s + i) * 4;
                        memcpy(&out.rgba[o], &sheet_up.rgba[(size_t(py) * sheet_up.width + px) * 4], 3);
                    }
                }
            }
        }
    }
    return any;
}

bool covers(const Sheet& sheet, const std::string& tile, const std::vector<uint8_t>& drawn) {
    std::vector<uint8_t> given(drawn.size(), 0);
    bool any = false;
    for (const Piece& p : sheet.pieces) {
        if (p.tile != tile || p.owned.size() != drawn.size()) continue;
        any = true;
        for (size_t i = 0; i < given.size(); i++) given[i] |= p.owned[i];
    }
    if (!any) return false;
    for (size_t i = 0; i < drawn.size(); i++) {
        if (drawn[i] && !given[i]) return false;
    }
    return true;
}

std::vector<std::string> tiles(const Sheet& sheet) {
    std::vector<std::string> out;
    for (const Piece& p : sheet.pieces) {
        if (std::find(out.begin(), out.end(), p.tile) == out.end()) out.push_back(p.tile);
    }
    return out;
}

std::string write_layout(const std::vector<Sheet>& sheets) {
    std::string out = "# Half-body sheets: written by Dump Textures, read by Install Upscaled. Leave it be.\n";
    for (const Sheet& s : sheets) {
        out += "sheet " + s.path + " " + std::to_string(s.width) + " " + std::to_string(s.height) + "\n";
        for (const Piece& p : s.pieces) {
            out += "piece " + p.tile + " " + std::to_string(p.tile_width) + " " + std::to_string(p.tile_height) + " " +
                   std::to_string(p.orient) + " " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + rle(p.shown) +
                   " " + rle(p.core) + " " + rle(p.owned) + "\n";
        }
    }
    return out;
}

std::vector<Sheet> read_layout(const std::string& text) {
    std::vector<Sheet> out;
    std::stringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::stringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind == "sheet") {
            Sheet s;
            ss >> s.path >> s.width >> s.height;
            if (!ss.fail()) out.push_back(std::move(s));
        }
        else if (kind == "piece" && !out.empty()) {
            Piece p;
            std::string shown, core, owned;
            ss >> p.tile >> p.tile_width >> p.tile_height >> p.orient >> p.x >> p.y >> shown >> core >> owned;
            if (ss.fail() || p.orient < 0 || p.orient > 7 || p.tile_width == 0 || p.tile_height == 0) continue;
            size_t n = size_t(p.tile_width) * p.tile_height;
            p.shown = unrle(shown, n);
            p.core = unrle(core, n);
            p.owned = unrle(owned, n);
            out.back().pieces.push_back(std::move(p));
        }
    }
    return out;
}

}
