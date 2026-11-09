// Building a track's ground underlay for Fill Scenery Gaps (include/scenery_fill.h,
// docs/graphics_effects.md#fill-scenery-gaps). Nothing here touches the game, so tools/car_probes_test.cpp --fill runs
// it on its own.
//
// 1. The ground: the track's upward-facing triangles as placed (rush2::catalog::for_each_placed_triangle), each cell
//    of a 32 ft grid marked covered where one lies over its center, with the lowest such surface's height, texture and
//    vertex color (the lowest, so the underlay never comes up through a bridge, a roof or a road decal).
// 2. Heights and colors everywhere else: filled in from the covered cells by pull-push (a pyramid of averages, then
//    each empty cell blended from the level above), so the ground carries on smoothly from its edges and flattens
//    out far away.
// 3. Textures: the few ground textures covering the most of the track, each one's coverage filled in by pull-push
//    like the heights; each cell takes the strongest, so big areas (grass, water, sand) carry on and a road reaching
//    the edge doesn't.
// 4. What is drawn: the fine layer, the cells within 10 cells (320 ft) of the ground that have none, and the covered
//    cells beside them (tucked under the real ground at the edge); the coarse layer, 256 ft cells with no ground in
//    them, out 4000 ft past the track and a little lower, under the fine layer where they overlap, so no gap opens
//    between the two.
// 5. Never through the track: every triangle drawn (any way it faces: embankments and walls falling away from a road
//    too) sets a floor on each cell it overlaps, a bound under its lowest point there. A fine corner sits 2 ft under
//    the floors and heights of its four cells, a coarse corner 6 ft under every fine corner of its cells, so each quad
//    (a mix of its corners) is under everything over its cell.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <string>

#include "asset_catalog.h"
#include "scenery_fill.h"
#include "texture_upscale.h"

namespace rush2::scenery_fill {

namespace {
    using rush2::catalog::Container;
    using rush2::catalog::Placement;

    constexpr float cell = 32.0f;            // Fine cell, feet
    constexpr int coarse_cells = 8;          // Fine cells per coarse cell side (256 ft)
    constexpr int band = 10;                 // Fine cells drawn out from the ground
    constexpr float margin = 4000.0f;        // Feet of coarse ground past the track
    constexpr float fine_drop = 2.0f;        // Feet the fine layer sits under the ground around it
    constexpr float coarse_drop = 6.0f;      // ... and the coarse layer
    constexpr float min_up = 0.5f;           // A ground triangle's normal y at least (60 degrees from vertical)
    constexpr size_t max_kinds = 8;          // Ground textures the underlay picks from
    constexpr float min_share = 0.02f;       // ... each covering at least this much of the ground
    constexpr int fine_chunk = 16;           // Fine cells per chunk side (512 ft)
    constexpr int coarse_chunk = 4;          // Coarse cells per chunk side (1024 ft)
    constexpr int max_strip = 15;            // Quads per strip: 32 vertices, the RSP's vertex buffer
    constexpr float max_texels = 1000.0f;    // A vertex's s and t stay under this (s10.5 holds up to 1024)

    constexpr uint8_t fmt_rgba = 0, fmt_ci = 2, fmt_ia = 3, fmt_i = 4;
    constexpr uint8_t siz_4b = 0, siz_8b = 1, siz_16b = 2;

    struct Grid {
        int nx = 0, nz = 0;
        float x0 = 0, z0 = 0;                // World position of cell (0, 0)'s corner
        size_t at(int x, int z) const { return size_t(z) * nx + x; }
    };

    // Pull-push: fills the cells with weight 0 from the others. values: channels per cell.
    void pull_push(int nx, int nz, int channels, std::vector<float>& values, std::vector<float>& weights) {
        if (nx <= 1 && nz <= 1) return;
        int px = (nx + 1) / 2, pz = (nz + 1) / 2;
        std::vector<float> pv(size_t(px) * pz * channels, 0.0f), pw(size_t(px) * pz, 0.0f);
        for (int z = 0; z < nz; z++) {
            for (int x = 0; x < nx; x++) {
                float w = weights[size_t(z) * nx + x];
                if (w <= 0) continue;
                size_t p = size_t(z / 2) * px + x / 2;
                pw[p] += w;
                for (int c = 0; c < channels; c++) pv[p * channels + c] += values[(size_t(z) * nx + x) * channels + c] * w;
            }
        }
        for (size_t p = 0; p < pw.size(); p++) {
            if (pw[p] > 0) {
                for (int c = 0; c < channels; c++) pv[p * channels + c] /= pw[p];
                pw[p] = std::min(pw[p], 1.0f);
            }
        }
        pull_push(px, pz, channels, pv, pw);
        // Each cell blended with the level above, sampled bilinearly at its center.
        for (int z = 0; z < nz; z++) {
            for (int x = 0; x < nx; x++) {
                size_t i = size_t(z) * nx + x;
                float w = std::min(weights[i], 1.0f);
                if (w >= 1.0f) continue;
                float fx = std::clamp((x + 0.5f) / 2.0f - 0.5f, 0.0f, float(px - 1));
                float fz = std::clamp((z + 0.5f) / 2.0f - 0.5f, 0.0f, float(pz - 1));
                int ix = std::min(int(fx), px - 1), iz = std::min(int(fz), pz - 1);
                int ix1 = std::min(ix + 1, px - 1), iz1 = std::min(iz + 1, pz - 1);
                float tx = fx - ix, tz = fz - iz;
                for (int c = 0; c < channels; c++) {
                    auto pvv = [&](int a, int b) { return pv[(size_t(b) * px + a) * channels + c]; };
                    float up = (pvv(ix, iz) * (1 - tx) + pvv(ix1, iz) * tx) * (1 - tz) + (pvv(ix, iz1) * (1 - tx) + pvv(ix1, iz1) * tx) * tz;
                    float& v = values[i * channels + c];
                    v = w > 0 ? v * w + up * (1 - w) : up;
                }
                weights[i] = 1.0f;
            }
        }
    }

    // The track's texture as the underlay loads it, or false when it can't be (a size or format the loads here don't
    // handle, or an encoding that wouldn't decode to the same image: then the packs couldn't find it either).
    bool encode_texture(const rush2::catalog::Texture& t, Texture& out) {
        const auto& img = t.image;
        auto pow2 = [](uint32_t v) { return v >= 1 && (v & (v - 1)) == 0; };
        if (img.empty() || t.painted || !pow2(t.width) || !pow2(t.height) || img.width != t.width || img.height != t.height) return false;
        const bool paletted = t.tlut != 0;
        if (paletted && (t.siz > siz_8b || t.indices.size() != size_t(t.width) * t.height)) return false;
        if (!paletted && !((t.fmt == fmt_rgba && t.siz == siz_16b) || (t.fmt == fmt_ia && t.siz <= siz_16b) || (t.fmt == fmt_i && t.siz <= siz_8b))) return false;
        const uint32_t bits = 4u << t.siz;
        const uint32_t row_bytes = t.width * bits / 8;
        // LOADBLOCK: whole 64-bit words per row; TMEM: 4 KB, or the lower 2 KB under a palette.
        if (row_bytes < 8 || row_bytes % 8 != 0 || row_bytes * t.height > (paletted ? 2048u : 4096u)) return false;

        out = Texture{};
        out.name = t.name;
        out.fmt = t.fmt;
        out.siz = t.siz;
        out.width = t.width;
        out.height = t.height;
        out.tlut = t.tlut;
        out.texels.assign(size_t(row_bytes) * t.height, 0);
        auto px = [&](size_t i) { return &img.rgba[i * 4]; };
        auto put = [&](size_t i, uint32_t value) {
            switch (bits) {
                case 4: out.texels[i / 2] |= uint8_t((value & 0xF) << ((i & 1) ? 0 : 4)); break;
                case 8: out.texels[i] = uint8_t(value); break;
                case 16: out.texels[i * 2] = uint8_t(value >> 8); out.texels[i * 2 + 1] = uint8_t(value); break;
            }
        };
        const size_t count = size_t(t.width) * t.height;
        if (paletted) {
            out.palette.assign(bits == 4 ? 16 : 256, 0);
            for (size_t i = 0; i < count; i++) {
                uint8_t index = t.indices[i];
                put(i, index);
                const uint8_t* p = px(i);
                out.palette[index] = t.tlut == 0xC000
                    ? uint16_t((p[0] << 8) | p[3])
                    : uint16_t(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >= 128 ? 1 : 0));
            }
        }
        else {
            for (size_t i = 0; i < count; i++) {
                const uint8_t* p = px(i);
                uint32_t v = 0;
                if (t.fmt == fmt_rgba) v = ((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >= 128 ? 1 : 0);
                else if (t.fmt == fmt_ia && bits == 16) v = (p[0] << 8) | p[3];
                else if (t.fmt == fmt_ia && bits == 8) v = ((p[0] >> 4) << 4) | (p[3] >> 4);
                else if (t.fmt == fmt_ia) v = ((p[0] >> 5) << 1) | (p[3] >= 128 ? 1 : 0);
                else if (bits == 8) v = p[0];
                else v = p[0] >> 4;
                put(i, v);
            }
        }

        // It must decode, as RT64 decodes it, to the catalog's image.
        return image_of(out).rgba == img.rgba;
    }

    template <typename T> void put_value(std::vector<uint8_t>& out, const T& v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    template <typename T> bool get_value(const std::vector<uint8_t>& in, size_t& at, T& v) {
        if (at + sizeof(T) > in.size()) return false;
        memcpy(&v, &in[at], sizeof(T));
        at += sizeof(T);
        return true;
    }
}

rush2::upscale::Image image_of(const Texture& t) {
    // TMEM as LOADBLOCK and LOADTLUT leave it (odd rows' 32-bit words swapped in pairs; each palette entry at
    // 0x800 + index * 8, four times), decoded as RT64 decodes it.
    const uint32_t row_bytes = t.width * (4u << t.siz) / 8;
    std::vector<uint8_t> tmem(4096, 0);
    for (uint32_t y = 0; y < t.height; y++) {
        for (uint32_t x = 0; x < row_bytes; x++) {
            uint32_t to = y * row_bytes + ((y & 1) ? (((x / 4) ^ 1) * 4 + (x & 3)) : x);
            if (to < tmem.size() && size_t(y) * row_bytes + x < t.texels.size()) tmem[to] = t.texels[size_t(y) * row_bytes + x];
        }
    }
    for (size_t i = 0; i < t.palette.size() && i < 256; i++) {
        for (int r = 0; r < 4; r++) {
            tmem[0x800 + i * 8 + r * 2] = uint8_t(t.palette[i] >> 8);
            tmem[0x800 + i * 8 + r * 2 + 1] = uint8_t(t.palette[i]);
        }
    }
    return rush2::upscale::decode_tmem(tmem.data(), t.fmt, t.siz, 0, row_bytes / 8, 0, t.tlut, t.width, t.height);
}

Mesh build(const std::vector<const Container*>& containers, const std::vector<Placement>& placements, BuildStats* stats) {
    auto start = std::chrono::steady_clock::now();
    Mesh mesh;

    // The ground triangles, and each texture's size in the world (feet per texel, from its upward triangles' areas in
    // the world and in its own space).
    struct Ground {
        float v[3][3];
        float rgb[3];
        int texture;    // Into `sources`, -1 none
    };
    std::vector<Ground> ground;
    // Every triangle drawn, whichever way it faces: the underlay stays under all of them (an embankment or a wall
    // falling away from a road's edge isn't ground, but a sheet at the road's height would come up through it).
    struct Solid {
        float v[3][3];
    };
    std::vector<Solid> solids;
    std::map<std::pair<const Container*, int>, int> source_index;
    struct Source {
        const Container* container;
        int texture;
        double world_area = 0, uv_area = 0;
    };
    std::vector<Source> sources;
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    rush2::catalog::for_each_placed_triangle(containers, placements, [&](const rush2::catalog::PlacedTriangle& pt) {
        if (pt.model->name.find("SKY") != std::string::npos) return;   // A sky model, in case one is placed
        float e1[3], e2[3];
        for (int k = 0; k < 3; k++) {
            e1[k] = pt.v[1][k] - pt.v[0][k];
            e2[k] = pt.v[2][k] - pt.v[0][k];
        }
        // Clockwise front faces: the front normal is e2 x e1 (turned for a mirroring placement).
        float n[3] = { e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0] };
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-4f) return;
        Solid solid;
        memcpy(solid.v, pt.v, sizeof(solid.v));
        solids.push_back(solid);
        if ((pt.mirrored ? -n[1] : n[1]) < min_up * len) return;
        Ground g;
        memcpy(g.v, pt.v, sizeof(g.v));
        for (int k = 0; k < 3; k++) {
            g.rgb[k] = 0;
            for (int v = 0; v < 3; v++) g.rgb[k] += (pt.mesh->lit ? 180.0f : pt.source[v]->color[k]) / 3.0f;
        }
        g.texture = -1;
        int t = pt.mesh->texture;
        if (t >= 0 && t < (int)pt.container->textures.size()) {
            auto [it, added] = source_index.emplace(std::make_pair(pt.container, t), (int)sources.size());
            if (added) sources.push_back({ pt.container, t });
            g.texture = it->second;
            const auto* s = pt.source;
            double uv = std::fabs((s[1]->uv[0] - s[0]->uv[0]) * (s[2]->uv[1] - s[0]->uv[1]) - (s[2]->uv[0] - s[0]->uv[0]) * (s[1]->uv[1] - s[0]->uv[1])) / 2;
            sources[g.texture].world_area += len / 2;
            sources[g.texture].uv_area += uv;
        }
        for (int v = 0; v < 3; v++) {
            lo[0] = std::min(lo[0], g.v[v][0]);
            lo[1] = std::min(lo[1], g.v[v][2]);
            hi[0] = std::max(hi[0], g.v[v][0]);
            hi[1] = std::max(hi[1], g.v[v][2]);
        }
        ground.push_back(g);
    });
    BuildStats st;
    st.triangles = ground.size();
    if (ground.empty()) {
        if (stats) *stats = st;
        return mesh;
    }

    // The textures the underlay can draw, and their texel size in feet.
    std::vector<int> drawable(sources.size(), -1);
    std::vector<float> texel_ft;
    for (size_t s = 0; s < sources.size(); s++) {
        Texture t;
        if (!encode_texture(sources[s].container->textures[size_t(sources[s].texture)], t)) continue;
        drawable[s] = (int)mesh.textures.size();
        double per_texel = sources[s].uv_area > 1e-9 ? std::sqrt(sources[s].world_area / (sources[s].uv_area * t.width * t.height)) : 0.5;
        texel_ft.push_back(std::clamp((float)per_texel, 0.05f, 16.0f));
        mesh.textures.push_back(std::move(t));
    }

    // The grid: the ground's extent and the margin, in whole coarse cells.
    const float coarse = cell * coarse_cells;
    Grid g;
    g.x0 = std::floor((lo[0] - margin) / coarse) * coarse;
    g.z0 = std::floor((lo[1] - margin) / coarse) * coarse;
    g.nx = int(std::ceil((hi[0] + margin - g.x0) / coarse)) * coarse_cells;
    g.nz = int(std::ceil((hi[1] + margin - g.z0) / coarse)) * coarse_cells;
    const size_t cells = size_t(g.nx) * g.nz;
    std::vector<float> low(cells, 1e30f), rgb(cells * 3, 0.0f), weight(cells, 0.0f);
    std::vector<int> label(cells, -1);   // The lowest surface's texture (sources), then the vote (mesh.textures)

    // 1. Rasterize: each cell center under a triangle gets its height there, if lowest.
    for (const Ground& t : ground) {
        float minx = std::min({ t.v[0][0], t.v[1][0], t.v[2][0] }), maxx = std::max({ t.v[0][0], t.v[1][0], t.v[2][0] });
        float minz = std::min({ t.v[0][2], t.v[1][2], t.v[2][2] }), maxz = std::max({ t.v[0][2], t.v[1][2], t.v[2][2] });
        int x0 = std::max(0, int(std::ceil((minx - g.x0) / cell - 0.5f))), x1 = std::min(g.nx - 1, int(std::floor((maxx - g.x0) / cell - 0.5f)));
        int z0 = std::max(0, int(std::ceil((minz - g.z0) / cell - 0.5f))), z1 = std::min(g.nz - 1, int(std::floor((maxz - g.z0) / cell - 0.5f)));
        float ax = t.v[0][0], az = t.v[0][2];
        float bx = t.v[1][0] - ax, bz = t.v[1][2] - az, cx = t.v[2][0] - ax, cz = t.v[2][2] - az;
        float det = bx * cz - bz * cx;
        if (std::fabs(det) < 1e-6f) continue;
        for (int z = z0; z <= z1; z++) {
            for (int x = x0; x <= x1; x++) {
                float px = g.x0 + (x + 0.5f) * cell - ax, pz = g.z0 + (z + 0.5f) * cell - az;
                float u = (px * cz - pz * cx) / det, v = (bx * pz - bz * px) / det;
                if (u < 0 || v < 0 || u + v > 1) continue;
                float y = t.v[0][1] + u * (t.v[1][1] - t.v[0][1]) + v * (t.v[2][1] - t.v[0][1]);
                size_t i = g.at(x, z);
                if (y < low[i]) {
                    low[i] = y;
                    for (int k = 0; k < 3; k++) rgb[i * 3 + k] = t.rgb[k];
                    label[i] = t.texture >= 0 ? drawable[size_t(t.texture)] : -1;
                    weight[i] = 1.0f;
                }
            }
        }
    }
    std::vector<uint8_t> covered(cells, 0);
    for (size_t i = 0; i < cells; i++) {
        covered[i] = weight[i] > 0;
        st.covered += covered[i];
    }

    // Each cell's floor: a height every triangle over any part of the cell stays above. For a triangle overlapping
    // the cell, its lowest point there is at least its plane's lowest at the cell's corners and at least its lowest
    // vertex (the larger of the two bounds).
    std::vector<float> floor_h(cells, 1e30f);
    for (const Solid& t : solids) {
        float minx = std::min({ t.v[0][0], t.v[1][0], t.v[2][0] }), maxx = std::max({ t.v[0][0], t.v[1][0], t.v[2][0] });
        float minz = std::min({ t.v[0][2], t.v[1][2], t.v[2][2] }), maxz = std::max({ t.v[0][2], t.v[1][2], t.v[2][2] });
        float miny = std::min({ t.v[0][1], t.v[1][1], t.v[2][1] });
        int x0 = std::max(0, int(std::floor((minx - g.x0) / cell))), x1 = std::min(g.nx - 1, int(std::floor((maxx - g.x0) / cell)));
        int z0 = std::max(0, int(std::floor((minz - g.z0) / cell))), z1 = std::min(g.nz - 1, int(std::floor((maxz - g.z0) / cell)));
        if (x0 > x1 || z0 > z1) continue;
        float ax = t.v[0][0], az = t.v[0][2];
        float bx = t.v[1][0] - ax, bz = t.v[1][2] - az, cx = t.v[2][0] - ax, cz = t.v[2][2] - az;
        float det = bx * cz - bz * cx;
        bool planar = std::fabs(det) > 1e-3f;
        for (int z = z0; z <= z1; z++) {
            for (int x = x0; x <= x1; x++) {
                float rx0 = g.x0 + x * cell, rz0 = g.z0 + z * cell;
                const float corners[4][2] = { { rx0, rz0 }, { rx0 + cell, rz0 }, { rx0, rz0 + cell }, { rx0 + cell, rz0 + cell } };
                // Separating axes: the triangle's edges (the box's are the bounds above).
                bool apart = false;
                for (int e = 0; e < 3 && !apart; e++) {
                    const float* a = t.v[e];
                    const float* b = t.v[(e + 1) % 3];
                    const float* c = t.v[(e + 2) % 3];
                    float nx = -(b[2] - a[2]), nz = b[0] - a[0];
                    float sc = nx * (c[0] - a[0]) + nz * (c[2] - a[2]);
                    if (sc == 0) continue;
                    bool all_out = true;
                    for (const auto& p : corners) {
                        float sp = nx * (p[0] - a[0]) + nz * (p[1] - a[2]);
                        if ((sc > 0 && sp >= 0) || (sc < 0 && sp <= 0)) {
                            all_out = false;
                            break;
                        }
                    }
                    apart = all_out;
                }
                if (apart) continue;
                float bound = miny;
                if (planar) {
                    float plane_min = 1e30f;
                    for (const auto& p : corners) {
                        float px = p[0] - ax, pz = p[1] - az;
                        float u = (px * cz - pz * cx) / det, v = (bx * pz - bz * px) / det;
                        plane_min = std::min(plane_min, t.v[0][1] + u * (t.v[1][1] - t.v[0][1]) + v * (t.v[2][1] - t.v[0][1]));
                    }
                    bound = std::max(bound, plane_min);
                }
                float& f = floor_h[g.at(x, z)];
                f = std::min(f, bound);
            }
        }
    }

    // 2. Heights and colors everywhere.
    std::vector<float> height(cells);
    for (size_t i = 0; i < cells; i++) height[i] = covered[i] ? low[i] : 0.0f;
    {
        std::vector<float> w = weight;
        pull_push(g.nx, g.nz, 1, height, w);
        w = weight;
        pull_push(g.nx, g.nz, 3, rgb, w);
    }

    // 3. Textures: the ground textures covering the most cells (up to max_kinds, each at least min_share of the
    //    ground), each one's coverage filled in by pull-push; a cell takes the strongest. Big areas win far out and
    //    the regions' edges are smooth, where the nearest texture drew a road on out as a stripe.
    std::vector<int> vote(cells, -1);
    {
        std::vector<size_t> count(mesh.textures.size(), 0);
        size_t labeled = 0;
        for (size_t i = 0; i < cells; i++) {
            if (covered[i] && label[i] >= 0) {
                count[size_t(label[i])]++;
                labeled++;
            }
        }
        std::vector<int> kinds;
        for (size_t t = 0; t < count.size(); t++) if (count[t] > 0) kinds.push_back((int)t);
        std::sort(kinds.begin(), kinds.end(), [&](int a, int b) { return count[size_t(a)] > count[size_t(b)]; });
        while (kinds.size() > 1 && (kinds.size() > max_kinds || count[size_t(kinds.back())] < labeled * min_share)) kinds.pop_back();
        if (!kinds.empty()) {
            const int n = (int)kinds.size();
            std::vector<int> kind_of(mesh.textures.size(), -1);
            for (int k = 0; k < n; k++) kind_of[size_t(kinds[size_t(k)])] = k;
            std::vector<float> share(cells * n, 0.0f), w(cells, 0.0f);
            for (size_t i = 0; i < cells; i++) {
                if (!covered[i] || label[i] < 0 || kind_of[size_t(label[i])] < 0) continue;
                share[i * n + kind_of[size_t(label[i])]] = 1.0f;
                w[i] = 1.0f;
            }
            pull_push(g.nx, g.nz, n, share, w);
            for (size_t i = 0; i < cells; i++) {
                int best = 0;
                for (int k = 1; k < n; k++) if (share[i * n + k] > share[i * n + best]) best = k;
                vote[i] = kinds[size_t(best)];
            }
        }
    }
    std::deque<size_t> queue;

    // 4. Which cells are drawn. Distance (in cells, 8-neighbour) to the nearest covered cell, up to the band.
    std::vector<uint8_t> dist(cells, 255);
    for (size_t i = 0; i < cells; i++) {
        if (covered[i]) {
            dist[i] = 0;
            queue.push_back(i);
        }
    }
    while (!queue.empty()) {
        size_t i = queue.front();
        queue.pop_front();
        if (dist[i] >= band) continue;
        int x = int(i % g.nx), z = int(i / g.nx);
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                int xx = x + dx, zz = z + dz;
                if (xx < 0 || zz < 0 || xx >= g.nx || zz >= g.nz) continue;
                size_t j = g.at(xx, zz);
                if (dist[j] != 255) continue;
                dist[j] = uint8_t(dist[i] + 1);
                queue.push_back(j);
            }
        }
    }
    // Fine: the empty cells in the band, and the covered ones touching one.
    std::vector<uint8_t> fine(cells, 0);
    for (int z = 0; z < g.nz; z++) {
        for (int x = 0; x < g.nx; x++) {
            size_t i = g.at(x, z);
            if (!covered[i]) {
                fine[i] = dist[i] <= band;
                continue;
            }
            for (int dz = -1; dz <= 1 && !fine[i]; dz++) {
                for (int dx = -1; dx <= 1 && !fine[i]; dx++) {
                    int xx = x + dx, zz = z + dz;
                    fine[i] = xx >= 0 && zz >= 0 && xx < g.nx && zz < g.nz && !covered[g.at(xx, zz)];
                }
            }
        }
    }

    // Corner heights and colors: under the lowest cell around each corner.
    const int cx = g.nx + 1, cz = g.nz + 1;
    std::vector<float> corner_h(size_t(cx) * cz), corner_rgb(size_t(cx) * cz * 3);
    for (int z = 0; z < cz; z++) {
        for (int x = 0; x < cx; x++) {
            float h = 1e30f, c[3] = {};
            int n = 0;
            for (int dz = -1; dz <= 0; dz++) {
                for (int dx = -1; dx <= 0; dx++) {
                    int xx = x + dx, zz = z + dz;
                    if (xx < 0 || zz < 0 || xx >= g.nx || zz >= g.nz) continue;
                    size_t i = g.at(xx, zz);
                    h = std::min({ h, height[i], floor_h[i] });
                    for (int k = 0; k < 3; k++) c[k] += rgb[i * 3 + k];
                    n++;
                }
            }
            size_t o = size_t(z) * cx + x;
            corner_h[o] = h - fine_drop;
            for (int k = 0; k < 3; k++) corner_rgb[o * 3 + k] = c[k] / std::max(n, 1);
        }
    }

    auto make_vertex = [&](float wx, float wz, float h, const float* c, float s0, float t0, float tf) {
        Vertex v;
        v.pos[0] = wx;
        v.pos[1] = h;
        v.pos[2] = wz;
        v.st[0] = (wx - s0) / tf;
        v.st[1] = (wz - t0) / tf;
        for (int k = 0; k < 3; k++) v.rgb[k] = (uint8_t)std::clamp(int(c[k] + 0.5f), 0, 255);
        return v;
    };
    // A strip's texture origin: the texture's whole repeats, so neighbouring strips line up.
    auto origin = [&](float w, float repeat) { return std::floor(w / repeat) * repeat; };

    // Fine strips, by chunk.
    const int fcx = (g.nx + fine_chunk - 1) / fine_chunk, fcz = (g.nz + fine_chunk - 1) / fine_chunk;
    std::vector<Chunk> fine_chunks(size_t(fcx) * fcz);
    for (int z = 0; z < g.nz; z++) {
        int x = 0;
        while (x < g.nx) {
            size_t i = g.at(x, z);
            int tex = vote[i];
            if (!fine[i] || tex < 0) {
                x++;
                continue;
            }
            const Texture& t = mesh.textures[size_t(tex)];
            float tf = texel_ft[size_t(tex)];
            // The texels a strip spans stay under max_texels from its origin (up to one repeat before its start).
            int limit = std::clamp(int((max_texels - float(std::max(t.width, t.height))) * tf / cell), 1, max_strip);
            int chunk_end = (x / fine_chunk + 1) * fine_chunk;
            int n = 0;
            while (x + n < g.nx && x + n < chunk_end && n < limit && fine[g.at(x + n, z)] && vote[g.at(x + n, z)] == tex) n++;
            Strip strip;
            strip.texture = tex;
            float wx0 = g.x0 + x * cell, wz0 = g.z0 + z * cell;
            float s0 = origin(wx0, t.width * tf), t0 = origin(wz0, t.height * tf);
            for (int k = 0; k <= n; k++) {
                size_t a = size_t(z) * cx + x + k, b = size_t(z + 1) * cx + x + k;
                strip.top.push_back(make_vertex(wx0 + k * cell, wz0, corner_h[a], &corner_rgb[a * 3], s0, t0, tf));
                strip.bottom.push_back(make_vertex(wx0 + k * cell, wz0 + cell, corner_h[b], &corner_rgb[b * 3], s0, t0, tf));
            }
            fine_chunks[size_t(z / fine_chunk) * fcx + x / fine_chunk].strips.push_back(std::move(strip));
            st.fine += n;
            x += n;
        }
    }

    // Coarse: 256 ft cells with no ground in them that the fine layer doesn't fill.
    const int gx = g.nx / coarse_cells, gz = g.nz / coarse_cells;
    std::vector<uint8_t> coarse_on(size_t(gx) * gz, 0);
    for (int z = 0; z < gz; z++) {
        for (int x = 0; x < gx; x++) {
            bool any_ground = false, all_fine = true;
            for (int dz = 0; dz < coarse_cells; dz++) {
                for (int dx = 0; dx < coarse_cells; dx++) {
                    size_t i = g.at(x * coarse_cells + dx, z * coarse_cells + dz);
                    any_ground |= covered[i] != 0;
                    all_fine &= fine[i] != 0;
                }
            }
            coarse_on[size_t(z) * gx + x] = !any_ground && !all_fine;
        }
    }
    // Coarse corners: under every fine corner of the coarse cells around them, so a coarse quad (any mix of its
    // corners) stays under every fine corner, and so under every triangle, in its cell.
    std::vector<float> coarse_h(size_t(gx + 1) * (gz + 1)), coarse_rgb(size_t(gx + 1) * (gz + 1) * 3);
    for (int z = 0; z <= gz; z++) {
        for (int x = 0; x <= gx; x++) {
            float h = 1e30f;
            int fx = x * coarse_cells, fz = z * coarse_cells;
            for (int dz = -coarse_cells; dz <= coarse_cells; dz++) {
                for (int dx = -coarse_cells; dx <= coarse_cells; dx++) {
                    int xx = fx + dx, zz = fz + dz;
                    if (xx < 0 || zz < 0 || xx >= cx || zz >= cz) continue;
                    h = std::min(h, corner_h[size_t(zz) * cx + xx]);
                }
            }
            size_t o = size_t(z) * (gx + 1) + x, f = size_t(fz) * cx + fx;
            coarse_h[o] = h - (coarse_drop - fine_drop);
            for (int k = 0; k < 3; k++) coarse_rgb[o * 3 + k] = corner_rgb[f * 3 + k];
        }
    }
    const int ccx = (gx + coarse_chunk - 1) / coarse_chunk, ccz = (gz + coarse_chunk - 1) / coarse_chunk;
    std::vector<Chunk> coarse_chunks(size_t(ccx) * ccz);
    for (int z = 0; z < gz; z++) {
        int x = 0;
        while (x < gx) {
            int tex = vote[g.at(x * coarse_cells + coarse_cells / 2, z * coarse_cells + coarse_cells / 2)];
            if (!coarse_on[size_t(z) * gx + x] || tex < 0) {
                x++;
                continue;
            }
            const Texture& t = mesh.textures[size_t(tex)];
            // Far away a texel can be bigger: doubled until a coarse cell's texels fit.
            float tf = texel_ft[size_t(tex)];
            float room = max_texels - float(std::max(t.width, t.height));
            while (coarse / tf > room) tf *= 2;
            int limit = std::clamp(int(room * tf / coarse), 1, max_strip);
            int chunk_end = (x / coarse_chunk + 1) * coarse_chunk;
            int n = 0;
            while (x + n < gx && x + n < chunk_end && n < limit && coarse_on[size_t(z) * gx + x + n] &&
                   vote[g.at((x + n) * coarse_cells + coarse_cells / 2, z * coarse_cells + coarse_cells / 2)] == tex) n++;
            Strip strip;
            strip.texture = tex;
            float wx0 = g.x0 + x * coarse, wz0 = g.z0 + z * coarse;
            float s0 = origin(wx0, t.width * tf), t0 = origin(wz0, t.height * tf);
            for (int k = 0; k <= n; k++) {
                size_t a = size_t(z) * (gx + 1) + x + k, b = size_t(z + 1) * (gx + 1) + x + k;
                strip.top.push_back(make_vertex(wx0 + k * coarse, wz0, coarse_h[a], &coarse_rgb[a * 3], s0, t0, tf));
                strip.bottom.push_back(make_vertex(wx0 + k * coarse, wz0 + coarse, coarse_h[b], &coarse_rgb[b * 3], s0, t0, tf));
            }
            coarse_chunks[size_t(z / coarse_chunk) * ccx + x / coarse_chunk].strips.push_back(std::move(strip));
            st.coarse += n;
            x += n;
        }
    }

    // Chunks: their bounds, strips sorted by texture.
    for (auto* list : { &fine_chunks, &coarse_chunks }) {
        for (Chunk& c : *list) {
            if (c.strips.empty()) continue;
            float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
            for (const Strip& s : c.strips) {
                for (const auto* row : { &s.top, &s.bottom }) {
                    for (const Vertex& v : *row) {
                        for (int k = 0; k < 3; k++) {
                            mn[k] = std::min(mn[k], v.pos[k]);
                            mx[k] = std::max(mx[k], v.pos[k]);
                        }
                    }
                }
                st.quads += s.top.size() - 1;
                st.vertices += s.top.size() * 2;
            }
            float r2 = 0;
            for (int k = 0; k < 3; k++) {
                c.center[k] = (mn[k] + mx[k]) / 2;
                r2 += (mx[k] - mn[k]) * (mx[k] - mn[k]) / 4;
            }
            c.radius = std::sqrt(r2);
            // Vertices are s16 in 1/16 ft from the center: a chunk spanning more (a deep valley) is left out.
            if (mx[1] - mn[1] > 4000.0f) continue;
            std::stable_sort(c.strips.begin(), c.strips.end(), [](const Strip& a, const Strip& b) { return a.texture < b.texture; });
            mesh.chunks.push_back(std::move(c));
        }
    }

    st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (stats) *stats = st;
    return mesh;
}

std::vector<uint8_t> Mesh::serialize() const {
    std::vector<uint8_t> out;
    put_value(out, build_version);
    put_value(out, (uint32_t)textures.size());
    for (const Texture& t : textures) {
        put_value(out, (uint32_t)t.name.size());
        out.insert(out.end(), t.name.begin(), t.name.end());
        put_value(out, t.fmt);
        put_value(out, t.siz);
        put_value(out, t.width);
        put_value(out, t.height);
        put_value(out, t.tlut);
        put_value(out, (uint32_t)t.texels.size());
        out.insert(out.end(), t.texels.begin(), t.texels.end());
        put_value(out, (uint32_t)t.palette.size());
        for (uint16_t p : t.palette) put_value(out, p);
    }
    put_value(out, (uint32_t)chunks.size());
    for (const Chunk& c : chunks) {
        for (float f : c.center) put_value(out, f);
        put_value(out, c.radius);
        put_value(out, (uint32_t)c.strips.size());
        for (const Strip& s : c.strips) {
            put_value(out, s.texture);
            put_value(out, (uint32_t)s.top.size());
            for (const auto* row : { &s.top, &s.bottom }) {
                for (const Vertex& v : *row) put_value(out, v);
            }
        }
    }
    return out;
}

bool Mesh::deserialize(const std::vector<uint8_t>& in) {
    *this = Mesh{};
    size_t at = 0;
    uint32_t version = 0, count = 0;
    if (!get_value(in, at, version) || version != build_version || !get_value(in, at, count) || count > 4096) return false;
    textures.resize(count);
    for (Texture& t : textures) {
        uint32_t n = 0;
        if (!get_value(in, at, n) || at + n > in.size()) return false;
        t.name.assign(reinterpret_cast<const char*>(&in[at]), n);
        at += n;
        if (!get_value(in, at, t.fmt) || !get_value(in, at, t.siz) || !get_value(in, at, t.width) || !get_value(in, at, t.height) ||
            !get_value(in, at, t.tlut) || !get_value(in, at, n) || at + n > in.size()) return false;
        t.texels.assign(in.begin() + at, in.begin() + at + n);
        at += n;
        if (!get_value(in, at, n) || n > 256) return false;
        t.palette.resize(n);
        for (uint16_t& p : t.palette) if (!get_value(in, at, p)) return false;
    }
    if (!get_value(in, at, count)) return false;
    chunks.resize(count);
    for (Chunk& c : chunks) {
        uint32_t strips = 0;
        for (float& f : c.center) if (!get_value(in, at, f)) return false;
        if (!get_value(in, at, c.radius) || !get_value(in, at, strips) || strips > 100000) return false;
        c.strips.resize(strips);
        for (Strip& s : c.strips) {
            uint32_t n = 0;
            if (!get_value(in, at, s.texture) || !get_value(in, at, n) || n < 2 || n > max_strip + 1 ||
                s.texture < 0 || s.texture >= (int)textures.size()) return false;
            s.top.resize(n);
            s.bottom.resize(n);
            for (auto* row : { &s.top, &s.bottom }) {
                for (Vertex& v : *row) if (!get_value(in, at, v)) return false;
            }
        }
    }
    return at == in.size();
}

}
