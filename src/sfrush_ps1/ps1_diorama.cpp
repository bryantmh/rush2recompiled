// The track select diorama of the PS1 track (docs/sfrush_ps1.md#track-select-diorama): a hand-made model in the
// style of the N64 SF Rush's (asset 10: TRK1O2 ...), not a shrunk copy of the track (that reads as noise at diorama
// size: src/rush2049/track2049_art.cpp use_miniature). An island of city ground on a dirt base, the route in red on
// top, a few dozen blocky buildings with window textures and the course's landmarks, all laid out by hand in
// assets/sfrush_ps1/chinatown_diorama.json (glb meters, the frame of the track's track.glb: tools/diorama_map.py draws
// that frame with a grid to place things by). The route comes from the disc's AI path; the textures are painted here,
// so nothing of the disc but the route's line is in the model.
//
// The model is written as the mod track's diorama.glb ([menu] diorama), which the custom track converter fits to the
// stock dioramas' size (src/track_mod/track_mod_convert.cpp build_diorama).

#include <algorithm>
#include <cmath>
#include <fstream>

#include "sfrush_ps1.h"
#include "texture_upscale.h"
#include "clipper2/clipper.h"
#include "../arcade2049/arcade_internal.h"

namespace ps1 = rush2::sfrush_ps1;
namespace a49 = rush2::arcade2049;
using a49::V2;
using a49::V3;
using json = nlohmann::json;

namespace {
    constexpr double pi = 3.14159265358979323846;

    V3 sub(const V3& a, const V3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    V3 cross(const V3& a, const V3& b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
    double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    V3 unit(const V3& a) {
        double l = std::sqrt(dot(a, a));
        return l > 1e-12 ? V3{ a[0] / l, a[1] / l, a[2] / l } : V3{ 0, 1, 0 };
    }

    // ---- textures ------------------------------------------------------------------------------------------------
    // Painted in the N64 SF Rush dioramas' manner: small, noisy, a few flat colors. 64 x 64 RGBA, deterministic.
    constexpr int tex_size = 64;
    struct Paint {
        std::vector<uint8_t> px = std::vector<uint8_t>(tex_size * tex_size * 4, 255);
        uint32_t seed;
        explicit Paint(uint32_t s) : seed(s * 2654435761u + 12345u) {}
        double rnd() {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) / double(1 << 24);
        }
        void set(int x, int y, std::array<int, 3> c, int a = 255) {
            x = ((x % tex_size) + tex_size) % tex_size;
            y = ((y % tex_size) + tex_size) % tex_size;
            uint8_t* p = &px[size_t(y * tex_size + x) * 4];
            for (int k = 0; k < 3; k++) p[k] = uint8_t(std::clamp(c[k], 0, 255));
            p[3] = uint8_t(a);
        }
        std::array<int, 3> get(int x, int y) const {
            x = ((x % tex_size) + tex_size) % tex_size;
            y = ((y % tex_size) + tex_size) % tex_size;
            const uint8_t* p = &px[size_t(y * tex_size + x) * 4];
            return { p[0], p[1], p[2] };
        }
        void rect(int x0, int y0, int w, int h, std::array<int, 3> c) {
            for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) set(x, y, c);
        }
        // Every texel jittered by up to +-amount (the same for the three channels).
        void noise(int amount) {
            for (int y = 0; y < tex_size; y++) for (int x = 0; x < tex_size; x++) {
                std::array<int, 3> c = get(x, y);
                int d = int((rnd() * 2 - 1) * amount);
                set(x, y, { c[0] + d, c[1] + d, c[2] + d });
            }
        }
    };

    std::array<int, 3> mix(std::array<int, 3> a, std::array<int, 3> b, double f) {
        return { int(a[0] + (b[0] - a[0]) * f), int(a[1] + (b[1] - a[1]) * f), int(a[2] + (b[2] - a[2]) * f) };
    }

    // City seen from above: grey blocks of roofs in a grid of darker streets, a few colored roofs.
    Paint city_ground() {
        Paint p(1);
        p.rect(0, 0, tex_size, tex_size, { 96, 96, 100 });
        for (int by = 0; by < tex_size; by += 16) for (int bx = 0; bx < tex_size; bx += 16) {
            for (int k = 0; k < 6; k++) {
                int w = 3 + int(p.rnd() * 6), h = 3 + int(p.rnd() * 6);
                int x = bx + 2 + int(p.rnd() * (13 - w)), y = by + 2 + int(p.rnd() * (13 - h));
                double r = p.rnd();
                std::array<int, 3> c = r < 0.55 ? std::array<int, 3>{ 150, 150, 152 } : r < 0.8 ? std::array<int, 3>{ 190, 188, 182 }
                                     : r < 0.9 ? std::array<int, 3>{ 160, 120, 100 } : std::array<int, 3>{ 120, 135, 150 };
                c = mix(c, { 210, 210, 210 }, p.rnd() * 0.3);
                p.rect(x, y, w, h, c);
                p.rect(x + w, y + 1, 1, h, { 70, 70, 74 });   // its shadow
                p.rect(x + 1, y + h, w, 1, { 70, 70, 74 });
            }
        }
        p.noise(14);
        return p;
    }

    Paint grass() {
        Paint p(2);
        p.rect(0, 0, tex_size, tex_size, { 60, 130, 44 });
        for (int k = 0; k < 90; k++) {
            int x = int(p.rnd() * tex_size), y = int(p.rnd() * tex_size), r = 1 + int(p.rnd() * 3);
            std::array<int, 3> c = p.rnd() < 0.5 ? std::array<int, 3>{ 40, 100, 34 } : std::array<int, 3>{ 90, 160, 60 };
            for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) if (dx * dx + dy * dy <= r * r) p.set(x + dx, y + dy, c);
        }
        p.noise(16);
        return p;
    }

    // The base's sides: earth in splotches, as the N64 dioramas' (TRK4O2's 16 x 16 tile): a 16 x 16 field of browns,
    // from dark loam to sandy, blended up to 64 x 64 (wrapping, so the tile repeats seamlessly), a few darker clods.
    Paint dirt() {
        Paint p(3);
        constexpr int n = 16;
        const std::array<std::array<int, 3>, 5> browns = { { { 74, 52, 34 }, { 104, 76, 50 }, { 132, 100, 68 },
                                                             { 156, 124, 88 }, { 118, 92, 70 } } };
        std::array<std::array<double, 3>, n * n> field;
        for (auto& f : field) {
            const std::array<int, 3>& c = browns[size_t(p.rnd() * browns.size()) % browns.size()];
            double j = (p.rnd() * 2 - 1) * 10;
            f = { c[0] + j, c[1] + j, c[2] + j * 0.7 };
        }
        const int step = tex_size / n;
        for (int y = 0; y < tex_size; y++) for (int x = 0; x < tex_size; x++) {
            double fx = (x + 0.5) / step - 0.5, fy = (y + 0.5) / step - 0.5;
            int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            double u = fx - x0, v = fy - y0;
            auto f = [&](int i, int j) { return field[size_t(((j % n + n) % n) * n + ((i % n + n) % n))]; };
            std::array<int, 3> out;
            for (int k = 0; k < 3; k++) {
                double a = f(x0, y0)[k] * (1 - u) + f(x0 + 1, y0)[k] * u;
                double b = f(x0, y0 + 1)[k] * (1 - u) + f(x0 + 1, y0 + 1)[k] * u;
                out[k] = int(a * (1 - v) + b * v);
            }
            p.set(x, y, out);
        }
        for (int k = 0; k < 24; k++) {
            int x = int(p.rnd() * tex_size), y = int(p.rnd() * tex_size), r = 1 + int(p.rnd() * 2);
            for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
                if (dx * dx + dy * dy <= r * r) p.set(x + dx, y + dy, mix(p.get(x + dx, y + dy), { 50, 34, 22 }, 0.5));
            }
        }
        p.noise(6);
        return p;
    }
    Paint water() {
        Paint p(4);
        p.rect(0, 0, tex_size, tex_size, { 30, 70, 150 });
        for (int k = 0; k < 70; k++) {
            int x = int(p.rnd() * tex_size), y = int(p.rnd() * tex_size), w = 2 + int(p.rnd() * 5);
            p.rect(x, y, w, 1, p.rnd() < 0.6 ? std::array<int, 3>{ 70, 120, 200 } : std::array<int, 3>{ 20, 50, 120 });
        }
        p.noise(10);
        return p;
    }

    // A wall: `color`, four floors of four windows, the odd one lit.
    Paint wall(uint32_t seed, std::array<int, 3> color, std::array<int, 3> glass, int cols = 4, int rows = 4) {
        Paint p(seed);
        p.rect(0, 0, tex_size, tex_size, color);
        p.noise(8);
        int cw = tex_size / cols, rh = tex_size / rows;
        for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
            int x = c * cw + cw / 4, y = r * rh + rh / 4, w = cw / 2, h = rh / 2 + 1;
            std::array<int, 3> g = p.rnd() < 0.12 ? std::array<int, 3>{ 230, 220, 150 } : mix(glass, { 0, 0, 0 }, p.rnd() * 0.3);
            p.rect(x, y, w, h, g);
            p.rect(x, y + h, w, 1, mix(color, { 255, 255, 255 }, 0.35));   // the sill
            p.rect(x - 1, y, 1, h, mix(color, { 0, 0, 0 }, 0.25));
        }
        return p;
    }

    // A flat roof: tar with a lighter rim and a vent or two (mapped 0..1 across the roof).
    Paint roof(uint32_t seed, std::array<int, 3> color) {
        Paint p(seed);
        p.rect(0, 0, tex_size, tex_size, color);
        p.noise(10);
        std::array<int, 3> rim = mix(color, { 255, 255, 255 }, 0.3);
        p.rect(0, 0, tex_size, 4, rim);
        p.rect(0, tex_size - 4, tex_size, 4, rim);
        p.rect(0, 0, 4, tex_size, rim);
        p.rect(tex_size - 4, 0, 4, tex_size, rim);
        for (int k = 0; k < 2; k++) {
            int x = 12 + int(p.rnd() * 30), y = 12 + int(p.rnd() * 30);
            p.rect(x, y, 10, 8, mix(color, { 40, 40, 40 }, 0.5));
            p.rect(x, y + 8, 10, 2, mix(color, { 0, 0, 0 }, 0.6));
        }
        return p;
    }

    // Roof tiles in rows (the Chinatown roofs).
    Paint tiles(uint32_t seed, std::array<int, 3> color) {
        Paint p(seed);
        for (int y = 0; y < tex_size; y++) {
            double f = (y % 8) / 8.0;
            p.rect(0, y, tex_size, 1, mix(color, { 0, 0, 0 }, 0.35 * f));
        }
        for (int y = 0; y < tex_size; y += 8) for (int x = (y / 8) % 2 * 4; x < tex_size; x += 8) p.rect(x, y, 1, 8, mix(color, { 0, 0, 0 }, 0.4));
        p.noise(10);
        return p;
    }

    // The pyramid's skin: pale concrete with rows of narrow windows.
    Paint pyramid_skin() {
        Paint p(9);
        p.rect(0, 0, tex_size, tex_size, { 214, 208, 196 });
        for (int y = 2; y < tex_size; y += 6) for (int x = 1; x < tex_size; x += 4) p.rect(x, y, 2, 3, { 70, 80, 96 });
        p.noise(6);
        return p;
    }

    // Columns of warm stone (the Palace of Fine Arts' rotunda and colonnade), dark between them.
    Paint colonnade() {
        Paint p(10);
        p.rect(0, 0, tex_size, tex_size, { 74, 56, 40 });
        for (int x = 0; x < tex_size; x += 16) p.rect(x + 3, 10, 9, tex_size - 10, { 206, 168, 116 });
        p.rect(0, 0, tex_size, 10, { 214, 176, 124 });                  // the frieze
        for (int x = 0; x < tex_size; x += 8) p.rect(x, 3, 4, 4, { 170, 132, 88 });
        p.noise(8);
        return p;
    }

    Paint dome() {
        Paint p(11);
        p.rect(0, 0, tex_size, tex_size, { 200, 150, 96 });
        for (int x = 0; x < tex_size; x += 8) p.rect(x, 0, 2, tex_size, { 150, 104, 64 });
        p.noise(10);
        return p;
    }

    Paint steel() {
        Paint p(12);
        p.rect(0, 0, tex_size, tex_size, { 150, 156, 166 });
        for (int x = 0; x < tex_size; x += 8) p.rect(x, 0, 1, tex_size, { 110, 116, 126 });
        for (int y = 0; y < tex_size; y += 8) p.rect(0, y, tex_size, 1, { 110, 116, 126 });
        p.noise(8);
        return p;
    }

    Paint asphalt() {
        Paint p(13);
        p.rect(0, 0, tex_size, tex_size, { 70, 70, 74 });
        p.noise(12);
        return p;
    }

    // The route: Rush's red line, a lighter top edge so it reads at a glance.
    Paint route() {
        Paint p(14);
        p.rect(0, 0, tex_size, tex_size, { 210, 20, 20 });
        p.noise(6);
        return p;
    }

    Paint foliage() {
        Paint p(15);
        p.rect(0, 0, tex_size, tex_size, { 34, 80, 30 });
        for (int k = 0; k < 160; k++) {
            int x = int(p.rnd() * tex_size), y = int(p.rnd() * tex_size);
            p.rect(x, y, 2, 2, p.rnd() < 0.5 ? std::array<int, 3>{ 20, 56, 20 } : std::array<int, 3>{ 70, 130, 50 });
        }
        p.noise(10);
        return p;
    }

    // ---- geometry ------------------------------------------------------------------------------------------------
    struct Model {
        a49::Glb g{ "rush2 sfrush_ps1 diorama", false };
        std::map<std::string, int> material_of;      // name -> index into prims
        std::vector<a49::Glb::Prim> prims;
        std::map<std::string, Paint> paints;
        size_t triangles = 0;

        int mat(const std::string& name) {
            auto it = material_of.find(name);
            if (it != material_of.end()) return it->second;
            auto pi = paints.find(name);
            if (pi == paints.end()) throw std::runtime_error("no texture " + name);
            int image = g.image(name, a49::png(pi->second.px, tex_size, tex_size));
            a49::Glb::Prim p;
            p.material = g.material(name, image, "OPAQUE", false, { 10497, 10497 });
            prims.push_back(std::move(p));
            return material_of[name] = int(prims.size()) - 1;
        }
        // A triangle or quad facing `out` (its winding is turned to face it). uv per corner.
        void poly(const std::string& m, std::vector<V3> p, std::vector<V2> uv, const V3& out, const std::vector<V3>* normals = nullptr) {
            V3 n = unit(cross(sub(p[1], p[0]), sub(p[2], p[0])));
            if (p.size() == 4) {
                V3 n2 = unit(cross(sub(p[2], p[0]), sub(p[3], p[0])));
                n = unit({ n[0] + n2[0], n[1] + n2[1], n[2] + n2[2] });
            }
            std::vector<V3> nn;
            if (normals) nn = *normals;
            if (dot(n, out) < 0) {
                std::reverse(p.begin(), p.end());
                std::reverse(uv.begin(), uv.end());
                if (normals) std::reverse(nn.begin(), nn.end());
                n = { -n[0], -n[1], -n[2] };
            }
            a49::Glb::Prim& pr = prims[size_t(mat(m))];
            uint32_t b = uint32_t(pr.verts.size());
            for (size_t i = 0; i < p.size(); i++) {
                pr.verts.push_back(p[i]);
                pr.normals.push_back(normals ? nn[i] : n);
                pr.uvs.push_back(uv[i]);
            }
            for (uint32_t i = 1; i + 1 < p.size(); i++) pr.tris.push_back({ b, b + i, b + i + 1 });
            triangles += p.size() - 2;
        }
    };

    struct Spec {
        std::vector<V2> outline;
        double ground = 0, base_depth = 40, grid = 90, falloff_near = 40, falloff_far = 160, water_level = -30;
        double city_tile = 70, grass_tile = 50;
        struct Area { std::string kind; std::vector<V2> poly; double height = NAN; };
        std::vector<Area> areas;
        double route_width = 14, route_raise = 3, route_step = 50, route_turn = 10;
        int route_smooth = 0;
        double height_scale = 1, building_scale = 1, tree_scale = 1;
        std::array<double, 2> side_tile{ 380, 140 };   // meters one tile of earth covers on the base's sides
        std::vector<std::array<double, 4>> hills;     // x, z, radius, height
        json buildings, trees, landmarks;
    };

    std::vector<V2> read_poly(const json& j) {
        std::vector<V2> out;
        for (const json& p : j) out.push_back({ p.at(0).get<double>(), p.at(1).get<double>() });
        return out;
    }

    bool inside(const std::vector<V2>& poly, V2 p) {
        bool in = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            if ((poly[i][1] > p[1]) != (poly[j][1] > p[1]) &&
                p[0] < (poly[j][0] - poly[i][0]) * (p[1] - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0]) in = !in;
        }
        return in;
    }

    // Chaikin corner cutting: rounds the outline the way the stock bases' edges are rounded.
    std::vector<V2> rounded(std::vector<V2> p, int passes) {
        for (int k = 0; k < passes; k++) {
            std::vector<V2> q;
            for (size_t i = 0; i < p.size(); i++) {
                const V2& a = p[i];
                const V2& b = p[(i + 1) % p.size()];
                q.push_back({ a[0] * 0.75 + b[0] * 0.25, a[1] * 0.75 + b[1] * 0.25 });
                q.push_back({ a[0] * 0.25 + b[0] * 0.75, a[1] * 0.25 + b[1] * 0.75 });
            }
            p = q;
        }
        return p;
    }

    // Twice the signed area: positive counter-clockwise in (x, z).
    double orientation(const std::vector<V2>& p) {
        double a = 0;
        for (size_t i = 0; i < p.size(); i++) a += p[i][0] * p[(i + 1) % p.size()][1] - p[(i + 1) % p.size()][0] * p[i][1];
        return a > 0 ? 1.0 : -1.0;
    }

    // The part of `poly` on the inside (keep > 0) or outside (keep < 0) of the line a -> b of a polygon of `orient`.
    std::vector<V2> clip_half(const std::vector<V2>& poly, V2 a, V2 b, double orient, double keep) {
        auto side = [&](const V2& p) { return keep * orient * ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])); };
        std::vector<V2> out;
        for (size_t k = 0; k < poly.size(); k++) {
            V2 p = poly[k], q = poly[(k + 1) % poly.size()];
            double sp = side(p), sq = side(q);
            if (sp >= 0) out.push_back(p);
            if ((sp >= 0) != (sq >= 0)) {
                double t = sp / (sp - sq);
                out.push_back({ p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t });
            }
        }
        return out.size() >= 3 ? out : std::vector<V2>{};
    }

    // Triangles of a simple polygon (either turn), by ear clipping.
    std::vector<std::array<size_t, 3>> ear_clip(const std::vector<V2>& p) {
        std::vector<std::array<size_t, 3>> out;
        std::vector<size_t> v;
        for (size_t i = 0; i < p.size(); i++) v.push_back(i);
        double turn = orientation(p);
        auto cross2 = [&](size_t a, size_t b, size_t c) {
            return turn * ((p[b][0] - p[a][0]) * (p[c][1] - p[a][1]) - (p[b][1] - p[a][1]) * (p[c][0] - p[a][0]));
        };
        size_t guard = 0;
        while (v.size() > 3 && guard++ < 10000) {
            bool cut = false;
            for (size_t i = 0; i < v.size(); i++) {
                size_t a = v[(i + v.size() - 1) % v.size()], b = v[i], c = v[(i + 1) % v.size()];
                if (cross2(a, b, c) <= 1e-9) continue;     // a reflex (or flat) corner
                bool empty = true;
                for (size_t k : v) {
                    if (k == a || k == b || k == c) continue;
                    if (cross2(a, b, k) >= 0 && cross2(b, c, k) >= 0 && cross2(c, a, k) >= 0) { empty = false; break; }
                }
                if (!empty) continue;
                out.push_back({ a, b, c });
                v.erase(v.begin() + long(i));
                cut = true;
                break;
            }
            if (!cut) {     // degenerate leftovers: drop a flat corner
                out.push_back({ v[0], v[1], v[2] });
                v.erase(v.begin() + 1);
            }
        }
        if (v.size() == 3) out.push_back({ v[0], v[1], v[2] });
        return out;
    }

    // `poly` split by the convex `clip`: the part inside it, and the pieces outside (one per edge it crosses).
    void split_convex(const std::vector<V2>& poly, const std::vector<V2>& clip, std::vector<V2>& in, std::vector<std::vector<V2>>& outside) {
        double orient = orientation(clip);
        in = poly;
        for (size_t i = 0; i < clip.size() && !in.empty(); i++) {
            V2 a = clip[i], b = clip[(i + 1) % clip.size()];
            std::vector<V2> o = clip_half(in, a, b, orient, -1);
            if (!o.empty()) outside.push_back(o);
            in = clip_half(in, a, b, orient, 1);
        }
    }

    double smoothstep(double e0, double e1, double x) {
        double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
        return t * t * (3 - 2 * t);
    }
}

bool ps1::build_diorama(const std::vector<std::array<double, 3>>& route_points, const std::filesystem::path& spec_path,
                        std::vector<uint8_t>& out, std::string& log) {
    char line[256];
    Spec s;
    json j;
    {
        std::ifstream f(spec_path);
        if (!f) {
            log += "missing " + rush2::upscale::path_utf8(spec_path) + "\n";
            return false;
        }
        j = json::parse(f, nullptr, false, true);
        if (j.is_discarded()) {
            log += rush2::upscale::path_utf8(spec_path) + " isn't valid JSON\n";
            return false;
        }
    }
    try {
        // The island's edge, fitted to the course: the route widened by `margin` each side, its loops filled, the
        // notches narrower than 2 x `closing` closed, plus the `add` shapes (a bay, a landmark's ground), rounded.
        const json& o = j.at("outline");
        double margin = o.value("margin", 120.0), closing = o.value("closing", 120.0);
        Clipper2Lib::PathD path;
        for (const auto& p : route_points) path.push_back({ p[0], p[2] });
        Clipper2Lib::PathsD shape = Clipper2Lib::InflatePaths({ path }, margin, Clipper2Lib::JoinType::Round,
                                                             Clipper2Lib::EndType::Joined, 2.0, 1);
        Clipper2Lib::PathsD outers;
        for (const Clipper2Lib::PathD& p : shape) if (Clipper2Lib::Area(p) > 0) outers.push_back(p);
        for (const json& a : o.value("add", json::array())) {
            Clipper2Lib::PathD q;
            for (const V2& p : read_poly(a)) q.push_back({ p[0], p[1] });
            if (Clipper2Lib::Area(q) < 0) std::reverse(q.begin(), q.end());
            outers.push_back(q);
        }
        shape = Clipper2Lib::Union(outers, Clipper2Lib::FillRule::NonZero, 1);
        shape = Clipper2Lib::InflatePaths(shape, closing, Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon, 2.0, 1);
        shape = Clipper2Lib::InflatePaths(shape, -closing, Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon, 2.0, 1);
        size_t best = 0;
        for (size_t i = 0; i < shape.size(); i++) if (std::abs(Clipper2Lib::Area(shape[i])) > std::abs(Clipper2Lib::Area(shape[best]))) best = i;
        if (shape.empty()) throw std::runtime_error("the outline came out empty");
        Clipper2Lib::PathD edge = Clipper2Lib::SimplifyPath(shape[best], o.value("simplify", 12.0), true);
        for (const Clipper2Lib::PointD& p : edge) s.outline.push_back({ p.x, p.y });
        s.outline = rounded(s.outline, o.value("rounding", 1));
        s.ground = j.value("ground", s.ground);
        s.base_depth = j.value("base_depth", s.base_depth);
        s.grid = j.value("grid", s.grid);
        s.falloff_near = j.value("falloff_near", s.falloff_near);
        s.falloff_far = j.value("falloff_far", s.falloff_far);
        s.water_level = j.value("water_level", s.water_level);
        s.city_tile = j.value("city_tile", s.city_tile);
        s.grass_tile = j.value("grass_tile", s.grass_tile);
        s.height_scale = j.value("height_scale", s.height_scale);
        s.building_scale = j.value("building_scale", s.building_scale);
        s.tree_scale = j.value("tree_scale", s.tree_scale);
        if (j.contains("side_tile")) s.side_tile = { j["side_tile"].at(0).get<double>(), j["side_tile"].at(1).get<double>() };
        for (const json& h : j.value("hills", json::array())) {
            s.hills.push_back({ h.at(0).get<double>(), h.at(1).get<double>(), h.at(2).get<double>(), h.at(3).get<double>() });
        }
        for (const json& a : j.value("areas", json::array())) {
            if (!a.contains("kind")) continue;      // a note ({"_": ...})
            Spec::Area area{ a.at("kind").get<std::string>(), rounded(read_poly(a.at("poly")), a.value("rounding", 1)) };
            if (a.contains("height")) area.height = a["height"].get<double>();
            s.areas.push_back(area);
        }
        if (j.contains("route")) {
            const json& r = j["route"];
            s.route_width = r.value("width", s.route_width);
            s.route_raise = r.value("raise", s.route_raise);
            s.route_step = r.value("step", s.route_step);
            s.route_turn = r.value("turn", s.route_turn);
            s.route_smooth = r.value("smooth", s.route_smooth);
        }
        s.buildings = j.value("buildings", json::array());
        s.trees = j.value("trees", json::array());
        s.landmarks = j.value("landmarks", json::array());
    } catch (const std::exception& e) {
        log += rush2::upscale::path_utf8(spec_path) + ": " + e.what() + "\n";
        return false;
    }
    if (s.outline.size() < 3 || route_points.size() < 8) {
        log += "the diorama needs an outline and the route\n";
        return false;
    }

    Model m;
    m.paints.emplace("city", city_ground());
    m.paints.emplace("grass", grass());
    m.paints.emplace("dirt", dirt());
    m.paints.emplace("water", water());
    m.paints.emplace("route", route());
    m.paints.emplace("foliage", foliage());
    m.paints.emplace("pyramid", pyramid_skin());
    m.paints.emplace("colonnade", colonnade());
    m.paints.emplace("dome", dome());
    m.paints.emplace("steel", steel());
    m.paints.emplace("asphalt", asphalt());
    // Walls: name -> (color, glass).
    const std::map<std::string, std::pair<std::array<int, 3>, std::array<int, 3>>> walls = {
        { "tan", { { 196, 170, 126 }, { 60, 70, 90 } } },     { "white", { { 220, 218, 210 }, { 70, 90, 120 } } },
        { "grey", { { 150, 152, 158 }, { 50, 60, 80 } } },    { "blue", { { 120, 140, 176 }, { 40, 50, 80 } } },
        { "brick", { { 150, 76, 56 }, { 50, 50, 60 } } },     { "green", { { 150, 186, 150 }, { 50, 70, 80 } } },
        { "pink", { { 214, 160, 160 }, { 60, 70, 90 } } },    { "yellow", { { 220, 196, 120 }, { 70, 70, 90 } } },
        { "red", { { 176, 40, 34 }, { 240, 200, 90 } } },     { "glass", { { 90, 110, 140 }, { 150, 190, 220 } } },
    };
    uint32_t seed = 20;
    for (const auto& [name, c] : walls) {
        m.paints.emplace("wall_" + name, wall(seed++, c.first, c.second, name == "glass" ? 8 : 4, name == "glass" ? 8 : 4));
    }
    m.paints.emplace("roof_flat", roof(40, { 120, 116, 110 }));
    m.paints.emplace("roof_tan", roof(41, { 170, 150, 120 }));
    m.paints.emplace("tiles_red", tiles(42, { 170, 40, 30 }));
    m.paints.emplace("tiles_green", tiles(43, { 50, 130, 80 }));
    m.paints.emplace("tiles_grey", tiles(44, { 110, 110, 116 }));

    // The route: a closed loop, thinned to a point where it turns or every route_step meters.
    std::vector<V3> route;
    {
        route.push_back(route_points[0]);
        for (size_t i = 1; i < route_points.size(); i++) {
            const V3& p = route_points[i];
            const V3& q = route.back();
            double d = std::hypot(p[0] - q[0], p[2] - q[2]);
            const V3& nx = route_points[(i + 1) % route_points.size()];
            V3 dir = unit({ nx[0] - p[0], 0, nx[2] - p[2] });
            V3 from = unit({ p[0] - q[0], 0, p[2] - q[2] });
            double turn = std::acos(std::clamp(dot(dir, from), -1.0, 1.0)) * 180 / pi;
            if (d >= s.route_step || (turn > s.route_turn && d > s.route_width)) route.push_back(p);
        }
        // The stock dioramas draw a simplified line: corner cutting irons out the small wiggles.
        for (int k = 0; k < s.route_smooth; k++) {
            std::vector<V3> q;
            for (size_t i = 0; i < route.size(); i++) {
                const V3& a = route[i];
                const V3& b = route[(i + 1) % route.size()];
                q.push_back({ a[0] * 0.75 + b[0] * 0.25, a[1] * 0.75 + b[1] * 0.25, a[2] * 0.75 + b[2] * 0.25 });
                q.push_back({ a[0] * 0.25 + b[0] * 0.75, a[1] * 0.25 + b[1] * 0.75, a[2] * 0.25 + b[2] * 0.75 });
            }
            route = q;
        }
    }

    // Ground height: the route's height near it (its rises and falls height_scale times the track's, so they read at
    // diorama size), easing to `ground` away from it, plus the hills (kept off the route); flat water, its shore too.
    std::vector<V3> dense = route_points;
    for (V3& p : dense) p[1] = s.ground + (p[1] - s.ground) * s.height_scale;
    for (size_t i = 0; i < route.size(); i++) route[i][1] = s.ground + (route[i][1] - s.ground) * s.height_scale;
    auto near_edge = [](const std::vector<V2>& poly, V2 p) {
        for (size_t i = 0; i < poly.size(); i++) {
            const V2& a = poly[i];
            const V2& b = poly[(i + 1) % poly.size()];
            double dx = b[0] - a[0], dz = b[1] - a[1], l2 = dx * dx + dz * dz;
            double t = l2 > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dz) / l2, 0.0, 1.0) : 0.0;
            if (std::hypot(a[0] + dx * t - p[0], a[1] + dz * t - p[1]) < 0.5) return true;
        }
        return false;
    };
    // Distance to the band's line (the thinned, smoothed route, so the ground under the band follows the band) and
    // its height there.
    auto route_distance = [&](double x, double z, double* h = nullptr) {
        double best = 1e30;
        for (size_t i = 0; i < route.size(); i++) {
            const V3& a = route[i];
            const V3& b = route[(i + 1) % route.size()];
            double dx = b[0] - a[0], dz = b[2] - a[2], l2 = dx * dx + dz * dz;
            double t = l2 > 0 ? std::clamp(((x - a[0]) * dx + (z - a[2]) * dz) / l2, 0.0, 1.0) : 0.0;
            double d = std::hypot(a[0] + dx * t - x, a[2] + dz * t - z);
            if (d < best) {
                best = d;
                if (h) *h = a[1] + (b[1] - a[1]) * t;
            }
        }
        return best;
    };
    // The ground as designed (a smooth function); the model is this sampled on the grid (`height`, below).
    auto shape = [&](double x, double z) {
        for (const Spec::Area& a : s.areas) {
            if (a.kind == "water" && (inside(a.poly, { x, z }) || near_edge(a.poly, { x, z }))) return s.water_level;
        }
        double h = s.ground;
        double best = route_distance(x, z, &h);
        double f = 1.0 - smoothstep(s.falloff_near, s.falloff_far, best);
        double out = s.ground + (h - s.ground) * f;
        double keep_off = smoothstep(s.grid * 0.8, s.grid * 0.8 + 80, best);   // no ground vertex near the route lifted
        for (const auto& [hx, hz, hr, hh] : s.hills) {
            double d = std::hypot(hx - x, hz - z);
            if (d < hr) out += hh * 0.5 * (1 + std::cos(pi * d / hr)) * keep_off;
        }
        for (const Spec::Area& a : s.areas) {
            if (!std::isnan(a.height) && inside(a.poly, { x, z })) out = std::max(out, a.height);
        }
        return out;
    };
    auto ground_normal = [&](double x, double z) {
        double e = s.grid * 0.25;
        double hx = shape(x + e, z) - shape(x - e, z), hz = shape(x, z + e) - shape(x, z - e);
        return unit({ -hx, 2 * e, -hz });
    };
    auto kind_at = [&](double x, double z) {
        std::string k = "city";
        for (const Spec::Area& a : s.areas) if (inside(a.poly, { x, z })) k = a.kind;
        return k;
    };

    // Ground: the designed shape sampled on a grid, each cell split along its diagonal into two flat triangles (the
    // model's ground is that piecewise flat surface, `height`). Each triangle is clipped to the outline and cut along
    // the areas' edges (areas convex; a later one drawn over an earlier one). Every piece lies in one flat triangle and
    // takes its heights from it, so pieces meet without cracks wherever their corners fall.
    double lo_x = 1e30, hi_x = -1e30, lo_z = 1e30, hi_z = -1e30;
    for (const V2& a : s.outline) {
        lo_x = std::min(lo_x, a[0]); hi_x = std::max(hi_x, a[0]);
        lo_z = std::min(lo_z, a[1]); hi_z = std::max(hi_z, a[1]);
    }
    lo_x -= 1; lo_z -= 1;
    const int cols = int(std::ceil((hi_x + 1 - lo_x) / s.grid)), rows = int(std::ceil((hi_z + 1 - lo_z) / s.grid));
    std::vector<double> corner(size_t(cols + 1) * size_t(rows + 1));
    for (int r = 0; r <= rows; r++) for (int c = 0; c <= cols; c++) {
        corner[size_t(r) * size_t(cols + 1) + size_t(c)] = shape(lo_x + c * s.grid, lo_z + r * s.grid);
    }
    auto height = [&](double x, double z) {
        double fx = std::clamp((x - lo_x) / s.grid, 0.0, double(cols) - 1e-9), fz = std::clamp((z - lo_z) / s.grid, 0.0, double(rows) - 1e-9);
        int c = int(fx), r = int(fz);
        double u = fx - c, v = fz - r;
        auto at = [&](int dc, int dr) { return corner[size_t(r + dr) * size_t(cols + 1) + size_t(c + dc)]; };
        // The diagonal runs from (c + 1, r) to (c, r + 1).
        if (u + v <= 1) return at(0, 0) + u * (at(1, 0) - at(0, 0)) + v * (at(0, 1) - at(0, 0));
        return at(1, 1) + (1 - u) * (at(0, 1) - at(1, 1)) + (1 - v) * (at(1, 0) - at(1, 1));
    };
    double lowest = 1e30;
    Clipper2Lib::PathD edge_path;
    for (const V2& p : s.outline) edge_path.push_back({ p[0], p[1] });
    std::vector<std::pair<std::string, std::vector<V2>>> pieces;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            double gx = lo_x + c * s.grid, gz = lo_z + r * s.grid;
            for (int half = 0; half < 2; half++) {
                Clipper2Lib::PathD tri = half == 0
                    ? Clipper2Lib::PathD{ { gx, gz }, { gx + s.grid, gz }, { gx, gz + s.grid } }
                    : Clipper2Lib::PathD{ { gx + s.grid, gz }, { gx + s.grid, gz + s.grid }, { gx, gz + s.grid } };
                std::vector<std::pair<std::string, std::vector<V2>>> parts;
                for (const Clipper2Lib::PathD& q : Clipper2Lib::Intersect({ tri }, { edge_path }, Clipper2Lib::FillRule::NonZero, 4)) {
                    std::vector<V2> piece;
                    for (const Clipper2Lib::PointD& p : q) piece.push_back({ p.x, p.y });
                    if (piece.size() >= 3) parts.push_back({ "city", piece });
                }
                for (const Spec::Area& a : s.areas) {
                    std::vector<std::pair<std::string, std::vector<V2>>> next;
                    for (const auto& [k, p] : parts) {
                        std::vector<V2> in;
                        std::vector<std::vector<V2>> outside;
                        split_convex(p, a.poly, in, outside);
                        if (!in.empty()) next.push_back({ a.kind, in });
                        for (const std::vector<V2>& o : outside) next.push_back({ k, o });
                    }
                    parts = next;
                }
                pieces.insert(pieces.end(), parts.begin(), parts.end());
            }
        }
    }
    // The ground's rim: the edges only one ground triangle has that lie on the outline (inside, an edge can be met by
    // two shorter ones, which leaves no crack on a flat surface), each oriented counter-clockwise round the ground.
    auto key = [](const V2& p) { return std::make_pair(std::llround(p[0] * 100), std::llround(p[1] * 100)); };
    std::map<std::pair<std::pair<long long, long long>, std::pair<long long, long long>>, std::pair<V2, V2>> edges;
    for (const auto& [k, piece] : pieces) {
        double tile = k == "city" ? s.city_tile : s.grass_tile;
        for (std::array<size_t, 3> tri : ear_clip(piece)) {
            if (orientation({ piece[tri[0]], piece[tri[1]], piece[tri[2]] }) < 0) std::swap(tri[1], tri[2]);
            std::vector<V3> p, n;
            std::vector<V2> uv;
            for (size_t i : tri) {
                double h = height(piece[i][0], piece[i][1]);
                lowest = std::min(lowest, h);
                p.push_back({ piece[i][0], h, piece[i][1] });
                n.push_back(ground_normal(piece[i][0], piece[i][1]));
                uv.push_back({ piece[i][0] / tile, piece[i][1] / tile });
            }
            m.poly(k, p, uv, { 0, 1, 0 }, &n);
            for (int e = 0; e < 3; e++) {
                const V2& a = piece[tri[size_t(e)]];
                const V2& b = piece[tri[size_t((e + 1) % 3)]];
                auto ka = key(a), kb = key(b);
                if (ka == kb) continue;
                auto back = edges.find({ kb, ka });
                if (back != edges.end()) edges.erase(back);
                else edges[{ ka, kb }] = { a, b };
            }
        }
    }

    // The base's sides, down to base_depth under the lowest ground, hung from the rim: earth, or water where the bay
    // meets the edge. Earth is mapped as the N64 dioramas' is (TRK4O2: one 16 x 16 tile of brown splotches about 380
    // by 140 units), so its splotches read at diorama size rather than as a fine repeat.
    double floor_y = lowest - s.base_depth;
    {
        auto on_outline = [&](const V2& p) {
            for (size_t i = 0; i < s.outline.size(); i++) {
                const V2& a = s.outline[i];
                const V2& b = s.outline[(i + 1) % s.outline.size()];
                double dx = b[0] - a[0], dz = b[1] - a[1], l2 = dx * dx + dz * dz;
                double t = l2 > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dz) / l2, 0.0, 1.0) : 0.0;
                if (std::hypot(a[0] + dx * t - p[0], a[1] + dz * t - p[1]) < 0.05) return true;
            }
            return false;
        };
        // Chain the rim so the earth's texture runs on round it.
        std::map<std::pair<long long, long long>, std::pair<V2, V2>> next;
        for (const auto& [k, e] : edges) {
            V2 mid{ (e.first[0] + e.second[0]) / 2, (e.first[1] + e.second[1]) / 2 };
            if (on_outline(e.first) && on_outline(e.second) && on_outline(mid)) next[k.first] = e;
        }
        const double tile_w = s.side_tile[0], tile_h = s.side_tile[1];
        double run = 0;
        while (!next.empty()) {
            auto it = next.begin();
            while (it != next.end()) {
                auto [p, q] = it->second;
                next.erase(it);
                double hp = height(p[0], p[1]), hq = height(q[0], q[1]);
                double seg = std::hypot(q[0] - p[0], q[1] - p[1]);
                std::string k2 = kind_at((p[0] + q[0]) / 2, (p[1] + q[1]) / 2) == "water" ? "water" : "dirt";
                // Outward: right of an edge running counter-clockwise.
                V3 out{ q[1] - p[1], 0, -(q[0] - p[0]) };
                m.poly(k2, { { p[0], floor_y, p[1] }, { q[0], floor_y, q[1] }, { q[0], hq, q[1] }, { p[0], hp, p[1] } },
                       { { run / tile_w, 0 }, { (run + seg) / tile_w, 0 }, { (run + seg) / tile_w, -(hq - floor_y) / tile_h },
                         { run / tile_w, -(hp - floor_y) / tile_h } }, out);
                run += seg;
                it = next.find(key(q));
            }
            run = std::ceil(run / tile_w) * tile_w;     // a new loop (an island part) starts on a whole tile
        }
    }
    // The route: a red band, its top route_raise over the ground.
    {
        size_t n = route.size();
        std::vector<V3> lft(n), rgt(n);
        std::vector<double> top(n);
        for (size_t i = 0; i < n; i++) {
            const V3& a = route[(i + n - 1) % n];
            const V3& b = route[(i + 1) % n];
            V3 t = unit({ b[0] - a[0], 0, b[2] - a[2] });
            V3 lat{ -t[2], 0, t[0] };
            double w = s.route_width / 2;
            lft[i] = { route[i][0] + lat[0] * w, 0, route[i][2] + lat[2] * w };
            rgt[i] = { route[i][0] - lat[0] * w, 0, route[i][2] - lat[2] * w };
            top[i] = std::max({ route[i][1], height(lft[i][0], lft[i][2]), height(rgt[i][0], rgt[i][2]),
                                height(route[i][0], route[i][2]) }) + s.route_raise;
        }
        // Between two points the ground can rise over the straight band (a crest of the course the thinning skipped):
        // sample it along each segment and lift both ends over the highest sample.
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < n; i++) {
                size_t k = (i + 1) % n;
                for (int q = 1; q < 8; q++) {
                    double f = q / 8.0;
                    double need = -1e30;
                    for (const V3* e : { &lft[i], &rgt[i] }) {
                        const V3& e2 = e == &lft[i] ? lft[k] : rgt[k];
                        double x = (*e)[0] + (e2[0] - (*e)[0]) * f, z = (*e)[2] + (e2[2] - (*e)[2]) * f;
                        need = std::max(need, height(x, z) + s.route_raise);
                    }
                    double x = route[i][0] + (route[k][0] - route[i][0]) * f, z = route[i][2] + (route[k][2] - route[i][2]) * f;
                    need = std::max(need, height(x, z) + s.route_raise);
                    double have = top[i] + (top[k] - top[i]) * f;
                    if (have < need) {
                        top[i] += need - have;
                        top[k] += need - have;
                    }
                }
            }
        }
        for (size_t i = 0; i < n; i++) {
            size_t k = (i + 1) % n;
            V3 l0 = { lft[i][0], top[i], lft[i][2] }, r0 = { rgt[i][0], top[i], rgt[i][2] };
            V3 l1 = { lft[k][0], top[k], lft[k][2] }, r1 = { rgt[k][0], top[k], rgt[k][2] };
            // A flat strip, as the N64 dioramas' red line is (it clears the ground everywhere: above).
            m.poly("route", { l0, r0, r1, l1 }, { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }, { 0, 1, 0 });
        }
    }

    // A box: footprint centered on (x, z), w x d, turned `angle` degrees, from y0 up h; walls and a roof material
    // ("" none). Walls' windows are cell meters wide and tall.
    auto box = [&](double x, double z, double w, double d, double angle, double y0, double h, const std::string& wall_mat,
                   const std::string& roof_mat, double cell) {
        double ca = std::cos(angle * pi / 180), sa = std::sin(angle * pi / 180);
        auto at = [&](double u, double v) { return V2{ x + u * ca - v * sa, z + u * sa + v * ca }; };
        V2 c[4] = { at(-w / 2, -d / 2), at(w / 2, -d / 2), at(w / 2, d / 2), at(-w / 2, d / 2) };
        for (int i = 0; i < 4; i++) {
            const V2& a = c[i];
            const V2& b = c[(i + 1) % 4];
            double len = std::hypot(b[0] - a[0], b[1] - a[1]);
            V3 out{ (a[0] + b[0]) / 2 - x, 0, (a[1] + b[1]) / 2 - z };
            m.poly(wall_mat, { { a[0], y0, a[1] }, { b[0], y0, b[1] }, { b[0], y0 + h, b[1] }, { a[0], y0 + h, a[1] } },
                   { { 0, 0 }, { len / cell, 0 }, { len / cell, -h / cell }, { 0, -h / cell } }, out);
        }
        if (!roof_mat.empty()) {
            m.poly(roof_mat, { { c[0][0], y0 + h, c[0][1] }, { c[1][0], y0 + h, c[1][1] }, { c[2][0], y0 + h, c[2][1] },
                               { c[3][0], y0 + h, c[3][1] } }, { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }, { 0, 1, 0 });
        }
    };
    // A hipped roof over a w x d footprint (plus `over` each way), rising `rise` to a ridge.
    auto hip = [&](double x, double z, double w, double d, double angle, double y0, double rise, double over,
                   const std::string& mat) {
        double ca = std::cos(angle * pi / 180), sa = std::sin(angle * pi / 180);
        auto at = [&](double u, double v, double y) { return V3{ x + u * ca - v * sa, y, z + u * sa + v * ca }; };
        double hw = w / 2 + over, hd = d / 2 + over;
        double ridge = std::max(0.0, hw - hd);
        V3 c[4] = { at(-hw, -hd, y0), at(hw, -hd, y0), at(hw, hd, y0), at(-hw, hd, y0) };
        V3 r0 = at(-ridge, 0, y0 + rise), r1 = at(ridge, 0, y0 + rise);
        auto face_out = [&](const V3& a, const V3& b) { return V3{ (a[0] + b[0]) / 2 - x, rise, (a[2] + b[2]) / 2 - z }; };
        double tile = 16;     // meters a tile texture repeats (finer reads as stripes at diorama size)
        m.poly(mat, { c[0], c[1], r1, r0 }, { { 0, 0 }, { 2 * hw / tile, 0 }, { 2 * hw / tile, -hd / tile }, { 0, -hd / tile } },
               face_out(c[0], c[1]));
        m.poly(mat, { c[2], c[3], r0, r1 }, { { 0, 0 }, { 2 * hw / tile, 0 }, { 2 * hw / tile, -hd / tile }, { 0, -hd / tile } },
               face_out(c[2], c[3]));
        m.poly(mat, { c[1], c[2], r1 }, { { 0, 0 }, { 2 * hd / tile, 0 }, { hd / tile, -hd / tile } }, face_out(c[1], c[2]));
        m.poly(mat, { c[3], c[0], r0 }, { { 0, 0 }, { 2 * hd / tile, 0 }, { hd / tile, -hd / tile } }, face_out(c[3], c[0]));
        // Under the eaves, so the overhang isn't see-through from below.
        if (over > 0) m.poly(mat, { c[0], c[1], c[2], c[3] }, { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }, { 0, -1, 0 });
    };
    // The lowest ground under a footprint (so a building on a slope doesn't float).
    auto foot = [&](double x, double z, double w, double d) {
        double r = std::max(w, d) / 2, h = 1e30;
        for (int i = -1; i <= 1; i++) for (int k = -1; k <= 1; k++) h = std::min(h, height(x + i * r, z + k * r));
        return h;
    };
    // How close a footprint comes to the route's middle (the band is route_width wide): listed in the log.
    auto route_clearance = [&](double x, double z, double r) {
        double best = 1e30;
        for (const V3& p : dense) best = std::min(best, std::hypot(p[0] - x, p[2] - z));
        return best - r;
    };

    size_t building_count = 0;
    std::string near_route, off_island;
    for (const json& b : s.buildings) {
        if (!b.contains("at")) continue;    // a heading ({"_": ...})
        std::vector<double> at = b.at("at").get<std::vector<double>>();
        std::vector<double> size = b.value("size", std::vector<double>{ 40, 40 });
        double h = b.value("height", 40.0) * s.building_scale, angle = b.value("angle", 0.0), cell = b.value("cell", 14.0) * s.building_scale;
        std::string wall_mat = "wall_" + b.value("wall", std::string("tan"));
        std::string roof_kind = b.value("roof", std::string("flat"));
        int tiers = b.value("tiers", 1);
        double shrink = b.value("shrink", 0.7);
        double y = foot(at[0], at[1], size[0] * s.building_scale, size[1] * s.building_scale) - 1;
        double w = size[0] * s.building_scale, d = size[1] * s.building_scale;
        double clear = route_clearance(at[0], at[1], std::hypot(w, d) / 2);
        {
            // Every corner of the footprint (eaves included) on the island, or it hangs over the edge.
            double reach = std::hypot(w, d) / 2 + (roof_kind == "flat" ? 0.0 : b.value("eaves", 4.0));
            bool on = true;
            for (int k = 0; k < 8; k++) on = on && inside(s.outline, { at[0] + reach * std::cos(k * pi / 4), at[1] + reach * std::sin(k * pi / 4) });
            if (!on) {
                snprintf(line, sizeof(line), " (%.0f,%.0f)", at[0], at[1]);
                off_island += line;
            }
        }
        if (clear < s.route_width / 2 + 2) {
            snprintf(line, sizeof(line), " (%.0f,%.0f)%.0f", at[0], at[1], clear);
            near_route += line;
        }
        std::string flat_roof = b.value("roof_color", std::string("flat")) == "tan" ? "roof_tan" : "roof_flat";
        for (int t = 0; t < tiers; t++) {
            // Tiers: a pagoda's storeys or a tower's setbacks, each `shrink` the one under it.
            double th = t == 0 ? h : h * b.value("tier_height", 0.5);
            if (roof_kind == "flat") {
                box(at[0], at[1], w, d, angle, y, th, wall_mat, flat_roof, cell);
            } else {
                box(at[0], at[1], w, d, angle, y, th, wall_mat, "", cell);
                hip(at[0], at[1], w, d, angle, y + th, std::min(w, d) * b.value("pitch", 0.35), b.value("eaves", 4.0),
                    "tiles_" + roof_kind);
            }
            y += th;
            w *= shrink;
            d *= shrink;
        }
        building_count++;
    }

    // Trees: a squat eight-sided bush.
    for (const json& t : s.trees) {
        std::vector<double> at = t.get<std::vector<double>>();
        if (!inside(s.outline, { at[0], at[1] })) {
            snprintf(line, sizeof(line), " tree(%.0f,%.0f)", at[0], at[1]);
            off_island += line;
        }
        double r = (at.size() > 2 ? at[2] : 12) * s.tree_scale;
        double y = height(at[0], at[1]);
        V3 top{ at[0], y + r * 1.6, at[1] }, bottom{ at[0], y, at[1] };
        for (int k = 0; k < 6; k++) {
            double a0 = k * pi / 3, a1 = (k + 1) * pi / 3;
            V3 p0{ at[0] + r * std::cos(a0), y + r * 0.6, at[1] + r * std::sin(a0) };
            V3 p1{ at[0] + r * std::cos(a1), y + r * 0.6, at[1] + r * std::sin(a1) };
            V3 out{ std::cos((a0 + a1) / 2), 0.4, std::sin((a0 + a1) / 2) };
            m.poly("foliage", { p0, p1, top }, { { 0, 0 }, { 1, 0 }, { 0.5, -1 } }, out);
            m.poly("foliage", { p0, p1, bottom }, { { 0, 0 }, { 1, 0 }, { 0.5, 1 } }, { out[0], -0.6, out[2] });
        }
    }

    // Landmarks.
    for (const json& l : s.landmarks) {
        std::string kind = l.at("kind").get<std::string>();
        std::vector<double> at = l.at("at").get<std::vector<double>>();
        double angle = l.value("angle", 0.0);
        double y = foot(at[0], at[1], 30, 30) - 1;
        if (kind == "pyramid") {
            // The Transamerica Pyramid: a slim four-sided spire on its own height.
            double base = l.value("base", 40.0), h = l.value("height", 200.0), spire = l.value("spire", 0.18);
            double ca = std::cos(angle * pi / 180), sa = std::sin(angle * pi / 180);
            double r = base / 2;
            V3 c[4];
            for (int k = 0; k < 4; k++) {
                double u = (k == 0 || k == 3) ? -r : r, v = k < 2 ? -r : r;
                c[k] = { at[0] + u * ca - v * sa, y, at[1] + u * sa + v * ca };
            }
            double body = h * (1 - spire);
            // The body rises to a small square (a tenth of the base) the spire stands on.
            double tr = r * 0.12;
            V3 t[4];
            for (int k = 0; k < 4; k++) t[k] = { at[0] + (c[k][0] - at[0]) * tr / r, y + body, at[1] + (c[k][2] - at[1]) * tr / r };
            for (int k = 0; k < 4; k++) {
                const V3& a = c[k];
                const V3& b = c[(k + 1) % 4];
                V3 out{ (a[0] + b[0]) / 2 - at[0], 0.2, (a[2] + b[2]) / 2 - at[1] };
                double vv = body / 8;
                m.poly("pyramid", { a, b, t[(k + 1) % 4], t[k] }, { { 0, 0 }, { base / 8, 0 }, { base / 8 * 0.55, -vv }, { base / 8 * 0.45, -vv } }, out);
                m.poly("pyramid", { t[k], t[(k + 1) % 4], { at[0], y + h, at[1] } }, { { 0, 0 }, { 1, 0 }, { 0.5, -2 } }, out);
            }
        } else if (kind == "rotunda") {
            // The Palace of Fine Arts: an eight-sided ring of columns under a dome.
            double r = l.value("radius", 22.0), h = l.value("height", 24.0), dh = l.value("dome", 22.0);
            int sides = 8;
            for (int k = 0; k < sides; k++) {
                double a0 = k * 2 * pi / sides, a1 = (k + 1) * 2 * pi / sides;
                V3 p0{ at[0] + r * std::cos(a0), y, at[1] + r * std::sin(a0) }, p1{ at[0] + r * std::cos(a1), y, at[1] + r * std::sin(a1) };
                V3 out{ std::cos((a0 + a1) / 2), 0, std::sin((a0 + a1) / 2) };
                m.poly("colonnade", { p0, p1, { p1[0], y + h, p1[2] }, { p0[0], y + h, p0[2] } }, { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, -1 } }, out);
                // The dome: two rings to the top.
                double r1 = r * 0.92, r2 = r * 0.55;
                V3 q0{ at[0] + r1 * std::cos(a0), y + h, at[1] + r1 * std::sin(a0) }, q1{ at[0] + r1 * std::cos(a1), y + h, at[1] + r1 * std::sin(a1) };
                V3 s0{ at[0] + r2 * std::cos(a0), y + h + dh * 0.75, at[1] + r2 * std::sin(a0) }, s1{ at[0] + r2 * std::cos(a1), y + h + dh * 0.75, at[1] + r2 * std::sin(a1) };
                m.poly("dome", { q0, q1, s1, s0 }, { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, -1 } }, { out[0], 0.5, out[2] });
                m.poly("dome", { s0, s1, { at[0], y + h + dh, at[1] } }, { { 0, 0 }, { 1, 0 }, { 0.5, -1 } }, { out[0], 2, out[2] });
                // The ledge the dome sits on.
                m.poly("colonnade", { { p0[0], y + h, p0[2] }, { p1[0], y + h, p1[2] }, q1, q0 }, { { 0, 0 }, { 1, 0 }, { 1, 0.1 }, { 0, 0.1 } }, { 0, 1, 0 });
            }
        } else if (kind == "colonnade") {
            // A curved row of columns: an arc of `segments` wall pieces round `at`, radius r, from a0 to a1 degrees.
            double r = l.value("radius", 60.0), h = l.value("height", 14.0), thick = l.value("thick", 6.0);
            double a0 = l.value("from", 0.0) * pi / 180, a1 = l.value("to", 180.0) * pi / 180;
            int n = l.value("segments", 6);
            for (int k = 0; k < n; k++) {
                double b0 = a0 + (a1 - a0) * k / n, b1 = a0 + (a1 - a0) * (k + 1) / n;
                for (double rr : { r - thick / 2, r + thick / 2 }) {
                    V3 p0{ at[0] + rr * std::cos(b0), y, at[1] + rr * std::sin(b0) }, p1{ at[0] + rr * std::cos(b1), y, at[1] + rr * std::sin(b1) };
                    double sgn = rr > r ? 1 : -1;
                    V3 out{ sgn * std::cos((b0 + b1) / 2), 0, sgn * std::sin((b0 + b1) / 2) };
                    m.poly("colonnade", { p0, p1, { p1[0], y + h, p1[2] }, { p0[0], y + h, p0[2] } }, { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, -1 } }, out);
                }
                V3 i0{ at[0] + (r - thick / 2) * std::cos(b0), y + h, at[1] + (r - thick / 2) * std::sin(b0) };
                V3 i1{ at[0] + (r - thick / 2) * std::cos(b1), y + h, at[1] + (r - thick / 2) * std::sin(b1) };
                V3 o0{ at[0] + (r + thick / 2) * std::cos(b0), y + h, at[1] + (r + thick / 2) * std::sin(b0) };
                V3 o1{ at[0] + (r + thick / 2) * std::cos(b1), y + h, at[1] + (r + thick / 2) * std::sin(b1) };
                m.poly("colonnade", { i0, i1, o1, o0 }, { { 0, 0 }, { 1, 0 }, { 1, 0.15 }, { 0, 0.15 } }, { 0, 1, 0 });
            }
        } else if (kind == "bridge") {
            // A suspension bridge from `at` to `to`: a deck at `deck` meters, towers at the given fractions of its
            // length, and the cables between them as thin bands.
            std::vector<double> to = l.at("to").get<std::vector<double>>();
            double deck = l.value("deck", 20.0), width = l.value("width", 16.0), tower = l.value("tower", 70.0);
            std::vector<double> towers = l.value("towers", std::vector<double>{ 0.3, 0.7 });
            V3 dir = unit({ to[0] - at[0], 0, to[1] - at[1] });
            V3 lat{ -dir[2], 0, dir[0] };
            double len = std::hypot(to[0] - at[0], to[1] - at[1]);
            double ang = std::atan2(dir[2], dir[0]) * 180 / pi;
            double dy = deck + l.value("base", s.water_level);
            box((at[0] + to[0]) / 2, (at[1] + to[1]) / 2, len, width, ang, dy - 3, 3, "steel", "asphalt", 8);
            std::vector<V3> tops;
            for (double f : towers) {
                double tx = at[0] + (to[0] - at[0]) * f, tz = at[1] + (to[1] - at[1]) * f;
                for (double sd : { -1.0, 1.0 }) {
                    box(tx + lat[0] * sd * width / 2, tz + lat[2] * sd * width / 2, 5, 5, ang, s.water_level, dy - s.water_level + tower, "steel", "steel", 8);
                }
                tops.push_back({ tx, dy + tower, tz });
            }
            // Cables: from each end of the deck up to the first tower, between towers sagging, down to the far end.
            std::vector<V3> path = { { at[0], dy, at[1] } };
            for (size_t i = 0; i < tops.size(); i++) {
                if (i > 0) {
                    const V3& a = tops[i - 1];
                    const V3& b = tops[i];
                    for (int k = 1; k < 4; k++) {
                        double f = k / 4.0;
                        double sag = 4 * f * (1 - f) * tower * 0.8;
                        path.push_back({ a[0] + (b[0] - a[0]) * f, a[1] - sag, a[2] + (b[2] - a[2]) * f });
                    }
                }
                path.push_back(tops[i]);
            }
            path.push_back({ to[0], dy, to[1] });
            for (double sd : { -1.0, 1.0 }) {
                for (size_t i = 0; i + 1 < path.size(); i++) {
                    V3 a{ path[i][0] + lat[0] * sd * width / 2, path[i][1], path[i][2] + lat[2] * sd * width / 2 };
                    V3 b{ path[i + 1][0] + lat[0] * sd * width / 2, path[i + 1][1], path[i + 1][2] + lat[2] * sd * width / 2 };
                    V3 out{ lat[0] * sd, 0, lat[2] * sd };
                    m.poly("steel", { a, b, { b[0], b[1] - 2, b[2] }, { a[0], a[1] - 2, a[2] } }, { { 0, 0 }, { 1, 0 }, { 1, 0.1 }, { 0, 0.1 } }, out);
                    m.poly("steel", { a, b, { b[0], b[1] - 2, b[2] }, { a[0], a[1] - 2, a[2] } }, { { 0, 0 }, { 1, 0 }, { 1, 0.1 }, { 0, 0.1 } }, { -out[0], 0, -out[2] });
                }
            }
        } else {
            log += "diorama: unknown landmark " + kind + "\n";
        }
    }

    std::vector<a49::Glb::Prim> prims;
    for (a49::Glb::Prim& p : m.prims) if (!p.tris.empty()) prims.push_back(std::move(p));
    m.g.textured_mesh("DIORAMA", prims);
    out = m.g.bytes();
    snprintf(line, sizeof(line), "diorama: %zu triangles, %zu buildings, %zu trees, %zu landmarks, route of %zu points\n",
             m.triangles, building_count, s.trees.size(), s.landmarks.size(), route.size());
    log += line;
    if (!near_route.empty()) log += "diorama: buildings over the route (x,z)clearance:" + near_route + "\n";
    if (!off_island.empty()) log += "diorama: off the island:" + off_island + "\n";
    return true;
}
