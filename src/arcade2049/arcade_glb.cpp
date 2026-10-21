// A minimal glTF 2.0 binary writer for the arcade converters (src/arcade2049/arcade_internal.h): a port of
// tools/stock_track_glb.py's Glb, which the Python converters write their mods with.

#include <algorithm>
#include <cstring>

#include "arcade_internal.h"

namespace a49 = rush2::arcade2049;
using nlohmann::json;

a49::Glb::Glb(const std::string& generator, bool default_material) {
    js = { { "asset", { { "version", "2.0" }, { "generator", generator } } }, { "scene", 0 },
           { "scenes", json::array({ { { "nodes", json::array() } } }) }, { "nodes", json::array() },
           { "meshes", json::array() }, { "accessors", json::array() }, { "bufferViews", json::array() },
           { "buffers", json::array() }, { "materials", json::array() } };
    if (default_material) {
        // Material 0: the plain one mesh() gives collision, AI paths and Empties' meshes.
        js["materials"].push_back({ { "name", "stock" }, { "pbrMetallicRoughness",
                                    { { "baseColorFactor", { 0.5, 0.5, 0.5, 1.0 } }, { "metallicFactor", 0.0 } } } });
    }
}

int a49::Glb::view(const void* data, size_t size, int target) {
    while (bin.size() % 4) bin.push_back(0);
    json v = { { "buffer", 0 }, { "byteOffset", bin.size() }, { "byteLength", size } };
    if (target) v["target"] = target;
    js["bufferViews"].push_back(v);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bin.insert(bin.end(), p, p + size);
    return int(js["bufferViews"].size()) - 1;
}

int a49::Glb::accessor_f(const std::vector<float>& values, const char* kind, int width, int target, bool minmax) {
    json a = { { "bufferView", view(values.data(), values.size() * 4, target) }, { "componentType", 5126 },
               { "count", values.size() / width }, { "type", kind } };
    if (minmax && !values.empty()) {
        json lo = json::array(), hi = json::array();
        for (int k = 0; k < width; k++) {
            float mn = values[k], mx = values[k];
            for (size_t i = k; i < values.size(); i += width) mn = std::min(mn, values[i]), mx = std::max(mx, values[i]);
            lo.push_back(mn);
            hi.push_back(mx);
        }
        a["min"] = lo;
        a["max"] = hi;
    }
    js["accessors"].push_back(a);
    return int(js["accessors"].size()) - 1;
}

int a49::Glb::accessor_u(const std::vector<uint32_t>& values, int target) {
    json a = { { "bufferView", view(values.data(), values.size() * 4, target) }, { "componentType", 5125 },
               { "count", values.size() }, { "type", "SCALAR" } };
    js["accessors"].push_back(a);
    return int(js["accessors"].size()) - 1;
}

int a49::Glb::node(json n, const std::optional<Trs>& trs) {
    if (trs) {
        n["translation"] = trs->translation;
        n["rotation"] = trs->rotation;
        n["scale"] = trs->scale;
    }
    js["nodes"].push_back(n);
    int i = int(js["nodes"].size()) - 1;
    js["scenes"][0]["nodes"].push_back(i);
    return i;
}

int a49::Glb::image(const std::string& name, const std::vector<uint8_t>& png_bytes) {
    int v = view(png_bytes.data(), png_bytes.size(), 0);
    if (!js.contains("images")) js["images"] = json::array();
    js["images"].push_back({ { "name", name }, { "mimeType", "image/png" }, { "bufferView", v } });
    return int(js["images"].size()) - 1;
}

int a49::Glb::material(const std::string& name, std::optional<int> image, const std::string& alpha_mode,
                       bool double_sided, std::array<int, 2> wrap) {
    json m = { { "name", name },
               { "pbrMetallicRoughness", { { "baseColorFactor", { 1.0, 1.0, 1.0, 1.0 } }, { "metallicFactor", 0.0 } } } };
    if (image) {
        if (!js.contains("samplers")) js["samplers"] = json::array();
        if (!js.contains("textures")) js["textures"] = json::array();
        js["samplers"].push_back({ { "wrapS", wrap[0] }, { "wrapT", wrap[1] } });
        js["textures"].push_back({ { "source", *image }, { "sampler", js["samplers"].size() - 1 } });
        m["pbrMetallicRoughness"]["baseColorTexture"] = { { "index", js["textures"].size() - 1 } };
    }
    if (alpha_mode != "OPAQUE") m["alphaMode"] = alpha_mode;
    if (double_sided) m["doubleSided"] = true;
    js["materials"].push_back(m);
    return int(js["materials"].size()) - 1;
}

namespace {
    std::vector<float> flat3(const std::vector<a49::V3>& v) {
        std::vector<float> out;
        out.reserve(v.size() * 3);
        for (const auto& p : v) for (double c : p) out.push_back(float(c));
        return out;
    }
    std::vector<uint32_t> flat_tris(const std::vector<std::array<uint32_t, 3>>& tris) {
        std::vector<uint32_t> out;
        out.reserve(tris.size() * 3);
        for (const auto& t : tris) out.insert(out.end(), t.begin(), t.end());
        return out;
    }
}

int a49::Glb::textured_mesh(const std::string& name, const std::vector<Prim>& prims, const std::optional<Trs>& trs) {
    json out = json::array();
    for (const Prim& p : prims) {
        std::vector<float> uv;
        for (const auto& t : p.uvs) uv.push_back(float(t[0])), uv.push_back(float(t[1]));
        json at = { { "POSITION", accessor_f(flat3(p.verts), "VEC3", 3, 34962, true) },
                    { "NORMAL", accessor_f(flat3(p.normals), "VEC3", 3, 34962, false) },
                    { "TEXCOORD_0", accessor_f(uv, "VEC2", 2, 34962, false) } };
        if (!p.colors.empty()) {
            std::vector<float> col;
            for (const auto& c : p.colors) for (double x : c) col.push_back(float(x));
            at["COLOR_0"] = accessor_f(col, "VEC4", 4, 34962, false);
        }
        out.push_back({ { "attributes", at }, { "material", p.material }, { "indices", accessor_u(flat_tris(p.tris), 34963) } });
    }
    js["meshes"].push_back({ { "name", name }, { "primitives", out } });
    return node({ { "name", name }, { "mesh", js["meshes"].size() - 1 } }, trs);
}

int a49::Glb::mesh(const std::string& name, const std::vector<V3>& verts, const std::vector<V3>& normals,
                   const std::vector<std::array<uint32_t, 3>>& tris,
                   const std::vector<std::pair<std::string, std::vector<double>>>& attrs) {
    json prim = { { "attributes", { { "POSITION", accessor_f(flat3(verts), "VEC3", 3, 34962, true) },
                                    { "NORMAL", accessor_f(flat3(normals), "VEC3", 3, 34962, false) },
                                    { "TEXCOORD_0", accessor_f(std::vector<float>(verts.size() * 2, 0.0f), "VEC2", 2,
                                                               34962, false) } } },
                  { "indices", accessor_u(flat_tris(tris), 34963) }, { "material", 0 } };
    for (const auto& [key, values] : attrs) {
        std::vector<float> f(values.begin(), values.end());
        prim["attributes"][key] = accessor_f(f, "SCALAR", 1, 34962, false);
    }
    js["meshes"].push_back({ { "name", name }, { "primitives", json::array({ prim }) } });
    return node({ { "name", name }, { "mesh", js["meshes"].size() - 1 } }, {});
}

int a49::Glb::empty(const std::string& name, const std::optional<std::array<double, 16>>& matrix) {
    json n = { { "name", name } };
    if (matrix) n["matrix"] = *matrix;
    return node(n, {});
}

void a49::Glb::animate(const std::string& name, int node_index, const std::vector<double>& times,
                       const std::vector<std::array<double, 4>>* rotation, const std::vector<V3>* scale,
                       const std::vector<V3>* translation) {
    json anim = { { "name", name }, { "channels", json::array() }, { "samplers", json::array() } };
    int t = accessor_f(std::vector<float>(times.begin(), times.end()), "SCALAR", 1, 0, true);
    auto channel = [&](const char* path, std::vector<float> values, const char* kind, int width) {
        int out = accessor_f(values, kind, width, 0, false);
        anim["samplers"].push_back({ { "input", t }, { "output", out }, { "interpolation", "LINEAR" } });
        anim["channels"].push_back({ { "sampler", anim["samplers"].size() - 1 },
                                     { "target", { { "node", node_index }, { "path", path } } } });
    };
    if (rotation) {
        std::vector<float> v;
        for (const auto& q : *rotation) for (double c : q) v.push_back(float(c));
        channel("rotation", v, "VEC4", 4);
    }
    if (scale) channel("scale", flat3(*scale), "VEC3", 3);
    if (translation) channel("translation", flat3(*translation), "VEC3", 3);
    if (!js.contains("animations")) js["animations"] = json::array();
    js["animations"].push_back(anim);
}

void a49::Glb::set_parent(int child, int parent) {
    auto& roots = js["scenes"][0]["nodes"];
    for (size_t i = 0; i < roots.size(); i++) {
        if (roots[i] == child) { roots.erase(i); break; }
    }
    js["nodes"][parent]["children"].push_back(child);
}

std::vector<uint8_t> a49::Glb::bytes() {
    while (bin.size() % 4) bin.push_back(0);
    js["buffers"] = json::array({ { { "byteLength", bin.size() } } });
    std::string j = js.dump();
    while (j.size() % 4) j += ' ';
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (8 * i))); };
    u32(0x46546C67);
    u32(2);
    u32(uint32_t(28 + j.size() + bin.size()));
    u32(uint32_t(j.size()));
    u32(0x4E4F534A);
    out.insert(out.end(), j.begin(), j.end());
    u32(uint32_t(bin.size()));
    u32(0x004E4942);
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

std::array<double, 16> a49::gate_matrix(const V3& pos, const V3& direction, double radius) {
    double fx = -direction[0], fz = direction[2];
    double n = std::hypot(fx, fz);
    if (n == 0) n = 1.0;
    fx /= n, fz /= n;
    V3 z{ -fx, 0.0, -fz }, y{ 0.0, 1.0, 0.0 };
    V3 x = cross(y, z);
    double s = radius / feet_per_meter;
    V3 t = to_gltf(pos);
    return { x[0] * s, x[1] * s, x[2] * s, 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, t[0], t[1], t[2], 1 };
}
