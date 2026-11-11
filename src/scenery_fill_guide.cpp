// Diorama guides for Fill Scenery Gaps (include/scenery_fill.h, docs/graphics_effects.md#diorama-guides): each Rush 2
// and SF Rush track's track select diorama, laid over the track by a fit of its red route line to the track's AI path
// (tools/diorama_fit.py), tells the underlay builder (src/scenery_fill_build.cpp) what the land past the track's own
// ground is (sea, park, city, beach) and how it lies. Nothing here touches the game, so tools/car_probes_test.cpp
// --fill --guide builds with it offline.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>

#include "asset_catalog.h"
#include "scenery_fill.h"

namespace rush2::scenery_fill {

namespace {
    // tools/diorama_fit.py's output per track version. SF Rush's arcade versions (Rush The Rock) are built in the N64
    // tracks' frame and take their fits (arcade Sunset's shorter figure 8 fits badly on its own); the PlayStation
    // versions of tracks 1-3 are the N64 courses turned a quarter, fitted on their own. Night and snow versions take
    // the PlayStation's water colors (src/sfrush_ps1/ps1_track.cpp's sea).
    constexpr uint32_t snow_water = 0x103163, night_water = 0x000010;
    const Fit fits[] = {
        { "rush2:VEGAS", "VEGASTRACK", 9.5855f, 268.00f, false, 195.9f, -104.8f, 0 },
        { "rush2:NYONE", "NYLTRACK", 12.7822f, 358.50f, false, 1142.5f, -273.3f, 0 },
        { "rush2:HAWAII", "HAWAIITRACK", 11.3903f, 272.88f, false, -107.5f, 943.8f, 0 },
        { "rush2:NYTWO", "NYUTRACK", 11.4897f, 92.80f, false, 907.7f, 1665.8f, 0 },
        { "rush2:ALCATRAZ", "ALCATRAZTRACK", 6.2774f, 358.33f, false, 134.1f, -214.2f, 0 },
        { "rush2:LA", "LATRACK", 9.9324f, 0.83f, false, 746.6f, -206.8f, 0 },
        { "rush2:SEATTLE", "SEATTLETRACK", 7.4130f, 359.67f, false, -1426.8f, 189.8f, 0 },
        { "rush2:ATARI", "ATARITRACK", 4.8496f, 1.20f, false, -806.7f, -213.3f, 0 },

        { "sfrush:1", "TRK1O2", 14.2832f, 88.88f, false, 669.5f, 1626.2f, 0 },
        { "sfrush:2", "TRK_2O2", 10.3117f, 180.25f, false, 2055.0f, 1858.6f, 0 },
        { "sfrush:3", "TRK_3O2", 6.7895f, 180.73f, false, 4808.5f, 1167.1f, 0 },
        { "sfrush:4", "TRK4O2", 3.8329f, 181.37f, false, -1492.1f, 2101.6f, 0 },
        { "sfrush:5", "TRK5O1", 5.2017f, 264.01f, false, -2630.4f, 2486.0f, 0 },
        { "sfrush:6", "TRK6O3", 5.7130f, 89.81f, false, -513.5f, -2298.7f, 0 },
        { "sfrush:7", "TRK7O5", 6.3343f, 359.28f, false, 107.4f, 111.1f, 0 },

        { "golden_gate_arcade", "TRK1O2", 14.2832f, 88.88f, false, 669.5f, 1626.2f, 0 },
        { "embarcadero_arcade", "TRK_2O2", 10.3117f, 180.25f, false, 2055.0f, 1858.6f, 0 },
        { "market_arcade", "TRK_3O2", 6.7895f, 180.73f, false, 4808.5f, 1167.1f, 0 },
        { "downtown_arcade", "TRK4O2", 3.8329f, 181.37f, false, -1492.1f, 2101.6f, 0 },
        { "the_heights_arcade", "TRK5O1", 5.2017f, 264.01f, false, -2630.4f, 2486.0f, 0 },
        { "sunset_arcade", "TRK6O3", 5.7130f, 89.81f, false, -513.5f, -2298.7f, 0 },
        { "the_rock_arcade", "TRK7O5", 6.3343f, 359.28f, false, 107.4f, 111.1f, 0 },

        { "golden_gate_snow", "TRK1O2", 14.1819f, 359.03f, false, 1561.6f, -674.5f, snow_water },
        { "golden_gate_night", "TRK1O2", 14.1819f, 359.03f, false, 1561.6f, -674.5f, night_water },
        { "embarcadero_snow", "TRK_2O2", 10.2958f, 90.35f, false, 3117.4f, -2353.0f, snow_water },
        { "embarcadero_night", "TRK_2O2", 10.2958f, 90.35f, false, 3117.4f, -2353.0f, night_water },
        { "market_snow", "TRK_3O2", 6.7937f, 90.50f, false, 1169.5f, -4807.2f, snow_water },
        { "market_night", "TRK_3O2", 6.7937f, 90.50f, false, 1169.5f, -4807.2f, night_water },
    };

    // A diorama triangle's kind by its color (its texture's mean times its vertex colors' mean, 0-255), as
    // tools/diorama_fit.py tells the red route line.
    Kind kind_of(const float c[3]) {
        float r = c[0], g = c[1], b = c[2];
        if (r > 90 && r > 1.8f * g && r > 1.8f * b) return Kind::route;
        if (b > r + 25 && b > g + 5) return Kind::water;
        if (g > r + 8 && g >= b - 5) return Kind::green;
        if (r > b + 25 && g > b + 10) return Kind::sand;
        return Kind::city;
    }
}

const Fit* find_fit(const std::string& key) {
    for (const Fit& f : fits) {
        if (key == f.key) return &f;
    }
    return nullptr;
}

bool sfrush_diorama(const Fit& fit) {
    return strncmp(fit.key, "rush2:", 6) != 0;
}

uint64_t Guide::hash() const {
    uint64_t h = 0xCBF29CE484222325ull;
    auto add = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    };
    for (const Triangle& t : triangles) {
        add(t.v, sizeof(t.v));
        add(&t.kind, 1);
        add(t.rgb, 3);
    }
    add(water, 3);
    return h;
}

bool make_guide(const rush2::catalog::Container& dioramas, const Fit& fit, Guide& out) {
    out = Guide{};
    out.key = fit.key;
    if (fit.water != 0) {
        for (int k = 0; k < 3; k++) out.water[k] = uint8_t(fit.water >> (16 - 8 * k));
    }
    const rush2::catalog::Model* model = nullptr;
    for (const auto& m : dioramas.models) {
        if (m.name == fit.model && !m.lods.empty()) model = &m;
    }
    if (!model) return false;
    std::map<int, std::array<float, 3>> means;
    auto mean = [&](int t) {
        auto it = means.find(t);
        if (it != means.end()) return it->second;
        std::array<float, 3> c = { 255, 255, 255 };
        if (t >= 0 && t < (int)dioramas.textures.size()) {
            const auto& img = dioramas.textures[size_t(t)].image;
            double s[3] = {};
            size_t n = img.rgba.size() / 4;
            for (size_t i = 0; i < n; i++) for (int k = 0; k < 3; k++) s[k] += img.rgba[i * 4 + k];
            if (n > 0) for (int k = 0; k < 3; k++) c[k] = float(s[k] / n);
        }
        return means[t] = c;
    };
    const float rad = fit.deg * 3.14159265f / 180.0f, cs = std::cos(rad), sn = std::sin(rad);
    for (const auto& m : model->lods[0].meshes) {
        std::array<float, 3> base = mean(m.texture);
        for (size_t i = 0; i + 2 < m.vertices.size(); i += 3) {
            const auto* v = &m.vertices[i];
            float e1[3], e2[3];
            for (int k = 0; k < 3; k++) {
                e1[k] = v[1].position[k] - v[0].position[k];
                e2[k] = v[2].position[k] - v[0].position[k];
            }
            float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len < 1e-6f || std::fabs(n[1]) < 0.5f * len) continue;
            float c[3];
            for (int k = 0; k < 3; k++) {
                float vc = m.lit ? 255.0f : (v[0].color[k] + v[1].color[k] + v[2].color[k]) / 3.0f;
                c[k] = base[k] * vc / 255.0f;
            }
            Kind kind = kind_of(c);
            Guide::Triangle t;
            t.kind = kind;
            for (int k = 0; k < 3; k++) t.rgb[k] = uint8_t(std::clamp(c[k], 0.0f, 255.0f));
            for (int p = 0; p < 3; p++) {
                // The dumped glb's frame (x negated), the fit, then the builder's frame (x negated back).
                float dx = -v[p].position[0], dz = v[p].position[2];
                if (fit.mirror) dx = -dx;
                float fx = fit.s * (cs * dx - sn * dz) + fit.tx, fz = fit.s * (sn * dx + cs * dz) + fit.tz;
                t.v[p][0] = -fx;
                t.v[p][1] = fit.s * v[p].position[1];
                t.v[p][2] = fz;
            }
            out.triangles.push_back(t);
        }
    }
    return !out.triangles.empty();
}

}
