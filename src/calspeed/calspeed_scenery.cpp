// Extended scenery for California Speed's tracks (include/calspeed_scenery.h, docs/calspeed_research.md#extended-scenery).
//
// The track's own section models (the placement records drawing models of its container, asset 62 + t) are read back
// into world triangles by running their display lists: G_VTX / G_TRI1 / G_TRI2, and the state each triangle is drawn
// with, which California Speed's lists set with only a call into a texture-load list (G_DL into the container's
// [7]..[8] range), G_SETOTHERMODE_L / _H and G_SETCOMBINE. New models replay those same commands, so they draw with the
// track's own textures and render modes; they are appended to the container, which the merge then copies whole
// (track2049_convert.cpp merge_models moves every pointer that lands in it).
//
// Behind the start (point-to-point tracks): the triangles crossing a plane across the road just inside its back end
// give the cross-section (each piece with its texture coordinates and color at the plane, and the rate its texture
// coordinates change at going back along the road). It is swept back along a path that goes straight on until
// everything California Speed built behind the road has ended (there the sweep lies over it with the same texture
// mapping), then bends away and rises over a crest and drops, per track (`starts`), so its end is out of sight from
// the start line. Tree walls that stop just short of the cut go on with it, tree cards are set down along it, and
// Mojave's canyon walls, taken from where they stand ahead, rise out of the ground along it. Monterey's left side and
// Mount Shasta's right come from a plane ahead where they run on further; Central Valley's right tree wall and
// Mojave's right slope past the 260 ft reach are swept too; Los Angeles' angled overpass decks go on along their own
// heading and its left edge turns out from where the stock one ends. Road-closed barricades stand across each carriageway on the straight part (Highway 1's on the stock
// road before its crest). All of it in reach has collision: every piece of the cross-section along the
// whole sweep, walls on its open edges, and invisible walls across it at the barricades and at its end. The sea's
// near tiles are Rush 2's water hazard (type 3).
//
// The sea: on the tracks listed in `seas`, the water (triangles whose texture is mostly blue and that face up) is
// carried out to the horizon as a plane 1.5 ft under the track's water level, in 256 ft cells, leaving out every cell
// where the track has geometry below the water (tunnels, the shore). Near cells are 3,840 ft tiles (the 1/16 ft s16
// vertices reach 2,047 ft from their record), far ones 15,360 ft tiles drawn at 4 times scale.
//
// Collision: new polygons go after the file's own, and every quadtree quadrant they overlap gets them in its leaf: a
// copy of its leaf with them added, or, past 36 polygons (the queries' u16[36] stack buffers), a new node splitting the
// quadrant until each leaf fits. Those leaves go after the vertex lists, out of the leaf section's 16-bit reach; a node
// whose mask has bit 4 + q set takes quadrant q's leaf from the far-leaf table, by the hooks of
// src/rush2049/track2049_collision.cpp on a file tagged with scenery_collision_tag. Rush 2's own walk is kept
// otherwise, so California Speed's lookups are unchanged. The root's box grows (by as much on both sides, keeping its
// splits) to hold the new polygons: Rush 2 gives a point outside it no collision.

#include "calspeed_scenery.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace {
    using Bytes = std::vector<uint8_t>;
    using V3 = std::array<double, 3>;

    V3 add(const V3& a, const V3& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
    V3 sub(const V3& a, const V3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    V3 mul(const V3& a, double s) { return { a[0] * s, a[1] * s, a[2] * s }; }
    double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    V3 cross(const V3& a, const V3& b) {
        return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    }
    double len(const V3& a) { return std::sqrt(dot(a, a)); }
    V3 norm(const V3& a) { double l = len(a); return l > 1e-12 ? mul(a, 1.0 / l) : V3{ 0, 0, 0 }; }

    uint32_t u32(const Bytes& d, size_t o) {
        return o + 4 <= d.size() ? (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3] : 0;
    }
    uint16_t u16(const Bytes& d, size_t o) { return o + 2 <= d.size() ? uint16_t((d[o] << 8) | d[o + 1]) : 0; }
    int16_t s16(const Bytes& d, size_t o) { return int16_t(u16(d, o)); }
    float f32(const Bytes& d, size_t o) { uint32_t w = u32(d, o); float f; memcpy(&f, &w, 4); return f; }
    void put32(Bytes& d, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) d[o + i] = uint8_t(v >> (24 - 8 * i)); }
    void put16(Bytes& d, size_t o, uint16_t v) { d[o] = uint8_t(v >> 8); d[o + 1] = uint8_t(v); }
    void putf(Bytes& d, size_t o, float f) { uint32_t w; memcpy(&w, &f, 4); put32(d, o, w); }
    void add32(Bytes& d, uint32_t v) { for (int i = 0; i < 4; i++) d.push_back(uint8_t(v >> (24 - 8 * i))); }
    void add16(Bytes& d, uint16_t v) { d.push_back(uint8_t(v >> 8)); d.push_back(uint8_t(v)); }
    void align(Bytes& d, size_t a) { while (d.size() % a) d.push_back(0); }
    std::string cstr(const Bytes& d, size_t o, size_t n) {
        std::string s;
        for (size_t i = 0; i < n && o + i < d.size() && d[o + i]; i++) s += char(d[o + i]);
        return s;
    }

    struct Cmd { uint32_t w0, w1; };

    // ---- per-track settings -----------------------------------------------------------------------------------
    // How the road goes on behind the start. turn: degrees, + toward the left of a car facing back down the road
    // (its right on the start line); rise: height offsets (ft) along the sweep after the straight part, as (distance
    // past the bend's start, offset) pairs; the sweep ends at the last one.
    struct Start {
        bool on = false;
        double turn = 0, radius = 600;
        std::vector<std::pair<double, double>> rise;
        double reach = 260;         // ft across the road (each side) the cross-section is taken over
        double barricade = 45;      // ft behind the road's end the barricades stand
        bool fixed = false;         // take `turn` as it is (else the best of a few turns either way)
    };

    // Per track (California Speed's order): point-to-point tracks only.
    Start start_for(int t) {
        Start s;
        s.on = t >= 0 && t < 16 && t != 7 && t != 12 && t != 13 && t != 14 && t != 15;
        s.turn = 70;
        s.radius = 600;
        s.rise = { { 0, 0 }, { 250, 10 }, { 520, -40 }, { 900, -110 } };
        return s;
    }

    // Tracks whose water is the sea, carried to the horizon.
    bool sea_for(int t) {
        return t == 2 || t == 3 || t == 5 || t == 6;
    }

    // ---- reading the track's models -------------------------------------------------------------------------------
    struct Vtx {
        V3 p;
        double st[2];       // s10.5 texture coordinates as stored
        uint8_t c[4];
    };
    struct Tri {
        Vtx v[3];
        int state;
    };
    // A draw state: the commands that set it, replayed before its triangles.
    struct State {
        std::vector<Cmd> cmds;
        uint32_t load = 0;              // its texture-load list
        int width = 0, height = 0;      // render tile size (texels)
        double period[2] = { 0, 0 };    // texture repeat per axis in s10.5 units, 0 = clamped
        float color[3] = { 0, 0, 0 };   // mean texel color (0-1)
        bool blue = false;
    };

    struct Container {
        const Bytes& d;
        uint32_t h[10];
        std::map<std::string, uint32_t> lists;  // model name -> LOD 0 list
        explicit Container(const Bytes& data) : d(data) {
            for (int i = 0; i < 10; i++) h[i] = u32(d, i * 4);
            for (uint32_t i = 0; i < h[4]; i++) {
                lists[cstr(d, h[1] + i * 0x18, 15)] = u32(d, h[0] + i * 0x34 + 4 + 8);
            }
        }
        bool load_list(uint32_t a) const { return a >= h[7] && a < h[8]; }
    };

    // RGBA of an N64 texel source (fmt, siz) at index i of `texels`, palette entries RGBA5551.
    void texel_rgb(const Bytes& d, uint32_t texels, uint32_t palette, int fmt, int siz, uint32_t i, float out[3]) {
        auto c16 = [&](uint16_t v) {
            out[0] = ((v >> 11) & 31) / 31.0f; out[1] = ((v >> 6) & 31) / 31.0f; out[2] = ((v >> 1) & 31) / 31.0f;
        };
        auto gray = [&](float g) { out[0] = out[1] = out[2] = g; };
        if (fmt == 0 && siz == 2) { c16(u16(d, texels + i * 2)); return; }
        if (fmt == 0 && siz == 3) {
            uint32_t v = u32(d, texels + i * 4);
            out[0] = (v >> 24) / 255.0f; out[1] = ((v >> 16) & 255) / 255.0f; out[2] = ((v >> 8) & 255) / 255.0f;
            return;
        }
        if (fmt == 2) {
            uint32_t index = siz == 0 ? ((texels + i / 2 < d.size() ? d[texels + i / 2] : 0) >> ((i & 1) ? 0 : 4)) & 15
                                      : (texels + i < d.size() ? d[texels + i] : 0);
            c16(u16(d, palette + index * 2));
            return;
        }
        if (fmt == 4) {
            if (siz == 0) gray((((texels + i / 2 < d.size() ? d[texels + i / 2] : 0) >> ((i & 1) ? 0 : 4)) & 15) / 15.0f);
            else gray((texels + i < d.size() ? d[texels + i] : 0) / 255.0f);
            return;
        }
        if (fmt == 3) {
            if (siz == 1) gray(((texels + i < d.size() ? d[texels + i] : 0) >> 4) / 15.0f);
            else if (siz == 2) gray((u16(d, texels + i * 2) >> 8) / 255.0f);
            else gray((((texels + i / 2 < d.size() ? d[texels + i / 2] : 0) >> ((i & 1) ? 0 : 4)) >> 1) / 7.0f);
            return;
        }
        gray(0.5f);
    }

    // What a texture-load list loads: render tile size and wrap, and the mean color of its texels.
    void read_load(const Container& c, uint32_t list, State& s) {
        uint32_t timg = 0, texels = 0, palette = 0;
        int fmt = 0, siz = 2;
        for (uint32_t o = list; o + 8 <= c.h[8] && o < list + 8 * 64; o += 8) {
            uint32_t w0 = u32(c.d, o), w1 = u32(c.d, o + 4);
            uint8_t op = uint8_t(w0 >> 24);
            if (op == 0xFD) timg = w1 & 0xFFFFFF;
            else if (op == 0xF0) palette = timg;
            else if (op == 0xF3 || op == 0xF4) texels = timg;
            else if (op == 0xF5 && ((w1 >> 24) & 7) == 0) {
                fmt = (w0 >> 21) & 7;
                siz = (w0 >> 19) & 3;
                int cm[2] = { int((w1 >> 8) & 3), int((w1 >> 18) & 3) };
                int mask[2] = { int((w1 >> 4) & 15), int((w1 >> 14) & 15) };
                for (int a = 0; a < 2; a++) {
                    s.period[a] = (cm[a] & 2) ? 0 : (mask[a] ? double(1 << mask[a]) : -1) * ((cm[a] & 1) ? 2 : 1);
                }
            }
            else if (op == 0xF2 && ((w1 >> 24) & 7) == 0) {
                s.width = int(((w1 >> 12) & 0xFFF) / 4 + 1);
                s.height = int((w1 & 0xFFF) / 4 + 1);
            }
            else if (op == 0xDF) break;
        }
        for (int a = 0; a < 2; a++) {
            if (s.period[a] < 0) s.period[a] = a == 0 ? s.width : s.height;
            s.period[a] *= 32.0;
        }
        if (s.width <= 0 || s.height <= 0 || texels == 0) return;
        double sum[3] = { 0, 0, 0 };
        int n = 0;
        for (int y = 0; y < s.height; y += 2) {
            for (int x = 0; x < s.width; x += 2) {
                float rgb[3];
                texel_rgb(c.d, texels, palette, fmt, siz, uint32_t(y * s.width + x), rgb);
                for (int k = 0; k < 3; k++) sum[k] += rgb[k];
                n++;
            }
        }
        for (int k = 0; k < 3; k++) s.color[k] = float(sum[k] / n);
        s.blue = s.color[2] > 0.3f && s.color[2] > s.color[0] * 1.5f && s.color[2] > s.color[1] * 1.15f;
    }

    struct Scene {
        std::vector<Tri> tris;
        std::vector<State> states;
    };

    // Runs a model's list (and the plain lists it calls) collecting triangles in the world.
    struct Walker {
        const Container& c;
        Scene& scene;
        std::map<std::vector<uint32_t>, int> interned;
        // Current state: the texture-load list and the other mode / combine words by their first word.
        uint32_t load = 0;
        std::map<uint32_t, uint32_t> modes;
        Vtx buf[64] = {};
        bool loaded[64] = {};
        double m[9] = {}, pos[3] = {};
        bool unsupported = false;   // inline texture loads: triangles skipped

        int state_id() {
            std::vector<uint32_t> key = { load };
            for (const auto& [w0, w1] : modes) { key.push_back(w0); key.push_back(w1); }
            auto it = interned.find(key);
            if (it != interned.end()) return it->second;
            State s;
            s.load = load;
            s.cmds.push_back({ 0xE7000000, 0 });
            for (const auto& [w0, w1] : modes) s.cmds.push_back({ w0, w1 });
            if (load) {
                s.cmds.push_back({ 0xDE000000, load });
                read_load(c, load, s);
            }
            int id = int(scene.states.size());
            scene.states.push_back(std::move(s));
            interned[key] = id;
            return id;
        }

        void tri(int a, int b, int d) {
            if (unsupported || a >= 64 || b >= 64 || d >= 64 || !loaded[a] || !loaded[b] || !loaded[d]) return;
            Tri t;
            t.v[0] = buf[a]; t.v[1] = buf[b]; t.v[2] = buf[d];
            t.state = state_id();
            scene.tris.push_back(t);
        }

        void run(uint32_t list, int depth) {
            if (depth > 4) return;
            for (uint32_t o = list; o + 8 <= c.d.size(); o += 8) {
                uint32_t w0 = u32(c.d, o), w1 = u32(c.d, o + 4);
                uint8_t op = uint8_t(w0 >> 24);
                switch (op) {
                case 0x01: {
                    int n = (w0 >> 12) & 0xFF, end = (w0 & 0xFFF) >> 1, first = end - n;
                    uint32_t at = w1 & 0xFFFFFF;
                    for (int i = 0; i < n && first + i < 64 && first + i >= 0; i++) {
                        size_t v = at + size_t(i) * 16;
                        double l[3] = { double(s16(c.d, v)), double(s16(c.d, v + 2)), double(s16(c.d, v + 4)) };
                        Vtx& out = buf[first + i];
                        loaded[first + i] = true;
                        for (int k = 0; k < 3; k++) {
                            out.p[k] = pos[k] + (l[0] * m[k] + l[1] * m[3 + k] + l[2] * m[6 + k]) / 16.0;
                        }
                        out.st[0] = s16(c.d, v + 8);
                        out.st[1] = s16(c.d, v + 10);
                        for (int k = 0; k < 4; k++) out.c[k] = v + 12 + k < c.d.size() ? c.d[v + 12 + k] : 255;
                    }
                    break;
                }
                case 0x05: tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2); break;
                case 0x06:
                    tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                    tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                    break;
                case 0xDE: {
                    uint32_t target = w1 & 0xFFFFFF;
                    if (c.load_list(target)) { load = target; unsupported = false; }
                    else run(target, depth + 1);
                    if ((w0 >> 16) & 0xFF) return;  // branch: no return here
                    break;
                }
                case 0xDF: return;
                case 0xE2: case 0xE3: case 0xD9: modes[w0] = w1; break;
                case 0xFC: case 0xFA: case 0xFB: case 0xD7:
                    // One combine / color at a time: drop others of the same opcode.
                    for (auto it = modes.begin(); it != modes.end();) {
                        if ((it->first >> 24) == op) it = modes.erase(it); else ++it;
                    }
                    modes[w0] = w1;
                    break;
                case 0xFD: case 0xF5: case 0xF3: case 0xF4: case 0xF2: case 0xF0: unsupported = true; break;
                default: break;
                }
            }
        }
    };

    struct Record {
        std::string name;
        double m[9], pos[3];
        int16_t next, child;
    };

    // Placement file: u32 count, count x {u32 offset, name[16]}, then 0x64-byte records to the end.
    std::vector<Record> read_records(const Bytes& p, uint32_t& first) {
        std::vector<Record> out;
        first = u32(p, 4);
        for (size_t o = first; o + 0x64 <= p.size(); o += 0x64) {
            Record r;
            r.name = cstr(p, o, 16);
            for (int k = 0; k < 9; k++) r.m[k] = f32(p, o + 0x10 + k * 4);
            for (int k = 0; k < 3; k++) r.pos[k] = f32(p, o + 0x34 + k * 4);
            r.next = s16(p, o + 0x44);
            r.child = s16(p, o + 0x46);
            out.push_back(r);
        }
        return out;
    }

    Scene read_scene(const Container& c, const Bytes& placement) {
        Scene scene;
        Walker w{ c, scene };
        uint32_t first;
        for (const Record& r : read_records(placement, first)) {
            auto it = c.lists.find(r.name.substr(0, 15));
            if (it == c.lists.end() || it->second == 0) continue;
            memcpy(w.m, r.m, sizeof(w.m));
            memcpy(w.pos, r.pos, sizeof(w.pos));
            w.load = 0;
            w.modes.clear();
            w.unsupported = false;
            for (bool& l : w.loaded) l = false;
            w.run(it->second, 0);
        }
        return scene;
    }

    // ---- collision file ---------------------------------------------------------------------------------------
    struct Collision {
        Bytes& d;
        uint32_t segs, nodes, polys, verts, leaf_bytes, vlist_bytes;
        size_t node0, poly0, vert0, leaf0, vlist0;
        explicit Collision(Bytes& data) : d(data) {
            segs = u16(d, 0); nodes = u16(d, 2); polys = u16(d, 4); verts = u16(d, 6);
            leaf_bytes = u16(d, 8); vlist_bytes = u16(d, 10);
            node0 = 12 + size_t(segs) * 0x84;
            poly0 = node0 + size_t(nodes) * 0x14;
            vert0 = poly0 + size_t(polys) * 0x18;
            leaf0 = vert0 + size_t(verts) * 8;
            vlist0 = leaf0 + leaf_bytes;
        }
        bool valid() const { return vlist0 + vlist_bytes <= d.size() && nodes > 0; }
        V3 vertex(uint32_t k) const {
            size_t o = vert0 + size_t(k) * 8;
            uint16_t frac = u16(d, o + 6);
            const int shift[3] = { 10, 5, 0 };
            V3 v;
            for (int a = 0; a < 3; a++) v[a] = (s16(d, o + a * 2) * 32 + ((frac >> shift[a]) & 31)) / 32.0;
            return v;
        }
        // Polygon i's world vertices.
        std::vector<V3> polygon(uint32_t i, uint16_t* flags = nullptr, uint16_t* info = nullptr) const {
            size_t p = poly0 + size_t(i) * 0x18;
            if (flags) *flags = u16(d, p);
            if (info) *info = u16(d, p + 2);
            int count = u16(d, p + 2) & 0xF;
            double m[9];
            for (int k = 0; k < 9; k++) m[k] = s16(d, p + 4 + k * 2) / 16384.0;
            std::vector<V3> w;
            size_t at = vlist0 + u16(d, p + 0x16);
            while (count > 0 && at + 2 <= d.size()) {
                uint32_t v = u16(d, at);
                at += 2;
                int run = 0;
                if (count >= 2 && at < d.size() && d[at] >= 0xC0) run = d[at++] & 0x3F;
                count -= run + 1;
                for (int k = 0; k <= run; k++) {
                    V3 l = vertex(v + k);
                    if (w.empty()) w.push_back(l);
                    else w.push_back({ w[0][0] + m[0] * l[0] + m[3] * l[1] + m[6] * l[2],
                                       w[0][1] + m[1] * l[0] + m[4] * l[1] + m[7] * l[2],
                                       w[0][2] + m[2] * l[0] + m[5] * l[1] + m[8] * l[2] });
                }
            }
            return w;
        }
    };

    // The highest polygon of types 0 to `max_type` (floors: 2) under (x, z) within `above` ft over y and 30 under;
    // flags/info of it.
    bool ground_at(const Collision& col, double x, double y, double z, double above, uint16_t& flags, uint16_t& info, double& height,
                   int max_type = 2) {
        bool found = false;
        for (uint32_t i = 0; i < col.polys; i++) {
            uint16_t f, in;
            size_t p = col.poly0 + size_t(i) * 0x18;
            if ((col.d[p + 1] & 0xF) > max_type) continue;
            std::vector<V3> w = col.polygon(i, &f, &in);
            if (w.size() < 3) continue;
            // Inside the x/z outline (convex, either winding)?
            int sign = 0;
            bool inside = true;
            for (size_t k = 0; k < w.size() && inside; k++) {
                const V3& a = w[k];
                const V3& b = w[(k + 1) % w.size()];
                double c = (b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0]);
                if (std::fabs(c) < 1e-9) continue;
                int s = c > 0 ? 1 : -1;
                if (sign == 0) sign = s;
                else if (s != sign) inside = false;
            }
            if (!inside) continue;
            V3 n = norm(cross(sub(w[1], w[0]), sub(w[2], w[0])));
            if (std::fabs(n[1]) < 0.2) continue;
            double h = w[0][1] - (n[0] * (x - w[0][0]) + n[2] * (z - w[0][2])) / n[1];
            if (h > y + above || h < y - 30) continue;
            if (!found || h > height) {
                found = true;
                height = h;
                flags = f;
                info = in;
            }
        }
        return found;
    }

    // ---- new geometry -------------------------------------------------------------------------------------------
    struct OutVtx {
        V3 p;
        int32_t st[2];
        uint8_t c[4];
    };
    struct OutQuad {    // a triangle when v[3] repeats v[2] (count 3)
        OutVtx v[4];
        int count = 4;
        int state;      // Scene state, or own_* below
        bool bare = false;  // out of reach on purpose: no collision under it
    };
    constexpr int own_stripes = -1, own_sign = -2;

    struct NewModel {
        std::string name;
        std::vector<OutQuad> quads;
        V3 origin{};
        double scale = 1.0;     // drawn at this scale (record matrix); vertices are (p - origin) / scale at 1/16 ft
    };

    struct NewPoly {
        std::vector<V3> v;      // convex, planar, any winding
        uint16_t flags, info_hi;
        V3 front;               // the side the polygon faces (its normal is turned to it)
    };

    // Own textures: the barricade boards' orange and white stripes over a white block (32 x 16 RGBA16), and the
    // ROAD CLOSED sign (128 x 32 IA8: 4-bit intensity, alpha full; in I8 the intensity is the alpha too, and the dark
    // letters went see-through wherever a track's render mode takes texture alpha).
    Bytes stripe_texels() {
        Bytes b;
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 32; x++) {
                bool orange = y < 8 && ((x + y) / 4) % 2 == 0;
                int r = orange ? 31 : 30, g = orange ? 13 : 30, bl = orange ? 1 : 29;
                add16(b, uint16_t((r << 11) | (g << 6) | (bl << 1) | 1));
            }
        }
        return b;
    }

    Bytes sign_texels() {
        // 5 x 7 capitals.
        static const std::map<char, std::array<uint8_t, 7>> font = {
            { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } }, { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
            { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
            { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
            { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } }, { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
        };
        constexpr int w = 128, h = 32;
        constexpr uint8_t light = 0xEF, dark = 0x1F;   // IA8: intensity << 4 | alpha
        Bytes b(w * h, light);
        for (int x = 0; x < w; x++) { b[x] = b[w + x] = b[(h - 1) * w + x] = b[(h - 2) * w + x] = dark; }
        for (int y = 0; y < h; y++) { b[y * w] = b[y * w + 1] = b[y * w + w - 1] = b[y * w + w - 2] = dark; }
        auto text = [&](const char* s, int top) {
            int n = int(strlen(s)), width = n * 12 - 2, left = (w - width) / 2;
            for (int i = 0; i < n; i++) {
                const auto& g = font.at(s[i]);
                for (int r = 0; r < 7; r++) for (int col = 0; col < 5; col++) {
                    if (!(g[r] & (0x10 >> col))) continue;
                    for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
                        b[(top + r * 2 + dy) * w + left + i * 12 + col * 2 + dx] = dark;
                    }
                }
            }
        };
        text("ROAD", 2);
        text("CLOSED", 17);
        return b;
    }

    std::vector<Cmd> own_load(int which, uint32_t texels) {
        if (which == own_stripes) {     // RGBA16 32 x 16, repeating
            constexpr uint32_t w = 32, h = 16, line = w * 2 / 8;
            constexpr uint32_t tile = (4u << 14) | (5u << 4);      // mask t 16, s 32
            return { { 0xFD100000u, texels }, { 0xF5100000u, 0x07000000u | tile }, { 0xE6000000u, 0 },
                     { 0xF3000000u, 0x07000000u | ((w * h - 1) << 12) | (2048 / line) }, { 0xE7000000u, 0 },
                     { 0xF5100000u | (line << 9), tile }, { 0xF2000000u, (((w - 1) * 4) << 12) | ((h - 1) * 4) } };
        }
        // IA8 128 x 32, clamped: loaded as 16-bit texels, drawn as 8-bit.
        constexpr uint32_t w = 128, h = 32, line = (w + 7) / 8;
        uint32_t tile = (2u << 18) | (5u << 14) | (2u << 8) | (7u << 4);
        uint32_t words = std::max(1u, w / 8);
        return { { 0xFD700000u, texels }, { 0xF5700000u, 0x07000000u | tile }, { 0xE6000000u, 0 },
                 { 0xF3000000u, 0x07000000u | (((w * h + 1) / 2 - 1) << 12) | ((2048 + words - 1) / words) },
                 { 0xE7000000u, 0 }, { 0xF5680000u | (line << 9), tile },
                 { 0xF2000000u, (((w - 1) * 4) << 12) | ((h - 1) * 4) } };
    }

    // Appends the new models to the container: texels, vertices and lists after its end, then a new model table
    // (its own records and the new ones), texture and palette tables (copied) and name table (sorted), with the
    // header pointed at them. The old tables stay as unreferenced bytes.
    bool append_models(Bytes& g, const Scene& scene, std::vector<NewModel>& models, std::string& why) {
        Container c(g);
        uint32_t h[10];
        memcpy(h, c.h, sizeof(h));
        std::vector<std::pair<std::string, std::array<uint8_t, 0x34>>> records;
        std::vector<std::array<uint8_t, 0x18>> names;
        for (uint32_t i = 0; i < h[4]; i++) {
            std::array<uint8_t, 0x34> r;
            std::array<uint8_t, 0x18> n;
            memcpy(r.data(), &g[h[0] + i * 0x34], 0x34);
            memcpy(n.data(), &g[h[1] + i * 0x18], 0x18);
            records.push_back({ cstr(g, h[1] + i * 0x18, 16), r });
            names.push_back(n);
        }
        std::set<std::string> taken;
        for (const auto& r : records) taken.insert(r.first.substr(0, 15));
        Bytes tex_table(g.begin() + h[2], g.begin() + h[2] + size_t(h[5]) * 0x20);
        Bytes pal_table(g.begin() + h[3], g.begin() + h[3] + size_t(h[6]) * 0x18);

        bool uses_own = false;
        for (const NewModel& m : models) for (const OutQuad& q : m.quads) uses_own |= q.state < 0;
        align(g, 8);
        uint32_t stripes_at = 0, sign_at = 0;
        if (uses_own) {
            Bytes s = stripe_texels(), n = sign_texels();
            stripes_at = uint32_t(g.size());
            g.insert(g.end(), s.begin(), s.end());
            align(g, 8);
            sign_at = uint32_t(g.size());
            g.insert(g.end(), n.begin(), n.end());
            align(g, 8);
        }

        std::vector<std::pair<std::string, std::array<uint8_t, 0x34>>> added;
        std::vector<std::array<uint8_t, 0x18>> added_names;
        for (NewModel& m : models) {
            if (taken.contains(m.name.substr(0, 15))) { why = "model name " + m.name + " is taken"; return false; }
            // Vertices: quads by state, 8 quads (32 vertices) per G_VTX.
            std::map<int, std::vector<const OutQuad*>> by_state;
            for (const OutQuad& q : m.quads) by_state[q.state].push_back(&q);
            Bytes verts;
            std::vector<Cmd> list;
            std::vector<size_t> vtx_cmds;   // list entries whose w1 is an offset into verts
            float radius = 0;
            bool own_mode = false;
            for (const auto& [state, quads] : by_state) {
                if (state >= 0) {
                    if (own_mode) list.push_back({ 0xE3001001, 0x00008000 });
                    own_mode = false;
                    for (const Cmd& cmd : scene.states[size_t(state)].cmds) list.push_back(cmd);
                }
                else {
                    if (!own_mode) {
                        list.push_back({ 0xE7000000, 0 });
                        list.push_back({ 0xE200001C, 0xC8112230 });     // fog, then AA z-buffered opaque
                        list.push_back({ 0xE3001001, 0x00000000 });     // no TLUT: true color
                        list.push_back({ 0xE3000F00, 0x00000000 });     // no mipmaps
                        list.push_back({ 0xFC127FFF, 0xFFFFF238 });     // texture x shade
                        list.push_back({ 0xD7000002, 0xFFFFFFFF });     // texture on, tile 0, scale 1
                        own_mode = true;
                    }
                    for (const Cmd& cmd : own_load(state, state == own_stripes ? stripes_at : sign_at)) list.push_back(cmd);
                }
                for (size_t i = 0; i < quads.size(); i += 8) {
                    size_t n = std::min<size_t>(8, quads.size() - i);
                    uint32_t at = uint32_t(verts.size());
                    for (size_t k = 0; k < n; k++) {
                        const OutQuad& q = *quads[i + k];
                        for (int j = 0; j < 4; j++) {
                            const OutVtx& v = q.v[std::min(j, q.count - 1)];
                            V3 rel = mul(sub(v.p, m.origin), 1.0 / m.scale);
                            radius = std::max(radius, float(len(sub(v.p, m.origin))));
                            for (int a = 0; a < 3; a++) {
                                long u = std::lround(rel[a] * 16.0);
                                if (u < -32767 || u > 32767) { why = m.name + ": vertex out of reach"; return false; }
                                add16(verts, uint16_t(int16_t(u)));
                            }
                            add16(verts, 0);
                            for (int a = 0; a < 2; a++) add16(verts, uint16_t(int16_t(std::clamp(v.st[a], -32767, 32767))));
                            for (int a = 0; a < 4; a++) verts.push_back(v.c[a]);
                        }
                    }
                    uint32_t slots = uint32_t(n * 4);
                    vtx_cmds.push_back(list.size());
                    list.push_back({ 0x01000000 | (slots << 12) | (slots << 1), at });
                    for (size_t k = 0; k < n; k++) {
                        const OutQuad& q = *quads[i + k];
                        uint32_t b = uint32_t(k * 4);
                        uint32_t t0 = ((b * 2) << 16) | (((b + 1) * 2) << 8) | ((b + 2) * 2);
                        if (q.count == 3) list.push_back({ 0x05000000 | t0, 0 });
                        else list.push_back({ 0x06000000 | t0, ((b * 2) << 16) | (((b + 2) * 2) << 8) | ((b + 3) * 2) });
                    }
                }
            }
            if (own_mode || !by_state.empty()) list.push_back({ 0xE3001001, 0x00008000 });  // the TLUT mode Rush 2's lists leave set
            list.push_back({ 0xDF000000, 0 });
            align(g, 8);
            uint32_t vbase = uint32_t(g.size());
            g.insert(g.end(), verts.begin(), verts.end());
            align(g, 8);
            uint32_t lbase = uint32_t(g.size());
            for (size_t i = 0; i < list.size(); i++) {
                uint32_t w1 = list[i].w1;
                if (std::find(vtx_cmds.begin(), vtx_cmds.end(), i) != vtx_cmds.end()) w1 += vbase;
                add32(g, list[i].w0);
                add32(g, w1);
            }
            std::array<uint8_t, 0x34> r{};
            Bytes rb;
            add32(rb, 1);
            add16(rb, 0); add16(rb, 0); add32(rb, 0x44FA0000);  // flags 0, distance 2000
            add32(rb, lbase);
            for (int k = 1; k < 4; k++) { add16(rb, 0); add16(rb, 0); add32(rb, 0); add32(rb, 0); }
            memcpy(r.data(), rb.data(), 0x34);
            std::array<uint8_t, 0x18> n{};
            memcpy(n.data(), m.name.data(), std::min<size_t>(15, m.name.size()));
            float rad = radius;
            uint32_t bits;
            memcpy(&bits, &rad, 4);
            for (int k = 0; k < 4; k++) n[16 + k] = uint8_t(bits >> (24 - 8 * k));
            added.push_back({ m.name, r });
            added_names.push_back(n);
        }
        for (size_t i = 0; i < added.size(); i++) { records.push_back(added[i]); names.push_back(added_names[i]); }
        // Sorted by name (Rush 2's lookup is a binary search over 15 characters); name i belongs to model i.
        std::vector<size_t> order(records.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return strncmp(reinterpret_cast<const char*>(names[a].data()), reinterpret_cast<const char*>(names[b].data()), 15) < 0;
        });
        align(g, 8);
        h[0] = uint32_t(g.size());
        for (size_t i : order) g.insert(g.end(), records[i].second.begin(), records[i].second.end());
        h[2] = uint32_t(g.size());
        g.insert(g.end(), tex_table.begin(), tex_table.end());
        h[3] = uint32_t(g.size());
        g.insert(g.end(), pal_table.begin(), pal_table.end());
        h[1] = uint32_t(g.size());
        for (size_t i : order) g.insert(g.end(), names[i].begin(), names[i].end());
        h[4] = uint32_t(records.size());
        for (int i = 0; i < 10; i++) put32(g, size_t(i) * 4, h[i]);
        return true;
    }

    // Appends top-level records drawing `models` (at their origins, scaled) to the end of the placement's top-level
    // chain. Their culling boxes are empty: they are never a visibility region (the regions are the first records of
    // the chain, as many as the track's region count) and never hidden.
    void append_records(Bytes& p, const std::vector<NewModel>& models) {
        uint32_t first;
        std::vector<Record> recs = read_records(p, first);
        if (recs.empty() || models.empty()) return;
        int last = 0;
        for (int guard = 0; recs[size_t(last)].next >= 0 && guard < 100000; guard++) last = recs[size_t(last)].next;
        int base = int(recs.size());
        put16(p, first + size_t(last) * 0x64 + 0x44, uint16_t(int16_t(base)));
        for (size_t i = 0; i < models.size(); i++) {
            Bytes r(0x64, 0);
            memcpy(r.data(), models[i].name.data(), std::min<size_t>(15, models[i].name.size()));
            for (int k = 0; k < 9; k++) putf(r, 0x10 + k * 4, k % 4 == 0 ? float(models[i].scale) : 0.0f);
            for (int k = 0; k < 3; k++) putf(r, 0x34 + k * 4, float(models[i].origin[k]));
            put32(r, 0x40, 0x40);
            put16(r, 0x44, uint16_t(int16_t(i + 1 < models.size() ? base + int(i) + 1 : -1)));
            put16(r, 0x46, 0xFFFF);
            p.insert(p.end(), r.begin(), r.end());
        }
    }

    // ---- collision ----------------------------------------------------------------------------------------------
    constexpr int max_leaf = 36;     // the leaf queries' u16[36] stack buffers

    struct Cell { int32_t x0, x1, z0, z1; };

    // Rush 2's quadrant boxes, as California Speed's builder split them (toward zero, children sharing the line).
    Cell quadrant(const Cell& c, int q) {
        int32_t sx = (c.x0 + c.x1) / 2, sz = (c.z0 + c.z1) / 2;
        Cell o = c;
        if (q & 1) o.x0 = sx; else o.x1 = sx;
        if (q & 2) o.z1 = sz; else o.z0 = sz;
        return o;
    }

    struct Box { double x0, x1, z0, z1; };
    bool overlaps(const Box& b, const Cell& c, double margin) {
        return b.x1 >= c.x0 - margin && b.x0 <= c.x1 + margin && b.z1 >= c.z0 - margin && b.z0 <= c.z1 + margin;
    }

    bool add_collision(Bytes& file, const std::vector<NewPoly>& add, std::string& why) {
        Collision col(file);
        if (!col.valid()) { why = "bad collision file"; return false; }
        if (col.d[col.node0 + 2] != 0) { why = "collision root already tagged"; return false; }
        // Old polygon boxes (for refiling a split quadrant's polygons).
        std::vector<Box> boxes;
        for (uint32_t i = 0; i < col.polys; i++) {
            std::vector<V3> w = col.polygon(i);
            Box b{ 1e30, -1e30, 1e30, -1e30 };
            for (const V3& v : w) { b.x0 = std::min(b.x0, v[0]); b.x1 = std::max(b.x1, v[0]); b.z0 = std::min(b.z0, v[2]); b.z1 = std::max(b.z1, v[2]); }
            boxes.push_back(b);
        }
        // New polygons: records, vertices, vertex lists.
        Bytes polys(file.begin() + col.poly0, file.begin() + col.vert0);
        Bytes vertsb(file.begin() + col.vert0, file.begin() + col.leaf0);
        Bytes leaves(file.begin() + col.leaf0, file.begin() + col.vlist0);
        Bytes vlist(file.begin() + col.vlist0, file.begin() + col.vlist0 + col.vlist_bytes);
        Bytes nodes(file.begin() + col.node0, file.begin() + col.poly0);
        uint32_t nv = col.verts;
        for (const NewPoly& np : add) {
            V3 n = norm(cross(sub(np.v[1], np.v[0]), sub(np.v[2], np.v[0])));
            std::vector<V3> v = np.v;
            if (dot(n, np.front) < 0) { n = mul(n, -1); }
            // Wind so that with x along the first edge and y the normal, every later vertex has local z > 0
            // (track_mod_convert.cpp build_collision; func_8006C670's edge tests).
            V3 x = norm(sub(v[1], v[0]));
            V3 z = norm(cross(x, n));
            if (dot(sub(v[2], v[0]), z) < 0) { std::reverse(v.begin() + 1, v.end()); x = norm(sub(v[1], v[0])); z = norm(cross(x, n)); }
            V3 y = norm(cross(z, x));
            const V3* rows[3] = { &x, &y, &z };
            Bytes rec(0x18, 0);
            put16(rec, 0, np.flags);
            put16(rec, 2, uint16_t((np.info_hi & 0xFFF0) | v.size()));
            double mq[9];
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
                int16_t q = int16_t(std::clamp(std::lround((*rows[r])[c] * 16384.0), -32767l, 32767l));
                put16(rec, 4 + (r * 3 + c) * 2, uint16_t(q));
                mq[r * 3 + c] = q / 16384.0;
            }
            if (vlist.size() > 0xFFF0) { why = "vertex lists full"; return false; }
            put16(rec, 0x16, uint16_t(vlist.size()));
            polys.insert(polys.end(), rec.begin(), rec.end());
            auto vert = [&](const V3& p) {
                int32_t q[3];
                for (int k = 0; k < 3; k++) q[k] = int32_t(std::floor(p[k] * 32.0 + 0.5));
                for (int k = 0; k < 3; k++) add16(vertsb, uint16_t(int16_t(q[k] >> 5)));
                add16(vertsb, uint16_t(((q[0] & 31) << 10) | ((q[1] & 31) << 5) | (q[2] & 31)));
            };
            vert(v[0]);
            for (size_t k = 1; k < v.size(); k++) {
                V3 d = sub(v[k], v[0]);
                vert({ mq[0] * d[0] + mq[1] * d[1] + mq[2] * d[2], 0.0, mq[6] * d[0] + mq[7] * d[1] + mq[8] * d[2] });
            }
            add16(vlist, uint16_t(nv));
            vlist.push_back(uint8_t(0xC0 | (v.size() - 1)));
            nv += uint32_t(v.size());
        }
        uint32_t first_new = col.polys;
        uint32_t total_polys = col.polys + uint32_t(add.size());
        if (total_polys > 8191 || nv > 0xFFFF) { why = "too many polygons"; return false; }
        std::vector<Box> new_boxes;
        for (const NewPoly& np : add) {
            Box b{ 1e30, -1e30, 1e30, -1e30 };
            for (const V3& v : np.v) { b.x0 = std::min(b.x0, v[0]); b.x1 = std::max(b.x1, v[0]); b.z0 = std::min(b.z0, v[2]); b.z1 = std::max(b.z1, v[2]); }
            new_boxes.push_back(b);
        }

        // Leaves: far leaves (bytes after the vertex lists) and their offsets from the leaf section.
        Bytes far;
        std::vector<uint32_t> far_index;  // far-leaf table: offset into `far` of each
        auto leaf_polys = [&](uint32_t offset) {
            std::vector<uint32_t> out;
            if (offset == 0) return out;
            size_t o = offset;
            // The count byte is the number of polygons, not of entries (a run entry covers several).
            int count = leaves[o++];
            while (int(out.size()) < count) {
                uint16_t e = u16(leaves, o);
                o += 2;
                for (uint32_t k = 0; k <= uint32_t(e >> 13); k++) out.push_back((e & 0x1FFF) + k);
            }
            return out;
        };
        auto make_far = [&](const std::vector<uint32_t>& list) -> uint16_t {
            uint32_t at = uint32_t(far.size());
            std::vector<uint16_t> entries;
            for (size_t i = 0; i < list.size();) {
                size_t j = i;
                while (j + 1 < list.size() && list[j + 1] == list[j] + 1 && j - i < 7) j++;
                entries.push_back(uint16_t((uint32_t(j - i) << 13) | list[i]));
                i = j + 1;
            }
            far.push_back(uint8_t(list.size()));  // polygon count, as the game reads it
            for (uint16_t e : entries) add16(far, e);
            far_index.push_back(at);
            return uint16_t(far_index.size() - 1);
        };
        auto node_field = [&](uint32_t i, size_t off) -> uint8_t& { return nodes[size_t(i) * 0x14 + off]; };
        auto node_count = [&]() { return uint32_t(nodes.size() / 0x14); };
        auto node_cell = [&](uint32_t i) {
            size_t o = size_t(i) * 0x14;
            return Cell{ s16(nodes, o + 4), s16(nodes, o + 6), s16(nodes, o + 8), s16(nodes, o + 10) };
        };
        // A point outside the root's box gets no collision at all (collision_node_find_cached_8006CE28 climbs parents
        // until the cell is inside a node's box and gives up past the root), so where the new polygons reach past it
        // the root's box grows by as much on both sides of that axis: min + max stays, so do its split lines (in
        // either rounding), and its edge quadrants take the new cells, as `clip` files them.
        {
            Box all{ 1e30, -1e30, 1e30, -1e30 };
            for (const Box& b : new_boxes) { all.x0 = std::min(all.x0, b.x0); all.x1 = std::max(all.x1, b.x1); all.z0 = std::min(all.z0, b.z0); all.z1 = std::max(all.z1, b.z1); }
            Cell r = node_cell(0);
            int32_t dx = std::max({ 0, r.x0 - (int32_t(std::floor(all.x0)) - 4), int32_t(std::ceil(all.x1)) + 4 - r.x1 });
            int32_t dz = std::max({ 0, r.z0 - (int32_t(std::floor(all.z0)) - 4), int32_t(std::ceil(all.z1)) + 4 - r.z1 });
            if (r.x0 - dx < -32768 || r.x1 + dx > 32767 || r.z0 - dz < -32768 || r.z1 + dz > 32767) { why = "collision root too big"; return false; }
            put16(nodes, 4, uint16_t(int16_t(r.x0 - dx))); put16(nodes, 6, uint16_t(int16_t(r.x1 + dx)));
            put16(nodes, 8, uint16_t(int16_t(r.z0 - dz))); put16(nodes, 10, uint16_t(int16_t(r.z1 + dz)));
        }
        const Cell root = node_cell(0);
        // The cells that reach quadrant q of a node whose cells are `region`: Rush 2's descent
        // (collision_node_find_8006CD60) splits on the node's box but never checks it, so a node's edge quadrants
        // also take the cells of its region past its box.
        auto clip = [&](const Cell& region, const Cell& box, int q) {
            Cell s = quadrant(box, q), r = region;
            if (q & 1) r.x0 = std::max(r.x0, s.x0); else r.x1 = std::min(r.x1, s.x1);
            if (q & 2) r.z1 = std::min(r.z1, s.z1); else r.z0 = std::max(r.z0, s.z0);
            return r;
        };
        // Whether polygon p (old: its box; new: its outline in x/z, as a long diagonal triangle's box covers cells it
        // never reaches) comes within 2 units of the cells `r`.
        auto touches = [&](uint32_t p, const Cell& r) {
            if (p < first_new) return overlaps(boxes[p], r, 2.0);
            if (!overlaps(new_boxes[p - first_new], r, 2.0)) return false;
            const std::vector<V3>& v = add[p - first_new].v;
            const double cx[4] = { r.x0 - 2.0, r.x1 + 2.0, r.x1 + 2.0, r.x0 - 2.0 }, cz[4] = { r.z0 - 2.0, r.z0 - 2.0, r.z1 + 2.0, r.z1 + 2.0 };
            // Separating edge of the polygon: the cells all lie outside it (the polygon's own side taken from its
            // other corners).
            for (size_t k = 0; k < v.size(); k++) {
                const V3& a = v[k];
                const V3& b = v[(k + 1) % v.size()];
                double ex = b[0] - a[0], ez = b[2] - a[2];
                if (std::fabs(ex) + std::fabs(ez) < 1e-9) continue;
                double inner = 0;
                for (const V3& o : v) {
                    double side = ex * (o[2] - a[2]) - ez * (o[0] - a[0]);
                    if (std::fabs(side) > std::fabs(inner)) inner = side;
                }
                if (inner == 0) continue;
                bool apart = true;
                for (int c = 0; c < 4 && apart; c++) {
                    double side = ex * (cz[c] - a[2]) - ez * (cx[c] - a[0]);
                    if ((side > 0) == (inner > 0) || side == 0) apart = false;
                }
                if (apart) return false;
            }
            return true;
        };
        size_t dropped = 0;
        std::string drop_note;
        // Files `list` (old polygon indices plus new ones) into quadrant q of node `parent` with box `cell`, reached
        // by the cells `region`. A node made to split it takes `region` as its box: every cell in it reaches the node,
        // so the cached walk's climb still finds it, and the splits divide all of it (an edge quadrant's cells past the
        // box would otherwise all fall in the new node's edge quadrants again, however small it got).
        std::function<void(uint32_t, int, const Cell&, const Cell&, const std::vector<uint32_t>&)> file_quadrant;
        file_quadrant = [&](uint32_t parent, int q, const Cell& cell, const Cell& region, const std::vector<uint32_t>& list) {
            size_t child_at = size_t(parent) * 0x14 + 0xC + q * 2;
            bool small = region.x1 - region.x0 <= 8 && region.z1 - region.z0 <= 8;
            if (int(list.size()) <= max_leaf || small) {
                std::vector<uint32_t> l = list;
                if (int(l.size()) > max_leaf) {
                    if (!dropped) {
                        size_t olds = 0;
                        for (uint32_t pi : l) olds += pi < first_new;
                        drop_note = " (first: cells x " + std::to_string(region.x0) + ".." + std::to_string(region.x1) + " z " +
                                    std::to_string(region.z0) + ".." + std::to_string(region.z1) + ", " + std::to_string(olds) +
                                    " of the track's, " + std::to_string(l.size() - olds) + " new)";
                    }
                    dropped += l.size() - max_leaf;
                    l.resize(max_leaf);
                }
                uint16_t idx = make_far(l);
                node_field(parent, 3) = uint8_t((node_field(parent, 3) & ~(1u << q)) | (0x10u << q));
                put16(nodes, child_at, idx);
                return;
            }
            uint32_t id = node_count();
            Bytes n(0x14, 0);
            put16(n, 0, uint16_t(int16_t(parent)));
            n[3] = 0x0F;
            put16(n, 4, uint16_t(int16_t(region.x0))); put16(n, 6, uint16_t(int16_t(region.x1)));
            put16(n, 8, uint16_t(int16_t(region.z0))); put16(n, 10, uint16_t(int16_t(region.z1)));
            nodes.insert(nodes.end(), n.begin(), n.end());
            node_field(parent, 3) = uint8_t((node_field(parent, 3) | (1u << q)) & ~(0x10u << q));
            put16(nodes, child_at, uint16_t(id));
            for (int k = 0; k < 4; k++) {
                Cell sub = quadrant(region, k);
                Cell r = sub;
                std::vector<uint32_t> mine;
                for (uint32_t p : list) if (touches(p, r)) mine.push_back(p);
                if (mine.empty()) {
                    node_field(id, 3) &= uint8_t(~(1u << k));
                    put16(nodes, size_t(id) * 0x14 + 0xC + k * 2, 0);
                }
                else {
                    file_quadrant(id, k, sub, r, mine);
                }
            }
        };
        // Every quadrant (leaf or null) a new polygon's box overlaps, through the existing tree.
        struct Hit { uint32_t node; int q; Cell cell, region; };
        std::vector<Hit> hits;
        std::function<void(uint32_t, const Cell&)> visit = [&](uint32_t i, const Cell& region) {
            uint8_t mask = node_field(i, 3);
            Cell c = node_cell(i);
            for (int q = 0; q < 4; q++) {
                Cell s = quadrant(c, q);
                Cell r = clip(region, c, q);
                bool any = false;
                for (size_t k = 0; k < add.size() && !any; k++) any = touches(first_new + uint32_t(k), r);
                if (!any) continue;
                uint16_t child = u16(nodes, size_t(i) * 0x14 + 0xC + q * 2);
                if ((mask & (1u << q)) && child != 0) visit(child, r);
                else hits.push_back({ i, q, s, r });
            }
        };
        visit(0, root);
        for (const Hit& h : hits) {
            uint8_t mask = node_field(h.node, 3);
            uint16_t child = u16(nodes, size_t(h.node) * 0x14 + 0xC + h.q * 2);
            std::vector<uint32_t> list = (mask & (1u << h.q)) ? std::vector<uint32_t>{} : leaf_polys(child);
            for (size_t k = 0; k < add.size(); k++) if (touches(first_new + uint32_t(k), h.region)) list.push_back(first_new + uint32_t(k));
            file_quadrant(h.node, h.q, h.cell, h.region, list);
        }
        if (dropped) { why = std::to_string(dropped) + " polygons left out of full leaves" + drop_note; return false; }
        if (nodes.size() / 0x14 > 0x7FFF) { why = "too many nodes"; return false; }

        // Reassemble: header, segs, nodes, polys, verts, leaves (unchanged), vertex lists, far table and leaves.
        Bytes out(file.begin(), file.begin() + col.node0);
        put16(out, 2, uint16_t(nodes.size() / 0x14));
        put16(out, 4, uint16_t(total_polys));
        put16(out, 6, uint16_t(nv));
        put16(out, 10, uint16_t(vlist.size()));
        nodes[2] = rush2::calspeed::scenery_collision_tag;
        out.insert(out.end(), nodes.begin(), nodes.end());
        out.insert(out.end(), polys.begin(), polys.end());
        out.insert(out.end(), vertsb.begin(), vertsb.end());
        size_t leaf_at = out.size();
        out.insert(out.end(), leaves.begin(), leaves.end());
        out.insert(out.end(), vlist.begin(), vlist.end());
        align(out, 4);
        size_t table_at = out.size();
        size_t far_at = table_at + far_index.size() * 4;
        for (uint32_t f : far_index) add32(out, uint32_t(far_at + f - leaf_at));
        out.insert(out.end(), far.begin(), far.end());
        align(out, 8);
        (void)table_at;
        file = std::move(out);
        return true;
    }

    // ---- behind the start -----------------------------------------------------------------------------------------
    std::vector<V3> spine_points(const Bytes& path) {
        constexpr size_t route = 0x32C;
        uint32_t n = u16(path, route);
        uint32_t branches = path.size() > route + 8 ? path[route + 8] : 0;
        size_t o = route + 16 + size_t(branches) * 16 + 2;
        std::vector<V3> out;
        for (uint32_t i = 0; i < n && o + 6 <= path.size(); i++, o += 6) {
            out.push_back({ double(s16(path, o)), double(s16(path, o + 2)), double(s16(path, o + 4)) });
        }
        return out;
    }

    // A piece of the cross-section: two ends (across, height) with their texture coordinates and colors, the rate the
    // coordinates change per foot back along the road, the state it draws with, and its surface's facing.
    struct ProfileEnd { double l, y; double st[2]; double c[4]; };
    struct Piece {
        ProfileEnd e[2];
        double rate[2];
        int state;
        V3 normal;
        double lead = 0;        // taken this many ft ahead of the cut (it starts there): drawn straight back to the cut first
        bool bare = false;      // behind one of the track's own walls: out of reach, no collision
        bool run_on = false;    // goes on straight past the sweep's end (a canyon wall too tall for the crest to hide)
        double skip[2] = { 0, 0 };   // per end: the stock surface goes on this many ft behind the cut, drawn from there
        double drift = 0;       // a structure crossing at an angle: ft across per ft along (forward)
        double hold = 0;        // ... kept on its own heading this far back (past the stock one's end), then eased out
        double rise_len = 0;    // grows out of flat ground at `rise_base` at the cut to its full shape this far back
        double rise_base = 0;
        double pull_d = 0;      // the stock copy of this edge piece goes on this far behind the cut, curving in by
        double pull_dl = 0;     // ... this many ft across: drawn from its end there, back out to its own line over 30 ft
    };
    // How far across a drifting structure has moved `s` ft back from the cut: on its own heading out to `hold`, then
    // turned onto the road's over 300 ft.
    double drift_shift(const Piece& pc, double s) {
        constexpr double S = 300, J = 30;
        s = std::max(s, 0.0);
        double past = std::max(s - pc.hold, 0.0);
        double eased = past < S ? past - past * past / (2 * S) : S / 2;
        double pulled = 0;
        if (pc.pull_d > 0) {
            double u = std::clamp((s - pc.pull_d) / J, 0.0, 1.0);
            pulled = pc.pull_dl * (1 - u * u * (3 - 2 * u));
        }
        return -pc.drift * (std::min(s, pc.hold) + eased) + pulled;
    }

    // The sweep's path: per sample, distance back, center, back direction, left-of-back across, turn so far, height.
    struct Sample { double s; V3 c; V3 t, n; double turn, h; };

    struct Hermite { double s, h, slope; };
    double eval_height(const std::vector<Hermite>& k, double s) {
        if (s <= k.front().s) return k.front().h + k.front().slope * (s - k.front().s);
        for (size_t i = 0; i + 1 < k.size(); i++) {
            if (s <= k[i + 1].s) {
                double d = k[i + 1].s - k[i].s, u = (s - k[i].s) / d;
                double h00 = 2 * u * u * u - 3 * u * u + 1, h10 = u * u * u - 2 * u * u + u;
                double h01 = -2 * u * u * u + 3 * u * u, h11 = u * u * u - u * u;
                return h00 * k[i].h + h10 * d * k[i].slope + h01 * k[i + 1].h + h11 * d * k[i + 1].slope;
            }
        }
        return k.back().h;
    }

    bool behind_start(int t, const Scene& scene, const Collision& col, const Bytes& path, std::vector<NewModel>& models,
                      std::vector<NewPoly>& polys, std::string& report, const std::set<int>& skip_states) {
        Start cfg = start_for(t);
        if (!cfg.on) return true;
        std::vector<V3> spine = spine_points(path);
        if (spine.size() < 4) { report += "no spine\n"; return false; }
        V3 s0 = spine[0];
        V3 f{ 0, 0, 0 };
        double grade = 0;
        for (const V3& p : spine) {
            double d = std::hypot(p[0] - s0[0], p[2] - s0[2]);
            if (d >= 40) { f = norm(V3{ p[0] - s0[0], 0, p[2] - s0[2] }); grade = (p[1] - s0[1]) / d; break; }
        }
        if (len(f) < 0.5) { report += "no start direction\n"; return false; }
        V3 n{ -f[2], 0, f[0] };     // across (left of a car facing back is -n... sign fixed by `turn`)
        auto along = [&](const V3& p) { return dot(sub(p, s0), f); };
        auto across = [&](const V3& p) { return dot(sub(p, s0), n); };

        // The road's back end: how far back the floor triangles reach along lines parallel to the spine's, across
        // the road. The road's end can be slanted; the cut goes where the whole road is still there (the most
        // forward of the lines' ends near the spine's own).
        auto floor_end = [&](double line) {
            double a_end = 1e30;
            for (const Tri& tr : scene.tris) {
                double a[3], l[3], y = 0;
                for (int k = 0; k < 3; k++) { a[k] = along(tr.v[k].p); l[k] = across(tr.v[k].p) - line; y += tr.v[k].p[1] / 3; }
                if (std::fabs(y - s0[1]) > 25) continue;
                V3 nrm = norm(cross(sub(tr.v[1].p, tr.v[0].p), sub(tr.v[2].p, tr.v[0].p)));
                if (std::fabs(nrm[1]) < 0.7) continue;
                double lo = 1e30;
                for (int k = 0; k < 3; k++) {
                    int j = (k + 1) % 3;
                    if ((l[k] < 0) == (l[j] < 0) || l[k] == l[j]) continue;
                    lo = std::min(lo, a[k] + (a[j] - a[k]) * l[k] / (l[k] - l[j]));
                }
                if (lo > -400 && lo < 100) a_end = std::min(a_end, lo);
            }
            return a_end;
        };
        double a_end = floor_end(0), centre = a_end;
        if (a_end < 1e29) {
            for (double line = -30; line <= 30; line += 5) {
                double e = floor_end(line);
                if (e < 1e29 && e > a_end && e < centre + 60) a_end = e;
            }
        }
        if (a_end > 1e29) {
            char line[160];
            snprintf(line, sizeof(line), "no road at the start (%.0f, %.0f, %.0f) facing (%.2f, %.2f)\n", s0[0], s0[1], s0[2], f[0], f[2]);
            report += line;
            return false;
        }
        double cut = a_end + 0.75;
        // The barricades stand `cfg.barricade` ft behind where the road ends on the spine's line.
        double barricade = cfg.barricade + std::max(0.0, a_end - centre);

        // The cross-section at the plane `plane` ft along (the cut, or ahead of it to compare).
        auto section = [&](double plane, double lim = 0) {
        if (lim <= 0) lim = cfg.reach;
        std::vector<Piece> pieces;
        for (const Tri& tr : scene.tris) {
            if (skip_states.contains(tr.state)) continue;
            double a[3];
            for (int k = 0; k < 3; k++) a[k] = along(tr.v[k].p) - plane;
            if ((a[0] > 0 && a[1] > 0 && a[2] > 0) || (a[0] < 0 && a[1] < 0 && a[2] < 0)) continue;
            ProfileEnd ends[2];
            int found = 0;
            for (int k = 0; k < 3 && found < 2; k++) {
                int j = (k + 1) % 3;
                if ((a[k] < 0) == (a[j] < 0) || a[k] == a[j]) continue;
                double u = a[k] / (a[k] - a[j]);
                V3 p = add(tr.v[k].p, mul(sub(tr.v[j].p, tr.v[k].p), u));
                ProfileEnd& e = ends[found++];
                e.l = across(p);
                e.y = p[1];
                for (int x = 0; x < 2; x++) e.st[x] = tr.v[k].st[x] + (tr.v[j].st[x] - tr.v[k].st[x]) * u;
                for (int x = 0; x < 4; x++) e.c[x] = tr.v[k].c[x] + (double(tr.v[j].c[x]) - tr.v[k].c[x]) * u;
            }
            if (found < 2) continue;
            if (std::hypot(ends[0].l - ends[1].l, ends[0].y - ends[1].y) < 0.05) continue;
            if (ends[0].y < s0[1] - 400 || ends[0].y > s0[1] + 800) continue;
            Piece pc;
            pc.e[0] = ends[0];
            pc.e[1] = ends[1];
            pc.state = tr.state;
            V3 e1 = sub(tr.v[1].p, tr.v[0].p), e2 = sub(tr.v[2].p, tr.v[0].p);
            pc.normal = norm(cross(e1, e2));
            // A surface facing along the road (a tree's card, a sign, a building's end) would sweep into a wall.
            if (std::fabs(dot(pc.normal, f)) > 0.6) continue;
            // Texture rate going back (-f) within the triangle's plane.
            V3 back = mul(f, -1);
            double g11 = dot(e1, e1), g12 = dot(e1, e2), g22 = dot(e2, e2), det = g11 * g22 - g12 * g12;
            double b1 = dot(e1, back), b2 = dot(e2, back);
            double al = det != 0 ? (b1 * g22 - b2 * g12) / det : 0, be = det != 0 ? (b2 * g11 - b1 * g12) / det : 0;
            V3 proj = add(mul(e1, al), mul(e2, be));
            double pl = len(proj);
            for (int x = 0; x < 2; x++) {
                double d1 = tr.v[1].st[x] - tr.v[0].st[x], d2 = tr.v[2].st[x] - tr.v[0].st[x];
                pc.rate[x] = pl > 0.2 ? (al * d1 + be * d2) / pl : 0;
                if (scene.states[size_t(tr.state)].period[x] == 0) pc.rate[x] = 0;
            }
            // Clip to the reach across.
            bool keep = true;
            for (int side = 0; side < 2 && keep; side++) {
                double sgn = side ? -1 : 1;
                ProfileEnd& p0 = pc.e[0];
                ProfileEnd& p1 = pc.e[1];
                bool o0 = sgn * p0.l > lim, o1 = sgn * p1.l > lim;
                if (o0 && o1) { keep = false; break; }
                if (o0 || o1) {
                    ProfileEnd& out = o0 ? p0 : p1;
                    const ProfileEnd& in = o0 ? p1 : p0;
                    double u = (sgn * lim - in.l) / (out.l - in.l);
                    ProfileEnd e;
                    e.l = sgn * lim;
                    e.y = in.y + (out.y - in.y) * u;
                    for (int x = 0; x < 2; x++) e.st[x] = in.st[x] + (out.st[x] - in.st[x]) * u;
                    for (int x = 0; x < 4; x++) e.c[x] = in.c[x] + (out.c[x] - in.c[x]) * u;
                    out = e;
                }
            }
            if (keep) pieces.push_back(pc);
        }
        return pieces;
        };
        std::vector<Piece> pieces = section(cut);
        if (pieces.empty()) { report += "empty cross-section\n"; return false; }
        // A structure crossing over the road ahead of the cut (Los Angeles' wide flat underside 25 ft up, whose
        // diagonal edge the cut clips a corner of) would sweep into a strip hanging in the air: an elevated flat
        // surface that 20 ft ahead (same state, same height) is much wider and reaches over the spine is left out.
        // Raised ground beside the road (Monterey's) widens ahead too but never reaches over it.
        {
            std::vector<Piece> ahead = section(cut + 20);
            auto width_at = [&](const std::vector<Piece>& list, int state, double y) {
                double w = 0;
                for (const Piece& pc : list) {
                    if (pc.state == state && std::fabs(pc.e[0].y - y) < 0.5 && std::fabs(pc.e[1].y - y) < 0.5) w += std::fabs(pc.e[1].l - pc.e[0].l);
                }
                return w;
            };
            auto over_spine = [&](int state, double y) {
                for (const Piece& pc : ahead) {
                    if (pc.state == state && std::fabs(pc.e[0].y - y) < 0.5 && std::fabs(pc.e[1].y - y) < 0.5 &&
                        std::min(pc.e[0].l, pc.e[1].l) < 0 && std::max(pc.e[0].l, pc.e[1].l) > 0) return true;
                }
                return false;
            };
            std::vector<Piece> kept;
            int dropped = 0;
            for (const Piece& pc : pieces) {
                double y = pc.e[0].y;
                bool flat_up = std::fabs(pc.e[1].y - y) < 0.5 && y > s0[1] + 12;
                if (flat_up && over_spine(pc.state, y)) {
                    double here = width_at(pieces, pc.state, y);
                    if (width_at(ahead, pc.state, y) > here * 2 + 15) { dropped++; continue; }
                }
                kept.push_back(pc);
            }
            if (dropped) {
                char line[120];
                snprintf(line, sizeof(line), "start: %d pieces of structures crossing over the road left out\n", dropped);
                report += line;
            }
            pieces = std::move(kept);
        }
        // Texture drift: a structure running a little off the road's direction (Los Angeles' overpass decks, 16 deg)
        // has its texture's across axis change going back too, so swept straight back its stripes would slide
        // sideways off the deck. An axis that changes much more across the piece than going back, while the other
        // changes faster going back, is the across axis: it stops changing. A short piece of a run of one state (a
        // squeezed seam strip) takes the along rates of the run's longest piece. Only a structure's underside (over
        // 10 ft up, facing down: in the track's winding a floor's normal points down, an underside's up): the road's
        // own pieces carry small drifts that keep its lane lines meeting, and raised ground keeps its look.
        int drift_fixed = 0;
        {
            auto overhead = [&](const Piece& pc) { return std::min(pc.e[0].y, pc.e[1].y) > s0[1] + 10 && pc.normal[1] > 0.3; };
            for (Piece& pc : pieces) {
                if (!overhead(pc)) continue;
                double w = std::hypot(pc.e[1].l - pc.e[0].l, pc.e[1].y - pc.e[0].y);
                for (int x = 0; x < 2; x++) {
                    double grad = std::fabs(pc.e[1].st[x] - pc.e[0].st[x]) / std::max(w, 0.1);
                    if (grad > 2 * std::fabs(pc.rate[x]) && std::fabs(pc.rate[1 - x]) > std::fabs(pc.rate[x]) && pc.rate[x] != 0) {
                        pc.rate[x] = 0;
                        drift_fixed++;
                    }
                }
            }
            std::vector<int> run(pieces.size());
            for (size_t i = 0; i < pieces.size(); i++) run[i] = int(i);
            std::function<int(int)> root = [&](int i) { return run[size_t(i)] == i ? i : run[size_t(i)] = root(run[size_t(i)]); };
            for (size_t i = 0; i < pieces.size(); i++) for (size_t j = i + 1; j < pieces.size(); j++) {
                if (pieces[i].state != pieces[j].state) continue;
                bool joined = false;
                for (const ProfileEnd& a : pieces[i].e) for (const ProfileEnd& b : pieces[j].e) joined |= std::hypot(a.l - b.l, a.y - b.y) < 0.3;
                if (joined) run[size_t(root(int(i)))] = root(int(j));
            }
            auto width = [](const Piece& pc) { return std::hypot(pc.e[1].l - pc.e[0].l, pc.e[1].y - pc.e[0].y); };
            std::vector<int> longest(pieces.size(), -1);
            for (size_t i = 0; i < pieces.size(); i++) {
                int r = root(int(i));
                if (longest[size_t(r)] < 0 || width(pieces[i]) > width(pieces[size_t(longest[size_t(r)])])) longest[size_t(r)] = int(i);
            }
            std::vector<Piece> before = pieces;
            for (size_t i = 0; i < pieces.size(); i++) {
                const Piece& big = before[size_t(longest[size_t(root(int(i)))])];
                if (overhead(pieces[i]) && width(pieces[i]) < width(big) * 0.25 &&
                    (pieces[i].rate[0] != big.rate[0] || pieces[i].rate[1] != big.rate[1])) {
                    pieces[i].rate[0] = big.rate[0];
                    pieces[i].rate[1] = big.rate[1];
                    drift_fixed++;
                }
            }
        }
        if (drift_fixed) {
            char line[120];
            snprintf(line, sizeof(line), "start: %d texture rates of structures' undersides straightened\n", drift_fixed);
            report += line;
        }
        // A structure over the road can cross it at an angle and end square to its own heading past the cut (Los
        // Angeles's overpass deck, 14 degrees off the road, goes on up to 70 ft behind the cut on one side). Swept
        // straight back from the cut, the copy kinked away from it and lay in the same plane as the stock deck over
        // that wedge, flickering through it. So a raised structure (pieces over 10 ft up, joined end to end: deck,
        // underside, fascia; with a floor among them) goes on along its own heading (from its fascia's facing) past where the stock one
        // stops, then turns onto the road's over 300 ft (drift_shift); and each end of each piece starts where the
        // stock surface under it stops: the planes behind the cut, every 2 ft out to 150, while one has a piece of
        // the same state through that end's point (within 1 ft).
        {
            int trimmed = 0, drifting = 0;
            double furthest = 0;
            std::vector<size_t> raised;
            for (size_t i = 0; i < pieces.size(); i++) {
                const Piece& pc = pieces[i];
                if (t == 8 && std::min(pc.e[0].y, pc.e[1].y) > s0[1] + 10) raised.push_back(i);   // Los Angeles only
            }
            // Structures: raised pieces joined end to end.
            std::vector<size_t> group(pieces.size());
            for (size_t i = 0; i < pieces.size(); i++) group[i] = i;
            std::function<size_t(size_t)> top = [&](size_t i) { return group[i] == i ? i : group[i] = top(group[i]); };
            for (size_t i : raised) for (size_t j : raised) {
                bool joined = false;
                for (const ProfileEnd& a : pieces[i].e) for (const ProfileEnd& b : pieces[j].e) joined |= std::hypot(a.l - b.l, a.y - b.y) < 0.3;
                if (joined) group[top(i)] = top(j);
            }
            // A structure's heading from its upright faces: a face along w = cos f + sin n faces -sin f + cos n.
            std::map<size_t, std::vector<double>> slopes;
            for (size_t i : raised) {
                const Piece& pc = pieces[i];
                double mf = dot(pc.normal, f), mn = dot(pc.normal, n);
                if (std::fabs(pc.normal[1]) > 0.3 || std::fabs(mn) < 0.5 || std::fabs(pc.e[1].y - pc.e[0].y) < 1) continue;
                slopes[top(i)].push_back(-mf / mn);
            }
            std::set<size_t> decks;   // a structure needs a floor (a lone raised wall is a bank's hedge)
            for (size_t i : raised) if (std::fabs(pieces[i].normal[1]) > 0.7) decks.insert(top(i));
            for (auto& [g, v] : slopes) {
                if (!decks.contains(g)) continue;
                std::sort(v.begin(), v.end());
                double drift = v[v.size() / 2];
                if (std::fabs(drift) < 0.03) continue;
                drifting++;
                double l0 = 1e30, l1 = -1e30, y0 = 1e30, y1 = -1e30;
                int count = 0;
                for (size_t i : raised) {
                    if (top(i) != g) continue;
                    pieces[i].drift = drift;
                    pieces[i].bare = true;   // overhead, out of reach (its collision would stay on the road's line)
                    count++;
                    for (const ProfileEnd& e : pieces[i].e) { l0 = std::min(l0, e.l); l1 = std::max(l1, e.l); y0 = std::min(y0, e.y); y1 = std::max(y1, e.y); }
                }
                char line[160];
                snprintf(line, sizeof(line), "start: structure at an angle: %d pieces, across %.0f..%.0f, up %.0f..%.0f, %.3f ft across a ft\n", count, l0,
                         l1, y0 - s0[1], y1 - s0[1], drift);
                report += line;
            }
            if (!raised.empty()) {
                std::vector<std::array<bool, 2>> open(pieces.size(), { true, true });
                for (double d = 2; d <= 150; d += 2) {
                    std::vector<Piece> behind = section(cut - d);
                    bool any = false;
                    for (size_t i : raised) {
                        for (int k = 0; k < 2; k++) {
                            if (!open[i][k]) continue;
                            ProfileEnd e = pieces[i].e[k];
                            e.l -= pieces[i].drift * d;   // the stock structure goes straight on its heading
                            bool covered = false;
                            for (const Piece& b : behind) {
                                if (b.state != pieces[i].state) continue;
                                double lo = std::min(b.e[0].l, b.e[1].l), hi = std::max(b.e[0].l, b.e[1].l);
                                if (e.l < lo - 1 || e.l > hi + 1) continue;
                                double u = hi - lo < 1e-6 ? 0.5 : std::clamp((e.l - b.e[0].l) / (b.e[1].l - b.e[0].l), 0.0, 1.0);
                                if (std::fabs(b.e[0].y + (b.e[1].y - b.e[0].y) * u - e.y) < 1) { covered = true; break; }
                            }
                            if (covered) {
                                pieces[i].skip[k] = d;
                                any = true;
                            } else {
                                open[i][k] = false;
                            }
                        }
                    }
                    if (!any) break;
                }
                std::map<size_t, double> hold;
                for (size_t i : raised) hold[top(i)] = std::max({ hold[top(i)], pieces[i].skip[0], pieces[i].skip[1] });
                for (size_t i : raised) {
                    pieces[i].hold = hold[top(i)];
                    if (pieces[i].skip[0] > 0 || pieces[i].skip[1] > 0) trimmed++;
                    furthest = std::max({ furthest, pieces[i].skip[0], pieces[i].skip[1] });
                }
            }
            if (trimmed || drifting) {
                char line[160];
                snprintf(line, sizeof(line),
                         "start: %d pieces of structures start where the stock ones end (up to %.0f ft behind the cut), %d at an angle to the road\n",
                         trimmed, furthest, drifting);
                report += line;
            }
        }
        if (getenv("SCENERY_PIECES")) {
            for (const Piece& pc : pieces) {
                char line[300];
                snprintf(line, sizeof(line), "  piece state %d: (%.1f, %.1f) - (%.1f, %.1f) st (%.0f,%.0f)-(%.0f,%.0f) rate %.2f %.2f n (%.2f %.2f %.2f) period %.0f %.0f\n",
                         pc.state, pc.e[0].l, pc.e[0].y - s0[1], pc.e[1].l, pc.e[1].y - s0[1], pc.e[0].st[0], pc.e[0].st[1], pc.e[1].st[0],
                         pc.e[1].st[1], pc.rate[0], pc.rate[1], dot(pc.normal, f), pc.normal[1], dot(pc.normal, n),
                         scene.states[size_t(pc.state)].period[0], scene.states[size_t(pc.state)].period[1]);
                report += line;
            }
        }

        // How far back California Speed built behind the road's end (within the reach, near its height): the sweep
        // goes straight on over it, with the same texture mapping, before it bends. Only what goes on from the cut
        // counts, up to the first 40 ft gap: other parts of the track pass behind some starts.
        double built = 0;
        {
            std::vector<uint8_t> bins(70, 0);   // 10 ft bins behind the cut
            for (const Tri& tr : scene.tris) {
                double b0 = 1e30, b1 = -1e30;
                bool in = false;
                for (const Vtx& v : tr.v) {
                    double back = cut - along(v.p);
                    b0 = std::min(b0, back);
                    b1 = std::max(b1, back);
                    in |= std::fabs(across(v.p)) < cfg.reach && std::fabs(v.p[1] - s0[1]) < 150;
                }
                if (!in || b1 <= 0) continue;
                for (int k = std::max(0, int(b0 / 10)); k <= std::min(69, int(b1 / 10)); k++) bins[size_t(k)] = 1;
            }
            int gap = 0;
            for (int k = 0; k < 70; k++) {
                if (bins[size_t(k)]) { built = (k + 1) * 10.0; gap = 0; }
                else if (++gap >= 4) break;
            }
        }
        double straight = std::max(barricade + 40.0, built + 20.0);
        V3 c0 = add(s0, mul(f, cut));
        c0[1] = 0;

        // The path for a turn of `degrees` (on a radius of `radius` ft, eased in and out over 120 ft) after the
        // straight part, with the heights `rise`.
        using Rise = std::vector<std::pair<double, double>>;
        auto make_samples = [&](double degrees, double radius, const Rise& rise) {
            double total = straight + rise.back().first;
            std::vector<Hermite> keys = { { 0, 0, -grade }, { straight, -grade * straight, -grade } };
            for (size_t i = 1; i < rise.size(); i++) {
                keys.push_back({ straight + rise[i].first, -grade * straight + rise[i].second, 0 });
            }
            for (size_t i = 2; i + 1 < keys.size(); i++) keys[i].slope = (keys[i + 1].h - keys[i - 1].h) / (keys[i + 1].s - keys[i - 1].s);
            double turn = degrees * 3.14159265358979 / 180.0;
            double kmax = 1.0 / radius, ramp = 120.0;
            double plateau = std::max(0.0, std::fabs(turn) / kmax - ramp);
            if (std::fabs(turn) < kmax * ramp) { kmax = std::fabs(turn) / ramp; plateau = 0; }
            double sign = turn >= 0 ? 1 : -1;
            auto curvature = [&](double s) {
                double u = s - straight;
                if (u <= 0) return 0.0;
                if (u < ramp) return sign * kmax * u / ramp;
                if (u < ramp + plateau) return sign * kmax;
                if (u < 2 * ramp + plateau) return sign * kmax * (2 * ramp + plateau - u) / ramp;
                return 0.0;
            };
            std::vector<Sample> out;
            V3 c = c0;
            double th = 0, next = 0;
            for (double s = 0; s <= total + 0.5; s += 1.0) {
                V3 tt = add(mul(f, -std::cos(th)), mul(n, std::sin(th)));
                V3 nn = add(mul(n, std::cos(th)), mul(f, std::sin(th)));
                double step = curvature(s) != 0 || (s > straight - 40 && s < straight + 40) ? 12.0 : s > straight + 1500 ? 120.0 : 30.0;
                bool brk = std::fabs(s - barricade) < 0.5 || std::fabs(s - barricade - 15) < 0.5 || std::fabs(s - straight) < 0.5;
                if (s >= next || brk || s + 1 > total + 0.5) {
                    out.push_back({ s, c, tt, nn, th, eval_height(keys, s) });
                    next = s + step;
                }
                c = add(c, tt);
                th += curvature(s + 0.5);
            }
            return out;
        };
        auto place = [&](const Sample& sm, double l, double y) {
            V3 p = add(add(sm.c, mul(sm.n, l)), V3{ 0, y + sm.h, 0 });
            return p;
        };

        // Which way it bends: of the turns tried, the one that stays clear of the rest of the track (triangles in its
        // path past what was built behind the start) and hides its end best from the grid (sight lines from where the
        // cars and the chase camera stand to the end's cross-section, blocked by the track or the sweep itself).
        std::vector<double> turns = { cfg.turn, -cfg.turn, cfg.turn * 1.3, -cfg.turn * 1.3, cfg.turn * 0.65, -cfg.turn * 0.65 };
        if (cfg.fixed) turns.resize(1);
        std::vector<V3> centroids;
        std::vector<const Tri*> near_tris;
        for (const Tri& tr : scene.tris) {
            V3 c = mul(add(add(tr.v[0].p, tr.v[1].p), tr.v[2].p), 1.0 / 3);
            if (std::hypot(c[0] - c0[0], c[2] - c0[2]) > straight + cfg.rise.back().first + cfg.reach + 300) continue;
            centroids.push_back(c);
            near_tris.push_back(&tr);
        }
        std::vector<V3> eyes;
        for (double a : { 20.0, 60.0 }) for (double l : { -12.0, 0.0, 12.0 }) for (double up : { 5.0, 14.0 }) {
            eyes.push_back(add(add(s0, mul(f, a)), add(mul(n, l), V3{ 0, grade * a + up, 0 })));
        }
        auto ray_hit = [](const V3& o, const V3& d, double max_t, const V3& a, const V3& b, const V3& c) {
            V3 e1 = sub(b, a), e2 = sub(c, a), pv = cross(d, e2);
            double det = dot(e1, pv);
            if (std::fabs(det) < 1e-9) return false;
            double inv = 1 / det;
            V3 tv = sub(o, a);
            double u = dot(tv, pv) * inv;
            if (u < 0 || u > 1) return false;
            V3 qv = cross(tv, e1);
            double v = dot(d, qv) * inv;
            if (v < 0 || u + v > 1) return false;
            double t = dot(e2, qv) * inv;
            return t > 0.01 && t < max_t;
        };
        // Overlap and sight lines of a plan.
        auto judge = [&](const std::vector<Sample>& sm, int& overlap, int& seen) {
            // Overlap: track triangles inside the swept corridor past the straight part.
            overlap = 0;
            for (const V3& c : centroids) {
                for (const Sample& x : sm) {
                    if (x.s < straight) continue;
                    V3 d = sub(c, x.c);
                    double side = dot(d, x.n), ahead = dot(d, x.t);
                    double hy = c[1] - (s0[1] + x.h);
                    if (std::fabs(ahead) <= 15 && std::fabs(side) < cfg.reach && hy > -80 && hy < 250) { overlap++; break; }
                }
            }
            // Sight lines to the end's cross-section.
            std::vector<std::array<V3, 3>> occluders;
            for (const Tri* tr : near_tris) occluders.push_back({ tr->v[0].p, tr->v[1].p, tr->v[2].p });
            for (size_t i = 0; i + 1 < sm.size(); i++) {
                for (const Piece& pc : pieces) {
                    V3 a0 = place(sm[i], pc.e[0].l, pc.e[0].y), a1 = place(sm[i], pc.e[1].l, pc.e[1].y);
                    V3 b0 = place(sm[i + 1], pc.e[0].l, pc.e[0].y), b1 = place(sm[i + 1], pc.e[1].l, pc.e[1].y);
                    occluders.push_back({ a0, a1, b1 });
                    occluders.push_back({ a0, b1, b0 });
                }
            }
            std::vector<V3> targets;
            for (const Piece& pc : pieces) for (const ProfileEnd& e : pc.e) {
                V3 p = place(sm.back(), e.l, e.y);
                p = add(p, mul(sm.back().t, -1.0));     // just this side of the end
                p[1] += 1.0;
                targets.push_back(p);
            }
            seen = 0;
            for (const V3& eye : eyes) {
                for (const V3& target : targets) {
                    V3 d = sub(target, eye);
                    double dist = len(d);
                    d = mul(d, 1 / dist);
                    bool blocked = false;
                    for (const auto& o : occluders) {
                        if (ray_hit(eye, d, dist - 2, o[0], o[1], o[2])) { blocked = true; break; }
                    }
                    if (!blocked) seen++;
                }
            }
        };
        double best_score = 1e30, best_turn = cfg.turn;
        int best_overlap = 0, best_seen = 0;
        for (double degrees : turns) {
            int overlap, seen;
            judge(make_samples(degrees, cfg.radius, cfg.rise), overlap, seen);
            double score = seen * 10.0 + overlap;
            if (score < best_score) { best_score = score; best_turn = degrees; best_overlap = overlap; best_seen = seen; }
        }
        std::vector<Sample> samples = make_samples(best_turn, cfg.radius, cfg.rise);
        const char* kind = "bend";
        // Nothing hides its end (a bridge over open water, a plain): the road goes on gently instead, out past the
        // farthest the game draws (8,000 ft with Draw Distance at 4x), into the fog.
        if (best_seen > 0) {
            Rise far = { { 0, 0 }, { 2500, 6 }, { 9000, -20 } };
            double degrees = best_turn >= 0 ? 15 : -15;
            int overlap, seen;
            judge(make_samples(degrees, 4000, far), overlap, seen);
            if (overlap <= best_overlap) {
                samples = make_samples(degrees, 4000, far);
                best_turn = degrees;
                best_overlap = overlap;
                best_seen = seen;
                kind = "long road";
            }
        }
        {
            char line[200];
            snprintf(line, sizeof(line), "start: %s, turn %.0f deg, %d track triangles in its way, %d sight lines to its end\n", kind,
                     best_turn, best_overlap, best_seen);
            report += line;
        }

        // Collision surfaces of each piece, sampled at the cut.
        std::vector<int> piece_flags(pieces.size(), -1);
        std::vector<uint16_t> piece_info(pieces.size(), 0);
        for (size_t i = 0; i < pieces.size(); i++) {
            const Piece& pc = pieces[i];
            double ml = (pc.e[0].l + pc.e[1].l) / 2, my = (pc.e[0].y + pc.e[1].y) / 2;
            bool flat = std::fabs(pc.e[0].y - pc.e[1].y) < std::fabs(pc.e[0].l - pc.e[1].l);
            V3 at = add(add(s0, mul(f, cut + 1.5)), mul(n, ml));
            uint16_t fl, in;
            double gh;
            if (flat && ground_at(col, at[0], my, at[2], 3.0, fl, in, gh) && std::fabs(gh - my) < 3.0) {
                piece_flags[i] = fl & ~0x3000;
                piece_info[i] = in;
            }
            else if (!flat && std::fabs(ml) < 120 && std::fabs(pc.e[0].l - pc.e[1].l) < 40) {
                piece_flags[i] = 5;
            }
        }

        // The road across: the road pieces (type 0, flat, within 8 ft of the start's height and 80 ft of the spine)
        // through the spine, a gap of up to 25 ft between them taken in (a median: Santa Cruz's spine runs on the
        // dirt between carriageways 5 ft apart in height).
        double road0 = 0, road1 = 0;
        std::vector<std::pair<double, double>> road_runs;   // across road0..road1: carriageways and the medians between
        {
            std::vector<std::pair<double, double>> road;
            for (size_t i = 0; i < pieces.size(); i++) {
                const Piece& pc = pieces[i];
                double lo = std::min(pc.e[0].l, pc.e[1].l), hi = std::max(pc.e[0].l, pc.e[1].l);
                bool flat = std::fabs(pc.e[0].y - pc.e[1].y) < (hi - lo);
                if (piece_flags[i] >= 0 && (piece_flags[i] & 0xF) == 0 && flat && std::fabs(pc.e[0].y - s0[1]) < 8 && hi > -80 &&
                    lo < 80) {
                    road.push_back({ lo, hi });
                }
            }
            std::sort(road.begin(), road.end());
            road0 = road1 = 0;
            bool grown = true;
            while (grown) {
                grown = false;
                for (const auto& [a, b] : road) {
                    if (a <= road1 + 25.0 && b > road1) { road1 = b; grown = true; }
                    if (b >= road0 - 25.0 && a < road0) { road0 = a; grown = true; }
                }
            }
            if (road1 - road0 < 12) { road0 = -20; road1 = 20; }
            std::vector<std::pair<double, double>> lanes;
            for (const auto& [a, b] : road) {
                if (b <= road0 || a >= road1) continue;
                double lo = std::max(a, road0), hi = std::min(b, road1);
                if (!lanes.empty() && lo <= lanes.back().second + 1.0) lanes.back().second = std::max(lanes.back().second, hi);
                else lanes.push_back({ lo, hi });
            }
            double at = road0;
            for (const auto& [a, b] : lanes) {
                if (a > at + 1.0) road_runs.push_back({ at, a });
                road_runs.push_back({ std::max(a, at), b });
                at = std::max(at, b);
            }
            if (at < road1 - 1.0) road_runs.push_back({ at, road1 });
            if (road_runs.empty()) road_runs.push_back({ road0, road1 });
            if (road_runs.size() > 1) {
                std::string line = "start: road runs";
                char part[48];
                for (const auto& [a, b] : road_runs) { snprintf(part, sizeof(part), " %.1f..%.1f", a, b); line += part; }
                report += line + "\n";
            }
        // Los Angeles' left edge (- side: its barrier 21 ft out and the hedge behind it): the stock road goes on up to
        // 55 ft behind the cut, narrowing (its barrier comes in to 14 ft out, the hedge to 18), so swept straight from
        // the cut the copies stood beside the stock ones: two rails. Each piece of that edge starts where its stock
        // copy ends (followed plane by plane behind the cut, every 2 ft: a piece of the same state over the same heights
        // within 3 ft across of the last), from there, and turns back out to its own line over 30 ft (drift_shift).
        if (t == 8) {
            std::vector<size_t> edge;
            for (size_t i = 0; i < pieces.size(); i++) {
                const Piece& pc = pieces[i];
                if (pc.lead > 0 || pc.bare) continue;
                double lo = std::min(pc.e[0].l, pc.e[1].l), hi = std::max(pc.e[0].l, pc.e[1].l);
                if (hi <= road0 + 1 && lo >= road0 - 25 && std::max(pc.e[0].y, pc.e[1].y) < s0[1] + 40) edge.push_back(i);
            }
            std::vector<double> at(pieces.size(), 0), end(pieces.size(), 0);
            std::vector<std::vector<double>> shift(pieces.size());
            for (size_t i : edge) at[i] = (pieces[i].e[0].l + pieces[i].e[1].l) / 2;
            std::vector<bool> open(pieces.size(), true);
            for (double d = 2; d <= 120; d += 2) {
                std::vector<Piece> behind = section(cut - d);
                bool any = false;
                for (size_t i : edge) {
                    if (!open[i]) continue;
                    const Piece& pc = pieces[i];
                    double ylo = std::min(pc.e[0].y, pc.e[1].y), yhi = std::max(pc.e[0].y, pc.e[1].y);
                    const Piece* found = nullptr;
                    for (const Piece& b : behind) {
                        if (b.state != pc.state) continue;
                        double bl = (b.e[0].l + b.e[1].l) / 2;
                        double blo = std::min(b.e[0].y, b.e[1].y), bhi = std::max(b.e[0].y, b.e[1].y);
                        if (std::fabs(bl - at[i]) > 3 || bhi < ylo - 0.5 || blo > yhi + 0.5) continue;   // overlapping heights (split differently)
                        if (!found || std::fabs(bl - at[i]) < std::fabs((found->e[0].l + found->e[1].l) / 2 - at[i])) found = &b;
                    }
                    if (!found) { open[i] = false; continue; }
                    at[i] = (found->e[0].l + found->e[1].l) / 2;
                    end[i] = d;
                    shift[i].push_back(at[i] - (pc.e[0].l + pc.e[1].l) / 2);
                    any = true;
                }
                if (!any) break;
            }
            std::string notes;
            for (size_t i : edge) {
                char line[120];
                snprintf(line, sizeof(line), " state %d (%.1f, %.1f)-(%.1f, %.1f) %.0f ft, %.1f ft across;", pieces[i].state, pieces[i].e[0].l,
                         pieces[i].e[0].y - s0[1], pieces[i].e[1].l, pieces[i].e[1].y - s0[1], end[i], shift[i].empty() ? 0.0 : shift[i].back());
                notes += line;
                if (end[i] <= 0) continue;
                pieces[i].pull_d = end[i];
                pieces[i].pull_dl = shift[i].back();
                pieces[i].skip[0] = pieces[i].skip[1] = end[i];
            }
            if (!notes.empty()) report += "start: the - side edge starts where the stock edge ends:" + notes + "\n";
        }
        }

        // The track's own collision walls (any type over 2) where they cross the plane `at` ft along: ends across and
        // height, surface and facing. California Speed walls a road with invisible walls far taller than what is drawn
        // there (Monterey's stone wall is 3 ft, its wall 88 ft).
        struct WallCross { double l0, y0, l1, y1; uint16_t flags, info; V3 front; };
        auto track_walls_at = [&](double at) {
            std::vector<WallCross> out;
            for (uint32_t pi = 0; pi < col.polys; pi++) {
                size_t rec = col.poly0 + size_t(pi) * 0x18;
                if ((col.d[rec + 1] & 0xF) <= 2) continue;
                uint16_t fl, in;
                std::vector<V3> w = col.polygon(pi, &fl, &in);
                if (w.size() < 3) continue;
                double ends_l[2], ends_y[2];
                int found = 0;
                for (size_t k = 0; k < w.size() && found < 2; k++) {
                    const V3& a = w[k];
                    const V3& b = w[(k + 1) % w.size()];
                    double da = along(a) - at, db = along(b) - at;
                    if ((da < 0) == (db < 0) || da == db) continue;
                    V3 p = add(a, mul(sub(b, a), da / (da - db)));
                    ends_l[found] = across(p);
                    ends_y[found] = p[1];
                    found++;
                }
                if (found < 2) continue;
                if (std::hypot(ends_l[0] - ends_l[1], ends_y[0] - ends_y[1]) < 0.5) continue;
                if (std::fabs(ends_l[0]) > cfg.reach && std::fabs(ends_l[1]) > cfg.reach) continue;
                V3 front{ s16(col.d, rec + 10) / 16384.0, s16(col.d, rec + 12) / 16384.0, s16(col.d, rec + 14) / 16384.0 };
                out.push_back({ ends_l[0], ends_y[0], ends_l[1], ends_y[1], fl, in, front });
            }
            return out;
        };

        // The authored data at the cut past the cross-section's 260 ft on one side (sg), out to `lim`, is swept too
        // where it matters: Mojave Desert's + side slope runs on to 379 ft (so its canyon walls hang from the authored
        // edge), and Central Valley's tree wall stands 279 ft out on the + side, 77 ft tall, all along the road (the
        // reach cut it off at the cut and it stopped there). No collision when `bare` or behind a track wall on that
        // side (out of reach).
        auto past_reach = [&](double sg, double lim, bool bare) {
            if (!bare) {
                for (const WallCross& w : track_walls_at(cut + 3)) {
                    if ((w.l0 + w.l1) * sg <= 0) continue;
                    if (std::max(sg * w.l0, sg * w.l1) <= cfg.reach && std::max(w.y0, w.y1) > s0[1] + 10) bare = true;
                }
            }
            int count = 0;
            for (Piece q : section(cut, lim)) {
                if (sg * q.e[0].l > sg * q.e[1].l) std::swap(q.e[0], q.e[1]);   // inner end first
                double a = sg * q.e[0].l, b = sg * q.e[1].l;
                if (b <= cfg.reach + 0.01) continue;
                if (a < cfg.reach) {
                    double u = (sg * cfg.reach - q.e[0].l) / (q.e[1].l - q.e[0].l);
                    ProfileEnd e;
                    e.l = sg * cfg.reach;
                    e.y = q.e[0].y + (q.e[1].y - q.e[0].y) * u;
                    for (int x = 0; x < 2; x++) e.st[x] = q.e[0].st[x] + (q.e[1].st[x] - q.e[0].st[x]) * u;
                    for (int x = 0; x < 4; x++) e.c[x] = q.e[0].c[x] + (q.e[1].c[x] - q.e[0].c[x]) * u;
                    q.e[0] = e;
                }
                q.bare = bare;
                pieces.push_back(q);
                piece_flags.push_back(-1);
                piece_info.push_back(0);
                count++;
            }
            return count;
        };
        if (t == 4) {
            char line[100];
            snprintf(line, sizeof(line), "start: %d pieces past the reach on the + side swept on\n", past_reach(1, 600, false));
            report += line;
        }

        // Mojave Desert's canyon walls stand off the road from 600 ft ahead, past the cross-section's reach: each side's
        // profile out to 1,500 ft from the road is taken at a plane where its wall is whole and runs on unbroken (+ side
        // 1,500 ft ahead, rising to 311 ft; - side 2,100 ft, a ridge to 129 ft and a face to 259 ft behind it; the valley
        // between is below the near crest's line from the road), across and height from the spine there (the road winds
        // and climbs), from where the swept ground ends on that side (no gap) and lifted or lowered to meet that ground's
        // edge, then swept back from the cut with the rest, and on straight past the sweep's end for 4,000 ft (the crest
        // hides the road's end, not the end of a 300 ft wall). The authored ground past the 260 ft reach at the cut is
        // swept too, so the walls hang from the authored data's own edge at the cut, and they rise out of it: flat at
        // that edge's height at the cut, full height 600 ft back (a wall standing at the cut, or carried ahead of it,
        // had an open end beside authored land that has nothing there, and the sky showed around it). Behind the
        // corridor's own walls: no collision.
        // Mojave's cutouts' bottom rows (a point of it, its along where the strip ends, its side), for the land under them.
        struct CutFoot { V3 p; double from; double sg; int state; double st[2]; uint8_t c[4]; };
        std::vector<CutFoot> cut_feet;
        if (t == 11) {
            struct Side { double plane; double sign; };
            int added = 0, outer = 0;
            outer = past_reach(-1, 1500, true);
            // The + side is the authored slope up to the canyon's rim, one quad whose back edge lies on the cut (from
            // 241 ft out, 6 ft up, to 443 ft out, 17 ft up, within 1.5 ft along of the cut): it goes on from that edge
            // itself, each end on the edge's own vertex (place, texture and color), so the two share their seam.
            // (Canyon walls hung from it, from a plane 1,500 ft ahead, made a bump and a break against it.)
            {
                // The cut's own piece of it (to the 260 ft reach) gives way to the whole edge, which keeps its surface.
                int flags = -1;
                uint16_t info = 0;
                for (size_t i = 0; i < pieces.size();) {
                    const Piece& pc = pieces[i];
                    if (pc.lead == 0 && std::min(pc.e[0].l, pc.e[1].l) > 239 && std::fabs(pc.normal[1]) > 0.5) {
                        flags = piece_flags[i];
                        info = piece_info[i];
                        pieces.erase(pieces.begin() + i);
                        piece_flags.erase(piece_flags.begin() + i);
                        piece_info.erase(piece_info.begin() + i);
                    } else {
                        i++;
                    }
                }
                std::vector<Piece> slope;
                for (Piece pc : section(cut + 3, 600)) {
                    if (std::min(pc.e[0].l, pc.e[1].l) < 239 || std::fabs(pc.normal[1]) < 0.5) continue;
                    slope.push_back(pc);
                }
                for (Piece& pc : slope) {
                    for (int k = 0; k < 2; k++) {
                        ProfileEnd& e = pc.e[k];
                        const Vtx* best = nullptr;
                        double best_d = 1e30;
                        for (const Tri& tr : scene.tris) {
                            if (tr.state != pc.state) continue;
                            for (const Vtx& v : tr.v) {
                                double da = along(v.p) - cut;
                                if (std::fabs(da) > 4) continue;
                                double d = std::hypot(across(v.p) - e.l, v.p[1] - e.y);
                                if (d < 3 && d < best_d) { best = &v; best_d = d; }
                            }
                        }
                        if (!best) continue;
                        double sv = cut - along(best->p);   // its distance back from the cut (ahead: negative)
                        e.l = across(best->p);
                        e.y = best->p[1];
                        for (int x = 0; x < 2; x++) e.st[x] = best->st[x] - pc.rate[x] * sv;
                        for (int x = 0; x < 4; x++) e.c[x] = best->c[x];
                        pc.skip[k] = sv;
                    }
                    if (std::hypot(pc.e[1].l - pc.e[0].l, pc.e[1].y - pc.e[0].y) < 0.5) continue;   // slivers at its corners
                    pc.run_on = true;
                    if (getenv("SCENERY_PIECES")) {
                        char line[200];
                        snprintf(line, sizeof(line), "  slope state %d: (%.1f, %.1f) - (%.1f, %.1f) from %.1f, %.1f ft back\n", pc.state, pc.e[0].l,
                                 pc.e[0].y - s0[1], pc.e[1].l, pc.e[1].y - s0[1], pc.skip[0], pc.skip[1]);
                        report += line;
                    }
                    pieces.push_back(pc);
                    piece_flags.push_back(flags);
                    piece_info.push_back(info);
                    outer++;
                }
            }
            for (Side sd : { Side{ 2100, -1 } }) {
                const V3* sp = nullptr;
                for (const V3& q : spine) {
                    if (along(q) >= sd.plane) { sp = &q; break; }
                }
                if (!sp) continue;
                double sl = across(*sp), sy = (*sp)[1] - s0[1];
                // From where the swept ground ends on this side (260 ft out, the reach, or short of it: the - side's
                // at 241 ft), at its height there, so the two meet with no gap or step.
                double from = 0, edge = s0[1];
                for (const Piece& pc : pieces) {
                    if (pc.lead > 0) continue;
                    for (const ProfileEnd& e : pc.e) if (sd.sign * e.l > from) { from = sd.sign * e.l; edge = e.y; }
                }
                constexpr double to = 1500;
                // The far part of the profile there, relative to the road, clipped to from..to on this side.
                auto clip_at = [](const Piece& pc, double l) {
                    double u = (l - pc.e[0].l) / (pc.e[1].l - pc.e[0].l);
                    ProfileEnd e;
                    e.l = l;
                    e.y = pc.e[0].y + (pc.e[1].y - pc.e[0].y) * u;
                    for (int x = 0; x < 2; x++) e.st[x] = pc.e[0].st[x] + (pc.e[1].st[x] - pc.e[0].st[x]) * u;
                    for (int x = 0; x < 4; x++) e.c[x] = pc.e[0].c[x] + (pc.e[1].c[x] - pc.e[0].c[x]) * u;
                    return e;
                };
                std::vector<Piece> wall;
                for (Piece pc : section(sd.plane, std::fabs(sl) + to + 100)) {
                    for (ProfileEnd& e : pc.e) { e.l = (e.l - sl) * sd.sign; e.y -= sy; }   // l: out from the road on this side
                    if (pc.e[0].l > pc.e[1].l) std::swap(pc.e[0], pc.e[1]);
                    if (pc.e[1].l <= from || pc.e[0].l >= to) continue;
                    if (pc.e[0].l < from) pc.e[0] = clip_at(pc, from);
                    if (pc.e[1].l > to) pc.e[1] = clip_at(pc, to);
                    wall.push_back(pc);
                }
                // Meet the swept ground's edge.
                double join = 1e30;
                for (const Piece& pc : wall) if (std::fabs(pc.e[0].l - from) < 0.01) join = std::min(join, pc.e[0].y);
                if (join > 1e29) continue;
                // One unbroken profile from there outward: each piece starting where the last ended (going out, or up
                // a face). Where the plane's profile breaks (the - side's climb to 133 ft at 393 ft out, then a valley
                // from 124 ft under the road at 517 ft: the sky showed through between), it goes straight on to the
                // nearest piece out that starts no more than 30 ft lower, and the valley is left out.
                for (Piece& pc : wall) {
                    if (std::fabs(pc.e[1].l - pc.e[0].l) < 1 && pc.e[1].y < pc.e[0].y) std::swap(pc.e[0], pc.e[1]);   // faces go up
                }
                std::vector<Piece> chain;
                {
                    double at_l = from, at_y = join;
                    for (size_t steps = 0; steps < wall.size() * 2; steps++) {
                        const Piece* next = nullptr;
                        for (const Piece& pc : wall) {
                            if (std::fabs(pc.e[0].l - at_l) > 1 || std::fabs(pc.e[0].y - at_y) > 1) continue;
                            if (pc.e[1].l < at_l + 0.1 && pc.e[1].y < at_y + 0.1) continue;   // each step goes out or up
                            if (!next || pc.e[1].l > next->e[1].l + 0.1 || (pc.e[1].l > next->e[1].l - 0.1 && pc.e[1].y > next->e[1].y)) next = &pc;
                        }
                        if (!next && !chain.empty()) {
                            const Piece* across = nullptr;
                            for (const Piece& pc : wall) {
                                if (pc.e[0].l < at_l + 1 || pc.e[0].y < at_y - 30) continue;
                                if (!across || pc.e[0].l < across->e[0].l) across = &pc;
                            }
                            if (across) {
                                Piece bridge = chain.back();
                                bridge.e[0] = chain.back().e[1];
                                bridge.e[1] = across->e[0];
                                chain.push_back(bridge);
                                next = across;
                            }
                        }
                        if (!next) break;
                        chain.push_back(*next);
                        at_l = next->e[1].l;
                        at_y = next->e[1].y;
                    }
                }
                wall = chain;
                // On the inside of the bend nothing may reach past its radius (it would fold over itself there: the +
                // side's face at 743 ft twisted into a ribbon with the sky under it; squeezed in, it stood as a spike).
                // That side's profile ends at 3/4 of the radius (Mojave's + side: its slope, up to 50 ft at 450 ft).
                {
                    double inward = dot(sub(samples.back().c, samples[0].c), n) * sd.sign;
                    double far_l = from;
                    for (const Piece& pc : wall) for (const ProfileEnd& e : pc.e) far_l = std::max(far_l, e.l);
                    double limit = 0.75 * cfg.radius;
                    if (inward > 0 && far_l > limit && limit > from) {
                        std::vector<Piece> kept;
                        for (Piece pc : wall) {
                            if (pc.e[0].l >= limit) continue;
                            if (pc.e[1].l > limit) pc.e[1] = clip_at(pc, limit);
                            kept.push_back(pc);
                        }
                        wall = kept;
                    }
                }
                for (Piece& pc : wall) {
                    for (ProfileEnd& e : pc.e) { e.l *= sd.sign; e.y += edge - join; }
                    pc.bare = true;
                    pc.run_on = true;
                    // Out of the flat ground's edge at the cut (nothing stands there beside the authored land ahead, which
                    // ends at that edge) to the canyon's full height 600 ft back.
                    pc.rise_len = 600;
                    pc.rise_base = edge;
                    if (getenv("SCENERY_PIECES")) {
                        char line[200];
                        snprintf(line, sizeof(line), "  canyon state %d: (%.1f, %.1f) - (%.1f, %.1f)\n", pc.state, pc.e[0].l, pc.e[0].y - s0[1],
                                 pc.e[1].l, pc.e[1].y - s0[1]);
                        report += line;
                    }
                    pieces.push_back(pc);
                    piece_flags.push_back(-1);
                    piece_info.push_back(0);
                    added++;
                }
            }
            char line[100];
            snprintf(line, sizeof(line), "start: %d pieces of authored ground past the reach and %d of the canyon walls swept on\n", outer,
                     added);
            report += line;

            // The distant canyon cutouts on either side (a wall strip of three rows of quads: the + side's from 600 ft
            // ahead, 1,030 ft out, 33 to 221 ft up; the - side's from 1,580 ft ahead, 1,550 ft out) end in a column of
            // vertices facing the start, and the sky showed past it. Each goes on from that column itself: straight
            // back along the road (parallel to the start's straight) to 4,000 ft behind spine 0, each row from its own
            // vertex (place, color, its texture going on at the rate it changes along the strip's last quads), so the
            // two share the column's edges. One model a 1,000 ft (`SCNXCUTnn`); no collision (far out of reach).
            int cut_models = 0;
            for (V3 seed : { V3{ 680, 1080, 130 }, V3{ 1650, -1550, 60 } }) {   // along, across, up from spine 0
                V3 at = add(add(add(s0, mul(f, seed[0])), mul(n, seed[1])), V3{ 0, seed[2], 0 });
                const Tri* near = nullptr;
                double near_d = 1e30;
                for (const Tri& tr : scene.tris) {
                    V3 m = mul(add(add(tr.v[0].p, tr.v[1].p), tr.v[2].p), 1.0 / 3);
                    double d = std::sqrt(dot(sub(m, at), sub(m, at)));
                    if (d < near_d) { near_d = d; near = &tr; }
                }
                if (!near || near_d > 300) continue;
                int state = near->state;
                // The strip: its state's triangles within 2,500 ft, and upright ones of other states sharing a corner
                // with them (the - side's rocky top, a second strip on another texture above the first).
                std::vector<const Tri*> strip;
                std::set<int> states = { state };
                auto close_to = [&](const Tri& tr) { V3 d = sub(tr.v[0].p, at); return std::sqrt(dot(d, d)) < 2500; };
                for (const Tri& tr : scene.tris) {
                    if (tr.state == state && close_to(tr)) strip.push_back(&tr);
                }
                for (const Tri& tr : scene.tris) {
                    if (states.contains(tr.state) || !close_to(tr)) continue;
                    V3 tn = cross(sub(tr.v[1].p, tr.v[0].p), sub(tr.v[2].p, tr.v[0].p));
                    double tl = std::sqrt(dot(tn, tn));
                    if (tl < 1e-6 || std::fabs(tn[1] / tl) > 0.5) continue;
                    bool touches = false;
                    for (const Tri* o : strip) {
                        if (o->state != state) continue;
                        for (const Vtx& a : tr.v) for (const Vtx& b : o->v) { V3 d = sub(a.p, b.p); touches |= dot(d, d) < 0.25; }
                        if (touches) break;
                    }
                    if (touches) states.insert(tr.state);
                }
                for (const Tri& tr : scene.tris) {
                    if (tr.state != state && states.contains(tr.state) && close_to(tr)) strip.push_back(&tr);
                }
                // Its vertices (merged within 0.5 ft) and edges; the end column: the vertices nearest the start
                // (along within 200 ft of the nearest) on edges used once.
                std::vector<Vtx> verts;
                auto index = [&](const Vtx& v) {
                    for (size_t i = 0; i < verts.size(); i++) {
                        V3 d = sub(verts[i].p, v.p);
                        if (dot(d, d) < 0.25) return int(i);
                    }
                    verts.push_back(v);
                    return int(verts.size() - 1);
                };
                std::map<std::pair<int, int>, int> uses;
                std::vector<std::array<int, 3>> faces;
                for (const Tri* tr : strip) {
                    std::array<int, 3> fi{ index(tr->v[0]), index(tr->v[1]), index(tr->v[2]) };
                    faces.push_back(fi);
                    for (int k = 0; k < 3; k++) uses[{ std::min(fi[k], fi[(k + 1) % 3]), std::max(fi[k], fi[(k + 1) % 3]) }]++;
                }
                double nearest = 1e30;
                for (const Vtx& v : verts) nearest = std::min(nearest, along(v.p));
                std::vector<int> column;
                for (const auto& [e, count] : uses) {
                    if (count != 1) continue;
                    for (int k : { e.first, e.second }) {
                        if (along(verts[size_t(k)].p) > nearest + 200) continue;
                        if (std::find(column.begin(), column.end(), k) == column.end()) column.push_back(k);
                    }
                }
                // Only the column's chain from its lowest vertex up through edges used once within it.
                std::sort(column.begin(), column.end(), [&](int x, int y) { return verts[size_t(x)].p[1] < verts[size_t(y)].p[1]; });
                std::vector<int> chain;
                if (!column.empty()) chain.push_back(column[0]);
                for (bool grew = true; grew;) {
                    grew = false;
                    for (int k : column) {
                        if (std::find(chain.begin(), chain.end(), k) != chain.end()) continue;
                        int last = chain.back();
                        auto it = uses.find({ std::min(last, k), std::max(last, k) });
                        if (it != uses.end() && it->second == 1 && verts[size_t(k)].p[1] > verts[size_t(last)].p[1]) {
                            chain.push_back(k);
                            grew = true;
                            break;
                        }
                    }
                }
                if (chain.size() < 2) continue;
                // The texture's rate along the strip, from the strip's bottom and top edges (edges used once leaving
                // the column's lowest and highest vertex, to the next column): only the axis that changes along them
                // goes on (the other holds, as along a row); rows between take the rate in proportion to height.
                auto edge_rate = [&](int k, double out[2]) {
                    out[0] = out[1] = 0;
                    const Vtx& v = verts[size_t(k)];
                    for (const auto& [e, count] : uses) {
                        if (count != 1 || (e.first != k && e.second != k)) continue;
                        int j = e.first == k ? e.second : e.first;
                        if (std::find(chain.begin(), chain.end(), j) != chain.end()) continue;
                        const Vtx& w = verts[size_t(j)];
                        V3 d = sub(v.p, w.p);
                        double len = std::sqrt(dot(d, d));
                        if (len < 1) continue;
                        for (int x = 0; x < 2; x++) out[x] = (v.st[x] - w.st[x]) / len;
                        return;
                    }
                };
                double low_rate[2], high_rate[2];
                edge_rate(chain.front(), low_rate);
                edge_rate(chain.back(), high_rate);
                int axis = std::fabs(low_rate[0]) + std::fabs(high_rate[0]) >= std::fabs(low_rate[1]) + std::fabs(high_rate[1]) ? 0 : 1;
                struct Row { Vtx v; double rate[2]; };
                std::vector<Row> rows;
                double y0 = verts[size_t(chain.front())].p[1], y1 = verts[size_t(chain.back())].p[1];
                for (int k : chain) {
                    const Vtx& v = verts[size_t(k)];
                    double u = y1 > y0 ? (v.p[1] - y0) / (y1 - y0) : 0;
                    Row r{ v, { 0, 0 } };
                    r.rate[axis] = low_rate[axis] + (high_rate[axis] - low_rate[axis]) * u;
                    rows.push_back(r);
                }
                // It faces the road. The game culls the side the cross product points to (Rush 2's world turns x the
                // other way), so that product points away from the road (the - side's strip, summed from its own
                // triangles, came out the other way and was never drawn).
                double side = across(rows[0].v.p) > 0 ? 1 : -1;
                V3 face = mul(n, side);
                // Each pair of rows takes its texture coordinates from the authored end triangle holding both its
                // vertices (the strip wraps its texture per triangle: a vertex's coordinates differ between the
                // triangles sharing it), and one rate along the strip: the top edge's (the skyline's scale).
                std::vector<std::array<std::array<double, 2>, 2>> seg_st(rows.size());
                std::vector<std::array<double, 2>> seg_rate(rows.size(), { 0, 0 });
                std::vector<int> seg_state(rows.size(), state);
                for (size_t r = 0; r + 1 < rows.size(); r++) {
                    seg_st[r] = { { { rows[r].v.st[0], rows[r].v.st[1] }, { rows[r + 1].v.st[0], rows[r + 1].v.st[1] } } };
                    seg_rate[r][axis] = high_rate[axis];
                    for (const Tri* tr : strip) {
                        int lo = -1, hi = -1;
                        for (int k = 0; k < 3; k++) {
                            V3 a = sub(tr->v[k].p, rows[r].v.p), b = sub(tr->v[k].p, rows[r + 1].v.p);
                            if (dot(a, a) < 0.25) lo = k;
                            if (dot(b, b) < 0.25) hi = k;
                        }
                        if (lo < 0 || hi < 0) continue;
                        seg_st[r] = { { { tr->v[lo].st[0], tr->v[lo].st[1] }, { tr->v[hi].st[0], tr->v[hi].st[1] } } };
                        seg_state[r] = tr->state;
                        // Its rate along the strip: from its third corner (in the next column) and the column corner at
                        // that corner's height, on the axis that changes most.
                        const Vtx& w = tr->v[3 - lo - hi];
                        int k = std::fabs(w.p[1] - tr->v[lo].p[1]) < std::fabs(w.p[1] - tr->v[hi].p[1]) ? lo : hi;
                        V3 d = sub(tr->v[k].p, w.p);
                        double len = std::sqrt(dot(d, d));
                        if (len > 1) {
                            double du[2] = { (tr->v[k].st[0] - w.st[0]) / len, (tr->v[k].st[1] - w.st[1]) / len };
                            int ax = std::fabs(du[0]) >= std::fabs(du[1]) ? 0 : 1;
                            seg_rate[r] = { 0, 0 };
                            seg_rate[r][ax] = du[ax];
                        }
                        break;
                    }
                }
                {
                    CutFoot cf{ rows[0].v.p, along(rows[0].v.p), side, seg_state[0], { seg_st[0][0][0], seg_st[0][0][1] }, {} };
                    for (int x = 0; x < 4; x++) cf.c[x] = rows[0].v.c[x];
                    cut_feet.push_back(cf);
                }
                V3 back = mul(f, -1);
                double from = nearest, to = -4000;
                for (int piece = 0; from > to; piece++) {
                    double step_end = std::max(to, from - 1000);
                    NewModel m;
                    char name[16];
                    snprintf(name, sizeof(name), "SCNXCUT%02d", cut_models++);
                    m.name = name;
                    V3 lo{ 1e30, 1e30, 1e30 }, hi{ -1e30, -1e30, -1e30 };
                    for (double d0 = from; d0 > step_end + 0.01; d0 -= 250) {
                        double d1 = std::max(step_end, d0 - 250);
                        for (size_t r = 0; r + 1 < rows.size(); r++) {
                            auto vtx = [&](const Row& row, double d, const double* base) {
                                // `d`: the along this vertex is brought to (the column's own along where it starts).
                                double go = std::max(0.0, along(row.v.p) - d);
                                OutVtx o;
                                o.p = add(row.v.p, mul(back, go));
                                for (int x = 0; x < 2; x++) o.st[x] = int32_t(std::lround(base[x] + seg_rate[r][x] * go));
                                for (int x = 0; x < 4; x++) o.c[x] = row.v.c[x];
                                return o;
                            };
                            OutQuad q;
                            q.state = seg_state[r];
                            q.bare = true;
                            double near_edge = piece == 0 && d0 == from ? 1e9 : d0;   // the first quad starts on the column itself
                            q.v[0] = vtx(rows[r], near_edge, seg_st[r][0].data());
                            q.v[1] = vtx(rows[r + 1], near_edge, seg_st[r][1].data());
                            q.v[2] = vtx(rows[r + 1], d1, seg_st[r][1].data());
                            q.v[3] = vtx(rows[r], d1, seg_st[r][0].data());
                            V3 qn = cross(sub(q.v[2].p, q.v[0].p), sub(q.v[3].p, q.v[1].p));
                            if (dot(qn, face) < 0) { std::swap(q.v[0], q.v[1]); std::swap(q.v[2], q.v[3]); }
                            const State& st = scene.states[size_t(seg_state[r])];
                            for (int x = 0; x < 2; x++) {
                                double per = st.period[x];
                                if (per <= 0) continue;
                                int32_t lo_st = std::min({ q.v[0].st[x], q.v[1].st[x], q.v[2].st[x], q.v[3].st[x] });
                                int32_t shift = int32_t(std::floor(lo_st / per) * per);
                                for (int k = 0; k < 4; k++) q.v[k].st[x] -= shift;
                            }
                            for (int k = 0; k < 4; k++) for (int x = 0; x < 3; x++) { lo[x] = std::min(lo[x], q.v[k].p[x]); hi[x] = std::max(hi[x], q.v[k].p[x]); }
                            m.quads.push_back(q);
                        }
                    }
                    if (!m.quads.empty()) {
                        m.origin = { std::round((lo[0] + hi[0]) / 2), std::round((lo[1] + hi[1]) / 2), std::round((lo[2] + hi[2]) / 2) };
                        models.push_back(std::move(m));
                    }
                    from = step_end;
                }
                char cl[160];
                snprintf(cl, sizeof(cl), "start: cutout (state %d) goes on from its end column of %zu vertices, %.0f ft ahead, %.0f ft out\n", state,
                         rows.size(), along(rows[0].v.p), across(rows[0].v.p));
                report += cl;
                if (getenv("SCENERY_PIECES")) {
                    for (const Row& r : rows) {
                        snprintf(cl, sizeof(cl), "  cutout row (%.0f %.0f %.0f) st %.0f %.0f rate %.2f %.2f\n", along(r.v.p), across(r.v.p), r.v.p[1] - s0[1],
                                 r.v.st[0], r.v.st[1], r.rate[0], r.rate[1]);
                        report += cl;
                    }
                }
            }
        }

        // Sides that stop short at the cut. Where a side's ground ends nearer the road at the cut than a little ahead
        // (the stock edge slants in toward the cut: Monterey's left grass ends 54 ft out at the cut and runs to its tree
        // wall 190 ft out 40 ft ahead; Mount Shasta's right green hill and its trees stop short of the cut), that side's
        // outer profile comes from the nearest plane ahead (20 to 250 ft) where it runs on furthest (at least 30 ft past
        // the cut's edge): from the cut's edge outward, the ground's chain of pieces (each starting where the last ended,
        // the one reaching furthest out; or a jump to one starting lower, the gap hidden behind the crest from the road),
        // and the walls standing at its end (the tree wall; tree cards in between are left to the card copies). It is
        // drawn straight from that plane back to the cut, moved up or down to meet the cut's ground edge exactly, and
        // swept from the cut with the rest.
        {
            std::string notes;
            std::vector<WallCross> side_walls = track_walls_at(cut + 3);
            for (double sg : { -1.0, 1.0 }) {
                double edge = 0;
                for (const Piece& pc : pieces) {
                    if (pc.lead > 0) continue;
                    for (const ProfileEnd& e : pc.e) edge = std::max(edge, sg * e.l);
                }
                // Only where it was asked for: Monterey's left (- side) and Mount Shasta's right (+ side, its green hill
                // and the trees on it stopping short of the cut).
                std::vector<double> planes;
                double lim = cfg.reach;
                if ((t == 0 && sg < 0) || ((t == 4 || t == 9) && sg > 0)) planes = { 20, 40, 80, 150, 250 };

                auto clip_at = [](const Piece& pc, double l) {
                    double u = (l - pc.e[0].l) / (pc.e[1].l - pc.e[0].l);
                    ProfileEnd e;
                    e.l = l;
                    e.y = pc.e[0].y + (pc.e[1].y - pc.e[0].y) * u;
                    for (int x = 0; x < 2; x++) e.st[x] = pc.e[0].st[x] + (pc.e[1].st[x] - pc.e[0].st[x]) * u;
                    for (int x = 0; x < 4; x++) e.c[x] = pc.e[0].c[x] + (pc.e[1].c[x] - pc.e[0].c[x]) * u;
                    return e;
                };
                std::vector<Piece> best;
                double best_reach = edge + 30, best_d = 0;
                for (double d : planes) {
                    // This side's pieces past the edge, inner end first.
                    std::vector<Piece> side;
                    for (Piece pc : section(cut + d, lim)) {
                        if (sg * pc.e[0].l > sg * pc.e[1].l) std::swap(pc.e[0], pc.e[1]);
                        double a = sg * pc.e[0].l, b = sg * pc.e[1].l;
                        if (b <= edge + 0.01) continue;
                        if (a < edge) pc.e[0] = clip_at(pc, sg * edge);
                        side.push_back(pc);
                    }
                    auto out_of = [&](const Piece& pc) { return sg * pc.e[1].l - sg * pc.e[0].l; };
                    std::vector<Piece> chain;
                    double at_l = edge, at_y = 0;
                    bool started = false;
                    for (size_t steps = 0; steps < side.size(); steps++) {
                        const Piece* next = nullptr;
                        for (const Piece& pc : side) {
                            if (out_of(pc) < 0.5) continue;   // upright: a wall or a card
                            if (started && sg * pc.e[1].l <= at_l + 0.1) continue;   // each step goes outward
                            double a = sg * pc.e[0].l;
                            bool joins = started ? std::fabs(a - at_l) < 2 && std::fabs(pc.e[0].y - at_y) < 2 : std::fabs(a - edge) < 2;
                            if (!joins) continue;
                            if (!next || sg * pc.e[1].l > sg * next->e[1].l) next = &pc;
                        }
                        if (!next && started) {
                            // A jump outward to the nearest piece starting lower (behind the crest from the road).
                            for (const Piece& pc : side) {
                                if (out_of(pc) < 0.5) continue;
                                double a = sg * pc.e[0].l;
                                if (a <= at_l || pc.e[0].y > at_y - 1) continue;
                                if (!next || a < sg * next->e[0].l) next = &pc;
                            }
                        }
                        if (!next) break;
                        chain.push_back(*next);
                        // One joining within 2 ft starts exactly where the last ended (Mount Shasta's hill had 1 and 2 ft
                        // gaps between its pieces, a line of sky across it).
                        if (started && std::fabs(sg * next->e[0].l - at_l) < 2 && std::fabs(next->e[0].y - at_y) < 2) {
                            chain.back().e[0].l = sg * at_l;
                            chain.back().e[0].y = at_y;
                        }
                        at_l = sg * next->e[1].l;
                        at_y = next->e[1].y;
                        started = true;
                    }
                    if (chain.empty() || at_l <= best_reach) continue;
                    // The walls standing at its end, their foot taken down to 1 ft under the chain's ground where they
                    // stand (Monterey's tree wall stood 6 ft in from the grass's end with its foot half a foot over it,
                    // and the sky showed between), the texture going on down the wall as it did.
                    size_t ground_n = chain.size();
                    for (const Piece& pc : side) {
                        if (out_of(pc) >= 0.5 || std::fabs(sg * pc.e[0].l - at_l) >= 8) continue;
                        Piece w = pc;
                        int lo = w.e[0].y <= w.e[1].y ? 0 : 1;
                        ProfileEnd& foot = w.e[size_t(lo)];
                        const ProfileEnd& top = w.e[size_t(1 - lo)];
                        double ground = foot.y;
                        for (size_t g = 0; g < ground_n; g++) {
                            double a = sg * chain[g].e[0].l, b = sg * chain[g].e[1].l, at = sg * foot.l;
                            if (at < std::min(a, b) - 0.01 || at > std::max(a, b) + 0.01 || std::fabs(b - a) < 0.01) continue;
                            ground = chain[g].e[0].y + (chain[g].e[1].y - chain[g].e[0].y) * (at - a) / (b - a);
                        }
                        double drop = foot.y - (ground - 1);
                        double h = top.y - foot.y;
                        if (drop > 0 && h > 1) {
                            double u = drop / h;
                            foot.l += (foot.l - top.l) * u;
                            for (int x = 0; x < 2; x++) foot.st[x] += (foot.st[x] - top.st[x]) * u;
                            foot.y -= drop;
                        }
                        chain.push_back(w);
                    }
                    best = chain;
                    best_reach = at_l;
                    best_d = d;
                }
                if (best.empty()) continue;
                // Out of reach behind a track wall on this side: no collision.
                bool behind = false;
                for (const WallCross& w : side_walls) {
                    if ((w.l0 + w.l1) * sg <= 0) continue;
                    if (std::max(sg * w.l0, sg * w.l1) <= edge + 1 && std::max(w.y0, w.y1) > s0[1] + 10) behind = true;
                }
                // It meets the cut's ground at its edge exactly (the whole profile moved up or down by the difference):
                // taken as it was on its plane, Monterey's stood 0.9 ft over the cut's grass and the sky showed between.
                double edge_y = s0[1];
                for (const Piece& pc : pieces) {
                    if (pc.lead > 0) continue;
                    for (const ProfileEnd& e : pc.e) if (std::fabs(sg * e.l - edge) < 0.01) edge_y = e.y;
                }
                double shift = edge_y - (best[0].e[0].y - grade * best_d);
                for (Piece pc : best) {
                    for (ProfileEnd& e : pc.e) {
                        e.y += shift - grade * best_d;
                        for (int x = 0; x < 2; x++) e.st[x] += pc.rate[x] * best_d;
                    }
                    if (getenv("SCENERY_PIECES")) {
                        char pl[160];
                        snprintf(pl, sizeof(pl), "  side piece state %d: (%.1f, %.1f) - (%.1f, %.1f)\n", pc.state, pc.e[0].l, pc.e[0].y - s0[1],
                                 pc.e[1].l, pc.e[1].y - s0[1]);
                        report += pl;
                    }
                    pc.lead = best_d;
                    pc.bare = behind || t == 11;
                    pc.run_on = t == 11;
                    pieces.push_back(pc);
                    piece_flags.push_back(-1);
                    piece_info.push_back(0);
                }
                char line[160];
                snprintf(line, sizeof(line), " %s side from %.0f ft out to %.0f from %.0f ft ahead (%zu pieces%s);", sg < 0 ? "-" : "+", edge,
                         best_reach, best_d, best.size(), behind ? ", behind a wall" : "");
                notes += line;
            }
            if (!notes.empty()) report += "start: sides carried on:" + notes + "\n";
        }

        // Tree walls (tall surfaces facing across the road with a green texture) that start a little ahead of the cut
        // (Silicon Valley's) go on too: the cross-sections up to 150 ft ahead give the ones of them not at the cut (no
        // piece of their state within 15 ft across there or on a nearer plane), each carried straight back to the cut
        // and swept from there with its texture going on as it went. Only foliage: other walls stopping short of the
        // cut end there on purpose (Los Angeles' crossing structure).
        {
            double half_road = std::max(std::fabs(road0), std::fabs(road1));
            std::vector<Piece> found;
            auto near_one = [&](const std::vector<Piece>& list, const Piece& pc) {
                double ml = (pc.e[0].l + pc.e[1].l) / 2;
                for (const Piece& q : list) {
                    if (q.state == pc.state && std::fabs((q.e[0].l + q.e[1].l) / 2 - ml) < 15) return true;
                }
                return false;
            };
            std::vector<double> tall_planes = { 10.0, 25.0, 50.0, 90.0, 150.0 };
            // Mount Shasta's right tree wall, on the green hill's crest 74 ft out, ends 337 ft ahead of the cut (and
            // stopped abruptly in the view back): from just inside its end.
            if (t == 9) tall_planes.push_back(338);
            for (double d : tall_planes) {
                std::vector<Piece> here;
                for (Piece pc : section(cut + d)) {
                    double ml = (pc.e[0].l + pc.e[1].l) / 2;
                    double y0 = std::min(pc.e[0].y, pc.e[1].y), y1 = std::max(pc.e[0].y, pc.e[1].y);
                    if (getenv("SCENERY_PIECES") && y1 - y0 > 12) {
                        const float* cc = scene.states[size_t(pc.state)].color;
                        char pl[200];
                        snprintf(pl, sizeof(pl), "  tall %.0f state %d: (%.1f, %.1f)-(%.1f, %.1f) n %.2f %.2f col %.2f %.2f %.2f rate %.2f %.2f near %d\n", d, pc.state,
                                 pc.e[0].l, pc.e[0].y - s0[1], pc.e[1].l, pc.e[1].y - s0[1], pc.normal[1], dot(pc.normal, n), cc[0], cc[1], cc[2],
                                 pc.rate[0], pc.rate[1], int(near_one(pieces, pc)));
                        report += pl;
                    }
                    if (std::fabs(pc.normal[1]) > 0.5 || std::fabs(dot(pc.normal, n)) < 0.7) continue;
                    if (y1 - y0 < 12 || y1 < s0[1] + 10 || std::fabs(ml) < half_road + 10) continue;
                    const float* col = scene.states[size_t(pc.state)].color;
                    if (!(col[1] > col[0] * 1.05f && col[1] > col[2])) continue;
                    if (near_one(pieces, pc) || near_one(found, pc)) continue;
                    // The ground at the cut has to reach it (within 15 ft): one standing past bare ground's end
                    // (Monterey's at 190 ft, its grass ending at 54) shows the gap, and Santa Cruz's would stand in the
                    // sea. Only on Silicon Valley may the ground end in a wall over 6 ft that hides the gap from the road
                    // instead (its tree wall at 128 ft stands behind a 10 ft wall at 85), and on Santa Cruz's + side for
                    // walls stopping within 25 ft of the cut (its tree wall at 108 ft and the hillside at 227 behind its
                    // 30 ft tree wall at 63, which stopped 10 ft short of the cut).
                    double ground_reach = 0, wall_at = 0, wall_top = -1e30;
                    for (const Piece& g : pieces) {
                        for (const ProfileEnd& e : g.e) {
                            if (e.l * ml <= 0) continue;
                            if (std::fabs(g.normal[1]) >= 0.7) ground_reach = std::max(ground_reach, std::fabs(e.l));
                            else if (std::fabs(e.l) > wall_at + 0.5) { wall_at = std::fabs(e.l); wall_top = std::max(g.e[0].y, g.e[1].y); }
                            else if (std::fabs(e.l) > wall_at - 0.5) wall_top = std::max(wall_top, std::max(g.e[0].y, g.e[1].y));
                        }
                    }
                    bool hidden = (t == 1 || (t == 5 && ml > 0 && d <= 25)) && wall_at >= ground_reach - 1 && wall_top > s0[1] + 6;
                    if (ground_reach < std::fabs(ml) - 15 && !hidden) continue;
                    // Its texture's across axis (the one changing much more up the surface than going back) holds.
                    double w = std::hypot(pc.e[1].l - pc.e[0].l, pc.e[1].y - pc.e[0].y);
                    for (int x = 0; x < 2; x++) {
                        double grad = std::fabs(pc.e[1].st[x] - pc.e[0].st[x]) / std::max(w, 0.1);
                        if (grad > 2 * std::fabs(pc.rate[x]) && std::fabs(pc.rate[1 - x]) > std::fabs(pc.rate[x])) pc.rate[x] = 0;
                    }
                    // At the cut: lower by the grade over d, texture as it goes on back.
                    for (ProfileEnd& e : pc.e) {
                        e.y -= grade * d;
                        for (int x = 0; x < 2; x++) e.st[x] += pc.rate[x] * d;
                    }
                    // Its texture has to go on along the road (one held still smears into streaks: Highway 1's bank).
                    if (std::fabs(pc.rate[0]) < 1 && std::fabs(pc.rate[1]) < 1) continue;
                    pc.lead = d;
                    here.push_back(pc);
                }
                found.insert(found.end(), here.begin(), here.end());
            }
            // One with a track wall (over 10 ft up) between it and the spine is out of reach: no collision (it would
            // only crowd the file).
            std::vector<WallCross> walls = track_walls_at(cut + 3);
            for (Piece& pc : found) {
                double near_l = std::min(std::fabs(pc.e[0].l), std::fabs(pc.e[1].l));
                for (const WallCross& w : walls) {
                    if ((w.l0 + w.l1) * (pc.e[0].l + pc.e[1].l) <= 0) continue;   // other side
                    if (std::max(std::fabs(w.l0), std::fabs(w.l1)) < near_l && std::max(w.y0, w.y1) > s0[1] + 10) pc.bare = true;
                }
            }
            // Santa Cruz's + side: the cut's ground ends 2 ft short of its 30 ft tree wall at 63 ft, the wall's foot 1 ft
            // over it, and the hole showed the trees behind. The ground goes on out to the wall (on its own slope, the
            // texture as it went) and the wall's foot down to 1 ft under it.
            if (t == 5) {
                for (Piece& w : found) {
                    double wl = (w.e[0].l + w.e[1].l) / 2;
                    if (wl <= 0) continue;
                    Piece* ground = nullptr;
                    int ge = 0;
                    for (Piece& g : pieces) {
                        if (g.lead > 0 || std::fabs(g.normal[1]) < 0.7) continue;
                        for (int k = 0; k < 2; k++) {
                            if (g.e[k].l > wl || g.e[k].l < wl - 5) continue;
                            if (!ground || g.e[k].l > ground->e[ge].l) { ground = &g; ge = k; }
                        }
                    }
                    if (!ground) continue;
                    ProfileEnd& end = ground->e[ge];
                    const ProfileEnd& other = ground->e[1 - ge];
                    double span = end.l - other.l;
                    if (span < 1) continue;
                    double u = (wl - end.l) / span;
                    end.y += (end.y - other.y) * u;
                    for (int x = 0; x < 2; x++) end.st[x] += (end.st[x] - other.st[x]) * u;
                    end.l = wl;
                    int lo = w.e[0].y <= w.e[1].y ? 0 : 1;
                    ProfileEnd& foot = w.e[lo];
                    const ProfileEnd& top = w.e[1 - lo];
                    double drop = foot.y - (end.y - 1), h = top.y - foot.y;
                    if (drop > 0 && h > 1) {
                        for (int x = 0; x < 2; x++) foot.st[x] += (foot.st[x] - top.st[x]) * drop / h;
                        foot.y -= drop;
                    }
                }
                // Its ground's last authored edge slants across the cut (from 0.5 ft ahead at the wall to 0.5 behind at
                // the rail), and a sliver of sky showed between: the + side ground starts from that edge's vertices.
                for (Piece& g : pieces) {
                    if (g.lead > 0 || std::fabs(g.normal[1]) < 0.7 || std::min(g.e[0].l, g.e[1].l) < 40 ||
                        std::hypot(g.e[1].l - g.e[0].l, g.e[1].y - g.e[0].y) < 0.5) continue;
                    for (int k = 0; k < 2; k++) {
                        ProfileEnd& e = g.e[k];
                        const Vtx* best = nullptr;
                        double best_d = 1e30;
                        for (const Tri& tr : scene.tris) {
                            if (tr.state != g.state) continue;
                            for (const Vtx& v : tr.v) {
                                if (std::fabs(along(v.p) - cut) > 2) continue;
                                double d = std::hypot(across(v.p) - e.l, v.p[1] - e.y);
                                if (d < 1 && d < best_d) { best = &v; best_d = d; }
                            }
                        }
                        if (!best) continue;
                        double sv = cut - along(best->p);
                        e.l = across(best->p);
                        e.y = best->p[1];
                        for (int x = 0; x < 2; x++) e.st[x] = best->st[x] - g.rate[x] * sv;
                        g.skip[k] = sv;
                        if (getenv("SCENERY_PIECES")) {
                            char pl[160];
                            snprintf(pl, sizeof(pl), "  snap state %d end %d: (%.1f, %.1f) %.1f back\n", g.state, k, e.l, e.y - s0[1], sv);
                            report += pl;
                        }
                    }
                }
            }
            for (const Piece& pc : found) {
                pieces.push_back(pc);
                piece_flags.push_back(-1);
                piece_info.push_back(0);
            }
            if (!found.empty()) {
                char line[120];
                snprintf(line, sizeof(line), "start: %zu pieces of tall surfaces starting ahead of the cut carried on:", found.size());
                report += line;
                for (const Piece& pc : found) {
                    snprintf(line, sizeof(line), " state %d (%.1f, %.1f)-(%.1f, %.1f) from %.0f ft ahead%s,", pc.state, pc.e[0].l, pc.e[0].y - s0[1],
                             pc.e[1].l, pc.e[1].y - s0[1], pc.lead, pc.bare ? " (behind a wall)" : "");
                    report += line;
                }
                report += "\n";
            }
        }

        // The visible sweep, in pieces of path of at most 1,000 ft.
        auto out_vtx = [&](const Piece& pc, int e, const Sample& sm) {
            const ProfileEnd& pe = pc.e[e];
            OutVtx v;
            double y = pe.y;
            if (pc.rise_len > 0) {
                double u = std::clamp(sm.s / pc.rise_len, 0.0, 1.0);
                y = pc.rise_base + (pe.y - pc.rise_base) * u * u * (3 - 2 * u);
            }
            v.p = place(sm, pe.l + drift_shift(pc, sm.s), y);
            double sigma = sm.s - sm.turn * pe.l;
            for (int x = 0; x < 2; x++) v.st[x] = int32_t(std::lround(pe.st[x] + pc.rate[x] * sigma));
            for (int x = 0; x < 4; x++) v.c[x] = uint8_t(std::clamp(std::lround(pe.c[x]), 0l, 255l));
            return v;
        };
        // A point `d` ft ahead of the cut on the straight line the sweep starts on.
        auto lead_sample = [&](double d) {
            Sample v = samples[0];
            v.s = -d;
            v.c = add(samples[0].c, mul(f, d));
            v.h = samples[0].h + grade * d;
            return v;
        };
        size_t chunk_start = 0;
        int index = 0;
        while (chunk_start + 1 < samples.size()) {
            size_t chunk_end = chunk_start + 1;
            while (chunk_end + 1 < samples.size() && samples[chunk_end].s - samples[chunk_start].s < 1000) chunk_end++;
            NewModel m;
            char name[16];
            snprintf(name, sizeof(name), "SCNXBACK%02d", index++);
            m.name = name;
            V3 lo{ 1e30, 1e30, 1e30 }, hi{ -1e30, -1e30, -1e30 };
            for (size_t i = chunk_start; i < chunk_end; i++) {
                const Sample& a = samples[i];
                const Sample& b = samples[i + 1];
                for (const Piece& pc : pieces) {
                    if (i == 0 && pc.lead > 0) {
                        Sample ahead = lead_sample(pc.lead);
                        OutQuad q;
                        q.state = pc.state;
                        q.bare = pc.bare;
                        q.v[0] = out_vtx(pc, 0, ahead);
                        q.v[1] = out_vtx(pc, 1, ahead);
                        q.v[2] = out_vtx(pc, 1, samples[0]);
                        q.v[3] = out_vtx(pc, 0, samples[0]);
                        V3 qn = cross(sub(q.v[1].p, q.v[0].p), sub(q.v[2].p, q.v[0].p));
                        V3 turned = add(add(mul(samples[0].t, -dot(pc.normal, f)), V3{ 0, pc.normal[1], 0 }), mul(samples[0].n, dot(pc.normal, n)));
                        if (dot(qn, turned) < 0) { std::swap(q.v[0], q.v[1]); std::swap(q.v[2], q.v[3]); }
                        const State& st = scene.states[size_t(pc.state)];
                        for (int x = 0; x < 2; x++) {
                            double per = st.period[x];
                            if (per <= 0) continue;
                            int32_t lo_st = std::min({ q.v[0].st[x], q.v[1].st[x], q.v[2].st[x], q.v[3].st[x] });
                            int32_t shift = int32_t(std::floor(lo_st / per) * per);
                            for (int k = 0; k < 4; k++) q.v[k].st[x] -= shift;
                        }
                        for (int k = 0; k < 4; k++) for (int x = 0; x < 3; x++) { lo[x] = std::min(lo[x], q.v[k].p[x]); hi[x] = std::max(hi[x], q.v[k].p[x]); }
                        m.quads.push_back(q);
                    }
                    // Where the stock surface goes on behind the cut, each end starts where it stops.
                    if (pc.skip[0] >= b.s && pc.skip[1] >= b.s) continue;
                    auto from = [&](int k) {
                        // (on the first stretch an end may start a little ahead of the cut: an authored edge's vertex)
                        if (pc.skip[k] <= a.s && !(a.s == 0 && pc.skip[k] < 0)) return a;
                        double u = std::min(1.0, (pc.skip[k] - a.s) / (b.s - a.s));
                        Sample v = a;
                        v.s = a.s + (b.s - a.s) * u;
                        v.c = add(a.c, mul(sub(b.c, a.c), u));
                        v.h = a.h + (b.h - a.h) * u;
                        v.turn = a.turn + (b.turn - a.turn) * u;
                        return v;
                    };
                    OutQuad q;
                    q.state = pc.state;
                    q.bare = pc.bare;
                    q.v[0] = out_vtx(pc, 0, from(0));
                    q.v[1] = out_vtx(pc, 1, from(1));
                    q.v[2] = out_vtx(pc, 1, b);
                    q.v[3] = out_vtx(pc, 0, b);
                    // Keep the source triangle's facing: its normal carried along with the path's frame (f goes to
                    // -t, n to the sample's n).
                    V3 qn = cross(sub(q.v[1].p, q.v[0].p), sub(q.v[2].p, q.v[0].p));
                    V3 turned = add(add(mul(a.t, -dot(pc.normal, f)), V3{ 0, pc.normal[1], 0 }), mul(a.n, dot(pc.normal, n)));
                    if (dot(qn, turned) < 0) { std::swap(q.v[0], q.v[1]); std::swap(q.v[2], q.v[3]); }
                    // Whole repeats off the texture coordinates.
                    const State& st = scene.states[size_t(pc.state)];
                    for (int x = 0; x < 2; x++) {
                        double per = st.period[x];
                        if (per <= 0) continue;
                        int32_t lo_st = std::min({ q.v[0].st[x], q.v[1].st[x], q.v[2].st[x], q.v[3].st[x] });
                        int32_t shift = int32_t(std::floor(lo_st / per) * per);
                        for (int k = 0; k < 4; k++) q.v[k].st[x] -= shift;
                    }
                    for (int k = 0; k < 4; k++) for (int x = 0; x < 3; x++) { lo[x] = std::min(lo[x], q.v[k].p[x]); hi[x] = std::max(hi[x], q.v[k].p[x]); }
                    m.quads.push_back(q);
                }
            }
            m.origin = { std::round((lo[0] + hi[0]) / 2), std::round((lo[1] + hi[1]) / 2), std::round((lo[2] + hi[2]) / 2) };
            models.push_back(std::move(m));
            chunk_start = chunk_end;
        }
        // One model a 1,000 ft (its s16 vertices reach 2,047 ft from its middle).
        for (int k = 0; k < 4; k++) {
            NewModel m;
            char run_name[16];
            snprintf(run_name, sizeof(run_name), "SCNXRUNON%02d", k);
            m.name = run_name;
            V3 lo{ 1e30, 1e30, 1e30 }, hi{ -1e30, -1e30, -1e30 };
            for (const Piece& pc : pieces) {
                if (!pc.run_on) continue;
                {
                    Sample a = samples.back(), b = samples.back();
                    a.s += k * 1000.0;
                    a.c = add(a.c, mul(a.t, k * 1000.0));
                    b.s += (k + 1) * 1000.0;
                    b.c = add(b.c, mul(b.t, (k + 1) * 1000.0));
                    OutQuad q;
                    q.state = pc.state;
                    q.bare = true;
                    q.v[0] = out_vtx(pc, 0, a);
                    q.v[1] = out_vtx(pc, 1, a);
                    q.v[2] = out_vtx(pc, 1, b);
                    q.v[3] = out_vtx(pc, 0, b);
                    V3 qn = cross(sub(q.v[1].p, q.v[0].p), sub(q.v[2].p, q.v[0].p));
                    V3 turned = add(add(mul(a.t, -dot(pc.normal, f)), V3{ 0, pc.normal[1], 0 }), mul(a.n, dot(pc.normal, n)));
                    if (dot(qn, turned) < 0) { std::swap(q.v[0], q.v[1]); std::swap(q.v[2], q.v[3]); }
                    const State& st = scene.states[size_t(pc.state)];
                    for (int x = 0; x < 2; x++) {
                        double per = st.period[x];
                        if (per <= 0) continue;
                        int32_t lo_st = std::min({ q.v[0].st[x], q.v[1].st[x], q.v[2].st[x], q.v[3].st[x] });
                        int32_t shift = int32_t(std::floor(lo_st / per) * per);
                        for (int v = 0; v < 4; v++) q.v[v].st[x] -= shift;
                    }
                    for (int v = 0; v < 4; v++) for (int x = 0; x < 3; x++) { lo[x] = std::min(lo[x], q.v[v].p[x]); hi[x] = std::max(hi[x], q.v[v].p[x]); }
                    m.quads.push_back(q);
                }
            }
            if (!m.quads.empty()) {
                m.origin = { std::round((lo[0] + hi[0]) / 2), std::round((lo[1] + hi[1]) / 2), std::round((lo[2] + hi[2]) / 2) };
                models.push_back(std::move(m));
            }
        }

        // Mojave's land out to its cutouts: the ground (authored or swept) ends 440 to 690 ft out and the cutouts stand
        // 1,030 and 1,550 ft out; from the start's straight their feet sit under the ground's skyline, but from the road
        // behind the cut, 100 ft lower, the sky showed under them. Every 25 ft along each cutout, the drawn ground's
        // outer edge on a plane across the road (walking out from the road through ground pieces that touch) is joined
        // by land to the cutout's bottom row; the land's inner edge tucks 3 ft in and 1 ft under the ground so the
        // two never part. Its texture is the edge's, laid flat at the ground's scale. One model a 1,000 ft
        // (`SCNXLANDnn`), no collision (out of reach).
        if (!cut_feet.empty()) {
            struct Seg { double l0, y0, l1, y1; int state; double st0[2], st1[2]; uint8_t c[4]; };
            struct Flat { V3 p[3]; double st[3][2]; uint8_t c[4]; int state; };
            std::vector<Flat> flats;
            auto add_flat = [&](const V3* p, const double (*st)[2], const uint8_t* c, int state) {
                V3 tn = cross(sub(p[1], p[0]), sub(p[2], p[0]));
                double tl = std::sqrt(dot(tn, tn));
                if (tl < 1e-6 || std::fabs(tn[1] / tl) < 0.2) return;   // walls and cards
                Flat fl;
                for (int k = 0; k < 3; k++) { fl.p[k] = p[k]; fl.st[k][0] = st[k][0]; fl.st[k][1] = st[k][1]; }
                for (int x = 0; x < 4; x++) fl.c[x] = c[x];
                fl.state = state;
                flats.push_back(fl);
            };
            for (const Tri& tr : scene.tris) {
                V3 p[3] = { tr.v[0].p, tr.v[1].p, tr.v[2].p };
                double st[3][2] = { { tr.v[0].st[0], tr.v[0].st[1] }, { tr.v[1].st[0], tr.v[1].st[1] }, { tr.v[2].st[0], tr.v[2].st[1] } };
                add_flat(p, st, tr.v[0].c, tr.state);
            }
            for (const NewModel& m : models) {
                if (m.name.rfind("SCNXCUT", 0) == 0) continue;
                for (const OutQuad& q : m.quads) {
                    if (q.state < 0) continue;
                    for (int t0 = 0; t0 < q.count - 2; t0++) {
                        int ix[3] = { 0, t0 + 1, t0 + 2 };
                        V3 p[3];
                        double st[3][2];
                        for (int k = 0; k < 3; k++) { p[k] = q.v[ix[k]].p; st[k][0] = q.v[ix[k]].st[0]; st[k][1] = q.v[ix[k]].st[1]; }
                        add_flat(p, st, q.v[0].c, q.state);
                    }
                }
            }
            int land_models = 0, land_quads = 0;
            for (const CutFoot& cf : cut_feet) {
                double foot_l = cf.sg * across(cf.p);
                // The ground's outer edge on the plane at along `a`: from the road out through touching pieces.
                struct Edge { bool ok = false; double l, y, st[2], rate; uint8_t c[4]; int state; };
                auto edge_at = [&](double a) {
                    std::vector<Seg> segs;
                    for (const Flat& fl : flats) {
                        double d[3];
                        for (int k = 0; k < 3; k++) d[k] = along(fl.p[k]) - a;
                        if ((d[0] > 0 && d[1] > 0 && d[2] > 0) || (d[0] < 0 && d[1] < 0 && d[2] < 0)) continue;
                        double pl[2], py[2], pst[2][2];
                        int got = 0;
                        for (int k = 0; k < 3 && got < 2; k++) {
                            int j = (k + 1) % 3;
                            if ((d[k] < 0) == (d[j] < 0)) continue;
                            double u = d[k] / (d[k] - d[j]);
                            V3 p = add(fl.p[k], mul(sub(fl.p[j], fl.p[k]), u));
                            pl[got] = cf.sg * across(p);
                            py[got] = p[1];
                            for (int x = 0; x < 2; x++) pst[got][x] = fl.st[k][x] + (fl.st[j][x] - fl.st[k][x]) * u;
                            got++;
                        }
                        if (got < 2) continue;
                        if (std::max(pl[0], pl[1]) < 0 || std::max(pl[0], pl[1]) > foot_l - 20) continue;
                        int i0 = pl[0] <= pl[1] ? 0 : 1, i1 = 1 - i0;
                        Seg sg{ pl[i0], py[i0], pl[i1], py[i1], fl.state, { pst[i0][0], pst[i0][1] }, { pst[i1][0], pst[i1][1] }, {} };
                        for (int x = 0; x < 4; x++) sg.c[x] = fl.c[x];
                        segs.push_back(sg);
                    }
                    Edge e;
                    if (segs.empty()) return e;
                    // Start on the piece nearest the road, then each step to a piece starting within 2 ft (and 3 ft
                    // up or down) of the edge so far that reaches further out.
                    const Seg* at = &segs[0];
                    for (const Seg& sg : segs) if (sg.l0 < at->l0) at = &sg;
                    for (bool grew = true; grew;) {
                        grew = false;
                        const Seg* next = nullptr;
                        for (const Seg& sg : segs) {
                            if (sg.l1 <= at->l1 + 0.1) continue;
                            if (sg.l0 > at->l1 + 2) continue;
                            // its height where the edge so far ends
                            double y = sg.l1 - sg.l0 > 0.01 ? sg.y0 + (sg.y1 - sg.y0) * std::clamp((at->l1 - sg.l0) / (sg.l1 - sg.l0), 0.0, 1.0) : sg.y0;
                            if (std::fabs(y - at->y1) > 3) continue;
                            if (!next || sg.l1 > next->l1) next = &sg;
                        }
                        if (next) { at = next; grew = true; }
                    }
                    e.ok = true;
                    e.l = at->l1;
                    e.y = at->y1;
                    e.state = at->state;
                    for (int x = 0; x < 4; x++) e.c[x] = at->c[x];
                    double len = std::max(0.5, std::hypot(at->l1 - at->l0, at->y1 - at->y0));
                    e.rate = std::max(std::fabs(at->st1[0] - at->st0[0]), std::fabs(at->st1[1] - at->st0[1])) / len;
                    return e;
                };
                double from = cf.from, to = -4000;
                // One texture for the side's land: the ground state found most often at its edge (taking each plane's
                // own put road-shoulder strips and striped slopes in it), at that state's rate across the ground.
                std::vector<Edge> edges;
                for (double a = from; a > to - 0.01; a -= 25) edges.push_back(edge_at(std::max(a, to)));
                std::map<int, int> votes;
                for (const Edge& e : edges) if (e.ok) votes[e.state]++;
                int land_state = -1;
                for (const auto& [st, count] : votes) if (land_state < 0 || count > votes[land_state]) land_state = st;
                // (the median: a short piece across a texture wrap gives a rate many times the ground's)
                std::vector<double> rates;
                for (const Edge& e : edges) if (e.ok && e.state == land_state && e.rate > 0) rates.push_back(e.rate);
                double rate = 0;
                if (!rates.empty()) {
                    std::nth_element(rates.begin(), rates.begin() + rates.size() / 2, rates.end());
                    rate = rates[rates.size() / 2];
                }
                if (getenv("SCENERY_PIECES")) {
                    char pl[120];
                    snprintf(pl, sizeof(pl), "  land %+.0f side: state %d, %.1f a ft, %zu of %zu planes with an edge\n", cf.sg, land_state, rate,
                             rates.size(), edges.size());
                    report += pl;
                }
                size_t edge_i = 0;
                Edge prev = edges[edge_i++];
                double prev_a = from;
                for (int piece = 0; from > to; piece++) {
                    double step_end = std::max(to, from - 1000);
                    NewModel m;
                    char name[16];
                    snprintf(name, sizeof(name), "SCNXLAND%02d", land_models++);
                    m.name = name;
                    V3 lo{ 1e30, 1e30, 1e30 }, hi{ -1e30, -1e30, -1e30 };
                    for (double a = from - 25; a > step_end - 0.01; a -= 25) {
                        double a1 = std::max(a, step_end);
                        Edge cur = edge_i < edges.size() ? edges[edge_i++] : edge_at(a1);
                        if (prev.ok && cur.ok && rate > 0) {
                            auto vtx = [&](double av, double l, double y, const Edge& e) {
                                OutVtx o;
                                o.p = add(add(s0, mul(f, av)), mul(n, cf.sg * l));
                                o.p[1] = y;
                                o.st[0] = int32_t(std::lround(dot(sub(o.p, s0), f) * rate));
                                o.st[1] = int32_t(std::lround(dot(sub(o.p, s0), n) * rate));
                                for (int x = 0; x < 4; x++) o.c[x] = e.c[x];
                                return o;
                            };
                            // On the cutout's bottom row exactly (its vertices: the column's, moved back along -f).
                            auto foot = [&](double av, const Edge& e) {
                                OutVtx o = vtx(av, foot_l, 0, e);
                                o.p = add(cf.p, mul(f, av - along(cf.p)));
                                o.st[0] = int32_t(std::lround(dot(sub(o.p, s0), f) * rate));
                                o.st[1] = int32_t(std::lround(dot(sub(o.p, s0), n) * rate));
                                return o;
                            };
                            OutQuad q;
                            q.state = land_state;
                            q.bare = true;
                            q.v[0] = vtx(prev_a, prev.l - 3, prev.y - 1, prev);
                            q.v[1] = foot(prev_a, prev);
                            q.v[2] = foot(a1, cur);
                            q.v[3] = vtx(a1, cur.l - 3, cur.y - 1, cur);
                            // Across, the texture runs up the land's own slope from the foot (laid flat it streaked
                            // where the land climbs from the swept ground 100 ft under the cutout's foot).
                            for (int k : { 0, 3 }) {
                                const OutVtx& ft = q.v[k == 0 ? 1 : 2];
                                V3 d = sub(q.v[k].p, ft.p);
                                q.v[k].st[1] = int32_t(std::lround(ft.st[1] - std::sqrt(dot(d, d)) * rate));
                            }
                            V3 qn = cross(sub(q.v[2].p, q.v[0].p), sub(q.v[3].p, q.v[1].p));
                            if (qn[1] > 0) { std::swap(q.v[0], q.v[1]); std::swap(q.v[2], q.v[3]); }   // seen from above
                            const State& st = scene.states[size_t(q.state)];
                            for (int x = 0; x < 2; x++) {
                                double per = st.period[x];
                                if (per <= 0) continue;
                                int32_t lo_st = std::min({ q.v[0].st[x], q.v[1].st[x], q.v[2].st[x], q.v[3].st[x] });
                                int32_t shift = int32_t(std::floor(lo_st / per) * per);
                                for (int k = 0; k < 4; k++) q.v[k].st[x] -= shift;
                            }
                            for (int k = 0; k < 4; k++) for (int x = 0; x < 3; x++) { lo[x] = std::min(lo[x], q.v[k].p[x]); hi[x] = std::max(hi[x], q.v[k].p[x]); }
                            m.quads.push_back(q);
                            land_quads++;
                        }
                        prev = cur;
                        prev_a = a1;
                    }
                    if (!m.quads.empty()) {
                        m.origin = { std::round((lo[0] + hi[0]) / 2), std::round((lo[1] + hi[1]) / 2), std::round((lo[2] + hi[2]) / 2) };
                        models.push_back(std::move(m));
                    }
                    from = step_end;
                }
            }
            char line[120];
            snprintf(line, sizeof(line), "start: land out to the cutouts: %d quads\n", land_quads);
            report += line;
        }

        // Trees: the upright cards beside the road (tree and bush cards, which face along the road and so aren't swept)
        // on the `card_window` ft ahead of the cut are set down again every `card_window` ft along the sweep, each
        // vertex at its distance back, across and height in the sweep's frame there, past what was built behind.
        {
            constexpr double card_window = 240;
            auto frame_at = [&](double s) {
                size_t i = 0;
                while (i + 2 < samples.size() && samples[i + 1].s < s) i++;
                const Sample& a = samples[i];
                const Sample& b = samples[i + 1];
                double u = std::clamp((s - a.s) / std::max(1e-6, b.s - a.s), 0.0, 1.0);
                Sample m;
                m.s = s;
                m.c = add(a.c, mul(sub(b.c, a.c), u));
                m.t = norm(add(a.t, mul(sub(b.t, a.t), u)));
                m.n = norm(add(a.n, mul(sub(b.n, a.n), u)));
                m.turn = a.turn + (b.turn - a.turn) * u;
                m.h = a.h + (b.h - a.h) * u;
                // Past the end the path goes on straight (only for the extent of a card).
                if (s > samples.back().s) { m = samples.back(); m.c = add(m.c, mul(m.t, s - m.s)); m.s = s; }
                return m;
            };
            double half_road = std::max(std::fabs(road0), std::fabs(road1));
            std::vector<const Tri*> cards;
            const bool list_cards = getenv("SCENERY_CARDS") != nullptr;
            for (const Tri& tr : scene.tris) {
                V3 c = mul(add(add(tr.v[0].p, tr.v[1].p), tr.v[2].p), 1.0 / 3);
                double back = cut - along(c);
                V3 nrm = norm(cross(sub(tr.v[1].p, tr.v[0].p), sub(tr.v[2].p, tr.v[0].p)));
                double top = std::max({ tr.v[0].p[1], tr.v[1].p[1], tr.v[2].p[1] });
                const char* why = back > 0 || back < -card_window ? "along" : std::fabs(across(c)) > cfg.reach ? "reach"
                                : std::fabs(across(c)) < half_road + 4 ? "road" : std::fabs(nrm[1]) > 0.5 ? "flat"
                                : std::fabs(dot(nrm, f)) < 0.35 ? "facing" : top < s0[1] + 6 || top > s0[1] + 200 ? "height" : nullptr;
                if (list_cards && back < 0 && back > -2 * card_window && top > s0[1] + 15 && std::fabs(across(c)) < 2 * cfg.reach) {
                    char line[200];
                    snprintf(line, sizeof(line), "  card? state %d back %.0f across %.0f top %.0f facing %.2f up %.2f: %s\n", tr.state, back,
                             across(c), top - s0[1], dot(nrm, f), nrm[1], why ? why : "taken");
                    report += line;
                }
                if (why) continue;
                cards.push_back(&tr);
            }
            double last = samples.back().s;
            NewModel m;
            int count = 0, index = 0;
            V3 lo{ 1e30, 1e30, 1e30 }, hi{ -1e30, -1e30, -1e30 };
            auto flush = [&]() {
                if (m.quads.empty()) return;
                char name[16];
                snprintf(name, sizeof(name), "SCNXTREES%02d", index++);
                m.name = name;
                m.origin = { std::round((lo[0] + hi[0]) / 2), std::round((lo[1] + hi[1]) / 2), std::round((lo[2] + hi[2]) / 2) };
                models.push_back(std::move(m));
                m = NewModel();
                lo = { 1e30, 1e30, 1e30 };
                hi = { -1e30, -1e30, -1e30 };
            };
            for (int k = 1; k * card_window < last + card_window; k++) {
                double s_lo = k * card_window - card_window, s_hi = k * card_window;
                if (s_lo < built || s_hi > last - 30) continue;
                for (const Tri* tr : cards) {
                    OutQuad q;
                    q.count = 3;
                    q.state = tr->state;
                    for (int j = 0; j < 3; j++) {
                        const Vtx& v = tr->v[j];
                        Sample fr = frame_at(cut - along(v.p) + k * card_window);
                        q.v[j].p = add(add(fr.c, mul(fr.n, across(v.p))), V3{ 0, v.p[1] + fr.h, 0 });
                        q.v[j].st[0] = int32_t(v.st[0]);
                        q.v[j].st[1] = int32_t(v.st[1]);
                        memcpy(q.v[j].c, v.c, 4);
                        for (int x = 0; x < 3; x++) { lo[x] = std::min(lo[x], q.v[j].p[x]); hi[x] = std::max(hi[x], q.v[j].p[x]); }
                    }
                    q.v[3] = q.v[2];
                    m.quads.push_back(q);
                }
                count++;
                if (hi[0] - lo[0] > 1500 || hi[2] - lo[2] > 1500) flush();
            }
            flush();
            char line[120];
            snprintf(line, sizeof(line), "start: %zu tree card triangles set down %d times\n", cards.size(), count);
            report += line;
        }

        // Barricades across the road (Type III: two posts, three striped boards, lamps), the middle one with the sign.
        {
            const Sample* sb = &samples[0];
            for (const Sample& sm : samples) if (std::fabs(sm.s - barricade) < std::fabs(sb->s - barricade)) sb = &sm;
            double road_y = s0[1] - grade * sb->s;
            // Where they stand: on the sweep (`bs`, over the cut's profile), or for Highway 1 on the stock road 420 ft
            // ahead of spine 0, on the start's side of the hill's crest (behind the crest, 340 ft back, the road from
            // the grid 1,250 ft ahead hid them). There the road's own profile is under them and the carriageway
            // follows the spine across.
            Sample bs = *sb;
            const std::vector<Piece>* under = &pieces;
            std::vector<Piece> ahead;
            double bar_y = road_y, run_shift = 0;
            if (t == 2) {
                constexpr double at = 420;
                for (size_t i = 1; i < spine.size(); i++) {
                    double a0 = along(spine[i - 1]), a1 = along(spine[i]);
                    if (a0 > at || a1 < at || a1 <= a0) continue;
                    V3 sp = add(spine[i - 1], mul(sub(spine[i], spine[i - 1]), (at - a0) / (a1 - a0)));
                    bar_y = sp[1];
                    run_shift = across(sp);
                    break;
                }
                bs.s = cut - at;
                bs.c = add(s0, mul(f, at));
                bs.c[1] = 0;
                bs.t = mul(f, -1);
                bs.n = n;
                bs.turn = 0;
                bs.h = 0;
                ahead = section(at);
                under = &ahead;
            }
            // Ground height under a point across: the highest flat piece covering it.
            auto ground = [&](double l) {
                double best = -1e30;
                for (const Piece& pc : *under) {
                    double a = pc.e[0].l, b = pc.e[1].l;
                    if (std::fabs(pc.e[0].y - pc.e[1].y) > std::fabs(a - b)) continue;
                    if (l < std::min(a, b) || l > std::max(a, b) || a == b) continue;
                    double y = pc.e[0].y + (pc.e[1].y - pc.e[0].y) * (l - a) / (b - a);
                    if (y < bar_y + 10) best = std::max(best, y);
                }
                return best > -1e29 ? best + bs.h : bar_y;
            };
            // One row per carriageway or median (so no barricade straddles a level change), each up to 10.5 ft.
            struct Slot { double l0, l1; bool sign; };
            std::vector<Slot> slots;
            {
                double widest = 0;
                for (const auto& [a, b] : road_runs) widest = std::max(widest, b - a);
                for (auto [a, b] : road_runs) {
                    a += run_shift;
                    b += run_shift;
                    int count = std::max(1, int(std::ceil((b - a) / 10.5)));
                    double each = (b - a) / count;
                    // The sign on the middle barricade of each carriageway as wide as the widest (not a median).
                    for (int i = 0; i < count; i++) slots.push_back({ a + i * each + 0.5, a + (i + 1) * each - 0.5, i == count / 2 && b - a >= widest * 0.6 });
                }
            }
            // Shade like the road under it.
            double shade = 0;
            int shaded = 0;
            for (const Piece& pc : *under) {
                if (std::fabs(pc.e[0].y - bar_y) < 3) { shade += (pc.e[0].c[0] + pc.e[0].c[1] + pc.e[0].c[2]) / 3; shaded++; }
            }
            shade = shaded ? std::clamp(shade / shaded / 255.0 * 1.15, 0.55, 1.0) : 0.9;
            NewModel m;
            m.name = "SCNXBARRICADE";
            V3 up{ 0, 1, 0 };
            V3 fwd = mul(bs.t, -1);    // toward the start line
            const V3 light = norm(V3{ 0.30, 0.80, 0.52 });
            auto box = [&](V3 center, V3 ax, V3 ay, V3 az, double hx, double hy, double hz, int state, double u0, double u1,
                           double v0, double v1, std::array<double, 3> tint, bool front_back_only) {
                // Six faces (or front and back), each a quad with the texture rectangle (u0..u1, v0..v1) in texels.
                struct Face { V3 n, a, b; double ha, hb, hn; };
                std::vector<Face> faces = { { az, ax, ay, hx, hy, hz }, { mul(az, -1), mul(ax, -1), ay, hx, hy, hz } };
                if (!front_back_only) {
                    faces.push_back({ ax, mul(az, -1), ay, hz, hy, hx });
                    faces.push_back({ mul(ax, -1), az, ay, hz, hy, hx });
                    faces.push_back({ ay, ax, mul(az, -1), hx, hz, hy });
                    faces.push_back({ mul(ay, -1), ax, az, hx, hz, hy });
                }
                for (const Face& fc : faces) {
                    OutQuad q;
                    q.state = state;
                    V3 c = add(center, mul(fc.n, fc.hn));
                    V3 corners[4] = { add(add(c, mul(fc.a, -fc.ha)), mul(fc.b, -fc.hb)), add(add(c, mul(fc.a, fc.ha)), mul(fc.b, -fc.hb)),
                                      add(add(c, mul(fc.a, fc.ha)), mul(fc.b, fc.hb)), add(add(c, mul(fc.a, -fc.ha)), mul(fc.b, fc.hb)) };
                    double uv[4][2] = { { u0, v1 }, { u1, v1 }, { u1, v0 }, { u0, v0 } };
                    double lit = 0.72 + 0.28 * std::max(0.0, dot(norm(fc.n), light));
                    for (int k = 0; k < 4; k++) {
                        q.v[k].p = corners[k];
                        q.v[k].st[0] = int32_t(std::lround((uv[k][0] - 0.5) * 32));
                        q.v[k].st[1] = int32_t(std::lround((uv[k][1] - 0.5) * 32));
                        for (int x = 0; x < 3; x++) q.v[k].c[x] = uint8_t(std::clamp(tint[x] * lit * shade * 255.0, 0.0, 255.0));
                        q.v[k].c[3] = 255;
                    }
                    // Counter-clockwise seen from the face's front.
                    if (dot(cross(sub(q.v[1].p, q.v[0].p), sub(q.v[2].p, q.v[0].p)), fc.n) < 0) {
                        std::swap(q.v[1], q.v[3]);
                    }
                    m.quads.push_back(q);
                }
            };
            V3 across_v = bs.n;
            const std::array<double, 3> white{ 1, 1, 1 }, gray{ 0.75, 0.75, 0.72 }, amber{ 1.0, 0.75, 0.15 };
            for (const Slot& slot : slots) {
                double lm = (slot.l0 + slot.l1) / 2, half = (slot.l1 - slot.l0) / 2;
                // The boards stay level over the higher foot; each leg reaches down to the ground under it (on a
                // sloped median one leg is longer).
                V3 feet[2];
                for (int side = 0; side < 2; side++) {
                    double lp = lm + (side ? 1 : -1) * (half - 0.6);
                    double gy = ground(lp);
                    feet[side] = place(bs, lp, gy - bs.h);
                    feet[side][1] = gy;
                }
                double top = std::max(feet[0][1], feet[1][1]);
                V3 base = mul(add(feet[0], feet[1]), 0.5);
                base[1] = top;
                for (int side = 0; side < 2; side++) {
                    const V3& foot = feet[side];
                    double leg = (top + 5.2 - foot[1]) / 2;
                    box(add(foot, V3{ 0, leg, 0 }), across_v, up, fwd, 0.17, leg, 0.17, own_stripes, 2, 30, 9, 15, gray, false);
                    box(add(foot, V3{ 0, 0.08, 0 }), across_v, up, fwd, 0.25, 0.08, 1.1, own_stripes, 2, 30, 9, 15, gray, false);
                    V3 lamp = foot;
                    lamp[1] = top + 5.35;
                    box(lamp, across_v, up, fwd, 0.22, 0.17, 0.22, own_stripes, 2, 30, 9, 15, amber, false);
                }
                for (double by : { 1.4, 2.8, 4.2 }) {
                    double u_len = (half * 2) / 0.33;     // stripes: 4 texels a band, about 16 in
                    box(add(base, V3{ 0, by, 0 }), across_v, up, fwd, half, 0.4, 0.06, own_stripes, 0, u_len, 0, 8, white, false);
                }
                if (slot.sign) {
                    // The sign is bolted on the face of the upper two boards, toward the start. Its text runs to the
                    // right of someone looking at it from there: `across_v` is their left (checked in game).
                    box(add(add(base, V3{ 0, 3.5, 0 }), mul(fwd, 0.11)), mul(across_v, -1), up, fwd, 2.4, 1.2, 0.04, own_sign, 0, 128,
                        0, 32, white, true);
                }
            }
            V3 lo{ 1e30, 1e30, 1e30 }, hi{ -1e30, -1e30, -1e30 };
            for (const OutQuad& q : m.quads) for (const OutVtx& v : q.v) for (int x = 0; x < 3; x++) { lo[x] = std::min(lo[x], v.p[x]); hi[x] = std::max(hi[x], v.p[x]); }
            m.origin = { std::round((lo[0] + hi[0]) / 2), std::round((lo[1] + hi[1]) / 2), std::round((lo[2] + hi[2]) / 2) };
            models.push_back(std::move(m));

            // Collision for all of it: every piece of the cross-section along the whole sweep, its open outer edges
            // and its end walled, and a wall across it at the barricades. A piece takes the surface under it at the
            // cut; one with none there takes its nearest flat neighbor's if flat (else type 0), or is a wall (5).
            std::vector<int> flags_all = piece_flags;
            std::vector<uint16_t> info_all = piece_info;
            for (size_t i = 0; i < pieces.size(); i++) {
                if (flags_all[i] >= 0) continue;
                const Piece& pc = pieces[i];
                bool flat = std::fabs(pc.e[0].y - pc.e[1].y) < std::fabs(pc.e[0].l - pc.e[1].l);
                if (!flat) { flags_all[i] = 5; continue; }
                double ml = (pc.e[0].l + pc.e[1].l) / 2, best = 1e30;
                flags_all[i] = 0;
                for (size_t j = 0; j < pieces.size(); j++) {
                    if (piece_flags[j] < 0 || (piece_flags[j] & 0xF) > 2) continue;
                    double d = std::fabs((pieces[j].e[0].l + pieces[j].e[1].l) / 2 - ml);
                    if (d < best) { best = d; flags_all[i] = piece_flags[j]; info_all[i] = piece_info[j]; }
                }
            }
            double lmin = 1e30, lmax = -1e30, ylo = 1e30, yhi = -1e30;
            for (const Piece& pc : pieces) for (const ProfileEnd& e : pc.e) {
                if (pc.bare) continue;
                lmin = std::min(lmin, e.l); lmax = std::max(lmax, e.l);
                ylo = std::min(ylo, e.y); yhi = std::max(yhi, e.y);
            }
            // Collision needs no texture: neighbors on one line of the profile with the same surface are one polygon
            // (the road's lane lines and patches would otherwise crowd the leaves), and a piece lying on one already
            // taken (a decal) is dropped.
            struct CPiece { double l0, y0, l1, y1; int flags; uint16_t info; bool under; V3 normal; bool carried = false; };
            std::vector<CPiece> cps;
            for (size_t i = 0; i < pieces.size(); i++) {
                const Piece& pc = pieces[i];
                if (pc.bare) continue;
                CPiece c{ pc.e[0].l, pc.e[0].y, pc.e[1].l, pc.e[1].y, flags_all[i], info_all[i], piece_flags[i] >= 0, pc.normal,
                           pc.lead > 0 && std::fabs(pc.e[1].l - pc.e[0].l) < 0.2 * std::fabs(pc.e[1].y - pc.e[0].y) + 0.5 };
                if (c.l0 > c.l1 || (c.l0 == c.l1 && c.y0 > c.y1)) { std::swap(c.l0, c.l1); std::swap(c.y0, c.y1); }
                cps.push_back(c);
            }
            std::sort(cps.begin(), cps.end(), [](const CPiece& a, const CPiece& b) { return a.l0 < b.l0; });
            auto off_line = [](const CPiece& c, double l, double y) {
                double dl = c.l1 - c.l0, dy = c.y1 - c.y0, n = std::sqrt(dl * dl + dy * dy);
                return n < 1e-9 ? 1e30 : std::fabs((l - c.l0) * dy - (y - c.y0) * dl) / n;
            };
            std::vector<CPiece> merged;
            for (const CPiece& c : cps) {
                bool done = false;
                for (CPiece& m : merged) {
                    if (m.flags != c.flags || m.info != c.info || m.under != c.under || m.carried != c.carried) continue;
                    if (off_line(m, c.l0, c.y0) > 0.05 || off_line(m, c.l1, c.y1) > 0.05) continue;
                    // Positions along m's line: overlapping or touching spans join.
                    double dl = m.l1 - m.l0, dy = m.y1 - m.y0, n2 = dl * dl + dy * dy;
                    double ta = ((c.l0 - m.l0) * dl + (c.y0 - m.y0) * dy) / n2, tb = ((c.l1 - m.l0) * dl + (c.y1 - m.y0) * dy) / n2;
                    double gap = 0.05 / std::sqrt(n2);
                    if (std::max(ta, tb) < -gap || std::min(ta, tb) > 1 + gap) continue;
                    double lo = std::min({ 0.0, ta, tb }), hi = std::max({ 1.0, ta, tb });
                    CPiece g = m;
                    g.l0 = m.l0 + dl * lo; g.y0 = m.y0 + dy * lo;
                    g.l1 = m.l0 + dl * hi; g.y1 = m.y0 + dy * hi;
                    m = g;
                    done = true;
                    break;
                }
                if (!done) merged.push_back(c);
            }
            // Rounded profiles (curbs, banks) come in many short pieces: two joined end to end with the same surface
            // become one while their shared point lies within 0.3 ft of the joined line.
            for (bool joined = true; joined;) {
                joined = false;
                for (size_t i = 0; i < merged.size() && !joined; i++) {
                    for (size_t j = 0; j < merged.size() && !joined; j++) {
                        if (i == j) continue;
                        CPiece& m = merged[i];
                        const CPiece& c = merged[j];
                        if (m.flags != c.flags || m.info != c.info || m.under != c.under || m.carried != c.carried) continue;
                        CPiece g = m;
                        double sl, sy;   // the shared point
                        if (std::fabs(m.l1 - c.l0) <= 0.05 && std::fabs(m.y1 - c.y0) <= 0.05) { sl = m.l1; sy = m.y1; g.l1 = c.l1; g.y1 = c.y1; }
                        else if (!m.carried) continue;
                        // A carried tree wall's stacked pieces (two triangles a plane) come in either order.
                        else if (std::fabs(m.l1 - c.l1) <= 0.05 && std::fabs(m.y1 - c.y1) <= 0.05) { sl = m.l1; sy = m.y1; g.l1 = c.l0; g.y1 = c.y0; }
                        else if (std::fabs(m.l0 - c.l0) <= 0.05 && std::fabs(m.y0 - c.y0) <= 0.05) { sl = m.l0; sy = m.y0; g.l0 = c.l1; g.y0 = c.y1; }
                        else if (std::fabs(m.l0 - c.l1) <= 0.05 && std::fabs(m.y0 - c.y1) <= 0.05) { sl = m.l0; sy = m.y0; g.l0 = c.l0; g.y0 = c.y0; }
                        else continue;
                        if (off_line(g, sl, sy) > (m.carried ? 3.0 : 0.3)) continue;   // a tree wall needn't be exact
                        m = g;
                        merged.erase(merged.begin() + j);
                        joined = true;
                    }
                }
            }
            // The track's own walls go on too, as its collision has them (track_walls_at): a car's body points pass
            // over a wall only as tall as the drawing. Each wall crossing the plane 3 ft ahead of the cut (they stop
            // about at it) gives a piece with its flags and facing.
            {
                std::vector<CPiece> tall;
                for (const WallCross& wc : track_walls_at(cut + 3)) {   // along grows into the track
                    // A wall facing along the road would sweep into a slab across it.
                    if (std::fabs(dot(wc.front, f)) > 0.6) continue;
                    CPiece c{ wc.l0, wc.y0, wc.l1, wc.y1, int(wc.flags & ~0x3000), wc.info, true, wc.front };
                    if (c.l0 > c.l1) { std::swap(c.l0, c.l1); std::swap(c.y0, c.y1); }
                    // A wall quad comes as two triangles: their pieces on one line with one surface are one.
                    bool joined = false;
                    for (CPiece& t : tall) {
                        if (t.flags != c.flags || t.info != c.info) continue;
                        if (off_line(t, c.l0, c.y0) > 0.05 || off_line(t, c.l1, c.y1) > 0.05) continue;
                        double dl = t.l1 - t.l0, dy = t.y1 - t.y0, n2 = dl * dl + dy * dy;
                        double ta = ((c.l0 - t.l0) * dl + (c.y0 - t.y0) * dy) / n2, tb = ((c.l1 - t.l0) * dl + (c.y1 - t.y0) * dy) / n2;
                        double gap = 0.05 / std::sqrt(n2);
                        if (std::max(ta, tb) < -gap || std::min(ta, tb) > 1 + gap) continue;
                        double lo = std::min({ 0.0, ta, tb }), hi = std::max({ 1.0, ta, tb });
                        CPiece g = t;
                        g.l0 = t.l0 + dl * lo; g.y0 = t.y0 + dy * lo;
                        g.l1 = t.l0 + dl * hi; g.y1 = t.y0 + dy * hi;
                        t = g;
                        joined = true;
                        break;
                    }
                    if (!joined) tall.push_back(c);
                }
                // A drawn wall lying on one of those (within 0.3 ft of its line, inside its span) is left to it.
                std::vector<CPiece> kept;
                for (const CPiece& c : merged) {
                    bool on = false;
                    if ((c.flags & 0xF) > 2) {
                        for (const CPiece& t : tall) {
                            if (off_line(t, c.l0, c.y0) > 0.3 || off_line(t, c.l1, c.y1) > 0.3) continue;
                            double dl = t.l1 - t.l0, dy = t.y1 - t.y0, n2 = dl * dl + dy * dy;
                            double ta = ((c.l0 - t.l0) * dl + (c.y0 - t.y0) * dy) / n2, tb = ((c.l1 - t.l0) * dl + (c.y1 - t.y0) * dy) / n2;
                            if (std::min(ta, tb) >= -0.01 && std::max(ta, tb) <= 1.01) { on = true; break; }
                        }
                    }
                    if (!on) kept.push_back(c);
                }
                kept.insert(kept.end(), tall.begin(), tall.end());
                merged = kept;
            }
            // The path: a lead of 20 ft ahead of the cut (where the track's own collision can stop short of it off
            // the road; only pieces with no surface under them at the cut take it), then the samples, split into
            // segments shared by every piece (so neighbors share their edges): a segment goes on while the sweep's
            // middle and both edges stay within `tol` of straight lines and its full-width quad within tol / 2 of
            // flat. Coarser when the file's polygon numbers (13 bits in a leaf entry) run short.
            std::vector<Sample> path = { samples[0] };
            {
                const Sample& a0 = samples[0];
                const Sample& a1 = samples[1];
                double k = 20 / std::max(1e-6, a1.s - a0.s);
                path[0].s = a0.s - 20;
                path[0].c = add(a0.c, mul(sub(a0.c, a1.c), k));
                path[0].h = a0.h + (a0.h - a1.h) * k;
            }
            path.insert(path.end(), samples.begin(), samples.end());
            auto straight_enough = [&](size_t i, size_t j, double tol) {
                const double ls[3] = { lmin, 0.0, lmax };
                for (double l : ls) {
                    V3 pa = place(path[i], l, 0), pb = place(path[j], l, 0);
                    for (size_t k = i + 1; k < j; k++) {
                        double u = (path[k].s - path[i].s) / std::max(1e-6, path[j].s - path[i].s);
                        if (len(sub(place(path[k], l, 0), add(pa, mul(sub(pb, pa), u)))) > tol) return false;
                    }
                }
                V3 q0 = place(path[i], lmin, 0), q1 = place(path[i], lmax, 0), q2 = place(path[j], lmax, 0), q3 = place(path[j], lmin, 0);
                V3 nq = norm(cross(sub(q1, q0), sub(q2, q0)));
                return std::fabs(dot(sub(q3, q0), nq)) <= tol / 2;
            };
            bool open_lo = false, open_hi = false;
            for (const CPiece& c : merged) {
                bool flat = (c.flags & 0xF) <= 2;
                if (flat && c.l0 <= lmin + 0.01) open_lo = true;
                if (flat && c.l1 >= lmax - 0.01) open_hi = true;
            }
            // The collision for segments `cuts`.
            auto build = [&](const std::vector<size_t>& cuts, std::vector<NewPoly>& out) {
                for (size_t ci = 0; ci + 1 < cuts.size(); ci++) {
                    const Sample& sa = path[cuts[ci]];
                    const Sample& sb2 = path[cuts[ci + 1]];
                    bool lead = ci == 0;
                    for (const CPiece& pc : merged) {
                        if (lead && pc.under) continue;
                        if (std::hypot(pc.l1 - pc.l0, pc.y1 - pc.y0) < 0.1) continue;   // a sliver of the profile
                        NewPoly p;
                        p.v = { place(sa, pc.l0, pc.y0), place(sa, pc.l1, pc.y1), place(sb2, pc.l1, pc.y1), place(sb2, pc.l0, pc.y0) };
                        p.flags = uint16_t(pc.flags);
                        p.info_hi = pc.info;
                        if ((p.flags & 0xF) <= 2) p.front = { 0, 1, 0 };
                        else {
                            // A wall faces as its triangles did at the cut, turned with the path.
                            const Sample& s0f = samples[0];
                            p.front = add(add(mul(sa.n, dot(pc.normal, s0f.n)), mul(sa.t, dot(pc.normal, s0f.t))), V3{ 0, pc.normal[1], 0 });
                        }
                        bool t1 = len(cross(sub(p.v[1], p.v[0]), sub(p.v[2], p.v[0]))) >= 0.5;
                        bool t2 = len(cross(sub(p.v[2], p.v[0]), sub(p.v[3], p.v[0]))) >= 0.5;
                        if (!t1 && !t2) continue;
                        // A file polygon is flat (its plane from its first three corners): a quad twisted over 0.15 ft by the bend and
                        // the grade (a wide sloped piece twists most) goes in as two triangles, so neighbors meet exactly.
                        double twist = t1 ? std::fabs(dot(sub(p.v[3], p.v[0]), norm(cross(sub(p.v[1], p.v[0]), sub(p.v[2], p.v[0]))))) : 1.0;
                        if (twist <= 0.15) { out.push_back(p); continue; }
                        NewPoly a = p, b = p;
                        a.v = { p.v[0], p.v[1], p.v[2] };
                        b.v = { p.v[0], p.v[2], p.v[3] };
                        if (t1) out.push_back(a);
                        if (t2) out.push_back(b);
                    }
                    // The open outer edges: a wall from 10 ft under to 30 ft over the edge, facing in.
                    for (int side = 0; side < 2 && !lead; side++) {
                        if (side == 0 ? !open_lo : !open_hi) continue;
                        double l = side == 0 ? lmin : lmax, ey = 0;
                        for (const CPiece& c : merged) {
                            if (std::fabs(c.l0 - l) < 0.01) ey = c.y0;
                            if (std::fabs(c.l1 - l) < 0.01) ey = c.y1;
                        }
                        NewPoly w;
                        w.v = { place(sa, l, ey - 10), place(sb2, l, ey - 10), place(sb2, l, ey + 30), place(sa, l, ey + 30) };
                        w.flags = 5;
                        w.info_hi = 0;
                        w.front = mul(sa.n, side == 0 ? 1.0 : -1.0);
                        out.push_back(w);
                    }
                }
            };
            // The finest segments whose polygons fit the file (their numbers are 13 bits in a leaf entry).
            int budget = 8190 - int(col.polys) - int(polys.size()) - 2;
            std::vector<size_t> cuts;
            std::vector<NewPoly> built;
            for (double tol = 0.25; ; tol *= 2) {
                cuts = { 0, 1 };   // path[0] the lead's start, path[1] = samples[0], the cut
                size_t i = 1;
                while (i + 1 < path.size()) {
                    size_t j = i + 1;
                    while (j + 1 < path.size() && straight_enough(i, j + 1, tol)) j++;
                    cuts.push_back(j);
                    i = j;
                }
                built.clear();
                build(cuts, built);
                char t[160];
                snprintf(t, sizeof(t), "start: collision tolerance %.2f ft: %zu segments, %zu pieces, %zu polygons of %d free\n", tol,
                         cuts.size() - 1, merged.size(), built.size(), budget);
                report += t;
                if (int(built.size()) <= budget || tol > 64) break;
            }
            polys.insert(polys.end(), built.begin(), built.end());
            // The carried-on surfaces' stretch from where they were taken to the collision's lead (20 ft ahead).
            for (size_t i = 0; i < pieces.size(); i++) {
                const Piece& pc = pieces[i];
                if (pc.lead <= 20 || pc.bare) continue;
                Sample a = lead_sample(pc.lead), b = path[0];
                NewPoly w;
                w.v = { place(a, pc.e[0].l, pc.e[0].y), place(a, pc.e[1].l, pc.e[1].y), place(b, pc.e[1].l, pc.e[1].y), place(b, pc.e[0].l, pc.e[0].y) };
                w.flags = uint16_t(flags_all[i]);
                w.info_hi = info_all[i];
                w.front = (w.flags & 0xF) <= 2 ? V3{ 0, 1, 0 } : pc.normal;
                polys.push_back(w);
            }
            // Los Angeles' left edge where it turns back out from the stock edge's end to its own line: walls along the
            // turn, one a sample (the straight collision of the sweep stands on that line).
            for (size_t i = 0; i < pieces.size(); i++) {
                const Piece& pc = pieces[i];
                if (pc.pull_d <= 0 || pc.bare) continue;
                for (size_t k = 0; k + 1 < path.size(); k++) {
                    const Sample& a = path[k];
                    const Sample& b = path[k + 1];
                    if (b.s <= pc.pull_d || a.s >= pc.pull_d + 30) continue;
                    NewPoly w;
                    auto at = [&](const Sample& sm, int e) { return place(sm, pc.e[e].l + drift_shift(pc, sm.s), pc.e[e].y); };
                    w.v = { at(a, 0), at(a, 1), at(b, 1), at(b, 0) };
                    w.flags = uint16_t(flags_all[i]);
                    w.info_hi = info_all[i];
                    w.front = (w.flags & 0xF) <= 2 ? V3{ 0, 1, 0 } : pc.normal;
                    polys.push_back(w);
                }
            }
            auto across_wall = [&](const Sample& at, V3 front, double base_y) {
                double wy0 = std::min(ylo, base_y - 30), wy1 = std::max(yhi, base_y + 60);
                NewPoly w;
                w.v = { place(at, lmin, wy0), place(at, lmax, wy0), place(at, lmax, wy1), place(at, lmin, wy1) };
                w.flags = 5;
                w.info_hi = 0;
                w.front = front;
                polys.push_back(w);
            };
            across_wall(bs, fwd, bar_y);
            across_wall(samples.back(), mul(samples.back().t, -1), road_y);

            // Every quad drawn in the sweep must lie on a collision polygon (its middle within 0.6 ft of one's plane,
            // inside its edges).
            int quads = 0, bare = 0;
            for (const NewModel& m : models) {
                if (m.name.rfind("SCNXBACK", 0) != 0) continue;
                for (const OutQuad& q : m.quads) {
                    V3 c{ 0, 0, 0 };
                    for (int k = 0; k < q.count; k++) c = add(c, mul(q.v[k].p, 1.0 / q.count));
                    if (q.bare || len(cross(sub(q.v[1].p, q.v[0].p), sub(q.v[2].p, q.v[0].p))) < 0.01) continue;
                    quads++;
                    bool on = false;
                    for (const NewPoly& p : polys) {
                        V3 pn = norm(cross(sub(p.v[1], p.v[0]), sub(p.v[2], p.v[0])));
                        if (std::fabs(dot(sub(c, p.v[0]), pn)) > 0.6) continue;
                        double sign = 0;
                        bool in = true;
                        for (size_t k = 0; k < p.v.size() && in; k++) {
                            const V3& a = p.v[k];
                            const V3& b = p.v[(k + 1) % p.v.size()];
                            double e = dot(cross(sub(b, a), sub(c, a)), pn) / std::max(1e-9, len(sub(b, a)));
                            if (std::fabs(e) < 0.1) continue;
                            if (sign == 0) sign = e;
                            else if ((e > 0) != (sign > 0)) in = false;
                        }
                        if (in) { on = true; break; }
                    }
                    if (!on) bare++;
                }
            }
            char cov[160];
            snprintf(cov, sizeof(cov), "start: collision %zu segments, %d of %d drawn quads without collision\n", cuts.size() - 1,
                     bare, quads);
            report += cov;
        }
        char line[200];
        snprintf(line, sizeof(line), "start: cut %.1f ft behind spine 0, %zu pieces, road %.1f..%.1f, straight %.0f ft, %zu samples\n",
                 -cut, pieces.size(), road0, road1, straight, samples.size());
        report += line;
        return true;
    }

    // ---- the sea --------------------------------------------------------------------------------------------------
    bool sea(int t, const Scene& scene, const Collision& col_file, std::vector<NewModel>& models, std::vector<NewPoly>& polys,
             std::set<int>& water_states, std::string& report) {
        // Water: blue-textured triangles facing up.
        double area = 0;
        std::vector<std::pair<double, double>> heights;     // (y, area)
        const Tri* biggest = nullptr;
        double biggest_area = 0;
        double col[4] = { 0, 0, 0, 0 };
        for (const Tri& tr : scene.tris) {
            if (!scene.states[size_t(tr.state)].blue) continue;
            V3 c = cross(sub(tr.v[1].p, tr.v[0].p), sub(tr.v[2].p, tr.v[0].p));
            double a = len(c) / 2;
            if (a < 1 || std::fabs(c[1]) / (2 * a) < 0.97) continue;
            water_states.insert(tr.state);
            area += a;
            heights.push_back({ (tr.v[0].p[1] + tr.v[1].p[1] + tr.v[2].p[1]) / 3, a });
            for (int k = 0; k < 4; k++) col[k] += a * (tr.v[0].c[k] + tr.v[1].c[k] + tr.v[2].c[k]) / 3;
            if (a > biggest_area) { biggest_area = a; biggest = &tr; }
        }
        if (!sea_for(t)) { water_states.clear(); return true; }
        if (area < 1e6 || biggest == nullptr) { report += "sea: no water\n"; water_states.clear(); return true; }
        std::sort(heights.begin(), heights.end());
        double half = 0, level = heights.front().first;
        for (const auto& [y, a] : heights) { half += a; if (half >= area / 2) { level = y; break; } }
        double y = level - 1.5;
        for (int k = 0; k < 4; k++) col[k] /= area;
        // The track's own water collision: the commonest surface under the water triangles' middles.
        std::map<uint32_t, double> surfaces;   // flags << 16 | info -> area
        double no_surface = 0;
        for (const Tri& tr : scene.tris) {
            if (!water_states.contains(tr.state)) continue;
            V3 c = mul(add(add(tr.v[0].p, tr.v[1].p), tr.v[2].p), 1.0 / 3);
            double a = len(cross(sub(tr.v[1].p, tr.v[0].p), sub(tr.v[2].p, tr.v[0].p))) / 2;
            uint16_t fl, in;
            double gh;
            if (ground_at(col_file, c[0], c[1], c[2], 3.0, fl, in, gh, 15) && std::fabs(gh - c[1]) < 3) surfaces[uint32_t(fl) << 16 | in] += a;
            else no_surface += a;
        }
        {
            char line[200];
            for (const auto& [k, a] : surfaces) {
                snprintf(line, sizeof(line), "sea: water collision flags %04X info %04X over %.0f sq ft\n", k >> 16, k & 0xFFFF, a);
                report += line;
            }
            snprintf(line, sizeof(line), "sea: no collision under %.0f sq ft of water\n", no_surface);
            report += line;
        }

        // Texture mapping from the biggest water triangle: st as an affine function of x and z.
        const Tri& w = *biggest;
        double x1 = w.v[1].p[0] - w.v[0].p[0], z1 = w.v[1].p[2] - w.v[0].p[2];
        double x2 = w.v[2].p[0] - w.v[0].p[0], z2 = w.v[2].p[2] - w.v[0].p[2];
        double det = x1 * z2 - x2 * z1;
        double gx[2], gz[2];
        for (int a = 0; a < 2; a++) {
            double d1 = w.v[1].st[a] - w.v[0].st[a], d2 = w.v[2].st[a] - w.v[0].st[a];
            gx[a] = (d1 * z2 - d2 * z1) / det;
            gz[a] = (x1 * d2 - x2 * d1) / det;
        }
        const State& ws = scene.states[size_t(w.state)];

        // Cells: 256 ft over the track's extent plus 9,000 ft; a cell is left out where the track has anything but
        // water lower than 2 ft over the sea.
        double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
        for (const Tri& tr : scene.tris) for (const Vtx& v : tr.v) {
            lo[0] = std::min(lo[0], v.p[0]); hi[0] = std::max(hi[0], v.p[0]);
            lo[1] = std::min(lo[1], v.p[2]); hi[1] = std::max(hi[1], v.p[2]);
        }
        constexpr double cell = 256, margin = 9000, near_margin = 4000;
        double ox = std::floor((lo[0] - margin) / cell) * cell, oz = std::floor((lo[1] - margin) / cell) * cell;
        int nx = int(std::ceil((hi[0] + margin - ox) / cell)), nz = int(std::ceil((hi[1] + margin - oz) / cell));
        std::vector<uint8_t> blocked(size_t(nx) * nz, 0);
        for (const Tri& tr : scene.tris) {
            if (water_states.contains(tr.state)) continue;
            double ymin = std::min({ tr.v[0].p[1], tr.v[1].p[1], tr.v[2].p[1] });
            if (ymin > level + 2) continue;
            double x0 = std::min({ tr.v[0].p[0], tr.v[1].p[0], tr.v[2].p[0] }), x1b = std::max({ tr.v[0].p[0], tr.v[1].p[0], tr.v[2].p[0] });
            double z0 = std::min({ tr.v[0].p[2], tr.v[1].p[2], tr.v[2].p[2] }), z1b = std::max({ tr.v[0].p[2], tr.v[1].p[2], tr.v[2].p[2] });
            for (int i = std::max(0, int((x0 - ox) / cell)); i <= std::min(nx - 1, int((x1b - ox) / cell)); i++) {
                for (int j = std::max(0, int((z0 - oz) / cell)); j <= std::min(nz - 1, int((z1b - oz) / cell)); j++) {
                    blocked[size_t(j) * nx + i] = 1;
                }
            }
        }
        // Near region (fine tiles) and far tiles.
        auto in_near = [&](double x, double z) {
            return x >= lo[0] - near_margin && x <= hi[0] + near_margin && z >= lo[1] - near_margin && z <= hi[1] + near_margin;
        };
        int index = 0;
        auto tile = [&](double tx0, double tz0, double size, double scale, bool near) {
            // Greedy rectangles of open cells in this tile.
            int i0 = int(std::lround((tx0 - ox) / cell)), j0 = int(std::lround((tz0 - oz) / cell));
            int n = int(std::lround(size / cell));
            std::vector<uint8_t> used(size_t(n) * n, 0);
            NewModel m;
            char name[16];
            snprintf(name, sizeof(name), "SCNXSEA%03d", index);
            m.name = name;
            m.scale = scale;
            m.origin = { tx0 + size / 2, y, tz0 + size / 2 };
            auto open = [&](int a, int b) {
                int i = i0 + a, j = j0 + b;
                if (i < 0 || j < 0 || i >= nx || j >= nz) return false;
                if (used[size_t(b) * n + a] || blocked[size_t(j) * nx + i]) return false;
                double cx = ox + (i + 0.5) * cell, cz = oz + (j + 0.5) * cell;
                return in_near(cx, cz) == near;
            };
            for (int b = 0; b < n; b++) for (int a = 0; a < n; a++) {
                if (!open(a, b)) continue;
                int w2 = 1;
                while (a + w2 < n && open(a + w2, b)) w2++;
                int h2 = 1;
                for (bool ok = true; ok && b + h2 < n; ) {
                    for (int k = 0; k < w2; k++) ok = ok && open(a + k, b + h2);
                    if (ok) h2++;
                }
                for (int bb = 0; bb < h2; bb++) for (int aa = 0; aa < w2; aa++) used[size_t(b + bb) * n + a + aa] = 1;
                // The rectangle in quads small enough for the texture coordinates' range (1,000 texels across).
                double rx0 = tx0 + a * cell, rz0 = tz0 + b * cell, rx1 = rx0 + w2 * cell, rz1 = rz0 + h2 * cell;
                // Its collision (near tiles, the ones a car can reach): Rush 2's water hazard (type 3: a wheel or body
                // point touching it wrecks the car, which is reset), where California Speed's own water mostly has
                // none and a car sinks through it until the reset.
                if (near) {
                    NewPoly p;
                    p.v = { V3{ rx0, y, rz0 }, V3{ rx1, y, rz0 }, V3{ rx1, y, rz1 }, V3{ rx0, y, rz1 } };
                    p.flags = 3;
                    p.info_hi = 0;
                    p.front = { 0, 1, 0 };
                    polys.push_back(p);
                }
                double rate = std::max({ std::fabs(gx[0]), std::fabs(gz[0]), std::fabs(gx[1]), std::fabs(gz[1]) }) / 32.0;
                double piece = std::max(cell, std::floor(450.0 / std::max(rate, 1e-6) / cell) * cell);
                for (double qx = rx0; qx < rx1 - 1; qx += piece) for (double qz = rz0; qz < rz1 - 1; qz += piece) {
                    double qx1 = std::min(rx1, qx + piece), qz1 = std::min(rz1, qz + piece);
                    OutQuad q;
                    q.state = w.state;
                    double cs[4][2] = { { qx, qz }, { qx, qz1 }, { qx1, qz1 }, { qx1, qz } };
                    for (int k = 0; k < 4; k++) {
                        q.v[k].p = { cs[k][0], y, cs[k][1] };
                        for (int s = 0; s < 2; s++) {
                            q.v[k].st[s] = int32_t(std::lround(w.v[0].st[s] + gx[s] * (cs[k][0] - w.v[0].p[0]) + gz[s] * (cs[k][1] - w.v[0].p[2])));
                        }
                        for (int s = 0; s < 4; s++) q.v[k].c[s] = uint8_t(std::clamp(std::lround(col[s]), 0l, 255l));
                    }
                    if (cross(sub(q.v[1].p, q.v[0].p), sub(q.v[2].p, q.v[0].p))[1] < 0) std::swap(q.v[1], q.v[3]);
                    for (int s = 0; s < 2; s++) {
                        double per = ws.period[s];
                        if (per <= 0) continue;
                        int32_t lo_st = std::min({ q.v[0].st[s], q.v[1].st[s], q.v[2].st[s], q.v[3].st[s] });
                        int32_t shift = int32_t(std::floor(lo_st / per) * per);
                        for (int k = 0; k < 4; k++) q.v[k].st[s] -= shift;
                    }
                    m.quads.push_back(q);
                }
            }
            if (!m.quads.empty()) { models.push_back(std::move(m)); index++; }
        };
        constexpr double near_tile = 15 * cell, far_tile = 60 * cell;   // 3,840 ft at 1:1, 15,360 ft at 4x
        for (double tz = oz; tz < oz + nz * cell; tz += near_tile) for (double tx = ox; tx < ox + nx * cell; tx += near_tile) {
            if (tx + near_tile < lo[0] - near_margin || tx > hi[0] + near_margin || tz + near_tile < lo[1] - near_margin || tz > hi[1] + near_margin) continue;
            tile(tx, tz, near_tile, 1.0, true);
        }
        for (double tz = oz; tz < oz + nz * cell; tz += far_tile) for (double tx = ox; tx < ox + nx * cell; tx += far_tile) {
            tile(tx, tz, far_tile, 4.0, false);
        }
        char line[200];
        snprintf(line, sizeof(line), "sea: water level %.1f ft over %.0f sq ft of water, %d tiles\n", level, area, index);
        report += line;
        return true;
    }
}

bool rush2::calspeed::add_scenery(int t, std::vector<uint8_t>& geometry, std::vector<uint8_t>& placement,
                                  std::vector<uint8_t>& collision, const std::vector<uint8_t>& path, SceneryResult& result) {
    result = {};
    if (geometry.size() < 0x28 || placement.size() < 0x18) { result.report = "bad files"; return false; }
    Bytes g = geometry, p = placement, c = collision;
    Container container(g);
    Scene scene = read_scene(container, p);
    if (scene.tris.empty()) { result.report = "no track triangles"; return false; }
    {
        double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
        for (const Tri& tr : scene.tris) for (const Vtx& v : tr.v) for (int k = 0; k < 3; k++) {
            lo[k] = std::min(lo[k], v.p[k]); hi[k] = std::max(hi[k], v.p[k]);
        }
        char line[200];
        snprintf(line, sizeof(line), "track: %zu triangles, %zu states, x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f\n", scene.tris.size(),
                 scene.states.size(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
        result.report += line;
    }
    std::vector<NewModel> models;
    std::vector<NewPoly> polys;
    std::set<int> water;
    {
        Collision col(c);
        if (!col.valid()) { result.report += "bad collision\n"; return false; }
        if (!sea(t, scene, col, models, polys, water, result.report)) return false;
        if (!behind_start(t, scene, col, path, models, polys, result.report, water)) return false;
    }
    if (models.empty()) return true;
    std::string why;
    if (!append_models(g, scene, models, why)) { result.report += why + "\n"; return false; }
    append_records(p, models);
    if (!polys.empty() && !add_collision(c, polys, why)) { result.report += why + "\n"; return false; }
    geometry = std::move(g);
    placement = std::move(p);
    collision = std::move(c);
    result.models = int(models.size());
    result.records = int(models.size());
    result.polygons = int(polys.size());
    return true;
}
