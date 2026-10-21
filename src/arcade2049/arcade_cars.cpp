// Converts the Rush 2049 arcade (SE) cars into a car mod (docs/car_mods.md#new-cars, #liveries; the arcade's formats:
// docs/arcade2049.md#cars). The Truck, Deuce and Caddy become new cars, every color scheme (CARS/CARn/COLn: whole
// liveries with their own texture sets and parts) a STYLE; five arcade schemes become liveries of Rush 2049's N64
// cars. Per scheme: the body's full-detail LOD (CARBODY LOD 0) in the model dump's space (1/16 ft, x negated), one
// material per arcade texture, drawn as authored (no paint mask: each scheme keeps the arcade's colors), and its
// hood camera hood (HOOD, as Rush 2049's N64 cars draw theirs). The brake lights (BRAKE) and the wheels (Rush 2's
// are drawn) are left out.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "arcade2049.h"
#include "texture_upscale.h"
#include "arcade_internal.h"

namespace a49 = rush2::arcade2049;
using namespace a49;

namespace {
    struct NewCar {
        const char* folder;
        const char* id;
        const char* name;
        const char* base;    // the Rush 2 car it drives as (physics, engine sound, options)
    };
    // Each drives as the Rush 2 car of its kind. The arcade can't tell them apart: it gives every car one physics
    // descriptor (docs/arcade2049.md#car-physics), so its values made the Deuce and the Caddy drive alike, and harder
    // than any stock car.
    const NewCar new_cars[] = {
        { "CAR8", "TRUCK", "TRUCK", "PICKUP" },
        { "CAR10", "DEUCE", "DEUCE", "HOTROD" },
        { "CAR11", "CADDY", "CADDY", "CADDY" },
    };

    // Arcade color schemes as liveries of Rush 2049 (N64) cars: the arcade folder, its COL folders, and how far
    // forward (ft) the N64 car's wheels sit from the arcade's (the center of its descriptor's wheel points: the
    // Bruiser's 0x80110984, 4.425 / -3.425; the others' 0x801108D0, the arcade's own). The arcade body moves by that,
    // so it sits on the N64 car's wheels (a side and top silhouette fit agrees).
    struct Livery {
        const char* car;
        const char* folder;
        std::vector<int> cols;
        double dz;
    };
    const Livery liveries[] = {
        { "GX-2", "CAR2", { 6 }, 0.0 },
        { "MAGNUM", "CAR4", { 4, 6, 7 }, 0.0 },
        { "SUPER GT", "CAR5", { 5, 7 }, 0.0 },
        { "8-BALL", "CAR6", { 1, 3 }, 0.0 },
        { "BRUISER", "CAR7", { 1, 2, 3, 5, 6, 7, 8 }, 0.5 },
    };

    // The arcade's wheels, the same for every car (docs/arcade2049.md#cars: its one car descriptor's wheel block,
    // copied into each race car, and its WBLUR wheels placed there): right wheels at x 2.75, front z 3.925 / rear z
    // -3.925, 0.8 / 0.75 ft below the body's origin (Rush 2's descriptor: y -0.8 / -0.75, the drawn center 1 ft
    // higher, so 0.2 / 0.25 above). Every arcade wheel is 1.01 ft round (WHEEL), so the wheel model (15/16 ft at
    // scale 1) is drawn at 1.08.
    constexpr double front[2] = { 2.75, 3.925 }, rear[2] = { 2.75, -3.925 };
    constexpr double wheel_y[2] = { 0.2, 0.25 };
    constexpr double wheel_scale = 1.08;
    constexpr double units = 16.0;     // vertex units per foot (the model dump's space)

    // The arcade's collision boxes (project-r.exe VA 0x140664130, 12 x {front, rear, half width, height}, by car
    // index = CARn - 1; docs/arcade2049.md#car-physics). Height: the N64 port of this code gives every car 3.5 ft; the
    // arcade's table has 2 for all (and its drone box, VA 0x1406EF824), which isn't checked against Rush 2's box use,
    // so the N64's is kept.
    constexpr double arcade_box[12][3] = { { 5.75, 6.5, 3 }, { 6.29, 6.58, 2.9 }, { 6.05, 6.65, 2.9 }, { 6.25, 5.75, 3 },
                                           { 6.44, 6.64, 3 }, { 6.25, 6.75, 3 }, { 5.75, 6, 3 }, { 6.7, 6.97, 2.8 },
                                           { 6.25, 6, 3 }, { 6.75, 7, 3 }, { 6.5, 6.5, 3 }, { 6.25, 5.5, 3 } };

    // Per corner of the triangles: the face normals of the triangles sharing the corner's point (to 1/1000) within
    // `crease` degrees of this one, averaged.
    std::vector<V3> smooth_normals(const std::vector<V3>& P, double crease = 60.0) {
        size_t m = P.size() / 3;
        std::vector<V3> fn(m);
        for (size_t f = 0; f < m; f++) {
            V3 n = cross(P[3 * f + 1] - P[3 * f], P[3 * f + 2] - P[3 * f]);
            fn[f] = n * (1.0 / std::max(norm(n), 1e-9));
        }
        auto key = [](const V3& p) {
            return std::array<int64_t, 3>{ std::llround(p[0] * 1000), std::llround(p[1] * 1000), std::llround(p[2] * 1000) };
        };
        std::map<std::array<int64_t, 3>, std::vector<size_t>> keys;
        for (size_t f = 0; f < m; f++) for (int k = 0; k < 3; k++) keys[key(P[3 * f + k])].push_back(f);
        double c = std::cos(crease * 3.14159265358979323846 / 180.0);
        std::vector<V3> out(P.size());
        for (size_t f = 0; f < m; f++) {
            for (int k = 0; k < 3; k++) {
                V3 n{};
                for (size_t g : keys[key(P[3 * f + k])]) {
                    if (dot(fn[g], fn[f]) >= c) n = n + fn[g];
                }
                out[3 * f + k] = n * (1.0 / std::max(norm(n), 1e-9));
            }
        }
        return out;
    }

    // The car's color schemes: its COL folders, in number order.
    std::vector<std::string> color_schemes(const std::filesystem::path& dir) {
        std::vector<std::pair<int, std::string>> found;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            std::string n = rush2::upscale::path_utf8(e.path().filename());
            if (n.size() > 3 && (n[0] == 'C' || n[0] == 'c') && (n[1] == 'O' || n[1] == 'o') && (n[2] == 'L' || n[2] == 'l')) {
                found.push_back({ std::atoi(n.c_str() + 3), n });
            }
        }
        std::sort(found.begin(), found.end());
        std::vector<std::string> out;
        for (auto& f : found) out.push_back(f.second);
        return out;
    }

    // Color scheme `col` of the car (object `obj`: CARBODY, or HOOD, the hood camera's hood) as a glb in the model
    // dump's space, moved dz ft forward; empty without that object.
    std::vector<uint8_t> convert(const std::filesystem::path& data, const std::string& folder, const std::string& col,
                                 double dz, const std::string& obj, size_t& tri_count, std::string& error) {
        tri_count = 0;
        Track t;
        if (!t.open(data / "CARS" / folder / col, error)) return {};
        const Object* o = t.find(obj);
        if (!o) return {};
        const Lod& lod = o->lod0;
        std::set<int> used;
        for (const auto& tr : lod.tris) if (tr[3] < t.textures.size()) used.insert(tr[3]);
        Glb g("rush2-recomp src/arcade2049 (tools/car_mod.py)", false);
        std::vector<Glb::Prim> prims;
        for (int ti : used) {
            const Texture& tex = t.textures[ti];
            std::vector<uint8_t> rgba;
            if (!tex.rgba(t.texels(), rgba)) continue;
            V2 s = tex.uv_scale();
            Glb::Prim p;
            std::vector<V3> G;
            for (const auto& tr : lod.tris) {
                if (tr[3] != ti) continue;
                for (int k : { tr[0], tr[2], tr[1] }) {   // mirroring x flips the winding
                    if (k >= int(lod.verts.size())) continue;
                    const Vertex& v = lod.verts[k];
                    G.push_back({ -v.pos[0] * units, v.pos[1] * units, (v.pos[2] + dz) * units });
                    p.uvs.push_back({ v.st[0] / s[0], v.st[1] / s[1] });
                }
            }
            if (G.empty() || G.size() % 3) continue;
            p.verts = G;
            p.normals = smooth_normals(G);
            for (uint32_t k = 0; k < G.size(); k += 3) p.tris.push_back({ k, k + 1, k + 2 });
            nlohmann::json m = { { "name", tex.name },
                                 { "pbrMetallicRoughness", { { "metallicFactor", 0.0 }, { "roughnessFactor", 1.0 } } } };
            int image = g.image(tex.name, png(rgba, tex.w, tex.h));
            if (!g.js.contains("samplers")) {
                g.js["samplers"] = { { { "magFilter", 9729 }, { "minFilter", 9987 }, { "wrapS", 10497 }, { "wrapT", 10497 } } };
                g.js["textures"] = nlohmann::json::array();
            }
            g.js["textures"].push_back({ { "sampler", 0 }, { "source", image } });
            m["pbrMetallicRoughness"]["baseColorTexture"] = { { "index", g.js["textures"].size() - 1 } };
            g.js["materials"].push_back(m);
            p.material = int(g.js["materials"].size()) - 1;
            tri_count += p.tris.size();
            prims.push_back(std::move(p));
        }
        g.textured_mesh(folder + "_" + col + "_" + obj, prims);
        return g.bytes();
    }
}

bool a49::build_car_mod(const std::filesystem::path& data_dir, ModFiles& out, std::string& log,
                        const std::function<bool()>& cancelled) {
    std::string error;
    char line[256];
    for (const NewCar& car : new_cars) {
        std::vector<std::string> schemes = color_schemes(data_dir / "CARS" / car.folder);
        if (schemes.empty()) {
            log += std::string("no color schemes in CARS/") + car.folder + "\n";
            return false;
        }
        nlohmann::json files = nlohmann::json::array(), hoods = nlohmann::json::array();
        for (const std::string& col : schemes) {
            if (cancelled && cancelled()) return false;
            size_t tris = 0;
            std::vector<uint8_t> glb = convert(data_dir, car.folder, col, 0.0, "CARBODY", tris, error);
            if (glb.empty()) {
                log += std::string(car.folder) + "/" + col + ": " + (error.empty() ? "no CARBODY" : error) + "\n";
                return false;
            }
            std::string name = std::string(car.id) + "_" + col + ".glb";
            out["rush2/new_cars/" + name] = std::move(glb);
            files.push_back(name);
            snprintf(line, sizeof(line), "%s/%s -> %s: %zu triangles\n", car.folder, col.c_str(), name.c_str(), tris);
            log += line;
            std::vector<uint8_t> hood = convert(data_dir, car.folder, col, 0.0, "HOOD", tris, error);
            std::string hood_name = hood.empty() ? "" : std::string(car.id) + "_" + col + "_HOOD.glb";
            if (!hood.empty()) out["rush2/new_cars/" + hood_name] = std::move(hood);
            hoods.push_back(hood_name);
        }
        int index = std::atoi(car.folder + 3) - 1;
        nlohmann::json spec = {
            { "name", car.name }, { "base", car.base }, { "schemes", files }, { "hoods", hoods },
            { "box", { arcade_box[index][0], arcade_box[index][1], arcade_box[index][2], 3.5 } },
            { "wheels", { { "front", { front[0] * units, wheel_y[0] * units, front[1] * units } },
                          { "rear", { rear[0] * units, wheel_y[1] * units, rear[1] * units } } } },
            { "wheel_scale", { wheel_scale, wheel_scale } },
            { "source", std::string("San Francisco Rush 2049 SE (arcade) ") + car.folder + ", built by the Games tab" } };
        out["rush2/new_cars/" + std::string(car.id) + ".json"] = text_bytes(spec.dump(4));
    }
    for (const Livery& l : liveries) {
        std::string lid;
        for (const char* c = l.car; *c; c++) if (*c != ' ' && *c != '-') lid += *c;
        nlohmann::json files = nlohmann::json::array(), hoods = nlohmann::json::array();
        for (int c : l.cols) {
            if (cancelled && cancelled()) return false;
            std::string col = "COL" + std::to_string(c);
            size_t tris = 0;
            std::vector<uint8_t> glb = convert(data_dir, l.folder, col, l.dz, "CARBODY", tris, error);
            if (glb.empty()) {
                log += std::string(l.folder) + "/" + col + ": " + (error.empty() ? "no CARBODY" : error) + "\n";
                return false;
            }
            std::string name = lid + "_" + col + ".glb";
            out["rush2/liveries/" + name] = std::move(glb);
            files.push_back(name);
            snprintf(line, sizeof(line), "%s/%s -> livery %s: %zu triangles\n", l.folder, col.c_str(), name.c_str(), tris);
            log += line;
            std::vector<uint8_t> hood = convert(data_dir, l.folder, col, l.dz, "HOOD", tris, error);
            std::string hood_name = hood.empty() ? "" : lid + "_" + col + "_HOOD.glb";
            if (!hood.empty()) out["rush2/liveries/" + hood_name] = std::move(hood);
            hoods.push_back(hood_name);
        }
        out["rush2/liveries/" + lid + ".json"] = text_bytes(nlohmann::json{
            { "car", l.car }, { "styles", files }, { "hoods", hoods },
            { "source", std::string("San Francisco Rush 2049 SE (arcade) ") + l.folder + ", built by the Games tab" } }.dump(4));
    }
    out["mod.json"] = text_bytes(nlohmann::json{
        { "game_id", "rush2" }, { "id", car_mod_id }, { "display_name", "Rush 2049 Arcade Cars" },
        { "description", "San Francisco Rush 2049 Special Edition (arcade): the Truck, Deuce and Caddy as new cars, and "
                         "arcade liveries for the GX-2, Magnum, Super GT, 8-Ball and Bruiser (STYLE row), built from the "
                         "arcade data by the Games tab." },
        { "short_description", "Rush 2049 arcade cars" }, { "version", "1.0.0" }, { "authors", { "rush2-recomp" } },
        { "minimum_recomp_version", "0.0.0" } }.dump(4));
    return true;
}
