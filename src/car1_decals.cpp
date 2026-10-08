// Rush 1's car decals as masks on Rush 2's car panel textures. Port of tools/rush1/cardecal.py (and the parts of
// tools/rush1/cartex.py it uses); it must give the same masks bit for bit (tools/rush1/cpp_test/build.bat checks it).
// No game or UI dependencies, so the offline test links it alone.
//
// Both games' car files are a model container: header, model records (0x34 bytes, +12 = the display list), name
// records (0x18) and texture records (0x20: name[16], u16 w, u16 h, u8 fmt, u8 siz, s16 palette, u32 data, u32 flags).
// Rush 1's header is {names, model count, 0, 0, textures, texture count, palettes, palette count} with segment
// pointers (each car file has its own segment, in the top byte) and F3DEX1 display lists; Rush 2's is 10 words
// {models, names, textures, palettes, model count, texture count, palette count, ...} with F3DEX2 lists. Coordinates
// are the same car-local ones in both.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>

#include "car1_decals.h"

namespace rush2::car1decals {
    const Car cars[car_count] = {
        // Rush 1 asset 25 + its car's place in {BMW, CAMARO, SUPRA, BUGATTI, VWBUS, VIPER, VWBUG, CONCEPT, TAXI, HOTROD, FORM1}.
        { "CAMARO", 5, 26, false },
        { "VWBUS", 8, 29, true },
        { "TAXI", 16, 33, true },
        { "HOTROD", 17, 34, false },
    };
}

namespace {
    using rush2::car1decals::Pattern;

    // Radius, in car units, within which a Rush 2 texel's body point must find the Rush 1 body.
    constexpr double max_distance = 3.0;
    constexpr int min_speck = 6;   // decal components (8-connected) smaller than this are dropped

    uint32_t u32(const std::vector<uint8_t>& d, size_t o) {
        return o + 4 <= d.size() ? (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3] : 0;
    }
    uint32_t u16(const std::vector<uint8_t>& d, size_t o) {
        return o + 2 <= d.size() ? (uint32_t(d[o]) << 8) | d[o + 1] : 0;
    }
    int s16(const std::vector<uint8_t>& d, size_t o) {
        return int16_t(u16(d, o));
    }
    std::string cstr(const std::vector<uint8_t>& d, size_t o) {
        std::string s;
        for (size_t i = o; i < o + 16 && i < d.size() && d[i] != 0; i++) {
            s += char(d[i]);
        }
        return s;
    }

    struct Vtx {
        double x, y, z, s, t;
    };
    struct Tri {
        Vtx v[3];
        bool tex = false;
        uint32_t data = 0;      // texel offset in the file
        int w = 0, h = 0;
        double uls = 0, ult = 0;
    };
    using Meshes = std::map<std::string, std::vector<Tri>>;

    struct TexRec {
        std::string name;
        int w, h;
        uint32_t data;
    };

    // Rush 1 car file: model names, display list starts and its palette (the RED paint set; fixed colours are the
    // same in all ten).
    struct File {
        const std::vector<uint8_t>& d;
        int game;
        std::vector<std::string> models;
        std::vector<uint32_t> starts;
        std::vector<TexRec> textures;   // Rush 2 only
        uint32_t seg = 0;
    };

    bool parse(File& f) {
        const auto& d = f.d;
        if (d.size() < 0x40) {
            return false;
        }
        uint32_t names, count, models;
        if (f.game == 1) {
            names = u32(d, 0) & 0xFFFFFF;
            count = u32(d, 4);
            models = 0x20;
            f.seg = u32(d, 0) >> 24;
        }
        else {
            models = u32(d, 0);
            names = u32(d, 4);
            count = u32(d, 16);
            uint32_t tex = u32(d, 8), tex_count = u32(d, 20);
            for (uint32_t i = 0; i < tex_count; i++) {
                size_t o = tex + i * 0x20;
                f.textures.push_back({ cstr(d, o), (int)u16(d, o + 16), (int)u16(d, o + 18), u32(d, o + 24) });
            }
        }
        if (count > 256) {
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            f.models.push_back(cstr(d, names + i * 0x18));
            f.starts.push_back(u32(d, models + i * 0x34 + 12));
        }
        return true;
    }

    // Walks every model's display list: Rush 1 = F3DEX1 (VTX 04, DL 06, TRI1 BF, TRI2 B1, vertex indices x2, ENDDL
    // B8), Rush 2 = F3DEX2 (VTX 01, DL DE, TRI1 05, TRI2 06, ENDDL DF). The texture of a triangle is the largest
    // texture load (SETTIMG then SETTILESIZE) so far in the model: Rush 1 car panels pick between a 16x8 and a 64x32
    // list with B4/B0 (RDPHALF_1 + BRANCH_Z), and walking both takes the large, near one.
    Meshes meshes(const File& f) {
        const auto& d = f.d;
        Meshes out;
        const uint32_t vtx_op = f.game == 1 ? 0x04 : 0x01, dl_op = f.game == 1 ? 0x06 : 0xDE, end_op = f.game == 1 ? 0xB8 : 0xDF;
        const uint32_t dl_seg = f.game == 1 ? f.seg : 0;
        for (size_t m = 0; m < f.models.size(); m++) {
            std::vector<Tri>& tris = out[f.models[m]];
            Vtx vbuf[64];
            bool have[64] = {};
            uint32_t img = 0;
            bool have_img = false, have_best = false;
            uint32_t best_data = 0;
            int best_w = 0, best_h = 0;
            double best_uls = 0, best_ult = 0;
            auto walk = [&](auto&& self, uint32_t a, int depth) -> void {
                size_t o = a & 0xFFFFFF;
                while (o + 8 <= d.size()) {
                    uint32_t w0 = u32(d, o), w1 = u32(d, o + 4), op = w0 >> 24;
                    if (op == vtx_op) {
                        int n, v0;
                        if (f.game == 1) {
                            n = (w0 >> 10) & 0x3F;
                            v0 = int((w0 >> 16) & 0xFF) / 2;
                        }
                        else {
                            n = (w0 >> 12) & 0xFF;
                            v0 = int((w0 >> 1) & 0x7F) - n;
                        }
                        size_t src = w1 & 0xFFFFFF;
                        for (int k = 0; k < n; k++) {
                            size_t q = src + k * 16;
                            if (q + 16 <= d.size() && v0 + k >= 0 && v0 + k < 64) {
                                vbuf[v0 + k] = { (double)s16(d, q), (double)s16(d, q + 2), (double)s16(d, q + 4),
                                                 s16(d, q + 8) / 32.0, s16(d, q + 10) / 32.0 };
                                have[v0 + k] = true;
                            }
                        }
                    }
                    else if (op == 0xFD) {
                        img = w1 & 0xFFFFFF;
                        have_img = true;
                    }
                    else if (op == 0xF2 && have_img) {
                        int w = (int((w1 >> 12) & 0xFFF) - int((w0 >> 12) & 0xFFF)) / 4 + 1;
                        int h = (int(w1 & 0xFFF) - int(w0 & 0xFFF)) / 4 + 1;
                        if (!have_best || w * h >= best_w * best_h) {
                            have_best = true;
                            best_data = img;
                            best_w = w;
                            best_h = h;
                            best_uls = ((w0 >> 12) & 0xFFF) / 4.0;
                            best_ult = (w0 & 0xFFF) / 4.0;
                        }
                    }
                    else if ((f.game == 1 && (op == 0xBF || op == 0xB1)) || (f.game == 2 && (op == 0x05 || op == 0x06))) {
                        uint32_t ids[2][3];
                        int count;
                        if ((f.game == 1 && op == 0xBF)) {
                            ids[0][0] = (w1 >> 16) & 0xFF; ids[0][1] = (w1 >> 8) & 0xFF; ids[0][2] = w1 & 0xFF;
                            count = 1;
                        }
                        else if (f.game == 2 && op == 0x05) {
                            ids[0][0] = (w0 >> 16) & 0xFF; ids[0][1] = (w0 >> 8) & 0xFF; ids[0][2] = w0 & 0xFF;
                            count = 1;
                        }
                        else {
                            ids[0][0] = (w0 >> 16) & 0xFF; ids[0][1] = (w0 >> 8) & 0xFF; ids[0][2] = w0 & 0xFF;
                            ids[1][0] = (w1 >> 16) & 0xFF; ids[1][1] = (w1 >> 8) & 0xFF; ids[1][2] = w1 & 0xFF;
                            count = 2;
                        }
                        for (int c = 0; c < count; c++) {
                            Tri t;
                            bool ok = true;
                            for (int k = 0; k < 3; k++) {
                                uint32_t i = ids[c][k] / 2;
                                if (i >= 64 || !have[i]) {
                                    ok = false;
                                    break;
                                }
                                t.v[k] = vbuf[i];
                            }
                            if (ok) {
                                if (have_best) {
                                    t.tex = true;
                                    t.data = best_data;
                                    t.w = best_w;
                                    t.h = best_h;
                                    t.uls = best_uls;
                                    t.ult = best_ult;
                                }
                                tris.push_back(t);
                            }
                        }
                    }
                    else if (op == dl_op && (w1 >> 24) == dl_seg && depth < 6) {
                        self(self, w1, depth + 1);
                    }
                    else if (op == end_op) {
                        return;
                    }
                    o += 8;
                }
            };
            walk(walk, f.starts[m], 0);
        }
        return out;
    }

    struct Vec {
        double x, y, z;
    };
    Vec sub(const Vec& a, const Vec& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    double dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec cross(const Vec& a, const Vec& b) {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }
    bool unit(const Vec& v, Vec& out) {
        double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (n < 1e-9) {
            return false;
        }
        out = { v.x / n, v.y / n, v.z / n };
        return true;
    }
    Vec pos(const Vtx& v) { return { v.x, v.y, v.z }; }

    // Barycentric coordinates of the point of triangle abc closest to p (Ericson, Real-Time Collision Detection).
    void closest_bary(const Vec& p, const Vec& a, const Vec& b, const Vec& c, double out[3]) {
        Vec ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
        double d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0 && d2 <= 0) { out[0] = 1; out[1] = 0; out[2] = 0; return; }
        Vec bp = sub(p, b);
        double d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0 && d4 <= d3) { out[0] = 0; out[1] = 1; out[2] = 0; return; }
        double vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) {
            double v = d1 / (d1 - d3);
            out[0] = 1 - v; out[1] = v; out[2] = 0;
            return;
        }
        Vec cp = sub(p, c);
        double d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0 && d5 <= d6) { out[0] = 0; out[1] = 0; out[2] = 1; return; }
        double vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) {
            double w = d2 / (d2 - d6);
            out[0] = 1 - w; out[1] = 0; out[2] = w;
            return;
        }
        double va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
            double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            out[0] = 0; out[1] = 1 - w; out[2] = w;
            return;
        }
        double den = 1.0 / (va + vb + vc);
        double v = vb * den, w = vc * den;
        out[0] = 1 - v - w; out[1] = v; out[2] = w;
    }

    struct Rgb {
        int r, g, b, a;
    };
    Rgb rgba16(uint32_t v) {
        return { int((v >> 11) & 31) * 255 / 31, int((v >> 6) & 31) * 255 / 31, int((v >> 1) & 31) * 255 / 31, (v & 1) ? 255 : 0 };
    }
    double luma(const Rgb& c) {
        return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b;
    }

    // Rush 1's paint ramps (palette indices that differ between its ten paint sets) and Rush 2's (1-31 main, 33-63
    // accent: tools/rush1/cartex.py R1_BODY / R2_BODY).
    bool r1_body(int i) {
        return (i >= 1 && i <= 25) || i == 30 || (i >= 65 && i <= 86);
    }
    bool r2_body(int i) {
        return (i >= 1 && i <= 31) || (i >= 33 && i <= 63);
    }

    // Palette `want` (a name containing it) of a car file's palette table, or all 256 entries of the named one.
    bool read_palette(const std::vector<uint8_t>& d, uint32_t table, uint32_t count, const char* want, std::vector<Rgb>& out) {
        for (uint32_t i = 0; i < count && i < 256; i++) {
            size_t o = (table & 0xFFFFFF) + i * 0x18;
            std::string name = cstr(d, o);
            if (name.find(want) == std::string::npos) {
                continue;
            }
            size_t data = u32(d, o + 20) & 0xFFFFFF;
            if (data + 512 > d.size()) {
                return false;
            }
            out.clear();
            for (int k = 0; k < 256; k++) {
                out.push_back(rgba16(u16(d, data + k * 2)));
            }
            return true;
        }
        return false;
    }

    void drop_specks(std::vector<uint8_t>& mask, int w, int h) {
        std::vector<uint8_t> seen(w * h, 0), out(w * h, 0);
        std::vector<int> comp;
        for (int s = 0; s < w * h; s++) {
            if (!mask[s] || seen[s]) {
                continue;
            }
            comp.assign(1, s);
            seen[s] = 1;
            for (size_t k = 0; k < comp.size(); k++) {
                int cy = comp[k] / w, cx = comp[k] % w;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int ny = cy + dy, nx = cx + dx;
                        if (ny >= 0 && ny < h && nx >= 0 && nx < w && mask[ny * w + nx] && !seen[ny * w + nx]) {
                            seen[ny * w + nx] = 1;
                            comp.push_back(ny * w + nx);
                        }
                    }
                }
            }
            if ((int)comp.size() >= min_speck) {
                for (int q : comp) {
                    out[q] = 1;
                }
            }
        }
        mask = out;
    }

    // The model-name prefix of a car in a file (the part before "D0_FL1": CAMARO, BUGATTI / BUGAT, ...).
    bool prefix_of(const Meshes& m, std::string& prefix) {
        for (const auto& [name, tris] : m) {
            if (name.size() > 6 && name.compare(name.size() - 6, 6, "D0_FL1") == 0) {
                prefix = name.substr(0, name.size() - 6);
                return true;
            }
        }
        return false;
    }
}

bool rush2::car1decals::build(int car, const std::vector<uint8_t>& r1_car, const std::vector<uint8_t>& r2_car,
                              const std::vector<uint8_t>& stripe_asset, Pattern& out, const char** why) {
    const char* fail_text;
    if (why == nullptr) {
        why = &fail_text;
    }
    if (car < 0 || car >= car_count) {
        *why = "car index";
        return false;
    }
    const bool white = cars[car].white;
    File f1{ r1_car, 1 }, f2{ r2_car, 2 };
    if (!parse(f1)) {
        *why = "Rush 1 car header";
        return false;
    }
    if (!parse(f2)) {
        *why = "Rush 2 car header";
        return false;
    }
    std::vector<Rgb> pal1, pal2;
    if (!read_palette(r1_car, u32(r1_car, 24), u32(r1_car, 28), "RED", pal1)) {
        *why = "Rush 1 RED palette";
        return false;
    }
    if (!read_palette(stripe_asset, u32(stripe_asset, 12), u32(stripe_asset, 24), "CARPALETTE", pal2)) {
        *why = "CARPALETTE";
        return false;
    }
    Meshes m1 = meshes(f1), m2 = meshes(f2);
    std::string pre1, pre2;
    if (!prefix_of(m1, pre1) || !prefix_of(m2, pre2)) {
        *why = "model names";
        return false;
    }

    struct R1Tri {
        Vec p[3];
        double uv[3][2];
        Tri tri;
        Vec n;
        double cx;
    };
    std::vector<R1Tri> r1;
    for (const char* part : { "FL1", "FR1", "RL1", "RR1", "TOP1", "WIN1" }) {
        for (const Tri& t : m1[pre1 + "D0_" + part]) {
            if (!t.tex) {
                continue;
            }
            R1Tri r;
            for (int k = 0; k < 3; k++) {
                r.p[k] = pos(t.v[k]);
                r.uv[k][0] = t.v[k].s;
                r.uv[k][1] = t.v[k].t;
            }
            if (!unit(cross(sub(r.p[1], r.p[0]), sub(r.p[2], r.p[0])), r.n)) {
                continue;
            }
            r.cx = (r.p[0].x + r.p[1].x + r.p[2].x) / 3.0;
            r.tri = t;
            r1.push_back(r);
        }
    }

    // Rush 2's panel textures by name, and the triangles drawing each.
    std::map<uint32_t, const TexRec*> by_data;
    for (const TexRec& t : f2.textures) {
        by_data[t.data] = &t;
    }
    std::map<std::string, std::vector<const Tri*>> panels;
    for (const char* part : { "FL1", "FR1", "RL1", "RR1", "TOP1" }) {
        for (const Tri& t : m2[pre2 + "D0_" + part]) {
            auto it = t.tex ? by_data.find(t.data) : by_data.end();
            if (it != by_data.end()) {
                panels[it->second->name].push_back(&t);
            }
        }
    }

    for (const auto& [name, tris] : panels) {
        const TexRec* tex = nullptr;
        for (const TexRec& t : f2.textures) {
            if (t.name == name) {
                tex = &t;
                break;
            }
        }
        size_t at = name.rfind("D0_");
        if (tex == nullptr || at == std::string::npos || at + 3 >= name.size() || name.size() != at + 4) {
            continue;
        }
        int n = name[at + 3] - '0';
        if (n < 1 || n > 6) {
            continue;
        }
        int w = tex->w, h = tex->h;
        if (w <= 0 || h <= 0 || tex->data + size_t(w) * h > r2_car.size()) {
            continue;
        }
        std::vector<uint8_t> mask(size_t(w) * h, 0);
        for (const Tri* tp : tris) {
            const Tri& t = *tp;
            Vec P[3];
            double UV[3][2];
            for (int k = 0; k < 3; k++) {
                P[k] = pos(t.v[k]);
                UV[k][0] = t.v[k].s;
                UV[k][1] = t.v[k].t;
            }
            Vec n2;
            if (!unit(cross(sub(P[1], P[0]), sub(P[2], P[0])), n2)) {
                continue;
            }
            double a0 = UV[0][0] - UV[2][0], a1 = UV[1][0] - UV[2][0];
            double b0 = UV[0][1] - UV[2][1], b1 = UV[1][1] - UV[2][1];
            double det = a0 * b1 - a1 * b0;
            if (std::fabs(det) < 1e-9) {
                continue;
            }
            int x0 = (int)std::floor(std::min({ UV[0][0], UV[1][0], UV[2][0] }));
            int y0 = (int)std::floor(std::min({ UV[0][1], UV[1][1], UV[2][1] }));
            int x1 = (int)std::ceil(std::max({ UV[0][0], UV[1][0], UV[2][0] }));
            int y1 = (int)std::ceil(std::max({ UV[0][1], UV[1][1], UV[2][1] }));
            for (int j = std::max(y0, 0); j <= std::min(y1, h - 1); j++) {
                for (int i = std::max(x0, 0); i <= std::min(x1, w - 1); i++) {
                    double rx = i - UV[2][0], ry = j - UV[2][1];
                    double l0 = (rx * b1 - ry * a1) / det;
                    double l1 = (a0 * ry - b0 * rx) / det;
                    double l2 = 1 - l0 - l1;
                    if (std::min({ l0, l1, l2 }) < -0.02) {
                        continue;
                    }
                    Vec p = { l0 * P[0].x + l1 * P[1].x + l2 * P[2].x, l0 * P[0].y + l1 * P[1].y + l2 * P[2].y,
                              l0 * P[0].z + l1 * P[1].z + l2 * P[2].z };
                    int best = -1;
                    double best_dist = 0, best_bc[3] = {};
                    for (size_t k = 0; k < r1.size(); k++) {
                        const R1Tri& r = r1[k];
                        if (std::fabs(dot(r.n, n2)) < 0.5) {
                            continue;
                        }
                        if ((r.cx < 0) != (p.x < 0) && std::fabs(p.x) > 3) {
                            continue;
                        }
                        double bc[3];
                        closest_bary(p, r.p[0], r.p[1], r.p[2], bc);
                        Vec q = { bc[0] * r.p[0].x + bc[1] * r.p[1].x + bc[2] * r.p[2].x,
                                  bc[0] * r.p[0].y + bc[1] * r.p[1].y + bc[2] * r.p[2].y,
                                  bc[0] * r.p[0].z + bc[1] * r.p[1].z + bc[2] * r.p[2].z };
                        Vec dd = sub(q, p);
                        double dist = std::sqrt(dd.x * dd.x + dd.y * dd.y + dd.z * dd.z);
                        if (best < 0 || dist < best_dist) {
                            best = (int)k;
                            best_dist = dist;
                            best_bc[0] = bc[0]; best_bc[1] = bc[1]; best_bc[2] = bc[2];
                        }
                    }
                    if (best < 0 || best_dist > max_distance) {
                        continue;
                    }
                    const R1Tri& r = r1[best];
                    int u = (int)std::floor((best_bc[0] * r.uv[0][0] + best_bc[1] * r.uv[1][0] + best_bc[2] * r.uv[2][0]) - r.tri.uls + 0.5);
                    int v = (int)std::floor((best_bc[0] * r.uv[0][1] + best_bc[1] * r.uv[1][1] + best_bc[2] * r.uv[2][1]) - r.tri.ult + 0.5);
                    u = std::min(std::max(u, 0), r.tri.w - 1);
                    v = std::min(std::max(v, 0), r.tri.h - 1);
                    size_t at1 = r.tri.data + size_t(v) * r.tri.w + u;
                    if (at1 >= r1_car.size()) {
                        continue;
                    }
                    int idx = r1_car[at1];
                    int s = j * w + i;
                    int idx2 = r2_car[tex->data + s];
                    const Rgb& c1 = pal1[idx];
                    const Rgb& c2 = pal2[idx2];
                    double l1c = luma(c1), l2c = luma(c2);
                    bool decal;
                    if (white) {
                        decal = l1c > 190 && l1c - l2c > 40;
                    }
                    else {
                        decal = std::max({ c1.r, c1.g, c1.b }) - std::min({ c1.r, c1.g, c1.b }) > 50 && c1.g >= 16;
                    }
                    mask[s] = (decal && idx != 0 && !r1_body(idx) && r2_body(idx2)) ? 1 : 0;
                }
            }
        }
        drop_specks(mask, w, h);
        Panel& p = out.panel[n];
        p.w = w;
        p.h = h;
        p.full = mask;
        p.lod.assign(size_t(w / 4) * (h / 4), 0);
        for (int y = 0; y < h / 4; y++) {
            for (int x = 0; x < w / 4; x++) {
                int sum = 0;
                for (int dy = 0; dy < 4; dy++) {
                    for (int dx = 0; dx < 4; dx++) {
                        sum += mask[(y * 4 + dy) * w + x * 4 + dx];
                    }
                }
                p.lod[y * (w / 4) + x] = sum >= 8 ? 1 : 0;
            }
        }
    }
    return true;
}
