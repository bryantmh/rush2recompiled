// Car model mods (include/car_mods.h, docs/car_mods.md).
//
// A mod's rush2/cars/<prefix>.glb is the whole car as one mesh, in the model dump's space (docs/asset_catalog.md,
// Model dump: vertex units of 1/16 ft, y up, +z the front, x negated). It becomes a Rush 2 car container like a
// converted Rush 2049 car's (docs/rush2049_research/cars.md section 8): <prefix>FRAME1 is the mesh, in two LODs as
// Rush 2's FRAME1s have, and every other part an empty list, so the panel swaps of Rush 2's damage draw nothing and
// the body dents instead (rush2::car2049::set_body_car). Any number of triangles: the lists load 32 vertices at a time.
//
// Textures: each image the mesh uses gets a stand-in, a CI8 texture of at most 2 KB in Rush 2's car palette
// (CARPALETTE, asset 0x1C), so Rush 2's own paint (func_8008582C) colors it: texels the mod's paint mask marks take
// MAIN COLOR's ramp (entries 1-31) or ACCENT COLOR's (33-63) by their brightness, the rest the nearest fixed entry.
// It is what draws without RT64's replacements; with them, rush2::derive replaces each stand-in drawn (one RT64 hash
// per paint) with the mod's full-size image in that paint (texels 0 and 1 of a stand-in are entries 1 and 33, so the
// paint can be read from it). A scuffed copy of each stand-in is what a damaged panel draws.
//
// The containers are kept on disk (track_cache blobs keyed on the glb, the palette and the build): converting
// decodes the mod's images.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <filesystem>
#include <initializer_list>
#include <cctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "recomp.h"

#include "asset_catalog.h"
#include "assets.h"
#include "car2049.h"
#include "car_mods.h"
#include "texture_derive.h"
#include "texture_upscale.h"
#include "track_cache.h"

namespace {
    using rush2::upscale::Image;

    constexpr int types = rush2::car2049::rush2_types;
    constexpr int first_car_asset = 0x1D;
    constexpr uint32_t car_slot_size = 0x8001CF28; // largest car asset: every car slot is allocated at it
    constexpr int stripe_asset = 0x1C; // holds CARPALETTE
    constexpr uint32_t game_state = 0x8010C0D0;
    // Part name prefixes per type (0x800CAF3C).
    const char* const prefixes[types] = { "PICKUP", "INTEG", "VETTE", "SLED", "BMW", "CAMARO", "SUPRA", "BUGAT", "VWBUS",
        "VIPER", "VWBUG", "CONCPT", "CIVIC", "CADDY", "MUST", "SUV", "TAXI", "HOTROD", "FORM1", "GT90", "ROCKET", "DEW" };
    // Every part a Rush 2 car has (func_80086700 looks each up and uses the handle unchecked).
    const char* const parts[] = {
        "FRAME1", "D0_FR1", "D2_FR1", "D0_FL1", "D2_FL1", "D0_RR1", "D2_RR1", "D0_RL1", "D2_RL1", "D0_TOP1",
        "D1_TOP1", "D0_DFR1", "D2_DFR1", "D0_DFL1", "D2_DFL1", "D0_DRR1", "D2_DRR1", "D0_DRL1", "D2_DRL1", "D0_DTOP1",
        "D0_D2FR2", "D2_D2FR2", "D0_D2FL2", "D2_D2FL2", "D0_D2RR2", "D2_D2RR2", "D0_D2RL2", "D2_D2RL2", "D0_D2TOP2",
        "H0_L", "H2_L", "H0_R", "H2_R", "H0D_L", "H2D_L", "H0D_R", "H2D_R",
    };
    constexpr uint32_t cache_version = 4;

    // Rush 2's car palette classes (0x800C5670, docs/game_fixes.md Rocket windshield): 1-31 MAIN, 33-63 ACCENT,
    // 64-95 / 112-143 / 208-255 stripe stamped, the rest fixed.
    constexpr int main_first = 1, main_last = 31, accent_first = 33, accent_last = 63;

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }
    void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
        for (int i = 0; i < 4; i++) d[o + i] = uint8_t(v >> (24 - 8 * i));
    }
    void put16(std::vector<uint8_t>& d, size_t o, uint16_t v) {
        d[o] = uint8_t(v >> 8);
        d[o + 1] = uint8_t(v);
    }
    void add32(std::vector<uint8_t>& d, uint32_t v) {
        for (int i = 0; i < 4; i++) d.push_back(uint8_t(v >> (24 - 8 * i)));
    }
    void add_float(std::vector<uint8_t>& d, float f) {
        uint32_t v;
        memcpy(&v, &f, 4);
        add32(d, v);
    }
    void align8(std::vector<uint8_t>& d) {
        while (d.size() % 8) d.push_back(0);
    }

    // ---- glTF binary -------------------------------------------------------------------------------------------
    struct Part {
        std::vector<std::array<float, 3>> pos, nrm;
        std::vector<std::array<float, 2>> uv;
        std::vector<std::array<float, 4>> col; // COLOR_0 (0-1), when the mesh has it: a dumped model's baked lighting
        std::vector<uint32_t> tris;  // 3 per triangle, wound as glTF (counter-clockwise from outside)
        int image = -1, mask = -1;   // into Model::images
    };
    struct Model {
        std::vector<Part> parts;
        std::vector<std::vector<uint8_t>> images; // encoded (PNG, JPEG...)
    };

    bool read_accessor(const nlohmann::json& js, const std::vector<uint8_t>& bin, int index, int width,
                       std::vector<float>& out) {
        const auto& a = js["accessors"][index];
        if (!a.contains("bufferView")) return false;
        const auto& v = js["bufferViews"][a["bufferView"].get<int>()];
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

    // A node's matrix (column-major, as glTF stores it).
    std::array<float, 16> node_matrix(const nlohmann::json& n) {
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

    bool parse_glb(const std::vector<uint8_t>& bytes, Model& model, std::string& error) {
        auto u32le = [&](size_t o) { uint32_t v; memcpy(&v, &bytes[o], 4); return v; };
        if (bytes.size() < 20 || u32le(0) != 0x46546C67) { error = "not a glb"; return false; }
        nlohmann::json js;
        std::vector<uint8_t> bin;
        for (size_t o = 12; o + 8 <= bytes.size();) {
            uint32_t n = u32le(o), kind = u32le(o + 4);
            if (o + 8 + n > bytes.size()) break;
            if (kind == 0x4E4F534A) js = nlohmann::json::parse(bytes.begin() + o + 8, bytes.begin() + o + 8 + n, nullptr, false);
            if (kind == 0x004E4942) bin.assign(bytes.begin() + o + 8, bytes.begin() + o + 8 + n);
            o += 8 + ((n + 3) & ~3u);
        }
        if (!js.is_object() || !js.contains("meshes") || !js.contains("nodes")) { error = "no meshes"; return false; }
        // Images by texture index, each image once.
        std::map<int, int> image_of_source;
        auto texture_image = [&](int texture) -> int {
            if (!js.contains("textures") || texture < 0 || texture >= (int)js["textures"].size()) return -1;
            int source = js["textures"][texture].value("source", -1);
            if (source < 0 || !js.contains("images") || source >= (int)js["images"].size()) return -1;
            auto it = image_of_source.find(source);
            if (it != image_of_source.end()) return it->second;
            const auto& img = js["images"][source];
            if (!img.contains("bufferView")) return -1;
            const auto& v = js["bufferViews"][img["bufferView"].get<int>()];
            size_t start = v.value("byteOffset", size_t(0)), length = v.value("byteLength", size_t(0));
            if (start + length > bin.size()) return -1;
            model.images.emplace_back(bin.begin() + start, bin.begin() + start + length);
            return image_of_source[source] = int(model.images.size() - 1);
        };
        std::function<void(int, const std::array<float, 16>&)> node = [&](int i, const std::array<float, 16>& parent) {
            const auto& n = js["nodes"][i];
            std::array<float, 16> m = multiply(parent, node_matrix(n));
            const float* a = m.data();
            float det = a[0] * (a[5] * a[10] - a[9] * a[6]) - a[4] * (a[1] * a[10] - a[9] * a[2]) + a[8] * (a[1] * a[6] - a[5] * a[2]);
            // A node scaled to nothing is hidden: the model dump's other LODs ("<model> LOD n", scale 0).
            if (std::fabs(det) < 1e-12f) return;
            if (n.contains("mesh")) {
                // Normals by the inverse transpose of the 3x3 part; a mirroring matrix flips the winding.
                for (const auto& p : js["meshes"][n["mesh"].get<int>()]["primitives"]) {
                    if (p.value("mode", 4) != 4 || !p.contains("attributes") || !p["attributes"].contains("POSITION")) continue;
                    const auto& at = p["attributes"];
                    Part part;
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
                        part.pos.push_back({ m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12],
                                             m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13],
                                             m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] });
                        std::array<float, 3> nn = { 0, 1, 0 };
                        if (nrm.size() == pos.size()) {
                            const float* q = &nrm[k * 3];
                            nn = { m[0] * q[0] + m[4] * q[1] + m[8] * q[2], m[1] * q[0] + m[5] * q[1] + m[9] * q[2],
                                   m[2] * q[0] + m[6] * q[1] + m[10] * q[2] };
                            float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                            if (l > 1e-9f) for (float& c : nn) c /= l;
                        }
                        part.nrm.push_back(nn);
                        part.uv.push_back(uv.size() == count * 2 ? std::array<float, 2>{ uv[k * 2], uv[k * 2 + 1] }
                                                                  : std::array<float, 2>{ 0.0f, 0.0f });
                        if (col.size() == count * col_width) {
                            const float* c = &col[k * col_width];
                            part.col.push_back({ c[0], c[1], c[2], col_width == 4 ? c[3] : 1.0f });
                        }
                    }
                    for (size_t k = 0; k + 2 < idx.size(); k += 3) {
                        uint32_t t[3] = { uint32_t(idx[k]), uint32_t(idx[k + 1]), uint32_t(idx[k + 2]) };
                        if (t[0] >= count || t[1] >= count || t[2] >= count) continue;
                        if (det < 0) std::swap(t[1], t[2]);
                        part.tris.insert(part.tris.end(), t, t + 3);
                    }
                    if (p.contains("material") && js.contains("materials")) {
                        const auto& mat = js["materials"][p["material"].get<int>()];
                        if (mat.contains("pbrMetallicRoughness") && mat["pbrMetallicRoughness"].contains("baseColorTexture")) {
                            part.image = texture_image(mat["pbrMetallicRoughness"]["baseColorTexture"].value("index", -1));
                        }
                        if (mat.contains("extras") && mat["extras"].contains("rush2_paint")) {
                            part.mask = texture_image(mat["extras"]["rush2_paint"].get<int>());
                        }
                    }
                    if (!part.tris.empty()) model.parts.push_back(std::move(part));
                }
            }
            if (n.contains("children")) for (const auto& c : n["children"]) node(c.get<int>(), m);
        };
        const std::array<float, 16> identity = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        int scene = js.value("scene", 0);
        if (js.contains("scenes") && scene < (int)js["scenes"].size()) {
            for (const auto& n : js["scenes"][scene]["nodes"]) node(n.get<int>(), identity);
        }
        else {
            for (size_t i = 0; i < js["nodes"].size(); i++) node(int(i), identity);
        }
        if (model.parts.empty()) { error = "no triangles"; return false; }
        return true;
    }

    // ---- stand-in textures -------------------------------------------------------------------------------------
    struct Rgba { uint8_t r, g, b, a; };
    std::vector<Rgba> car_palette; // CARPALETTE, 256 entries

    bool load_car_palette(uint8_t* rdram) {
        std::vector<uint8_t> d;
        if (!rush2::assets::read_original(rdram, stripe_asset, d) || d.size() < 0x28) return false;
        uint32_t table = be32(d, 12), count = be32(d, 24);
        for (uint32_t i = 0; i < count && table + i * 0x18 + 0x18 <= d.size(); i++) {
            uint32_t r = table + i * 0x18;
            if (strncmp(reinterpret_cast<const char*>(&d[r]), "CARPALETTE", 16) != 0) continue;
            uint32_t data = be32(d, r + 20) & 0xFFFFFF;
            if (data + 512 > d.size()) return false;
            car_palette.clear();
            for (int k = 0; k < 256; k++) {
                uint16_t c = uint16_t((d[data + k * 2] << 8) | d[data + k * 2 + 1]);
                auto five = [](int v) { return uint8_t((v << 3) | (v >> 2)); };
                car_palette.push_back({ five((c >> 11) & 31), five((c >> 6) & 31), five((c >> 1) & 31), uint8_t(c & 1 ? 255 : 0) });
            }
            return true;
        }
        return false;
    }

    struct StandIn {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> texels, scuffed;
        int image = -1, mask = -1;
        uint32_t offset = 0;              // in the container
        uint64_t key = 0, scuffed_key = 0; // index_key of each
    };

    // A paint mask is red = MAIN COLOR weight, green = ACCENT COLOR weight. A grayscale one (every texel r = g = b, as
    // most paint programs save a one-channel mask) means MAIN only: read as is, it would mix the two colors half and half.
    void decode_mask(const std::vector<uint8_t>& file, Image& mask) {
        if (!rush2::upscale::decode_image(file, mask)) return;
        for (size_t i = 0; i + 3 < mask.rgba.size(); i += 4) {
            if (mask.rgba[i] != mask.rgba[i + 1] || mask.rgba[i] != mask.rgba[i + 2]) return;
        }
        for (size_t i = 0; i + 3 < mask.rgba.size(); i += 4) {
            mask.rgba[i + 1] = 0;
            mask.rgba[i + 2] = 0;
        }
    }

    // A stand-in for image (and mask): the size its aspect suits (64 x 32, 32 x 64 or 32 x 32), each texel the
    // CARPALETTE entry that draws it best (see the top of the file).
    bool make_stand_in(const Model& model, StandIn& s, uint32_t seed) {
        Image image, mask;
        if (s.image < 0 || !rush2::upscale::decode_image(model.images[s.image], image) || image.empty()) {
            image.width = image.height = 1;
            image.rgba = { 200, 200, 200, 255 };
        }
        if (s.mask >= 0) decode_mask(model.images[s.mask], mask);
        float aspect = float(image.width) / float(image.height);
        s.width = aspect >= 1.5f ? 64 : 32;
        s.height = aspect <= 0.67f ? 64 : 32;
        Image small = rush2::upscale::shrink(image, s.width, s.height);
        Image small_mask = mask.empty() ? Image{} : rush2::upscale::shrink(mask, s.width, s.height);
        std::vector<int> fixed;
        for (int i = 0; i < 256; i++) {
            bool painted = (i >= 1 && i <= 31) || (i >= 33 && i <= 95) || (i >= 112 && i <= 143) || i >= 208;
            const Rgba& c = car_palette[i];
            bool marker = c.r == 255 && c.g == 0 && c.b == 255;
            if (!painted && c.a != 0 && !marker) fixed.push_back(i);
        }
        auto ramp = [&](int first, int last, float lum) {
            int best = first;
            float bd = 1e9f;
            for (int i = first; i <= last; i++) {
                float d = std::fabs(float(car_palette[i].r) - lum);
                if (d < bd) { bd = d; best = i; }
            }
            return best;
        };
        size_t n = size_t(s.width) * s.height;
        s.texels.resize(n);
        for (size_t i = 0; i < n; i++) {
            const uint8_t* p = &small.rgba[i * 4];
            float wm = small_mask.empty() ? 0.0f : small_mask.rgba[i * 4] / 255.0f;
            float wa = small_mask.empty() ? 0.0f : small_mask.rgba[i * 4 + 1] / 255.0f;
            float lum = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) * (230.0f / 255.0f);
            if (wm >= 0.5f && wm >= wa) s.texels[i] = uint8_t(ramp(main_first, main_last, lum));
            else if (wa >= 0.5f) s.texels[i] = uint8_t(ramp(accent_first, accent_last, lum));
            else {
                int best = fixed.empty() ? 0 : fixed[0];
                float bd = 1e9f;
                for (int f : fixed) {
                    const Rgba& c = car_palette[f];
                    float d = 0.3f * (c.r - p[0]) * (c.r - p[0]) + 0.59f * (c.g - p[1]) * (c.g - p[1]) +
                              0.11f * (c.b - p[2]) * (c.b - p[2]);
                    if (d < bd) { bd = d; best = f; }
                }
                s.texels[i] = uint8_t(best);
            }
        }
        // The paint the stand-in is drawn in, for rush2::derive.
        s.texels[0] = main_first;
        s.texels[1] = accent_first;
        // The damaged copy: the ramps' texels shifted lighter and darker in blotches (as Rush 2's D1 textures are).
        s.scuffed = s.texels;
        auto hash = [seed](int x, int y) {
            uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ seed * 83492791u;
            h ^= h >> 13;
            h *= 0x5BD1E995u;
            h ^= h >> 15;
            return float(h & 0xFFFF) / 65535.0f;
        };
        for (size_t i = 2; i < n; i++) {
            int v = s.texels[i];
            int first = v >= main_first && v <= main_last ? main_first : v >= accent_first && v <= accent_last ? accent_first : -1;
            if (first < 0) continue;
            int x = int(i % s.width) / 3, y = int(i / s.width) / 3;
            int shift = int(std::lround((hash(x, y) - 0.5f) * 8.0f));
            s.scuffed[i] = uint8_t(std::clamp(v + shift, first, first + 30));
        }
        s.key = rush2::upscale::index_key(s.texels, s.width, s.height, 1);
        s.scuffed_key = rush2::upscale::index_key(s.scuffed, s.width, s.height, 1);
        return true;
    }

    // ---- the car container ---------------------------------------------------------------------------------------
    // One LOD's FRAME1 list and its vertices (appended to d): per stand-in its texture load, then the triangles, 32
    // vertices per G_VTX. Returns the list's offset.
    uint32_t emit_body(std::vector<uint8_t>& d, const Model& model, const std::vector<StandIn>& stand_ins,
                       const std::vector<uint32_t>& load_lists, float& radius) {
        std::vector<uint8_t> vertices;
        std::vector<std::pair<uint32_t, uint32_t>> list; // commands; a G_VTX's w1 is relative to the vertices
        std::vector<size_t> vtx_commands;
        auto cmd = [&](uint32_t w0, uint32_t w1) { list.emplace_back(w0, w1); };
        // Render mode and combiner as Rush 2's car panels set them (PICKUPD0_TOP1).
        bool first = true;
        const float light[3] = { 0.30f, 0.80f, 0.52f };
        for (size_t si = 0; si < stand_ins.size(); si++) {
            const StandIn& s = stand_ins[si];
            bool loaded = false;
            for (const Part& part : model.parts) {
                if (part.image != s.image || part.mask != s.mask) continue;
                if (!loaded) {
                    cmd(0xDE000000, load_lists[si]);
                    if (first) {
                        cmd(0xE200001C, 0xC8112230);
                        cmd(0xFC1216AC, 0xF0FFFE38);
                        first = false;
                    }
                    loaded = true;
                }
                std::vector<int> slot_of(part.pos.size(), -1);
                std::vector<uint32_t> slots;
                std::vector<std::array<int, 3>> tris;
                auto flush = [&]() {
                    if (slots.empty()) return;
                    uint32_t at = uint32_t(vertices.size());
                    for (uint32_t v : slots) {
                        const auto& p = part.pos[v];
                        const auto& nn = part.nrm[v];
                        const auto& uv = part.uv[v];
                        // The game's x is glTF's negated (the model dump's convention).
                        int16_t xyz[3] = { int16_t(std::clamp(std::lround(-p[0]), -32767l, 32767l)),
                                           int16_t(std::clamp(std::lround(p[1]), -32767l, 32767l)),
                                           int16_t(std::clamp(std::lround(p[2]), -32767l, 32767l)) };
                        radius = std::max(radius, std::sqrt(float(xyz[0]) * xyz[0] + float(xyz[1]) * xyz[1] + float(xyz[2]) * xyz[2]));
                        float shade = 0.72f + 0.28f * std::max(0.0f, -nn[0] * light[0] + nn[1] * light[1] + nn[2] * light[2]);
                        uint8_t c = uint8_t(std::clamp(shade * 255.0f, 0.0f, 255.0f));
                        // The N64 puts texel i's center at coordinate i (Rush 2's panels run 0 to width - 1), glTF at
                        // i + 0.5: without the half texel, every sample lands half a stand-in texel (16 texels of a
                        // 1024-wide image) off, into the gaps between the image's islands.
                        int32_t st[2] = { int32_t(std::lround((uv[0] * s.width - 0.5f) * 32.0f)),
                                          int32_t(std::lround((uv[1] * s.height - 0.5f) * 32.0f)) };
                        for (int k = 0; k < 3; k++) { vertices.push_back(uint8_t(uint16_t(xyz[k]) >> 8)); vertices.push_back(uint8_t(xyz[k])); }
                        vertices.push_back(0x80); // flags: another vertex follows (the mirror walk; width 0)
                        vertices.push_back(0x00);
                        for (int k = 0; k < 2; k++) {
                            int16_t t = int16_t(std::clamp(st[k], -32767, 32767));
                            vertices.push_back(uint8_t(uint16_t(t) >> 8));
                            vertices.push_back(uint8_t(t));
                        }
                        vertices.push_back(c);
                        vertices.push_back(c);
                        vertices.push_back(c);
                        vertices.push_back(0xFF);
                    }
                    uint32_t count = uint32_t(slots.size());
                    vtx_commands.push_back(list.size());
                    cmd(0x01000000 | (count << 12) | (count << 1), at);
                    for (size_t t = 0; t < tris.size(); t += 2) {
                        uint32_t a = (tris[t][0] * 2 << 16) | (tris[t][1] * 2 << 8) | (tris[t][2] * 2);
                        if (t + 1 < tris.size()) {
                            uint32_t b = (tris[t + 1][0] * 2 << 16) | (tris[t + 1][1] * 2 << 8) | (tris[t + 1][2] * 2);
                            cmd(0x06000000 | a, b);
                        }
                        else {
                            cmd(0x05000000 | a, 0);
                        }
                    }
                    for (uint32_t v : slots) slot_of[v] = -1;
                    slots.clear();
                    tris.clear();
                };
                for (size_t t = 0; t + 2 < part.tris.size(); t += 3) {
                    uint32_t v[3] = { part.tris[t], part.tris[t + 1], part.tris[t + 2] };
                    int fresh = 0;
                    for (int k = 0; k < 3; k++) fresh += slot_of[v[k]] < 0 && (k == 0 || v[k] != v[0]) && (k < 2 || v[2] != v[1]);
                    if (slots.size() + fresh > 32) flush();
                    std::array<int, 3> tri;
                    for (int k = 0; k < 3; k++) {
                        if (slot_of[v[k]] < 0) {
                            slot_of[v[k]] = int(slots.size());
                            slots.push_back(v[k]);
                        }
                        tri[k] = slot_of[v[k]];
                    }
                    tris.push_back(tri);
                }
                flush();
            }
        }
        cmd(0xDF000000, 0);
        // The last vertex ends the chain.
        if (vertices.size() >= 16) vertices[vertices.size() - 16 + 6] = 0;
        align8(d);
        uint32_t base = uint32_t(d.size());
        d.insert(d.end(), vertices.begin(), vertices.end());
        align8(d);
        uint32_t at = uint32_t(d.size());
        for (size_t i : vtx_commands) list[i].second += base;
        for (const auto& [w0, w1] : list) {
            add32(d, w0);
            add32(d, w1);
        }
        return at;
    }

    // The container: header, stand-in texels, their load lists, model / texture / palette tables, the lists and
    // vertices, the name table (docs/rush2049_research/geometry.md section 2).
    std::vector<uint8_t> build_container(const std::string& prefix, const Model& model, std::vector<StandIn>& stand_ins) {
        std::vector<uint8_t> d(0x28, 0);
        for (StandIn& s : stand_ins) {
            align8(d);
            s.offset = uint32_t(d.size());
            d.insert(d.end(), s.texels.begin(), s.texels.end());
        }
        align8(d);
        uint32_t txld_start = uint32_t(d.size());
        std::vector<uint32_t> load_lists;
        for (const StandIn& s : stand_ins) {
            load_lists.push_back(uint32_t(d.size()));
            uint32_t line = s.width / 8;            // 64-bit words per row of CI8
            uint32_t bytes = s.width * s.height;
            uint32_t masks = s.width == 64 ? 6 : 5, maskt = s.height == 64 ? 6 : 5;
            uint32_t tile = (maskt << 14) | (masks << 4); // wrap in both
            uint32_t dxt = 2048 / line;
            // Loaded as 16-bit texels (half as many), drawn as CI 8-bit: as Rush 2's car texture loads do.
            for (uint32_t w : { 0xFD500000u, s.offset, 0xF5500000u, 0x07000000u | tile, 0xE6000000u, 0u,
                                0xF3000000u, 0x07000000u | ((bytes / 2 - 1) << 12) | dxt, 0xE7000000u, 0u,
                                0xF5480000u | (line << 9), tile,
                                0xF2000000u, (((s.width - 1) * 4) << 12) | ((s.height - 1) * 4), 0xDF000000u, 0u }) {
                add32(d, w);
            }
        }
        uint32_t txld_end = uint32_t(d.size());
        // Names sorted (func_8005BCB4's binary search), model i under name i.
        std::vector<std::string> names;
        for (const char* p : parts) names.push_back(prefix + p);
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
            return strncmp(a.c_str(), b.c_str(), 15) < 0;
        });
        uint32_t model_table = uint32_t(d.size());
        d.resize(d.size() + names.size() * 0x34, 0);
        uint32_t texture_table = uint32_t(d.size());
        for (size_t i = 0; i < stand_ins.size(); i++) {
            const StandIn& s = stand_ins[i];
            std::vector<uint8_t> r(0x20, 0);
            char name[17];
            snprintf(name, sizeof(name), "%sM%02zu", prefix.c_str(), i); // unique per car: bank_instance tells bodies apart by it
            memcpy(r.data(), name, strlen(name));
            put16(r, 0x10, uint16_t(s.width));
            put16(r, 0x12, uint16_t(s.height));
            r[0x14] = 1; // 8-bit
            r[0x15] = 2; // CI
            put16(r, 0x16, 0xFFFF);
            put32(r, 0x18, s.offset);
            put32(r, 0x1C, 0x28000000); // texels (as Rush 2's car textures), wrapping
            d.insert(d.end(), r.begin(), r.end());
        }
        uint32_t palette_table = uint32_t(d.size());
        align8(d);
        uint32_t empty = uint32_t(d.size());
        add32(d, 0xDF000000);
        add32(d, 0);
        float radius = 0.0f;
        uint32_t lod0 = emit_body(d, model, stand_ins, load_lists, radius);
        uint32_t lod1 = emit_body(d, model, stand_ins, load_lists, radius); // its own copy: each LOD is relocated
        align8(d);
        uint32_t name_table = uint32_t(d.size());
        for (size_t i = 0; i < names.size(); i++) {
            bool body = names[i] == prefix + "FRAME1";
            uint32_t r = model_table + uint32_t(i) * 0x34;
            put32(d, r, body ? 2 : 1);
            for (int l = 0; l < 4; l++) {
                uint32_t e = r + 4 + l * 12;
                float dist = l == 0 ? 80.0f : (body && l == 1) ? 600.0f : 0.0f;
                uint32_t list = l == 0 ? (body ? lod0 : empty) : (body && l == 1) ? lod1 : 0;
                uint32_t bits;
                memcpy(&bits, &dist, 4);
                put32(d, e + 4, bits);
                put32(d, e + 8, list);
            }
            std::vector<uint8_t> n(0x18, 0);
            memcpy(n.data(), names[i].data(), std::min<size_t>(15, names[i].size()));
            float rad = body ? radius / 16.0f : 0.5f;
            uint32_t bits;
            memcpy(&bits, &rad, 4);
            put32(n, 16, bits);
            d.insert(d.end(), n.begin(), n.end());
        }
        put32(d, 0, model_table);
        put32(d, 4, name_table);
        put32(d, 8, texture_table);
        put32(d, 12, palette_table);
        put32(d, 16, uint32_t(names.size()));
        put32(d, 20, uint32_t(stand_ins.size()));
        put32(d, 24, 0);
        put32(d, 28, txld_start);
        put32(d, 32, txld_end);
        return d;
    }

    // ---- model mods ------------------------------------------------------------------------------------------------
    // A mod's rush2/models/.../<MODEL>.glb replaces every model of that name (the model dump's file name,
    // rush2::catalog::file_name) wherever the game loads it: Rush 2's assets, the converted tracks, the 2049 cars.
    // The model's lists are rebuilt from the mesh and appended to the container (rush2::assets::set_patcher), in its
    // own render state, with true-color (RGBA16) stand-ins that rush2::derive replaces with the full-size images.
    struct TrueColor {
        uint32_t width = 0, height = 0;
        int image = -1;
        std::vector<uint8_t> texels;    // RGBA16, big-endian
        uint64_t key = 0;               // content_key as RT64 decodes it
    };
    struct ModelMod {
        std::string mod_id, file;
        uint64_t glb_hash = 0;
        std::shared_ptr<const Model> model;
        std::vector<TrueColor> stand_ins;
        size_t triangles = 0;
    };
    std::map<std::string, ModelMod> model_mods; // by model name (file name, upper case)
    std::string model_signature;

    std::string upper(std::string s) {
        for (char& c : s) c = char(std::toupper((unsigned char)c));
        return s;
    }

    void make_true_color(const Model& model, TrueColor& s) {
        Image image;
        if (s.image < 0 || !rush2::upscale::decode_image(model.images[s.image], image) || image.empty()) {
            image.width = image.height = 1;
            image.rgba = { 200, 200, 200, 255 };
        }
        float aspect = float(image.width) / float(image.height);
        s.width = aspect >= 1.5f ? 64 : 32; // 4 KB at most: TMEM
        s.height = aspect <= 0.67f ? 64 : 32;
        Image small = rush2::upscale::shrink(image, s.width, s.height);
        s.texels.resize(size_t(s.width) * s.height * 2);
        for (size_t i = 0; i < size_t(s.width) * s.height; i++) {
            const uint8_t* q = &small.rgba[i * 4];
            uint16_t c = uint16_t(((q[0] >> 3) << 11) | ((q[1] >> 3) << 6) | ((q[2] >> 3) << 1) | (q[3] >= 128 ? 1 : 0));
            s.texels[i * 2] = uint8_t(c >> 8);
            s.texels[i * 2 + 1] = uint8_t(c);
        }
        s.key = rush2::upscale::rgba16_key(s.texels, s.width, s.height);
    }

    // A mod model's stand-ins, kept on disk by the glb (decoding the images is the slow part).
    bool load_model_mod(const std::vector<uint8_t>& glb, ModelMod& m, std::string& error) {
        auto model = std::make_shared<Model>();
        if (!parse_glb(glb, *model, error)) return false;
        for (const Part& p : model->parts) m.triangles += p.tris.size() / 3;
        m.glb_hash = rush2::track_cache::hash(glb.data(), glb.size(), cache_version);
        char name[40];
        snprintf(name, sizeof(name), "modelmod_%016llx", (unsigned long long)m.glb_hash);
        std::vector<uint8_t> blob;
        std::vector<int> images;
        for (const Part& p : model->parts) {
            if (std::find(images.begin(), images.end(), p.image) == images.end()) images.push_back(p.image);
        }
        if (rush2::track_cache::load_blob(name, m.glb_hash, blob)) {
            size_t o = 0;
            auto get32 = [&]() { uint32_t v = o + 4 <= blob.size() ? be32(blob, o) : 0; o += 4; return v; };
            uint32_t n = get32();
            for (uint32_t i = 0; i < n && o < blob.size(); i++) {
                TrueColor s;
                s.image = int(int32_t(get32()));
                s.width = get32();
                s.height = get32();
                size_t size = size_t(s.width) * s.height * 2;
                if (o + size > blob.size()) break;
                s.texels.assign(blob.begin() + o, blob.begin() + o + size);
                o += size;
                s.key = rush2::upscale::rgba16_key(s.texels, s.width, s.height);
                m.stand_ins.push_back(std::move(s));
            }
            if (m.stand_ins.size() != n || n != images.size()) m.stand_ins.clear();
        }
        if (m.stand_ins.empty()) {
            blob.clear();
            add32(blob, uint32_t(images.size()));
            for (int image : images) {
                TrueColor s;
                s.image = image;
                make_true_color(*model, s);
                for (uint32_t v : { uint32_t(s.image), s.width, s.height }) add32(blob, v);
                blob.insert(blob.end(), s.texels.begin(), s.texels.end());
                m.stand_ins.push_back(std::move(s));
            }
            rush2::track_cache::save_blob(name, m.glb_hash, blob);
        }
        m.model = model;
        return true;
    }

    // The render state a model's own list sets (following its G_DLs): its first render mode and its first textured
    // combiner.
    struct ListState {
        bool has_mode = false, has_combine = false;
        uint32_t mode[2] = {}, combine[2] = {};
    };
    void list_state(const std::vector<uint8_t>& d, uint32_t list, ListState& st, int depth = 0) {
        for (uint32_t pc = list; pc + 8 <= d.size() && depth < 4 && !(st.has_mode && st.has_combine); pc += 8) {
            uint32_t w0 = be32(d, pc), w1 = be32(d, pc + 4);
            switch (w0 >> 24) {
            case 0xE2: if (!st.has_mode) { st.has_mode = true; st.mode[0] = w0; st.mode[1] = w1; } break;
            case 0xFC: // the first combiner whose first cycle starts from TEXEL0 (a textured part)
                if (!st.has_combine && ((w0 >> 20) & 0xF) == 1) { st.has_combine = true; st.combine[0] = w0; st.combine[1] = w1; }
                break;
            case 0xDE:
                if ((w1 & 0xFFFFFF) < d.size()) list_state(d, w1 & 0xFFFFFF, st, depth + 1);
                if (((w0 >> 16) & 0xFF) == 1) return; // a branch: no return
                break;
            case 0xDF: return;
            }
        }
    }

    // One LOD's list for a mod model, appended to d with its vertices; lit: the LOD draws vertex colors as normals
    // (Rush 2049's LOD flag 0x10).
    uint32_t emit_model(std::vector<uint8_t>& d, const ModelMod& mm, const std::vector<uint32_t>& texels,
                        const ListState& st, bool lit, float& radius) {
        std::vector<uint8_t> vertices;
        std::vector<std::pair<uint32_t, uint32_t>> list;
        std::vector<size_t> vtx_commands;
        auto cmd = [&](uint32_t w0, uint32_t w1) { list.emplace_back(w0, w1); };
        cmd(0xE3001001, 0x00000000);            // no TLUT: true color
        cmd(0xE3000F00, 0x00000000);            // no mipmaps
        // The model's own render mode (blending, z, fog) and textured combiner, always textured: its list may start on
        // an untextured part (ALCATRAZTRACK: G_TEXTURE off, a shade-only combiner) and switch further on. Without a
        // textured combiner: TEXEL0 x SHADE (color and alpha), the second cycle passing it on.
        if (st.has_mode) cmd(st.mode[0], st.mode[1]);
        cmd(st.has_combine ? st.combine[0] : 0xFC1219FF, st.has_combine ? st.combine[1] : 0xFFFFFE38);
        cmd(0xD7000002, 0xFFFFFFFF);            // G_TEXTURE on, tile 0, scale 1
        const float light[3] = { 0.30f, 0.80f, 0.52f };
        const Model& model = *mm.model;
        for (size_t si = 0; si < mm.stand_ins.size(); si++) {
            const TrueColor& s = mm.stand_ins[si];
            bool loaded = false;
            for (const Part& part : model.parts) {
                if (part.image != s.image) continue;
                if (!loaded) {
                    uint32_t line = s.width * 2 / 8;
                    uint32_t masks = s.width == 64 ? 6 : 5, maskt = s.height == 64 ? 6 : 5;
                    uint32_t tile = (maskt << 14) | (masks << 4);
                    for (auto [w0, w1] : std::initializer_list<std::pair<uint32_t, uint32_t>>{
                             { 0xFD100000u, texels[si] }, { 0xF5100000u, 0x07000000u | tile }, { 0xE6000000u, 0u },
                             { 0xF3000000u, 0x07000000u | ((s.width * s.height - 1) << 12) | (2048 / line) },
                             { 0xE7000000u, 0u }, { 0xF5100000u | (line << 9), tile },
                             { 0xF2000000u, (((s.width - 1) * 4) << 12) | ((s.height - 1) * 4) } }) {
                        cmd(w0, w1);
                    }
                    loaded = true;
                }
                std::vector<int> slot_of(part.pos.size(), -1);
                std::vector<uint32_t> slots;
                std::vector<std::array<int, 3>> tris;
                auto flush = [&]() {
                    if (slots.empty()) return;
                    uint32_t at = uint32_t(vertices.size());
                    for (uint32_t v : slots) {
                        const auto& p = part.pos[v];
                        const auto& nn = part.nrm[v];
                        const auto& uv = part.uv[v];
                        int16_t xyz[3] = { int16_t(std::clamp(std::lround(-p[0]), -32767l, 32767l)),
                                           int16_t(std::clamp(std::lround(p[1]), -32767l, 32767l)),
                                           int16_t(std::clamp(std::lround(p[2]), -32767l, 32767l)) };
                        radius = std::max(radius, std::sqrt(float(xyz[0]) * xyz[0] + float(xyz[1]) * xyz[1] + float(xyz[2]) * xyz[2]));
                        uint8_t rgba[4];
                        if (lit) { // normals, game space (x negated)
                            float n3[3] = { -nn[0], nn[1], nn[2] };
                            for (int k = 0; k < 3; k++) rgba[k] = uint8_t(int8_t(std::clamp(std::lround(n3[k] * 127.0f), -127l, 127l)));
                            rgba[3] = 255;
                        }
                        else if (!part.col.empty()) {
                            for (int k = 0; k < 4; k++) rgba[k] = uint8_t(std::clamp(part.col[v][k] * 255.0f + 0.5f, 0.0f, 255.0f));
                        }
                        else {
                            float shade = 0.72f + 0.28f * std::max(0.0f, -nn[0] * light[0] + nn[1] * light[1] + nn[2] * light[2]);
                            rgba[0] = rgba[1] = rgba[2] = uint8_t(std::clamp(shade * 255.0f, 0.0f, 255.0f));
                            rgba[3] = 255;
                        }
                        // N64 texel centers are at whole coordinates (see emit_body).
                        int32_t stc[2] = { int32_t(std::lround((uv[0] * s.width - 0.5f) * 32.0f)),
                                           int32_t(std::lround((uv[1] * s.height - 0.5f) * 32.0f)) };
                        for (int k = 0; k < 3; k++) { vertices.push_back(uint8_t(uint16_t(xyz[k]) >> 8)); vertices.push_back(uint8_t(xyz[k])); }
                        vertices.push_back(0);
                        vertices.push_back(0);
                        for (int k = 0; k < 2; k++) {
                            int16_t t = int16_t(std::clamp(stc[k], -32767, 32767));
                            vertices.push_back(uint8_t(uint16_t(t) >> 8));
                            vertices.push_back(uint8_t(t));
                        }
                        vertices.insert(vertices.end(), rgba, rgba + 4);
                    }
                    uint32_t count = uint32_t(slots.size());
                    vtx_commands.push_back(list.size());
                    cmd(0x01000000 | (count << 12) | (count << 1), at);
                    for (size_t t = 0; t < tris.size(); t += 2) {
                        uint32_t a = (tris[t][0] * 2 << 16) | (tris[t][1] * 2 << 8) | (tris[t][2] * 2);
                        if (t + 1 < tris.size()) {
                            uint32_t b = (tris[t + 1][0] * 2 << 16) | (tris[t + 1][1] * 2 << 8) | (tris[t + 1][2] * 2);
                            cmd(0x06000000 | a, b);
                        }
                        else {
                            cmd(0x05000000 | a, 0);
                        }
                    }
                    for (uint32_t v : slots) slot_of[v] = -1;
                    slots.clear();
                    tris.clear();
                };
                for (size_t t = 0; t + 2 < part.tris.size(); t += 3) {
                    uint32_t v[3] = { part.tris[t], part.tris[t + 1], part.tris[t + 2] };
                    int fresh = 0;
                    for (int k = 0; k < 3; k++) fresh += slot_of[v[k]] < 0 && (k == 0 || v[k] != v[0]) && (k < 2 || v[2] != v[1]);
                    if (slots.size() + fresh > 32) flush();
                    std::array<int, 3> tri;
                    for (int k = 0; k < 3; k++) {
                        if (slot_of[v[k]] < 0) {
                            slot_of[v[k]] = int(slots.size());
                            slots.push_back(v[k]);
                        }
                        tri[k] = slot_of[v[k]];
                    }
                    tris.push_back(tri);
                }
                flush();
            }
        }
        cmd(0xE3001001, 0x00008000); // the TLUT mode Rush 2's lists leave set (RGBA16)
        cmd(0xDF000000, 0);
        align8(d);
        uint32_t base = uint32_t(d.size());
        d.insert(d.end(), vertices.begin(), vertices.end());
        align8(d);
        uint32_t at = uint32_t(d.size());
        for (size_t i : vtx_commands) list[i].second += base;
        for (const auto& [w0, w1] : list) {
            add32(d, w0);
            add32(d, w1);
        }
        return at;
    }

    // rush2::assets::Patcher: rebuilds the models of container `base` that a model mod replaces.
    bool patch_container(uint8_t* rdram, int index, const std::vector<uint8_t>& base, std::vector<uint8_t>& out) {
        if (model_mods.empty() || base.size() < 0x28) return false;
        uint32_t models = be32(base, 0), names = be32(base, 4), count = be32(base, 16);
        if (count == 0 || count > 8192 || models + count * 0x34 > base.size() || names + count * 0x18 > base.size()) return false;
        std::vector<std::pair<uint32_t, const ModelMod*>> hits;
        for (uint32_t i = 0; i < count; i++) {
            const char* n = reinterpret_cast<const char*>(&base[names + i * 0x18]);
            std::string name(n, strnlen(n, 16));
            auto it = model_mods.find(upper(rush2::catalog::file_name(name)));
            if (it != model_mods.end()) hits.push_back({ i, &it->second });
        }
        if (hits.empty()) return false;
        out = base;
        for (auto [i, mm] : hits) {
            // The stand-ins' texels, once per model.
            std::vector<uint32_t> texels;
            for (const TrueColor& s : mm->stand_ins) {
                align8(out);
                texels.push_back(uint32_t(out.size()));
                out.insert(out.end(), s.texels.begin(), s.texels.end());
            }
            uint32_t record = models + i * 0x34;
            uint32_t lods = std::min(be32(out, record), 4u);
            ListState st;
            if (lods > 0) {
                uint32_t first = be32(out, record + 4 + 8);
                if (first < base.size()) list_state(base, first, st);
            }
            float radius = 0.0f;
            for (uint32_t l = 0; l < lods; l++) {
                uint32_t e = record + 4 + l * 12;
                bool lit = (((out[e + 2] << 8) | out[e + 3]) & 0x10) != 0;
                uint32_t list = emit_model(out, *mm, texels, st, lit, radius); // its own copy: each LOD is relocated
                put32(out, e + 8, list);
            }
            uint32_t n = names + i * 0x18;
            float old_radius;
            uint32_t bits = be32(out, n + 16);
            memcpy(&old_radius, &bits, 4);
            float r = std::max(old_radius, radius / 16.0f);
            memcpy(&bits, &r, 4);
            put32(out, n + 16, bits);
            printf("[modelmod] %s (asset 0x%X) from %s: %zu triangles\n", mm->file.c_str(), index, mm->mod_id.c_str(),
                   mm->triangles);
        }
        // A car's asset copies live in slots of a fixed size (func_800A37F4): make them hold it.
        bool car = (index >= first_car_asset && index < first_car_asset + types) || (index >= 0x71 && index <= 0x7D);
        if (car && out.size() > (uint32_t)MEM_W(0, (int32_t)car_slot_size)) {
            MEM_W(0, (int32_t)car_slot_size) = (int32_t)((out.size() + 15) & ~15u);
        }
        fflush(stdout);
        return true;
    }

    // Every enabled mod's rush2/models/**/*.glb, first in mod order winning a name.
    struct ModelFile {
        std::string name, path;     // model name (upper case), path in the mod
        size_t mod;
    };

    // ---- mods --------------------------------------------------------------------------------------------------
    struct Applied {
        std::string mod_id;
        uint64_t glb_hash = 0;
        std::vector<StandIn> stand_ins;
        std::shared_ptr<const Model> model; // its images, for rush2::derive
    };
    Applied applied[types];
    bool has_own[types] = {};
    std::vector<uint8_t> own[types];     // each type's asset before its mod (a recomp fix, or empty: the ROM's)
    std::string last_signature;
    bool initialized = false;
    int frames = 0;

    using rush2::mod_files::ModFile;
    using rush2::mod_files::enabled_mods;
    using rush2::mod_files::read_mod_file;

    // The car of `type` from glb: its container (cached) and stand-ins. False on a bad file.
    bool convert(int type, const std::vector<uint8_t>& glb, std::shared_ptr<Model>& model, std::vector<uint8_t>& car,
                 std::vector<StandIn>& stand_ins, std::string& error) {
        model = std::make_shared<Model>();
        if (!parse_glb(glb, *model, error)) return false;
        std::vector<uint8_t> pal;
        for (const Rgba& c : car_palette) pal.insert(pal.end(), { c.r, c.g, c.b, c.a });
        uint64_t source = rush2::track_cache::hash(glb.data(), glb.size(),
                                                   rush2::track_cache::hash(pal.data(), pal.size(), cache_version));
        std::string cache_name = std::string("carmod_") + prefixes[type];
        std::vector<uint8_t> blob;
        if (rush2::track_cache::load_blob(cache_name, source, blob) && blob.size() >= 8) {
            // car size, car, stand-in count, per stand-in: offset, width, height, image, mask, keys, scuffed texels
            size_t o = 0;
            auto get32 = [&]() { uint32_t v = o + 4 <= blob.size() ? be32(blob, o) : 0; o += 4; return v; };
            uint32_t size = get32();
            if (o + size <= blob.size()) {
                car.assign(blob.begin() + o, blob.begin() + o + size);
                o += size;
                uint32_t n = get32();
                stand_ins.clear();
                for (uint32_t i = 0; i < n && o < blob.size(); i++) {
                    StandIn s;
                    s.offset = get32();
                    s.width = get32();
                    s.height = get32();
                    s.image = int(int32_t(get32()));
                    s.mask = int(int32_t(get32()));
                    s.key = (uint64_t(get32()) << 32);
                    s.key |= get32();
                    s.scuffed_key = (uint64_t(get32()) << 32);
                    s.scuffed_key |= get32();
                    size_t texels = size_t(s.width) * s.height;
                    if (o + texels > blob.size() || s.offset + texels > car.size()) break;
                    s.texels.assign(car.begin() + s.offset, car.begin() + s.offset + texels);
                    s.scuffed.assign(blob.begin() + o, blob.begin() + o + texels);
                    o += texels;
                    stand_ins.push_back(std::move(s));
                }
                if (stand_ins.size() == n) return true;
            }
        }
        // One stand-in per image and mask the mesh uses.
        stand_ins.clear();
        for (const Part& p : model->parts) {
            bool have = false;
            for (const StandIn& s : stand_ins) have = have || (s.image == p.image && s.mask == p.mask);
            if (!have) {
                StandIn s;
                s.image = p.image;
                s.mask = p.mask;
                make_stand_in(*model, s, uint32_t(type * 977 + stand_ins.size()));
                stand_ins.push_back(std::move(s));
            }
        }
        car = build_container(prefixes[type], *model, stand_ins);
        blob.clear();
        add32(blob, uint32_t(car.size()));
        blob.insert(blob.end(), car.begin(), car.end());
        add32(blob, uint32_t(stand_ins.size()));
        for (const StandIn& s : stand_ins) {
            for (uint32_t v : { s.offset, s.width, s.height, uint32_t(s.image), uint32_t(s.mask), uint32_t(s.key >> 32),
                                uint32_t(s.key), uint32_t(s.scuffed_key >> 32), uint32_t(s.scuffed_key) }) {
                add32(blob, v);
            }
            blob.insert(blob.end(), s.scuffed.begin(), s.scuffed.end());
        }
        rush2::track_cache::save_blob(cache_name, source, blob);
        return true;
    }

    std::vector<ModelFile> model_files(const std::vector<ModFile>& mods) {
        std::vector<ModelFile> out;
        std::set<std::string> seen;
        auto add = [&](const std::string& path, size_t mod) {
            const std::string prefix = "rush2/models/";
            std::string lower = path;
            for (char& c : lower) c = char(std::tolower((unsigned char)c));
            if (lower.rfind(prefix, 0) != 0 || lower.size() < 4 || lower.substr(lower.size() - 4) != ".glb") return;
            std::string file = path.substr(path.find_last_of('/') + 1);
            std::string name = upper(file.substr(0, file.size() - 4));
            if (seen.insert(name).second) out.push_back({ name, path, mod });
        };
        for (size_t i = 0; i < mods.size(); i++) {
            rush2::upscale::ZipReader zip(mods[i].path);
            if (zip.is_open()) {
                for (const std::string& n : zip.names()) add(n, i);
                continue;
            }
            std::error_code ec;
            std::filesystem::path dir = mods[i].path / "rush2" / "models";
            if (!std::filesystem::is_directory(dir, ec)) continue;
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::u8string rel = std::filesystem::relative(it->path(), mods[i].path, ec).generic_u8string();
                add(std::string(rel.begin(), rel.end()), i);
            }
        }
        return out;
    }

    // Loads the model mods of the enabled mods and patches the game's assets with them.
    void apply_models(uint8_t* rdram, const std::vector<ModFile>& mods) {
        std::vector<ModelFile> files = model_files(mods);
        std::string signature;
        for (const ModelFile& f : files) {
            std::error_code ec;
            auto time = std::filesystem::last_write_time(mods[f.mod].path, ec);
            signature += mods[f.mod].mod_id + ":" + f.path + ":" + std::to_string(ec ? 0 : time.time_since_epoch().count()) + ";";
        }
        if (signature == model_signature) return;
        model_signature = signature;
        std::map<std::string, ModelMod> loaded;
        for (const ModelFile& f : files) {
            std::vector<uint8_t> glb;
            std::string error;
            ModelMod m;
            m.mod_id = mods[f.mod].mod_id;
            m.file = f.path;
            if (!read_mod_file(mods[f.mod], f.path, glb) || !load_model_mod(glb, m, error)) {
                fprintf(stderr, "[modelmod] %s in %s: %s\n", f.path.c_str(), m.mod_id.c_str(), error.empty() ? "unreadable" : error.c_str());
                continue;
            }
            loaded.emplace(f.name, std::move(m));
        }
        model_mods = std::move(loaded);
        printf("[modelmod] %zu model replacements\n", model_mods.size());
        rush2::assets::set_patcher(rdram, model_mods.empty() ? rush2::assets::Patcher() : rush2::assets::Patcher(patch_container));
    }

    // The stand-ins of every car and model applied, for rush2::derive.
    void publish_textures() {
        std::unordered_map<uint64_t, rush2::derive::ModTexture> textures;
        for (const auto& [name, mm] : model_mods) {
            for (const TrueColor& s : mm.stand_ins) {
                std::shared_ptr<const Model> model = mm.model;
                int image = s.image;
                rush2::derive::ModTexture mt;
                mt.id = mm.glb_hash ^ (uint64_t(uint16_t(image)) << 48) ^ 0x6D6F64656Cull;
                mt.load = [model, image](Image& out, Image&) {
                    return image >= 0 && rush2::upscale::decode_image(model->images[image], out);
                };
                textures[s.key] = mt;
            }
        }
        for (int t = 0; t < types; t++) {
            const Applied& a = applied[t];
            if (!a.model) continue;
            for (const StandIn& s : a.stand_ins) {
                std::shared_ptr<const Model> model = a.model;
                int image = s.image, mask = s.mask;
                rush2::derive::ModTexture mt;
                mt.id = (uint64_t(t) << 32) | (uint64_t(uint16_t(image)) << 16) | uint16_t(mask);
                mt.load = [model, image, mask](Image& out, Image& out_mask) {
                    if (image < 0 || !rush2::upscale::decode_image(model->images[image], out)) return false;
                    if (mask >= 0) decode_mask(model->images[mask], out_mask);
                    return true;
                };
                mt.main_entry = main_first;
                mt.accent_entry = accent_first;
                mt.seed = uint32_t(t * 977 + image);
                textures[s.key] = mt;
                if (s.scuffed_key != s.key) { // no paint, nothing to scuff (a glass color)
                    mt.damaged = true;
                    textures[s.scuffed_key] = mt;
                }
            }
        }
        rush2::derive::set_mod_textures("cars", std::move(textures));
    }

    void apply(uint8_t* rdram) {
        std::string signature;
        std::vector<ModFile> mods = enabled_mods(signature);
        last_signature = signature;
        std::string models_before = model_signature;
        apply_models(rdram, mods);
        bool changed = model_signature != models_before;
        for (int t = 0; t < types; t++) {
            std::string name = std::string("rush2/cars/") + prefixes[t] + ".glb";
            std::vector<uint8_t> glb;
            const ModFile* from = nullptr;
            for (const ModFile& m : mods) {
                if (read_mod_file(m, name, glb)) {
                    from = &m;
                    break;
                }
            }
            Applied& a = applied[t];
            if (from == nullptr) {
                if (a.model) {
                    rush2::car2049::clear_body_car(rdram, t, own[t]);
                    printf("[carmod] %s: back to its own car\n", prefixes[t]);
                    a = Applied{};
                    changed = true;
                }
                continue;
            }
            uint64_t h = rush2::track_cache::hash(glb.data(), glb.size());
            if (a.model && a.glb_hash == h && a.mod_id == from->mod_id) continue;
            std::shared_ptr<Model> model;
            std::vector<uint8_t> car;
            std::vector<StandIn> stand_ins;
            std::string error;
            if (!convert(t, glb, model, car, stand_ins, error)) {
                fprintf(stderr, "[carmod] %s in %s: %s\n", name.c_str(), from->mod_id.c_str(), error.c_str());
                continue;
            }
            if (!has_own[t]) {
                std::vector<rush2::assets::Span> spans;
                own[t].clear();
                rush2::assets::replacement(first_car_asset + t, own[t], spans);
                has_own[t] = true;
            }
            std::map<uint32_t, std::vector<uint8_t>> scuffed;
            size_t triangles = 0;
            for (const Part& p : model->parts) triangles += p.tris.size() / 3;
            for (const StandIn& s : stand_ins) scuffed[s.offset] = s.scuffed;
            rush2::car2049::set_body_car(rdram, t, std::move(car), prefixes[t], scuffed);
            printf("[carmod] %s from %s: %zu triangles, %zu textures\n", prefixes[t], from->mod_id.c_str(), triangles,
                   stand_ins.size());
            a.mod_id = from->mod_id;
            a.glb_hash = h;
            a.stand_ins = std::move(stand_ins);
            a.model = model;
            changed = true;
        }
        if (changed) publish_textures();
        fflush(stdout);
    }
}

std::vector<rush2::mod_files::ModFile> rush2::mod_files::enabled_mods(std::string& signature) {
    std::vector<ModFile> out;
    for (const recomp::mods::ModDetails& mod : recomp::mods::get_all_mod_details("rush2")) {
        bool on = recomp::mods::is_mod_enabled(mod.mod_id);
        signature += mod.mod_id + (on ? "+" : "-") + ";";
        if (on) out.push_back({ mod.mod_id, recomp::mods::get_mod_filename(mod.mod_id) });
    }
    return out;
}

bool rush2::mod_files::read_mod_file(const ModFile& mod, const std::string& name, std::vector<uint8_t>& out) {
    rush2::upscale::ZipReader zip(mod.path);
    if (zip.is_open()) return zip.read(name, out);
    out = rush2::upscale::read_file(mod.path / std::filesystem::u8path(name));
    return !out.empty();
}

void rush2::car_mods::init(uint8_t* rdram) {
    initialized = load_car_palette(rdram);
    if (!initialized) {
        fprintf(stderr, "[carmod] CARPALETTE not found: car mods are off\n");
        return;
    }
    apply(rdram);
}

void rush2::car_mods::check(uint8_t* rdram) {
    if (!initialized || ++frames < 30) return;
    frames = 0;
    int32_t state = MEM_W(0, (int32_t)game_state);
    if (state < 0 || state > 1) return; // only in the menus: no car is loaded then
    std::string signature;
    enabled_mods(signature);
    if (signature != last_signature) apply(rdram);
}
