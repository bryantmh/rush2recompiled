#pragma once
// Hand fixes for AI lanes in a Rush 2 format path file (assets 0x57+ / 0x63+; tools/rush2049/paths.py has the layout),
// for ported tracks whose original lanes run a Rush 2 car into something: the N64 Rush 2049 and SF Rush ports keep the
// original games' lanes, which pass posts and wall corners closer than a Rush 2 car's body clears
// (docs/rush2049_research/race.md#lane-fixes). Each fix moves the chosen lanes sideways toward a point on the road
// beside them (off the side of what the cars hit; the PS1 conversion's lane_fixes do the same), at full strength `span`
// lane points either side of each lane's point nearest it and eased out over `ease` more. Only those points move;
// speeds and behavior codes stay.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rush2::path_lane_fix {
    struct Fix {
        int lanes;          // bit k: lane k (0-3)
        double x, y, z;     // the point on the road to move toward, in the path's coordinates (game x, y, z in ft)
        double shift;       // ft
        int span = 2, ease = 8;
        // When 0-3: instead of shifting, the lanes take that lane's points (same index; the lanes of a path file have
        // the same count where this is used) over the stretch, blended in and out over `ease`.
        int from = -1;
    };

    // Standing-start ramps left in a lap: the N64 Rush 2049 lanes carry the race start's acceleration from the grid
    // (a drop of over 50 mph to under 40 within two points, then a steady climb: 147, 100, 28, 40, 48, 54, 58...) on
    // a fast straight: Metro's lanes 0, 2 and 3 forward (lane 2 nine points before the line, lane 0 four after) and
    // Presidio's lanes 1-3 backward. Rush 2 drones braked to a crawl there every lap and the cars behind ran into them
    // (Metro: 8 wrecks a race at the line). Each such ramp eases instead from the speed before the drop to the speed it
    // climbs to, keeping any point already faster. Returns how many ramps changed.
    inline size_t level_start_ramps(std::vector<uint8_t>& path) {
        constexpr size_t route = 0x32C;
        if (path.size() < route + 16) return 0;
        auto u16 = [&](size_t o) { return size_t(path[o]) << 8 | path[o + 1]; };
        size_t o = route + 16 + 16 * size_t(path[route + 8]);
        if (o + 2 > path.size()) return 0;
        o += 2 + 6 * u16(o);
        size_t changed = 0;
        for (int k = 0; k < 4; k++) {
            if (o + 8 > path.size()) return changed;
            const size_t m = u16(o), at = o + 8;
            o += 8 + 8 * m;
            if (o > path.size() || m < 16) return changed;
            auto speed = [&](size_t i) -> uint8_t& { return path[at + 8 * (i % m) + 6]; };
            for (size_t i = 0; i < m; i++) {
                const int before = std::max<int>(speed(i + m - 1), speed(i + m - 2));
                if (speed(i) >= 40 || before - speed(i) <= 50) continue;
                bool rising = true;
                for (size_t t = 0; t < 6 && rising; t++) rising = speed(i + t + 1) > speed(i + t);
                if (!rising) continue;
                size_t end = i;
                while (end < i + m && speed(end + 1) > speed(end)) end++;
                // A drop over two points (141, 100, 28) starts at the first of them.
                const size_t from = speed(i + m - 1) < before ? i + m - 1 : i + m;
                const double top = speed(end), n = double(end + m - from + 1);
                for (size_t j = from; j <= end + m; j++) {
                    double v = before + (top - before) * double(j - from + 1) / n;
                    if (v > speed(j)) speed(j) = uint8_t(std::lround(v));
                }
                changed++;
            }
        }
        return changed;
    }

    // A speed cap where the original lanes take a corner faster than a Rush 2 car stays upright (cars rolled over at
    // 96-107 mph holding the turn): every lane point within `radius` ft of (x, z) is held to `mph`, and the `ease`
    // points before the first one in each pass come down to it gradually, so the drones brake before the corner.
    struct SpeedCap {
        double x, z, radius;
        int mph;
        int ease = 4;
    };

    // Applies `count` speed caps to `path` in place; returns how many lane points changed.
    inline size_t cap_speeds(std::vector<uint8_t>& path, const SpeedCap* caps, size_t count) {
        constexpr size_t route = 0x32C;
        if (path.size() < route + 16) return 0;
        auto u16 = [&](size_t o) { return size_t(path[o]) << 8 | path[o + 1]; };
        auto s16 = [&](size_t o) { return double(int16_t(uint16_t(u16(o)))); };
        size_t o = route + 16 + 16 * size_t(path[route + 8]);
        if (o + 2 > path.size()) return 0;
        o += 2 + 6 * u16(o);
        size_t changed = 0;
        for (int k = 0; k < 4; k++) {
            if (o + 8 > path.size()) return changed;
            const size_t m = u16(o), at = o + 8;
            o += 8 + 8 * m;
            if (o > path.size() || m < 3) return changed;
            for (size_t c = 0; c < count; c++) {
                const SpeedCap& cap = caps[c];
                auto inside = [&](size_t i) {
                    i %= m;
                    return std::hypot(s16(at + 8 * i) - cap.x, s16(at + 8 * i + 4) - cap.z) <= cap.radius;
                };
                for (size_t i = 0; i < m; i++) {
                    if (!inside(i)) continue;
                    uint8_t& s = path[at + 8 * i + 6];
                    if (s > cap.mph) s = uint8_t(cap.mph), changed++;
                    if (inside(i + m - 1)) continue;   // the approach: only before the first point of a pass
                    for (int e = 1; e <= cap.ease; e++) {
                        uint8_t& p = path[at + 8 * ((i + m - size_t(e)) % m) + 6];
                        int limit = cap.mph + (int(p) - cap.mph) * e / (cap.ease + 1);
                        if (p > limit) p = uint8_t(limit), changed++;
                    }
                }
            }
        }
        return changed;
    }

    // Applies `count` fixes to `path` in place; returns how many lane fixes were applied (lanes x fixes).
    inline size_t apply(std::vector<uint8_t>& path, const Fix* fixes, size_t count) {
        constexpr size_t route = 0x32C;
        if (path.size() < route + 16) return 0;
        auto u16 = [&](size_t o) { return size_t(path[o]) << 8 | path[o + 1]; };
        auto s16 = [&](size_t o) { return double(int16_t(uint16_t(u16(o)))); };
        size_t o = route + 16 + 16 * size_t(path[route + 8]);   // past the branch records
        if (o + 2 > path.size()) return 0;
        o += 2 + 6 * u16(o);                                     // past the spine and branch points
        size_t at[4], n[4];
        for (int k = 0; k < 4; k++) {
            if (o + 8 > path.size()) return 0;
            n[k] = u16(o);
            at[k] = o + 8;
            o += 8 + 8 * n[k];
            if (o > path.size()) return 0;
        }
        size_t applied = 0;
        for (size_t f = 0; f < count; f++) {
            const Fix& fx = fixes[f];
            for (int k = 0; k < 4; k++) {
                if (!(fx.lanes >> k & 1) || n[k] < 3) continue;
                const size_t m = n[k];
                auto px = [&](size_t i) { return s16(at[k] + 8 * i); };
                auto py = [&](size_t i) { return s16(at[k] + 8 * i + 2); };
                auto pz = [&](size_t i) { return s16(at[k] + 8 * i + 4); };
                size_t i0 = 0;
                double best = 1e300;
                for (size_t i = 0; i < m; i++) {
                    double d = std::hypot(px(i) - fx.x, pz(i) - fx.z) + 4.0 * std::fabs(py(i) - fx.y);
                    if (d < best) best = d, i0 = i;
                }
                // Toward the point across the lane at i0; each moved point goes along its own lateral that way.
                auto lateral = [&](size_t i, double& lx, double& lz) {
                    size_t a = (i + m - 1) % m, b = (i + 1) % m;
                    double tx = px(b) - px(a), tz = pz(b) - pz(a), l = std::hypot(tx, tz);
                    if (l == 0) l = 1;
                    lx = -tz / l, lz = tx / l;
                };
                double lx0, lz0;
                lateral(i0, lx0, lz0);
                const double side = (fx.x - px(i0)) * lx0 + (fx.z - pz(i0)) * lz0 >= 0 ? 1.0 : -1.0;
                const bool copy = fx.from >= 0 && fx.from < 4 && n[fx.from] == m;
                auto ox = [&](size_t i) { return s16(at[fx.from] + 8 * i); };
                auto oz = [&](size_t i) { return s16(at[fx.from] + 8 * i + 4); };
                std::vector<std::pair<size_t, std::pair<double, double>>> moved;
                for (int d = -(fx.span + fx.ease); d <= fx.span + fx.ease; d++) {
                    size_t i = size_t((long(i0) + d) % long(m) + long(m)) % m;
                    double t = std::abs(d) <= fx.span ? 1.0 : 1.0 - double(std::abs(d) - fx.span) / double(fx.ease + 1);
                    double b = t * t * (3 - 2 * t), w = b * fx.shift;
                    if (copy) {
                        moved.push_back({ i, { px(i) + (ox(i) - px(i)) * b, pz(i) + (oz(i) - pz(i)) * b } });
                        continue;
                    }
                    double lx, lz;
                    lateral(i, lx, lz);
                    if (lx * lx0 + lz * lz0 < 0) lx = -lx, lz = -lz;
                    moved.push_back({ i, { px(i) + lx * side * w, pz(i) + lz * side * w } });
                }
                for (const auto& [i, p] : moved) {   // written after all were read: laterals come from the original lane
                    for (int a = 0; a < 2; a++) {
                        int16_t v = int16_t(std::lround(a == 0 ? p.first : p.second));
                        size_t w = at[k] + 8 * i + (a == 0 ? 0 : 4);
                        path[w] = uint8_t(uint16_t(v) >> 8);
                        path[w + 1] = uint8_t(uint16_t(v));
                    }
                }
                applied++;
            }
        }
        return applied;
    }
}
