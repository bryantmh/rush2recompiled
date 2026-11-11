// Building a track's ground underlay for Fill Scenery Gaps (include/scenery_fill.h,
// docs/graphics_effects.md#fill-scenery-gaps). Nothing here touches the game, so tools/car_probes_test.cpp --fill runs
// it on its own.
//
// 1. The ground: the track's upward-facing triangles as placed (rush2::catalog::for_each_placed_triangle), each cell
//    of a 32 ft grid marked covered where one lies over its center, with the lowest such surface's height, texture and
//    vertex color (the lowest, so the underlay never comes up through a bridge, a roof or a road decal). A small
//    group of cells standing well above all the ground near it is a roof with no floor under it, not ground.
// 2. Heights and colors everywhere else: filled in from the covered cells by pull-push (a pyramid of averages, then
//    each empty cell blended from the level above), so the ground carries on smoothly from its edges and flattens
//    out far away.
// 3. Textures: the few ground textures covering the most of the track, each one's coverage filled in by pull-push
//    like the heights; each cell takes the strongest, so big areas (grass, water, sand) carry on and a road reaching
//    the edge doesn't.
// 4. What is drawn: the fine layer, the cells within 10 cells (320 ft) of the ground that have none, and the covered
//    cells beside them (tucked under the real ground at the edge), out to whole 256 ft cells; the coarse layer, the
//    256 ft cells with no ground or fine cells in them, out 4000 ft past the track and a little lower. Where the fine
//    layer ends over a lower coarse one, its edge hangs a skirt down to it, so the sky never shows between them.
// 5. Never through the track: every triangle drawn (any way it faces: embankments and walls falling away from a road
//    too) sets a floor on each cell it overlaps, a bound under its lowest point there. A fine corner sits 2 ft under
//    the floors and heights of its four cells, a coarse corner 6 ft under the fine corners of the track's cells within
//    a coarse cell of it, so each quad (a mix of its corners) is under everything over its cell.
// 6. The sea: a custom track's SEA mesh (FILLSEA) and a diorama's water, at the track's own water level.
// 7. With a diorama (a Guide, src/scenery_fill_guide.cpp: the track's track select diorama laid over it): its water
//    is sea, its land's lie (times a factor fitted to the track's heights) shapes the ground past the track, sloping
//    down to the sea along the shore, and each kind of its land (park, city, beach) takes the track's commonest
//    texture in the diorama's areas of that kind whose hue is near the diorama's there, never a water or striped
//    (road) texture.
// Sources: a texture of which a triangle maps only a rectangle (a page of pictures, as the PlayStation SF Rush's) is
// cut down to that rectangle, so the underlay repeats one picture rather than the page.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <tuple>

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
    constexpr const char* sea_model = "FILLSEA";   // A custom track's SEA mesh (src/track_mod/track_mod_convert.cpp)
    constexpr float sea_clear = 30.0f;       // Feet: the track's ground higher over the sea is a deck on it
    constexpr float max_stripes = 2.0f;      // A diorama's land never takes a texture this striped (a road's lanes)

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

Mesh build(const std::vector<const Container*>& containers, const std::vector<Placement>& placements, const Guide* guide,
           BuildStats* stats) {
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
    // A source: a texture, or where a triangle maps only a rectangle of it (a texture page holding many pictures, as
    // the PlayStation SF Rush's: each face a quad mapping one picture), that rectangle alone, so the underlay repeats
    // the picture and not the page.
    struct Source {
        const Container* container;
        int texture;
        int x0 = 0, y0 = 0, sx = 0, sy = 0;     // The texels used (sx 0: all of it)
        double world_area = 0, uv_area = 0;     // uv_area in the texture's texels
    };
    std::map<std::tuple<const Container*, int, int, int, int, int>, int> source_index;
    std::vector<Source> sources;
    auto add_source = [&](const Container* c, int t, const rush2::catalog::Vertex* const v[3], double world_area) {
        const auto& tex = c->textures[size_t(t)];
        int x0 = 0, y0 = 0, sx = 0, sy = 0;
        // Per axis: the corners' span in texels (whole repeats taken out), rounded to whole texels; the whole axis
        // when that is most of it, or it crosses the texture's edge.
        auto axis = [&](int k, uint32_t size, int& at, int& span) {
            float a = v[0]->uv[k] * size, b = v[1]->uv[k] * size, cc = v[2]->uv[k] * size;
            float lo = std::min({ a, b, cc }), hi = std::max({ a, b, cc });
            float shift = std::floor((lo + 0.5f) / size) * size;
            int l = std::max(0, int(std::lround(lo - shift))), h = int(std::lround(hi - shift));
            if (h - l < 4 || h - l > int(size) * 3 / 4 || h > int(size)) return false;
            at = l;
            span = h - l;
            return true;
        };
        bool cx = axis(0, tex.width, x0, sx), cy = axis(1, tex.height, y0, sy);
        if (!cx && !cy) x0 = y0 = sx = sy = 0;
        else {
            if (!cx) x0 = 0, sx = int(tex.width);
            if (!cy) y0 = 0, sy = int(tex.height);
        }
        auto [it, added] = source_index.emplace(std::make_tuple(c, t, x0, y0, sx, sy), (int)sources.size());
        if (added) sources.push_back({ c, t, x0, y0, sx, sy });
        Source& src = sources[size_t(it->second)];
        src.world_area += world_area;
        src.uv_area += std::fabs((v[1]->uv[0] - v[0]->uv[0]) * (v[2]->uv[1] - v[0]->uv[1]) - (v[2]->uv[0] - v[0]->uv[0]) * (v[1]->uv[1] - v[0]->uv[1])) / 2 *
                       double(tex.width) * tex.height;
        return it->second;
    };
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
            g.texture = add_source(pt.container, t, pt.source, len / 2);
        }
        for (int v = 0; v < 3; v++) {
            lo[0] = std::min(lo[0], g.v[v][0]);
            lo[1] = std::min(lo[1], g.v[v][2]);
            hi[0] = std::max(hi[0], g.v[v][0]);
            hi[1] = std::max(hi[1], g.v[v][2]);
        }
        ground.push_back(g);
    });
    // The sea: the track container's model sea_model, never placed (the game never draws it), its vertices in feet.
    std::vector<Ground> sea;
    if (!containers.empty() && containers[0]) {
        const Container* c = containers[0];
        for (const auto& model : c->models) {
            if (model.name != sea_model || model.lods.empty()) continue;
            for (const auto& m : model.lods[0].meshes) {
                for (size_t i = 0; i + 2 < m.vertices.size(); i += 3) {
                    Ground s;
                    for (int v = 0; v < 3; v++) {
                        for (int k = 0; k < 3; k++) {
                            s.v[v][k] = m.vertices[i + v].position[k];
                            s.rgb[k] = (v == 0 ? 0.0f : s.rgb[k]) + m.vertices[i + v].color[k] / 3.0f;
                        }
                    }
                    s.texture = -1;
                    if (m.texture >= 0 && m.texture < (int)c->textures.size()) {
                        const auto* a = &m.vertices[i];
                        const rush2::catalog::Vertex* const corners[3] = { &a[0], &a[1], &a[2] };
                        float e1[3], e2[3];
                        for (int k = 0; k < 3; k++) {
                            e1[k] = a[1].position[k] - a[0].position[k];
                            e2[k] = a[2].position[k] - a[0].position[k];
                        }
                        float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
                        s.texture = add_source(c, m.texture, corners, std::sqrt(nx * nx + ny * ny + nz * nz) / 2);
                    }
                    sea.push_back(s);
                }
            }
        }
    }
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
        const Source& src = sources[s];
        const rush2::catalog::Texture& whole = src.container->textures[size_t(src.texture)];
        double resample = 1.0;    // Texels of the texture drawn per texel of the source
        if (src.sx == 0) {
            if (!encode_texture(whole, t)) continue;
        }
        else {
            // The rectangle, resampled (nearest) to powers of two (16 to 64 texels a side) to repeat.
            auto pow2 = [](int n) {
                int p = 16;
                while (p < 64 && p * 3 / 2 < n) p *= 2;
                return p;
            };
            const int w = pow2(src.sx), h = pow2(src.sy);
            resample = std::sqrt(double(w) * h / (double(src.sx) * src.sy));
            rush2::catalog::Texture part = whole;
            part.name += "@" + std::to_string(src.x0) + "," + std::to_string(src.y0) + "," + std::to_string(src.sx) + "," + std::to_string(src.sy);
            part.width = uint32_t(w);
            part.height = uint32_t(h);
            part.image.width = part.width;
            part.image.height = part.height;
            part.image.rgba.assign(size_t(w) * h * 4, 0);
            if (!whole.indices.empty()) part.indices.assign(size_t(w) * h, 0);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int fx = std::min(src.x0 + x * src.sx / w, int(whole.width) - 1), fy = std::min(src.y0 + y * src.sy / h, int(whole.height) - 1);
                    size_t from = size_t(fy) * whole.width + fx, to = size_t(y) * w + x;
                    if (from * 4 + 3 >= whole.image.rgba.size()) continue;
                    memcpy(&part.image.rgba[to * 4], &whole.image.rgba[from * 4], 4);
                    if (!whole.indices.empty() && from < whole.indices.size()) part.indices[to] = whole.indices[from];
                }
            }
            if (!encode_texture(part, t)) continue;
        }
        drawable[s] = (int)mesh.textures.size();
        double per_texel = src.uv_area > 1e-9 ? std::sqrt(src.world_area / src.uv_area) / resample : 0.5;
        texel_ft.push_back(std::clamp((float)per_texel, 0.05f, 16.0f));
        mesh.textures.push_back(std::move(t));
    }
    // Each texture's mean color, and whether it is water (blue).
    std::vector<std::array<float, 3>> tex_rgb(mesh.textures.size());
    std::vector<char> blue(mesh.textures.size(), 0);
    std::vector<float> stripes(mesh.textures.size(), 1.0f);
    for (size_t t = 0; t < mesh.textures.size(); t++) {
        auto img = image_of(mesh.textures[t]);
        double c[3] = {};
        size_t n = std::max<size_t>(img.rgba.size() / 4, 1);
        for (size_t p = 0; p < img.rgba.size() / 4; p++) for (int k = 0; k < 3; k++) c[k] += img.rgba[p * 4 + k];
        for (int k = 0; k < 3; k++) tex_rgb[t][k] = float(c[k] / n);
        blue[t] = tex_rgb[t][2] > tex_rgb[t][0] * 1.25f + 20.0f && tex_rgb[t][2] >= tex_rgb[t][1];
        // Stripes: how much more the texels change across one axis than along the other (a road's lane lines).
        double across[2] = {};
        const uint32_t w = img.width, h = img.height;
        auto lum = [&](uint32_t x, uint32_t y) {
            const uint8_t* p = &img.rgba[(size_t(y % h) * w + x % w) * 4];
            return 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2];
        };
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                across[0] += std::fabs(lum(x + 1, y) - lum(x, y));
                across[1] += std::fabs(lum(x, y + 1) - lum(x, y));
            }
        }
        stripes[t] = float((std::max(across[0], across[1]) + 1.0) / (std::min(across[0], across[1]) + 1.0));
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
    // 1a. Roofs with no floor under them: a small group of covered cells (joined where neighbors' heights are within
    //     roof_join) standing over roof_rise above all the ground within roof_reach of it (covered cells outside the
    //     group, water left out) is a roof, not ground: it stops counting as covered, and the ground round it fills
    //     in under it (the floors keep it under the roof). A group with no such ground near (an island, a pier over
    //     the sea plane) stays ground. The PlayStation Golden Gate's rotunda has no floor under its dome, so the dome
    //     was the lowest surface in its cells and the underlay rose to it as a green cone (reported 2026-11-10).
    {
        constexpr float roof_join = 24.0f;       // Feet between neighboring cells of one surface (a dome's curve)
        constexpr float roof_rise = 20.0f;       // Feet a roof stands over all the ground near it, at least
        constexpr size_t roof_cells = 100;       // A roof's cells at most (320 x 320 ft)
        constexpr int roof_reach = 4;            // Cells round a roof searched for the ground under it (128 ft)
        std::vector<int> group(cells, -1);
        std::vector<size_t> members, queue;
        int groups = 0;
        for (size_t s = 0; s < cells; s++) {
            if (weight[s] <= 0 || group[s] >= 0) continue;
            members.clear();
            queue.assign(1, s);
            group[s] = groups;
            auto neighbors = [&](size_t i, int reach, auto&& fn) {
                int x = int(i % size_t(g.nx)), z = int(i / size_t(g.nx));
                for (int dz = -reach; dz <= reach; dz++) {
                    for (int dx = -reach; dx <= reach; dx++) {
                        int xx = x + dx, zz = z + dz;
                        if ((dx == 0 && dz == 0) || xx < 0 || zz < 0 || xx >= g.nx || zz >= g.nz) continue;
                        size_t j = g.at(xx, zz);
                        if (weight[j] > 0) fn(j);
                    }
                }
            };
            while (!queue.empty()) {
                size_t i = queue.back();
                queue.pop_back();
                members.push_back(i);
                neighbors(i, 1, [&](size_t j) {
                    if (group[j] < 0 && std::fabs(low[j] - low[i]) <= roof_join) {
                        group[j] = groups;
                        queue.push_back(j);
                    }
                });
            }
            const int id = groups++;
            if (members.size() > roof_cells) continue;
            float group_min = 1e30f, border_max = -1e30f;
            for (size_t i : members) {
                group_min = std::min(group_min, low[i]);
                neighbors(i, roof_reach, [&](size_t j) {
                    if (group[j] != id && !(label[j] >= 0 && blue[size_t(label[j])])) border_max = std::max(border_max, low[j]);
                });
            }
            if (border_max < -1e29f || group_min < border_max + roof_rise) continue;
            st.roofs += members.size();
            for (size_t i : members) {
                weight[i] = 0.0f;
                low[i] = 1e30f;
                label[i] = -1;
                for (int k = 0; k < 3; k++) rgb[i * 3 + k] = 0.0f;
            }
        }
    }
    // 1b. The sea (a custom track's SEA mesh, model sea_model): a cell over it is sea, at its height and with its
    //     texture, unless the track's own ground there is within sea_clear of it (a quay, a ship's deck: the track's
    //     ground is drawn). Ground higher over the sea is a deck on it (a bridge, an elevated road): the sea is drawn
    //     under it. Sea cells feed the heights and textures, but only the track's own ground counts as covered for
    //     what is drawn, so the sea far from the track is drawn coarse.
    std::vector<float> sea_h(cells, 1e30f);
    std::vector<int> sea_label(cells, -1);
    std::vector<float> sea_rgb(cells * 3, 0.0f);
    for (const Ground& t : sea) {
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
                if (y < sea_h[i]) {
                    sea_h[i] = y;
                    sea_label[i] = t.texture >= 0 ? drawable[size_t(t.texture)] : -1;
                    for (int k = 0; k < 3; k++) sea_rgb[i * 3 + k] = t.rgb[k];
                }
            }
        }
    }
    // 1c. The diorama (a Guide): each cell's kind of land and the lowest of its land's surfaces there (a building
    //     stands on the ground, so the lowest is the ground), or water where only water is under the cell center.
    //     Past the diorama's edge both carry on from it (the sea round an island stays sea).
    std::vector<float> guide_h(cells, 1e30f);
    std::vector<uint8_t> guide_kind(cells, uint8_t(Kind::none)), guide_direct(cells, 0);
    std::vector<uint8_t> guide_rgb(cells * 3, 0);   // The diorama's color there
    constexpr int guide_kinds = 5;   // Kind::none .. Kind::sand
    if (guide) {
        std::vector<uint8_t> wet(cells, 0);
        for (const Guide::Triangle& t : guide->triangles) {
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
                    size_t i = g.at(x, z);
                    if (t.kind == Kind::route) continue;
                    if (t.kind == Kind::water) {
                        wet[i] = 1;
                        continue;
                    }
                    float y = t.v[0][1] + u * (t.v[1][1] - t.v[0][1]) + v * (t.v[2][1] - t.v[0][1]);
                    if (y < guide_h[i]) {
                        guide_h[i] = y;
                        guide_kind[i] = uint8_t(t.kind);
                        for (int k = 0; k < 3; k++) guide_rgb[i * 3 + k] = t.rgb[k];
                    }
                }
            }
        }
        std::vector<float> share(cells * guide_kinds, 0.0f), w(cells, 0.0f), h(cells, 0.0f), hw(cells, 0.0f);
        for (size_t i = 0; i < cells; i++) {
            if (guide_h[i] < 1e29f) {
                h[i] = guide_h[i];
                hw[i] = 1.0f;
            }
            else if (wet[i]) guide_kind[i] = uint8_t(Kind::water);
            if (guide_kind[i] != uint8_t(Kind::none)) {
                share[i * guide_kinds + guide_kind[i]] = 1.0f;
                w[i] = 1.0f;
                guide_direct[i] = 1;
                st.guided++;
            }
        }
        if (st.guided > 0) {
            pull_push(g.nx, g.nz, guide_kinds, share, w);
            for (size_t i = 0; i < cells; i++) {
                int best = 1;
                for (int k = 2; k < guide_kinds; k++) if (share[i * guide_kinds + k] > share[i * guide_kinds + best]) best = k;
                if (!guide_direct[i]) guide_kind[i] = uint8_t(best);
            }
            pull_push(g.nx, g.nz, 1, h, hw);
            guide_h = h;
        }
    }
    // The diorama's sea, where the track has no ground: at the track's own water's height with its texture (the
    // track's ground over the diorama's water with a blue texture, the commonest), else the SEA mesh's, else a flat
    // texture of the guide's water color a little under the lowest ground near the shore.
    if (guide && st.guided > 0) {
        float level = 1e30f;
        int water_label = -1;
        float water_rgb[3] = { 255, 255, 255 };
        std::vector<size_t> blue_count(mesh.textures.size(), 0);
        for (size_t i = 0; i < cells; i++) {
            if (weight[i] > 0 && guide_direct[i] && guide_kind[i] == uint8_t(Kind::water) && label[i] >= 0 && blue[size_t(label[i])]) {
                blue_count[size_t(label[i])]++;
            }
        }
        size_t best = 0;
        for (size_t t = 0; t < blue_count.size(); t++) {
            if (blue_count[t] > best) {
                best = blue_count[t];
                water_label = (int)t;
            }
        }
        if (best >= 8) {
            std::vector<float> hs;
            double c[3] = {};
            for (size_t i = 0; i < cells; i++) {
                if (weight[i] > 0 && guide_direct[i] && guide_kind[i] == uint8_t(Kind::water) && label[i] == water_label) {
                    hs.push_back(low[i]);
                    for (int k = 0; k < 3; k++) c[k] += rgb[i * 3 + k];
                }
            }
            std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
            level = hs[hs.size() / 2];
            for (int k = 0; k < 3; k++) water_rgb[k] = float(c[k] / hs.size());
        }
        else {
            water_label = -1;
            std::vector<float> hs;
            std::map<int, size_t> labels;
            for (size_t i = 0; i < cells; i++) {
                if (sea_h[i] < 1e29f) {
                    hs.push_back(sea_h[i]);
                    labels[sea_label[i]]++;
                }
            }
            if (!hs.empty()) {
                std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
                level = hs[hs.size() / 2];
                size_t most = 0;
                for (const auto& [l, n] : labels) {
                    if (n > most) {
                        most = n;
                        water_label = l;
                    }
                }
            }
            else {
                // The lowest ground within 320 ft of the diorama's water.
                for (int z = 0; z < g.nz; z++) {
                    for (int x = 0; x < g.nx; x++) {
                        size_t i = g.at(x, z);
                        if (weight[i] <= 0) continue;
                        bool near = false;
                        for (int dz = -band; dz <= band && !near; dz += 2) {
                            for (int dx = -band; dx <= band && !near; dx += 2) {
                                int xx = x + dx, zz = z + dz;
                                near = xx >= 0 && zz >= 0 && xx < g.nx && zz < g.nz && guide_direct[g.at(xx, zz)] &&
                                       guide_kind[g.at(xx, zz)] == uint8_t(Kind::water);
                            }
                        }
                        if (near) level = std::min(level, low[i] - 4.0f);
                    }
                }
                if (level > 1e29f) {
                    for (size_t i = 0; i < cells; i++) if (weight[i] > 0) level = std::min(level, low[i] - 20.0f);
                }
                Texture t;
                t.name = "FILLWATER";
                t.fmt = fmt_rgba;
                t.siz = siz_16b;
                t.width = t.height = 16;
                t.texels.resize(16 * 16 * 2);
                const uint8_t* wc = guide->water;
                for (int p = 0; p < 16 * 16; p++) {
                    // A faint ripple, so it reads as water.
                    int d = ((p * 7 + (p / 16) * 3) % 5) - 2;
                    auto ch = [&](int k) { return uint32_t(std::clamp(int(wc[k]) + d * 3, 0, 255) >> 3); };
                    uint16_t v = uint16_t((ch(0) << 11) | (ch(1) << 6) | (ch(2) << 1) | 1);
                    t.texels[size_t(p) * 2] = uint8_t(v >> 8);
                    t.texels[size_t(p) * 2 + 1] = uint8_t(v);
                }
                water_label = (int)mesh.textures.size();
                mesh.textures.push_back(std::move(t));
                texel_ft.push_back(4.0f);
            }
        }
        for (size_t i = 0; i < cells; i++) {
            if (weight[i] > 0 || sea_h[i] < 1e29f || guide_kind[i] != uint8_t(Kind::water)) continue;
            sea_h[i] = level;
            sea_label[i] = water_label;
            for (int k = 0; k < 3; k++) sea_rgb[i * 3 + k] = water_rgb[k];
        }
    }
    std::vector<uint8_t> covered(cells, 0), is_sea(cells, 0);
    for (size_t i = 0; i < cells; i++) {
        covered[i] = weight[i] > 0;
        if (sea_h[i] < 1e29f && (!covered[i] || low[i] > sea_h[i] + sea_clear)) {
            covered[i] = 0;
            low[i] = sea_h[i];
            label[i] = sea_label[i];
            for (int k = 0; k < 3; k++) rgb[i * 3 + k] = sea_rgb[i * 3 + k];
            weight[i] = 1.0f;
            is_sea[i] = 1;
            st.sea++;
        }
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
    for (size_t i = 0; i < cells; i++) height[i] = weight[i] > 0 ? low[i] : 0.0f;
    if (guide && st.guided > 0) {
        // With a diorama: its lie, times a factor fitted to the track's ground where both are (a diorama's hills are
        // taller than the track's), plus what the track's ground differs from that, filled in from where it is known
        // (so the ground meets the track's edges and the diorama's hills and valleys go on past them).
        double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
        for (size_t i = 0; i < cells; i++) {
            if (!covered[i] || !guide_direct[i] || guide_kind[i] == uint8_t(Kind::water)) continue;
            sx += guide_h[i];
            sy += low[i];
            sxx += double(guide_h[i]) * guide_h[i];
            sxy += double(guide_h[i]) * low[i];
            n++;
        }
        float a = 0;
        if (n > 50) {
            double var = sxx / n - (sx / n) * (sx / n), cov = sxy / n - (sx / n) * (sy / n);
            if (var > 1.0) a = (float)std::clamp(cov / var, 0.0, 1.0);
        }
        st.lie = a;
        std::vector<float> rest(cells, 0.0f), w = weight;
        for (size_t i = 0; i < cells; i++) {
            if (is_sea[i]) w[i] = 0.0f;   // The sea's level isn't the land's
            else if (weight[i] > 0) rest[i] = low[i] - a * guide_h[i];
        }
        pull_push(g.nx, g.nz, 1, rest, w);
        for (size_t i = 0; i < cells; i++) if (weight[i] <= 0) height[i] = a * guide_h[i] + rest[i];
        // The shore: land within shore_cells of the sea comes down to the sea's level there.
        constexpr int shore_cells = 12;
        std::vector<uint8_t> to_sea(cells, 255);
        std::vector<float> sea_level(cells, 0.0f);
        std::deque<size_t> q;
        for (size_t i = 0; i < cells; i++) {
            if (!is_sea[i]) continue;
            to_sea[i] = 0;
            sea_level[i] = low[i];
            q.push_back(i);
        }
        while (!q.empty()) {
            size_t i = q.front();
            q.pop_front();
            if (to_sea[i] >= shore_cells) continue;
            int x = int(i % g.nx), z = int(i / g.nx);
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, zz = z + dz;
                    if (xx < 0 || zz < 0 || xx >= g.nx || zz >= g.nz) continue;
                    size_t j = g.at(xx, zz);
                    if (to_sea[j] != 255) continue;
                    to_sea[j] = uint8_t(to_sea[i] + 1);
                    sea_level[j] = sea_level[i];
                    q.push_back(j);
                }
            }
        }
        for (size_t i = 0; i < cells; i++) {
            if (weight[i] > 0 || to_sea[i] == 255 || to_sea[i] == 0) continue;
            float t = float(to_sea[i]) / shore_cells;
            t = t * t * (3 - 2 * t);
            height[i] = sea_level[i] + (height[i] - sea_level[i]) * t;
        }
        std::vector<float> sea_shade(rgb);
        w = weight;
        for (size_t i = 0; i < cells; i++) if (is_sea[i]) w[i] = 0.0f;
        pull_push(g.nx, g.nz, 3, rgb, w);
        for (size_t i = 0; i < cells; i++) if (is_sea[i]) for (int k = 0; k < 3; k++) rgb[i * 3 + k] = sea_shade[i * 3 + k];
    }
    else {
        std::vector<float> w = weight;
        pull_push(g.nx, g.nz, 1, height, w);
        // Colors from the track's ground alone (the sea's shade would light up the land), the sea keeping its own.
        std::vector<float> sea_shade(rgb);
        w = weight;
        for (size_t i = 0; i < cells; i++) if (is_sea[i]) w[i] = 0.0f;
        pull_push(g.nx, g.nz, 3, rgb, w);
        for (size_t i = 0; i < cells; i++) if (is_sea[i]) for (int k = 0; k < 3; k++) rgb[i * 3 + k] = sea_shade[i * 3 + k];
    }

    // 3. Textures: the ground textures covering the most cells (up to max_kinds, each at least min_share of the
    //    ground), each one's coverage filled in by pull-push; a cell takes the strongest. Big areas win far out and
    //    the regions' edges are smooth, where the nearest texture drew a road on out as a stripe.
    std::vector<int> vote(cells, -1);
    {
        std::vector<size_t> count(mesh.textures.size(), 0);
        size_t labeled = 0;
        for (size_t i = 0; i < cells; i++) {
            if (weight[i] > 0 && !is_sea[i] && label[i] >= 0) {
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
                if (weight[i] <= 0 || is_sea[i] || label[i] < 0 || kind_of[size_t(label[i])] < 0) continue;
                share[i * n + kind_of[size_t(label[i])]] = 1.0f;
                w[i] = 1.0f;
            }
            std::vector<float> base_share = share, base_w = w;
            pull_push(g.nx, g.nz, n, share, w);
            for (size_t i = 0; i < cells; i++) {
                int best = 0;
                for (int k = 1; k < n; k++) if (share[i * n + k] > share[i * n + best]) best = k;
                vote[i] = kinds[size_t(best)];
            }
            // With a diorama, each kind of land (park, city, beach) takes the texture of the track's ground in the
            // diorama's areas of that kind (the commonest there), so a park goes on as grass and a city as paving; a
            // kind the track's ground never reaches keeps the vote above. Striped textures are left out (a road's lane
            // lines: a city's streets, repeated over a city, drew lane lines across it).
            if (guide && st.guided > 0) {
                // The hue (color over brightness, so a night or snow version's darker or whiter ground compares
                // alike) of the diorama's areas of a kind against each texture's: a texture far from it counts
                // for less (a grey city diorama goes on as concrete rather than red brick); water never is land.
                auto hue = [](const float* c, float out[3]) {
                    float sum = c[0] + c[1] + c[2] + 1.0f;
                    for (int k = 0; k < 3; k++) out[k] = c[k] / sum;
                };
                for (int kind = int(Kind::green); kind < guide_kinds; kind++) {
                    std::vector<double> total(n, 0.0);
                    double kind_rgb[3] = {};
                    size_t samples = 0, shown = 0;
                    for (size_t i = 0; i < cells; i++) {
                        if (guide_direct[i] && guide_kind[i] == kind) {
                            for (int k = 0; k < 3; k++) kind_rgb[k] += guide_rgb[i * 3 + k];
                            shown++;
                        }
                        if (base_w[i] <= 0 || guide_kind[i] != kind) continue;
                        for (int k = 0; k < n; k++) total[k] += base_share[i * n + k];
                        samples++;
                    }
                    float want[3] = {}, kc[3];
                    for (int k = 0; k < 3; k++) kc[k] = float(kind_rgb[k] / std::max<size_t>(shown, 1));
                    hue(kc, want);
                    int best = -1;
                    for (int k = 0; k < n; k++) {
                        size_t tx = size_t(kinds[size_t(k)]);
                        if (total[k] <= 0 || tx >= tex_rgb.size() || blue[tx] || stripes[tx] >= max_stripes) {
                            total[k] = 0;
                            continue;
                        }
                        float have[3];
                        hue(tex_rgb[tx].data(), have);
                        float d = std::sqrt((have[0] - want[0]) * (have[0] - want[0]) + (have[1] - want[1]) * (have[1] - want[1]) +
                                            (have[2] - want[2]) * (have[2] - want[2]));
                        total[k] /= 1.0 + (d / 0.1f) * (d / 0.1f);
                        if (best < 0 || total[k] > total[best]) best = k;
                    }
                    if (samples < 20 || best < 0) continue;
                    for (size_t i = 0; i < cells; i++) if (guide_kind[i] == kind) vote[i] = kinds[size_t(best)];
                }
            }
            for (size_t i = 0; i < cells; i++) {
                if (is_sea[i] && label[i] >= 0) vote[i] = label[i];   // the sea is water wherever it is
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
    // ... out to whole coarse cells, so a coarse cell is drawn all fine or all coarse and the two never overlap (a
    // coarse quad under fine ones had to come down to the lowest of them: on a shore, the sea's level, so the land
    // over it stood on a wall).
    for (int z = 0; z + coarse_cells <= g.nz; z += coarse_cells) {
        for (int x = 0; x + coarse_cells <= g.nx; x += coarse_cells) {
            bool any = false;
            for (int dz = 0; dz < coarse_cells && !any; dz++) {
                for (int dx = 0; dx < coarse_cells && !any; dx++) any = fine[g.at(x + dx, z + dz)] != 0;
            }
            if (!any) continue;
            for (int dz = 0; dz < coarse_cells; dz++) {
                for (int dx = 0; dx < coarse_cells; dx++) {
                    size_t i = g.at(x + dx, z + dz);
                    if (!covered[i]) fine[i] = 1;
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
            int fx = x * coarse_cells, fz = z * coarse_cells;
            float h = corner_h[size_t(fz) * cx + fx];
            for (int dz = -coarse_cells; dz <= coarse_cells; dz++) {
                for (int dx = -coarse_cells; dx <= coarse_cells; dx++) {
                    int xx = fx + dx, zz = fz + dz;
                    if (xx < 0 || zz < 0 || xx >= cx || zz >= cz) continue;
                    // Only under the track: its ground or any of its triangles touching the corner (the fine layer
                    // never overlaps the coarse one, and far out the coarse ground follows its own heights).
                    bool drawn = false;
                    for (int ez = -1; ez <= 0 && !drawn; ez++) {
                        for (int ex = -1; ex <= 0 && !drawn; ex++) {
                            int ax = xx + ex, az = zz + ez;
                            if (ax < 0 || az < 0 || ax >= g.nx || az >= g.nz) continue;
                            size_t i = g.at(ax, az);
                            drawn = covered[i] || floor_h[i] < 1e29f;
                        }
                    }
                    if (drawn) h = std::min(h, corner_h[size_t(zz) * cx + xx]);
                }
            }
            size_t o = size_t(z) * (gx + 1) + x, f = size_t(fz) * cx + fx;
            coarse_h[o] = h - (coarse_drop - fine_drop);
            for (int k = 0; k < 3; k++) coarse_rgb[o * 3 + k] = corner_rgb[f * 3 + k];
        }
    }
    // Skirts: where the fine layer ends on a slope, the coarse layer under it (its corners the lowest of 512 ft
    // around) starts lower, and the step between them showed the sky. Each outer edge of the fine layer hangs a wall
    // down to the coarse surface there.
    auto coarse_at = [&](float wx, float wz) {
        float fx = std::clamp((wx - g.x0) / coarse, 0.0f, float(gx) - 1e-3f), fz = std::clamp((wz - g.z0) / coarse, 0.0f, float(gz) - 1e-3f);
        int ix = int(fx), iz = int(fz);
        float tx = fx - ix, tz = fz - iz;
        auto h = [&](int a, int b) { return coarse_h[size_t(b) * (gx + 1) + a]; };
        return (h(ix, iz) * (1 - tx) + h(ix + 1, iz) * tx) * (1 - tz) + (h(ix, iz + 1) * (1 - tx) + h(ix + 1, iz + 1) * tx) * tz;
    };
    for (int z = 0; z < g.nz; z++) {
        for (int x = 0; x < g.nx; x++) {
            size_t i = g.at(x, z);
            int tex = vote[i];
            if (!fine[i] || tex < 0) continue;
            const Texture& t = mesh.textures[size_t(tex)];
            float tf = texel_ft[size_t(tex)];
            // Each side (its two corners, in the cell's corner grid) whose neighbour is neither fine nor ground.
            const int sides[4][6] = { { 0, -1, 0, 0, 1, 0 }, { 0, 1, 0, 1, 1, 1 }, { -1, 0, 0, 0, 0, 1 }, { 1, 0, 1, 0, 1, 1 } };
            for (const auto& sd : sides) {
                int nx = x + sd[0], nz = z + sd[1];
                if (nx < 0 || nz < 0 || nx >= g.nx || nz >= g.nz) continue;
                size_t j = g.at(nx, nz);
                if (fine[j] || covered[j]) continue;
                Strip strip;
                strip.texture = tex;
                float s0 = origin(g.x0 + x * cell, t.width * tf), t0 = origin(g.z0 + z * cell, t.height * tf);
                for (int e = 0; e < 2; e++) {
                    int px = x + sd[2 + e * 2], pz = z + sd[3 + e * 2];
                    float wx = g.x0 + px * cell, wz = g.z0 + pz * cell;
                    size_t c = size_t(pz) * cx + px;
                    float bottom = std::min(corner_h[c], coarse_at(wx, wz) - 1.0f);
                    strip.top.push_back(make_vertex(wx, wz, corner_h[c], &corner_rgb[c * 3], s0, t0, tf));
                    strip.bottom.push_back(make_vertex(wx, wz, bottom, &corner_rgb[c * 3], s0, t0, tf));
                }
                if (strip.top[0].pos[1] - strip.bottom[0].pos[1] < 0.5f && strip.top[1].pos[1] - strip.bottom[1].pos[1] < 0.5f) continue;
                fine_chunks[size_t(z / fine_chunk) * fcx + x / fine_chunk].strips.push_back(std::move(strip));
            }
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
    // Only the textures drawn are kept (every texture and picture of a page the ground uses was a candidate).
    {
        std::vector<int> remap(mesh.textures.size(), -1);
        std::vector<Texture> kept;
        for (Chunk& c : mesh.chunks) {
            for (Strip& s : c.strips) {
                int& r = remap[size_t(s.texture)];
                if (r < 0) {
                    r = (int)kept.size();
                    kept.push_back(mesh.textures[size_t(s.texture)]);
                }
                s.texture = r;
            }
            std::stable_sort(c.strips.begin(), c.strips.end(), [](const Strip& a, const Strip& b) { return a.texture < b.texture; });
        }
        mesh.textures = std::move(kept);
    }

    // The track's one-way road blocks: their texels and their mipmap levels' (named after them, ".mip2"...).
    if (!containers.empty() && containers[0]) {
        const auto& textures = containers[0]->textures;
        for (size_t i = 0; i < textures.size(); i++) {
            if (std::find(std::begin(one_sided_hashes), std::end(one_sided_hashes), textures[i].hash) == std::end(one_sided_hashes)) continue;
            const std::string mip = textures[i].name + ".mip";
            for (size_t j = 0; j < textures.size(); j++) {
                if ((j == i || textures[j].name.rfind(mip, 0) == 0) && textures[j].texels != UINT32_MAX &&
                    std::find(mesh.one_sided.begin(), mesh.one_sided.end(), textures[j].texels) == mesh.one_sided.end()) {
                    mesh.one_sided.push_back(textures[j].texels);
                }
            }
        }
    }

    st.seconds =std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
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
    put_value(out, (uint32_t)one_sided.size());
    for (uint32_t o : one_sided) put_value(out, o);
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
    if (!get_value(in, at, count) || count > 256) return false;
    one_sided.resize(count);
    for (uint32_t& o : one_sided) if (!get_value(in, at, o)) return false;
    return at == in.size();
}

}
