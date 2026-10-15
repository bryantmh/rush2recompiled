// Baking the cars' light probes for a track (include/car_probes.h, docs/graphics_effects.md#car-light-probes). Nothing
// here touches the game, so it builds on its own for testing (tools/car_probes_test.cpp).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <thread>

#include "asset_catalog.h"
#include "car_probes.h"

namespace rush2::probes {

namespace {
    using rush2::catalog::Container;
    using rush2::catalog::Placement;

    // A probe's height above the surface it stands on: about the middle of a car's body.
    constexpr float probe_height = 2.5f;
    // Rays per probe (a Fibonacci sphere, the same directions for every probe).
    // Hits further than this count as sky: the sky dome and the distant backdrops aren't lit surfaces near the car.
    constexpr float sky_distance = 400.0f;
    // Light a surface gives back, on top of its baked color times its texture's: N64 vertex colors are darker than the
    // light a surface really bounces, and tunnels came out nearly black without it.
    constexpr float bounce_gain = 1.6f;
    // The range a face's light is kept in, relative to an open road's. No side gets brighter than an open road: a
    // lamp-lit tunnel floor came out at up to 1.4 from below, and cars glowed from underneath.
    constexpr float min_light = 0.25f, max_light = 1.0f;
    // Triangles bigger than this (sq ft) get probes on a lattice of twice the spacing: the sea, the backdrop's hills,
    // the arenas' floors and long stretches of road. At full density they were most of a track's probes (Hawaii
    // 480,000 of 580,000); with none, cars on them were lit neutral, white next to the probes around (New York Two's
    // 42nd Street).
    constexpr float max_spawn_area = 20000.0f;
    // The car's roof over a probe (feet): what a ray hits lower than this lights only the underside.
    constexpr float roof_height = 2.0f;
    // The faces' order in Cube::c.
    constexpr float face_dirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

    struct Tri {
        float a[3], e1[3], e2[3];
        float n[3];          // Front-face normal (the game winds front faces clockwise in its left-handed space).
        float light[3];      // Baked color times texture color times bounce_gain.
    };

    struct Bvh {
        struct Node {
            float lo[3], hi[3];
            uint32_t first, count;   // count 0: an inner node whose children are first and first + 1
        };
        std::vector<Node> nodes;
        std::vector<uint32_t> order;

        void build(const std::vector<Tri>& tris) {
            order.resize(tris.size());
            for (uint32_t i = 0; i < tris.size(); i++) order[i] = i;
            std::vector<float> centers(tris.size() * 3);
            for (size_t i = 0; i < tris.size(); i++) {
                for (int k = 0; k < 3; k++) {
                    centers[i * 3 + k] = tris[i].a[k] + (tris[i].e1[k] + tris[i].e2[k]) / 3.0f;
                }
            }
            nodes.clear();
            nodes.reserve(tris.size() * 2 / 3 + 1);
            nodes.push_back({});
            struct Job { uint32_t node, first, count; };
            std::vector<Job> jobs = { { 0, 0, (uint32_t)tris.size() } };
            while (!jobs.empty()) {
                Job j = jobs.back();
                jobs.pop_back();
                Node n;
                for (int k = 0; k < 3; k++) { n.lo[k] = 1e30f; n.hi[k] = -1e30f; }
                float clo[3] = { 1e30f, 1e30f, 1e30f }, chi[3] = { -1e30f, -1e30f, -1e30f };
                for (uint32_t i = j.first; i < j.first + j.count; i++) {
                    const Tri& t = tris[order[i]];
                    for (int k = 0; k < 3; k++) {
                        float v[3] = { t.a[k], t.a[k] + t.e1[k], t.a[k] + t.e2[k] };
                        n.lo[k] = std::min({ n.lo[k], v[0], v[1], v[2] });
                        n.hi[k] = std::max({ n.hi[k], v[0], v[1], v[2] });
                        clo[k] = std::min(clo[k], centers[order[i] * 3 + k]);
                        chi[k] = std::max(chi[k], centers[order[i] * 3 + k]);
                    }
                }
                if (j.count <= 4) {
                    n.first = j.first;
                    n.count = j.count;
                    nodes[j.node] = n;
                    continue;
                }
                int axis = 0;
                for (int k = 1; k < 3; k++) {
                    if (chi[k] - clo[k] > chi[axis] - clo[axis]) axis = k;
                }
                uint32_t mid = j.count / 2;
                std::nth_element(order.begin() + j.first, order.begin() + j.first + mid, order.begin() + j.first + j.count,
                    [&](uint32_t x, uint32_t y) { return centers[x * 3 + axis] < centers[y * 3 + axis]; });
                n.first = (uint32_t)nodes.size();
                n.count = 0;
                nodes[j.node] = n;
                nodes.push_back({});
                nodes.push_back({});
                jobs.push_back({ n.first, j.first, mid });
                jobs.push_back({ n.first + 1, j.first + mid, j.count - mid });
            }
        }

        // The nearest triangle a ray hits within max_t, or -1; t gets its distance.
        int trace(const std::vector<Tri>& tris, const float o[3], const float d[3], float max_t, float& t) const {
            float inv[3];
            for (int k = 0; k < 3; k++) inv[k] = std::fabs(d[k]) > 1e-12f ? 1.0f / d[k] : 1e30f;
            int hit = -1;
            t = max_t;
            uint32_t stack[64];
            int sp = 0;
            stack[sp++] = 0;
            while (sp > 0) {
                const Node& n = nodes[stack[--sp]];
                float t0 = 0, t1 = t;
                for (int k = 0; k < 3; k++) {
                    float a = (n.lo[k] - o[k]) * inv[k], b = (n.hi[k] - o[k]) * inv[k];
                    if (a > b) std::swap(a, b);
                    t0 = std::max(t0, a);
                    t1 = std::min(t1, b);
                }
                if (t0 > t1) continue;
                if (n.count == 0) {
                    if (sp < 62) {
                        stack[sp++] = n.first;
                        stack[sp++] = n.first + 1;
                    }
                    continue;
                }
                for (uint32_t i = n.first; i < n.first + n.count; i++) {
                    const Tri& tr = tris[order[i]];
                    // Moller-Trumbore, both sides.
                    float p[3] = { d[1] * tr.e2[2] - d[2] * tr.e2[1], d[2] * tr.e2[0] - d[0] * tr.e2[2], d[0] * tr.e2[1] - d[1] * tr.e2[0] };
                    float det = tr.e1[0] * p[0] + tr.e1[1] * p[1] + tr.e1[2] * p[2];
                    if (std::fabs(det) < 1e-9f) continue;
                    float id = 1.0f / det;
                    float s[3] = { o[0] - tr.a[0], o[1] - tr.a[1], o[2] - tr.a[2] };
                    float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * id;
                    if (u < 0 || u > 1) continue;
                    float q[3] = { s[1] * tr.e1[2] - s[2] * tr.e1[1], s[2] * tr.e1[0] - s[0] * tr.e1[2], s[0] * tr.e1[1] - s[1] * tr.e1[0] };
                    float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * id;
                    if (v < 0 || u + v > 1) continue;
                    float tt = (tr.e2[0] * q[0] + tr.e2[1] * q[1] + tr.e2[2] * q[2]) * id;
                    if (tt > 1e-3f && tt < t) {
                        t = tt;
                        hit = (int)order[i];
                    }
                }
            }
            return hit;
        }
    };

    float luminance(const float c[3]) {
        return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    }

    uint64_t cell_key(int x, int y, int z) {
        return (uint64_t(uint32_t(x) & 0x1FFFFF) << 42) | (uint64_t(uint32_t(y) & 0x1FFFFF) << 21) | uint64_t(uint32_t(z) & 0x1FFFFF);
    }
    void cell_of_key(uint64_t key, int out[3]) {
        for (int k = 0; k < 3; k++) {
            int v = int((key >> (42 - 21 * k)) & 0x1FFFFF);
            out[k] = v >= 0x100000 ? v - 0x200000 : v;
        }
    }

    // The model a placement record draws, by name as write_glb finds it: an exact name, else the shortest model name
    // starting with it.
    struct ModelRef {
        const Container* container = nullptr;
        size_t model = 0;
    };
    ModelRef find_model(const std::vector<const Container*>& containers,
                        const std::vector<std::map<std::string, size_t>>& names, const std::string& name) {
        if (name.empty()) return {};
        for (size_t c = 0; c < containers.size(); c++) {
            if (auto it = names[c].find(name); it != names[c].end()) return { containers[c], it->second };
        }
        for (size_t c = 0; c < containers.size(); c++) {
            ModelRef best;
            for (auto it = names[c].lower_bound(name); it != names[c].end() && it->first.rfind(name, 0) == 0; ++it) {
                if (!best.container || it->first.size() < containers[c]->models[best.model].name.size()) {
                    best = { containers[c], it->second };
                }
            }
            if (best.container) return best;
        }
        return {};
    }
}

void Cube::eval(const float n[3], float out[3]) const {
    const float* x = c[n[0] >= 0 ? 0 : 1];
    const float* y = c[n[1] >= 0 ? 2 : 3];
    const float* z = c[n[2] >= 0 ? 4 : 5];
    float wx = n[0] * n[0], wy = n[1] * n[1], wz = n[2] * n[2];
    for (int k = 0; k < 3; k++) out[k] = wx * x[k] + wy * y[k] + wz * z[k];
}

Cube Cube::neutral() {
    Cube c;
    for (auto& f : c.c) f[0] = f[1] = f[2] = 1.0f;
    return c;
}

void ProbeSet::position(size_t i, float out[3]) const {
    int c[3];
    cell_of_key(keys[i], c);
    for (int k = 0; k < 3; k++) out[k] = (float(c[k]) + offsets[i * 3 + k] / 255.0f) * spacing;
}

bool ProbeSet::sample(const float pos[3], Cube& out, float radius, Painted* paint_out, float paint_radius) const {
    int cx = (int)std::floor(pos[0] / spacing), cy = (int)std::floor(pos[1] / spacing), cz = (int)std::floor(pos[2] / spacing);
    int reach = (int)std::ceil(radius / spacing);
    float r2 = radius * radius, p2 = paint_radius * paint_radius;
    bool painted = paint_out && !paint.empty() && paint_radius > 0;
    // Pass 0: near the car, height counting 3x. Pass 1 (none near: in the air): up to 80 ft under it, height a quarter.
    for (int pass = 0; pass < 2; pass++) {
        float sum[6][3] = {};
        float total = 0, paint_total = 0, paint_amount = 0, paint_rgb[3] = {};
        float height = pass == 0 ? 3.0f : 0.25f;
        int below = (int)std::ceil(80.0f / spacing);
        int near = (int)std::ceil(radius / 3.0f / spacing);
        for (int dy = pass == 0 ? -near : -below; dy <= (pass == 0 ? near : 0); dy++) {
            for (int dz = -reach; dz <= reach; dz++) {
                for (int dx = -reach; dx <= reach; dx++) {
                    uint64_t key = cell_key(cx + dx, cy + dy, cz + dz);
                    auto it = std::lower_bound(keys.begin(), keys.end(), key);
                    if (it == keys.end() || *it != key) continue;
                    size_t i = size_t(it - keys.begin());
                    float p[3];
                    position(i, p);
                    float ex = p[0] - pos[0], ey = (p[1] - pos[1]) * height, ez = p[2] - pos[2];
                    float d2 = ex * ex + ey * ey + ez * ez;
                    if (d2 >= r2) continue;
                    float w = 1.0f - d2 / r2;
                    w *= w;
                    const uint8_t* f = &faces[i * 18];
                    for (int k = 0; k < 18; k++) sum[k / 3][k % 3] += w * float(f[k]) / 128.0f;
                    total += w;
                    if (painted && pass == 0) {
                        // The paint's own radius, plus 2% of the light's weight: a probe further out only decides when
                        // none is within the paint radius, and one coming within it takes over smoothly.
                        float pw = 0.02f * w;
                        if (d2 < p2) {
                            float q = 1.0f - d2 / p2;
                            pw += q * q;
                        }
                        const uint8_t* c = &paint[i * 4];
                        float a = c[3] / 255.0f;
                        for (int k = 0; k < 3; k++) paint_rgb[k] += pw * a * c[k] / 255.0f;
                        paint_amount += pw * a;
                        paint_total += pw;
                    }
                }
            }
        }
        if (total <= 0) continue;
        for (int k = 0; k < 18; k++) out.c[k / 3][k % 3] = sum[k / 3][k % 3] / total;
        if (paint_total > 0) {
            paint_out->amount = paint_amount / paint_total;
            for (int k = 0; k < 3; k++) paint_out->rgb[k] = paint_amount > 0 ? paint_rgb[k] / paint_amount : 1.0f;
        }
        return true;
    }
    return false;
}

Cube CarLight::target(const ProbeSet& set, const float pos[3], float radius) {
    Cube t;
    if (!set.sample(pos, t, radius)) t = Cube::neutral();
    return t;
}

void CarLight::soften(Cube& c, float contrast) {
    // The mean of every face but the underside: the underside moves toward the rest, never the rest toward it (the
    // ground's color stays off the roof).
    float mean[3] = {};
    for (int f = 0; f < 6; f++) {
        if (f != 3) for (int k = 0; k < 3; k++) mean[k] += c.c[f][k] / 5.0f;
    }
    for (auto& f : c.c) for (int k = 0; k < 3; k++) f[k] = mean[k] + (f[k] - mean[k]) * contrast;
}

void CarLight::paint(Cube& c, const Painted& painted) {
    if (painted.amount <= 0) return;
    // The mean of every face but the underside: the ground doesn't change the paint on the rest.
    float mean = 0;
    for (int f = 0; f < 6; f++) {
        if (f != 3) mean += luminance(c.c[f]) / 5.0f;
    }
    mean = std::max(mean, 0.05f);
    for (auto& f : c.c) {
        float shape = std::min(luminance(f) / mean, 1.0f);
        float l = std::max(luminance(f), 1e-3f);
        for (int k = 0; k < 3; k++) {
            float tinted = f[k] / l * shape * painted.rgb[k];
            f[k] = (1.0f - painted.amount) * f[k] + painted.amount * tinted;
        }
    }
}

Cube CarLight::lit(const Painted* painted, const Params& params) const {
    Cube c = cube;
    soften(c, params.contrast);
    if (painted) paint(c, *painted);
    return c;
}

void CarLight::step(const ProbeSet& set, const float pos[3], const Painted* under, float dt, const Params& params) {
    float moved[3] = { pos[0] - last[0], pos[1] - last[1], pos[2] - last[2] };
    float dist = std::sqrt(moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2]);
    // A jump (a respawn, the race's first frame) takes the new place's light at once.
    bool snap = !valid || dist > 60.0f || dt <= 0;
    float ahead[3] = { pos[0], pos[1], pos[2] };
    if (!snap && dist > 1e-4f) {
        float look = std::min(dist / dt * params.lookahead, params.max_lookahead);
        for (int k = 0; k < 3; k++) ahead[k] += moved[k] / dist * look;
    }
    Cube t;
    Painted near{ { 1, 1, 1 }, -1 };
    if (!set.sample(ahead, t, params.radius, &near, params.paint_radius)) t = Cube::neutral();
    if (near.amount >= 0) under = &near;
    float a = snap ? 1.0f : 1.0f - std::exp(-dt / params.ease_time);
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 3; k++) cube.c[f][k] += (t.c[f][k] - cube.c[f][k]) * a;
    }
    // The painted colour eases on its own, quicker: leaving paint fades the amount and keeps the last colour.
    float pa = snap || params.paint_ease <= 0 ? 1.0f : 1.0f - std::exp(-dt / params.paint_ease);
    if (under && under->amount > 0) {
        // Onto paint from none takes its colour at once, so the fade in has no other colour in it.
        for (int k = 0; k < 3; k++) painted.rgb[k] = painted.amount > 0 ? painted.rgb[k] + (under->rgb[k] - painted.rgb[k]) * pa : under->rgb[k];
    }
    float amount = under ? under->amount : 0.0f;
    painted.amount += (amount - painted.amount) * pa;
    if (painted.amount < 1e-3f) painted.amount = 0;
    memcpy(last, pos, sizeof(last));
    valid = true;
}

std::vector<uint8_t> ProbeSet::serialize() const {
    uint32_t n = (uint32_t)count();
    // version, count, spacing, flags (none yet), then keys, offsets, faces and (when the set has paint) the paint.
    std::vector<uint8_t> out(16 + size_t(n) * (8 + 3 + 18) + paint.size());
    uint32_t flags = 0;
    memcpy(&out[0], &bake_version, 4);
    memcpy(&out[4], &n, 4);
    memcpy(&out[8], &spacing, 4);
    memcpy(&out[12], &flags, 4);
    memcpy(&out[16], keys.data(), size_t(n) * 8);
    memcpy(&out[16 + size_t(n) * 8], offsets.data(), offsets.size());
    memcpy(&out[16 + size_t(n) * 11], faces.data(), faces.size());
    if (!paint.empty()) memcpy(&out[16 + size_t(n) * 29], paint.data(), paint.size());
    return out;
}

bool ProbeSet::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 16) return false;
    uint32_t version, n;
    memcpy(&version, &data[0], 4);
    memcpy(&n, &data[4], 4);
    size_t plain = 16 + size_t(n) * (8 + 3 + 18);
    if (version != bake_version || (data.size() != plain && data.size() != plain + size_t(n) * 4)) return false;
    memcpy(&spacing, &data[8], 4);
    keys.resize(n);
    offsets.resize(size_t(n) * 3);
    faces.resize(size_t(n) * 18);
    memcpy(keys.data(), &data[16], size_t(n) * 8);
    memcpy(offsets.data(), &data[16 + size_t(n) * 8], offsets.size());
    memcpy(faces.data(), &data[16 + size_t(n) * 11], faces.size());
    paint.clear();
    if (data.size() > plain) paint.assign(data.begin() + plain, data.end());
    return true;
}

std::shared_ptr<ProbeSet> bake(const std::vector<const Container*>& containers, const std::vector<Placement>& placements,
                               BakeStats* stats, const BakeParams& params, const PaintSource* paint) {
    auto start = std::chrono::steady_clock::now();
    std::vector<std::map<std::string, size_t>> names(containers.size());
    for (size_t c = 0; c < containers.size(); c++) {
        for (size_t m = 0; m < containers[c]->models.size(); m++) names[c].emplace(containers[c]->models[m].name, m);
    }
    // Each texture's average color, and how much of it is see-through (cutouts: trees, fences) to leave out.
    std::map<std::pair<const Container*, int>, std::array<float, 4>> albedo;
    auto texture_color = [&](const Container* c, int t) {
        auto key = std::make_pair(c, t);
        if (auto it = albedo.find(key); it != albedo.end()) return it->second;
        std::array<float, 4> a = { 1, 1, 1, 1 };
        if (t >= 0 && t < (int)c->textures.size() && !c->textures[t].image.empty()) {
            const auto& px = c->textures[t].image.rgba;
            double s[4] = {};
            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                double al = px[i + 3] / 255.0;
                for (int k = 0; k < 3; k++) s[k] += px[i + k] / 255.0 * al;
                s[3] += al;
            }
            size_t count = px.size() / 4;
            if (s[3] > 0) for (int k = 0; k < 3; k++) a[k] = float(s[k] / s[3]);
            a[3] = count ? float(s[3] / count) : 1.0f;
        }
        albedo[key] = a;
        return a;
    };

    // The world's triangles: v * matrix / 16 + position (row vectors; models are drawn at 1/16 scale).
    std::vector<Tri> tris;
    for (const Placement& p : placements) {
        ModelRef ref = find_model(containers, names, p.name);
        if (!ref.container || ref.container->models[ref.model].lods.empty()) continue;
        const float* m = p.matrix;
        float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
        for (const auto& mesh : ref.container->models[ref.model].lods[0].meshes) {
            auto a = texture_color(ref.container, mesh.texture);
            if (a[3] < 0.6f) continue;
            for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
                float w[3][3];
                float vc[3] = {};
                for (int v = 0; v < 3; v++) {
                    const auto& vx = mesh.vertices[i + v];
                    for (int k = 0; k < 3; k++) {
                        w[v][k] = (vx.position[0] * m[0 * 3 + k] + vx.position[1] * m[1 * 3 + k] + vx.position[2] * m[2 * 3 + k]) / 16.0f + p.position[k];
                        // A lit LOD's vertex colors are its normals.
                        vc[k] += (mesh.lit ? 0.7f : vx.color[k] / 255.0f) / 3.0f;
                    }
                }
                Tri t;
                for (int k = 0; k < 3; k++) {
                    t.a[k] = w[0][k];
                    t.e1[k] = w[1][k] - w[0][k];
                    t.e2[k] = w[2][k] - w[0][k];
                    t.light[k] = vc[k] * a[k] * bounce_gain;
                }
                // Clockwise front faces: the front normal is e2 x e1 (turned for a mirroring placement).
                float n[3] = { t.e2[1] * t.e1[2] - t.e2[2] * t.e1[1], t.e2[2] * t.e1[0] - t.e2[0] * t.e1[2], t.e2[0] * t.e1[1] - t.e2[1] * t.e1[0] };
                float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len < 1e-6f) continue;
                for (int k = 0; k < 3; k++) t.n[k] = n[k] / len * (det < 0 ? -1.0f : 1.0f);
                tris.push_back(t);
            }
        }
    }
    auto set = std::make_shared<ProbeSet>();
    if (tris.empty()) return set;
    Bvh bvh;
    bvh.build(tris);

    // Probe spots: a car's height over the upward-facing triangles, one per cell of `spacing`.
    set->spacing = params.spacing;
    const float s = params.spacing;
    std::unordered_map<uint64_t, uint32_t> taken;
    std::vector<float> spots;
    std::vector<uint64_t> spot_keys;
    // coarse: a spot on a big triangle (left out of the open road's reference light: the sea and fields).
    std::vector<uint8_t> spot_coarse;
    auto add_spot = [&](float p[3], bool coarse) {
        p[1] += probe_height;
        uint64_t key = cell_key((int)std::floor(p[0] / s), (int)std::floor(p[1] / s), (int)std::floor(p[2] / s));
        if (taken.emplace(key, 0).second) {
            spots.insert(spots.end(), p, p + 3);
            spot_keys.push_back(key);
            spot_coarse.push_back(coarse);
        }
    };
    uint32_t hash = 0x9E3779B9u;
    for (const Tri& t : tris) {
        if (t.n[1] < 0.7f) continue;
        float c[3] = { t.e1[1] * t.e2[2] - t.e1[2] * t.e2[1], t.e1[2] * t.e2[0] - t.e1[0] * t.e2[2], t.e1[0] * t.e2[1] - t.e1[1] * t.e2[0] };
        float area = 0.5f * std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        // A big triangle (open ground, a long road) gets probes on a 20 ft lattice: a car on it is always within
        // 14 ft of one, inside the sample radius.
        int stride = area > max_spawn_area ? std::max(1, (int)std::lround(20.0f / s)) : 1;
        if (params.grid || stride > 1) {
            // The cell centers (x, z) the triangle covers seen from above, at the triangle's height there.
            float x[3] = { t.a[0], t.a[0] + t.e1[0], t.a[0] + t.e2[0] }, z[3] = { t.a[2], t.a[2] + t.e1[2], t.a[2] + t.e2[2] };
            float det = t.e1[0] * t.e2[2] - t.e2[0] * t.e1[2];
            if (std::fabs(det) < 1e-6f) continue;
            int x0 = (int)std::floor(std::min({ x[0], x[1], x[2] }) / s), x1 = (int)std::floor(std::max({ x[0], x[1], x[2] }) / s);
            int z0 = (int)std::floor(std::min({ z[0], z[1], z[2] }) / s), z1 = (int)std::floor(std::max({ z[0], z[1], z[2] }) / s);
            for (int gz = z0; gz <= z1; gz++) {
                for (int gx = x0; gx <= x1; gx++) {
                    if (stride > 1 && ((gx % stride + stride) % stride || (gz % stride + stride) % stride)) continue;
                    float px = (gx + 0.5f) * s - t.a[0], pz = (gz + 0.5f) * s - t.a[2];
                    float u = (px * t.e2[2] - t.e2[0] * pz) / det, v = (t.e1[0] * pz - px * t.e1[2]) / det;
                    if (u < 0 || v < 0 || u + v > 1) continue;
                    float p[3];
                    for (int k = 0; k < 3; k++) p[k] = t.a[k] + u * t.e1[k] + v * t.e2[k];
                    add_spot(p, stride > 1);
                }
            }
            continue;
        }
        // Points spread over the triangle, about one per cell's area (a small triangle gets one by chance).
        float want = area / (s * s * 0.5f);
        int points = (int)want;
        hash = hash * 1664525u + 1013904223u;
        if ((hash >> 8) * (1.0f / 16777216.0f) < want - points) points++;
        for (int i = 0; i < points; i++) {
            hash = hash * 1664525u + 1013904223u;
            float u = (hash >> 8) * (1.0f / 16777216.0f);
            hash = hash * 1664525u + 1013904223u;
            float v = (hash >> 8) * (1.0f / 16777216.0f);
            if (u + v > 1) { u = 1 - u; v = 1 - v; }
            float p[3];
            for (int k = 0; k < 3; k++) p[k] = t.a[k] + u * t.e1[k] + v * t.e2[k];
            add_spot(p, false);
        }
    }

    // The ray directions: a Fibonacci sphere.
    const int ray_count = std::max(8, params.rays);
    std::vector<std::array<float, 3>> dirs(ray_count);
    for (int i = 0; i < ray_count; i++) {
        float y = 1.0f - (i + 0.5f) * 2.0f / ray_count;
        float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        float phi = i * 2.39996323f;
        dirs[i] = { std::cos(phi) * r, y, std::sin(phi) * r };
    }
    size_t n = spots.size() / 3;
    std::vector<float> cubes(n * 18);
    std::vector<uint8_t> keep(n), open(n);
    std::atomic<size_t> next{ 0 };
    auto work = [&]() {
        for (size_t i; (i = next.fetch_add(64)) < n;) {
            for (size_t j = i; j < std::min(n, i + 64); j++) {
                const float* o = &spots[j * 3];
                // A random rotation for this probe's rays (a quaternion from its index), with params.rotate.
                float rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
                if (params.rotate) {
                    uint32_t h = uint32_t(j) * 2654435761u + 12345u;
                    float q[4];
                    for (float& v : q) {
                        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
                        v = (h >> 8) * (2.0f / 16777216.0f) - 1.0f;
                    }
                    float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                    if (l > 1e-3f) {
                        float w = q[0] / l, x = q[1] / l, y = q[2] / l, z = q[3] / l;
                        float m[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                                       2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                                       2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) };
                        memcpy(rot, m, sizeof(m));
                    }
                }
                float sum[6][3] = {}, weight[6] = {};
                int back = 0, up_rays = 0, up_sky = 0;
                for (int r = 0; r < ray_count; r++) {
                    float d[3];
                    for (int k = 0; k < 3; k++) d[k] = rot[k * 3] * dirs[r][0] + rot[k * 3 + 1] * dirs[r][1] + rot[k * 3 + 2] * dirs[r][2];
                    float t;
                    int hit = bvh.trace(tris, o, d, sky_distance, t);
                    float light[3] = { 1, 1, 1 };
                    if (hit >= 0) {
                        const Tri& tr = tris[hit];
                        for (int k = 0; k < 3; k++) light[k] = tr.light[k];
                        if (tr.n[0] * d[0] + tr.n[1] * d[1] + tr.n[2] * d[2] > 0) back++;
                    }
                    if (d[1] > 0.5f) {
                        up_rays++;
                        if (hit < 0) up_sky++;
                    }
                    for (int f = 0; f < 6; f++) {
                        float w = d[0] * face_dirs[f][0] + d[1] * face_dirs[f][1] + d[2] * face_dirs[f][2];
                        if (w <= 0) continue;
                        // The top and the sides see only what is above the car: rays going up, missing (the sky) or
                        // hitting something higher than its roof (roof_height over the probe). Everything else (the
                        // ground, curbs, grass banks) lights the underside alone, or it changed the roof, hood and deck.
                        if (f != 3 && (d[1] < 0 || (hit >= 0 && t * d[1] < roof_height))) continue;
                        for (int k = 0; k < 3; k++) sum[f][k] += w * light[k];
                        weight[f] += w;
                    }
                }
                // Mostly back faces around it: the probe is inside something (a deck, a wall), not where a car drives.
                keep[j] = back * 10 < ray_count * 4;
                open[j] = up_rays > 0 && up_sky * 10 >= up_rays * 9;
                for (int f = 0; f < 6; f++) {
                    for (int k = 0; k < 3; k++) cubes[j * 18 + f * 3 + k] = weight[f] > 0 ? sum[f][k] / weight[f] : 1.0f;
                }
            }
        }
    };
    unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()) - 1);
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; i++) pool.emplace_back(work);
    for (auto& t : pool) t.join();

    // params.smooth: each kept probe becomes the weighted average of the kept probes within that radius (height
    // counting 3x, weights (1 - d^2 / r^2)^2), which takes out the ray noise and one probe's odd view.
    if (params.smooth > 0) {
        std::unordered_map<uint64_t, uint32_t> at;
        for (uint32_t j = 0; j < n; j++) {
            if (keep[j]) at.emplace(spot_keys[j], j);
        }
        std::vector<float> smoothed(cubes.size());
        const float r = params.smooth, r2 = r * r;
        const int reach = (int)std::ceil(r / s);
        for (uint32_t j = 0; j < n; j++) {
            if (!keep[j]) continue;
            int c[3];
            cell_of_key(spot_keys[j], c);
            float sum[18] = {}, total = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dz = -reach; dz <= reach; dz++) {
                    for (int dx = -reach; dx <= reach; dx++) {
                        auto it = at.find(cell_key(c[0] + dx, c[1] + dy, c[2] + dz));
                        if (it == at.end()) continue;
                        const float* p = &spots[it->second * 3];
                        const float* o = &spots[j * 3];
                        float ex = p[0] - o[0], ey = (p[1] - o[1]) * 3.0f, ez = p[2] - o[2];
                        float d2 = ex * ex + ey * ey + ez * ez;
                        if (d2 >= r2) continue;
                        float w = 1.0f - d2 / r2;
                        w *= w;
                        for (int k = 0; k < 18; k++) sum[k] += w * cubes[it->second * 18 + k];
                        total += w;
                    }
                }
            }
            for (int k = 0; k < 18; k++) smoothed[j * 18 + k] = sum[k] / total;
        }
        for (uint32_t j = 0; j < n; j++) {
            if (keep[j]) memcpy(&cubes[j * 18], &smoothed[j * 18], 18 * sizeof(float));
        }
    }

    // Each face's open-road light: the median over the probes open to the sky above. An arena closed over (PIPE,
    // HALFPIPE) has next to no open road: there the median over all its probes is taken as normal, as the original
    // game lights its cars the same everywhere in it.
    float reference[6];
    size_t open_count = 0, kept = 0;
    for (size_t j = 0; j < n; j++) {
        open_count += keep[j] && open[j] && !spot_coarse[j];
        kept += keep[j] && !spot_coarse[j];
    }
    bool enclosed = open_count * 20 < kept;
    for (int f = 0; f < 6; f++) {
        std::vector<float> l;
        for (size_t j = 0; j < n; j++) {
            if (keep[j] && !spot_coarse[j] && (open[j] || enclosed)) l.push_back(luminance(&cubes[j * 18 + f * 3]));
        }
        reference[f] = 1.0f;
        if (!l.empty()) {
            std::nth_element(l.begin(), l.begin() + l.size() / 2, l.end());
            reference[f] = std::max(0.05f, l[l.size() / 2]);
        }
    }
    std::vector<uint32_t> sorted;
    for (uint32_t j = 0; j < n; j++) {
        if (keep[j]) sorted.push_back(j);
    }
    std::sort(sorted.begin(), sorted.end(), [&](uint32_t x, uint32_t y) { return spot_keys[x] < spot_keys[y]; });
    for (uint32_t j : sorted) {
        set->keys.push_back(spot_keys[j]);
        int c[3];
        cell_of_key(spot_keys[j], c);
        for (int k = 0; k < 3; k++) {
            float o = spots[j * 3 + k] / s - float(c[k]);
            set->offsets.push_back((uint8_t)std::clamp(std::lround(o * 255.0f), 0l, 255l));
        }
        for (int f = 0; f < 6; f++) {
            for (int k = 0; k < 3; k++) {
                float v = std::clamp(cubes[j * 18 + f * 3 + k] / reference[f], min_light, max_light);
                set->faces.push_back((uint8_t)std::lround(v * 128.0f));
            }
        }
    }
    // The painted colour under each probe: the nearest painted polygon straight down within 8 ft of the ground
    // (probe_height over it), else none.
    if (paint && !paint->colours.empty()) {
        std::vector<Tri> ptris(paint->colours.size());
        for (size_t i = 0; i < ptris.size(); i++) {
            const float* v = &paint->tris[i * 9];
            Tri& t = ptris[i];
            for (int k = 0; k < 3; k++) {
                t.a[k] = v[k];
                t.e1[k] = v[3 + k] - v[k];
                t.e2[k] = v[6 + k] - v[k];
                t.n[k] = 0;
                t.light[k] = 0;
            }
        }
        Bvh pbvh;
        pbvh.build(ptris);
        const float down[3] = { 0, -1, 0 };
        set->paint.assign(set->count() * 4, 0);
        for (size_t i = 0; i < set->count(); i++) {
            float p[3];
            set->position(i, p);
            p[1] += 2.0f;
            float t;
            int hit = pbvh.trace(ptris, p, down, probe_height + 2.0f + 8.0f, t);
            if (hit < 0) continue;
            uint32_t c = paint->colours[hit];
            uint8_t* o = &set->paint[i * 4];
            o[0] = uint8_t(c >> 24);
            o[1] = uint8_t(c >> 16);
            o[2] = uint8_t(c >> 8);
            o[3] = 255;
        }
    }
    if (stats) {
        stats->triangles = tris.size();
        stats->probes = set->count();
        stats->open = open_count;
        memcpy(stats->reference, reference, sizeof(reference));
        stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    return set;
}

}
