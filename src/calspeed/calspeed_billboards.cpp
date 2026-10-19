// Billboard trees for California Speed's tracks (include/calspeed_billboards.h, docs/calspeed_research.md#billboard-trees).
//
// California Speed builds most roadside trees as one upright see-through card in a track section's list. A card is
// kept flat unless it is a lone quad (two triangles sharing no corner with anything else in its section) standing
// upright as a rectangle, at most 1.6 times as wide as it is tall (tree walls and hedges are long strips), whose
// texels are partly see-through and mostly green where solid, and that no other upright surface crosses or continues
// (crossed "X" trees, rows of cards making a wall, signs on posts).
//
// Each section model with cards gets a copy of its LOD 0 list with their triangles taken out (G_NOOP, or G_TRI1 of a
// G_TRI2's other half), followed by the cards, grouped by draw state (the state replayed as calspeed_scenery.cpp does:
// G_RDPPIPESYNC, the other modes, combine and geometry mode the list set, and the G_DL into its texture-load list).
// Each card is its four vertices around its base's middle, upright, and is drawn as
//     G_MTX push, mul    T(base)                 (a static matrix after the container's end)
//     G_DL               turn                    (G_MTX nopush, mul 0x0D000000; G_ENDDL)
//     G_VTX 4, G_TRI2, G_POPMTX
// The `turn` list holds the only per-view pointer: segment 0x0D, which src/interpolation.cpp points at the view's
// yaw-only matrix before each view's scene is drawn. It works because Rush 2's relocator (func_8007786C) and the
// merge (track2049_convert.cpp walk_reloc) rebase only the top-level lists' pointers and never enter a G_DL target,
// so 0x0D000000 stays as it is; a pointer in the section's own list would get the container's load address added.
// The yaw matrix is view_billboard_basis_8007BF2C's: it shows the model's -z side to the camera, so a card's front is
// its local -z. Its front is the side away from cross(v1 - v0, v2 - v0) of its triangles (`front` below is that
// cross product's side): the game culls back faces and Rush 2's world turns x the other way, so the side the cross
// product points to is the culled one (cards built facing it were invisible). The cards stay part of their section's node: they are culled and
// hidden with it and cost no scene nodes. A section used by a record that turns or scales it is left alone (the turn
// is worked out in world axes).

#include "calspeed_billboards.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace {
    using Bytes = std::vector<uint8_t>;
    using V3 = std::array<double, 3>;

    V3 sub(const V3& a, const V3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    V3 cross(const V3& a, const V3& b) {
        return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    }
    double len(const V3& a) { return std::sqrt(dot(a, a)); }

    uint32_t u32(const Bytes& d, size_t o) {
        return o + 4 <= d.size() ? (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3] : 0;
    }
    uint16_t u16(const Bytes& d, size_t o) { return o + 2 <= d.size() ? uint16_t((d[o] << 8) | d[o + 1]) : 0; }
    int16_t s16(const Bytes& d, size_t o) { return int16_t(u16(d, o)); }
    uint8_t u8(const Bytes& d, size_t o) { return o < d.size() ? d[o] : 0; }
    float f32(const Bytes& d, size_t o) { uint32_t w = u32(d, o); float f; memcpy(&f, &w, 4); return f; }
    void put32(Bytes& d, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) d[o + i] = uint8_t(v >> (24 - 8 * i)); }
    void add32(Bytes& d, uint32_t v) { for (int i = 0; i < 4; i++) d.push_back(uint8_t(v >> (24 - 8 * i))); }
    void add16(Bytes& d, uint16_t v) { d.push_back(uint8_t(v >> 8)); d.push_back(uint8_t(v)); }
    void align8(Bytes& d) { while (d.size() % 8) d.push_back(0); }
    std::string cstr(const Bytes& d, size_t o, size_t n) {
        std::string s;
        for (size_t i = 0; i < n && o + i < d.size() && d[o + i]; i++) s += char(d[o + i]);
        return s;
    }

    // ---- choosing ---------------------------------------------------------------------------------------------
    constexpr double max_aspect = 1.6;      // width / height: wider cards are tree walls and hedges
    constexpr double min_height = 6;        // ft
    constexpr double max_tilt = 0.1;        // |normal.y| of an upright card
    constexpr double square = 0.15;         // its bottom corners (and top ones) level to this fraction of its height
    constexpr float min_clear = 0.08f;      // see-through texels, of all it shows
    constexpr double touch = 1.0;           // ft: an upright surface this close to a card's edge continues it

    // Each track's single-tree textures, picked by eye from tools/calspeed/billboards.py --review's sheet of every
    // texture on a card that passes the shape tests (signs, cattle, poles, bridge cables, tree rows and the like are
    // left out).
    const std::map<int, std::set<std::string>> trees = {
        { 0, { "T06CC8", "T08FC8", "T097E8", "T0A428", "T161C8" } },                         // Monterey
        { 1, { "T05A78", "T08538", "T0A558", "T20028" } },                                   // Silicon Valley
        { 2, { "T02868", "T02C88", "T034A8", "T03CC8", "T11A18", "T13C78", "T2B018" } },     // Highway 1
        { 4, { "T00148", "T00968", "T02E28", "T042E8", "T05D08", "T06528", "T14408", "T19528" } },  // Central Valley
        { 5, { "T01C78", "T02918", "T03138", "T05268", "T18F18" } },                         // Santa Cruz
        { 6, { "T08AC8", "T092E8", "T20368", "T20B88", "T213A8", "T21BC8" } },               // San Diego
        { 9, { "T08B78", "T08F98", "T093B8", "T09BF8" } },                                   // Mt. Shasta
        { 10, { "T10168", "T10988", "T111A8", "T115C8", "T119E8", "T13EA8" } },              // Yosemite
        { 11, { "T029F0", "T06E10", "T07B30", "T08350", "T08B70", "T08F90", "T0E670" } },     // Mojave Desert
    };
    bool tree_texture(int t, const std::string& name) {
        auto it = trees.find(t);
        return it != trees.end() && it->second.contains(name);
    }

    // ---- textures -------------------------------------------------------------------------------------------------
    struct Tex {
        uint32_t texels = 0, palette = 0;
        int fmt = 0, siz = 2, width = 0, height = 0, src_width = 0;
        bool ok = false;
        std::string name;   // "T" and the texels' offset in hex
    };

    // What a texture-load list loads into render tile 0.
    Tex read_tex(const Bytes& d, uint32_t list, uint32_t end) {
        Tex t;
        uint32_t timg = 0, timg_w = 0;
        bool tile_load = false;
        for (uint32_t o = list; o + 8 <= end && o < list + 8 * 64; o += 8) {
            uint32_t w0 = u32(d, o), w1 = u32(d, o + 4);
            uint8_t op = uint8_t(w0 >> 24);
            if (op == 0xFD) { timg = w1 & 0xFFFFFF; timg_w = (w0 & 0xFFF) + 1; }
            else if (op == 0xF0) t.palette = timg;
            else if (op == 0xF3) { t.texels = timg; tile_load = false; }
            else if (op == 0xF4) { t.texels = timg; tile_load = true; t.src_width = int(timg_w); }
            else if (op == 0xF5 && ((w1 >> 24) & 7) == 0) { t.fmt = (w0 >> 21) & 7; t.siz = (w0 >> 19) & 3; }
            else if (op == 0xF2 && ((w1 >> 24) & 7) == 0) {
                t.width = int(((w1 >> 12) & 0xFFF) / 4 + 1);
                t.height = int((w1 & 0xFFF) / 4 + 1);
            }
            else if (op == 0xDF) break;
        }
        if (!tile_load) t.src_width = t.width;
        t.ok = t.texels && t.width > 0 && t.height > 0;
        // Its name: most of California Speed's textures have no texture record, so the texels' offset in the track's
        // container (California Speed's own asset: what is appended to it goes after).
        char name[16];
        snprintf(name, sizeof(name), "T%05X", t.texels);
        t.name = name;
        return t;
    }

    // RGBA (0-1) of texel (x, y), wrapped.
    void texel(const Bytes& d, const Tex& t, int x, int y, float out[4]) {
        x = ((x % t.width) + t.width) % t.width;
        y = ((y % t.height) + t.height) % t.height;
        uint32_t i = uint32_t(y * t.src_width + x);
        auto nib = [&](uint32_t k) { return (u8(d, t.texels + k / 2) >> ((k & 1) ? 0 : 4)) & 15; };
        auto c16 = [&](uint16_t v) {
            out[0] = ((v >> 11) & 31) / 31.0f; out[1] = ((v >> 6) & 31) / 31.0f; out[2] = ((v >> 1) & 31) / 31.0f;
            out[3] = float(v & 1);
        };
        auto gray = [&](float g, float a) { out[0] = out[1] = out[2] = g; out[3] = a; };
        switch (t.fmt) {
        case 0:
            if (t.siz == 3) {
                uint32_t v = u32(d, t.texels + i * 4);
                out[0] = (v >> 24) / 255.0f; out[1] = ((v >> 16) & 255) / 255.0f; out[2] = ((v >> 8) & 255) / 255.0f;
                out[3] = (v & 255) / 255.0f;
            }
            else c16(u16(d, t.texels + i * 2));
            return;
        case 2: c16(u16(d, t.palette + (t.siz == 0 ? nib(i) : u8(d, t.texels + i)) * 2)); return;
        case 3:
            if (t.siz == 0) { int v = nib(i); gray((v >> 1) / 7.0f, float(v & 1)); }
            else if (t.siz == 1) { int v = u8(d, t.texels + i); gray((v >> 4) / 15.0f, (v & 15) / 15.0f); }
            else { uint16_t v = u16(d, t.texels + i * 2); gray((v >> 8) / 255.0f, (v & 255) / 255.0f); }
            return;
        case 4:
            if (t.siz == 0) { float g = nib(i) / 15.0f; gray(g, g); }
            else { float g = u8(d, t.texels + i) / 255.0f; gray(g, g); }
            return;
        default: gray(0.5f, 1.0f); return;
        }
    }

    // ---- reading the sections -----------------------------------------------------------------------------------
    struct Cmd { uint32_t w0, w1; };
    struct State {
        std::vector<Cmd> cmds;
        uint32_t load = 0;
    };
    struct Vtx {
        V3 p;               // model units (1/16 ft)
        int16_t st[2];
        uint8_t c[4];
    };
    struct Tri {
        Vtx v[3];
        int slot[3];
        int state;
        uint32_t cmd;       // its G_TRI1 / G_TRI2 in the file
        int half;           // 0: the first (or only) triangle of the command, 1: G_TRI2's second
        bool editable;      // in the model's own list (not a called list, not with inline texture loads)
    };
    struct Model {
        std::string name;
        uint32_t record;        // its model record (container [0] table)
        uint32_t list, list_end;
        std::vector<Tri> tris;
        bool usable = true;     // its list can be copied and its records don't turn or scale it
        std::map<int, std::vector<int>> groups;     // triangles joined by shared corners
        std::vector<int> group;                     // each triangle's group's size
        std::vector<V3> at;     // the records' positions (ft)
    };

    struct Walker {
        const Bytes& d;
        uint32_t loads_begin, loads_end;
        std::vector<State>& states;
        std::map<std::vector<uint32_t>, int> interned;
        Model& m;
        uint32_t load = 0;
        std::map<uint32_t, uint32_t> modes;
        Vtx buf[64] = {};
        bool loaded[64] = {};
        bool inline_load = false;

        int state_id() {
            std::vector<uint32_t> key = { load };
            for (const auto& [w0, w1] : modes) { key.push_back(w0); key.push_back(w1); }
            auto it = interned.find(key);
            if (it != interned.end()) return it->second;
            State s;
            s.load = load;
            s.cmds.push_back({ 0xE7000000, 0 });
            for (const auto& [w0, w1] : modes) s.cmds.push_back({ w0, w1 });
            if (load) s.cmds.push_back({ 0xDE000000, load });
            int id = int(states.size());
            states.push_back(std::move(s));
            interned[key] = id;
            return id;
        }

        void tri(int a, int b, int c, uint32_t cmd, int half, int depth) {
            if (a >= 64 || b >= 64 || c >= 64 || !loaded[a] || !loaded[b] || !loaded[c]) return;
            Tri t;
            int s[3] = { a, b, c };
            for (int k = 0; k < 3; k++) { t.v[k] = buf[s[k]]; t.slot[k] = s[k]; }
            t.state = state_id();
            t.cmd = cmd;
            t.half = half;
            t.editable = depth == 0 && !inline_load;
            m.tris.push_back(t);
        }

        void run(uint32_t list, int depth) {
            if (depth > 4) return;
            for (uint32_t o = list; o + 8 <= d.size(); o += 8) {
                uint32_t w0 = u32(d, o), w1 = u32(d, o + 4);
                uint8_t op = uint8_t(w0 >> 24);
                switch (op) {
                case 0x01: {
                    int n = (w0 >> 12) & 0xFF, end = (w0 & 0xFFF) >> 1, first = end - n;
                    uint32_t at = w1 & 0xFFFFFF;
                    for (int i = 0; i < n && first + i < 64 && first + i >= 0; i++) {
                        size_t v = at + size_t(i) * 16;
                        Vtx& out = buf[first + i];
                        loaded[first + i] = true;
                        for (int k = 0; k < 3; k++) out.p[k] = s16(d, v + k * 2);
                        out.st[0] = s16(d, v + 8);
                        out.st[1] = s16(d, v + 10);
                        for (int k = 0; k < 4; k++) out.c[k] = u8(d, v + 12 + k);
                    }
                    break;
                }
                case 0x05: tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2, o, 0, depth); break;
                case 0x06:
                    tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2, o, 0, depth);
                    tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2, o, 1, depth);
                    break;
                case 0xDE: {
                    uint32_t target = w1 & 0xFFFFFF;
                    if (target >= loads_begin && target < loads_end) { load = target; inline_load = false; }
                    else run(target, depth + 1);
                    if ((w0 >> 16) & 0xFF) {        // a branch: the list ends there
                        if (depth == 0) m.usable = false;
                        return;
                    }
                    break;
                }
                case 0xDF:
                    if (depth == 0) m.list_end = o;
                    return;
                case 0xE2: case 0xE3: case 0xD9: modes[w0] = w1; break;
                case 0xFC: case 0xFA: case 0xFB: case 0xD7:
                    for (auto it = modes.begin(); it != modes.end();) {
                        if ((it->first >> 24) == op) it = modes.erase(it); else ++it;
                    }
                    modes[w0] = w1;
                    break;
                case 0xFD: case 0xF5: case 0xF3: case 0xF4: case 0xF2: case 0xF0: inline_load = true; break;
                case 0x04: case 0xE0: case 0xE1: if (depth == 0) m.usable = false; break;  // conditional / BRANCH_Z
                default: break;
                }
            }
        }
    };

    std::array<int, 3> corner_key(const V3& p) { return { int(p[0]), int(p[1]), int(p[2]) }; }

    // ---- cards ----------------------------------------------------------------------------------------------------
    struct Card {
        int model;
        int tri[2];
        V3 corner[4];           // model units: bottom left, bottom right, top right, top left (seen from the front)
        int16_t st[4][2];
        uint8_t c[4][4];
        V3 base;                // model units: the bottom edge's middle
        V3 front;               // horizontal unit cross(v1 - v0, v2 - v0) of its triangles: its culled back side
        int state;
        size_t candidate;       // its BillboardCard
    };

    // The Mtx (s15.16, integer halves then fraction halves) of rows r (row vectors: row 3 is the translation).
    void add_mtx(Bytes& g, const double r[4][4]) {
        int32_t fx[16];
        for (int i = 0; i < 16; i++) fx[i] = int32_t(std::lround(r[i / 4][i % 4] * 65536.0));
        for (int i = 0; i < 16; i++) add16(g, uint16_t(uint32_t(fx[i]) >> 16));
        for (int i = 0; i < 16; i++) add16(g, uint16_t(uint32_t(fx[i]) & 0xFFFF));
    }
}

bool rush2::calspeed::add_billboards(int t, std::vector<uint8_t>& g, const std::vector<uint8_t>& placement,
                                     BillboardResult& result, bool crops, bool any_texture) {
    char line[256];
    if (g.size() < 0x28) { result.report = "no container\n"; return false; }
    uint32_t h[10];
    for (int i = 0; i < 10; i++) h[i] = u32(g, i * 4);

    // The container's models, by name.
    std::vector<Model> models;
    std::map<std::string, int> by_name;
    for (uint32_t i = 0; i < h[4]; i++) {
        Model m;
        m.name = cstr(g, h[1] + i * 0x18, 15);
        m.record = h[0] + i * 0x34;
        m.list = u32(g, m.record + 4 + 8);
        m.list_end = 0;
        if (m.name.starts_with("SCNX")) continue;      // the extended scenery's own (calspeed_scenery.cpp)
        by_name[m.name] = int(models.size());
        models.push_back(m);
    }

    // The sections: the placement's records drawing these models, which must neither turn nor scale them.
    uint32_t first = u32(placement, 4);
    for (size_t o = first; o + 0x64 <= placement.size(); o += 0x64) {
        auto it = by_name.find(cstr(placement, o, 15));
        if (it == by_name.end()) continue;
        Model& m = models[size_t(it->second)];
        for (int k = 0; k < 9; k++) {
            if (std::fabs(f32(placement, o + 0x10 + k * 4) - (k % 4 == 0 ? 1.0f : 0.0f)) > 1e-4f) m.usable = false;
        }
        m.at.push_back({ f32(placement, o + 0x34), f32(placement, o + 0x38), f32(placement, o + 0x3C) });
    }

    std::vector<State> states;
    for (Model& m : models) {
        if (m.at.empty() || m.list == 0) continue;
        Walker w{ g, h[7], h[8], states, {}, m };
        w.run(m.list, 0);
        if (m.list_end == 0) m.usable = false;
        std::map<std::array<int, 3>, std::vector<int>> at_corner;
        for (size_t ti = 0; ti < m.tris.size(); ti++) for (int k = 0; k < 3; k++) at_corner[corner_key(m.tris[ti].v[k].p)].push_back(int(ti));
        std::vector<int> parent(m.tris.size());
        for (size_t i = 0; i < parent.size(); i++) parent[i] = int(i);
        auto find = [&](int x) { while (parent[size_t(x)] != x) x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])]; return x; };
        for (const auto& [k, list] : at_corner) for (size_t i = 1; i < list.size(); i++) parent[size_t(find(list[i]))] = find(list[0]);
        for (size_t ti = 0; ti < m.tris.size(); ti++) m.groups[find(int(ti))].push_back(int(ti));
        m.group.assign(m.tris.size(), 0);
        for (const auto& [root, list] : m.groups) for (int ti : list) m.group[size_t(ti)] = int(list.size());
    }

    // Upright triangles in the world, on a 32 ft grid, for the crossing / continuing test.
    // A draw state's texture has see-through texels (any of them, sampled every other texel).
    std::map<int, bool> clear_of;
    auto see_through = [&](int state) {
        auto it = clear_of.find(state);
        if (it != clear_of.end()) return it->second;
        bool clear = false;
        Tex tex = read_tex(g, states[size_t(state)].load, h[8]);
        for (int y = 0; tex.ok && y < tex.height && !clear; y += 2) {
            for (int x = 0; x < tex.width && !clear; x += 2) {
                float rgba[4];
                texel(g, tex, x, y, rgba);
                clear = rgba[3] < 0.5f;
            }
        }
        return clear_of[state] = clear;
    };
    struct Upright { V3 a, b; double y0, y1; int model; int tri; bool card; };
    std::vector<Upright> uprights;
    std::map<std::pair<int, int>, std::vector<size_t>> grid;
    auto cell = [](double v) { return int(std::floor(v / 32.0)); };
    for (size_t mi = 0; mi < models.size(); mi++) {
        const Model& m = models[mi];
        for (const V3& at : m.at) {
            for (size_t ti = 0; ti < m.tris.size(); ti++) {
                const Tri& t = m.tris[ti];
                V3 p[3];
                for (int k = 0; k < 3; k++) for (int a = 0; a < 3; a++) p[k][a] = at[a] + t.v[k].p[a] / 16.0;
                V3 n = cross(sub(p[1], p[0]), sub(p[2], p[0]));
                double l = len(n);
                if (l < 1e-9 || std::fabs(n[1] / l) > 0.3) continue;
                // Its footprint: the farthest apart pair of corners in x / z.
                Upright u{ p[0], p[1], std::min({ p[0][1], p[1][1], p[2][1] }), std::max({ p[0][1], p[1][1], p[2][1] }), int(mi), int(ti), m.group[ti] == 2 };
                double best = -1;
                for (int i = 0; i < 3; i++) for (int j = i + 1; j < 3; j++) {
                    double dd = std::hypot(p[i][0] - p[j][0], p[i][2] - p[j][2]);
                    if (dd > best) { best = dd; u.a = p[i]; u.b = p[j]; }
                }
                size_t idx = uprights.size();
                uprights.push_back(u);
                int x0 = cell(std::min(u.a[0], u.b[0]) - touch), x1 = cell(std::max(u.a[0], u.b[0]) + touch);
                int z0 = cell(std::min(u.a[2], u.b[2]) - touch), z1 = cell(std::max(u.a[2], u.b[2]) + touch);
                for (int x = x0; x <= x1; x++) for (int z = z0; z <= z1; z++) grid[{ x, z }].push_back(idx);
            }
        }
    }

    // Lone quads: triangle groups joined by shared corners, two triangles with four corners.
    std::vector<Card> cards;
    for (size_t mi = 0; mi < models.size(); mi++) {
        Model& m = models[mi];
        if (m.at.empty()) continue;
        for (const auto& [root, list] : m.groups) {
            if (list.size() != 2) continue;
            const Tri& t0 = m.tris[size_t(list[0])];
            const Tri& t1 = m.tris[size_t(list[1])];
            std::vector<Vtx> corners;
            for (const Tri* t : { &t0, &t1 }) for (const Vtx& v : t->v) {
                bool seen = false;
                for (const Vtx& c : corners) seen |= corner_key(c.p) == corner_key(v.p);
                if (!seen) corners.push_back(v);
            }
            if (corners.size() != 4) continue;
            V3 n = cross(sub(t0.v[1].p, t0.v[0].p), sub(t0.v[2].p, t0.v[0].p));
            V3 n1 = cross(sub(t1.v[1].p, t1.v[0].p), sub(t1.v[2].p, t1.v[0].p));
            double ln = len(n), ln1 = len(n1);
            if (ln < 1e-9 || ln1 < 1e-9 || dot(n, n1) / (ln * ln1) < 0.98) continue;     // folded
            if (std::fabs(n[1] / ln) > max_tilt) continue;                                 // not upright
            const Tex tex = t0.state >= 0 ? read_tex(g, states[size_t(t0.state)].load, h[8]) : Tex{};
            if (!tex.ok) continue;

            Card c;
            c.model = int(mi);
            c.tri[0] = list[0];
            c.tri[1] = list[1];
            c.state = t0.state;
            c.front = { n[0], 0, n[2] };
            double lf = len(c.front);
            c.front = { c.front[0] / lf, 0, c.front[2] / lf };
            // Seen from the front, right is front x up (Rush 2's world turns x the other way, as glTF's x mirrors it:
            // the order only names the corners, the drawing keeps the triangles' own winding).
            V3 right = { -c.front[2], 0, c.front[0] };
            std::sort(corners.begin(), corners.end(), [](const Vtx& a, const Vtx& b) { return a.p[1] < b.p[1]; });
            Vtx bottom[2] = { corners[0], corners[1] }, top[2] = { corners[2], corners[3] };
            if (dot(bottom[0].p, right) > dot(bottom[1].p, right)) std::swap(bottom[0], bottom[1]);
            if (dot(top[0].p, right) > dot(top[1].p, right)) std::swap(top[0], top[1]);
            const Vtx* ordered[4] = { &bottom[0], &bottom[1], &top[1], &top[0] };
            for (int k = 0; k < 4; k++) {
                c.corner[k] = ordered[k]->p;
                c.st[k][0] = ordered[k]->st[0];
                c.st[k][1] = ordered[k]->st[1];
                memcpy(c.c[k], ordered[k]->c, 4);
            }
            for (int a = 0; a < 3; a++) c.base[a] = (c.corner[0][a] + c.corner[1][a]) / 2;
            double height = ((c.corner[2][1] + c.corner[3][1]) - (c.corner[0][1] + c.corner[1][1])) / 2 / 16.0;
            double width = std::max(std::hypot(c.corner[1][0] - c.corner[0][0], c.corner[1][2] - c.corner[0][2]),
                                    std::hypot(c.corner[2][0] - c.corner[3][0], c.corner[2][2] - c.corner[3][2])) / 16.0;

            BillboardCard bc;
            bc.model = m.name;
            bc.texture = tex.name;
            for (int a = 0; a < 3; a++) bc.base[a] = m.at[0][a] + c.base[a] / 16.0;
            bc.width = width;
            bc.height = height;

            // What it shows: its texels sampled across it (front view, upright).
            int ch = 48, cw = std::clamp(int(std::lround(48 * width / std::max(height, 0.1))), 4, 160);
            int solid = 0, green = 0, clear = 0;
            if (crops) bc.crop.assign(size_t(cw * ch * 4), 0);
            for (int y = 0; y < ch; y++) {
                for (int x = 0; x < cw; x++) {
                    double u = (x + 0.5) / cw, v = 1.0 - (y + 0.5) / ch;    // v up
                    double st[2];
                    for (int a = 0; a < 2; a++) {
                        double b = c.st[0][a] + (c.st[1][a] - c.st[0][a]) * u, t = c.st[3][a] + (c.st[2][a] - c.st[3][a]) * u;
                        st[a] = (b + (t - b) * v) / 32.0;
                    }
                    float rgba[4];
                    texel(g, tex, int(std::floor(st[0])), int(std::floor(st[1])), rgba);
                    if (rgba[3] < 0.5f) clear++;
                    else {
                        solid++;
                        if (rgba[1] > rgba[0] * 0.95f && rgba[1] > rgba[2] * 1.05f) green++;
                    }
                    if (crops) for (int k = 0; k < 4; k++) bc.crop[size_t((y * cw + x) * 4 + k)] = uint8_t(std::lround(rgba[k] * 255));
                }
            }
            if (crops) { bc.crop_w = cw; bc.crop_h = ch; }
            bc.clear = float(clear) / float(cw * ch);
            bc.green = solid ? float(green) / float(solid) : 0.0f;

            // Why it stays flat, first reason found.
            auto level = [&](int a, int b) { return std::fabs(c.corner[a][1] - c.corner[b][1]) / 16.0 <= square * height; };
            if (!t0.editable || !t1.editable) bc.why = "in a called list";
            else if (!m.usable) bc.why = "section can't be copied or is turned";
            else if (t0.state != t1.state) bc.why = "two draw states";
            else if (height < min_height) bc.why = "short";
            else if (!level(0, 1) || !level(2, 3)) bc.why = "not an upright rectangle";
            else if (width > max_aspect * height) bc.why = "wide";
            else if (bc.clear < min_clear) bc.why = "solid";
            else if (!any_texture && !tree_texture(t, tex.name)) bc.why = "not a tree";
            else {
                // Another upright surface crossing it, or carrying on from one of its edges (a row, a wall, a post).
                const V3& a = c.corner[0];
                const V3& b = c.corner[1];
                V3 wa = { m.at[0][0] + a[0] / 16.0, 0, m.at[0][2] + a[2] / 16.0 }, wb = { m.at[0][0] + b[0] / 16.0, 0, m.at[0][2] + b[2] / 16.0 };
                double y0 = m.at[0][1] + c.corner[0][1] / 16.0, y1 = y0 + height;
                auto seg_dist = [](const V3& p, const V3& q, const V3& r) {      // point r to segment pq, in x / z
                    double dx = q[0] - p[0], dz = q[2] - p[2], l2 = dx * dx + dz * dz;
                    double t = l2 > 0 ? std::clamp(((r[0] - p[0]) * dx + (r[2] - p[2]) * dz) / l2, 0.0, 1.0) : 0.0;
                    return std::hypot(p[0] + dx * t - r[0], p[2] + dz * t - r[2]);
                };
                auto crosses = [](const V3& p, const V3& q, const V3& r, const V3& s) {
                    auto side = [](const V3& o, const V3& e, const V3& x) { return (e[0] - o[0]) * (x[2] - o[2]) - (e[2] - o[2]) * (x[0] - o[0]); };
                    double d1 = side(p, q, r), d2 = side(p, q, s), d3 = side(r, s, p), d4 = side(r, s, q);
                    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
                };
                std::set<size_t> near;
                for (int x = cell(std::min(wa[0], wb[0]) - touch); x <= cell(std::max(wa[0], wb[0]) + touch); x++) {
                    for (int z = cell(std::min(wa[2], wb[2]) - touch); z <= cell(std::max(wa[2], wb[2]) + touch); z++) {
                        auto it = grid.find({ x, z });
                        if (it != grid.end()) near.insert(it->second.begin(), it->second.end());
                    }
                }
                for (size_t k : near) {
                    const Upright& u = uprights[k];
                    if (u.model == int(mi) && (u.tri == c.tri[0] || u.tri == c.tri[1])) continue;
                    double overlap = std::min(u.y1, y1) - std::max(u.y0, y0);
                    if (overlap < 0.2 * height) continue;
                    // A see-through surface through a card is a tree wall or hedge behind the tree: turning the card
                    // only moves foliage through foliage. Through a solid one (a slope, a wall) it would show.
                    // A card touching it makes a row (a wall of cards).
                    const Tri& x = models[size_t(u.model)].tris[size_t(u.tri)];
                    if (!u.card && see_through(x.state)) continue;
                    if (crosses(wa, wb, u.a, u.b)) { bc.why = u.card ? "crossed by a card" : "crossed by a solid surface"; break; }
                    if (seg_dist(wa, wb, u.a) < touch || seg_dist(wa, wb, u.b) < touch ||
                        seg_dist(u.a, u.b, wa) < touch || seg_dist(u.a, u.b, wb) < touch) {
                        bc.why = u.card ? "joined to a card" : "joined to a solid surface";
                        break;
                    }
                }
            }
            bc.chosen = bc.why.empty();
            c.candidate = result.candidates.size();
            result.candidates.push_back(std::move(bc));
            if (result.candidates.back().chosen) cards.push_back(c);
        }
    }

    // ---- writing ----------------------------------------------------------------------------------------------
    if (cards.empty()) {
        snprintf(line, sizeof(line), "%zu candidate cards, none chosen\n", result.candidates.size());
        result.report = line;
        return false;
    }
    std::map<int, std::vector<const Card*>> by_model;
    for (const Card& c : cards) by_model[c.model].push_back(&c);

    // The turn list: the one per-view matrix, read through the segment.
    align8(g);
    uint32_t turn = uint32_t(g.size());
    add32(g, 0xDA380001); add32(g, rush2::calspeed::billboard_segment << 24);    // G_MTX modelview, mul, nopush
    add32(g, 0xDF000000); add32(g, 0);

    for (const auto& [mi, list] : by_model) {
        Model& m = models[size_t(mi)];
        // Card vertices (around the base, in the card's frame: x across, y up, the visible side toward -z) and base matrices.
        std::vector<uint32_t> vert_at, mtx_at;
        for (const Card* c : list) {
            V3 z = { c->front[0], 0, c->front[2] };     // local -z: the visible side, away from `front`
            V3 x = { z[2], 0, -z[0] };      // with y up and z this is a turn about y (view_billboard_basis's form)
            align8(g);
            mtx_at.push_back(uint32_t(g.size()));
            double r[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { c->base[0], c->base[1], c->base[2], 1 } };
            add_mtx(g, r);
            vert_at.push_back(uint32_t(g.size()));
            for (int k = 0; k < 4; k++) {
                V3 d = sub(c->corner[k], c->base);
                double l[3] = { dot(d, x), d[1], dot(d, z) };
                for (double v : l) add16(g, uint16_t(int16_t(std::clamp<long>(std::lround(v), -32767, 32767))));
                add16(g, 0);            // no vertex chain, no mirrored width (mirror_vertex_chain_80077810)
                add16(g, uint16_t(c->st[k][0]));
                add16(g, uint16_t(c->st[k][1]));
                for (int a = 0; a < 4; a++) g.push_back(c->c[k][a]);
            }
        }

        // The section's list without the cards' triangles.
        Bytes body(g.begin() + m.list, g.begin() + m.list_end);
        std::map<uint32_t, int> dropped;     // command offset in body -> halves taken out (bit 0 first, bit 1 second)
        for (const Card* c : list) {
            for (int i = 0; i < 2; i++) {
                const Tri& t = m.tris[size_t(c->tri[i])];
                dropped[t.cmd - m.list] |= 1 << t.half;
            }
        }
        for (const auto& [o, halves] : dropped) {
            uint32_t w0 = u32(body, o), w1 = u32(body, o + 4);
            bool tri2 = (w0 >> 24) == 0x06;
            if (!tri2 || halves == 3) { put32(body, o, 0); put32(body, o + 4, 0); continue; }   // G_NOOP
            put32(body, o, 0x05000000 | ((halves == 1 ? w1 : w0) & 0xFFFFFF));                // G_TRI1 of the other half
            put32(body, o + 4, 0);
        }

        align8(g);
        uint32_t new_list = uint32_t(g.size());
        g.insert(g.end(), body.begin(), body.end());
        std::map<int, std::vector<size_t>> by_state;
        for (size_t i = 0; i < list.size(); i++) by_state[list[i]->state].push_back(i);
        for (const auto& [state, which] : by_state) {
            for (const Cmd& cmd : states[size_t(state)].cmds) { add32(g, cmd.w0); add32(g, cmd.w1); }
            for (size_t i : which) {
                const Card* c = list[i];
                add32(g, 0xDA380000); add32(g, mtx_at[i]);                  // G_MTX modelview, mul, push: T(base)
                add32(g, 0xDE000000); add32(g, turn);                       // the view's turn
                add32(g, 0x01004008); add32(g, vert_at[i]);                 // G_VTX 4 into slots 0-3
                // The two triangles with their own winding, on the new slots.
                uint32_t idx[2] = { 0, 0 };
                for (int k = 0; k < 2; k++) {
                    const Tri& t = m.tris[size_t(c->tri[k])];
                    uint32_t w = 0;
                    for (int j = 0; j < 3; j++) {
                        int slot = 0;
                        for (int q = 0; q < 4; q++) if (c->corner[q] == t.v[j].p) slot = q;
                        w |= uint32_t(slot * 2) << (16 - 8 * j);
                    }
                    idx[k] = w;
                }
                add32(g, 0x06000000 | idx[0]); add32(g, idx[1]);
                add32(g, 0xD8380002); add32(g, 0x40);                       // G_POPMTX
            }
        }
        add32(g, 0xE3001001); add32(g, 0x00008000);     // the TLUT mode Rush 2's lists leave set
        add32(g, 0xDF000000); add32(g, 0);
        put32(g, m.record + 4 + 8, new_list);
        result.models++;
    }
    result.cards = int(cards.size());

    std::map<std::string, int> why;
    for (const BillboardCard& c : result.candidates) if (!c.chosen) why[c.why]++;
    snprintf(line, sizeof(line), "%d cards turned to the camera in %d sections; %zu left flat:", result.cards, result.models,
             result.candidates.size() - cards.size());
    result.report = line;
    for (const auto& [w, n] : why) { snprintf(line, sizeof(line), " %s %d,", w.c_str(), n); result.report += line; }
    if (result.report.back() == ',') result.report.pop_back();
    result.report += "\n";
    return true;
}
