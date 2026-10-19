// Runs California Speed's extended scenery (src/calspeed/calspeed_scenery.cpp) outside the game on one track's files,
// writes the changed files, and checks the collision it added: every new polygon must be in the leaf Rush 2's walk
// finds for points across it (collision_node_find_8006CD60: floor split, the queries' x - 0.99 cell for negative x;
// a node mask bit 4 + q takes the leaf from the far-leaf table). Built and run by tools/calspeed/scenery_test.bat;
// tools/calspeed/scenery.py prepares the inputs and draws the result.
//
//     scenery_test.exe TRACK GEOMETRY PLACEMENT COLLISION PATH OUT_DIR
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "calspeed_scenery.h"

using Bytes = std::vector<uint8_t>;

static Bytes read_all(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void write_all(const std::string& path, const Bytes& d) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}
static uint16_t u16(const Bytes& d, size_t o) { return uint16_t((d[o] << 8) | d[o + 1]); }
static int16_t s16(const Bytes& d, size_t o) { return int16_t(u16(d, o)); }
static uint32_t u32(const Bytes& d, size_t o) { return (uint32_t(u16(d, o)) << 16) | u16(d, o + 2); }

// Whether floor polygon pi holds world point p by Rush 2's point test (inside its edges in its frame, from half a foot
// under its plane to 5 ft over).
static bool holds(const Bytes& c, int pi, double px, double py, double pz) {
    uint32_t segs = u16(c, 0), nodes = u16(c, 2), polys = u16(c, 4), verts = u16(c, 6);
    size_t poly0 = 12 + size_t(segs) * 0x84 + size_t(nodes) * 0x14, vert0 = poly0 + size_t(polys) * 0x18;
    size_t vlist0 = vert0 + size_t(verts) * 8 + u16(c, 8);
    size_t rec = poly0 + size_t(pi) * 0x18;
    if ((c[rec + 1] & 0xF) > 2) return false;
    int count = u16(c, rec + 2) & 0xF;
    size_t vat = vlist0 + u16(c, rec + 0x16);
    std::vector<uint32_t> idx;
    while ((int)idx.size() < count) {
        uint32_t v = u16(c, vat);
        vat += 2;
        int run = 0;
        if (count - (int)idx.size() >= 2 && c[vat] >= 0xC0) run = c[vat++] & 0x3F;
        for (int r = 0; r <= run; r++) idx.push_back(v + r);
    }
    double m[9];
    for (int k = 0; k < 9; k++) m[k] = s16(c, rec + 4 + k * 2) / 16384.0;
    auto vert = [&](uint32_t k, double o[3]) {
        size_t vo = vert0 + size_t(k) * 8;
        uint16_t frac = u16(c, vo + 6);
        const int sh[3] = { 10, 5, 0 };
        for (int a = 0; a < 3; a++) o[a] = (s16(c, vo + a * 2) * 32 + ((frac >> sh[a]) & 31)) / 32.0;
    };
    double o[3];
    vert(idx[0], o);
    double d[3] = { px - o[0], py - o[1], pz - o[2] }, l[3];
    for (int r = 0; r < 3; r++) l[r] = d[0] * m[r * 3] + d[1] * m[r * 3 + 1] + d[2] * m[r * 3 + 2];
    if (l[1] < -0.5 || l[1] >= 5) return false;
    std::vector<std::array<double, 2>> xz;
    for (int k = 0; k < count; k++) {
        double v[3];
        vert(idx[k], v);
        xz.push_back({ k ? v[0] : 0.0, k ? v[2] : 0.0 });
    }
    bool in = !(0.0 < l[2] * xz.back()[0] - l[0] * xz.back()[1]) && !(l[2] * xz[1][0] - l[0] * xz[1][1] < 0.0);
    for (size_t k = 2; k < xz.size(); k++) {
        in = in && !((l[2] - xz[k - 1][1]) * (xz[k][0] - xz[k - 1][0]) - (l[0] - xz[k - 1][0]) * (xz[k][1] - xz[k - 1][1]) < 0.0);
    }
    return in;
}

// The polygon indices Rush 2's ground query reads for world (x, z).
static std::vector<int> leaf_at(const Bytes& c, double x, double z) {
    uint32_t segs = u16(c, 0), nodes = u16(c, 2), polys = u16(c, 4), verts = u16(c, 6), leaf = u16(c, 8), vlist = u16(c, 10);
    size_t node0 = 12 + size_t(segs) * 0x84, leaf0 = node0 + size_t(nodes) * 0x14 + size_t(polys) * 0x18 + size_t(verts) * 8;
    size_t table = (leaf0 + leaf + vlist + 3) & ~size_t(3);
    bool tagged = (c[node0 + 2] & rush2::calspeed::scenery_collision_tag) != 0;
    auto cellv = [](double v) { return int32_t(int16_t(int32_t(v < 0 ? v - 0.99 : v))); };
    int32_t cx = cellv(x), cz = cellv(z);
    uint32_t n = 0;
    // collision_node_find_cached_8006CE28: a cell outside the root's box (min inclusive, max exclusive) gets nothing.
    if (cx < s16(c, node0 + 4) || cx >= s16(c, node0 + 6) || cz < s16(c, node0 + 8) || cz >= s16(c, node0 + 10)) return {};
    for (int depth = 0; depth < 64; depth++) {
        size_t o = node0 + size_t(n) * 0x14;
        int32_t sx = (s16(c, o + 4) + s16(c, o + 6)) >> 1, sz = (s16(c, o + 8) + s16(c, o + 10)) >> 1;
        int q = (cx >= sx ? 1 : 0) + (cz < sz ? 2 : 0);
        uint8_t mask = c[o + 3];
        uint16_t child = u16(c, o + 0xC + q * 2);
        if (mask & (1 << q)) {
            if (child == 0) return {};
            n = child;
            continue;
        }
        uint32_t off = child;
        if (tagged && (mask & (0x10 << q))) off = u32(c, table + size_t(child) * 4);
        if (off == 0) return {};
        std::vector<int> out;
        size_t p = leaf0 + off;
        int count = c[p++];
        while (int(out.size()) < count) {
            uint16_t e = u16(c, p);
            p += 2;
            for (int k = 0; k <= (e >> 13); k++) out.push_back((e & 0x1FFF) + k);
        }
        return out;
    }
    return {};
}

int main(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: scenery_test TRACK GEOMETRY PLACEMENT COLLISION PATH OUT_DIR\n");
        return 2;
    }
    int t = atoi(argv[1]);
    Bytes g = read_all(argv[2]), p = read_all(argv[3]), c = read_all(argv[4]), path = read_all(argv[5]);
    std::string out = argv[6];
    uint32_t old_polys = u16(c, 4);
    rush2::calspeed::SceneryResult r;
    bool ok = rush2::calspeed::add_scenery(t, g, p, c, path, r);
    printf("%s", r.report.c_str());
    fflush(stdout);
    printf("%s: %d models, %d records, %d polygons\n", ok ? "ok" : "FAILED", r.models, r.records, r.polygons);
    if (!ok) return 1;
    write_all(out + "/geometry.bin", g);
    printf("collision: polygons %u to %u are new\n", old_polys, uint32_t(u16(c, 4)) - 1);
    write_all(out + "/placement.bin", p);
    write_all(out + "/collision.bin", c);

    // Collision check: points across every new polygon (corners pulled 5% in, and its middle) must find it.
    uint32_t segs = u16(c, 0), nodes = u16(c, 2), polys = u16(c, 4);
    size_t poly0 = 12 + size_t(segs) * 0x84 + size_t(nodes) * 0x14, vert0 = poly0 + size_t(polys) * 0x18;
    size_t vlist0 = vert0 + size_t(u16(c, 6)) * 8 + u16(c, 8);
    // SCENERY_POLYS="1325,1326": those polygons (the track's own too), world corners and facing (the frame's normal).
    if (const char* list = getenv("SCENERY_POLYS")) {
        for (const char* q = list; *q;) {
            int pi = atoi(q);
            size_t rec = poly0 + size_t(pi) * 0x18;
            int count = u16(c, rec + 2) & 0xF;
            size_t vat = vlist0 + u16(c, rec + 0x16);
            std::vector<uint32_t> idx;
            while ((int)idx.size() < count) {
                uint32_t v = u16(c, vat);
                vat += 2;
                int run = 0;
                if (count - (int)idx.size() >= 2 && c[vat] >= 0xC0) run = c[vat++] & 0x3F;
                for (int r = 0; r <= run; r++) idx.push_back(v + r);
            }
            double m[9];
            for (int k = 0; k < 9; k++) m[k] = s16(c, rec + 4 + k * 2) / 16384.0;
            auto vert = [&](uint32_t k, double o[3]) {
                size_t vo = vert0 + size_t(k) * 8;
                uint16_t frac = u16(c, vo + 6);
                const int sh[3] = { 10, 5, 0 };
                for (int a = 0; a < 3; a++) o[a] = (s16(c, vo + a * 2) * 32 + ((frac >> sh[a]) & 31)) / 32.0;
            };
            double o[3];
            vert(idx[0], o);
            printf("polygon %d flags %04X normal (%.2f %.2f %.2f): (%.1f %.1f %.1f)", pi, u16(c, rec), m[3], m[4], m[5], o[0], o[1], o[2]);
            for (int k = 1; k < count; k++) {
                double l[3];
                vert(idx[k], l);
                printf(" (%.1f %.1f %.1f)", o[0] + m[0] * l[0] + m[3] * l[1] + m[6] * l[2], o[1] + m[1] * l[0] + m[4] * l[1] + m[7] * l[2],
                       o[2] + m[2] * l[0] + m[5] * l[1] + m[8] * l[2]);
            }
            printf("\n");
            while (*q && *q != ',') q++;
            if (*q == ',') q++;
        }
    }
    int misses = 0, probes = 0;
    for (uint32_t i = old_polys; i < polys; i++) {
        size_t rec = poly0 + size_t(i) * 0x18;
        int count = u16(c, rec + 2) & 0xF;
        size_t at = vlist0 + u16(c, rec + 0x16);
        uint32_t v0 = u16(c, at);
        double m[9];
        for (int k = 0; k < 9; k++) m[k] = s16(c, rec + 4 + k * 2) / 16384.0;
        auto vert = [&](uint32_t k, double o[3]) {
            size_t vo = vert0 + size_t(k) * 8;
            uint16_t frac = u16(c, vo + 6);
            const int sh[3] = { 10, 5, 0 };
            for (int a = 0; a < 3; a++) o[a] = (s16(c, vo + a * 2) * 32 + ((frac >> sh[a]) & 31)) / 32.0;
        };
        std::vector<std::array<double, 3>> w;
        double o[3];
        vert(v0, o);
        w.push_back({ o[0], o[1], o[2] });
        for (int k = 1; k < count; k++) {
            double l[3];
            vert(v0 + k, l);
            w.push_back({ o[0] + m[0] * l[0] + m[3] * l[1] + m[6] * l[2], o[1] + m[1] * l[0] + m[4] * l[1] + m[7] * l[2],
                          o[2] + m[2] * l[0] + m[5] * l[1] + m[8] * l[2] });
        }
        if (getenv("SCENERY_DUMP")) {
            printf("polygon %u type %d:", i, c[rec + 1] & 0xF);
            for (const auto& v : w) printf(" (%.1f %.1f %.1f)", v[0], v[1], v[2]);
            printf("\n");
        }
        double cx = 0, cz = 0, cy = 0;
        for (const auto& v : w) { cx += v[0] / w.size(); cy += v[1] / w.size(); cz += v[2] / w.size(); }
        // Rush 2's point test on a floor (California Speed's func_800AE884, calspeed_convert.cpp ground_under): 2 ft
        // over its middle must be inside its edges at height 2. A wall (car_wall_collision_query_8006FC94's body probe,
        // the same test) must hold its middle.
        bool floor = (c[rec + 1] & 0xF) <= 2;
        {
            double up = floor ? 2 : 0;
            double d[3] = { cx - o[0], cy + up - o[1], cz - o[2] }, l[3];
            for (int r = 0; r < 3; r++) l[r] = d[0] * m[r * 3] + d[1] * m[r * 3 + 1] + d[2] * m[r * 3 + 2];
            std::vector<std::array<double, 2>> xz;
            for (int k = 0; k < count; k++) {
                double v[3];
                vert(v0 + k, v);
                xz.push_back({ k ? v[0] : 0.0, k ? v[2] : 0.0 });
            }
            bool in = !(0.0 < l[2] * xz.back()[0] - l[0] * xz.back()[1]) && !(l[2] * xz[1][0] - l[0] * xz[1][1] < 0.0);
            for (size_t k = 2; k < xz.size(); k++) {
                in = in && !((l[2] - xz[k - 1][1]) * (xz[k][0] - xz[k - 1][0]) - (l[0] - xz[k - 1][0]) * (xz[k][1] - xz[k - 1][1]) < 0.0);
            }
            // A sloped sliver (a curb's top, under 1 ft across) puts the point 2 ft up outside its edges; the game's
            // probes land on its neighbors there, so only wider floors count.
            double ex = 0, ez = 0;
            for (const auto& v : xz) { ex = std::max(ex, std::fabs(v[0])); ez = std::max(ez, std::fabs(v[1])); }
            double across = std::min(ex, ez);
            if ((!in && (across >= 1 || !floor)) || std::fabs(l[1] - up * m[4]) > 0.2) {   // up along a sloped floor's normal
                printf("%s %u fails the point test: inside %d, height %.2f (local y row %.2f %.2f %.2f)\n",
                       floor ? "floor" : "wall", i, in, l[1], m[3], m[4], m[5]);
                misses++;
            }
        }
        std::vector<std::pair<double, double>> pts = { { cx, cz } };
        for (size_t k = 0; k < w.size(); k++) {
            const auto& a = w[k];
            const auto& b = w[(k + 1) % w.size()];
            for (int s = 0; s <= 20; s++) {
                double u = s / 20.0;
                double x = a[0] + (b[0] - a[0]) * u, z = a[2] + (b[2] - a[2]) * u;
                pts.push_back({ cx + (x - cx) * 0.97, cz + (z - cz) * 0.97 });   // as the game's self-test (RUSH2_TEST_COLLISION)
            }
        }
        // Rush 2's point test half a foot over each point, as the game's self-test probes a floor.
        auto inside_at = [&](double px, double py, double pz) {
            double d[3] = { px - o[0], py - o[1], pz - o[2] }, l[3];
            for (int r = 0; r < 3; r++) l[r] = d[0] * m[r * 3] + d[1] * m[r * 3 + 1] + d[2] * m[r * 3 + 2];
            std::vector<std::array<double, 2>> xz;
            for (int k = 0; k < count; k++) {
                double v[3];
                vert(v0 + k, v);
                xz.push_back({ k ? v[0] : 0.0, k ? v[2] : 0.0 });
            }
            bool in = !(0.0 < l[2] * xz.back()[0] - l[0] * xz.back()[1]) && !(l[2] * xz[1][0] - l[0] * xz[1][1] < 0.0);
            for (size_t k = 2; k < xz.size(); k++) {
                in = in && !((l[2] - xz[k - 1][1]) * (xz[k][0] - xz[k - 1][0]) - (l[0] - xz[k - 1][0]) * (xz[k][1] - xz[k - 1][1]) < 0.0);
            }
            return in;
        };
        for (const auto& [x, z] : pts) {
            probes++;
            std::vector<int> l = leaf_at(c, x, z);
            // Half a foot off the point along the floor's normal some floor of the leaf must hold it: no gap at a seam.
            // (Straight up drifts uphill in a sloped floor's frame, out of its edge at a wall's foot, where a wheel
            // can't reach.)
            if (floor && std::fabs(m[4]) > 0.2) {
                double py = cy - (m[3] * (x - cx) + m[5] * (z - cz)) / m[4];
                double qx = x + m[3] * 0.5, qy = py + m[4] * 0.5, qz = z + m[5] * 0.5;
                bool held = inside_at(qx, qy, qz);
                for (int k : l) held = held || holds(c, k, qx, qy, qz);
                if (!held) {
                    if (misses < 10) printf("nothing holds: polygon %u at (%.1f, %.1f, %.1f)\n", i, x, py, z);
                    misses++;
                }
            }
            bool found = false;
            for (int k : l) found |= k == int(i);
            if (!found) {
                if (misses < 10) printf("miss: polygon %u at (%.1f, %.1f), leaf of %zu\n", i, x, z, l.size());
                misses++;
            }
            if (l.size() > 36) { printf("leaf of %zu polygons at (%.1f, %.1f)\n", l.size(), x, z); misses++; }
        }
    }
    printf("collision check: %d probes, %d misses\n", probes, misses);

    // SCENERY_PROBE="x,y,z;x,y,z...": the wheel probe's answer at each point (the leaf's floors holding it from 5 ft
    // under to 250 ft over, the nearest).
    if (const char* probe = getenv("SCENERY_PROBE")) {
        std::string s = probe;
        size_t at = 0;
        while (at < s.size()) {
            size_t end = s.find(';', at);
            if (end == std::string::npos) end = s.size();
            double px, py, pz;
            if (sscanf(s.substr(at, end - at).c_str(), "%lf,%lf,%lf", &px, &py, &pz) == 3) {
                std::vector<int> l = leaf_at(c, px, pz);
                printf("probe (%.1f, %.1f, %.1f): leaf of %zu:", px, py, pz, l.size());
                for (int pi : l) {
                    size_t rec = poly0 + size_t(pi) * 0x18;
                    if ((c[rec + 1] & 0xF) > 2) {
                        printf(" [%d type %d n %.2f %.2f %.2f]", pi, c[rec + 1] & 0xF, s16(c, rec + 10) / 16384.0,
                               s16(c, rec + 12) / 16384.0, s16(c, rec + 14) / 16384.0);
                        continue;
                    }
                    int count = u16(c, rec + 2) & 0xF;
                    size_t vat = vlist0 + u16(c, rec + 0x16);
                    // Vertex indices (run encoded).
                    std::vector<uint32_t> idx;
                    while ((int)idx.size() < count) {
                        uint32_t v = u16(c, vat);
                        vat += 2;
                        int run = 0;
                        if (count - (int)idx.size() >= 2 && c[vat] >= 0xC0) run = c[vat++] & 0x3F;
                        for (int r = 0; r <= run; r++) idx.push_back(v + r);
                    }
                    double m[9];
                    for (int k = 0; k < 9; k++) m[k] = s16(c, rec + 4 + k * 2) / 16384.0;
                    auto vert = [&](uint32_t k, double o[3]) {
                        size_t vo = vert0 + size_t(k) * 8;
                        uint16_t frac = u16(c, vo + 6);
                        const int sh[3] = { 10, 5, 0 };
                        for (int a = 0; a < 3; a++) o[a] = (s16(c, vo + a * 2) * 32 + ((frac >> sh[a]) & 31)) / 32.0;
                    };
                    double o[3];
                    vert(idx[0], o);
                    double d[3] = { px - o[0], py - o[1], pz - o[2] }, lc[3];
                    for (int r = 0; r < 3; r++) lc[r] = d[0] * m[r * 3] + d[1] * m[r * 3 + 1] + d[2] * m[r * 3 + 2];
                    std::vector<std::array<double, 2>> xz;
                    for (int k = 0; k < count; k++) {
                        double v[3];
                        vert(idx[k], v);
                        xz.push_back({ k ? v[0] : 0.0, k ? v[2] : 0.0 });
                    }
                    bool in = !(0.0 < lc[2] * xz.back()[0] - lc[0] * xz.back()[1]) && !(lc[2] * xz[1][0] - lc[0] * xz[1][1] < 0.0);
                    for (size_t k = 2; k < xz.size(); k++) {
                        in = in && !((lc[2] - xz[k - 1][1]) * (xz[k][0] - xz[k - 1][0]) - (lc[0] - xz[k - 1][0]) * (xz[k][1] - xz[k - 1][1]) < 0.0);
                    }
                    if (in && lc[1] > -5 && lc[1] < 250) printf(" [%d h %.1f]", pi, lc[1]);
                }
                printf("\n");
            }
            at = end + 1;
        }
    }
    return misses ? 1 : 0;
}
