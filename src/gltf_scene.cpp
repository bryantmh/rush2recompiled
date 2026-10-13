// Binary glTF reader (include/gltf_scene.h). Used by custom tracks (src/track_mod/); the car and model mods keep their
// own flattening reader (src/car_mods.cpp parse_glb).

#include <cmath>
#include <cstring>
#include <functional>
#include <map>

#include "gltf_scene.h"

namespace {
    using nlohmann::json;

    bool read_accessor(const json& js, const std::vector<uint8_t>& bin, int index, int width, std::vector<float>& out) {
        if (!js.contains("accessors") || index < 0 || index >= (int)js["accessors"].size()) return false;
        const auto& a = js["accessors"][index];
        if (!a.contains("bufferView")) return false;
        int view = a["bufferView"].get<int>();
        if (view < 0 || view >= (int)js["bufferViews"].size()) return false;
        const auto& v = js["bufferViews"][view];
        int type = a["componentType"].get<int>();
        size_t count = a["count"].get<size_t>();
        size_t size = type == 5126 || type == 5125 ? 4 : type == 5123 || type == 5122 ? 2 : 1;
        size_t start = v.value("byteOffset", size_t(0)) + a.value("byteOffset", size_t(0));
        // Normalized integers (quantized UVs, 8-bit colors) are fractions of their type's largest value.
        float scale = !a.value("normalized", false) ? 1.0f : type == 5121 ? 1.0f / 255.0f : type == 5123 ? 1.0f / 65535.0f
                      : type == 5120 ? 1.0f / 127.0f : type == 5122 ? 1.0f / 32767.0f : 1.0f;
        size_t stride = v.value("byteStride", size_t(0));
        if (stride == 0) stride = size * width;
        if (start + (count ? (count - 1) * stride + size * width : 0) > bin.size()) return false;
        out.resize(count * width);
        for (size_t k = 0; k < count; k++) {
            for (int c = 0; c < width; c++) {
                const uint8_t* p = &bin[start + k * stride + c * size];
                float f = 0.0f;
                switch (type) {
                case 5126: memcpy(&f, p, 4); break;
                case 5125: { uint32_t u; memcpy(&u, p, 4); f = float(u); break; }
                case 5123: { uint16_t u; memcpy(&u, p, 2); f = float(u); break; }
                case 5122: { int16_t u; memcpy(&u, p, 2); f = float(u); break; }
                case 5121: f = float(*p); break;
                default: f = float(int8_t(*p)); break;
                }
                out[k * width + c] = type == 5126 ? f : f * scale;
            }
        }
        return true;
    }

    std::array<float, 16> node_matrix(const json& n) {
        std::array<float, 16> m = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        if (n.contains("matrix")) {
            for (int i = 0; i < 16; i++) m[i] = n["matrix"][i].get<float>();
            return m;
        }
        float t[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };
        if (n.contains("translation")) for (int i = 0; i < 3; i++) t[i] = n["translation"][i].get<float>();
        if (n.contains("rotation")) for (int i = 0; i < 4; i++) q[i] = n["rotation"][i].get<float>();
        if (n.contains("scale")) for (int i = 0; i < 3; i++) s[i] = n["scale"][i].get<float>();
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float r[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                       2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                       2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) }; // columns
        for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) m[c * 4 + k] = r[c * 3 + k] * s[c];
        for (int k = 0; k < 3; k++) m[12 + k] = t[k];
        return m;
    }

    std::array<float, 16> multiply(const std::array<float, 16>& a, const std::array<float, 16>& b) {
        std::array<float, 16> o{};
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) o[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
        return o;
    }
}

bool rush2::gltf::read_glb(const std::vector<uint8_t>& bytes, Scene& scene, std::string& error) {
    auto u32le = [&](size_t o) { uint32_t v; memcpy(&v, &bytes[o], 4); return v; };
    if (bytes.size() < 20 || u32le(0) != 0x46546C67) { error = "not a .glb file"; return false; }
    json js;
    std::vector<uint8_t> bin;
    for (size_t o = 12; o + 8 <= bytes.size();) {
        uint32_t n = u32le(o), kind = u32le(o + 4);
        if (o + 8 + n > bytes.size()) break;
        if (kind == 0x4E4F534A) js = json::parse(bytes.begin() + o + 8, bytes.begin() + o + 8 + n, nullptr, false);
        if (kind == 0x004E4942) bin.assign(bytes.begin() + o + 8, bytes.begin() + o + 8 + n);
        o += 8 + ((n + 3) & ~3u);
    }
    if (!js.is_object() || !js.contains("nodes")) { error = "the .glb has no nodes"; return false; }
    scene = Scene{};
    if (js.contains("asset")) scene.generator = js["asset"].value("generator", std::string());

    if (js.contains("images")) {
        for (const auto& img : js["images"]) {
            Image im;
            im.name = img.value("name", std::string());
            im.mime = img.value("mimeType", std::string());
            if (img.contains("bufferView")) {
                const auto& v = js["bufferViews"][img["bufferView"].get<int>()];
                size_t start = v.value("byteOffset", size_t(0)), length = v.value("byteLength", size_t(0));
                if (start + length <= bin.size()) im.data.assign(bin.begin() + start, bin.begin() + start + length);
            }
            scene.images.push_back(std::move(im));
        }
    }
    if (js.contains("materials")) {
        for (const auto& mat : js["materials"]) {
            Material m;
            m.name = mat.value("name", std::string());
            m.alpha_mode = mat.value("alphaMode", std::string("OPAQUE"));
            m.alpha_cutoff = mat.value("alphaCutoff", 0.5f);
            m.double_sided = mat.value("doubleSided", false);
            if (mat.contains("pbrMetallicRoughness")) {
                const auto& pbr = mat["pbrMetallicRoughness"];
                if (pbr.contains("baseColorFactor")) for (int i = 0; i < 4; i++) m.color[i] = pbr["baseColorFactor"][i].get<float>();
                if (pbr.contains("baseColorTexture")) {
                    int t = pbr["baseColorTexture"].value("index", -1);
                    if (js.contains("textures") && t >= 0 && t < (int)js["textures"].size()) {
                        const auto& tex = js["textures"][t];
                        m.image = tex.value("source", -1);
                        int s = tex.value("sampler", -1);
                        if (s >= 0 && js.contains("samplers") && s < (int)js["samplers"].size()) {
                            m.wrap_s = js["samplers"][s].value("wrapS", 10497);
                            m.wrap_t = js["samplers"][s].value("wrapT", 10497);
                        }
                    }
                }
            }
            if (m.image >= (int)scene.images.size()) m.image = -1;
            scene.materials.push_back(std::move(m));
        }
    }

    std::vector<int> index_of(js["nodes"].size(), -1); // glTF node -> scene.nodes
    std::function<void(int, const std::array<float, 16>&, int)> visit = [&](int i, const std::array<float, 16>& parent, int parent_out) {
        if (i < 0 || i >= (int)js["nodes"].size() || index_of[i] >= 0) return;
        const auto& n = js["nodes"][i];
        Node out;
        out.name = n.value("name", std::string());
        out.parent = parent_out;
        out.world = multiply(parent, node_matrix(n));
        if (n.contains("extras") && n["extras"].is_object()) out.extras = n["extras"];
        const float* a = out.world.data();
        float det = a[0] * (a[5] * a[10] - a[9] * a[6]) - a[4] * (a[1] * a[10] - a[9] * a[2]) + a[8] * (a[1] * a[6] - a[5] * a[2]);
        if (n.contains("mesh") && js.contains("meshes")) {
            const auto& mesh = js["meshes"][n["mesh"].get<int>()];
            if (mesh.contains("extras") && mesh["extras"].is_object()) {
                for (auto it = mesh["extras"].begin(); it != mesh["extras"].end(); ++it) {
                    if (!out.extras.contains(it.key())) out.extras[it.key()] = it.value();
                }
            }
            const auto& m = out.world;
            for (const auto& p : mesh["primitives"]) {
                int mode = p.value("mode", 4);
                if (mode >= 1 && mode <= 3) { out.loose_edges++; continue; }
                if (mode != 4 || !p.contains("attributes") || !p["attributes"].contains("POSITION")) continue;
                const auto& at = p["attributes"];
                Primitive prim;
                prim.material = p.value("material", -1);
                std::vector<float> pos, nrm, uv, idx, col;
                int col_width = 4;
                if (!read_accessor(js, bin, at["POSITION"].get<int>(), 3, pos)) continue;
                if (at.contains("NORMAL")) read_accessor(js, bin, at["NORMAL"].get<int>(), 3, nrm);
                if (at.contains("TEXCOORD_0")) read_accessor(js, bin, at["TEXCOORD_0"].get<int>(), 2, uv);
                if (at.contains("COLOR_0")) {
                    int c = at["COLOR_0"].get<int>();
                    col_width = js["accessors"][c].value("type", std::string("VEC4")) == "VEC3" ? 3 : 4;
                    read_accessor(js, bin, c, col_width, col);
                }
                size_t count = pos.size() / 3;
                if (p.contains("indices")) read_accessor(js, bin, p["indices"].get<int>(), 1, idx);
                else for (size_t k = 0; k < count; k++) idx.push_back(float(k));
                for (size_t k = 0; k < count; k++) {
                    const float* v = &pos[k * 3];
                    prim.pos.push_back({ m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12],
                                         m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13],
                                         m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] });
                    std::array<float, 3> nn = { 0, 1, 0 };
                    if (nrm.size() == pos.size()) {
                        // Normals by the inverse transpose; for the rotation-and-scale matrices Blender writes, the
                        // matrix itself followed by a renormalize is close enough except for non-uniform scale.
                        const float* q = &nrm[k * 3];
                        float s[3] = { std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]),
                                       std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]),
                                       std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]) };
                        float qs[3];
                        for (int c = 0; c < 3; c++) qs[c] = s[c] > 1e-12f ? q[c] / (s[c] * s[c]) : 0.0f;
                        nn = { m[0] * qs[0] + m[4] * qs[1] + m[8] * qs[2], m[1] * qs[0] + m[5] * qs[1] + m[9] * qs[2],
                               m[2] * qs[0] + m[6] * qs[1] + m[10] * qs[2] };
                        float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                        if (l > 1e-12f) for (float& c : nn) c /= l;
                    }
                    prim.nrm.push_back(nn);
                    prim.uv.push_back(uv.size() == count * 2 ? std::array<float, 2>{ uv[k * 2], uv[k * 2 + 1] }
                                                             : std::array<float, 2>{ 0.0f, 0.0f });
                    if (col.size() == count * col_width) {
                        const float* c = &col[k * col_width];
                        prim.col.push_back({ c[0], c[1], c[2], col_width == 4 ? c[3] : 1.0f });
                    }
                }
                for (size_t k = 0; k + 2 < idx.size(); k += 3) {
                    uint32_t t[3] = { uint32_t(idx[k]), uint32_t(idx[k + 1]), uint32_t(idx[k + 2]) };
                    if (t[0] >= count || t[1] >= count || t[2] >= count) continue;
                    if (det < 0) std::swap(t[1], t[2]);
                    prim.tris.insert(prim.tris.end(), t, t + 3);
                }
                out.primitives.push_back(std::move(prim));
            }
        }
        int me = int(scene.nodes.size());
        index_of[i] = me;
        std::array<float, 16> world = out.world;
        scene.nodes.push_back(std::move(out));
        if (n.contains("children")) for (const auto& c : n["children"]) visit(c.get<int>(), world, me);
    };
    const std::array<float, 16> identity = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    int s = js.value("scene", 0);
    if (js.contains("scenes") && s < (int)js["scenes"].size() && js["scenes"][s].contains("nodes")) {
        for (const auto& n : js["scenes"][s]["nodes"]) visit(n.get<int>(), identity, -1);
    }
    else {
        for (size_t i = 0; i < js["nodes"].size(); i++) visit(int(i), identity, -1);
    }
    return true;
}
