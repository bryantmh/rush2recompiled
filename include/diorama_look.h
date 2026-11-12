#pragma once
// Snow and night looks for the track select dioramas of SNOW / NIGHT course versions (docs/sfrush_ps1.md#snow-and-night-dioramas):
// the N64 SF Rush dioramas' version models (src/rush1/track1_convert.cpp add_band_model) and Chinatown's hand-made
// diorama (src/sfrush_ps1/ps1_diorama.cpp). The colors come from the PlayStation SF Rush's own snow and night texture
// sets, measured with tools/sfrush_ps1/snow_map.py.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace rush2::diorama_look {
    enum class Look { day, snow, night };

    using Rgb = std::array<int, 3>;

    inline double luminance(const Rgb& c) { return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]; }

    inline Rgb clamp_rgb(double r, double g, double b) {
        return { std::clamp(int(std::lround(r)), 0, 255), std::clamp(int(std::lround(g)), 0, 255),
                 std::clamp(int(std::lround(b)), 0, 255) };
    }

    inline Rgb mix(const Rgb& a, const Rgb& b, double f) {
        return clamp_rgb(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f);
    }

    // The PS1's night: a day color's night texel, from its night set's greys (104 -> (20, 20, 38), 136 -> (34, 33, 53),
    // 168 -> (45, 45, 71)): about half the saturation, then about 0.27 / 0.28 / 0.42 of it, bluer (the PS1's median is
    // 0.21 of the day's brightness). `lift` scales it.
    inline Rgb night(const Rgb& c, double lift = 1.0) {
        double l = luminance(c);
        double r = c[0] + (l - c[0]) * 0.55, g = c[1] + (l - c[1]) * 0.55, b = c[2] + (l - c[2]) * 0.55;
        return clamp_rgb((r * 0.27 + 2) * lift, (g * 0.28 + 2) * lift, (b * 0.42 + 6) * lift);
    }

    // The route's red stays lit at night (the line is what the diorama is for).
    inline bool is_route_red(const Rgb& c) { return c[0] > 60 && c[0] > 2 * c[1] + 10 && c[0] > 2 * c[2] + 10; }

    // The PS1's lit windows at night: mean (216, 206, 126); the dioramas' windows from warm yellow to pale.
    inline Rgb window_light(double t) { return mix({ 236, 206, 112 }, { 252, 238, 184 }, t); }

    // The PS1's new snow: mean (195, 195, 223), 10th-90th percentile (173, 173, 198) - (222, 231, 255). t 0..1 picks in it.
    inline Rgb snow_white(double t) { return mix({ 186, 188, 214 }, { 236, 242, 255 }, t); }

    // The PS1's snow set keeps the rest a little colder: its CLUTs' channels x 1.08, 1.09, 1.27 at the same median
    // brightness, so blue up against red and green.
    inline Rgb snow_cold(const Rgb& c) {
        double l = luminance(c);
        double r = c[0] * 1.08, g = c[1] * 1.09, b = c[2] * 1.27;
        double l2 = 0.299 * r + 0.587 * g + 0.114 * b;
        double k = l2 > 0 ? l / l2 : 1.0;
        return clamp_rgb(r * k, g * k, b * k);
    }

    // Smooth value noise in 0..1 over a plane (patches of snow), deterministic.
    inline double hash01(int64_t x, int64_t y, uint32_t seed) {
        uint64_t h = uint64_t(x) * 0x9E3779B185EBCA87ull ^ uint64_t(y) * 0xC2B2AE3D27D4EB4Full ^ (uint64_t(seed) << 17);
        h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 33;
        return double(h >> 11) / double(1ull << 53);
    }
    inline double value_noise(double x, double y, uint32_t seed) {
        double fx = std::floor(x), fy = std::floor(y);
        double u = x - fx, v = y - fy;
        u = u * u * (3 - 2 * u);
        v = v * v * (3 - 2 * v);
        int64_t ix = int64_t(fx), iy = int64_t(fy);
        double a = hash01(ix, iy, seed), b = hash01(ix + 1, iy, seed), c = hash01(ix, iy + 1, seed), d = hash01(ix + 1, iy + 1, seed);
        return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
    }
}
