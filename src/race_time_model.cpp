// The race time model (include/race_time_model.h, docs/rush2049_research/checkpoints.md#09-one-rule-the-race-time-model).
//
// Rush 2 times a race from its AI lanes (race_time_from_lanes_800924E4): each checkpoint adds the time to drive lane 0
// to the next one at the lane's speeds, and the start time is the first stretch + 10 s + a 30th of a lap, all times
// the difficulty factor. Its lanes were set for its cars: over its 14 paths that grant is 0.978 (0.90-1.07) of the
// best lap Rush 2's average car can drive on lane 0. Other games' lanes were set for other cars (0.78-1.15 on Rush
// 2049's, the arcade's and California Speed's), so their tracks are timed from that best lap instead: every stretch
// gets 0.978 x the time the average car needs for it, through Rush 2's own start rule, so each track's clock is as
// tight as Rush 2's average. Rush 2's and SF Rush's tracks keep their own times.
//
// The car and the lap are tools/calspeed/ai_pace.py's model (where 0.978 was measured): Rush 2's 21 cars but the
// secret Rocket averaged (mass, torque scale per gear, gear ratios; the STANDARD torque curve, final drive 3.3, drag
// 0.0135 v^2 + 105), 2.6 g of cornering on the lane's curve (the circle through the points 60 ft before and after,
// seen from above), 40 ft/s^2 of braking, flat out between. A lane that doesn't come back to its start (a run-in or
// point-to-point path) starts from a standstill. Built with each track's conversion (fill), so it is in the disk
// cache and a race only reads it. A slippery track ([handling] traction, the PS1 SF Rush's snow) scales the car's
// acceleration, cornering grip and braking by the factor at its speed, as src/track_traction.cpp scales every car's
// forces, so its clock grants the time the snow costs (the PS1's own snow times, 8 s more a gate, were for its two
// halvings; the recomp keeps one).

#include "race_time_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "car2049.h"

namespace {
    constexpr double g = 32.17;
    constexpr double grip_g = 2.6;
    constexpr double brake_ft_s2 = 40.0;
    constexpr double reach = 60.0;      // ft either side for a point's curve
    constexpr double gap = 1000.0;      // ft: points this far apart don't join (a lane jumping back)
    constexpr double rush2_grant = 0.978;   // Rush 2's own grant over the model's best lap (mean of its 14 paths)

    // Rush 2's average car (tools/rush2049/car_tables.py over Rush 2's cars but type 20).
    constexpr double mass = 108.3629;
    constexpr double torque_scale[3] = { 1.82019, 1.84157, 1.88548 };
    constexpr double gears[4] = { 3.09524, 1.75429, 1.25381, 0.95619 };
    constexpr double final_drive = 3.3;
    constexpr double torque_row[12] = { 75, 200, 250, 290, 300, 270, 200, 100, 0, -40, -60, -80 };   // STANDARD

    double torque_at(double rpm) {
        double x = rpm / 1150.0;
        int i = int(x);
        if (i >= 11) return torque_row[11];
        return torque_row[i] + (torque_row[i + 1] - torque_row[i]) * (x - i);
    }

    double accel(double v) {
        double best = 0.0;
        for (int k = 0; k < 4; k++) {
            double rpm = 9.549 * v * gears[k] * final_drive;
            if (rpm > 9200.0) continue;
            best = std::max(best, std::max(torque_at(rpm), 0.0) * torque_scale[std::min(k, 2)] * gears[k] * final_drive);
        }
        return (best - 0.0135 * v * v - 105.0) / mass;
    }

    double top_speed() {
        static const double top = [] {
            double v = 100.0;
            while (accel(v) > 0.0 && v < 1000.0) v += 0.05;
            return v;
        }();
        return top;
    }

    struct Point { double x, y, z; };

    uint32_t be16(const std::vector<uint8_t>& f, size_t o) { return o + 1 < f.size() ? uint32_t(f[o] << 8 | f[o + 1]) : 0; }

    // Lane 0's points of a path file (race.md 3.3): header, route header at 0x32C, branch records, the point count,
    // spine and branch points, then 4 lanes of {u16 count, u8, u8, u32} + count x {s16 x, y, z, u8 speed, u8 code}.
    bool lane0(const std::vector<uint8_t>& f, std::vector<Point>& out) {
        constexpr size_t route = 0x32C;
        if (f.size() < route + 16) return false;
        size_t spine = be16(f, route), branches = f[route + 8];
        size_t o = route + 16, points = spine;
        for (size_t b = 0; b < branches; b++) points += be16(f, o + b * 16 + 0xA);
        o += 16 * branches + 2 + 6 * points;
        size_t n = be16(f, o);
        o += 8;
        if (n < 3 || n > 4096 || o + n * 8 > f.size()) return false;
        out.resize(n);
        for (size_t i = 0; i < n; i++) {
            const size_t p = o + i * 8;
            out[i] = { double(int16_t(be16(f, p))), double(int16_t(be16(f, p + 2))), double(int16_t(be16(f, p + 4))) };
        }
        return true;
    }
}

std::vector<float> rush2::race_time::lane_profile(const std::vector<uint8_t>& path_file,
                                                  const std::vector<std::array<float, 2>>& traction) {
    // The factor at speed v (ft/s): the last step reached.
    auto grip = [&](double v) {
        double f = 1.0;
        for (const auto& st : traction) if (v / 1.4667 >= st[0]) f = st[1];
        return f;
    };
    std::vector<Point> pts;
    if (!lane0(path_file, pts)) return {};
    const int n = int(pts.size());
    auto flat = [&](int i, int j) { return std::hypot(pts[size_t(j)].x - pts[size_t(i)].x, pts[size_t(j)].z - pts[size_t(i)].z); };
    const bool loop = flat(n - 1, 0) < gap;
    std::vector<double> d(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) d[size_t(i)] = flat(i, (i + 1) % n);
    // The point `reach` ft before (s -1) or after (s +1) point i, or -1 (an end, or a gap).
    auto walk = [&](int i, int s) {
        double dist = 0.0;
        int j = i;
        for (int guard = 0; dist < reach && guard < n; guard++) {
            int k = j + s;
            if (loop) k = (k + n) % n;
            if (k < 0 || k >= n) return -1;
            double step = loop && std::abs(j - k) > 1 ? d[size_t(n - 1)] : d[size_t(std::min(j, k))];
            if (step >= gap) return -1;
            dist += step;
            j = k;
        }
        return dist < reach ? -1 : j;
    };
    const double top = top_speed();
    std::vector<double> v(size_t(n), top);
    for (int i = 0; i < n; i++) {
        int a = walk(i, -1), c = walk(i, 1);
        if (a < 0 || c < 0) continue;
        const Point& A = pts[size_t(a)];
        const Point& B = pts[size_t(i)];
        const Point& C = pts[size_t(c)];
        double ab = std::hypot(B.x - A.x, B.z - A.z), bc = std::hypot(C.x - B.x, C.z - B.z), ca = std::hypot(A.x - C.x, A.z - C.z);
        double area = std::fabs((B.x - A.x) * (C.z - A.z) - (B.z - A.z) * (C.x - A.x));
        if (area < 1e-6) continue;
        const double r = ab * bc * ca / (2.0 * area);
        double vc = std::sqrt(grip_g * g * r);
        // Solved over the steps (the grip falls as the speed rises): the fastest speed whose own factor holds it.
        if (!traction.empty()) {
            vc = std::sqrt(grip_g * g * r * grip(vc));
            vc = std::min(vc, std::sqrt(grip_g * g * r * grip(vc)));
        }
        v[size_t(i)] = std::min(v[size_t(i)], vc);
    }
    if (!loop) v[0] = 0.0;   // a run-in or point-to-point lane: from the grid
    const int steps = loop ? n : n - 1;
    for (int rep = 0; rep < (loop ? 2 : 1); rep++) {
        for (int i = steps - 1; i >= 0; i--) {
            int j = (i + 1) % n;
            if (d[size_t(i)] < gap) {
                const double vj = v[size_t(j)];
                v[size_t(i)] = std::min(v[size_t(i)], std::sqrt(vj * vj + 2.0 * brake_ft_s2 * grip(vj) * d[size_t(i)]));
            }
        }
    }
    for (int rep = 0; rep < (loop ? 2 : 1); rep++) {
        for (int i = 0; i < steps; i++) {
            int j = (i + 1) % n;
            if (d[size_t(i)] < gap) {
                double vi = v[size_t(i)];
                v[size_t(j)] = std::min(v[size_t(j)], std::sqrt(std::max(vi * vi + 2.0 * std::max(accel(vi), 0.0) * grip(vi) * d[size_t(i)], 0.0)));
            }
        }
    }
    std::vector<float> cum(size_t(n) + 1, 0.0f);
    double t = 0.0;
    for (int i = 0; i < n; i++) {
        if (i < steps && d[size_t(i)] < gap) t += d[size_t(i)] / std::max((v[size_t(i)] + v[size_t((i + 1) % n)]) / 2.0, 10.0);
        cum[size_t(i) + 1] = float(t);
    }
    return cum;
}

void rush2::race_time::fill(rush2::track2049::ConvertedTrack& track, const std::vector<std::array<float, 2>>& traction) {
    track.lane_time[0] = lane_profile(track.path, traction);
    track.lane_time[1] = lane_profile(track.path_backward, traction);
}

bool rush2::race_time::apply(uint8_t* rdram, const std::vector<float>& cum) {
    constexpr uint32_t header = 0x8010BCE8;      // Copy of the path header: checkpoints at +0xC, 0x50 bytes each.
    constexpr uint32_t lane_counts = 0x800D5780; // u16[4]
    const int count = (int16_t)MEM_H(0, (int32_t)(header + 8));
    const int loop_cp = (int16_t)MEM_H(0, (int32_t)(header + 2));
    const int arm = (int16_t)MEM_H(0, (int32_t)(header + 6));
    const int n = MEM_HU(0, (int32_t)lane_counts);
    if (count <= 0 || count > 10 || n < 3 || cum.size() != size_t(n) + 1 || loop_cp < 0 || loop_cp >= count) return false;
    const bool loop = cum[size_t(n)] > cum[size_t(n) - 1];
    auto cross = [&](int cp) { return (int16_t)MEM_H(0, (int32_t)(header + 0xC + cp * 0x50 + 0x22 + 2)); };   // lane 0's
    auto next = [&](int cp) { return cp + 1 < count ? cp + 1 : loop_cp; };
    // Lane 0 from crossing a to b; past its end a run-in lane goes on from the loop start's crossing (func_8008CCF8).
    auto stretch = [&](int a, int b) -> double {
        if (b >= a) return cum[size_t(b)] - cum[size_t(a)];
        if (loop) return cum[size_t(n)] - cum[size_t(a)] + cum[size_t(b)];
        int c = cross(loop_cp);
        return cum[size_t(n) - 1] - cum[size_t(a)] + (c >= 0 && c < n ? std::max(0.0, double(cum[size_t(b)] - cum[size_t(c)])) : 0.0);
    };
    for (int i = 0; i < count; i++) {
        int a = cross(i), b = cross(next(i));
        if (a < 0 || a >= n || b < 0 || b >= n) return false;
    }
    // In the mode's units: with Rush 2049 speeds every car is that much faster (car2049.cpp, Car Speeds).
    const double scale = rush2_grant / rush2::car2049::rush2_lane_scale();
    std::vector<int> t(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) t[size_t(i)] = std::max(1, int(stretch(cross(i), cross(next(i))) * scale));
    // Rush 2's start rule (race_time_from_lanes_800924E4): checkpoint 0's lap 1 time + 10, plus a 30th of the lap,
    // which the arm checkpoint's later-lap time also gets. The lap: the loop from its start round to it.
    double lap = 0.0;
    int cp = loop_cp;
    for (int k = 0; k < count; k++) {
        lap += t[size_t(cp)];
        cp = next(cp);
        if (cp == loop_cp) break;
    }
    for (int i = 0; i < count; i++) {
        MEM_H(0, (int32_t)(header + 0xC + i * 0x50 + 0x1E)) = (int16_t)t[size_t(i)];
        MEM_H(0, (int32_t)(header + 0xC + i * 0x50 + 0x20)) = (int16_t)t[size_t(i)];
    }
    MEM_H(0, (int32_t)header) = (int16_t)int(t[0] + 10 + lap / 30.0);
    if (arm >= 0 && arm < count) MEM_H(0, (int32_t)(header + 0xC + arm * 0x50 + 0x20)) = (int16_t)int(t[size_t(arm)] + lap / 30.0);
    static int logged = 0;
    if (logged++ < 64) {
        std::string list;
        for (int v : t) list += " " + std::to_string(v);
        printf("[racetime] model lap %.1f s (x%.3f): start %d, checkpoints%s\n", double(cum[size_t(loop ? n : n - 1)]),
               scale, (int16_t)MEM_H(0, (int32_t)header), list.c_str());
    }
    return true;
}
