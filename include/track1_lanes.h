#pragma once
// SF Rush (N64) AI lane files (assets 58-64 forward, 65-71 backward; tools/rush1/paths1.py, docs/rush1_research.md):
// 4 x { s16 count, s16 loop_index, s16 end, s16 pad; count x { s16 c0, c1, c2 (collision space: render z, x, -y);
// u8 speed (ft/s); u8 behaviour; s16 index } }. Read by the N64 track port (src/rush1/track1_convert.cpp) and by the
// PlayStation SF Rush conversion, which lays the N64's lines on its versions of the same courses (ps1_track.cpp).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rush2::track1 {
    struct FileLanePoint {
        int x, y, z;                 // Rush 2 game feet: (c1, -c2, c0)
        uint8_t fps, behaviour;      // speed in ft/s
    };
    struct FileLane {
        std::vector<FileLanePoint> points;   // cut at the file's end index
        size_t loop = 0;                 // where the lap loop starts (a run-in before it)
    };

    // False when the file doesn't hold four lanes exactly.
    inline bool parse_lanes(const uint8_t* d, size_t size, std::vector<FileLane>& out) {
        auto s16 = [&](size_t o) { return int16_t(uint16_t(d[o] << 8 | d[o + 1])); };
        out.clear();
        size_t o = 0;
        for (int l = 0; l < 4; l++) {
            if (o + 8 > size) return false;
            int n = s16(o), loop = s16(o + 2), end = s16(o + 4);
            if (n < 0 || o + 8 + size_t(n) * 10 > size) return false;
            FileLane lane;
            for (int k = 0; k < n; k++) {
                size_t p = o + 8 + size_t(k) * 10;
                lane.points.push_back({ s16(p + 2), -s16(p + 4), s16(p), d[p + 6], d[p + 7] });
            }
            o += 8 + size_t(n) * 10;
            if (!(2 <= end && end <= n)) end = n;
            lane.points.resize(size_t(end));
            lane.loop = 0 <= loop && loop < end ? size_t(loop) : 0;
            out.push_back(std::move(lane));
        }
        return o == size;
    }
}
