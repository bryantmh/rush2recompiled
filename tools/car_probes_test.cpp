// Bakes the cars' light probes (src/car_probes_bake.cpp, docs/graphics_effects.md#car-light-probes) outside the game.
//
//   car_probes_test [TRACK ...]                      per track: size, bake time, how the light spreads
//   car_probes_test --path RUN_LOG TRACK             CarLight::Params compared on recorded paths
//   car_probes_test --compare [--path RUN_LOG] TRACK BakeParams variants compared
//
// TRACK is a Rush 2 track slot (0-11; none: all twelve) or k49:K for Rush 2049 track K (1 Marina ... 6 Presidio),
// converted as the game converts it (RUSH2049_ROM). The first form prints percentiles of the light from above and the
// sides, to tune the bake against every stock track at once.
//
// --path replays the cars' paths a game run recorded (RUSH2_PROBE_TEST=path, "[ProbePath] t car x y z" lines in the
// run's log) through rush2::probes::CarLight, and prints per setting:
// - step p95 / max: the 95th percentile and the largest change of any face in one 50 ms record (open road = 1);
// - flicker: swings of the cube's mean light of over 0.02 that turn back (a light going up then down again, or down
//   then up) per minute of driving: popping;
// - lag: the time shift that best matches the light to the light at the car (negative = ahead).
// --compare bakes the track with each BakeParams variant (spacing, rays, grid placement, rotated rays) and prints its
// probe count, size and bake time, how much neighbouring probes differ (mean and 95th percentile of the largest face
// difference between a probe and the one a cell over in x), and with --path the replay scores at the default Params.
//
// Reads RUSH2_ROM (default rush2.us.recomp.z64 in the project root). Build from Git Bash:
//   cmd //c "tools\car_probes_test.bat"   (writes tmp\probes_test\car_probes_test.exe)
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "asset_catalog.h"
#include "assets.h"
#include "car_probes.h"
#include "rush2049_rom.h"
#include "track2049_convert.h"

using namespace rush2::catalog;
using rush2::probes::CarLight;
using rush2::probes::Cube;
using rush2::probes::ProbeSet;

static std::vector<uint8_t> read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
    return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

struct Point {
    float t, p[3];
    float paint = 0, rgb[3] = {};          // The 2049 painted colour under the car (amount, colour), when logged
};
using Paths = std::map<int, std::vector<Point>>;

static Paths load_paths(const std::string& log) {
    Paths paths;
    std::ifstream in(log);
    for (std::string line; std::getline(in, line);) {
        size_t at = line.find("[ProbePath] ");
        if (at == std::string::npos) continue;
        std::istringstream ls(line.substr(at + 12));
        Point pt;
        int car;
        if (!(ls >> pt.t >> car >> pt.p[0] >> pt.p[1] >> pt.p[2])) continue;
        if (!(ls >> pt.paint >> pt.rgb[0] >> pt.rgb[1] >> pt.rgb[2])) pt.paint = 0;
        paths[car].push_back(pt);
    }
    return paths;
}

static float mean_light(const Cube& c) {
    float m = 0;
    for (int f = 0; f < 6; f++) m += (0.299f * c.c[f][0] + 0.587f * c.c[f][1] + 0.114f * c.c[f][2]) / 6.0f;
    return m;
}

struct Scores {
    float step95 = 0, step_max = 0, flicker = 0, lag = 0;
    float face95[6] = {};                  // Step p95 of each face as shown (softened; +x -x +y -y +z -z)
    // The car's upper body as shown (the average over the roof's normal and the hood's, deck's and sides' at 45
    // degrees up): its 50 ms step p95 (largest channel), and per minute its swings of any channel over 0.015 that turn
    // back (color flicker).
    float body95 = 0, body_flicker = 0;
    float top_flicker = 0;                 // Flicker/min of the shown top face alone
    float paint_behind = 0;                // Mean |eased painted amount - logged amount|: how far easing trails
    float holes = 0;                       // Percent of records with no probe in reach (the car drawn neutral)
};

// The --path scores of one CarLight::Params on a probe set.
// The light on a car's upper body: the cube at the roof's normal and the hood's, deck's and sides' 45 degrees up.
static void upper_body(const Cube& c, float out[3]) {
    const float d = 0.7071f;
    const float normals[5][3] = { { 0, 1, 0 }, { 0, d, d }, { 0, d, -d }, { d, d, 0 }, { -d, d, 0 } };
    out[0] = out[1] = out[2] = 0;
    for (const auto& n : normals) {
        float v[3];
        c.eval(n, v);
        for (int k = 0; k < 3; k++) out[k] += v[k] / 5.0f;
    }
}

// use_paint: the shown cube (face steps, top flicker) has the logged painted colour on it, as the game draws it.
static Scores replay(const ProbeSet& set, const Paths& paths, const CarLight::Params& params, bool use_paint = false) {
    auto diff = [](const Cube& a, const Cube& b) {
        float d = 0;
        for (int f = 0; f < 6; f++) for (int k = 0; k < 3; k++) d = std::max(d, std::fabs(a.c[f][k] - b.c[f][k]));
        return d;
    };
    // A record is a jump (a respawn) when the car moved over 60 ft or the log skipped half a second.
    auto jump = [](const Point& a, const Point& b) {
        float d = 0;
        for (int k = 0; k < 3; k++) d += (a.p[k] - b.p[k]) * (a.p[k] - b.p[k]);
        return d > 3600.0f || b.t - a.t > 0.5f;
    };
    std::vector<float> steps, face_steps[6];
    std::vector<double> err(21, 0.0);
    std::vector<size_t> cnt(21, 0);
    double turns = 0, top_turns = 0, seconds = 0, paint_behind = 0, body_turns = 0;
    std::vector<float> body_steps;
    size_t paint_count = 0, holes = 0, records = 0;
    for (const auto& [car, path] : paths) {
        CarLight light;
        std::vector<float> got, raw, paint_got, paint_raw;
        Cube prev, prev_shown;
        float prev_body[3] = {}, body_extreme[3] = {};
        int body_dir[3] = {};
        float top_extreme = 0;
        int top_direction = 0;
        // The flicker count: a swing is counted when the light turns back after moving over 0.02 one way.
        float extreme = 0;
        int direction = 0;
        for (size_t i = 0; i < path.size(); i++) {
            float dt = i ? path[i].t - path[i - 1].t : 0.0f;
            bool jumped = i && jump(path[i - 1], path[i]);
            CarLight::Painted under{ { path[i].rgb[0], path[i].rgb[1], path[i].rgb[2] }, path[i].paint };
            light.step(set, path[i].p, under.amount > 0 ? &under : nullptr, dt, params);
            const CarLight::Painted* paint = use_paint && light.painted.amount > 0 ? &light.painted : nullptr;
            paint_got.push_back(light.painted.amount);
            paint_raw.push_back(under.amount);
            float m = mean_light(light.cube);
            if (i && !jumped) {
                steps.push_back(diff(light.cube, prev));
                Cube shown = light.lit(paint, params);
                for (int f = 0; f < 6; f++) {
                    float d = 0;
                    for (int k = 0; k < 3; k++) d = std::max(d, std::fabs(shown.c[f][k] - prev_shown.c[f][k]));
                    face_steps[f].push_back(d);
                }
                float body[3];
                upper_body(shown, body);
                float bd = 0;
                for (int k = 0; k < 3; k++) {
                    bd = std::max(bd, std::fabs(body[k] - prev_body[k]));
                    if (body_dir[k] >= 0 && body[k] < body_extreme[k] - 0.015f) {
                        if (body_dir[k] > 0) body_turns++;
                        body_dir[k] = -1;
                        body_extreme[k] = body[k];
                    }
                    else if (body_dir[k] <= 0 && body[k] > body_extreme[k] + 0.015f) {
                        if (body_dir[k] < 0) body_turns++;
                        body_dir[k] = 1;
                        body_extreme[k] = body[k];
                    }
                    else if ((body_dir[k] > 0 && body[k] > body_extreme[k]) || (body_dir[k] < 0 && body[k] < body_extreme[k])) {
                        body_extreme[k] = body[k];
                    }
                }
                body_steps.push_back(bd);
                float top = 0.299f * shown.c[2][0] + 0.587f * shown.c[2][1] + 0.114f * shown.c[2][2];
                if (top_direction >= 0 && top < top_extreme - 0.02f) {
                    if (top_direction > 0) top_turns++;
                    top_direction = -1;
                    top_extreme = top;
                }
                else if (top_direction <= 0 && top > top_extreme + 0.02f) {
                    if (top_direction < 0) top_turns++;
                    top_direction = 1;
                    top_extreme = top;
                }
                else if ((top_direction > 0 && top > top_extreme) || (top_direction < 0 && top < top_extreme)) {
                    top_extreme = top;
                }
                seconds += dt;
                if (direction >= 0 && m < extreme - 0.02f) {
                    if (direction > 0) turns++;
                    direction = -1;
                    extreme = m;
                }
                else if (direction <= 0 && m > extreme + 0.02f) {
                    if (direction < 0) turns++;
                    direction = 1;
                    extreme = m;
                }
                else if ((direction > 0 && m > extreme) || (direction < 0 && m < extreme)) {
                    extreme = m;
                }
            }
            else {
                extreme = m;
                direction = 0;
            }
            prev = light.cube;
            prev_shown = light.lit(paint, params);
            upper_body(prev_shown, prev_body);
            if (!i || jumped) {
                for (int k = 0; k < 3; k++) {
                    body_extreme[k] = prev_body[k];
                    body_dir[k] = 0;
                }
            }
            if (!i || jumped) {
                const float* c = prev_shown.c[2];
                top_extreme = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
                top_direction = 0;
            }
            got.push_back(m);
            raw.push_back(mean_light(CarLight::target(set, path[i].p, params.radius)));
            Cube near;
            if (!set.sample(path[i].p, near, params.radius)) holes++;
            records++;
        }
        for (size_t i = 0; i < paint_got.size(); i++) {
            paint_behind += std::fabs(paint_got[i] - paint_raw[i]);
            paint_count++;
        }
        // How well the light matches the light at the car `shift` records earlier.
        for (int shift = -10; shift <= 10; shift++) {
            for (size_t i = 10; i + 10 < got.size(); i++) {
                float e = got[i] - raw[i - shift];
                err[shift + 10] += e * e;
                cnt[shift + 10]++;
            }
        }
    }
    int best = 0;
    for (int k = 1; k < 21; k++) {
        if (err[k] / std::max<size_t>(cnt[k], 1) < err[best] / std::max<size_t>(cnt[best], 1)) best = k;
    }
    std::sort(steps.begin(), steps.end());
    Scores s;
    s.step95 = steps.empty() ? 0 : steps[steps.size() * 95 / 100];
    s.step_max = steps.empty() ? 0 : steps.back();
    for (int f = 0; f < 6; f++) {
        std::sort(face_steps[f].begin(), face_steps[f].end());
        s.face95[f] = face_steps[f].empty() ? 0 : face_steps[f][face_steps[f].size() * 95 / 100];
    }
    s.flicker = seconds > 0 ? float(turns / seconds * 60.0) : 0;
    s.top_flicker = seconds > 0 ? float(top_turns / seconds * 60.0) : 0;
    s.body_flicker = seconds > 0 ? float(body_turns / seconds * 60.0) : 0;
    std::sort(body_steps.begin(), body_steps.end());
    s.body95 = body_steps.empty() ? 0 : body_steps[body_steps.size() * 95 / 100];
    s.paint_behind = paint_count ? float(paint_behind / paint_count) : 0;
    s.holes = records ? float(100.0 * holes / records) : 0;
    s.lag = (best - 10) * 0.05f;
    return s;
}

// How much neighbouring probes differ: for each probe with one a cell over in x at the same height cell, the largest
// face difference (mean and 95th percentile).
static void neighbour_difference(const ProbeSet& set, float& mean, float& p95) {
    std::vector<float> d;
    for (size_t i = 0; i < set.count(); i++) {
        uint64_t next = set.keys[i] + (uint64_t(1) << 42);
        auto it = std::lower_bound(set.keys.begin(), set.keys.end(), next);
        if (it == set.keys.end() || *it != next) continue;
        size_t j = size_t(it - set.keys.begin());
        float m = 0;
        for (int k = 0; k < 18; k++) m = std::max(m, std::fabs(float(set.faces[i * 18 + k]) - float(set.faces[j * 18 + k])) / 128.0f);
        d.push_back(m);
    }
    mean = 0;
    for (float v : d) mean += v / std::max<size_t>(d.size(), 1);
    std::sort(d.begin(), d.end());
    p95 = d.empty() ? 0 : d[d.size() * 95 / 100];
}

int main(int argc, char** argv) {
    const char* rom_path = getenv("RUSH2_ROM");
    std::vector<uint8_t> rom = read_all(rom_path ? rom_path : "rush2.us.recomp.z64");
    if (rom.size() < 0x1100000) {
        printf("can't read the Rush 2 recomp ROM\n");
        return 1;
    }
    auto asset_rom = [&](int i) { return be32(rom, 0x01000000 + 0x800C185C - 0x800539E0 + i * 4); };
    auto asset = [&](int i, std::vector<uint8_t>& data) {
        uint32_t o = asset_rom(i);
        bool lz = i == 0x12 || i == 0x13 || (i >= 0x15 && i <= 0x18) || (i >= 0x1D && i <= 0x32);
        return lz ? rush2::track2049::rush2_lz_decompress(rom.data() + o, rom.size() - o, data)
                  : rush2::assets::inflate_raw(rom.data() + o, rom.size() - o, data);
    };
    const char* names[] = { "VEGAS", "NYONE", "HAWAII", "NYTWO", "ALCATRAZ", "LA", "SEATTLE", "HALFPIPE", "CRASH", "PIPE",
                            "ATARI", "STUNT1" };
    std::vector<std::string> tracks;
    std::string path_log;
    bool compare = false;
    bool dump = false;
    bool sky = false;
    int trace_car = -1;
    std::vector<float> above;
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--path") == 0 && a + 1 < argc) path_log = argv[++a];
        else if (strcmp(argv[a], "--compare") == 0) compare = true;
        else if (strcmp(argv[a], "--dump") == 0) dump = true;
        else if (strcmp(argv[a], "--sky") == 0) sky = true;
        else if (strcmp(argv[a], "--trace") == 0 && a + 1 < argc) trace_car = atoi(argv[++a]);
        else if (strcmp(argv[a], "--above") == 0 && a + 3 < argc) for (int k = 0; k < 3; k++) above.push_back((float)atof(argv[++a]));
        else tracks.push_back(argv[a]);
    }
    if (tracks.empty()) for (int i = 0; i < 12; i++) tracks.push_back(std::to_string(i));

    std::vector<Container> shared(2);
    for (int s = 0; s < 2; s++) {
        std::vector<uint8_t> data;
        std::string error;
        if (!asset(s == 0 ? 0x12 : 0x14, data) || !read_container(data, shared[s], error, true)) {
            printf("shared container %d: %s\n", s, error.c_str());
        }
    }
    std::shared_ptr<const rush2::rom2049::Source> source49;
    std::set<std::string> shared_models;

    for (const std::string& name : tracks) {
        std::vector<uint8_t> geometry, placement;
        rush2::probes::PaintSource paint;     // A 2049 track's painted lighting colours
        std::string label, error;
        if (name.rfind("k49:", 0) == 0) {
            int k = atoi(name.c_str() + 4);
            if (!source49) {
                const char* p49 = getenv("RUSH2049_ROM");
                source49 = rush2::rom2049::n64_source(std::make_shared<const std::vector<uint8_t>>(read_all(p49 ? p49 : "")));
                rush2::track2049::rush2_shared_model_names(rom, asset_rom(0x12), asset_rom(0x14), shared_models);
            }
            rush2::track2049::ConvertedTrack converted;
            if (!source49 || !rush2::track2049::convert_track(*source49, k, "HAWAII", shared_models, true, converted, error)) {
                printf("2049 track %d: can't convert (%s)\n", k, error.c_str());
                continue;
            }
            geometry = std::move(converted.geometry);
            placement = std::move(converted.placement);
            label = "2049 #" + std::to_string(k);
            rush2::track2049::car_light_paint(*source49, k, paint.tris, paint.colours);
            {
                std::vector<uint32_t> table;
                uint32_t air = 0;
                if (rush2::track2049::car_light_table(*source49->segment(rush2::rom2049::Segment::Main), k, table, air)) {
                    uint32_t base = 0;
                    std::vector<float> t;
                    std::vector<uint32_t> c;
                    rush2::track2049::car_light_paint(*source49, k, t, c, &base);
                    printf("2049 #%d colours: index 0 %08X, in the air %08X, floors' median %08X\n", k, table[0], air, base);
                }
            }
        }
        else {
            int slot = atoi(name.c_str());
            if (slot < 0 || slot > 11 || !asset(0x33 + slot, geometry) || !asset(0x3F + slot, placement)) {
                printf("track %s: can't read\n", name.c_str());
                continue;
            }
            label = names[slot];
        }
        Container track;
        if (!read_container(geometry, track, error, true)) {
            printf("%s: can't read (%s)\n", label.c_str(), error.c_str());
            continue;
        }
        std::vector<const Container*> containers = { &track, &shared[0], &shared[1] };
        std::vector<Placement> placements = read_placement(placement);

        if (sky) {
            // The sky models' average color (vertex color x texture average, alpha-weighted).
            for (const Container* c : containers) {
                for (const auto& m : c->models) {
                    if (m.name.find("SKY") == std::string::npos || m.lods.empty()) continue;
                    double sum[3] = {}, n = 0;
                    for (const auto& mesh : m.lods[0].meshes) {
                        double t[3] = { 1, 1, 1 };
                        if (mesh.texture >= 0 && mesh.texture < (int)c->textures.size()) {
                            const auto& px = c->textures[mesh.texture].image.rgba;
                            double s[3] = {}, w = 0;
                            for (size_t j = 0; j + 3 < px.size(); j += 4) {
                                for (int k = 0; k < 3; k++) s[k] += px[j + k] / 255.0 * px[j + 3];
                                w += px[j + 3];
                            }
                            if (w > 0) for (int k = 0; k < 3; k++) t[k] = s[k] / w;
                        }
                        for (const auto& v : mesh.vertices) {
                            for (int k = 0; k < 3; k++) sum[k] += (mesh.lit ? 1.0 : v.color[k] / 255.0) * t[k];
                            n++;
                        }
                    }
                    if (n > 0) printf("%s: %s average %.2f %.2f %.2f\n", label.c_str(), m.name.c_str(), sum[0] / n, sum[1] / n, sum[2] / n);
                }
            }
            continue;
        }
        if (!above.empty()) {
            // What rays from a car's height at each point hit (straight up and 45 degrees toward +-x, +-z), by model,
            // texture and how much of the texture is solid: what darkens a probe from above.
            std::map<std::string, size_t> models;
            for (const Container* c : containers) {
                for (size_t m = 0; m < c->models.size(); m++) models.emplace(c->models[m].name, m);
            }
            const float d = 0.7071f;
            const float dirs[6][3] = { { 0, 1, 0 }, { d, d, 0 }, { -d, d, 0 }, { 0, d, d }, { 0, d, -d }, { 0, -1, 0 } };
            for (size_t q = 0; q + 2 < above.size(); q += 3) {
                float o[3] = { above[q], above[q + 1] + 2.5f, above[q + 2] };
                printf("at %.0f %.0f %.0f\n", above[q], above[q + 1], above[q + 2]);
                for (const auto& dir : dirs) {
                    float best = 400;
                    std::string what = "sky";
                    for (const Placement& p : placements) {
                        const Model* model = nullptr;
                        const Container* owner = nullptr;
                        for (const Container* c : containers) {
                            for (const Model& m : c->models) {
                                if (m.name == p.name || (!model && m.name.rfind(p.name, 0) == 0 && !p.name.empty())) {
                                    model = &m;
                                    owner = c;
                                }
                            }
                            if (model) break;
                        }
                        if (!model || model->lods.empty()) continue;
                        for (const auto& mesh : model->lods[0].meshes) {
                            for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
                                float w[3][3];
                                for (int v = 0; v < 3; v++) {
                                    const auto& vx = mesh.vertices[i + v];
                                    for (int k = 0; k < 3; k++) {
                                        w[v][k] = (vx.position[0] * p.matrix[k] + vx.position[1] * p.matrix[3 + k] +
                                                   vx.position[2] * p.matrix[6 + k]) / 16.0f + p.position[k];
                                    }
                                }
                                // Moller-Trumbore, both sides.
                                float e1[3], e2[3], s[3], h[3], qv[3];
                                for (int k = 0; k < 3; k++) { e1[k] = w[1][k] - w[0][k]; e2[k] = w[2][k] - w[0][k]; s[k] = o[k] - w[0][k]; }
                                h[0] = dir[1] * e2[2] - dir[2] * e2[1]; h[1] = dir[2] * e2[0] - dir[0] * e2[2]; h[2] = dir[0] * e2[1] - dir[1] * e2[0];
                                float a = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
                                if (std::fabs(a) < 1e-8f) continue;
                                float u = (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]) / a;
                                if (u < 0 || u > 1) continue;
                                qv[0] = s[1] * e1[2] - s[2] * e1[1]; qv[1] = s[2] * e1[0] - s[0] * e1[2]; qv[2] = s[0] * e1[1] - s[1] * e1[0];
                                float v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) / a;
                                if (v < 0 || u + v > 1) continue;
                                float t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / a;
                                if (t <= 0.01f || t >= best) continue;
                                best = t;
                                float solid = 1;
                                std::string tex = "-";
                                if (mesh.texture >= 0 && mesh.texture < (int)owner->textures.size()) {
                                    const auto& tx = owner->textures[mesh.texture];
                                    tex = tx.name;
                                    const auto& px = tx.image.rgba;
                                    double sum = 0;
                                    for (size_t j = 3; j < px.size(); j += 4) sum += px[j] / 255.0;
                                    if (!px.empty()) solid = float(sum / (px.size() / 4));
                                }
                                char line[256];
                                // Clockwise front faces: the front normal is e2 x e1.
                                float n[3] = { e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0] };
                                float det = p.matrix[0] * (p.matrix[4] * p.matrix[8] - p.matrix[5] * p.matrix[7]) -
                                            p.matrix[1] * (p.matrix[3] * p.matrix[8] - p.matrix[5] * p.matrix[6]) +
                                            p.matrix[2] * (p.matrix[3] * p.matrix[7] - p.matrix[4] * p.matrix[6]);
                                bool front = (n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2]) * (det < 0 ? -1 : 1) < 0;
                                int vc[3] = {};
                                for (int v = 0; v < 3; v++) for (int k = 0; k < 3; k++) vc[k] += mesh.vertices[i + v].color[k];
                                // The texture's average color (what the bake multiplies the vertex color by).
                                double avg[3] = {}, wsum = 0;
                                if (mesh.texture >= 0 && mesh.texture < (int)owner->textures.size()) {
                                    const auto& px = owner->textures[mesh.texture].image.rgba;
                                    for (size_t j = 0; j + 3 < px.size(); j += 4) {
                                        for (int k = 0; k < 3; k++) avg[k] += px[j + k] * px[j + 3];
                                        wsum += px[j + 3];
                                    }
                                }
                                if (wsum > 0) for (double& a : avg) a /= wsum;
                                snprintf(line, sizeof(line), "%s (%s, %.0f%% solid, %s face, color %d %d %d, texture %.0f %.0f %.0f, cc %014llX%s)", p.name.c_str(),
                                         tex.c_str(), solid * 100, front ? "front" : "back", vc[0] / 3, vc[1] / 3, vc[2] / 3, avg[0], avg[1], avg[2],
                                         (unsigned long long)mesh.combine, mesh.lit ? ", lit" : "");
                                what = line;
                            }
                        }
                    }
                    printf("  dir %+.1f %+.1f %+.1f: %5.0f ft  %s\n", dir[0], dir[1], dir[2], best, what.c_str());
                }
            }
            continue;
        }
        if (dump && sky && !paint.colours.empty()) {
            // A 2049 track: the painted brightness under the probes open to the sky (up face 0.95 or more; unpainted
            // counts as white), as percentiles.
            auto set = rush2::probes::bake(containers, placements, nullptr, {}, &paint);
            std::vector<float> lum;
            for (size_t i = 0; i < set->count(); i++) {
                const uint8_t* up = &set->faces[i * 18 + 6];
                if ((0.299f * up[0] + 0.587f * up[1] + 0.114f * up[2]) / 128.0f < 0.95f) continue;
                const uint8_t* c = &set->paint[i * 4];
                lum.push_back(c[3] ? (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) / 255.0f : 1.0f);
            }
            std::sort(lum.begin(), lum.end());
            auto pct = [&](float p) { return lum.empty() ? 0.0f : lum[std::min(lum.size() - 1, size_t(p * lum.size()))]; };
            printf("%s: %zu open probes, painted brightness p10 %.2f p25 %.2f p50 %.2f p75 %.2f p90 %.2f\n", label.c_str(),
                   lum.size(), pct(0.1f), pct(0.25f), pct(0.5f), pct(0.75f), pct(0.9f));
            continue;
        }
        if (dump) {
            // Every probe: position, then each face's color (+x -x +y -y +z -z), for awk.
            auto set = rush2::probes::bake(containers, placements, nullptr, {}, paint.colours.empty() ? nullptr : &paint);
            for (size_t i = 0; i < set->count(); i++) {
                float p[3];
                set->position(i, p);
                printf("%.0f %.0f %.0f", p[0], p[1], p[2]);
                for (int k = 0; k < 18; k++) printf(" %.2f", set->faces[i * 18 + k] / 128.0f);
                if (!set->paint.empty()) printf(" paint %d %.2f", set->paint[i * 4 + 3] ? 1 : 0, set->paint[i * 4] / 255.0f);
                printf("\n");
            }
            continue;
        }
        if (compare) {
            Paths paths;
            if (!path_log.empty()) paths = load_paths(path_log);
            printf("%s, %zu recorded cars\n", label.c_str(), paths.size());
            printf("spacing rays rotate smooth   probes    KB  bake s   nbr mean  nbr p95   body p95 color flicker/min  holes\n");
            struct Variant {
                float spacing;
                int rays;
                bool rotate;
                float smooth;
            };
            const Variant variants[] = {
                { 10, 128, false, 0 }, { 5, 128, true, 12 }, { 5, 256, true, 12 },
            };
            for (const Variant& v : variants) {
                rush2::probes::BakeParams bp;
                bp.spacing = v.spacing;
                bp.rays = v.rays;
                bp.rotate = v.rotate;
                bp.smooth = v.smooth;
                rush2::probes::BakeStats stats;
                auto set = rush2::probes::bake(containers, placements, &stats, bp, paint.colours.empty() ? nullptr : &paint);
                float nmean, n95;
                neighbour_difference(*set, nmean, n95);
                Scores sc;
                if (!paths.empty()) sc = replay(*set, paths, CarLight::Params{});
                printf("%5.0f  %4d  %-6s %5.1f  %7zu %5zu  %6.2f   %8.3f %8.3f   %8.3f %14.1f  %5.2f%%\n", v.spacing, v.rays,
                       v.rotate ? "yes" : "no", v.smooth, set->count(), set->serialize().size() / 1024, stats.seconds,
                       nmean, n95, sc.body95, sc.body_flicker, sc.holes);
                fflush(stdout);
            }
            continue;
        }

        rush2::probes::BakeStats stats;
        auto set = rush2::probes::bake(containers, placements, &stats, {}, paint.colours.empty() ? nullptr : &paint);
        if (!path_log.empty()) {
            Paths paths = load_paths(path_log);
            if (trace_car == 99) {
                // Where cars find no probe in reach: the nearest probe in x/z within 20 ft and how far it is under the
                // car (the car in the air), or none (no probes there: a gap in the bake).
                std::map<int, int> below;
                int gaps = 0, total = 0, paint_misses = 0;
                for (const auto& [car, path] : paths) {
                    for (const Point& pt : path) {
                        Cube c;
                        total++;
                        rush2::probes::Painted near{ { 1, 1, 1 }, -1 };
                        if (set->sample(pt.p, c, 20.0f, &near, CarLight::Params{}.paint_radius)) {
                            // Light found but no paint probe in reach: the car falls back to the wheels' color.
                            if (!set->paint.empty() && near.amount < 0) paint_misses++;
                            continue;
                        }
                        float best = 1e9f, dy = 0;
                        for (size_t i = 0; i < set->count(); i++) {
                            float p[3];
                            set->position(i, p);
                            float dx = p[0] - pt.p[0], dz = p[2] - pt.p[2];
                            float d = dx * dx + dz * dz;
                            if (d < 400 && std::fabs(p[1] - pt.p[1]) < std::fabs(dy) + (best > 1e8f ? 1e9f : 0)) {
                                best = d;
                                dy = p[1] - pt.p[1];
                            }
                        }
                        if (best > 1e8f) {
                            if (gaps++ < 8) printf("gap at %.0f %.0f %.0f (car %d, %.2f s)\n", pt.p[0], pt.p[1], pt.p[2], car, pt.t);
                        }
                        else {
                            below[std::min(50, int(-dy / 5) * 5)]++;
                        }
                    }
                }
                printf("%d records, %d with no probe in reach: %d gaps\n", total, gaps + [&] { int n = 0; for (auto& b : below) n += b.second; return n; }(), gaps);
                for (auto& [d, n] : below) printf("  nearest probe %d+ ft under the car: %d\n", d, n);
                printf("  light found but no paint probe in reach (wheel color used): %d\n", paint_misses);
                continue;
            }
            if (trace_car >= 0) {
                // One car's light from above along its path: in its own cell, over the 20 ft sample, and as shown.
                CarLight light;
                CarLight::Params params;
                const auto& path = paths[trace_car];
                auto top = [](const Cube& c) { return 0.299f * c.c[2][0] + 0.587f * c.c[2][1] + 0.114f * c.c[2][2]; };
                printf("t x y z cell sample shown / paint amount (rgb) shown with paint\n");
                for (size_t i = 0; i < path.size(); i++) {
                    float dt = i ? path[i].t - path[i - 1].t : 0.0f;
                    CarLight::Painted under{ { path[i].rgb[0], path[i].rgb[1], path[i].rgb[2] }, path[i].paint };
                    light.step(*set, path[i].p, under.amount > 0 ? &under : nullptr, dt, params);
                    Cube cell = CarLight::target(*set, path[i].p, set->spacing);
                    printf("%.2f %.0f %.0f %.0f %.2f %.2f %.2f\n", path[i].t, path[i].p[0], path[i].p[1], path[i].p[2], top(cell),
                           top(CarLight::target(*set, path[i].p, params.radius)), top(light.lit(nullptr, params)));
                    Cube shown = light.lit(light.painted.amount > 0 ? &light.painted : nullptr, params);
                    float mean[3] = {};
                    for (const auto& f : shown.c) for (int k = 0; k < 3; k++) mean[k] += f[k] / 6.0f;
                    printf("    probe paint %.2f (%.2f %.2f %.2f) ", light.painted.amount, light.painted.rgb[0], light.painted.rgb[1],
                           light.painted.rgb[2]);
                    float body[3], body_probes[3];
                    upper_body(shown, body);
                    upper_body(light.lit(nullptr, params), body_probes);
                    printf("wheel paint %.2f (%.2f %.2f %.2f) shown %.2f mean %.2f %.2f %.2f body %.3f %.3f %.3f probes %.3f %.3f %.3f\n",
                           under.amount, under.rgb[0], under.rgb[1], under.rgb[2], top(shown), mean[0], mean[1], mean[2],
                           body[0], body[1], body[2], body_probes[0], body_probes[1], body_probes[2]);
                }
                continue;
            }
            printf("%s, %zu recorded cars\n", label.c_str(), paths.size());
            printf("radius  ease  ahead\n");
            auto run = [&](const CarLight::Params& p, const char* tag) {
                Scores sc = replay(*set, paths, p);
                printf("%5.0f  %5.2f  %5.2f   body p95 %6.3f  color flicker/min %6.1f  lag %+5.2f s  %s\n", p.radius,
                       p.ease_time, p.lookahead, sc.body95, sc.body_flicker, sc.lag, tag);
            };
            run(CarLight::Params{}, "(default)");
            // The faces as shown, from the probes alone and with the logged painted colour on them (eased over
            // paint_ease seconds; 0: at once, as Rush 2049 does).
            for (float ease : { -1.0f, 0.0f, 0.03f, 0.05f, 0.08f, 0.12f }) {
                CarLight::Params p;
                p.paint_ease = std::max(ease, 0.0f);
                Scores d = replay(*set, paths, p, ease >= 0);
                char tag[32];
                snprintf(tag, sizeof(tag), ease < 0 ? "probes only" : "paint %.2f s", ease);
                printf("  %-12s face step p95: +x %.3f -x %.3f up %.3f down %.3f +z %.3f -z %.3f  top flicker/min %.1f  body step p95 %.3f color flicker/min %.1f"
                       "  paint behind %.3f\n", tag, d.face95[0], d.face95[1], d.face95[2], d.face95[3], d.face95[4],
                       d.face95[5], d.top_flicker, d.body95, d.body_flicker, d.paint_behind);
            }
            for (float radius : { 20.0f, 30.0f, 40.0f }) {
                for (float ease : { 0.08f, 0.12f, 0.2f }) {
                    CarLight::Params p;
                    p.radius = radius;
                    p.ease_time = ease;
                    p.lookahead = ease * 0.75f;
                    run(p, "");
                }
            }
            continue;
        }
        auto blob = set->serialize();
        // Light per probe from above (+y) and the sides' average, as luminance.
        std::vector<float> up, side, down;
        for (size_t i = 0; i < set->count(); i++) {
            auto lum = [&](int f) {
                const uint8_t* c = &set->faces[i * 18 + f * 3];
                return (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) / 128.0f;
            };
            up.push_back(lum(2));
            down.push_back(lum(3));
            side.push_back((lum(0) + lum(1) + lum(4) + lum(5)) / 4.0f);
        }
        auto pct = [](std::vector<float> v, float p) {
            if (v.empty()) return 0.0f;
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, size_t(p * v.size()))];
        };
        size_t covered = std::count_if(up.begin(), up.end(), [](float v) { return v < 0.7f; });
        printf("%-8s: %6zu tris %6zu probes (%5zu open, %4.1f%% covered) %5.2fs %5zu KB  ref %.2f %.2f up %.2f down %.2f %.2f %.2f\n",
               label.c_str(), stats.triangles, stats.probes, stats.open, 100.0 * covered / std::max<size_t>(1, up.size()),
               stats.seconds, blob.size() / 1024, stats.reference[0], stats.reference[1], stats.reference[2],
               stats.reference[3], stats.reference[4], stats.reference[5]);
        printf("          up p5 %.2f p25 %.2f p50 %.2f   side p5 %.2f p25 %.2f p50 %.2f   down p5 %.2f p50 %.2f\n",
               pct(up, 0.05f), pct(up, 0.25f), pct(up, 0.5f), pct(side, 0.05f), pct(side, 0.25f), pct(side, 0.5f),
               pct(down, 0.05f), pct(down, 0.5f));
        // The covered probes alone: what tunnels and underpasses do to a car.
        std::vector<float> cu, cs;
        for (size_t i = 0; i < up.size(); i++) {
            if (up[i] < 0.7f) { cu.push_back(up[i]); cs.push_back(side[i]); }
        }
        if (!cu.empty()) {
            printf("          covered: up p25 %.2f p50 %.2f p75 %.2f   side p50 %.2f\n", pct(cu, 0.25f), pct(cu, 0.5f),
                   pct(cu, 0.75f), pct(cs, 0.5f));
        }
    }
    return 0;
}
